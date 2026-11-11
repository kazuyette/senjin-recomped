/* sa_d3d9.c - Direct3D 9 rewritten natively on OpenGL 3.3 (the subset Senjin Aleste needs).
 * The game's COM objects are small structures in MEM: [vtable][host id]; their methods are
 * native imports "D3D9!Interface::Method". Rendering: GL textures, render targets = FBOs (stored "top to bottom"
 * like D3D: row 0 is at the top), SM 2.0 shaders translated to GLSL (sa_d3dsm.c), minimal fixed pipeline.
 * This is not emulation: it is a native implementation of the API, as DXVK/wined3d do. */
#define GL_GLEXT_PROTOTYPES
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "sa_host.h"

extern SDL_Window *sa_window;
char *sm_to_glsl(const uint32_t *tok, int *is_ps);           /* sa_d3dsm.c */

/* ================= objects ================= */
enum { O_D3D = 1, O_DEV, O_TEX, O_SURF, O_VS, O_PS, O_SC, O_VB, O_DECL, O_QUERY, O_SB };
typedef struct obj {
    int type, ref; uint32_t g;                  /* address of the object in MEM */
    int w, h, levels; uint32_t fmt, usage, pool;
    GLuint tex, fbo; int is_rt;
    uint32_t lockmem[16];                       /* CPU copy per level while locked (textures) */
    uint32_t filled;                            /* levels uploaded at least once (bit per level) */
    struct obj *parent; int level;              /* surface: parent texture and level (or NULL) */
    struct obj *lvsurf[16];                     /* texture: surface returned by GetSurfaceLevel, per level */
    GLuint sh; uint32_t *code; int ncode;       /* shaders */
    uint32_t vbmem; uint32_t size;              /* vertex buffer */
} obj;
#define MAXO 8192
static obj *O[MAXO];
static uint32_t vt_d3d, vt_dev, vt_tex, vt_surf, vt_vs, vt_ps, vt_sc, vt_vb, vt_decl, vt_query;
static obj *mk(int type, uint32_t vt) {
    for (int i = 1; i < MAXO; i++) if (!O[i]) {
        obj *o = calloc(1, sizeof *o); o->type = type; o->ref = 1; o->g = g_alloc(16, 1);
        wr32(o->g, vt); wr32(o->g + 4, (uint32_t)i); wr32(o->g + 8, 0x44334439u); O[i] = o; return o;
    }
    sa_fatal("d3d9: too many objects"); return NULL;
}
static obj *me(void) { uint32_t i = rd32(ARG(0) + 4); return (i < MAXO && rd32(ARG(0) + 8) == 0x44334439u) ? O[i] : NULL; }
static obj *og(uint32_t g) { if (!g) return NULL; uint32_t i = rd32(g + 4); return (i < MAXO && rd32(g + 8) == 0x44334439u) ? O[i] : NULL; }

/* ================= device state ================= */
static SDL_GLContext ctx; static SDL_threadID ctx_thread;
static int bb_w = 1920, bb_h = 1080; static obj *backbuf, *dev_o, *sc_o, *autodepth;
static obj *rt0; static obj *tex_bound[8]; static obj *vs_cur, *ps_cur;
static uint32_t fvf, rs[256], tss[8][33], samp[8][14];
static float vsc[256][4], psc[32][4], xf[512][16];
static struct { uint32_t x, y, w, h; float mn, mx; } vp;
static struct { int32_t l, t, r, b; } scis;
static GLuint vao, vbo, ff_prog, cur_prog; static int in_scene;

static void cur(void) {
    if (!ctx) return;
    if (SDL_ThreadID() != ctx_thread) { SDL_GL_MakeCurrent(sa_window, ctx); ctx_thread = SDL_ThreadID(); }
}

/* ================= formats ================= */
static int fmt_gl(uint32_t f, GLenum *ifmt, GLenum *pf, GLenum *ty, int *bpp, int *compressed, GLint sw[4]) {
    sw[0] = GL_RED; sw[1] = GL_GREEN; sw[2] = GL_BLUE; sw[3] = GL_ALPHA; *compressed = 0;
    switch (f) {
    case 21: *ifmt = GL_RGBA8; *pf = GL_BGRA; *ty = GL_UNSIGNED_BYTE; *bpp = 4; return 1;            /* A8R8G8B8 */
    case 22: *ifmt = GL_RGBA8; *pf = GL_BGRA; *ty = GL_UNSIGNED_BYTE; *bpp = 4; sw[3] = GL_ONE; return 1;  /* X8R8G8B8 */
    case 23: *ifmt = GL_RGB565; *pf = GL_RGB; *ty = GL_UNSIGNED_SHORT_5_6_5; *bpp = 2; return 1;     /* R5G6B5 */
    case 25: *ifmt = GL_RGB5_A1; *pf = GL_BGRA; *ty = GL_UNSIGNED_SHORT_1_5_5_5_REV; *bpp = 2; return 1;
    case 26: *ifmt = GL_RGBA4; *pf = GL_BGRA; *ty = GL_UNSIGNED_SHORT_4_4_4_4_REV; *bpp = 2; return 1;
    case 28: *ifmt = GL_R8; *pf = GL_RED; *ty = GL_UNSIGNED_BYTE; *bpp = 1; sw[0] = sw[1] = sw[2] = GL_ZERO; sw[3] = GL_RED; return 1;   /* A8 */
    case 50: *ifmt = GL_R8; *pf = GL_RED; *ty = GL_UNSIGNED_BYTE; *bpp = 1; sw[0] = sw[1] = sw[2] = GL_RED; sw[3] = GL_ONE; return 1;    /* L8 */
    case 51: *ifmt = GL_RG8; *pf = GL_RG; *ty = GL_UNSIGNED_BYTE; *bpp = 2; sw[0] = sw[1] = sw[2] = GL_RED; sw[3] = GL_GREEN; return 1;  /* A8L8 */
    case 0x31545844: *ifmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; *bpp = 8; *compressed = 1; return 1;
    case 0x33545844: *ifmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; *bpp = 16; *compressed = 1; return 1;
    case 0x35545844: *ifmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; *bpp = 16; *compressed = 1; return 1;
    }
    return 0;
}
static uint32_t level_size(obj *t, int l, uint32_t *pitch, int *lw, int *lh) {
    GLenum a, b, c; int bpp, comp; GLint sw[4]; fmt_gl(t->fmt, &a, &b, &c, &bpp, &comp, sw);
    int w = t->w >> l, h = t->h >> l; if (w < 1) w = 1; if (h < 1) h = 1; *lw = w; *lh = h;
    if (comp) { int bw = (w + 3) / 4, bh = (h + 3) / 4; *pitch = (uint32_t)(bw * bpp); return (uint32_t)(bw * bh * bpp); }
    *pitch = (uint32_t)(w * bpp); return (uint32_t)(w * h * bpp);
}
static void tex_upload(obj *t, int l) {
    GLenum ifmt, pf, ty; int bpp, comp; GLint sw[4]; fmt_gl(t->fmt, &ifmt, &pf, &ty, &bpp, &comp, sw);
    uint32_t pitch; int w, h; uint32_t sz = level_size(t, l, &pitch, &w, &h);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (comp) glCompressedTexImage2D(GL_TEXTURE_2D, l, ifmt, w, h, 0, (GLsizei)sz, MEM + t->lockmem[l]);
    else glTexImage2D(GL_TEXTURE_2D, l, (GLint)ifmt, w, h, 0, pf, ty, MEM + t->lockmem[l]);
}
static obj *new_tex(int w, int h, int levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
    GLenum ifmt, pf, ty; int bpp, comp; GLint sw[4];
    if (!fmt_gl(fmt, &ifmt, &pf, &ty, &bpp, &comp, sw)) { sa_log("d3d9: unsupported texture format %u", fmt); return NULL; }
    obj *t = mk(O_TEX, vt_tex); t->w = w; t->h = h; t->fmt = fmt; t->usage = usage; t->pool = pool;
    if (levels == 0) { levels = 1; int m = w > h ? w : h; while (m > 1) { m >>= 1; levels++; } }
    if (usage & 1) levels = 1;
    t->levels = levels > 16 ? 16 : levels;
    cur(); glGenTextures(1, &t->tex); glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, sw);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
    for (int l = 0; l < t->levels; l++) {
        uint32_t pitch; int lw, lh; uint32_t sz = level_size(t, l, &pitch, &lw, &lh);
        if (comp) { void *z = calloc(1, sz); glCompressedTexImage2D(GL_TEXTURE_2D, l, ifmt, lw, lh, 0, (GLsizei)sz, z); free(z); }
        else glTexImage2D(GL_TEXTURE_2D, l, (GLint)ifmt, lw, lh, 0, pf, ty, NULL);
    }
    if (usage & 1) {   /* D3DUSAGE_RENDERTARGET */
        { extern obj *sa_playfield; if (w == 224 && h == 288) sa_playfield = t; }
        t->is_rt = 1; glGenFramebuffers(1, &t->fbo); glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    }
    return t;
}
static obj *surf_of(obj *t, int l) {
    obj *s = mk(O_SURF, vt_surf); s->parent = t; s->level = l; t->ref++;
    s->w = t->w >> l; s->h = t->h >> l; if (s->w < 1) s->w = 1; if (s->h < 1) s->h = 1; s->fmt = t->fmt; s->is_rt = t->is_rt;
    s->fbo = t->fbo; s->tex = t->tex; return s;
}
static GLuint fbo_of(obj *s) { return s ? (s->parent ? s->parent->fbo : s->fbo) : 0; }
static obj *tex_of(obj *s) { return s && s->parent ? s->parent : s; }

/* ================= shaders ================= */
static const char *VS_FF =
    "#version 330 core\nlayout(location=0) in vec4 aPos; layout(location=1) in vec4 aCol; layout(location=2) in vec2 aTex;\n"
    "uniform int uRHW; uniform vec4 uVP; uniform mat4 uWVP; out vec4 vD0; out vec4 vT0;\n"
    "void main(){ vD0=aCol.bgra; vT0=vec4(aTex,0,1);\n"
    " if(uRHW!=0){ float w = aPos.w!=0.0 ? 1.0/aPos.w : 1.0; vec2 p=(aPos.xy+0.5-uVP.xy)/uVP.zw*2.0-1.0; gl_Position=vec4(p.x*w,p.y*w,aPos.z*w,w); }\n"
    " else { gl_Position = vec4(aPos.xyz,1.0)*uWVP; gl_Position.y=-gl_Position.y; gl_Position.xy += gl_Position.w/uVP.zw; } }\n";
