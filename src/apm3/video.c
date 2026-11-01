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
static int v_fs, v_fs_applied = -1;   /* [config] FULLSCREEN: borderless fullscreen window (F11 / Alt+Enter) */

/* Fullscreen = borderless window covering the monitor (desktop resolution; D3D9 stretches the backbuffer to the
 * client area). The game's exclusive fullscreen is replaced by this (apply_pp), so switching is instant. */
static LONG s_style, s_exstyle; static RECT s_rect; static int s_saved;
static void set_fullscreen(HWND h, int on) {
    if (!h) return;
    MONITORINFO mi = { sizeof mi }; GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    RECT m = mi.rcMonitor;
    if (on) {
        if (!s_saved) { s_style = GetWindowLongA(h, GWL_STYLE); s_exstyle = GetWindowLongA(h, GWL_EXSTYLE); GetWindowRect(h, &s_rect); s_saved = 1; }
        SetWindowLongA(h, GWL_STYLE, (s_style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_POPUP | WS_VISIBLE);
        SetWindowLongA(h, GWL_EXSTYLE, s_exstyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(h, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        LONG st = s_saved ? s_style : GetWindowLongA(h, GWL_STYLE); RECT r = s_rect;
        if (!s_saved || !(st & WS_CAPTION)) {   /* started fullscreen: make a normal window, 80 % of the monitor, 16:9, centred */
            st = (st & ~WS_POPUP) | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
            int mw = m.right - m.left, mh = m.bottom - m.top, cw = mw * 8 / 10, chh = cw * 9 / 16;
            if (chh > mh * 8 / 10) { chh = mh * 8 / 10; cw = chh * 16 / 9; }
            r.left = m.left + (mw - cw) / 2; r.top = m.top + (mh - chh) / 2; r.right = r.left + cw; r.bottom = r.top + chh;
            AdjustWindowRect(&r, (DWORD)st, FALSE);
        }
        SetWindowLongA(h, GWL_STYLE, st); if (s_saved) SetWindowLongA(h, GWL_EXSTYLE, s_exstyle);
        SetWindowPos(h, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    sale_log("display: %s", on ? "fullscreen (borderless)" : "window");
}

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
static int ensure_out(IDirect3DDevice9 *dev, const D3DSURFACE_DESC *dd) {
    D3DSURFACE_DESC d = *dd;
    if (d.Width != s_w || d.Height != s_h || d.Format != s_fmt) {
        crt_release();
        if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &s_sys, NULL)) ||
            FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_DEFAULT, &s_def, NULL))) { crt_release(); return 0; }
        s_w = d.Width; s_h = d.Height; s_fmt = d.Format;
        s_sw = v_srcw > 0 ? v_srcw : (int)(d.Width * 100 / (v_scx > 0 ? v_scx : 100));
        s_sh = v_srch > 0 ? v_srch : (int)(d.Height * 100 / (v_scy > 0 ? v_scy : 100));
        if (s_sw < 1 || s_sw > (int)d.Width) s_sw = d.Width; if (s_sh < 1 || s_sh > (int)d.Height) s_sh = d.Height;
        free(s_src); free(s_dst); s_src = malloc((size_t)s_sw * s_sh * 4); s_dst = malloc((size_t)d.Width * d.Height * 4);
        sale_log("CRT: backbuffer %ux%u fmt %d -> source %dx%d", d.Width, d.Height, d.Format, s_sw, s_sh);
    }
    return 1;
}
static void crt_apply(IDirect3DDevice9 *dev) {
    IDirect3DSurface9 *bb = NULL; D3DSURFACE_DESC d;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    IDirect3DSurface9_GetDesc(bb, &d);
    if ((d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) || d.MultiSampleType != D3DMULTISAMPLE_NONE) goto out;
    if (!ensure_out(dev, &d)) goto out;
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

/* ---------- TATE ([video] tate=1, F7): playfield only, turned 90 degrees, scaled to fill ----------
 * The game draws its playfield into a 224x288 render target (captured at CreateTexture). Each frame it is read
 * back, turned (tate_dir 1 = monitor turned clockwise -> image turned counter-clockwise, 2 = the other way),
 * scaled to fill the backbuffer and written over it; the kit CRT filter is applied after the rotation (scanlines
 * follow the monitor's own lines, as on a cabinet). Texts drawn outside the playfield are not shown. */
static int v_tate, v_tate_dir = 1;
static IDirect3DTexture9 *t_play; static IDirect3DSurface9 *t_sys;   /* playfield RT (reference held) + CPU copy */
static uint32_t *t_rot, *t_crt; static size_t t_crt_n;
static HRESULT (STDMETHODCALLTYPE *real_CreateTexture)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
static HRESULT STDMETHODCALLTYPE my_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT lv, DWORD us, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9 **t, HANDLE *sh) {
    HRESULT hr = real_CreateTexture(d, w, h, lv, us, f, p, t, sh);
    if (SUCCEEDED(hr) && t && *t && (us & D3DUSAGE_RENDERTARGET) && w == 224 && h == 288) {
        if (t_play) IDirect3DTexture9_Release(t_play);
        t_play = *t; IDirect3DTexture9_AddRef(t_play);
    }
    return hr;
}
static void tate_release(void) {
    if (t_play) IDirect3DTexture9_Release(t_play); if (t_sys) IDirect3DSurface9_Release(t_sys);
    t_play = NULL; t_sys = NULL;
}
static int ensure_out(IDirect3DDevice9 *dev, const D3DSURFACE_DESC *d);   /* CPU / GPU surfaces of backbuffer size (CRT) */
static int tate_apply(IDirect3DDevice9 *dev) {
    IDirect3DSurface9 *bb = NULL, *ps = NULL; D3DSURFACE_DESC d, pd; int ok = 0;
    if (!t_play || FAILED(IDirect3DTexture9_GetSurfaceLevel(t_play, 0, &ps))) return 0;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) { IDirect3DSurface9_Release(ps); return 0; }
    IDirect3DSurface9_GetDesc(bb, &d); IDirect3DSurface9_GetDesc(ps, &pd);
    if ((d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) || (pd.Format != D3DFMT_X8R8G8B8 && pd.Format != D3DFMT_A8R8G8B8) || d.MultiSampleType != D3DMULTISAMPLE_NONE) goto out;
    if (!t_sys && FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, pd.Width, pd.Height, pd.Format, D3DPOOL_SYSTEMMEM, &t_sys, NULL))) goto out;
    if (!ensure_out(dev, &d)) goto out;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, ps, t_sys))) goto out;
    const int pw = (int)pd.Width, ph = (int)pd.Height, rw = ph, rh = pw;   /* rotated: 288x224 */
    if (!t_rot) t_rot = malloc((size_t)rw * rh * 4);
    D3DLOCKED_RECT lr; if (FAILED(IDirect3DSurface9_LockRect(t_sys, &lr, NULL, D3DLOCK_READONLY))) goto out;
    for (int y = 0; y < rh; y++) for (int x = 0; x < rw; x++) {
        int sx = v_tate_dir == 2 ? y : pw - 1 - y, sy = v_tate_dir == 2 ? ph - 1 - x : x;
        t_rot[(size_t)y * rw + x] = ((const uint32_t *)((const BYTE *)lr.pBits + (size_t)sy * lr.Pitch))[sx] & 0xFFFFFF;
    }
    IDirect3DSurface9_UnlockRect(t_sys);
    double sc = (double)d.Width / rw < (double)d.Height / rh ? (double)d.Width / rw : (double)d.Height / rh;
    int dw = (int)(rw * sc), dh = (int)(rh * sc), dx = ((int)d.Width - dw) / 2, dy = ((int)d.Height - dh) / 2;
    memset(s_dst, 0, (size_t)s_w * s_h * 4);
    if (kit_crt_usable(v_crt, rw, rh, dw, dh)) {
        if (t_crt_n < (size_t)dw * dh) { free(t_crt); t_crt = malloc((size_t)dw * dh * 4); t_crt_n = (size_t)dw * dh; }
        kit_crt_render(t_rot, rw, rh, t_crt, dw, dh, v_crt);
        for (int y = 0; y < dh; y++) memcpy(s_dst + (size_t)(y + dy) * s_w + dx, t_crt + (size_t)y * dw, (size_t)dw * 4);
    } else {
        for (int y = 0; y < dh; y++) { const uint32_t *row = t_rot + (size_t)((y * 2 + 1) * rh / (2 * dh)) * rw; uint32_t *o = s_dst + (size_t)(y + dy) * s_w + dx;
            for (int x = 0; x < dw; x++) o[x] = row[(x * 2 + 1) * rw / (2 * dw)]; }
    }
    if (FAILED(IDirect3DSurface9_LockRect(s_sys, &lr, NULL, 0))) goto out;
    for (UINT y = 0; y < s_h; y++) memcpy((BYTE *)lr.pBits + (size_t)y * lr.Pitch, s_dst + (size_t)y * s_w, s_w * 4);
    IDirect3DSurface9_UnlockRect(s_sys);
    if (SUCCEEDED(IDirect3DDevice9_UpdateSurface(dev, s_sys, NULL, s_def, NULL)) && SUCCEEDED(IDirect3DDevice9_StretchRect(dev, s_def, NULL, bb, NULL, D3DTEXF_NONE))) ok = 1;
