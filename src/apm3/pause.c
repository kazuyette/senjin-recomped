/* pause.c - Senjin Aleste, apm_x86.dll: pause of the port, and gamepad input cleanup.
 *
 * Pause: the "Pause" function chosen in the launcher (gamepad, default Back) and the P / Pause keys freeze the game.
 * The main thread is held after Present (video.c calls pause_after_present), the game clocks imported by the exe
 * (QueryPerformanceCounter, GetTickCount, timeGetTime) stand still and the XAudio2 engine is stopped. The game's own
 * pause input (cabinet button 5) has no visible effect on this build, so the DLL no longer sends it (apm3.c).
 *
 * DirectInput: the game has its own DirectInput-to-XInput layer (dinput.ini) for gamepads that are not XInput
 * (e.g. some Bluetooth pads). Reading the pad on top of the cabinet inputs, it is the likely cause of a reported
 * inverted stick Y axis [hypothesis]. Game controllers are hidden from it: the gamepad goes through the cabinet inputs only, like the native Linux build.
 * [apm] dinput_pads=1 in aleste.ini restores the game's own DirectInput gamepads. */
#include <windows.h>
#include <string.h>
#include <stdio.h>

void sale_log(const char *fmt, ...);
int apm_pause_down(void);                       /* apm3.c: pause control held */

/* ---------- import table patching (exe only) ---------- */
static int iat_hook(const char *dll, const char *name, void *fn, void **orig) {
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *id = (void *)(base + dd.VirtualAddress); id->Name; id++) {
        if (_stricmp((char *)base + id->Name, dll)) continue;
        IMAGE_THUNK_DATA *on = (void *)(base + id->OriginalFirstThunk), *ft = (void *)(base + id->FirstThunk);
        for (; on->u1.AddressOfData; on++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal)) continue;
            if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + on->u1.AddressOfData))->Name, name)) continue;
            DWORD old; VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            *orig = (void *)ft->u1.Function; ft->u1.Function = (ULONG_PTR)fn;
            VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}