static const char *PS_FF =
    "#version 330 core\nin vec4 vD0; in vec4 vT0; uniform sampler2D s0; uniform int uTex; uniform int uAT; uniform float uAref; out vec4 o;\n"
    "void main(){ vec4 c = uTex!=0 ? texture(s0, vT0.xy)*vD0 : vD0;\n"
    " bool pass = uAT==8 || (uAT==2 && c.a<uAref) || (uAT==3 && c.a==uAref) || (uAT==4 && c.a<=uAref) || (uAT==5 && c.a>uAref) || (uAT==6 && c.a!=uAref) || (uAT==7 && c.a>=uAref);\n"
    " if(!pass) discard;\n"
    " o=c; }\n";
static GLuint compile(GLenum t, const char *src) {
    GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); sa_log("GLSL: %s\n%s", log, src); }
    return s;
}
static GLuint link2(GLuint a, GLuint b) {
    GLuint p = glCreateProgram(); glAttachShader(p, a); glAttachShader(p, b);
    glBindAttribLocation(p, 0, "aPos"); glBindAttribLocation(p, 1, "aCol"); glBindAttribLocation(p, 2, "aTex");
    glLinkProgram(p); GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); sa_log("GLSL link: %s", log); }
    return p;
}
static GLuint ff_vs, ff_ps;
#define MAXP 64
/* uniform locations cached per program (glGetUniformLocation costs a string lookup) */
enum { U_RHW, U_VP, U_WVP, U_VC, U_PC, U_S0, U_S1, U_TEX, U_AT, U_AREF, U_N };
static const char *U_NAMES[U_N] = { "uRHW", "uVP", "uWVP", "vc", "pc", "s0", "s1", "uTex", "uAT", "uAref" };
static struct { GLuint vs, ps, prog; GLint loc[U_N]; } progs[MAXP]; static int nprogs; static GLint *cur_loc;
static GLuint prog_for(GLuint vs, GLuint ps) {
    for (int i = 0; i < nprogs; i++) if (progs[i].vs == vs && progs[i].ps == ps) { cur_loc = progs[i].loc; return progs[i].prog; }
    GLuint p = link2(vs, ps);
    static GLint tmp[U_N]; GLint *loc = nprogs < MAXP ? progs[nprogs].loc : tmp;
    for (int k = 0; k < U_N; k++) loc[k] = glGetUniformLocation(p, U_NAMES[k]);
    if (nprogs < MAXP) { progs[nprogs].vs = vs; progs[nprogs].ps = ps; progs[nprogs].prog = p; nprogs++; }
    cur_loc = loc; return p;
}

/* ================= draw ================= */
static void apply_state(void) {
    obj *rt = rt0 ? rt0 : backbuf; GLuint fb = fbo_of(rt);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    /* "top to bottom" storage: row 0 (D3D top) is GL row 0; so viewport/scissor use Y as is, and NDC Y is flipped in the shaders */
    glViewport((GLint)vp.x, (GLint)vp.y, (GLsizei)vp.w, (GLsizei)vp.h);
    if (rs[174]) { glEnable(GL_SCISSOR_TEST); glScissor(scis.l, scis.t, scis.r - scis.l, scis.b - scis.t); } else glDisable(GL_SCISSOR_TEST);
    static const GLenum bl[] = { GL_ZERO, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
        GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR };
    static const GLenum bo[] = { GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX };
    if (rs[27]) {
        glEnable(GL_BLEND);
        GLenum s = bl[rs[19] & 15], d = bl[rs[20] & 15], op = bo[rs[171] <= 5 ? rs[171] : 1];
        if (rs[206]) glBlendFuncSeparate(s, d, bl[rs[207] & 15], bl[rs[208] & 15]); else glBlendFunc(s, d);
        glBlendEquation(op);
    } else glDisable(GL_BLEND);
    uint32_t cw = rs[168]; glColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    for (int s = 0; s < 8; s++) {
        glActiveTexture(GL_TEXTURE0 + s);
        obj *t = tex_bound[s];
        glBindTexture(GL_TEXTURE_2D, t ? t->tex : 0);
        if (t) {
            static const GLenum wrap[] = { GL_REPEAT, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER, GL_MIRROR_CLAMP_TO_EDGE };
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[samp[s][1] <= 5 ? samp[s][1] : 1]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[samp[s][2] <= 5 ? samp[s][2] : 1]);
            int lin_mag = samp[s][5] >= 2, lin_min = samp[s][6] >= 2, mip = samp[s][7];
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, lin_mag ? GL_LINEAR : GL_NEAREST);
            GLenum mn = lin_min ? GL_LINEAR : GL_NEAREST;
            if (mip && t->levels > 1) mn = lin_min ? (mip >= 2 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : (mip >= 2 ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mn);
        }
    }
    glActiveTexture(GL_TEXTURE0);
}
static void draw_up(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride) {
    if (!count) return;
    static int nodraw = -1; if (nodraw < 0) nodraw = getenv("SA_NODRAW") != NULL; if (nodraw) return;   /* measurement: cost of the game alone */
    cur(); apply_state();
    uint32_t nv; GLenum mode;
    switch (prim) {
    case 1: mode = GL_POINTS; nv = count; break; case 2: mode = GL_LINES; nv = count * 2; break; case 3: mode = GL_LINE_STRIP; nv = count + 1; break;
    case 4: mode = GL_TRIANGLES; nv = count * 3; break; case 5: mode = GL_TRIANGLE_STRIP; nv = count + 2; break; case 6: mode = GL_TRIANGLE_FAN; nv = count + 2; break;
    default: return;
    }
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * stride), MEM + data, GL_STREAM_DRAW);
    /* vertex format: FVF (the only one used by the game, with or without a vertex shader) */
    int rhw = (fvf & 0xE) == 4; uintptr_t off = rhw ? 16 : 12;
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, rhw ? 4 : 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void *)0);
    if (fvf & 0x40) { glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (void *)off); off += 4; }
    else { glDisableVertexAttribArray(1); glVertexAttrib4f(1, 1, 1, 1, 1); }
    if (fvf & 0x80) off += 4;
    if (((fvf >> 8) & 15) >= 1) { glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void *)off); }
    else { glDisableVertexAttribArray(2); glVertexAttrib4f(2, 0, 0, 0, 1); }
    GLuint vs = vs_cur && !rhw ? vs_cur->sh : ff_vs, ps = ps_cur ? ps_cur->sh : ff_ps;
    GLuint p = prog_for(vs, ps); glUseProgram(p);
    glUniform1i(cur_loc[U_RHW], rhw);
    glUniform4f(cur_loc[U_VP], (float)vp.x, (float)vp.y, (float)vp.w, (float)vp.h);
    float wvp[16]; { /* world * view * projection (fixed pipeline, rare here) */
        float *A = xf[256], *V = xf[2], *P = xf[3], t1[16];
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += A[i * 4 + k] * V[k * 4 + j]; t1[i * 4 + j] = s; }
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += t1[i * 4 + k] * P[k * 4 + j]; wvp[i * 4 + j] = s; } }
    glUniformMatrix4fv(cur_loc[U_WVP], 1, GL_FALSE, wvp);
    glUniform4fv(cur_loc[U_VC], 256, &vsc[0][0]);
    glUniform4fv(cur_loc[U_PC], 32, &psc[0][0]);
    glUniform1i(cur_loc[U_S0], 0); glUniform1i(cur_loc[U_S1], 1);
    glUniform1i(cur_loc[U_TEX], tex_bound[0] != NULL && tss[0][1] != 1);
    glUniform1i(cur_loc[U_AT], rs[15] ? (int)rs[25] : 8);
    glUniform1f(cur_loc[U_AREF], (float)(rs[24] & 255) / 255.0f);
    glDrawArrays(mode, 0, (GLsizei)nv);
}

/* ================= method declaration macro ================= */
#define M(name, n) static void name(void)
#define R(n, v) RET_STD(n, v)

/* ---- shared IUnknown ---- */
M(Q_QI, 3) { if (ARG(2)) wr32(ARG(2), ARG(0)); obj *o = me(); if (o) o->ref++; R(3, 0); }
M(Q_AddRef, 1) { obj *o = me(); R(1, o ? ++o->ref : 1); }
/* Textures are really freed when their count drops to 0: CPU copies (game heap), GL texture and FBO. Without this
 * every stage leaked its textures (2048x2048 / 4096x2048, 16-32 MB of game heap each) until "game heap exhausted"
 * around the end of stage 2. GL names are deleted later by the rendering thread (gl_gc in present). Surfaces and
 * other objects are small and are kept. */
