/* d3dstats.c - Senjin Aleste: survey of what the game asks of Direct3D 9 ([video] stats=1).
 * Goal: know what the native renderer (OpenGL, port) will have to cover.
 * - Per-frame counters (averaged every 300 frames): draws by type, primitives, SetTexture,
 *   SetRenderTarget, shaders, FVF/declarations, Clear, StretchRect, modified render states.
 * - Creation log: textures / render targets (size, format, pool, usage), shaders.
 * - [video] dump=1: shaders written to d3d9_dump\vs_<crc>.bin / ps_<crc>.bin (SM2/SM3 bytecode). */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

void sale_log(const char *fmt, ...);
typedef void (*patch_fn)(void **, void *, void **);

static int s_dump; static char s_dir[MAX_PATH];
enum { C_DP, C_DIP, C_DPUP, C_DIPUP, C_PRIM, C_TEX, C_RT, C_VS, C_PS, C_FVF, C_DECL, C_CLEAR, C_STRETCH, C_RS, C_SS, C_VSC, C_PSC, C_N };
static const char *cname[C_N] = { "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "primitives",
    "SetTexture", "SetRenderTarget", "SetVertexShader", "SetPixelShader", "SetFVF", "SetVertexDeclaration", "Clear", "StretchRect",
    "SetRenderState", "SetSamplerState", "SetVertexShaderConstantF", "SetPixelShaderConstantF" };
static unsigned long long acc[C_N]; static unsigned frames;
static unsigned n_tex, n_rt, n_vs, n_ps, fvf_seen[16], n_fvf;