static void vt_patch(void **slot, void *fn, void **orig) {
    if (*slot == fn) return;
    DWORD old; VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
    *orig = *slot; *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

/* ---------- clocks that stand still while paused ---------- */
static volatile LONG g_paused;
static LONGLONG q_total, q_start;               /* QueryPerformanceCounter units */
static DWORD ms_total, ms_start;                /* milliseconds (GetTickCount / timeGetTime) */
static BOOL (WINAPI *r_QPC)(LARGE_INTEGER *);
static DWORD (WINAPI *r_GTC)(void);
static DWORD (WINAPI *r_TGT)(void);
static BOOL WINAPI h_QPC(LARGE_INTEGER *p) {
    BOOL r = r_QPC(p); if (r && p) p->QuadPart = (g_paused ? q_start : p->QuadPart) - q_total; return r;
}
static DWORD WINAPI h_GTC(void) { DWORD t = r_GTC(); return (g_paused ? ms_start : t) - ms_total; }
static DWORD WINAPI h_TGT(void) { DWORD t = r_TGT(); return (g_paused ? ms_start : t) - ms_total; }

/* ---------- XAudio2 2.7 engine (created through CoCreateInstance) ---------- */
static const GUID CLSID_XAudio2_27 = { 0x5a508685, 0xa254, 0x4fba, { 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf } };
static const GUID CLSID_XAudio2_27D = { 0xdb05ea35, 0x0329, 0x4d4b, { 0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52 } };
static void *g_xa;                              /* IXAudio2*: vtable 8 = CreateSourceVoice, 11 = StartEngine, 12 = StopEngine */
static HRESULT (WINAPI *r_CCI)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
static void vt_patch(void **slot, void *fn, void **orig);
/* source voices (vtable 19 = Start, 22 = FlushSourceBuffers): Start held during the hidden Start transition (#5, apm3.c) */
int apm_start_mute(void); DWORD apm_game_frame(void);
static HRESULT (STDMETHODCALLTYPE *r_VStart)(void *, UINT32, UINT32);
static HRESULT (STDMETHODCALLTYPE *r_CSV)(void *, void **, const void *, UINT32, float, void *, const void *, const void *);
static struct { void *v; DWORD frame; } held[16]; static int nheld;
static CRITICAL_SECTION held_cs;                /* the game may start sounds from another thread (initialised in pause_init) */
static void held_lock(void) { EnterCriticalSection(&held_cs); }
static void (STDMETHODCALLTYPE *r_VDestroy)(void *);
static void STDMETHODCALLTYPE h_VDestroy(void *v) {   /* a held voice destroyed by the game must not be touched later */
    held_lock();
    for (int i = 0; i < nheld; i++) if (held[i].v == v) { memmove(&held[i], &held[i + 1], (size_t)(nheld - i - 1) * sizeof held[0]); nheld--; i--; }
    LeaveCriticalSection(&held_cs);
    r_VDestroy(v);
}
static HRESULT STDMETHODCALLTYPE h_VStart(void *v, UINT32 fl, UINT32 op) {
    if (apm_start_mute()) { held_lock(); if (nheld < 16) { held[nheld].v = v; held[nheld].frame = apm_game_frame(); nheld++; } LeaveCriticalSection(&held_cs); return 0; }
    return r_VStart(v, fl, op);
}
static HRESULT STDMETHODCALLTYPE h_CSV(void *x, void **pv, const void *fmt, UINT32 fl, float ratio, void *cb, const void *sends, const void *fx) {
    HRESULT hr = r_CSV(x, pv, fmt, fl, ratio, cb, sends, fx);
    if (SUCCEEDED(hr) && pv && *pv) { vt_patch(&(*(void ***)*pv)[19], (void *)h_VStart, (void **)&r_VStart); vt_patch(&(*(void ***)*pv)[18], (void *)h_VDestroy, (void **)&r_VDestroy); }
    return hr;
}
void pause_audio_start_end(DWORD frame) {          /* sounds started in the frame where KEY CONFIG began: played; others dropped */
    int nk = 0, nd = 0;
    held_lock(); sale_log("start: %d sound start(s) held", nheld);
    for (int i = 0; i < nheld; i++) {
        int later = 0; for (int j = i + 1; j < nheld; j++) if (held[j].v == held[i].v) later = 1;
        if (later) continue;
        sale_log("start: voice %p (frame %lu, KEY CONFIG frame %lu): %s", held[i].v, held[i].frame, frame, held[i].frame == frame ? "play" : "drop");
        if (held[i].frame == frame) { r_VStart(held[i].v, 0, 0); nk++; }
        else { ((HRESULT (STDMETHODCALLTYPE *)(void *))(*(void ***)held[i].v)[22])(held[i].v); nd++; }
    }
    sale_log("start: %d played with KEY CONFIG, %d dropped", nk, nd); nheld = 0;
    LeaveCriticalSection(&held_cs);
}
static HRESULT WINAPI h_CCI(REFCLSID c, LPUNKNOWN o, DWORD ctx, REFIID i, LPVOID *pv) {
    HRESULT hr = r_CCI(c, o, ctx, i, pv);
    if (SUCCEEDED(hr) && pv && *pv && (!memcmp(c, &CLSID_XAudio2_27, sizeof(GUID)) || !memcmp(c, &CLSID_XAudio2_27D, sizeof(GUID)))) {
        g_xa = *pv; sale_log("pause: XAudio2 engine %p", g_xa);
        vt_patch(&(*(void ***)g_xa)[8], (void *)h_CSV, (void **)&r_CSV);
    }
    return hr;
}
static void xa_engine(int start) {
    if (!g_xa) return;
    void **vt = *(void ***)g_xa;
    if (start) ((HRESULT (STDMETHODCALLTYPE *)(void *))vt[11])(g_xa);
    else ((void (STDMETHODCALLTYPE *)(void *))vt[12])(g_xa);
}

/* ---------- DirectInput: game controllers hidden from the game ---------- */
typedef BOOL (CALLBACK *ENUMCB)(const void *inst, void *ref);
typedef struct { ENUMCB cb; void *ref; } enumctx;
static HRESULT (STDMETHODCALLTYPE *r_EnumA)(void *, DWORD, ENUMCB, void *, DWORD);
static HRESULT (STDMETHODCALLTYPE *r_EnumW)(void *, DWORD, ENUMCB, void *, DWORD);
static BOOL CALLBACK enum_filter(const void *inst, void *ref) {
    enumctx *c = ref; BYTE type = *(const BYTE *)((const BYTE *)inst + 36);   /* DIDEVICEINSTANCE.dwDevType, low byte */
    if (type >= 0x14) return TRUE;              /* DI8DEVTYPE_JOYSTICK and up (gamepads, wheels...): skipped, DIENUM_CONTINUE */
    return c->cb(inst, c->ref);
}
static HRESULT enum_common(HRESULT (STDMETHODCALLTYPE *real)(void *, DWORD, ENUMCB, void *, DWORD), void *self, DWORD cls, ENUMCB cb, void *ref, DWORD fl) {
    if (cls == 4) return 0;                     /* DI8DEVCLASS_GAMECTRL: none */
    enumctx c = { cb, ref };
    return real(self, cls, enum_filter, &c, fl);
}
static HRESULT STDMETHODCALLTYPE h_EnumA(void *s, DWORD cls, ENUMCB cb, void *ref, DWORD fl) { return enum_common(r_EnumA, s, cls, cb, ref, fl); }
static HRESULT STDMETHODCALLTYPE h_EnumW(void *s, DWORD cls, ENUMCB cb, void *ref, DWORD fl) { return enum_common(r_EnumW, s, cls, cb, ref, fl); }
static const GUID IID_IDirectInput8W_ = { 0xbf798031, 0x483a, 0x4da2, { 0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00 } };
static HRESULT (WINAPI *r_DI8C)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
static HRESULT WINAPI h_DI8C(HINSTANCE h, DWORD v, REFIID riid, LPVOID *pv, LPUNKNOWN o) {
    HRESULT hr = r_DI8C(h, v, riid, pv, o);
    if (SUCCEEDED(hr) && pv && *pv) {
        void **vt = *(void ***)*pv; int wide = !memcmp(riid, &IID_IDirectInput8W_, sizeof(GUID));
        if (wide) vt_patch(&vt[4], (void *)h_EnumW, (void **)&r_EnumW); else vt_patch(&vt[4], (void *)h_EnumA, (void **)&r_EnumA);
        sale_log("DirectInput8 (%s): game controllers hidden from the game", wide ? "W" : "A");
    }
    return hr;
}

/* ---------- pause loop ---------- */
static void pause_set(int on) {
    if (on) { LARGE_INTEGER q; r_QPC(&q); q_start = q.QuadPart; ms_start = r_GTC(); g_paused = 1; xa_engine(0); }
    else { LARGE_INTEGER q; r_QPC(&q); q_total += q.QuadPart - q_start; ms_total += r_GTC() - ms_start; g_paused = 0; xa_engine(1); }
    sale_log("pause: %s", on ? "on" : "off");
}
void pause_after_present(HWND hwnd) {
    static int was = 1;
    if (!r_QPC || !r_GTC) return;               /* clocks not hooked: no pause */
    int d = apm_pause_down();
    if (!(d && !was)) { was = d; return; }
    if (!hwnd) hwnd = GetForegroundWindow();
    char title[256] = "", t2[300]; GetWindowTextA(hwnd, title, sizeof title);
    snprintf(t2, sizeof t2, "%s - PAUSE", title); SetWindowTextA(hwnd, t2);
    pause_set(1);
    int held = 1, quit = 0;
    for (;;) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { quit = 1; PostQuitMessage((int)m.wParam); break; }
            TranslateMessage(&m); DispatchMessageA(&m);
        }
        if (quit) break;
        int p = apm_pause_down(); if (p && !held) break; held = p;
        if (GetForegroundWindow() == hwnd && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) break;   /* Esc: resume, the game then quits */
        Sleep(15);
    }
    pause_set(0);
    SetWindowTextA(hwnd, title);
    was = 1;
}

