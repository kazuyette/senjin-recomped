/* video.c - Senjin Aleste: Direct3D 9 overlay (native code, inside apm_x86.dll).
 * - Hooks the exe's Direct3DCreate9 import (IAT), then IDirect3D9::CreateDevice,
 *   IDirect3DDevice9::Reset/Present and IDirect3DSwapChain9::Present (vtables).
 * - Kit CRT filter (_kit/crt, Soul Calibur, unchanged): [video] crt=0..3, F9 in game.
 *   Source = cabinet image recovered by subsampling the backbuffer with the DISP_SCALE step (nearest),
 *   or [video] src_w/src_h if set.
 * - [video] vsync=-1 (game) / 0 (off) / 1 (on).
 * - [video] fps=1: FPS in the window title. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "kit_crt.h"

void sale_log(const char *fmt, ...);

static char v_ini[MAX_PATH];
void stats_frame(void); void stats_capture_rts(IDirect3DDevice9 *, const char *); void stats_install(IDirect3DDevice9 *, void (*)(void **, void *, void **), int, const char *);
void cache_install(IDirect3DDevice9 *, void (*)(void **, void *, void **)); void cache_reset(void); void cache_frame(void);
void nobezel_install(IDirect3DDevice9 *, void (*)(void **, void *, void **)); void nobezel_frame(void);
void stats_reset(void); void nobezel_reset(void);
static int v_stats, v_dump, v_cache, v_nobezel;
static int v_bbw, v_bbh, g_ow, g_oh;  /* [video] bb_w/bb_h: reduced backbuffer; size requested by the game */
static int v_crt, v_vsync = -1, v_fps, v_srcw, v_srch, v_scx = 200, v_scy = 200;
static HWND v_hwnd;

typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
static PFN_Create9 real_Create9;
static HRESULT (STDMETHODCALLTYPE *real_CreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static HRESULT (STDMETHODCALLTYPE *real_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static HRESULT (STDMETHODCALLTYPE *real_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
static HRESULT (STDMETHODCALLTYPE *real_SCPresent)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);

/* ---------- CRT ---------- */
static IDirect3DSurface9 *s_sys, *s_def; static UINT s_w, s_h; static D3DFORMAT s_fmt;
static uint32_t *s_src, *s_dst; static int s_sw, s_sh;
static void crt_release(void) {
    if (s_sys) IDirect3DSurface9_Release(s_sys); if (s_def) IDirect3DSurface9_Release(s_def);
    s_sys = s_def = NULL; s_w = s_h = 0;
}
static void crt_apply(IDirect3DDevice9 *dev) {
    IDirect3DSurface9 *bb = NULL; D3DSURFACE_DESC d;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    IDirect3DSurface9_GetDesc(bb, &d);
    if ((d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) || d.MultiSampleType != D3DMULTISAMPLE_NONE) goto out;
    if (d.Width != s_w || d.Height != s_h || d.Format != s_fmt) {
        crt_release();
        if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &s_sys, NULL)) ||
            FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_DEFAULT, &s_def, NULL))) { crt_release(); goto out; }
        s_w = d.Width; s_h = d.Height; s_fmt = d.Format;
        s_sw = v_srcw > 0 ? v_srcw : (int)(d.Width * 100 / (v_scx > 0 ? v_scx : 100));
        s_sh = v_srch > 0 ? v_srch : (int)(d.Height * 100 / (v_scy > 0 ? v_scy : 100));
        if (s_sw < 1 || s_sw > (int)d.Width) s_sw = d.Width; if (s_sh < 1 || s_sh > (int)d.Height) s_sh = d.Height;
        free(s_src); free(s_dst); s_src = malloc((size_t)s_sw * s_sh * 4); s_dst = malloc((size_t)d.Width * d.Height * 4);
        sale_log("CRT: backbuffer %ux%u fmt %d -> source %dx%d", d.Width, d.Height, d.Format, s_sw, s_sh);
    }
    if (!kit_crt_usable(v_crt, s_sw, s_sh, s_w, s_h)) goto out;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, s_sys))) goto out;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(s_sys, &lr, NULL, 0))) goto out;
    for (int y = 0; y < s_sh; y++) {                      /* nearest: center of the source pixel */
        const uint32_t *row = (const uint32_t *)((BYTE *)lr.pBits + (size_t)((y * 2 + 1) * s_h / (2 * s_sh)) * lr.Pitch);
        uint32_t *o = s_src + (size_t)y * s_sw;
        for (int x = 0; x < s_sw; x++) o[x] = row[(x * 2 + 1) * s_w / (2 * s_sw)] & 0xFFFFFF;
    }
    kit_crt_render(s_src, s_sw, s_sh, s_dst, s_w, s_h, v_crt);
    for (UINT y = 0; y < s_h; y++) memcpy((BYTE *)lr.pBits + (size_t)y * lr.Pitch, s_dst + (size_t)y * s_w, s_w * 4);
    IDirect3DSurface9_UnlockRect(s_sys);
    if (SUCCEEDED(IDirect3DDevice9_UpdateSurface(dev, s_sys, NULL, s_def, NULL)))
        IDirect3DDevice9_StretchRect(dev, s_def, NULL, bb, NULL, D3DTEXF_NONE);
