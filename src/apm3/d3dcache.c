/* d3dcache.c - Senjin Aleste : filtre des appels Direct3D 9 redondants ([video] cache=1).
 * Constat (relevé d3dstats, 2026-10-31) : par image, ~40 draws mais ~2400 SetFVF, ~2240 SetRenderState
 * et ~2400 SetVertexShaderConstantF, presque tous identiques à la valeur déjà en place. Chaque appel coûte
 * (pilote, Wine wow64) : on ne transmet que ceux qui changent quelque chose. Code natif, pas d'émulation.
 * Invalidé sur Reset et pendant l'enregistrement d'un state block. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <string.h>

void sale_log(const char *fmt, ...);
typedef void (*patch_fn)(void **, void *, void **);

static DWORD rs_val[256]; static BYTE rs_ok[256];
static DWORD fvf_val; static int fvf_ok;
static float vsc[256][4]; static BYTE vsc_ok[256];
static int recording;   /* 1 = cache coupé (state blocks utilisés par le jeu : Apply contournerait le cache) */
static unsigned long long skipped, passed;

static void invalidate(void) { memset(rs_ok, 0, sizeof rs_ok); memset(vsc_ok, 0, sizeof vsc_ok); fvf_ok = 0; }

static HRESULT (STDMETHODCALLTYPE *o_SetRenderState)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
static HRESULT STDMETHODCALLTYPE c_SetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) {
    if (!recording && (unsigned)s < 256) {
        if (rs_ok[s] && rs_val[s] == v) { skipped++; return D3D_OK; }
        rs_val[s] = v; rs_ok[s] = 1;
    }
    passed++; return o_SetRenderState(d, s, v);
}
static HRESULT (STDMETHODCALLTYPE *o_SetFVF)(IDirect3DDevice9 *, DWORD);
static HRESULT STDMETHODCALLTYPE c_SetFVF(IDirect3DDevice9 *d, DWORD f) {
    if (!recording) { if (fvf_ok && fvf_val == f) { skipped++; return D3D_OK; } fvf_val = f; fvf_ok = 1; }
    passed++; return o_SetFVF(d, f);
}
static HRESULT (STDMETHODCALLTYPE *o_SetVertexDeclaration)(IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *);
static HRESULT STDMETHODCALLTYPE c_SetVertexDeclaration(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v) {
    fvf_ok = 0; return o_SetVertexDeclaration(d, v);       /* change implicitement le FVF */
}
static HRESULT (STDMETHODCALLTYPE *o_SetVSConstF)(IDirect3DDevice9 *, UINT, const float *, UINT);
static HRESULT STDMETHODCALLTYPE c_SetVSConstF(IDirect3DDevice9 *d, UINT r, const float *p, UINT n) {
    if (!recording && p && r + n <= 256) {
        UINT i; for (i = 0; i < n; i++) if (!vsc_ok[r + i] || memcmp(vsc[r + i], p + i * 4, 16)) break;
        if (i == n) { skipped++; return D3D_OK; }
        memcpy(vsc[r], p, n * 16); memset(vsc_ok + r, 1, n);
    } else if (p && r < 256) memset(vsc_ok + r, 0, (r + n <= 256 ? n : 256 - r));
    passed++; return o_SetVSConstF(d, r, p, n);
}
static HRESULT (STDMETHODCALLTYPE *o_BeginStateBlock)(IDirect3DDevice9 *);
static HRESULT STDMETHODCALLTYPE c_BeginStateBlock(IDirect3DDevice9 *d) { if (!recording) sale_log("cache : state blocks utilisés, cache coupé"); recording = 1; return o_BeginStateBlock(d); }
static HRESULT (STDMETHODCALLTYPE *o_EndStateBlock)(IDirect3DDevice9 *, IDirect3DStateBlock9 **);
static HRESULT STDMETHODCALLTYPE c_EndStateBlock(IDirect3DDevice9 *d, IDirect3DStateBlock9 **b) { return o_EndStateBlock(d, b); }
static HRESULT (STDMETHODCALLTYPE *o_CreateStateBlock)(IDirect3DDevice9 *, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9 **);
static HRESULT STDMETHODCALLTYPE c_CreateStateBlock(IDirect3DDevice9 *d, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9 **b) { if (!recording) sale_log("cache : state blocks utilisés, cache coupé"); recording = 1; return o_CreateStateBlock(d, t, b); }

void cache_reset(void) { invalidate(); }
void cache_frame(void) {
    static unsigned f;
    if (++f % 600 == 0) { sale_log("cache : %llu appels évités, %llu transmis (600 images)", skipped, passed); skipped = passed = 0; }
}
void cache_install(IDirect3DDevice9 *dev, patch_fn patch) {
    void **vt = *(void ***)dev; invalidate();
    patch(&vt[57], (void *)c_SetRenderState, (void **)&o_SetRenderState);
    patch(&vt[89], (void *)c_SetFVF, (void **)&o_SetFVF);
    patch(&vt[87], (void *)c_SetVertexDeclaration, (void **)&o_SetVertexDeclaration);
    patch(&vt[94], (void *)c_SetVSConstF, (void **)&o_SetVSConstF);
    patch(&vt[59], (void *)c_CreateStateBlock, (void **)&o_CreateStateBlock);
    patch(&vt[60], (void *)c_BeginStateBlock, (void **)&o_BeginStateBlock);
    patch(&vt[61], (void *)c_EndStateBlock, (void **)&o_EndStateBlock);
    sale_log("cache : filtre des appels redondants actif");
}
