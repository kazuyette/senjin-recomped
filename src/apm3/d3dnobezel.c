/* d3dnobezel.c - Senjin Aleste: "playfield only" test ([video] nobezel=1).
 * The game draws the playfield into a 224x288 RT, then composites into a 1920x1080 RT:
 * bezel artwork (large textures) + scaled-up playfield. Here, while compositing into the
 * large RT, only the draws that read the 224x288 RT are let through: the bezel is no longer drawn.
 * Used to measure the cost of the bezel; the native version will have a real option. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>

void sale_log(const char *fmt, ...);
typedef void (*patch_fn)(void **, void *, void **);

static IDirect3DTexture9 *play_tex;          /* RT 224x288 */
static IDirect3DSurface9 *big_surf;          /* level 0 of the 1920x1080 RT */
static IDirect3DTexture9 *big_tex;
static int on_big; static IDirect3DBaseTexture9 *tex0;
static unsigned long long skipped;

static HRESULT (STDMETHODCALLTYPE *o_CreateTexture)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
static HRESULT STDMETHODCALLTYPE n_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT lv, DWORD us, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9 **t, HANDLE *sh) {
    HRESULT hr = o_CreateTexture(d, w, h, lv, us, f, p, t, sh);
    if (SUCCEEDED(hr) && t && *t && (us & D3DUSAGE_RENDERTARGET)) {
        if (w == 224 && h == 288) play_tex = *t;
        /* big_tex is dereferenced later (GetSurfaceLevel): keep a reference, dropped in nobezel_reset;
         * play_tex and big_surf are only compared, never dereferenced */
        else if (w >= 640 && h >= 480) { if (big_tex) IDirect3DTexture9_Release(big_tex); big_tex = *t; IDirect3DTexture9_AddRef(big_tex); big_surf = NULL; }
    }
    return hr;
}
static HRESULT (STDMETHODCALLTYPE *o_SetRenderTarget)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
static HRESULT STDMETHODCALLTYPE n_SetRenderTarget(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s) {
    if (i == 0) {
        if (big_tex && !big_surf) { IDirect3DTexture9_GetSurfaceLevel(big_tex, 0, &big_surf); if (big_surf) IDirect3DSurface9_Release(big_surf); }
        on_big = s && s == big_surf;
    }
    return o_SetRenderTarget(d, i, s);
}
static HRESULT (STDMETHODCALLTYPE *o_SetTexture)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
static HRESULT STDMETHODCALLTYPE n_SetTexture(IDirect3DDevice9 *d, DWORD s, IDirect3DBaseTexture9 *t) {
    if (s == 0) tex0 = t;
    return o_SetTexture(d, s, t);
}
static int keep(void) { return !on_big || !play_tex || tex0 == (IDirect3DBaseTexture9 *)play_tex; }
static HRESULT (STDMETHODCALLTYPE *o_DrawPrimitiveUP)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
static HRESULT STDMETHODCALLTYPE n_DrawPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT c, const void *p, UINT st) {
    if (!keep()) { skipped++; return D3D_OK; }
    return o_DrawPrimitiveUP(d, t, c, p, st);
}
static HRESULT (STDMETHODCALLTYPE *o_DrawPrimitive)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
static HRESULT STDMETHODCALLTYPE n_DrawPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT s, UINT c) {
    if (!keep()) { skipped++; return D3D_OK; }
    return o_DrawPrimitive(d, t, s, c);
}
/* Device Reset: D3DPOOL_DEFAULT resources must be released before it can succeed */
void nobezel_reset(void) {
    if (big_tex) IDirect3DTexture9_Release(big_tex);
    big_tex = NULL; big_surf = NULL; play_tex = NULL; on_big = 0;
}
void nobezel_frame(void) {
    static unsigned f; if (++f % 600 == 0) { sale_log("nobezel: %llu bezel draws skipped (600 frames), game RT %s", skipped, play_tex ? "found" : "missing"); skipped = 0; }
}
void nobezel_install(IDirect3DDevice9 *dev, patch_fn patch) {
    void **vt = *(void ***)dev;
    patch(&vt[23], (void *)n_CreateTexture, (void **)&o_CreateTexture);
    patch(&vt[37], (void *)n_SetRenderTarget, (void **)&o_SetRenderTarget);
    patch(&vt[65], (void *)n_SetTexture, (void **)&o_SetTexture);
    patch(&vt[83], (void *)n_DrawPrimitiveUP, (void **)&o_DrawPrimitiveUP);
    patch(&vt[81], (void *)n_DrawPrimitive, (void **)&o_DrawPrimitive);
    sale_log("nobezel: playfield only (test)");
}