#define DECL(idx, name, ret, args, call, body) \
    static ret (STDMETHODCALLTYPE *o_##name) args; \
    static ret STDMETHODCALLTYPE h_##name args { body; return o_##name call; }

DECL(81, DrawPrimitive, HRESULT, (IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT s, UINT c), (d, t, s, c), { acc[C_DP]++; acc[C_PRIM] += c; })
DECL(82, DrawIndexedPrimitive, HRESULT, (IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT c), (d, t, b, mi, nv, si, c), { acc[C_DIP]++; acc[C_PRIM] += c; })
DECL(83, DrawPrimitiveUP, HRESULT, (IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT c, const void *p, UINT st), (d, t, c, p, st), { acc[C_DPUP]++; acc[C_PRIM] += c; })
DECL(84, DrawIndexedPrimitiveUP, HRESULT, (IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT c, const void *ix, D3DFORMAT f, const void *v, UINT st), (d, t, mi, nv, c, ix, f, v, st), { acc[C_DIPUP]++; acc[C_PRIM] += c; })
DECL(65, SetTexture, HRESULT, (IDirect3DDevice9 *d, DWORD s, IDirect3DBaseTexture9 *t), (d, s, t), { acc[C_TEX]++; })
DECL(37, SetRenderTarget, HRESULT, (IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s), (d, i, s), { acc[C_RT]++; })
DECL(92, SetVertexShader, HRESULT, (IDirect3DDevice9 *d, IDirect3DVertexShader9 *s), (d, s), { acc[C_VS]++; })
DECL(107, SetPixelShader, HRESULT, (IDirect3DDevice9 *d, IDirect3DPixelShader9 *s), (d, s), { acc[C_PS]++; })
DECL(87, SetVertexDeclaration, HRESULT, (IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v), (d, v), { acc[C_DECL]++; })
DECL(43, Clear, HRESULT, (IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD s), (d, n, r, f, c, z, s), { acc[C_CLEAR]++; })
DECL(34, StretchRect, HRESULT, (IDirect3DDevice9 *d, IDirect3DSurface9 *a, const RECT *ra, IDirect3DSurface9 *b, const RECT *rb, D3DTEXTUREFILTERTYPE f), (d, a, ra, b, rb, f), { acc[C_STRETCH]++; })
DECL(57, SetRenderState, HRESULT, (IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v), (d, s, v), { acc[C_RS]++; })
DECL(69, SetSamplerState, HRESULT, (IDirect3DDevice9 *d, DWORD sm, D3DSAMPLERSTATETYPE t, DWORD v), (d, sm, t, v), { acc[C_SS]++; })
DECL(94, SetVertexShaderConstantF, HRESULT, (IDirect3DDevice9 *d, UINT r, const float *p, UINT n), (d, r, p, n), { acc[C_VSC]++; })
DECL(109, SetPixelShaderConstantF, HRESULT, (IDirect3DDevice9 *d, UINT r, const float *p, UINT n), (d, r, p, n), { acc[C_PSC]++; })
DECL(89, SetFVF, HRESULT, (IDirect3DDevice9 *d, DWORD fvf), (d, fvf), {
    acc[C_FVF]++; int k; for (k = 0; k < (int)n_fvf; k++) if (fvf_seen[k] == fvf) break;
    if (k == (int)n_fvf && n_fvf < 16) { fvf_seen[n_fvf++] = fvf; sale_log("stats: FVF 0x%lx", fvf); } })

static int n_rtslot;   /* RT textures created, for the capture */
static IDirect3DTexture9 *rt_tex[8];
static HRESULT (STDMETHODCALLTYPE *o_CreateTexture)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
static HRESULT STDMETHODCALLTYPE h_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT lv, DWORD us, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9 **t, HANDLE *sh) {
    HRESULT hr = o_CreateTexture(d, w, h, lv, us, f, p, t, sh);
    if (us & D3DUSAGE_RENDERTARGET) { if (n_rt++ < 64) sale_log("stats: RT texture %ux%u fmt=%d usage=0x%lx hr=0x%lx", w, h, f, us, hr);
        /* stats_capture_rts dereferences these later: keep a reference (dropped in stats_reset) */
        if (SUCCEEDED(hr) && t && *t && n_rtslot < 8) { rt_tex[n_rtslot++] = *t; IDirect3DTexture9_AddRef(*t); } }
    else if (n_tex++ < 400) sale_log("stats: texture %ux%u lvl=%u fmt=%d pool=%d usage=0x%lx hr=0x%lx", w, h, lv, f, p, us, hr);
    return hr;
}
/* Device Reset: D3DPOOL_DEFAULT resources (all RTs) must be released before it can succeed */
void stats_reset(void) {
    for (int i = 0; i < n_rtslot; i++) IDirect3DTexture9_Release(rt_tex[i]);
    n_rtslot = 0;
}
/* writes each known RT texture as BMP (rt0.bmp, rt1.bmp...) into dir */
void stats_capture_rts(IDirect3DDevice9 *dev, const char *dir) {
    for (int i = 0; i < n_rtslot; i++) {
        IDirect3DSurface9 *s = NULL, *sys = NULL; D3DSURFACE_DESC d;
        if (FAILED(IDirect3DTexture9_GetSurfaceLevel(rt_tex[i], 0, &s))) continue;
        IDirect3DSurface9_GetDesc(s, &d);
        if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
            SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, s, sys))) {
            D3DLOCKED_RECT lr; int bpp = (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) ? 4 : 0;
            if (bpp && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
                char p[MAX_PATH]; snprintf(p, sizeof p, "%srt%d.bmp", dir, i); FILE *f = fopen(p, "wb");
                if (f) { BITMAPFILEHEADER fh = { 0x4D42, 0, 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
                    BITMAPINFOHEADER ih = { sizeof ih, (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB };
                    fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4; fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
                    for (UINT y = 0; y < d.Height; y++) fwrite((BYTE *)lr.pBits + (size_t)y * lr.Pitch, 4, d.Width, f);
                    fclose(f); sale_log("capture: RT %d %ux%u -> rt%d.bmp", i, d.Width, d.Height, i); }
                IDirect3DSurface9_UnlockRect(sys);
            }
        }
        if (sys) IDirect3DSurface9_Release(sys); IDirect3DSurface9_Release(s);
    }
}
DECL(28, CreateRenderTarget, HRESULT, (IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lk, IDirect3DSurface9 **s, HANDLE *sh), (d, w, h, f, ms, q, lk, s, sh), {
    sale_log("stats: RT surface %ux%u fmt=%d ms=%d", w, h, f, ms); })