out:
    IDirect3DSurface9_Release(bb);
}

/* ---------- per frame: F9, FPS, CRT ---------- */
/* Remote capture: if capture.req exists next to the exe, the displayed image is written to capture.bmp
 * (checked every 30 frames; used to see the screen of a test machine without being there). */
static void capture_check(IDirect3DDevice9 *dev) {
    static unsigned n; if (++n % 30) return;
    char req[MAX_PATH], out[MAX_PATH]; strcpy(req, v_ini); char *e = strrchr(req, '\\'); if (!e) return;
    strcpy(e + 1, "capture.req"); if (GetFileAttributesA(req) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA(req); strcpy(out, req); strcpy(out + (e + 1 - req), "capture.bmp");
    IDirect3DSurface9 *bb = NULL, *sys = NULL; D3DSURFACE_DESC d;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    IDirect3DSurface9_GetDesc(bb, &d);
    if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
            FILE *f = fopen(out, "wb");
            if (f) {
                BITMAPFILEHEADER fh = { 0x4D42, 0, 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
                BITMAPINFOHEADER ih = { sizeof ih, (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB };
                fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
                for (UINT y = 0; y < d.Height; y++) fwrite((BYTE *)lr.pBits + (size_t)y * lr.Pitch, 4, d.Width, f);
                fclose(f); sale_log("capture: %ux%u -> capture.bmp", d.Width, d.Height);
                if (v_stats) { char dir[MAX_PATH]; strcpy(dir, out); *strrchr(dir, '\\') = 0; strcat(dir, "\\"); stats_capture_rts(dev, dir); }
            }
            IDirect3DSurface9_UnlockRect(sys);
        }
    }
    if (sys) IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(bb);
}
static void frame(IDirect3DDevice9 *dev) {
    capture_check(dev);
    static int f9; static DWORD t0, n; static char base[256];
    HWND fg = GetForegroundWindow();
    DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
    int down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F9) & 0x8000);
    if (down && !f9) {
        v_crt = kit_crt_next(v_crt); char b[8]; sprintf(b, "%d", v_crt);
        WritePrivateProfileStringA("video", "crt", b, v_ini); sale_log("F9: crt=%d", v_crt);
    }
    f9 = down;
    if (v_fps && v_hwnd) {
        DWORD t = GetTickCount(); n++;
        if (!base[0]) GetWindowTextA(v_hwnd, base, sizeof base - 32);
        if (t - t0 >= 1000) { char b[300]; snprintf(b, sizeof b, "%s - %.1f FPS%s", base, n * 1000.0 / (t - t0 ? t - t0 : 1), v_crt ? " - CRT" : ""); SetWindowTextA(v_hwnd, b); t0 = t; n = 0; }
    }
    if (v_crt) crt_apply(dev);
    if (v_stats) stats_frame();
    if (v_cache) cache_frame();
    if (v_nobezel) nobezel_frame();
}