out:
    IDirect3DSurface9_Release(ps); IDirect3DSurface9_Release(bb);
    return ok;
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
    { static int kfs; int fsk = pid == GetCurrentProcessId() && ((GetAsyncKeyState(VK_F11) & 0x8000) || ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_RETURN) & 0x8000)));
      if (fsk && !kfs) { v_fs = !v_fs; WritePrivateProfileStringA("config", "FULLSCREEN", v_fs ? "1" : "0", v_ini); }
      kfs = fsk;
      if (v_fs != v_fs_applied && v_hwnd) { if (v_fs || v_fs_applied >= 0) set_fullscreen(v_hwnd, v_fs); v_fs_applied = v_fs; } }   /* window at start: left as the game made it */
    if (v_fps && v_hwnd) {
        DWORD t = GetTickCount(); n++;
        if (!base[0]) GetWindowTextA(v_hwnd, base, sizeof base - 32);
        if (t - t0 >= 1000) { char b[300]; snprintf(b, sizeof b, "%s - %.1f FPS%s", base, n * 1000.0 / (t - t0 ? t - t0 : 1), v_crt ? " - CRT" : ""); SetWindowTextA(v_hwnd, b); t0 = t; n = 0; }
    }
    { static int k7; int dn = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F7) & 0x8000);
      if (dn && !k7) { v_tate = !v_tate; WritePrivateProfileStringA("video", "tate", v_tate ? "1" : "0", v_ini); sale_log("F7: TATE %s", v_tate ? "on" : "off"); }
      k7 = dn; }
    if (!(v_tate && tate_apply(dev)) && v_crt) crt_apply(dev);
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
    if (!pp->Windowed) { pp->Windowed = TRUE; pp->FullScreen_RefreshRateInHz = 0; v_fs = 1; sale_log("D3D9: exclusive fullscreen replaced by a borderless window"); }
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
    crt_release(); tate_release(); if (v_cache) cache_reset(); if (v_stats) stats_reset(); if (v_nobezel) nobezel_reset();
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
        patch(&vt[23], (void *)my_CreateTexture, (void **)&real_CreateTexture);   /* TATE: playfield RT */
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
    v_tate = GetPrivateProfileIntA("video", "tate", 0, ini) != 0;
    v_tate_dir = GetPrivateProfileIntA("video", "tate_dir", 1, ini) == 2 ? 2 : 1;
    v_cache = GetPrivateProfileIntA("video", "cache", 0, ini);
    v_stats = GetPrivateProfileIntA("video", "stats", 0, ini);
    v_dump = GetPrivateProfileIntA("video", "dump", 0, ini);
    v_bbw = GetPrivateProfileIntA("video", "bb_w", 0, ini);
    v_bbh = GetPrivateProfileIntA("video", "bb_h", 0, ini);
    v_fps = GetPrivateProfileIntA("video", "fps", 0, ini);
    v_srcw = GetPrivateProfileIntA("video", "src_w", 0, ini);
    v_srch = GetPrivateProfileIntA("video", "src_h", 0, ini);
    v_fs = GetPrivateProfileIntA("config", "FULLSCREEN", 0, ini) != 0;
    v_scx = GetPrivateProfileIntA("config", "DISP_SCALE_X", 200, ini);
    v_scy = GetPrivateProfileIntA("config", "DISP_SCALE_Y", 200, ini);
    sale_log("video: IAT Direct3DCreate9 %s", hook_iat() ? "hooked" : "not found");
}