static GLuint gc_names[2][256]; static int gc_n; static pthread_mutex_t gc_lk = PTHREAD_MUTEX_INITIALIZER;
static void gl_gc(void) {
    pthread_mutex_lock(&gc_lk);
    for (int i = 0; i < gc_n; i++) { if (gc_names[1][i]) glDeleteFramebuffers(1, &gc_names[1][i]); if (gc_names[0][i]) glDeleteTextures(1, &gc_names[0][i]); }
    gc_n = 0; pthread_mutex_unlock(&gc_lk);
}
static int tex_freed;
static void tex_free(obj *t) {
    extern obj *sa_playfield;
    if (t == sa_playfield || (backbuf && t == backbuf->parent)) return;   /* renderer's own targets: kept */
    for (int i = 0; i < 16; i++) if (t->lvsurf[i] && t->lvsurf[i]->ref > 0) return;   /* a level surface is still held */
    for (int i = 0; i < 16; i++) if (t->lockmem[i]) { gh_free(t->lockmem[i]); t->lockmem[i] = 0; }
    for (int i = 0; i < 8; i++) if (tex_bound[i] == t) tex_bound[i] = NULL;
    if (rt0 && tex_of(rt0) == t) rt0 = NULL;
    for (int i = 0; i < 16; i++) if (t->lvsurf[i]) { obj *s = t->lvsurf[i]; uint32_t k = rd32(s->g + 4); if (k < MAXO && O[k] == s) O[k] = NULL; wr32(s->g + 8, 0); free(s); }
    for (;;) {
        pthread_mutex_lock(&gc_lk);
        if (gc_n < 256) { gc_names[0][gc_n] = t->tex; gc_names[1][gc_n] = t->fbo; gc_n++; pthread_mutex_unlock(&gc_lk); break; }
        pthread_mutex_unlock(&gc_lk); SDL_Delay(1);   /* list full: wait for the next frame */
    }
    uint32_t k = rd32(t->g + 4); if (k < MAXO && O[k] == t) O[k] = NULL;
    tex_freed++;
    wr32(t->g + 8, 0);                                  /* stale pointers are recognised (me/og) and ignored */
    free(t);
}
M(Q_Release, 1) {
    obj *o = me(); if (!o) { R(1, 0); return; }
    int r = o->ref > 0 ? --o->ref : 0;
    if (r == 0 && o->type == O_TEX) tex_free(o);
    R(1, (uint32_t)r);
}
M(Q_GetDevice, 2) { if (ARG(1) && dev_o) { wr32(ARG(1), dev_o->g); dev_o->ref++; } R(2, 0); }
M(ok2, 2) { R(2, 0); } M(ok3, 3) { R(3, 0); } M(ok4, 4) { R(4, 0); } M(ok5, 5) { R(5, 0); } M(ok1, 1) { R(1, 0); }
M(ok6, 6) { R(6, 0); } M(ok7, 7) { R(7, 0); } M(ok9, 9) { R(9, 0); } M(ok10, 10) { R(10, 0); }
static void fail_n(int n) { RET_STD(n, 0x8876086Cu); }   /* D3DERR_INVALIDCALL */
M(fail3, 3) { fail_n(3); } M(fail4, 4) { fail_n(4); } M(fail5, 5) { fail_n(5); } M(fail7, 7) { fail_n(7); } M(fail8, 8) { fail_n(8); }

/* ================= IDirect3D9 ================= */
M(D_GetAdapterCount, 1) { R(1, 1); }
M(D_GetAdapterIdentifier, 4) { uint32_t p = ARG(3); memset(MEM + p, 0, 1100); strcpy((char *)MEM + p, "sa_gl"); strcpy((char *)MEM + p + 512, "OpenGL (Senjin Aleste native)"); wr32(p + 1064, 0x10DE); R(4, 0); }   /* VendorId (after DriverVersion at 1056) */
static const int modes[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 1024 }, { 1600, 900 }, { 1920, 1080 } };
M(D_GetAdapterModeCount, 3) { R(3, 7); }
M(D_EnumAdapterModes, 5) { uint32_t p = ARG(4), i = ARG(3) < 7 ? ARG(3) : 6; wr32(p, modes[i][0]); wr32(p + 4, modes[i][1]); wr32(p + 8, 60); wr32(p + 12, 22); R(5, 0); }
M(D_GetAdapterDisplayMode, 3) { uint32_t p = ARG(2); SDL_DisplayMode dm; if (SDL_GetDesktopDisplayMode(0, &dm)) { dm.w = 1920; dm.h = 1080; dm.refresh_rate = 60; }
    wr32(p, dm.w); wr32(p + 4, dm.h); wr32(p + 8, dm.refresh_rate ? dm.refresh_rate : 60); wr32(p + 12, 22); R(3, 0); }
M(D_CheckDeviceType, 6) { R(6, 0); }
M(D_CheckDeviceFormat, 7) { GLenum a, b, c; int bpp, comp; GLint sw[4]; uint32_t f = ARG(6);
    R(7, (fmt_gl(f, &a, &b, &c, &bpp, &comp, sw) || f == 75 || f == 77 || f == 80) ? 0 : 0x8876086Au); }
