/* sa_user32.c - USER32 / GDI32 / SHELL32 rewritten on SDL2: window, messages, the game's window procedure.
 * Only one real window (the game's); the other HWNDs are "ghost" windows with no display. */
#include <string.h>
#include <SDL2/SDL.h>
#include "sa_host.h"

extern void call_ind(uint32_t);
void PostQuit(void);
SDL_Window *sa_window;                      /* used by the renderer (sa_d3d9.c) */

#define MAXW 32
typedef struct { int used; uint32_t proc, style, exstyle, userdata; int x, y, w, h, visible; char title[128];
                 struct { char name[32]; uint32_t val; } prop[8]; SDL_Window *sdl; } win;
static win W[MAXW];
#define HWND_BASE 0x00010000u
static win *wget(uint32_t h) { uint32_t i = (h - HWND_BASE) / 4; return (h >= HWND_BASE && i < MAXW && W[i].used) ? &W[i] : NULL; }

#define MAXC 16
static struct { char name[64]; uint32_t proc; } C[MAXC]; static int nC;

/* call the game's window procedure: LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM) */
static uint32_t send(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    win *w = wget(h); if (!w || !w->proc) return 0;
    if ((w->proc & 0xFFF00000u) == G_MAGIC) return 0;   /* default procedure */
    CPU save = cpu;
    push32(lp); push32(wp); push32(msg); push32(h); push32(0xFFFFFFF0u);
    call_ind(w->proc);
    uint32_t r = cpu.eax; uint32_t esp = cpu.esp;
    cpu = save; (void)esp; cpu.eax = r;
    return r;
}

/* posted message queue (WM_ACTIVATE on show, PostMessage): served before SDL events */
static struct { uint32_t h, m, wp, lp; } PQ[64]; static unsigned pq_r, pq_w;
static void post(uint32_t h, uint32_t m, uint32_t wp, uint32_t lp) { if (pq_w - pq_r < 64) { PQ[pq_w & 63].h = h; PQ[pq_w & 63].m = m; PQ[pq_w & 63].wp = wp; PQ[pq_w & 63].lp = lp; pq_w++; } }

