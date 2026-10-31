/* d3dnobezel.c - Senjin Aleste : test « aire de jeu seule » ([video] nobezel=1).
 * Le jeu dessine l'aire de jeu dans une RT 224x288, puis compose dans une RT 1920x1080 :
 * décor de bordure (grandes textures) + aire de jeu agrandie. Ici, pendant la composition dans la
 * grande RT, on ne laisse passer que les dessins qui lisent la RT 224x288 : la bordure n'est plus dessinée.
 * Sert à mesurer le coût de la bordure ; la version native aura une vraie option. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>

void sale_log(const char *fmt, ...);
typedef void (*patch_fn)(void **, void *, void **);

static IDirect3DTexture9 *play_tex;          /* RT 224x288 */
static IDirect3DSurface9 *big_surf;          /* niveau 0 de la RT 1920x1080 */
static IDirect3DTexture9 *big_tex;
static int on_big; static IDirect3DBaseTexture9 *tex0;
static unsigned long long skipped;

static HRESULT (STDMETHODCALLTYPE *o_CreateTexture)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
static HRESULT STDMETHODCALLTYPE n_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT lv, DWORD us, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9 **t, HANDLE *sh) {
    HRESULT hr = o_CreateTexture(d, w, h, lv, us, f, p, t, sh);
    if (SUCCEEDED(hr) && t && *t && (us & D3DUSAGE_RENDERTARGET)) {
        if (w == 224 && h == 288) play_tex = *t;
        else if (w >= 640 && h >= 480) { big_tex = *t; big_surf = NULL; }
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
void nobezel_frame(void) {
    static unsigned f; if (++f % 600 == 0) { sale_log("nobezel : %llu dessins de bordure évités (600 images), RT jeu %s", skipped, play_tex ? "trouvée" : "absente"); skipped = 0; }
}
void nobezel_install(IDirect3DDevice9 *dev, patch_fn patch) {
    void **vt = *(void ***)dev;
    patch(&vt[23], (void *)n_CreateTexture, (void **)&o_CreateTexture);
    patch(&vt[37], (void *)n_SetRenderTarget, (void **)&o_SetRenderTarget);
    patch(&vt[65], (void *)n_SetTexture, (void **)&o_SetTexture);
    patch(&vt[83], (void *)n_DrawPrimitiveUP, (void **)&o_DrawPrimitiveUP);
    patch(&vt[81], (void *)n_DrawPrimitive, (void **)&o_DrawPrimitive);
    sale_log("nobezel : aire de jeu seule (test)");
}
