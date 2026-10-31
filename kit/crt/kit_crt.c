/* kit_crt.c - see kit_crt.h. Algorithm and constants identical to Soul Calibur (no setting changed). */
#include <stdlib.h>
#include <math.h>
#include "kit_crt.h"

static int *t_x0, *t_x1, *t_wx, *t_m, *t_y, *t_yf;
static void crt_tables(int w, int h, int dw, int dh, int lvl) {
    static const float SCAN[4] = { 0, 0.40f, 0.60f, 0.80f }, MASK[4] = { 0, 0.0f, 0.22f, 0.38f }, VIG[4] = { 0, 0.08f, 0.16f, 0.28f };
    free(t_x0); free(t_x1); free(t_wx); free(t_m); free(t_y); free(t_yf);
    t_x0 = malloc(dw * sizeof(int)); t_x1 = malloc(dw * sizeof(int)); t_wx = malloc(dw * sizeof(int)); t_m = malloc(dw * 3 * sizeof(int)); t_y = malloc(dh * sizeof(int)); t_yf = malloc(dh * sizeof(int));
    float sc = SCAN[lvl], mk = MASK[lvl], vg = VIG[lvl];
    float boost = 1.0f / (1.0f - sc * 0.33f);      /* compensates for the average brightness loss */
    float mboost = 1.0f / (1.0f - mk * 0.33f);
    for (int x = 0; x < dw; x++) {
        float fx = ((x + 0.5f) * w / dw) - 0.5f; int x0 = (int)floorf(fx); float fr = fx - x0; int x1 = x0 + 1;
        if (x0 < 0) x0 = 0; if (x1 < 0) x1 = 0; if (x0 > w - 1) x0 = w - 1; if (x1 > w - 1) x1 = w - 1;
        t_x0[x] = x0; t_x1[x] = x1; t_wx[x] = (int)(fr * 256);
        float cx = ((x + 0.5f) / dw) * 2 - 1; float v = 1.0f - vg * cx * cx * cx * cx * 2.0f; if (v < 0) v = 0;
        for (int c = 0; c < 3; c++) { float m = 1.0f; if (mk > 0) { m = ((x % 3) == c) ? 1.0f : (1.0f - mk); m *= mboost; } float o = m * v; if (o > 1.6f) o = 1.6f; t_m[x * 3 + c] = (int)(o * 256); }
    }
    for (int y = 0; y < dh; y++) {
        float fy = ((y + 0.5f) * h / dh); int sy = (int)floorf(fy); if (sy > h - 1) sy = h - 1; float fr = fy - sy; float d = fabsf(fr - 0.5f) * 2.0f;  /* 0 at the line center, 1 at the edge */
        float f = (1.0f - sc * d * d) * boost; float cy = ((y + 0.5f) / dh) * 2 - 1; float v = 1.0f - vg * cy * cy * cy * cy * 2.0f; if (v < 0) v = 0;
        t_y[y] = sy; t_yf[y] = (int)(f * v * 256);
    }
}
void kit_crt_render(const uint32_t *src, int w, int h, uint32_t *dst, int dw, int dh, int lvl) {
    static int pw, ph, pdw, pdh, plvl;
    if (pw != w || ph != h || pdw != dw || pdh != dh || plvl != lvl) { crt_tables(w, h, dw, dh, lvl); pw = w; ph = h; pdw = dw; pdh = dh; plvl = lvl; }
    for (int y = 0; y < dh; y++) {
        const uint32_t *row = src + (size_t)t_y[y] * w; int yf = t_yf[y]; uint32_t *o = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            uint32_t a = row[t_x0[x]], b = row[t_x1[x]]; int wx = t_wx[x], iw = 256 - wx;
            int r = ((((a >> 16) & 255) * iw + ((b >> 16) & 255) * wx) >> 8), g = ((((a >> 8) & 255) * iw + ((b >> 8) & 255) * wx) >> 8), bl = (((a & 255) * iw + (b & 255) * wx) >> 8);
            const int *m = t_m + x * 3;
            r = (r * yf >> 8) * m[0] >> 8; g = (g * yf >> 8) * m[1] >> 8; bl = (bl * yf >> 8) * m[2] >> 8;
            if (r > 255) r = 255; if (g > 255) g = 255; if (bl > 255) bl = 255;
            o[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
        }
    }
}