static void RegisterClassExA_(void) {
    uint32_t wc = ARG(0); const char *n = g_str(rd32(wc + 40));
    if (nC < MAXC) { snprintf(C[nC].name, 64, "%s", (rd32(wc + 40) < 0x10000) ? "#atom" : n); C[nC].proc = rd32(wc + 8); nC++; }
    if (sa_verbose) sa_log("RegisterClassExA(%s, proc 0x%08x)", C[nC - 1].name, C[nC - 1].proc);
    RET_STD(1, 0xC000 + nC);
}
static void CreateWindowExA_(void) {
    uint32_t cls = ARG(1); const char *title = ARG(2) ? g_str(ARG(2)) : "";
    int x = (int)ARG(4), y = (int)ARG(5), w = (int)ARG(6), h = (int)ARG(7);
    uint32_t proc = 0;
    for (int i = 0; i < nC; i++) if (cls < 0x10000 ? (cls == 0xC001u + i) : !strcmp(C[i].name, g_str(cls))) proc = C[i].proc;
    int k = 0; while (k < MAXW && W[k].used) k++;
    if (k == MAXW) { RET_STD(12, 0); return; }
    win *o = &W[k]; memset(o, 0, sizeof *o); o->used = 1; o->proc = proc; o->style = ARG(3); o->exstyle = ARG(0);
    o->x = x; o->y = y; o->w = (w <= 0 || w > 8192) ? 1280 : w; o->h = (h <= 0 || h > 8192) ? 720 : h;
    snprintf(o->title, sizeof o->title, "%s", title);
    uint32_t hw = HWND_BASE + 4u * (uint32_t)k;
    if (!sa_window) {
        o->sdl = SDL_CreateWindow("Senjin Aleste", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, o->w, o->h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
        sa_window = o->sdl;
        if (!o->sdl) sa_log("SDL_CreateWindow: %s", SDL_GetError());
    }
    if (sa_verbose) sa_log("CreateWindowExA(\"%s\", %dx%d) -> 0x%x", title, o->w, o->h, hw);
    uint32_t cs = g_alloc(48, 1); wr32(cs, ARG(11)); wr32(cs + 20, (uint32_t)o->w); wr32(cs + 16, (uint32_t)o->h);
    send(hw, 0x0081, 0, cs); send(hw, 0x0001, 0, cs);     /* WM_NCCREATE, WM_CREATE */
    gh_free(cs);
    if (o->style & 0x10000000u) {                          /* WS_VISIBLE: shown and activated as on Windows */
        o->visible = 1; if (o->sdl) SDL_ShowWindow(o->sdl);
        send(hw, 0x0018, 1, 0); post(hw, 0x0006, 1, 0); post(hw, 0x0007, 0, 0);   /* WM_SHOWWINDOW, WM_ACTIVATE, WM_SETFOCUS */
    }
    RET_STD(12, hw);
}
static void DestroyWindow_(void) { win *w = wget(ARG(0)); if (w) { send(ARG(0), 0x0002, 0, 0); if (w->sdl) { SDL_DestroyWindow(w->sdl); if (w->sdl == sa_window) sa_window = NULL; } w->used = 0; } RET_STD(1, 1); }
static void ShowWindow_(void) {
    uint32_t hw = ARG(0), cmd = ARG(1); win *w = wget(hw);
    if (w) {
        int was = w->visible; w->visible = cmd != 0;
        if (w->sdl) { if (w->visible) SDL_ShowWindow(w->sdl); else SDL_HideWindow(w->sdl); }
        if (w->visible && !was) { send(hw, 0x0018, 1, 0); post(hw, 0x0006, 1, 0); post(hw, 0x0007, 0, 0); }  /* WM_SHOWWINDOW, WM_ACTIVATE, WM_SETFOCUS */
    }
    RET_STD(2, 1);
}
static void DefWindowProcA_(void) {
    uint32_t msg = ARG(1);
    if (msg == 0x0010) PostQuit();             /* WM_CLOSE */
    RET_STD(4, msg == 0x0081 ? 1 : 0);
}
static int quit_posted;
void PostQuit(void) { quit_posted = 1; }
static void PostQuitMessage_(void) { quit_posted = 1; RET_STD(1, 0); }
static void CallWindowProcA_(void) {
    uint32_t proc = ARG(0), h = ARG(1), m = ARG(2), wp = ARG(3), lp = ARG(4);
    uint32_t r = 0;
    if ((proc & 0xFFF00000u) != G_MAGIC) { CPU save = cpu; push32(lp); push32(wp); push32(m); push32(h); push32(0xFFFFFFF0u); call_ind(proc); r = cpu.eax; cpu = save; }
    RET_STD(5, r);
}

/* ---- messages: SDL events -> Windows MSG ---- */
static int vk_of(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return k - 32;
    if (k >= '0' && k <= '9') return k;
    switch (k) {
    case SDLK_RETURN: return 0x0D; case SDLK_ESCAPE: return 0x1B; case SDLK_SPACE: return 0x20; case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09; case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26; case SDLK_RIGHT: return 0x27; case SDLK_DOWN: return 0x28;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10; case SDLK_LCTRL: case SDLK_RCTRL: return 0x11; case SDLK_LALT: case SDLK_RALT: return 0x12;
    case SDLK_PAUSE: return 0x13;
    default: if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (k - SDLK_F1); return 0;
    }
}
uint8_t sa_keys[256];                       /* keyboard state (GetAsyncKeyState, inputs) */
uint8_t sa_scan[SDL_NUM_SCANCODES];         /* physical keys (cabinet inputs: same places on every layout) */
static int next_msg(uint32_t msgp, int remove) {
    if (quit_posted) { memset(MEM + msgp, 0, 28); wr32(msgp + 4, 0x0012); if (remove) quit_posted = 0; return 1; }
    if (pq_r != pq_w) {
        unsigned i = pq_r & 63; wr32(msgp, PQ[i].h); wr32(msgp + 4, PQ[i].m); wr32(msgp + 8, PQ[i].wp); wr32(msgp + 12, PQ[i].lp); wr32(msgp + 16, SDL_GetTicks());
        if (remove) pq_r++; return 1;
    }
    SDL_Event e;
    while (remove ? SDL_PollEvent(&e) : SDL_PeepEvents(&e, 1, SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {
        uint32_t hw = HWND_BASE, m = 0, wp = 0, lp = 0;
        switch (e.type) {
        case SDL_QUIT: m = 0x0010; break;                                   /* WM_CLOSE */
        case SDL_KEYDOWN: case SDL_KEYUP: {
            if (e.key.keysym.scancode < SDL_NUM_SCANCODES) sa_scan[e.key.keysym.scancode] = e.type == SDL_KEYDOWN;
            int vk = vk_of(e.key.keysym.sym); if (!vk) break;
            sa_keys[vk] = e.type == SDL_KEYDOWN;
            m = e.type == SDL_KEYDOWN ? 0x0100 : 0x0101; wp = (uint32_t)vk; lp = 1 | (e.key.keysym.scancode << 16) | (e.type == SDL_KEYUP ? 0xC0000000u : 0);
            break; }
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) { m = 0x0006; wp = 1; }   /* WM_ACTIVATE */
            else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { m = 0x0006; wp = 0; }
            break;
        }
        if (!m) { if (!remove) SDL_PollEvent(&e); continue; }
        wr32(msgp, hw); wr32(msgp + 4, m); wr32(msgp + 8, wp); wr32(msgp + 12, lp); wr32(msgp + 16, SDL_GetTicks());
        return 1;
    }
    return 0;
}
/* pause loop (sa_d3d9.c): keyboard state only; returns 1 when the window is being closed (event left for the game) */
int sa_pump_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { SDL_PushEvent(&e); return 1; }
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            if (e.key.keysym.scancode < SDL_NUM_SCANCODES) sa_scan[e.key.keysym.scancode] = e.type == SDL_KEYDOWN;
            int vk = vk_of(e.key.keysym.sym); if (vk) sa_keys[vk] = e.type == SDL_KEYDOWN; }
    }
    return 0;
}
static void PeekMessageA_(void) { RET_STD(5, next_msg(ARG(0), ARG(4) & 1)); }
static void TranslateMessage_(void) { RET_STD(1, 0); }
static void DispatchMessageA_(void) { uint32_t m = ARG(0); RET_STD(1, send(rd32(m), rd32(m + 4), rd32(m + 8), rd32(m + 12))); }
static void SendMessageA_(void) { RET_STD(4, send(ARG(0), ARG(1), ARG(2), ARG(3))); }