void pause_after_present(HWND hwnd);   /* pause.c */
static int in_dev_present;   /* some implementations (DXVK) go through SwapChain::Present: do not process twice */
static HRESULT STDMETHODCALLTYPE my_Present(IDirect3DDevice9 *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *r) {
    frame(dev);
    in_dev_present = 1; HRESULT hr = real_Present(dev, a, b, w, r); in_dev_present = 0;
    pause_after_present(w ? w : v_hwnd); return hr;
}
static HRESULT STDMETHODCALLTYPE my_SCPresent(IDirect3DSwapChain9 *sc, const RECT *a, const RECT *b, HWND w, const RGNDATA *r, DWORD fl) {
    IDirect3DDevice9 *dev; if (!in_dev_present && SUCCEEDED(IDirect3DSwapChain9_GetDevice(sc, &dev))) { frame(dev); IDirect3DDevice9_Release(dev); }
    HRESULT hr = real_SCPresent(sc, a, b, w, r, fl);
    if (!in_dev_present) pause_after_present(w ? w : v_hwnd);
    return hr;
}
static void apply_pp(D3DPRESENT_PARAMETERS *pp) {
    if (!pp) return;
    if (v_vsync == 0) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    else if (v_vsync == 1) pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (v_bbw > 0 && v_bbh > 0 && pp->BackBufferWidth && pp->BackBufferHeight &&
        (pp->BackBufferWidth != (UINT)v_bbw || pp->BackBufferHeight != (UINT)v_bbh)) {
        g_ow = pp->BackBufferWidth; g_oh = pp->BackBufferHeight;
        sale_log("D3D9: backbuffer %dx%d -> %dx%d", g_ow, g_oh, v_bbw, v_bbh);
        pp->BackBufferWidth = v_bbw; pp->BackBufferHeight = v_bbh;
    }
    sale_log("D3D9: %ux%u windowed=%d fmt=%d interval=0x%x", pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed, pp->BackBufferFormat, pp->PresentationInterval);
}
/* Reduced backbuffer: viewports meant for the original size are scaled down. */
static HRESULT (STDMETHODCALLTYPE *real_SetViewport)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
static HRESULT STDMETHODCALLTYPE my_SetViewport(IDirect3DDevice9 *dev, const D3DVIEWPORT9 *vp) {
    if (g_ow && vp && (vp->Width > (DWORD)v_bbw || vp->Height > (DWORD)v_bbh || (vp->Width == (DWORD)g_ow && vp->Height == (DWORD)g_oh))) {
        D3DVIEWPORT9 v = *vp;
        v.X = vp->X * v_bbw / g_ow; v.Y = vp->Y * v_bbh / g_oh;
        v.Width = vp->Width * v_bbw / g_ow; v.Height = vp->Height * v_bbh / g_oh;
        return real_SetViewport(dev, &v);
    }
    return real_SetViewport(dev, vp);
}
static HRESULT STDMETHODCALLTYPE my_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
    crt_release(); if (v_cache) cache_reset(); if (v_stats) stats_reset(); if (v_nobezel) nobezel_reset();
    apply_pp(pp); return real_Reset(dev, pp);
}