void pause_init(const char *ini) {
    InitializeCriticalSection(&held_cs);
    int ok = iat_hook("KERNEL32.dll", "QueryPerformanceCounter", (void *)h_QPC, (void **)&r_QPC)
           & iat_hook("KERNEL32.dll", "GetTickCount", (void *)h_GTC, (void **)&r_GTC);
    if (!iat_hook("WINMM.dll", "timeGetTime", (void *)h_TGT, (void **)&r_TGT)) r_TGT = NULL;
    if (!ok) { r_QPC = NULL; r_GTC = NULL; }    /* (a hook already set stays harmless: totals are 0) */
    if (!r_QPC) { HMODULE k = GetModuleHandleA("kernel32.dll"); r_QPC = (void *)GetProcAddress(k, "QueryPerformanceCounter"); }
    if (!r_GTC) { HMODULE k = GetModuleHandleA("kernel32.dll"); r_GTC = (void *)GetProcAddress(k, "GetTickCount"); }
    iat_hook("ole32.dll", "CoCreateInstance", (void *)h_CCI, (void **)&r_CCI);
    int dpads = GetPrivateProfileIntA("apm", "dinput_pads", 0, ini);
    int di = !dpads && iat_hook("DINPUT8.dll", "DirectInput8Create", (void *)h_DI8C, (void **)&r_DI8C);
    sale_log("pause: clocks %s, DirectInput gamepads %s", ok ? "hooked" : "NOT hooked (pause still works, clocks run)", dpads ? "kept" : di ? "hidden" : "hook failed");
}