/* ---- geometry ---- */
static void GetClientRect_(void) { win *w = wget(ARG(0)); uint32_t r = ARG(1); wr32(r, 0); wr32(r + 4, 0); wr32(r + 8, w ? w->w : 1280); wr32(r + 12, w ? w->h : 720); RET_STD(2, w != NULL); }
static void GetWindowRect_(void) { win *w = wget(ARG(0)); uint32_t r = ARG(1); int x = w ? w->x : 0, y = w ? w->y : 0;
    wr32(r, x); wr32(r + 4, y); wr32(r + 8, x + (w ? w->w : 1280)); wr32(r + 12, y + (w ? w->h : 720)); RET_STD(2, w != NULL); }
static void AdjustWindowRectEx_(void) { RET_STD(4, 1); }
static void SetWindowPos_(void) {
    win *w = wget(ARG(0)); uint32_t fl = ARG(6);
    if (w) { if (!(fl & 2)) { w->x = (int)ARG(2); w->y = (int)ARG(3); } if (!(fl & 1)) { w->w = (int)ARG(4); w->h = (int)ARG(5); if (w->sdl) SDL_SetWindowSize(w->sdl, w->w, w->h); } }
    RET_STD(7, 1);
}
static void ClientToScreen_(void) { RET_STD(2, 1); }
static void GetSystemMetrics_(void) {
    SDL_DisplayMode dm = { 0 }; if (SDL_GetDesktopDisplayMode(0, &dm)) { dm.w = 1920; dm.h = 1080; }
    uint32_t i = ARG(0), v = 0;
    if (i == 0 || i == 78 || i == 16) v = (uint32_t)dm.w; else if (i == 1 || i == 79 || i == 17) v = (uint32_t)dm.h; else if (i == 80) v = 1;
    RET_STD(1, v);
}
static void IsIconic_(void) { RET_STD(1, 0); }
static void InvalidateRect_(void) { RET_STD(3, 1); }
static void GetWindowLongA_(void) {
    win *w = wget(ARG(0)); int32_t i = (int32_t)ARG(1); uint32_t v = 0;
    if (w) { if (i == -4) v = w->proc; else if (i == -16) v = w->style; else if (i == -20) v = w->exstyle; else if (i == -21) v = w->userdata; }
    RET_STD(2, v);
}
static void SetWindowLongA_(void) {
    win *w = wget(ARG(0)); int32_t i = (int32_t)ARG(1); uint32_t v = ARG(2), old = 0;
    if (w) { if (i == -4) { old = w->proc; w->proc = v; } else if (i == -16) { old = w->style; w->style = v; } else if (i == -20) { old = w->exstyle; w->exstyle = v; } else if (i == -21) { old = w->userdata; w->userdata = v; } }
    RET_STD(3, old);
}
static void SetPropA_(void) {
    win *w = wget(ARG(0)); const char *n = g_str(ARG(1));
    if (w) for (int i = 0; i < 8; i++) if (!w->prop[i].name[0] || !strcmp(w->prop[i].name, n)) { snprintf(w->prop[i].name, 32, "%s", n); w->prop[i].val = ARG(2); break; }
    RET_STD(3, w != NULL);
}
static void GetPropA_(void) { win *w = wget(ARG(0)); const char *n = g_str(ARG(1)); uint32_t v = 0; if (w) for (int i = 0; i < 8; i++) if (!strcmp(w->prop[i].name, n)) v = w->prop[i].val; RET_STD(2, v); }
static void RemovePropA_(void) { win *w = wget(ARG(0)); const char *n = g_str(ARG(1)); uint32_t v = 0; if (w) for (int i = 0; i < 8; i++) if (!strcmp(w->prop[i].name, n)) { v = w->prop[i].val; w->prop[i].name[0] = 0; } RET_STD(2, v); }
static void GetWindowTextLengthW_(void) { win *w = wget(ARG(0)); RET_STD(1, w ? (uint32_t)strlen(w->title) : 0); }
static void GetWindowTextW_(void) { win *w = wget(ARG(0)); uint32_t b = ARG(1), n = ARG(2), k = 0; if (w) for (; k + 1 < n && w->title[k]; k++) wr16(b + 2 * k, (uint8_t)w->title[k]); if (n) wr16(b + 2 * k, 0); RET_STD(3, k); }