M(D_CheckDeviceMultiSampleType, 6) { R(6, ARG(4) <= 1 ? 0 : 0x8876086Au); }
M(D_CheckDepthStencilMatch, 6) { R(6, 0); }
static void fill_caps(uint32_t p) {
    memset(MEM + p, 0, 308);
    wr32(p, 1); for (int o = 8; o <= 84; o += 4) wr32(p + o, 0xFFFFFFFFu);
    wr32(p + 20, 0x8000000Fu); wr32(p + 88, 8192); wr32(p + 92, 8192); wr32(p + 96, 2048); wr32(p + 100, 8192); wr32(p + 104, 8192); wr32(p + 108, 16);
    float f = 1e10f; memcpy(MEM + p + 112, &f, 4); f = -1e8f; memcpy(MEM + p + 116, &f, 4); memcpy(MEM + p + 120, &f, 4); f = 1e8f; memcpy(MEM + p + 124, &f, 4); memcpy(MEM + p + 128, &f, 4);
    wr32(p + 136, 0xFF); wr32(p + 140, 0x00080008); wr32(p + 144, 0x03FFFFFF); wr32(p + 148, 8); wr32(p + 152, 8); wr32(p + 156, 0x13B);
    wr32(p + 160, 8); wr32(p + 164, 6); f = 64; memcpy(MEM + p + 176, &f, 4); wr32(p + 180, 0xFFFFF); wr32(p + 184, 0xFFFFFF); wr32(p + 188, 16); wr32(p + 192, 508);
    wr32(p + 196, 0xFFFE0300u); wr32(p + 200, 256); wr32(p + 204, 0xFFFF0300u); f = 65504; memcpy(MEM + p + 208, &f, 4);
    wr32(p + 212, 0x51); wr32(p + 236, 0x3FF); wr32(p + 240, 4); wr32(p + 244, 0x03000300);
    wr32(p + 292, 65535); wr32(p + 296, 65535); wr32(p + 300, 32768); wr32(p + 304, 32768);
}
M(D_GetDeviceCaps, 4) { fill_caps(ARG(3)); R(4, 0); }
M(D_GetAdapterMonitor, 2) { R(2, 0x60001); }
static void create_gl(void) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE); SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    ctx = SDL_GL_CreateContext(sa_window);
    if (!ctx) sa_fatal("OpenGL 3.3 unavailable: %s", SDL_GetError());
    ctx_thread = SDL_ThreadID();
    sa_log("OpenGL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    ff_vs = compile(GL_VERTEX_SHADER, VS_FF); ff_ps = compile(GL_FRAGMENT_SHADER, PS_FF);
}
static void reset_states(void) {
    memset(rs, 0, sizeof rs); rs[7] = 1; rs[14] = 1; rs[19] = 2; rs[20] = 1; rs[22] = 3; rs[25] = 8; rs[168] = 15; rs[171] = 1;
    for (int s = 0; s < 8; s++) { samp[s][1] = samp[s][2] = 1; samp[s][5] = samp[s][6] = 1; samp[s][7] = 0; tss[s][1] = s ? 1 : 4; }
    for (int i = 0; i < 512; i++) { memset(xf[i], 0, 64); xf[i][0] = xf[i][5] = xf[i][10] = xf[i][15] = 1; }
    vp.x = vp.y = 0; vp.w = (uint32_t)bb_w; vp.h = (uint32_t)bb_h; vp.mn = 0; vp.mx = 1;
}
static void make_backbuffer(void) {
    if (backbuf && backbuf->parent) {           /* Reset: reuse the existing backbuffer, resized if needed */
        obj *t = backbuf->parent;
        if (t->w != bb_w || t->h != bb_h) {
            cur(); glBindTexture(GL_TEXTURE_2D, t->tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bb_w, bb_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
            t->w = backbuf->w = bb_w; t->h = backbuf->h = bb_h;
            if (t->lockmem[0]) { gh_free(t->lockmem[0]); t->lockmem[0] = 0; }   /* CPU copy had the old size */
            glBindFramebuffer(GL_FRAMEBUFFER, t->fbo); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        }
        backbuf->ref = 1; rt0 = backbuf; return;
    }
    obj *t = new_tex(bb_w, bb_h, 1, 1, 21, 0); backbuf = surf_of(t, 0); backbuf->ref = 1;
    rt0 = backbuf;
}
static int pace = 1;
M(D_CreateDevice, 7) {
    uint32_t pp = ARG(5), out = ARG(6);
    bb_w = (int)rd32(pp); bb_h = (int)rd32(pp + 4); if (bb_w <= 0) bb_w = 1920; if (bb_h <= 0) bb_h = 1080;
    sa_log("d3d9: CreateDevice %dx%d windowed=%u interval=0x%x", bb_w, bb_h, rd32(pp + 32), rd32(pp + 52));
    if (!ctx) create_gl();
    pace = rd32(pp + 52) != 0x80000000u;
    SDL_GL_SetSwapInterval(pace ? 1 : 0);
    dev_o = mk(O_DEV, vt_dev); sc_o = mk(O_SC, vt_sc);
    reset_states(); make_backbuffer();
    autodepth = mk(O_SURF, vt_surf); autodepth->w = bb_w; autodepth->h = bb_h; autodepth->fmt = 75;
    wr32(out, dev_o->g); R(7, 0);
}

/* ================= IDirect3DDevice9 ================= */
M(V_TestCooperativeLevel, 1) { R(1, 0); }
M(V_GetAvailableTextureMem, 1) { R(1, 1024u << 20); }
M(V_GetDirect3D, 2) { R(2, 0); }
M(V_GetDeviceCaps, 2) { fill_caps(ARG(1)); R(2, 0); }
M(V_GetDisplayMode, 3) { uint32_t p = ARG(2); wr32(p, bb_w); wr32(p + 4, bb_h); wr32(p + 8, 60); wr32(p + 12, 22); R(3, 0); }
M(V_GetCreationParameters, 2) { uint32_t p = ARG(1); wr32(p, 0); wr32(p + 4, 1); wr32(p + 8, 0x00010000u); wr32(p + 12, 0x40); R(2, 0); }
M(V_ShowCursor, 2) { R(2, 0); }
M(V_GetSwapChain, 3) { wr32(ARG(2), sc_o->g); sc_o->ref++; R(3, 0); }
M(V_GetNumberOfSwapChains, 1) { R(1, 1); }
M(V_Reset, 2) {
    uint32_t pp = ARG(1); bb_w = (int)rd32(pp); bb_h = (int)rd32(pp + 4); if (bb_w <= 0) bb_w = 1920; if (bb_h <= 0) bb_h = 1080;
    sa_log("d3d9: Reset %dx%d", bb_w, bb_h); cur(); reset_states(); make_backbuffer(); R(2, 0);
}
/* 60 Hz cabinet pacing: vertical sync is not guaranteed (WSLg, 120/144 Hz displays) */
static void limit_fps(void) {
    static double next; static int target = -1;
    if (target < 0) { const char *e = getenv("SA_FPS"); target = e ? atoi(e) : 60; }
    if (!pace || target <= 0) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double now = t.tv_sec + t.tv_nsec * 1e-9, step = 1.0 / target;
    if (next == 0 || now > next + 0.1 || now < next - 0.1) next = now;
    next += step;
    if (next > now) { double d = next - now; struct timespec w = { (time_t)d, (long)((d - (time_t)d) * 1e9) }; nanosleep(&w, NULL); }
}
/* ---------- final display: playfield only ([video] nobezel=1) and the kit's CRT filter ([video] crt=0..3) ----------
 * The CRT is the kit's one (_kit/crt/kit_crt.c, taken from Soul Calibur) translated to GLSL: same constants, same
 * formulas (horizontal interpolation, nearest line, scanlines, RGB triads, vignette), computed by the graphics
 * card instead of the CPU. Source: the 224x288 playfield (nobezel) or the whole image scaled back to its
 * "1x" size (size / DISP_SCALE, like the Windows DLL). F9: next CRT, F8: bezel on/off (saved). */
obj *sa_playfield;
/* [video] latency: frames the graphics driver may queue after Present. 0 = driver default (often 2-3),
 * 1 = at most one (wait for the previous frame's fence, almost free), 2 = none (glFinish: lowest input lag, costs GPU time). */
static int v_lat = 1;
static int v_crt = -1, v_nobezel, v_scx = 200, v_scy = 200, v_fs, v_fs_applied = -1;
/* TATE ([video] tate=1, F7): playfield only, turned 90 degrees for a monitor standing on its side, scaled to fill.
 * [video] tate_dir: 1 = monitor turned clockwise (image turned counter-clockwise), 2 = monitor turned counter-clockwise.
 * The CRT filter is applied after the rotation, so scanlines follow the monitor's own lines, as on a real cabinet. */
static int v_tate, v_tate_dir = 1;
static obj rot_o;                                   /* rotated playfield, 288x224, 1x grid */
static GLuint rot_prog;
static const char *ROT_PS =
    "#version 330 core\nuniform sampler2D uSrc; uniform ivec2 uSz; uniform int uDir; out vec4 o;\n"
    "void main(){ ivec2 d=ivec2(gl_FragCoord.xy); ivec2 p = uDir==2 ? ivec2(d.y, uSz.y-1-d.x) : ivec2(uSz.x-1-d.y, d.x); o=texelFetch(uSrc,p,0); }\n";
static GLuint crt_prog;
static const char *CRT_VS = "#version 330 core\nconst vec2 P[4]=vec2[4](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(1,1));\nvoid main(){ gl_Position=vec4(P[gl_VertexID],0,1); }\n";
static const char *CRT_PS =
    "#version 330 core\nuniform sampler2D uSrc; uniform ivec2 uTex; uniform ivec2 uGrid; uniform ivec4 uDst; uniform int uLvl; out vec4 o;\n"
    "const float SCAN[4]=float[4](0.0,0.40,0.60,0.80), MASK[4]=float[4](0.0,0.0,0.22,0.38), VIG[4]=float[4](0.0,0.08,0.16,0.28);\n"
    "vec3 px(int x,int y){ ivec2 t=ivec2((2*x+1)*uTex.x/(2*uGrid.x),(2*y+1)*uTex.y/(2*uGrid.y)); return texelFetch(uSrc,t,0).rgb; }\n"
    "void main(){\n"
    " int w=uGrid.x,h=uGrid.y,dw=uDst.z,dh=uDst.w; int x=int(gl_FragCoord.x)-uDst.x; int y=(uDst.y+dh-1)-int(gl_FragCoord.y);\n"
    " if(x<0||y<0||x>=dw||y>=dh){ o=vec4(0,0,0,1); return; }\n"
    " float sc=SCAN[uLvl],mk=MASK[uLvl],vg=VIG[uLvl]; float boost=1.0/(1.0-sc*0.33), mboost=1.0/(1.0-mk*0.33);\n"
    " float fx=((float(x)+0.5)*float(w)/float(dw))-0.5; int x0=int(floor(fx)); float fr=fx-float(x0); int x1=x0+1; x0=clamp(x0,0,w-1); x1=clamp(x1,0,w-1);\n"
    " int wx=int(fr*256.0);\n"
    " float cx=((float(x)+0.5)/float(dw))*2.0-1.0; float vx=max(1.0-vg*cx*cx*cx*cx*2.0,0.0);\n"
    " float fy=(float(y)+0.5)*float(h)/float(dh); int sy=min(int(floor(fy)),h-1); float fry=fy-float(sy); float d=abs(fry-0.5)*2.0;\n"
    " float f=(1.0-sc*d*d)*boost; float cy=((float(y)+0.5)/float(dh))*2.0-1.0; float vy=max(1.0-vg*cy*cy*cy*cy*2.0,0.0); int yf=int(f*vy*256.0);\n"
    " ivec3 a=ivec3(px(x0,sy)*255.0+0.5), b=ivec3(px(x1,sy)*255.0+0.5); ivec3 c=(a*(256-wx)+b*wx)>>8;\n"
    " ivec3 m; for(int k=0;k<3;k++){ float mm=1.0; if(mk>0.0){ mm=((x%3)==k)?1.0:(1.0-mk); mm*=mboost; } m[k]=int(min(mm*vx,1.6)*256.0); }\n"
    " c=min(((c*yf)>>8)*m>>8,ivec3(255)); o=vec4(vec3(c)/255.0,1.0); }\n";
static void video_settings(void) {
    v_crt = 0; FILE *f = fopen("aleste.ini", "rb"); char l[256]; int sec = 0;
    if (f) { while (fgets(l, sizeof l, f)) { if (l[0] == '[') sec = !strncmp(l, "[video]", 7);
        else if (sec && !strncmp(l, "crt=", 4)) v_crt = atoi(l + 4) & 3; else if (sec && !strncmp(l, "nobezel=", 8)) v_nobezel = atoi(l + 8) != 0;
        else if (sec && !strncmp(l, "tate=", 5)) v_tate = atoi(l + 5) != 0; else if (sec && !strncmp(l, "latency=", 8)) { v_lat = atoi(l + 8); if (v_lat < 0 || v_lat > 2) v_lat = 1; } else if (sec && !strncmp(l, "tate_dir=", 9)) v_tate_dir = atoi(l + 9) == 2 ? 2 : 1; } fclose(f); }
    f = fopen("config.txt", "rb");
    if (f) { while (fgets(l, sizeof l, f)) { if (!strncmp(l, "DISP_SCALE_X=", 13)) v_scx = atoi(l + 13); if (!strncmp(l, "DISP_SCALE_Y=", 13)) v_scy = atoi(l + 13); if (!strncmp(l, "FULLSCREEN=", 11)) v_fs = atoi(l + 11) != 0; } fclose(f); }
    if (v_scx < 100) v_scx = 100; if (v_scy < 100) v_scy = 100;
    sa_log("display: crt=%d bezel=%s tate=%d dir=%d latency=%d (F9: CRT, F8: bezel, F7: TATE)", v_crt, v_nobezel ? "off" : "on", v_tate, v_tate_dir, v_lat);
}
/* writes a key of section sec to aleste.ini (adds the section if needed) */
static void ini_set(const char *sec_name, const char *key, int val) {
    char hdr[64]; snprintf(hdr, sizeof hdr, "[%s]", sec_name); size_t hl = strlen(hdr);
    FILE *f = fopen("aleste.ini", "rb"); char *buf = NULL; long n = 0;
    if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); buf = malloc((size_t)n + 1); if (fread(buf, 1, (size_t)n, f) != (size_t)n) {} buf[n] = 0; fclose(f); }
    char *out = malloc((size_t)n + 256); size_t o = 0; int sec = 0, done = 0; size_t kl = strlen(key); const char *p = buf ? buf : "";
    while (*p) {
        const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p + 1) : strlen(p);
        if (*p == '[') { if (sec && !done) { o += (size_t)sprintf(out + o, "%s=%d\r\n", key, val); done = 1; } sec = !strncmp(p, hdr, hl); }
        if (sec && !strncmp(p, key, kl) && p[kl] == '=') { if (!done) o += (size_t)sprintf(out + o, "%s=%d\r\n", key, val); done = 1; }
        else { memcpy(out + o, p, len); o += len; }
        p += len;
    }
    if (!done) { if (!sec) o += (size_t)sprintf(out + o, "%s\r\n", hdr); o += (size_t)sprintf(out + o, "%s=%d\r\n", key, val); }
    f = fopen("aleste.ini", "wb"); if (f) { fwrite(out, 1, o, f); fclose(f); }
    free(out); free(buf);
}
static GLuint dim_prog;
static const char *DIM_PS = "#version 330 core\nout vec4 o; void main(){ o=vec4(0.0,0.0,0.0,0.55); }\n";
static void draw_frame(int dim);
static void pause_loop(void);
static void present(void) {
    cur(); gl_gc();
    if (v_crt < 0) video_settings();
    { extern uint8_t sa_keys[256]; static uint8_t k9, k8;
      if (sa_keys[0x78] && !k9) { v_crt = (v_crt + 1) & 3; ini_set("video", "crt", v_crt); sa_log("F9: crt=%d", v_crt); } k9 = sa_keys[0x78];
      if (sa_keys[0x77] && !k8) { v_nobezel = !v_nobezel; ini_set("video", "nobezel", v_nobezel); sa_log("F8: bezel %s", v_nobezel ? "off" : "on"); } k8 = sa_keys[0x77];
      static uint8_t k7; if (sa_keys[0x76] && !k7) { v_tate = !v_tate; ini_set("video", "tate", v_tate); sa_log("F7: TATE %s", v_tate ? "on" : "off"); } k7 = sa_keys[0x76];
      /* F11 or Alt+Enter: fullscreen (borderless, desktop resolution) <-> window; saved as the game's FULLSCREEN setting */
      static uint8_t kfs; int fsk = sa_keys[0x7A] || (sa_keys[0x12] && sa_keys[0x0D]);
      if (fsk && !kfs) { v_fs = !v_fs; ini_set("config", "FULLSCREEN", v_fs); sa_log("%s: %s", sa_keys[0x7A] ? "F11" : "Alt+Enter", v_fs ? "fullscreen" : "window"); } kfs = (uint8_t)fsk; }
    if (v_fs != v_fs_applied) { SDL_SetWindowFullscreen(sa_window, v_fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); v_fs_applied = v_fs; }
    { extern int sa_start_hidden(void); if (sa_start_hidden()) return; }   /* Start -> KEY CONFIG transition (#5) */
    draw_frame(0);
    static int perf = -1; static double t_end, acc_cpu, acc_swap; static int pn;
    if (perf < 0) perf = getenv("SA_PERF") != NULL;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0); double a = t0.tv_sec + t0.tv_nsec * 1e-9;
    SDL_GL_SwapWindow(sa_window);
    { static GLsync prev;                           /* frames in flight ([video] latency) */
      if (v_lat == 2) glFinish();
      else if (v_lat == 1) { if (prev) { glClientWaitSync(prev, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000); glDeleteSync(prev); }
                             prev = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); }
      else if (prev) { glDeleteSync(prev); prev = 0; } }
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1); double b = t1.tv_sec + t1.tv_nsec * 1e-9;
    if (perf && t_end > 0) { acc_cpu += a - t_end; acc_swap += b - a; if (++pn == 300) { sa_log("perf: game + render %.2f ms/frame, swap %.2f ms/frame", acc_cpu * 1000 / pn, acc_swap * 1000 / pn); acc_cpu = acc_swap = 0; pn = 0; } }
    limit_fps();
    { static int was = 1; int d = sa_pause_button(); if (d && !was) { pause_loop(); d = 1; } was = d; }
    { struct timespec t2; clock_gettime(CLOCK_MONOTONIC, &t2); t_end = t2.tv_sec + t2.tv_nsec * 1e-9; }
    static unsigned n;
    if (++n == 1 || n % 600 == 0) {
        unsigned top, fs, fb; g_heap_stats(&top, &fs, &fb); int live = 0; for (int i = 1; i < MAXO; i++) if (O[i] && O[i]->type == O_TEX) live++;
        sa_log("d3d9: frame %u presented (game heap %u MB used of %u, free %u+%u MB; textures %d live, %d freed)", n, top, (G_HEAP_HI - G_HEAP_LO) >> 20, fs, fb, live, tex_freed);
    }
}
/* Pause: the game's main thread waits here (its clocks and the sound stand still, see sa_misc.c); the last image
 * is shown dimmed. Same button again (or Esc, which then quits the game) to resume. */
