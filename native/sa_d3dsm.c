/* sa_d3dsm.c - translation of Direct3D 9 shaders (vs_2_0 / ps_2_0 bytecode) to GLSL 3.30, at creation time.
 * Covers the common SM 2.0 arithmetic instructions; an unknown instruction is reported in the log.
 * Output conventions identical to the fixed pipeline of sa_d3d9.c (top-to-bottom storage, D3D9 half-pixel). */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "sa_host.h"

typedef struct { char *s; size_t n, cap; } sb;
static void add(sb *b, const char *fmt, ...) {
    /* measure first, grow, then format directly into the destination (no fixed-size intermediate) */
    va_list a; va_start(a, fmt); int k = vsnprintf(NULL, 0, fmt, a); va_end(a);
    if (k < 0) return;
    if (b->n + (size_t)k + 1 > b->cap) { b->cap = (b->n + (size_t)k + 1) * 2; b->s = realloc(b->s, b->cap); }
    va_start(a, fmt); vsnprintf(b->s + b->n, b->cap - b->n, fmt, a); va_end(a);
    b->n += (size_t)k;
}
static int rtype(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
static int ps_mode;
static void regname(uint32_t t, char *o) {
    int ty = rtype(t), i = (int)(t & 0x7FF);
    switch (ty) {
    case 0: sprintf(o, "r%d", i); break;
    case 1: sprintf(o, ps_mode ? (i == 0 ? "vD0" : "vD1") : "v%d", i); break;
    case 2: sprintf(o, ps_mode ? "pcl[%d]" : "vcl[%d]", i); break;
    case 3: sprintf(o, ps_mode ? "vT%d" : "a0", i); break;
    case 4: sprintf(o, "oPos"); break;
    case 5: sprintf(o, "oD%d", i); break;
    case 6: sprintf(o, ps_mode ? "oC%d" : "oT%d", i); break;
    case 8: sprintf(o, "oC%d", i); break;
    case 10: sprintf(o, "s%d", i); break;
    default: sprintf(o, "unk%d_%d", ty, i);
    }
}
static void src(uint32_t t, char *o, int ncomp) {
    char r[32]; regname(t, r);
    uint32_t sw = (t >> 16) & 0xFF; const char *xyzw = "xyzw"; char s[5] = { 0 };
    for (int i = 0; i < 4; i++) s[i] = xyzw[(sw >> (2 * i)) & 3];
    int mod = (t >> 24) & 15;
    char body[64]; snprintf(body, sizeof body, "%s.%.*s", r, ncomp, s);
    if (mod == 1) sprintf(o, "(-%s)", body); else if (mod == 11) sprintf(o, "abs(%s)", body); else if (mod == 12) sprintf(o, "(-abs(%s))", body); else strcpy(o, body);
}
static void dstw(uint32_t t, char *d, char *mask, int *n) {
    char r[32]; regname(t, r); uint32_t m = (t >> 16) & 15; int k = 0; mask[0] = 0;
    for (int i = 0; i < 4; i++) if (m & (1u << i)) mask[k++] = "xyzw"[i];
    mask[k] = 0; *n = k; sprintf(d, "%s", r);
}

char *sm_to_glsl(const uint32_t *tok, int *is_ps) {
    sb b = { 0 }, body = { 0 };
    ps_mode = (tok[0] >> 16) == 0xFFFF; *is_ps = ps_mode;
    int maxr = 0, inputs[16] = { 0 }; int used_t[8] = { 0 }, used_oT[8] = { 0 }, used_s[8] = { 0 };
    for (int i = 0; i < 16; i++) inputs[i] = -1;
    const uint32_t *p = tok + 1;
    while (*p != 0x0000FFFF) {
        uint32_t ins = *p; uint32_t op = ins & 0xFFFF;
        if (op == 0xFFFE) { p += 1 + ((ins >> 16) & 0x7FFF); continue; }
        int len = (int)((ins >> 24) & 15);
        const uint32_t *a = p + 1;
        char d[32], m[8], s0[64], s1[64], s2[64]; int nm = 0;
        if (op == 31) {                         /* dcl */
            uint32_t usage = a[0] & 31; int ty = rtype(a[1]), idx = (int)(a[1] & 0x7FF);
            /* out-of-range register indices are ignored */
            if (!ps_mode && ty == 1 && idx < 16) inputs[idx] = (int)usage;
            if (ps_mode && ty == 3 && idx < 8) used_t[idx] = 1;
            if (ty == 10 && idx < 8) used_s[idx] = 1;
        } else if (op == 81) {                  /* def */
            float f[4]; memcpy(f, a + 1, 16); int idx = (int)(a[0] & 0x7FF);
            add(&body, "  %s[%d] = vec4(%.9g, %.9g, %.9g, %.9g);\n", ps_mode ? "pcl" : "vcl", idx, f[0], f[1], f[2], f[3]);
        } else if (op != 0) {
            dstw(a[0], d, m, &nm);
            if (rtype(a[0]) == 0 && (int)(a[0] & 0x7FF) + 1 > maxr) maxr = (int)(a[0] & 0x7FF) + 1;
            if (!ps_mode && rtype(a[0]) == 6) used_oT[a[0] & 7] = 1;
            int sat = (a[0] >> 20) & 1;
            char e[256] = "";
            int c = nm;
            switch (op) {
            case 1: src(a[1], s0, c); snprintf(e, sizeof e, "%s", s0); break;
            case 2: src(a[1], s0, c); src(a[2], s1, c); snprintf(e, sizeof e, "%s + %s", s0, s1); break;
            case 3: src(a[1], s0, c); src(a[2], s1, c); snprintf(e, sizeof e, "%s - %s", s0, s1); break;
            case 4: src(a[1], s0, c); src(a[2], s1, c); src(a[3], s2, c); snprintf(e, sizeof e, "%s * %s + %s", s0, s1, s2); break;
            case 5: src(a[1], s0, c); src(a[2], s1, c); snprintf(e, sizeof e, "%s * %s", s0, s1); break;
            case 6: src(a[1], s0, 4); snprintf(e, sizeof e, "vec4(1.0 / %s.w)%s", s0, ""); break;
            case 7: src(a[1], s0, 4); snprintf(e, sizeof e, "vec4(inversesqrt(abs(%s.w)))", s0); break;
            case 8: src(a[1], s0, 3); src(a[2], s1, 3); snprintf(e, sizeof e, "vec4(dot(%s, %s))", s0, s1); break;
            case 9: src(a[1], s0, 4); src(a[2], s1, 4); snprintf(e, sizeof e, "vec4(dot(%s, %s))", s0, s1); break;
            case 10: src(a[1], s0, c); src(a[2], s1, c); snprintf(e, sizeof e, "min(%s, %s)", s0, s1); break;
            case 11: src(a[1], s0, c); src(a[2], s1, c); snprintf(e, sizeof e, "max(%s, %s)", s0, s1); break;
            case 12: src(a[1], s0, c); src(a[2], s1, c); snprintf(e, sizeof e, "vec4(lessThan(%s, %s))%s", s0, s1, ""); break;
            case 13: src(a[1], s0, c); src(a[2], s1, c); snprintf(e, sizeof e, "vec4(greaterThanEqual(%s, %s))", s0, s1); break;
            case 14: src(a[1], s0, 4); snprintf(e, sizeof e, "vec4(exp2(%s.w))", s0); break;
            case 15: src(a[1], s0, 4); snprintf(e, sizeof e, "vec4(log2(abs(%s.w)))", s0); break;
            case 18: src(a[1], s0, c); src(a[2], s1, c); src(a[3], s2, c); snprintf(e, sizeof e, "mix(%s, %s, %s)", s2, s1, s0); break;
            case 19: src(a[1], s0, c); snprintf(e, sizeof e, "fract(%s)", s0); break;
            case 32: src(a[1], s0, 4); src(a[2], s1, 4); snprintf(e, sizeof e, "vec4(pow(abs(%s.w), %s.w))", s0, s1); break;
            case 35: src(a[1], s0, c); snprintf(e, sizeof e, "abs(%s)", s0); break;
            case 36: src(a[1], s0, 3); snprintf(e, sizeof e, "vec4(normalize(%s), 0.0)", s0); break;
            case 66: { char coord[64]; src(a[1], coord, 2); char smp[16]; regname(a[2], smp);
                snprintf(e, sizeof e, "texture(%s, %s)", smp, coord); break; }
            case 88: src(a[1], s0, c); src(a[2], s1, c); src(a[3], s2, c); snprintf(e, sizeof e, "mix(%s, %s, vec4(greaterThanEqual(%s, vec4(0.0))))", s2, s1, s0); break;
            case 90: src(a[1], s0, 2); src(a[2], s1, 2); src(a[3], s2, 4); snprintf(e, sizeof e, "vec4(dot(%s, %s) + %s.x)", s0, s1, s2); break;
            case 20: case 21: case 22: case 23: {
                int rows = (op == 20 || op == 22) ? 4 : 3, cols = (op == 20 || op == 21) ? 4 : 3; uint32_t base = a[2];
                src(a[1], s0, cols); char tmp[512] = "vec4(";
                for (int r = 0; r < 4; r++) { char x[96]; if (r < rows) { char cr[48]; uint32_t t2 = base + (uint32_t)r; src(t2, cr, cols); snprintf(x, sizeof x, "%sdot(%s, %s)", r ? ", " : "", s0, cr); } else snprintf(x, sizeof x, ", 0.0"); strcat(tmp, x); }
                strcat(tmp, ")"); snprintf(e, sizeof e, "%s", tmp); break; }
            default: sa_log("shader: opcode %u not translated", op); snprintf(e, sizeof e, "vec4(0.0)");
            }
            /* masked write: convert the expression to vec4, then select the components */
            char full[400];
            if (c < 4 && (op == 1 || op == 2 || op == 3 || op == 4 || op == 5 || op == 10 || op == 11 || op == 12 || op == 13 || op == 18 || op == 19 || op == 35 || op == 88))
                snprintf(full, sizeof full, "(%s)", e);
            else snprintf(full, sizeof full, "(%s).%s", e, m);
            if (sat) add(&body, "  %s.%s = clamp(%s, 0.0, 1.0);\n", d, m, full); else add(&body, "  %s.%s = %s;\n", d, m, full);
        }
        p += 1 + len;
    }
    add(&b, "#version 330 core\n");
    if (!ps_mode) {
        add(&b, "uniform vec4 vc[256]; uniform vec4 uVP;\n");
        for (int i = 0; i < 16; i++) if (inputs[i] >= 0) {
            int loc = inputs[i] == 0 ? 0 : inputs[i] == 10 ? 1 : inputs[i] == 5 ? 2 : 3;
            add(&b, "layout(location=%d) in vec4 %s;\n", loc, loc == 0 ? "aPos" : loc == 1 ? "aCol" : loc == 2 ? "aTex" : "aX");
        }
        add(&b, "out vec4 vD0; out vec4 vD1; out vec4 vT0; out vec4 vT1;\nvoid main(){\n  vec4 vcl[256]; for(int i=0;i<256;i++) vcl[i]=vc[i];\n");
        for (int i = 0; i < 16; i++) if (inputs[i] >= 0) {
            int u = inputs[i]; add(&b, "  vec4 v%d = %s;\n", i, u == 0 ? "vec4(aPos.xyz, 1.0)" : u == 10 ? "aCol.bgra" : u == 5 ? "vec4(aTex.xy, 0.0, 1.0)" : "aX");
        }
        add(&b, "  vec4 oPos = vec4(0,0,0,1), oD0 = vec4(1), oD1 = vec4(0), oT0 = vec4(0,0,0,1), oT1 = vec4(0,0,0,1);\n");
    } else {
        add(&b, "uniform vec4 pc[32]; uniform int uAT; uniform float uAref;\n");
        for (int i = 0; i < 8; i++) if (used_s[i]) add(&b, "uniform sampler2D s%d;\n", i);
        add(&b, "in vec4 vD0; in vec4 vD1; in vec4 vT0; in vec4 vT1; out vec4 fragColor;\nvoid main(){\n  vec4 pcl[32]; for(int i=0;i<32;i++) pcl[i]=pc[i];\n  vec4 oC0 = vec4(0);\n");
    }
    for (int i = 0; i < (maxr > 0 ? maxr : 1); i++) add(&b, "  vec4 r%d = vec4(0);\n", i);
    char *bd = body.s ? body.s : strdup("");
    add(&b, "%s", bd);
    if (!ps_mode) add(&b, "  gl_Position = oPos; gl_Position.y = -gl_Position.y; gl_Position.xy += gl_Position.w / uVP.zw;\n"
                       "  vD0 = clamp(oD0, 0.0, 1.0); vD1 = clamp(oD1, 0.0, 1.0); vT0 = oT0; vT1 = oT1;\n}\n");
    else add(&b, "  bool pass = uAT==8 || (uAT==2 && oC0.a<uAref) || (uAT==3 && oC0.a==uAref) || (uAT==4 && oC0.a<=uAref) || (uAT==5 && oC0.a>uAref) ||\n"
                 "    (uAT==6 && oC0.a!=uAref) || (uAT==7 && oC0.a>=uAref);\n  if(!pass) discard;\n  fragColor = oC0;\n}\n");
    if (bd != body.s) free(bd); else free(body.s);
    (void)used_t; (void)used_oT;
    if (sa_verbose > 1) sa_log("shader GLSL:\n%s", b.s);
    return b.s;
}
