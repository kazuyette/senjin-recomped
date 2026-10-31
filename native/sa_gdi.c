/* sa_gdi.c - the game's system text (MEmbedFont: APM3 / Aime messages, labels) rewritten natively.
 * The game creates a GDI font (CreateFontIndirectA, Shift-JIS charset), then requests each character
 * from GetGlyphOutlineA as GGO_BITMAP (1 bit/pixel). Here: FreeType on a Japanese font found by
 * fontconfig (Noto Sans CJK JP, IPAGothic, Takao...; SA_FONT=file to force one), and real
 * Shift-JIS (cp932) <-> Unicode conversions for MultiByteToWideChar / WideCharToMultiByte. */
#include <string.h>
#include <stdlib.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>
#include "sa_host.h"
#include "sa_sjis_tab.h"

/* ---------- Shift-JIS ---------- */
static int cmp2(const void *a, const void *b) { return (int)((const uint16_t *)a)[0] - (int)((const uint16_t *)b)[0]; }
uint16_t sjis_to_uni(uint16_t s) {
    if (s < 0x80) return s;
    uint16_t k[2] = { s, 0 }; const uint16_t *r = bsearch(k, sjis_s2u, sizeof sjis_s2u / sizeof *sjis_s2u, sizeof *sjis_s2u, cmp2);
    return r ? r[1] : 0x30FB;   /* ・ */
}
uint16_t uni_to_sjis(uint16_t u) {
    if (u < 0x80) return u;
    uint16_t k[2] = { u, 0 }; const uint16_t *r = bsearch(k, sjis_u2s, sizeof sjis_u2s / sizeof *sjis_u2s, sizeof *sjis_u2s, cmp2);
    return r ? r[1] : '?';
}
static int is_lead(uint8_t c) { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); }
static int sjis_cp(uint32_t cp) { return cp == 0 || cp == 1 || cp == 3 || cp == 932; }
/* MultiByteToWideChar(cp, flags, src, srclen, dst, dstlen) */
static void MultiByteToWideChar_(void) {
    uint32_t cp = ARG(0), src = ARG(2), dst = ARG(4), dn = ARG(5); int32_t sn = (int32_t)ARG(3);
    uint32_t len = sn < 0 ? (uint32_t)strlen(g_str(src)) + 1 : (uint32_t)sn, k = 0;
    for (uint32_t i = 0; i < len; ) {
        uint8_t c = rd8(src + i); uint32_t u;
        if (sjis_cp(cp) && is_lead(c) && i + 1 < len) { u = sjis_to_uni((uint16_t)(c << 8 | rd8(src + i + 1))); i += 2; }
        else if (sjis_cp(cp) && c >= 0xA1 && c <= 0xDF) { u = sjis_to_uni(c); i++; }
        else if (cp == 65001 && c >= 0x80) {
            int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1; u = c & (0x3F >> n); i++;
            for (int j = 0; j < n && i < len; j++, i++) u = u << 6 | (rd8(src + i) & 0x3F);
            if (u > 0xFFFF) u = 0xFFFD;
        } else { u = c; i++; }
        if (dn) { if (k >= dn) break; wr16(dst + 2 * k, (uint16_t)u); }
        k++;
    }
    RET_STD(6, k);
}
/* WideCharToMultiByte(cp, flags, src, srclen, dst, dstlen, defchar, useddef) */
static void WideCharToMultiByte_(void) {
    uint32_t cp = ARG(0), src = ARG(2), dst = ARG(4), dn = ARG(5); int32_t sn = (int32_t)ARG(3);
    uint32_t len = 0; if (sn < 0) { while (rd16(src + 2 * len)) len++; len++; } else len = (uint32_t)sn;
    uint32_t k = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint16_t u = rd16(src + 2 * i); uint8_t b[3]; int n = 1;
        if (u < 0x80) b[0] = (uint8_t)u;
        else if (cp == 65001) { if (u < 0x800) { b[0] = (uint8_t)(0xC0 | u >> 6); b[1] = (uint8_t)(0x80 | (u & 0x3F)); n = 2; } else { b[0] = (uint8_t)(0xE0 | u >> 12); b[1] = (uint8_t)(0x80 | (u >> 6 & 0x3F)); b[2] = (uint8_t)(0x80 | (u & 0x3F)); n = 3; } }
        else if (sjis_cp(cp)) { uint16_t s = uni_to_sjis(u); if (s > 0xFF) { b[0] = (uint8_t)(s >> 8); b[1] = (uint8_t)s; n = 2; } else b[0] = (uint8_t)s; }
        else b[0] = u < 0x100 ? (uint8_t)u : '?';
        if (dn) { if (k + (uint32_t)n > dn) break; for (int j = 0; j < n; j++) wr8(dst + k + (uint32_t)j, b[j]); }
        k += (uint32_t)n;
    }
    RET_STD(8, k);
}