static void pause_loop(void) {
    sa_pause_set(1);
    char title[256]; snprintf(title, sizeof title, "%s", SDL_GetWindowTitle(sa_window)); char t2[300]; snprintf(t2, sizeof t2, "%s - PAUSE", title); SDL_SetWindowTitle(sa_window, t2);
    extern uint8_t sa_keys[256]; extern int sa_pump_events(void);
    int was = 1;
    for (;;) {
        if (sa_pump_events()) break;                       /* window closed: let the game handle it */
        int d = sa_pause_button(); if (d && !was) break; was = d;
        if (sa_keys[0x1B]) break;                          /* Esc: resume, the game then sees Esc and quits */
        draw_frame(1); SDL_GL_SwapWindow(sa_window); SDL_Delay(16);
    }
    SDL_SetWindowTitle(sa_window, title);
    sa_pause_set(0);
}
static void draw_frame(int dim) {
    int ww, wh; SDL_GL_GetDrawableSize(sa_window, &ww, &wh);
    obj *src = (v_nobezel && sa_playfield) ? sa_playfield : backbuf->parent;
    if (v_tate && sa_playfield) {   /* TATE: turn the playfield into rot_o (288x224), then display it like the playfield */
        obj *p = sa_playfield;
        if (!rot_o.tex) {
            glGenTextures(1, &rot_o.tex); glBindTexture(GL_TEXTURE_2D, rot_o.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p->h, p->w, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glGenFramebuffers(1, &rot_o.fbo); glBindFramebuffer(GL_FRAMEBUFFER, rot_o.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rot_o.tex, 0);
            rot_o.w = p->h; rot_o.h = p->w;
        }
        if (!rot_prog) rot_prog = link2(compile(GL_VERTEX_SHADER, CRT_VS), compile(GL_FRAGMENT_SHADER, ROT_PS));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rot_o.fbo); glViewport(0, 0, rot_o.w, rot_o.h);
        glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glColorMask(1, 1, 1, 1);
        glUseProgram(rot_prog); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, p->tex);
        glUniform1i(glGetUniformLocation(rot_prog, "uSrc"), 0); glUniform2i(glGetUniformLocation(rot_prog, "uSz"), p->w, p->h);
        glUniform1i(glGetUniformLocation(rot_prog, "uDir"), v_tate_dir);
        glBindVertexArray(vao); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glUseProgram(0);
        src = &rot_o;
    }
    int pix = src == sa_playfield || src == &rot_o;                 /* 1x pixel sources (playfield, TATE) */
    int sw = src->w, sh = src->h;                                   /* size of the source texture */
    int gw = sw, gh = sh;                                            /* "1x" grid for the CRT */
    if (!pix) { gw = sw * 100 / v_scx; gh = sh * 100 / v_scy; }
    /* fit to the window keeping the aspect ratio (ratio of the 1x grid) */
    float s = (float)ww / gw < (float)wh / gh ? (float)ww / gw : (float)wh / gh;
    /* playfield only / TATE: fill the screen height (or width), proportional; sharp pixels below ("sharp bilinear") */
    int dw = (int)(gw * s), dh = (int)(gh * s), dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); glViewport(0, 0, ww, wh);
    glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (v_crt > 0 && dw >= gw && dh >= gh) {
        if (!crt_prog) crt_prog = link2(compile(GL_VERTEX_SHADER, CRT_VS), compile(GL_FRAGMENT_SHADER, CRT_PS));
        glUseProgram(crt_prog); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, src->tex);
        glUniform1i(glGetUniformLocation(crt_prog, "uSrc"), 0);
        glUniform2i(glGetUniformLocation(crt_prog, "uTex"), sw, sh); glUniform2i(glGetUniformLocation(crt_prog, "uGrid"), gw, gh);
        glUniform4i(glGetUniformLocation(crt_prog, "uDst"), dx, dy, dw, dh); glUniform1i(glGetUniformLocation(crt_prog, "uLvl"), v_crt);
        glBindVertexArray(vao); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glUseProgram(0);                                             /* apply_state() reapplies everything on each draw */
    } else {
        GLuint rfb = src->fbo; int rw = sw, rh = sh; GLenum flt = pix ? GL_NEAREST : GL_LINEAR;
        if (pix && (dw % sw || dh % sh)) {   /* non-integer scale: nearest to the next integer multiple, then linear down */
            static obj up_o; int k = (int)s + 1; if (k > 8) k = 8;
            if (up_o.w != sw * k || up_o.h != sh * k) {
                if (!up_o.tex) { glGenTextures(1, &up_o.tex); glGenFramebuffers(1, &up_o.fbo); }
                glBindTexture(GL_TEXTURE_2D, up_o.tex); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw * k, sh * k, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                glBindFramebuffer(GL_FRAMEBUFFER, up_o.fbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, up_o.tex, 0);
                up_o.w = sw * k; up_o.h = sh * k;
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, up_o.fbo);
            glBlitFramebuffer(0, 0, sw, sh, 0, 0, up_o.w, up_o.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            rfb = up_o.fbo; rw = up_o.w; rh = up_o.h; flt = GL_LINEAR;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
        /* vertical flip (top-to-bottom storage) */
        glBlitFramebuffer(0, 0, rw, rh, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, flt);
    }
    if (dim) {   /* pause: darken the image */
        if (!dim_prog) dim_prog = link2(compile(GL_VERTEX_SHADER, CRT_VS), compile(GL_FRAGMENT_SHADER, DIM_PS));
        glUseProgram(dim_prog); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glBlendEquation(GL_FUNC_ADD);
        glBindVertexArray(vao); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND); glUseProgram(0);
    }
    { extern void sa_capture_check(void); sa_capture_check(); }
}
M(V_Present, 5) { present(); R(5, 0); }
M(V_GetBackBuffer, 5) { wr32(ARG(4), backbuf->g); backbuf->ref++; R(5, 0); }
M(V_GetRasterStatus, 3) { memset(MEM + ARG(2), 0, 8); R(3, 0); }
M(V_CreateTexture, 9) {
    obj *t = new_tex((int)ARG(1), (int)ARG(2), (int)ARG(3), ARG(4), ARG(5), ARG(6));
    if (!t) { R(9, 0x8876086Cu); return; }
    wr32(ARG(7), t->g); R(9, 0);
}
M(V_CreateVertexBuffer, 7) { obj *b = mk(O_VB, vt_vb); b->size = ARG(1); b->vbmem = g_alloc(ARG(1), 1); b->fmt = ARG(3); wr32(ARG(5), b->g); R(7, 0); }
M(V_CreateRenderTarget, 9) { obj *t = new_tex((int)ARG(1), (int)ARG(2), 1, 1, ARG(3), 0); if (!t) { R(9, 0x8876086Cu); return; } obj *s = surf_of(t, 0); wr32(ARG(7), s->g); R(9, 0); }
M(V_CreateDepthStencilSurface, 9) { obj *s = mk(O_SURF, vt_surf); s->w = (int)ARG(1); s->h = (int)ARG(2); s->fmt = ARG(3); wr32(ARG(7), s->g); R(9, 0); }
M(V_CreateOffscreenPlainSurface, 7) {
    obj *s = mk(O_SURF, vt_surf); s->w = (int)ARG(1); s->h = (int)ARG(2); s->fmt = ARG(3); s->pool = ARG(4);
    s->vbmem = g_alloc((uint32_t)(s->w * s->h * 4), 1); wr32(ARG(5), s->g); R(7, 0);
}
static void blit(obj *src, const int *sr, obj *dst, const int *dr, int lin) {
    cur();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_of(src)); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_of(dst));
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(sr[0], sr[1], sr[2], sr[3], dr[0], dr[1], dr[2], dr[3], GL_COLOR_BUFFER_BIT, lin ? GL_LINEAR : GL_NEAREST);
}
M(V_StretchRect, 6) {
    obj *s = og(ARG(1)), *d = og(ARG(3)); if (!s || !d || !fbo_of(s) || !fbo_of(d)) { R(6, 0); return; }
    int sr[4] = { 0, 0, s->w, s->h }, dr[4] = { 0, 0, d->w, d->h };
    if (ARG(2)) { sr[0] = (int)rd32(ARG(2)); sr[1] = (int)rd32(ARG(2) + 4); sr[2] = (int)rd32(ARG(2) + 8); sr[3] = (int)rd32(ARG(2) + 12); }
    if (ARG(4)) { dr[0] = (int)rd32(ARG(4)); dr[1] = (int)rd32(ARG(4) + 4); dr[2] = (int)rd32(ARG(4) + 8); dr[3] = (int)rd32(ARG(4) + 12); }
    blit(s, sr, d, dr, ARG(5) == 2); R(6, 0);
}
M(V_GetRenderTargetData, 3) {
    obj *s = og(ARG(1)), *d = og(ARG(2)); if (!s || !d) { R(3, 0x8876086Cu); return; }
    if (!d->vbmem) d->vbmem = g_alloc((uint32_t)(d->w * d->h * 4), 1);
    cur(); glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_of(s)); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, d->w, d->h, GL_BGRA, GL_UNSIGNED_BYTE, MEM + d->vbmem); R(3, 0);
}
M(V_UpdateSurface, 5) { R(5, 0); }
M(V_ColorFill, 4) {
    obj *s = og(ARG(1)); uint32_t c = ARG(3); if (!s || !fbo_of(s)) { R(4, 0); return; }
    cur(); glBindFramebuffer(GL_FRAMEBUFFER, fbo_of(s)); glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1);
    glClearColor(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f); glClear(GL_COLOR_BUFFER_BIT); R(4, 0);
}
M(V_SetRenderTarget, 3) {
    if (ARG(1) == 0) { obj *s = og(ARG(2)); if (s) { rt0 = s; vp.x = vp.y = 0; vp.w = (uint32_t)s->w; vp.h = (uint32_t)s->h; scis.l = scis.t = 0; scis.r = s->w; scis.b = s->h; } }
    R(3, 0);
}
M(V_GetRenderTarget, 3) { obj *s = ARG(1) == 0 ? rt0 : NULL; if (s) { wr32(ARG(2), s->g); s->ref++; R(3, 0); } else { wr32(ARG(2), 0); R(3, 0x8876086Cu); } }
M(V_SetDepthStencilSurface, 2) { R(2, 0); }
M(V_GetDepthStencilSurface, 2) { wr32(ARG(1), autodepth->g); autodepth->ref++; R(2, 0); }
M(V_BeginScene, 1) { in_scene = 1; R(1, 0); }
M(V_EndScene, 1) { in_scene = 0; R(1, 0); }
M(V_Clear, 7) {
    uint32_t fl = ARG(3), c = ARG(4);
    if (fl & 1) {
        cur(); apply_state(); glEnable(GL_SCISSOR_TEST); glScissor((GLint)vp.x, (GLint)vp.y, (GLsizei)vp.w, (GLsizei)vp.h); glColorMask(1, 1, 1, 1);
        glClearColor(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f); glClear(GL_COLOR_BUFFER_BIT);
    }
    R(7, 0);
}
M(V_SetTransform, 3) { uint32_t t = ARG(1); if (t < 512) memcpy(xf[t], MEM + ARG(2), 64); R(3, 0); }
M(V_GetTransform, 3) { uint32_t t = ARG(1); if (t < 512) memcpy(MEM + ARG(2), xf[t], 64); R(3, 0); }
M(V_SetViewport, 2) { uint32_t p = ARG(1); vp.x = rd32(p); vp.y = rd32(p + 4); vp.w = rd32(p + 8); vp.h = rd32(p + 12); memcpy(&vp.mn, MEM + p + 16, 4); memcpy(&vp.mx, MEM + p + 20, 4); R(2, 0); }
M(V_GetViewport, 2) { uint32_t p = ARG(1); wr32(p, vp.x); wr32(p + 4, vp.y); wr32(p + 8, vp.w); wr32(p + 12, vp.h); memcpy(MEM + p + 16, &vp.mn, 4); memcpy(MEM + p + 20, &vp.mx, 4); R(2, 0); }
M(V_SetRenderState, 3) { if (ARG(1) < 256) rs[ARG(1)] = ARG(2); R(3, 0); }
M(V_GetRenderState, 3) { wr32(ARG(2), ARG(1) < 256 ? rs[ARG(1)] : 0); R(3, 0); }
M(V_GetTexture, 3) { obj *t = ARG(1) < 8 ? tex_bound[ARG(1)] : NULL; wr32(ARG(2), t ? t->g : 0); if (t) t->ref++; R(3, 0); }
M(V_SetTexture, 3) { if (ARG(1) < 8) tex_bound[ARG(1)] = og(ARG(2)); R(3, 0); }
M(V_GetTextureStageState, 4) { wr32(ARG(3), ARG(1) < 8 && ARG(2) < 33 ? tss[ARG(1)][ARG(2)] : 0); R(4, 0); }
M(V_SetTextureStageState, 4) { if (ARG(1) < 8 && ARG(2) < 33) tss[ARG(1)][ARG(2)] = ARG(3); R(4, 0); }
M(V_GetSamplerState, 4) { wr32(ARG(3), ARG(1) < 8 && ARG(2) < 14 ? samp[ARG(1)][ARG(2)] : 0); R(4, 0); }
M(V_SetSamplerState, 4) { if (ARG(1) < 8 && ARG(2) < 14) samp[ARG(1)][ARG(2)] = ARG(3); R(4, 0); }
M(V_ValidateDevice, 2) { if (ARG(1)) wr32(ARG(1), 1); R(2, 0); }
M(V_SetScissorRect, 2) { uint32_t p = ARG(1); scis.l = (int32_t)rd32(p); scis.t = (int32_t)rd32(p + 4); scis.r = (int32_t)rd32(p + 8); scis.b = (int32_t)rd32(p + 12); R(2, 0); }
M(V_GetScissorRect, 2) { uint32_t p = ARG(1); wr32(p, (uint32_t)scis.l); wr32(p + 4, (uint32_t)scis.t); wr32(p + 8, (uint32_t)scis.r); wr32(p + 12, (uint32_t)scis.b); R(2, 0); }
M(V_GetSoftwareVertexProcessing, 1) { R(1, 0); }
M(V_DrawPrimitiveUP, 5) { draw_up(ARG(1), ARG(2), ARG(3), ARG(4)); R(5, 0); }
static uint32_t stream_vb, stream_stride, stream_off;
M(V_SetStreamSource, 5) { obj *b = og(ARG(2)); stream_vb = b ? b->vbmem : 0; stream_off = ARG(3); stream_stride = ARG(4); R(5, 0); }
M(V_DrawPrimitive, 4) { if (stream_vb) draw_up(ARG(1), ARG(3), stream_vb + stream_off + ARG(2) * stream_stride, stream_stride); R(4, 0); }
M(V_SetFVF, 2) { fvf = ARG(1); R(2, 0); }
M(V_GetFVF, 2) { wr32(ARG(1), fvf); R(2, 0); }
static void make_shader(int ps) {
    uint32_t fn = ARG(1); obj *o = mk(ps ? O_PS : O_VS, ps ? vt_ps : vt_vs);
    int n = 1; while (n < 65536 && rd32(fn + 4 * n) != 0x0000FFFF) n++; n++;
    o->code = malloc(4 * n); memcpy(o->code, MEM + fn, 4 * n); o->ncode = n;
    int isps = 0; char *src = sm_to_glsl(o->code, &isps);
    cur(); o->sh = compile(isps ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, src); free(src);
    wr32(ARG(2), o->g);
}
M(V_CreateVertexShader, 3) { make_shader(0); R(3, 0); }
M(V_CreatePixelShader, 3) { make_shader(1); R(3, 0); }
M(V_SetVertexShader, 2) { vs_cur = og(ARG(1)); R(2, 0); }
M(V_GetVertexShader, 2) { wr32(ARG(1), vs_cur ? vs_cur->g : 0); R(2, 0); }
M(V_SetPixelShader, 2) { ps_cur = og(ARG(1)); R(2, 0); }
M(V_GetPixelShader, 2) { wr32(ARG(1), ps_cur ? ps_cur->g : 0); R(2, 0); }
M(V_SetVertexShaderConstantF, 4) { uint32_t r = ARG(1), n = ARG(3); if (r + n <= 256) memcpy(vsc[r], MEM + ARG(2), n * 16); R(4, 0); }
M(V_GetVertexShaderConstantF, 4) { uint32_t r = ARG(1), n = ARG(3); if (r + n <= 256) memcpy(MEM + ARG(2), vsc[r], n * 16); R(4, 0); }
M(V_SetPixelShaderConstantF, 4) { uint32_t r = ARG(1), n = ARG(3); if (r + n <= 32) memcpy(psc[r], MEM + ARG(2), n * 16); R(4, 0); }
M(V_GetPixelShaderConstantF, 4) { uint32_t r = ARG(1), n = ARG(3); if (r + n <= 32) memcpy(MEM + ARG(2), psc[r], n * 16); R(4, 0); }
M(V_CreateQuery, 3) { if (ARG(2)) { obj *q = mk(O_QUERY, vt_query); wr32(ARG(2), q->g); } R(3, 0); }
M(V_CreateStateBlock, 3) { R(3, 0x8876086Cu); }