/* ---- misc ---- */
static void LoadIconA_(void) { RET_STD(2, 0x20001); }
static void LoadCursorA_(void) { RET_STD(2, 0x20002); }
static int cursor_count;
static void ShowCursor_(void) { cursor_count += ARG(0) ? 1 : -1; SDL_ShowCursor(cursor_count >= 0); RET_STD(1, (uint32_t)cursor_count); }
static void SetTimer_(void) { RET_STD(4, ARG(1) ? ARG(1) : 1); }
static void KillTimer_(void) { RET_STD(2, 1); }
static void MessageBoxA_(void) { sa_log("MessageBox [%s] %s", ARG(2) ? g_str(ARG(2)) : "", ARG(1) ? g_str(ARG(1)) : ""); RET_STD(4, 1); }
static void SetFocus_(void) { RET_STD(1, HWND_BASE); }
static void GetFocus_(void) { RET_STD(0, HWND_BASE); }
static void SetCapture_(void) { RET_STD(1, 0); }
static void ReleaseCapture_(void) { RET_STD(0, 1); }
static void GetCapture_(void) { RET_STD(0, 0); }
static void GetKeyboardLayoutNameA_(void) { memcpy(MEM + ARG(0), "00000411", 9); RET_STD(1, 1); }
static void GetRawInputDeviceList_(void) { if (ARG(1)) wr32(ARG(1), 0); RET_STD(3, 0); }
static void GetRawInputDeviceInfoA_(void) { RET_STD(4, 0xFFFFFFFFu); }
static void GetDC_(void) { RET_STD(1, 0x30001); }
static void ReleaseDC_(void) { RET_STD(2, 1); }
static void DragAcceptFiles_(void) { RET_STD(2, 0); }
/* GDI: device caps and stock objects (fonts and glyphs are in sa_gdi.c) */
static void GetDeviceCaps_(void) { uint32_t i = ARG(1); RET_STD(2, (i == 88 || i == 90) ? 96 : (i == 8 ? 1920 : i == 10 ? 1080 : 0)); }
static void GetStockObject_(void) { RET_STD(1, 0x40000 + ARG(0)); }
/* CreateFontIndirectA, SelectObject, DeleteObject, GetTextMetricsA, GetGlyphOutlineA: see sa_gdi.c (FreeType) */

#define U(n) { "USER32.DLL!" #n, n##_ }
#define G(n) { "GDI32.DLL!" #n, n##_ }
static const imp_def defs[] = {
    U(RegisterClassExA), U(CreateWindowExA), U(DestroyWindow), U(ShowWindow), U(DefWindowProcA), U(PostQuitMessage), U(CallWindowProcA),
    U(PeekMessageA), U(TranslateMessage), U(DispatchMessageA), U(SendMessageA), U(GetClientRect), U(GetWindowRect), U(AdjustWindowRectEx),
    U(SetWindowPos), U(ClientToScreen), U(GetSystemMetrics), U(IsIconic), U(InvalidateRect), U(GetWindowLongA), U(SetWindowLongA),
    U(SetPropA), U(GetPropA), U(RemovePropA), U(GetWindowTextLengthW), U(GetWindowTextW), U(LoadIconA), U(LoadCursorA), U(ShowCursor),
    U(SetTimer), U(KillTimer), U(MessageBoxA), U(SetFocus), U(GetFocus), U(SetCapture), U(ReleaseCapture), U(GetCapture),
    U(GetKeyboardLayoutNameA), U(GetRawInputDeviceList), U(GetRawInputDeviceInfoA), U(GetDC), U(ReleaseDC),
    { "SHELL32.DLL!DragAcceptFiles", DragAcceptFiles_ },
    G(GetDeviceCaps), G(GetStockObject),
    { NULL, NULL }
};
void user32_register(void) {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");      /* Ctrl+C / kill: immediate exit (sa_main.c) */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)) sa_log("SDL_Init: %s", SDL_GetError());
    imp_register(defs);
}