/* ---------- fonts ---------- */
static FT_Library ft; static FT_Face face; static int ft_tried;
static void ft_open(void) {
    if (ft_tried) return; ft_tried = 1;
    if (FT_Init_FreeType(&ft)) { sa_log("text: FreeType unavailable"); return; }
    char file[1024] = ""; int index = 0;
    const char *env = getenv("SA_FONT"); if (env && *env) snprintf(file, sizeof file, "%s", env);
    if (!*file && FcInit()) {
        static const char *fam[] = { "Noto Sans CJK JP", "Noto Sans JP", "IPAexGothic", "IPAGothic", "TakaoGothic", "VL Gothic", "Source Han Sans JP", NULL };
        for (int i = 0; fam[i] && !*file; i++) {
            FcPattern *p = FcNameParse((const FcChar8 *)fam[i]); FcConfigSubstitute(NULL, p, FcMatchPattern); FcDefaultSubstitute(p);
            FcResult r; FcPattern *m = FcFontMatch(NULL, p, &r); FcChar8 *fn = NULL, *got = NULL;
            if (m && FcPatternGetString(m, FC_FAMILY, 0, &got) == FcResultMatch && got && strstr((const char *)got, fam[i]) == (const char *)got &&
                FcPatternGetString(m, FC_FILE, 0, &fn) == FcResultMatch) { snprintf(file, sizeof file, "%s", fn); FcPatternGetInteger(m, FC_INDEX, 0, &index); }
            if (m) FcPatternDestroy(m); FcPatternDestroy(p);
        }
        if (!*file) {   /* any font that covers Japanese */
            FcPattern *p = FcNameParse((const FcChar8 *)"sans-serif:lang=ja"); FcConfigSubstitute(NULL, p, FcMatchPattern); FcDefaultSubstitute(p);
            FcResult r; FcPattern *m = FcFontMatch(NULL, p, &r); FcChar8 *fn = NULL;
            if (m && FcPatternGetString(m, FC_FILE, 0, &fn) == FcResultMatch) { snprintf(file, sizeof file, "%s", fn); FcPatternGetInteger(m, FC_INDEX, 0, &index); }
            if (m) FcPatternDestroy(m); FcPatternDestroy(p);
        }
    }
    if (!*file || FT_New_Face(ft, file, index, &face)) { face = NULL; sa_log("text: no Japanese font (install fonts-noto-cjk): system texts missing"); return; }
    sa_log("text: font %s (%s)", face->family_name, file);
}
#define MAXF 32
static struct { int used, height; } F[MAXF];
#define HFONT_BASE 0x50000u
static int cur_font = -1;
static void CreateFontIndirectA_(void) {
    uint32_t lf = ARG(0);
    for (int i = 0; i < MAXF; i++) if (!F[i].used) { F[i].used = 1; F[i].height = (int32_t)rd32(lf); RET_STD(1, HFONT_BASE + (uint32_t)i + 1); return; }
    RET_STD(1, 0);
}
static void SelectObject_(void) {
    uint32_t h = ARG(1), prev = cur_font >= 0 ? HFONT_BASE + (uint32_t)cur_font + 1 : 0x40000u;
    if (h > HFONT_BASE && h <= HFONT_BASE + MAXF) cur_font = (int)(h - HFONT_BASE - 1);
    RET_STD(2, prev);
}
static void DeleteObject_(void) { uint32_t h = ARG(0); if (h > HFONT_BASE && h <= HFONT_BASE + MAXF) { F[h - HFONT_BASE - 1].used = 0; if (cur_font == (int)(h - HFONT_BASE - 1)) cur_font = -1; } RET_STD(1, 1); }
static int px_height(void) { return cur_font >= 0 ? F[cur_font].height : 16; }
/* FreeType size: negative height = character size, positive = cell height (ascender + descender) */
static void set_size(void) {
    int h = px_height(); if (!face) return;
    if (h < 0) FT_Set_Pixel_Sizes(face, 0, (FT_UInt)-h);
    else if (h > 0) { long cell = face->ascender - face->descender; FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(h * (long)face->units_per_EM / (cell > 0 ? cell : face->units_per_EM))); }
    else FT_Set_Pixel_Sizes(face, 0, 16);
}
static void GetTextMetricsA_(void) {
    uint32_t p = ARG(1); memset(MEM + p, 0, 56); ft_open(); int h = px_height(); if (h < 0) h = -h; if (!h) h = 16;
    int asc = h * 7 / 8, desc = h - asc, avg = h / 2, maxw = h;
    if (face) { set_size(); asc = (int)(face->size->metrics.ascender >> 6); desc = (int)(-face->size->metrics.descender >> 6); maxw = (int)(face->size->metrics.max_advance >> 6); avg = (int)(face->size->metrics.x_ppem / 2); }
    wr32(p, (uint32_t)(asc + desc)); wr32(p + 4, (uint32_t)asc); wr32(p + 8, (uint32_t)desc); wr32(p + 20, (uint32_t)avg); wr32(p + 24, (uint32_t)maxw);
    wr32(p + 28, 400); wr32(p + 40, 96); wr32(p + 44, 96); wr8(p + 48, 0x20); wr8(p + 49, 0xFF); wr8(p + 50, '?'); wr8(p + 51, ' ');
    wr8(p + 55, 0x80);                                   /* tmCharSet = SHIFTJIS_CHARSET */
    RET_STD(2, 1);
}
/* GetGlyphOutlineA(hdc, uChar, uFormat, lpgm, cjBuffer, pvBuffer, lpmat2): GGO_METRICS (0) / GGO_BITMAP (1) / GGO_GRAY8 (6) */
static void GetGlyphOutlineA_(void) {
    uint32_t ch = ARG(1), fmt = ARG(2), gm = ARG(3), cb = ARG(4), buf = ARG(5);
    ft_open();
    uint16_t u = ch > 0xFF ? sjis_to_uni((uint16_t)ch) : (ch >= 0xA1 && ch <= 0xDF ? sjis_to_uni((uint16_t)ch) : (uint16_t)ch);
    if (!face) { RET_STD(7, 0xFFFFFFFFu); return; }
    set_size();
    FT_UInt gi = FT_Get_Char_Index(face, u);
    if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) || FT_Render_Glyph(face->glyph, fmt == 1 ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL)) { RET_STD(7, 0xFFFFFFFFu); return; }
    FT_GlyphSlot g = face->glyph; FT_Bitmap *bm = &g->bitmap;
    uint32_t w = bm->width, h = bm->rows;
    if (gm) { wr32(gm, w ? w : 1); wr32(gm + 4, h ? h : 1); wr32(gm + 8, (uint32_t)g->bitmap_left); wr32(gm + 12, (uint32_t)g->bitmap_top);
              wr16(gm + 16, (uint16_t)(g->advance.x >> 6)); wr16(gm + 18, 0); }
    if (fmt == 0) { RET_STD(7, 1); return; }
    uint32_t stride = fmt == 1 ? (w >> 5) * 4 + 4 : (w + 3) & ~3u, need = stride * h;   /* 1 bit: same calculation as the game */
    if (!w || !h) { RET_STD(7, 0); return; }
    if (!cb || !buf) { RET_STD(7, need); return; }
    memset(MEM + buf, 0, cb < need ? cb : need);
    for (uint32_t y = 0; y < h && (y + 1) * stride <= cb; y++) {
        const uint8_t *row = bm->buffer + (int)y * bm->pitch;
        for (uint32_t x = 0; x < w; x++) {
            if (fmt == 1) { int on = bm->pixel_mode == FT_PIXEL_MODE_MONO ? (row[x >> 3] >> (7 - (x & 7))) & 1 : row[x] >= 128; if (on) MEM[buf + y * stride + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7)); }
            else { uint8_t v = bm->pixel_mode == FT_PIXEL_MODE_MONO ? (uint8_t)(((row[x >> 3] >> (7 - (x & 7))) & 1) * 255) : row[x]; MEM[buf + y * stride + x] = (uint8_t)((v * 64 + 127) / 255); }
        }
    }
    RET_STD(7, need);
}
static const imp_def defs[] = {
    { "KERNEL32.DLL!MultiByteToWideChar", MultiByteToWideChar_ }, { "KERNEL32.DLL!WideCharToMultiByte", WideCharToMultiByte_ },
    { "GDI32.DLL!CreateFontIndirectA", CreateFontIndirectA_ }, { "GDI32.DLL!SelectObject", SelectObject_ }, { "GDI32.DLL!DeleteObject", DeleteObject_ },
    { "GDI32.DLL!GetTextMetricsA", GetTextMetricsA_ }, { "GDI32.DLL!GetGlyphOutlineA", GetGlyphOutlineA_ },
    { NULL, NULL }
};
void gdi_register(void) { imp_register(defs); }
