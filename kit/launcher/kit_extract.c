/* kit_extract.c - voir kit_extract.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "kit_extract.h"
#ifdef _WIN32
#include <windows.h>
static FILE *fo(const char *p, const char *m) { wchar_t w[1100], wm[8]; MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1100); MultiByteToWideChar(CP_UTF8, 0, m, -1, wm, 8); return _wfopen(w, wm); }
static int mkd(const char *p) { wchar_t w[1100]; MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1100); return CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS; }
#else
#include <sys/stat.h>
static FILE *fo(const char *p, const char *m) { return fopen(p, m); }
static int mkd(const char *p) { return mkdir(p, 0777) == 0 || errno == EEXIST; }
#endif
static void mkdirs(const char *path) {
    char t[1100]; strncpy(t, path, 1099); t[1099] = 0;
    for (char *p = t + 1; *p; p++) if (*p == '/' || *p == '\\') { char c = *p; *p = 0; if (!(p == t + 2 && t[1] == ':')) mkd(t); *p = c; }
    mkd(t);
}
static long long fsize_of(const char *p) { FILE *f = fo(p, "rb"); if (!f) return -1; fseek(f, 0, SEEK_END); long long n = ftell(f); fclose(f); return n; }

typedef struct { FILE *f; int ss, off; } Img;
typedef struct { char path[600]; unsigned lba, size; } Ent;
static Ent *E; static int ne, cap;

static int read_data(Img *im, unsigned lba, unsigned char *out, int *len) {
    unsigned char t[2352]; if (fseek(im->f, (long)((long long)lba * im->ss), SEEK_SET) || fread(t, 1, im->ss, im->f) != (size_t)im->ss) return -1;
    *len = (im->off == 24 && (t[18] & 0x20)) ? 2324 : 2048;     /* XA form2 : 2324 octets utiles */
    memcpy(out, t + im->off, *len); return 0;
}
static unsigned le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static int walk(Img *im, unsigned lba, unsigned size, const char *prefix, int depth) {
    if (depth > 12 || size > (64u << 20)) return -1;
    unsigned ns = (size + 2047) / 2048; unsigned char *d = malloc((size_t)ns * 2048); if (!d) return -1;
    for (unsigned s = 0; s < ns; s++) { int l; if (read_data(im, lba + s, d + s * 2048, &l)) { free(d); return -1; } }
    for (unsigned o = 0; o < size;) {
        unsigned rl = d[o]; if (!rl) { o = (o / 2048 + 1) * 2048; continue; }
        if (o + rl > size) break;
        const unsigned char *r = d + o; unsigned nl = r[32]; o += rl;
        if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;                       /* . et .. */
        char nm[300]; if (nl > 255) nl = 255; memcpy(nm, r + 33, nl); nm[nl] = 0;
        char *sc = strchr(nm, ';'); if (sc) *sc = 0;
        size_t L = strlen(nm); if (L && nm[L - 1] == '.') nm[L - 1] = 0;
        char full[600]; snprintf(full, sizeof full, "%s%s", prefix, nm);
        if (r[25] & 2) { char sub[700]; snprintf(sub, sizeof sub, "%s/", full); if (walk(im, le32(r + 2), le32(r + 10), sub, depth + 1)) { free(d); return -1; } }
        else {
            if (ne == cap) { cap = cap ? cap * 2 : 256; E = realloc(E, cap * sizeof *E); }
            strncpy(E[ne].path, full, 599); E[ne].path[599] = 0; E[ne].lba = le32(r + 2); E[ne].size = le32(r + 10); ne++;
        }
    }
    free(d); return 0;
}