/* ================= IDirect3DTexture9 / Surface9 ================= */
M(T_GetLevelCount, 1) { obj *t = me(); R(1, t ? (uint32_t)t->levels : 1); }
M(T_GetType, 1) { R(1, 3); }
M(T_GetLevelDesc, 3) { obj *t = me(); uint32_t p = ARG(2); int l = (int)ARG(1); int w = t->w >> l, h = t->h >> l; if (w < 1) w = 1; if (h < 1) h = 1;
    wr32(p, t->fmt); wr32(p + 4, 1); wr32(p + 8, t->usage); wr32(p + 12, t->pool); wr32(p + 16, 0); wr32(p + 20, 0); wr32(p + 24, (uint32_t)w); wr32(p + 28, (uint32_t)h); R(3, 0); }
M(T_GetSurfaceLevel, 3) {
    obj *t = me(); uint32_t l = ARG(1);
    if (!t || l >= 16 || (int)l >= t->levels) { wr32(ARG(2), 0); R(3, 0x8876086Cu); return; }   /* D3DERR_INVALIDCALL */
    /* one surface object per (texture, level), returned again with an extra reference */
    obj *s = t->lvsurf[l];
    if (s) s->ref++; else s = t->lvsurf[l] = surf_of(t, (int)l);
    wr32(ARG(2), s->g); R(3, 0);
}
static int lock_level(obj *t, int l, uint32_t lr, uint32_t rect) {
    if (l < 0 || l >= 16 || l >= t->levels) return 0;   /* lockmem[] has 16 entries */
    uint32_t pitch; int w, h; uint32_t sz = level_size(t, l, &pitch, &w, &h);
    if (!t->lockmem[l]) {
        t->lockmem[l] = g_alloc(sz, 1);
        if (t->filled & (1u << l)) {   /* already uploaded once: read the level back so a partial lock keeps the rest */
            GLenum ifmt, pf, ty; int bpp, comp; GLint sw[4]; fmt_gl(t->fmt, &ifmt, &pf, &ty, &bpp, &comp, sw);
            cur(); glBindTexture(GL_TEXTURE_2D, t->tex); glPixelStorei(GL_PACK_ALIGNMENT, 1);
            if (comp) glGetCompressedTexImage(GL_TEXTURE_2D, l, MEM + t->lockmem[l]); else glGetTexImage(GL_TEXTURE_2D, l, pf, ty, MEM + t->lockmem[l]);
        }
    }
    uint32_t off = 0;
    if (rect) { uint32_t x = rd32(rect), y = rd32(rect + 4); GLenum a, b, c; int bpp, comp; GLint sw[4]; fmt_gl(t->fmt, &a, &b, &c, &bpp, &comp, sw);
        off = comp ? (y / 4) * pitch + (x / 4) * (uint32_t)bpp : y * pitch + x * (uint32_t)bpp; }
    wr32(lr, pitch); wr32(lr + 4, t->lockmem[l] + off); return 1;
}
M(T_LockRect, 5) { obj *t = me(); R(5, lock_level(t, (int)ARG(1), ARG(2), ARG(3)) ? 0 : 0x8876086Cu); }
/* Unlock: upload, then drop the CPU copy (it lived in the game heap: 16-32 MB per large texture, kept for nothing) */
static void unlock_level(obj *t, int l) {
    if (!t || l < 0 || l >= 16 || l >= t->levels || !t->lockmem[l]) return;
    cur(); tex_upload(t, l); t->filled |= 1u << l;
    gh_free(t->lockmem[l]); t->lockmem[l] = 0;
}
M(T_UnlockRect, 2) { unlock_level(me(), (int)ARG(1)); R(2, 0); }
M(T_AddDirtyRect, 2) { R(2, 0); }
M(S_GetType, 1) { R(1, 1); }
M(S_GetContainer, 3) { obj *s = me(); if (s->parent) { wr32(ARG(2), s->parent->g); s->parent->ref++; R(3, 0); } else R(3, 0x80004002u); }
M(S_GetDesc, 2) { obj *s = me(); uint32_t p = ARG(1); wr32(p, s->fmt); wr32(p + 4, 1); wr32(p + 8, s->is_rt ? 1 : 0); wr32(p + 12, s->pool); wr32(p + 16, 0); wr32(p + 20, 0); wr32(p + 24, (uint32_t)s->w); wr32(p + 28, (uint32_t)s->h); R(2, 0); }
M(S_LockRect, 4) {
    obj *s = me();
    if (s->parent) { if (!lock_level(s->parent, s->level, ARG(1), ARG(2))) { R(4, 0x8876086Cu); return; } }
    else { if (!s->vbmem) s->vbmem = g_alloc((uint32_t)(s->w * s->h * 4), 1); wr32(ARG(1), (uint32_t)s->w * 4); wr32(ARG(1) + 4, s->vbmem); }
    R(4, 0);
}
M(S_UnlockRect, 1) { obj *s = me(); if (s->parent && !s->parent->is_rt) unlock_level(s->parent, s->level); R(1, 0); }
/* vertex buffer */
M(B_Lock, 5) { obj *b = me(); wr32(ARG(3), b->vbmem + ARG(1)); R(5, 0); }
M(B_GetDesc, 2) { obj *b = me(); uint32_t p = ARG(1); memset(MEM + p, 0, 24); wr32(p, 100); wr32(p + 4, 2); wr32(p + 12, 0); wr32(p + 16, b->size); wr32(p + 20, b->fmt); R(2, 0); }
/* swap chain */
M(C_Present, 6) { present(); R(6, 0); }
M(C_GetBackBuffer, 4) { wr32(ARG(3), backbuf->g); backbuf->ref++; R(4, 0); }
M(C_GetDisplayMode, 2) { uint32_t p = ARG(1); wr32(p, bb_w); wr32(p + 4, bb_h); wr32(p + 8, 60); wr32(p + 12, 22); R(2, 0); }
M(C_GetPresentParameters, 2) { uint32_t p = ARG(1); memset(MEM + p, 0, 56); wr32(p, bb_w); wr32(p + 4, bb_h); wr32(p + 8, 22); wr32(p + 12, 1); wr32(p + 32, 1); R(2, 0); }
/* shaders */
M(H_GetFunction, 3) { obj *o = me(); if (ARG(1)) memcpy(MEM + ARG(1), o->code, 4 * o->ncode); if (ARG(2)) wr32(ARG(2), 4 * (uint32_t)o->ncode); R(3, 0); }
/* query */
M(Q_Issue, 2) { R(2, 0); } M(Q_GetData, 4) { if (ARG(1) && ARG(2) >= 4) wr32(ARG(1), 1); R(4, 0); } M(Q_GetType, 1) { R(1, 8); } M(Q_GetDataSize, 1) { R(1, 4); }