DECL(29, CreateDepthStencilSurface, HRESULT, (IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL dc, IDirect3DSurface9 **s, HANDLE *sh), (d, w, h, f, ms, q, dc, s, sh), {
    sale_log("stats: depth %ux%u fmt=%d", w, h, f); })

/* SM bytecode: version token, then tokens up to 0x0000FFFF */
static void dump_shader(const char *kind, const DWORD *fn) {
    if (!fn) return;
    unsigned n = 1; while (n < 65536 && fn[n] != 0x0000FFFF) n++; n++;
    DWORD crc = 0x811C9DC5; for (unsigned i = 0; i < n; i++) crc = (crc ^ fn[i]) * 0x01000193;
    sale_log("stats: %s %s_%d_%d, %u tokens, id %08lx", kind, kind, (int)((fn[0] >> 8) & 0xFF), (int)(fn[0] & 0xFF), n, crc);
    if (!s_dump) return;
    char p[MAX_PATH]; CreateDirectoryA(s_dir, NULL); snprintf(p, sizeof p, "%s\\%s_%08lx.bin", s_dir, kind, crc);
    FILE *f = fopen(p, "wb"); if (f) { fwrite(fn, 4, n, f); fclose(f); }
}
DECL(91, CreateVertexShader, HRESULT, (IDirect3DDevice9 *d, const DWORD *fn, IDirect3DVertexShader9 **s), (d, fn, s), { n_vs++; dump_shader("vs", fn); })
DECL(106, CreatePixelShader, HRESULT, (IDirect3DDevice9 *d, const DWORD *fn, IDirect3DPixelShader9 **s), (d, fn, s), { n_ps++; dump_shader("ps", fn); })

void stats_frame(void) {
    if (!o_DrawPrimitive) return;
    if (++frames < 300) return;
    char b[1400]; int o = snprintf(b, sizeof b, "stats: per-frame average over %u frames:", frames);
    for (int i = 0; i < C_N && o >= 0 && o < (int)sizeof b; i++) if (acc[i]) o += snprintf(b + o, sizeof b - o, " %s=%.1f", cname[i], (double)acc[i] / frames);
    sale_log("%s", b);
    sale_log("stats: created since start: %u textures, %u RT, %u VS, %u PS", n_tex, n_rt, n_vs, n_ps);
    memset(acc, 0, sizeof acc); frames = 0;
}

void stats_install(IDirect3DDevice9 *dev, patch_fn patch, int dump, const char *root) {
    s_dump = dump; snprintf(s_dir, sizeof s_dir, "%sd3d9_dump", root);
    void **vt = *(void ***)dev;
#define P(idx, name) patch(&vt[idx], (void *)h_##name, (void **)&o_##name)
    P(81, DrawPrimitive); P(82, DrawIndexedPrimitive); P(83, DrawPrimitiveUP); P(84, DrawIndexedPrimitiveUP);
    P(65, SetTexture); P(37, SetRenderTarget); P(92, SetVertexShader); P(107, SetPixelShader); P(87, SetVertexDeclaration);
    P(43, Clear); P(34, StretchRect); P(57, SetRenderState); P(69, SetSamplerState); P(94, SetVertexShaderConstantF);
    P(109, SetPixelShaderConstantF); P(89, SetFVF); P(23, CreateTexture); P(28, CreateRenderTarget);
    P(29, CreateDepthStencilSurface); P(91, CreateVertexShader); P(106, CreatePixelShader);
    sale_log("stats: Direct3D 9 survey active (dump=%d)", dump);
}