static int detect(Img *im) {
    static const int c[4][2] = { {2048, 0}, {2352, 16}, {2352, 24}, {2336, 8} };
    for (int i = 0; i < 4; i++) {
        unsigned char t[2352]; im->ss = c[i][0]; im->off = c[i][1];
        if (fseek(im->f, (long)(16LL * im->ss), SEEK_SET) == 0 && fread(t, 1, im->ss, im->f) == (size_t)im->ss && t[im->off] == 1 && !memcmp(t + im->off + 1, "CD001", 5)) return 0;
    }
    return -1;
}
/* .cue -> premier fichier FILE "xxx.bin" (chemin relatif au .cue) */
static int cue_to_bin(const char *cue, char *bin, int n) {
    FILE *f = fo(cue, "rb"); if (!f) return -1; char l[1100]; int ok = -1;
    while (fgets(l, sizeof l, f)) { char *p = strstr(l, "FILE"); if (!p) continue; char *a = strchr(p, '"'), *b = a ? strchr(a + 1, '"') : 0; if (!a || !b) continue; *b = 0;
        char dir[1100]; strncpy(dir, cue, 1099); dir[1099] = 0; char *s = strrchr(dir, '/'), *s2 = strrchr(dir, '\\'); if (s2 > s) s = s2; if (s) s[1] = 0; else dir[0] = 0;
        snprintf(bin, n, "%s%s", dir, a + 1); ok = 0; break; }
    fclose(f); return ok;
}
int kx_extract_image(const char *src, const char *dst, kx_progress cb, void *ud, char *err, int errn, int *nfiles) {
    char bin[1100]; size_t sl = strlen(src); ne = 0;
    if (sl > 4 && (!strcmp(src + sl - 4, ".cue") || !strcmp(src + sl - 4, ".CUE"))) { if (cue_to_bin(src, bin, sizeof bin)) { snprintf(err, errn, "Fichier .cue illisible"); return 1; } src = bin; }
    Img im; im.f = fo(src, "rb"); if (!im.f) { snprintf(err, errn, "Impossible d'ouvrir l'image : %s", src); return 2; }
    if (detect(&im)) { fclose(im.f); snprintf(err, errn, "Pas un disque ISO9660 reconnu (2048/2336/2352)"); return 3; }
    unsigned char pvd[2352]; int l; read_data(&im, 16, pvd, &l);
    if (walk(&im, le32(pvd + 156 + 2), le32(pvd + 156 + 10), "", 0)) { fclose(im.f); snprintf(err, errn, "Répertoire ISO illisible"); return 4; }
    unsigned long long total = 0, done = 0; for (int i = 0; i < ne; i++) total += E[i].size;
    mkdirs(dst); int rc = 0;
    for (int i = 0; i < ne && !rc; i++) {
        char out[1800]; snprintf(out, sizeof out, "%s/%s", dst, E[i].path);
        { char dd[1800]; strcpy(dd, out); char *s = strrchr(dd, '/'); if (s) { *s = 0; mkdirs(dd); } }
        if (fsize_of(out) == (long long)E[i].size) { done += E[i].size; if (cb) cb(ud, done, total, E[i].path); continue; }
        FILE *o = fo(out, "wb"); if (!o) { snprintf(err, errn, "Écriture impossible : %s", out); rc = 5; break; }
        unsigned ns = (E[i].size + 2047) / 2048, rem = E[i].size; unsigned char buf[2352];
        for (unsigned s = 0; s < ns; s++) {
            int len; if (read_data(&im, E[i].lba + s, buf, &len)) { snprintf(err, errn, "Lecture image : secteur %u (%s)", E[i].lba + s, E[i].path); rc = 6; break; }
            unsigned w = (len == 2324) ? 2324 : (rem < 2048 ? rem : 2048); fwrite(buf, 1, w, o); if (len == 2048) rem -= w;
            done += 2048 < E[i].size - s * 2048 ? 2048 : E[i].size - s * 2048;
            if (cb && (s & 63) == 0) cb(ud, done, total, E[i].path);
        }
        fclose(o);
    }
    fclose(im.f); if (nfiles) *nfiles = ne; if (cb && !rc) cb(ud, total, total, ""); free(E); E = NULL; cap = 0; return rc;
}
#ifdef KIT_EXTRACT_TEST
static void pr(void *u, unsigned long long d, unsigned long long t, const char *w) { (void)u; if (d == t) printf("progress %llu/%llu\n", d, t); }
int main(int c, char **v) { char e[300] = ""; int n = 0; int r = kx_extract_image(v[1], v[2], pr, 0, e, sizeof e, &n); printf("rc=%d files=%d %s\n", r, n, e); return r; }
#endif