/* ================= vtables ================= */
typedef struct { const char *name; void (*fn)(void); } ent;
static uint32_t build_vt(const char *iface, const ent *e, int n) {
    uint32_t vt = g_static(4 * (uint32_t)n);
    for (int i = 0; i < n; i++) {
        char full[128]; snprintf(full, sizeof full, "D3D9!%s::%s", iface, e[i].name ? e[i].name : "?");
        if (e[i].fn) { imp_def d[2] = { { strdup(full), e[i].fn }, { NULL, NULL } }; imp_register(d); }
        wr32(vt + 4 * (uint32_t)i, imp_magic_for(full));
    }
    return vt;
}
#define E(n, f) { n, f }
static void build_all(void) {
    static const ent d3d[] = { E("QueryInterface", Q_QI), E("AddRef", Q_AddRef), E("Release", Q_Release), E("RegisterSoftwareDevice", ok2),
        E("GetAdapterCount", D_GetAdapterCount), E("GetAdapterIdentifier", D_GetAdapterIdentifier), E("GetAdapterModeCount", D_GetAdapterModeCount),
        E("EnumAdapterModes", D_EnumAdapterModes), E("GetAdapterDisplayMode", D_GetAdapterDisplayMode), E("CheckDeviceType", D_CheckDeviceType),
        E("CheckDeviceFormat", D_CheckDeviceFormat), E("CheckDeviceMultiSampleType", D_CheckDeviceMultiSampleType), E("CheckDepthStencilMatch", D_CheckDepthStencilMatch),
        E("CheckDeviceFormatConversion", ok4), E("GetDeviceCaps", D_GetDeviceCaps), E("GetAdapterMonitor", D_GetAdapterMonitor), E("CreateDevice", D_CreateDevice) };
    vt_d3d = build_vt("IDirect3D9", d3d, 17);
    static const ent dev[119] = { E("QueryInterface", Q_QI), E("AddRef", Q_AddRef), E("Release", Q_Release), E("TestCooperativeLevel", V_TestCooperativeLevel),
        E("GetAvailableTextureMem", V_GetAvailableTextureMem), E("EvictManagedResources", ok1), E("GetDirect3D", V_GetDirect3D), E("GetDeviceCaps", V_GetDeviceCaps),
        E("GetDisplayMode", V_GetDisplayMode), E("GetCreationParameters", V_GetCreationParameters), E("SetCursorProperties", ok4), E("SetCursorPosition", ok4),
        E("ShowCursor", V_ShowCursor), E("CreateAdditionalSwapChain", fail3), E("GetSwapChain", V_GetSwapChain), E("GetNumberOfSwapChains", V_GetNumberOfSwapChains),
        E("Reset", V_Reset), E("Present", V_Present), E("GetBackBuffer", V_GetBackBuffer), E("GetRasterStatus", V_GetRasterStatus), E("SetDialogBoxMode", ok2),
        E("SetGammaRamp", ok4), E("GetGammaRamp", ok3), E("CreateTexture", V_CreateTexture), E("CreateVolumeTexture", ok10), E("CreateCubeTexture", fail8),
        E("CreateVertexBuffer", V_CreateVertexBuffer), E("CreateIndexBuffer", fail7), E("CreateRenderTarget", V_CreateRenderTarget),
        E("CreateDepthStencilSurface", V_CreateDepthStencilSurface), E("UpdateSurface", V_UpdateSurface), E("UpdateTexture", ok3), E("GetRenderTargetData", V_GetRenderTargetData),
        E("GetFrontBufferData", ok3), E("StretchRect", V_StretchRect), E("ColorFill", V_ColorFill), E("CreateOffscreenPlainSurface", V_CreateOffscreenPlainSurface),
        E("SetRenderTarget", V_SetRenderTarget), E("GetRenderTarget", V_GetRenderTarget), E("SetDepthStencilSurface", V_SetDepthStencilSurface),
        E("GetDepthStencilSurface", V_GetDepthStencilSurface), E("BeginScene", V_BeginScene), E("EndScene", V_EndScene), E("Clear", V_Clear),
        E("SetTransform", V_SetTransform), E("GetTransform", V_GetTransform), E("MultiplyTransform", ok3), E("SetViewport", V_SetViewport), E("GetViewport", V_GetViewport),
        E("SetMaterial", ok2), E("GetMaterial", ok2), E("SetLight", ok3), E("GetLight", ok3), E("LightEnable", ok3), E("GetLightEnable", ok3), E("SetClipPlane", ok3),
        E("GetClipPlane", ok3), E("SetRenderState", V_SetRenderState), E("GetRenderState", V_GetRenderState), E("CreateStateBlock", V_CreateStateBlock),
        E("BeginStateBlock", ok1), E("EndStateBlock", ok2), E("SetClipStatus", ok2), E("GetClipStatus", ok2), E("GetTexture", V_GetTexture), E("SetTexture", V_SetTexture),
        E("GetTextureStageState", V_GetTextureStageState), E("SetTextureStageState", V_SetTextureStageState), E("GetSamplerState", V_GetSamplerState),
        E("SetSamplerState", V_SetSamplerState), E("ValidateDevice", V_ValidateDevice), E("SetPaletteEntries", ok3), E("GetPaletteEntries", ok3),
        E("SetCurrentTexturePalette", ok2), E("GetCurrentTexturePalette", ok2), E("SetScissorRect", V_SetScissorRect), E("GetScissorRect", V_GetScissorRect),
        E("SetSoftwareVertexProcessing", ok2), E("GetSoftwareVertexProcessing", V_GetSoftwareVertexProcessing), E("SetNPatchMode", ok2), E("GetNPatchMode", ok1),
        E("DrawPrimitive", V_DrawPrimitive), E("DrawIndexedPrimitive", ok7), E("DrawPrimitiveUP", V_DrawPrimitiveUP), E("DrawIndexedPrimitiveUP", ok9),
        E("ProcessVertices", ok7), E("CreateVertexDeclaration", fail3), E("SetVertexDeclaration", ok2), E("GetVertexDeclaration", ok2), E("SetFVF", V_SetFVF),
        E("GetFVF", V_GetFVF), E("CreateVertexShader", V_CreateVertexShader), E("SetVertexShader", V_SetVertexShader), E("GetVertexShader", V_GetVertexShader),
        E("SetVertexShaderConstantF", V_SetVertexShaderConstantF), E("GetVertexShaderConstantF", V_GetVertexShaderConstantF), E("SetVertexShaderConstantI", ok4),
        E("GetVertexShaderConstantI", ok4), E("SetVertexShaderConstantB", ok4), E("GetVertexShaderConstantB", ok4), E("SetStreamSource", V_SetStreamSource),
        E("GetStreamSource", ok5), E("SetStreamSourceFreq", ok3), E("GetStreamSourceFreq", ok3), E("SetIndices", ok2), E("GetIndices", ok2),
        E("CreatePixelShader", V_CreatePixelShader), E("SetPixelShader", V_SetPixelShader), E("GetPixelShader", V_GetPixelShader),
        E("SetPixelShaderConstantF", V_SetPixelShaderConstantF), E("GetPixelShaderConstantF", V_GetPixelShaderConstantF), E("SetPixelShaderConstantI", ok4),
        E("GetPixelShaderConstantI", ok4), E("SetPixelShaderConstantB", ok4), E("GetPixelShaderConstantB", ok4), E("DrawRectPatch", ok4), E("DrawTriPatch", ok4),
        E("DeletePatch", ok2), E("CreateQuery", V_CreateQuery) };
    vt_dev = build_vt("IDirect3DDevice9", dev, 119);
    static const ent res9[] = { E("QueryInterface", Q_QI), E("AddRef", Q_AddRef), E("Release", Q_Release), E("GetDevice", Q_GetDevice), E("SetPrivateData", ok5),
        E("GetPrivateData", ok4), E("FreePrivateData", ok2), E("SetPriority", ok2), E("GetPriority", ok1), E("PreLoad", ok1) };
    ent tex[22]; memcpy(tex, res9, sizeof res9);
    tex[10] = (ent)E("GetType", T_GetType); tex[11] = (ent)E("SetLOD", ok2); tex[12] = (ent)E("GetLOD", ok1); tex[13] = (ent)E("GetLevelCount", T_GetLevelCount);
    tex[14] = (ent)E("SetAutoGenFilterType", ok2); tex[15] = (ent)E("GetAutoGenFilterType", ok1); tex[16] = (ent)E("GenerateMipSubLevels", ok1);
    tex[17] = (ent)E("GetLevelDesc", T_GetLevelDesc); tex[18] = (ent)E("GetSurfaceLevel", T_GetSurfaceLevel); tex[19] = (ent)E("LockRect", T_LockRect);
    tex[20] = (ent)E("UnlockRect", T_UnlockRect); tex[21] = (ent)E("AddDirtyRect", T_AddDirtyRect);
    vt_tex = build_vt("IDirect3DTexture9", tex, 22);
    ent surf[17]; memcpy(surf, res9, sizeof res9);
    surf[10] = (ent)E("GetType", S_GetType); surf[11] = (ent)E("GetContainer", S_GetContainer); surf[12] = (ent)E("GetDesc", S_GetDesc);
    surf[13] = (ent)E("LockRect", S_LockRect); surf[14] = (ent)E("UnlockRect", S_UnlockRect); surf[15] = (ent)E("GetDC", fail3); surf[16] = (ent)E("ReleaseDC", ok2);
    vt_surf = build_vt("IDirect3DSurface9", surf, 17);
    ent vb[14]; memcpy(vb, res9, sizeof res9);
    vb[10] = (ent)E("GetType", T_GetType); vb[11] = (ent)E("Lock", B_Lock); vb[12] = (ent)E("Unlock", ok1); vb[13] = (ent)E("GetDesc", B_GetDesc);
    vt_vb = build_vt("IDirect3DVertexBuffer9", vb, 14);
    static const ent sh[] = { E("QueryInterface", Q_QI), E("AddRef", Q_AddRef), E("Release", Q_Release), E("GetDevice", Q_GetDevice), E("GetFunction", H_GetFunction) };
    vt_vs = build_vt("IDirect3DVertexShader9", sh, 5); vt_ps = build_vt("IDirect3DPixelShader9", sh, 5);
    static const ent sc[] = { E("QueryInterface", Q_QI), E("AddRef", Q_AddRef), E("Release", Q_Release), E("Present", C_Present), E("GetFrontBufferData", ok2),
        E("GetBackBuffer", C_GetBackBuffer), E("GetRasterStatus", ok2), E("GetDisplayMode", C_GetDisplayMode), E("GetDevice", Q_GetDevice), E("GetPresentParameters", C_GetPresentParameters) };
    vt_sc = build_vt("IDirect3DSwapChain9", sc, 10);
    static const ent q[] = { E("QueryInterface", Q_QI), E("AddRef", Q_AddRef), E("Release", Q_Release), E("GetDevice", Q_GetDevice), E("GetType", Q_GetType),
        E("GetDataSize", Q_GetDataSize), E("Issue", Q_Issue), E("GetData", Q_GetData) };
    vt_query = build_vt("IDirect3DQuery9", q, 8);
    vt_decl = 0; (void)fail4; (void)fail5; (void)ok6;
}