static void patch(void **slot, void *fn, void **orig) {
    if (*slot == fn) return;
    DWORD old; VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    if (!*orig) *orig = *slot;
    *slot = fn; VirtualProtect(slot, sizeof(void *), old, &old);
}
static HRESULT STDMETHODCALLTYPE my_CreateDevice(IDirect3D9 *d3d, UINT ad, D3DDEVTYPE t, HWND w, DWORD fl, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
    apply_pp(pp);
    HRESULT hr = real_CreateDevice(d3d, ad, t, w, fl, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        v_hwnd = pp && pp->hDeviceWindow ? pp->hDeviceWindow : w;
        void **vt = *(void ***)*out;
        patch(&vt[16], (void *)my_Reset, (void **)&real_Reset);
        patch(&vt[17], (void *)my_Present, (void **)&real_Present);
        if (g_ow) patch(&vt[47], (void *)my_SetViewport, (void **)&real_SetViewport);
        { RECT rc; if (GetClientRect(v_hwnd, &rc)) sale_log("D3D9: client window %ldx%ld", rc.right, rc.bottom); }
        IDirect3DSwapChain9 *sc;
        if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(*out, 0, &sc))) { patch(&(*(void ***)sc)[3], (void *)my_SCPresent, (void **)&real_SCPresent); IDirect3DSwapChain9_Release(sc); }
        if (v_stats) { char root[MAX_PATH]; strcpy(root, v_ini); char *e = strrchr(root, '\\'); if (e) e[1] = 0; stats_install(*out, patch, v_dump, root); }
        if (v_cache) cache_install(*out, patch);
        if (v_nobezel) nobezel_install(*out, patch);   /* after stats: stats only sees the calls passed through */
        sale_log("D3D9: device created, hooks installed (crt=%d vsync=%d)", v_crt, v_vsync);
    } else sale_log("D3D9: CreateDevice failed 0x%08lx", hr);
    return hr;
}
static IDirect3D9 *WINAPI my_Create9(UINT sdk) {
    IDirect3D9 *d = real_Create9(sdk);
    if (d) patch(&(*(void ***)d)[16], (void *)my_CreateDevice, (void **)&real_CreateDevice);
    return d;
}

/* Replaces the Direct3DCreate9 entry in the exe's import table. */
static int hook_iat(void) {
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *id = (void *)(base + dd.VirtualAddress); id->Name; id++) {
        if (_stricmp((char *)base + id->Name, "d3d9.dll")) continue;
        IMAGE_THUNK_DATA *on = (void *)(base + id->OriginalFirstThunk), *ft = (void *)(base + id->FirstThunk);
        for (; on->u1.AddressOfData; on++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal)) continue;
            if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + on->u1.AddressOfData))->Name, "Direct3DCreate9")) continue;
            HMODULE m = GetModuleHandleA("d3d9.dll"); if (!m) m = LoadLibraryA("d3d9.dll");
            real_Create9 = (PFN_Create9)GetProcAddress(m, "Direct3DCreate9");
            if (!real_Create9) return 0;
            void *dummy = NULL; patch((void **)&ft->u1.Function, (void *)my_Create9, &dummy);
            return 1;
        }
    }
    return 0;
}

void video_init(const char *ini) {
    strncpy(v_ini, ini, MAX_PATH - 1);
    v_crt = GetPrivateProfileIntA("video", "crt", 0, ini) & 3;
    v_vsync = GetPrivateProfileIntA("video", "vsync", -1, ini);
    v_nobezel = GetPrivateProfileIntA("video", "nobezel", 0, ini);
    v_cache = GetPrivateProfileIntA("video", "cache", 0, ini);
    v_stats = GetPrivateProfileIntA("video", "stats", 0, ini);
    v_dump = GetPrivateProfileIntA("video", "dump", 0, ini);
    v_bbw = GetPrivateProfileIntA("video", "bb_w", 0, ini);
    v_bbh = GetPrivateProfileIntA("video", "bb_h", 0, ini);
    v_fps = GetPrivateProfileIntA("video", "fps", 0, ini);
    v_srcw = GetPrivateProfileIntA("video", "src_w", 0, ini);
    v_srch = GetPrivateProfileIntA("video", "src_h", 0, ini);
    v_scx = GetPrivateProfileIntA("config", "DISP_SCALE_X", 200, ini);
    v_scy = GetPrivateProfileIntA("config", "DISP_SCALE_Y", 200, ini);
    sale_log("video: IAT Direct3DCreate9 %s", hook_iat() ? "hooked" : "not found");
}