static void Direct3DCreate9_(void) {
    if (!vt_dev) build_all();
    obj *d = mk(O_D3D, vt_d3d); RET_STD(1, d->g);
}
static const imp_def defs[] = { { "D3D9.DLL!Direct3DCreate9", Direct3DCreate9_ }, { NULL, NULL } };
void d3d9_register(void) { imp_register(defs); }

/* remote frame capture (same mechanism as the Windows DLL: capture.req -> capture.bmp) */
unsigned sa_frame_no;
void sa_capture_check(void) {   /* image as shown in the window (before the buffer swap) */
    static int fd = -1; sa_frame_no++;
    if (fd < 0) fd = getenv("SA_FRAMEDUMP") != NULL;
    if (fd) {                   /* test: every frame around the Start / Aime states, 160x270 playfield, fd_<frame>_<state>.bmp */
        static unsigned last_nz = 0; uint32_t st = rd32(0x8efa30); extern int sa_start_dbg; if (st || sa_start_dbg) { last_nz = sa_frame_no; sa_start_dbg = 0; }
        if (last_nz && sa_frame_no - last_nz < 90) {
            int ww, wh; SDL_GL_GetDrawableSize(sa_window, &ww, &wh); int x0 = ww / 3, cw = ww / 3;
            uint8_t *px = malloc((size_t)cw * wh * 4); glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(x0, 0, cw, wh, GL_BGRA, GL_UNSIGNED_BYTE, px);
            int ow = cw / 4, oh = wh / 4; uint8_t *o = malloc((size_t)ow * oh * 4);
            for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) memcpy(o + 4 * (y * ow + x), px + 4 * ((size_t)y * 4 * cw + x * 4), 4);
            char nm[64]; snprintf(nm, sizeof nm, "fd_%05u_%02u.bmp", sa_frame_no, st); FILE *f = fopen(nm, "wb");
            if (f) { uint8_t h[54] = { 'B', 'M' }; uint32_t sz = 54 + (uint32_t)(ow * oh * 4); memcpy(h + 2, &sz, 4); h[10] = 54; h[14] = 40; int32_t w = ow, hh = oh;
                memcpy(h + 18, &w, 4); memcpy(h + 22, &hh, 4); h[26] = 1; h[28] = 32; fwrite(h, 1, 54, f); fwrite(o, 4, (size_t)ow * oh, f); fclose(f); }
            free(o); free(px);
        }
    }
    static unsigned n; if (++n % 30) return;
    FILE *r = fopen("capture.req", "rb"); if (!r) return; fclose(r); remove("capture.req");
    int ww, wh; SDL_GL_GetDrawableSize(sa_window, &ww, &wh);
    uint8_t *px = malloc((size_t)ww * wh * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, ww, wh, GL_BGRA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen("capture.bmp", "wb");
    if (f) {
        uint8_t h[54] = { 'B', 'M' }; uint32_t sz = 54 + (uint32_t)(ww * wh * 4);
        memcpy(h + 2, &sz, 4); h[10] = 54; h[14] = 40; int32_t w = ww, hh = wh; memcpy(h + 18, &w, 4); memcpy(h + 22, &hh, 4); h[26] = 1; h[28] = 32;
        fwrite(h, 1, 54, f); fwrite(px, 4, (size_t)ww * wh, f); fclose(f); sa_log("capture: capture.bmp (%dx%d)", ww, wh);
    }
    free(px);
}
