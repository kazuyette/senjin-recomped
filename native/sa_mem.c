/* sa_mem.c - mémoire du jeu : bloc de 4 Go (adresses 32 bits d'origine), tas, chaînes. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "sa_host.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

int sa_verbose = 0;
void sa_log(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr);
}
void sa_fatal(const char *fmt, ...) {
    va_list a; va_start(a, fmt); fprintf(stderr, "FATAL : "); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); exit(1);
}

void mem_init(void) {
#ifdef _WIN32
    /* réservation + engagement à la demande : simplifié ici (engagement complet, pages non touchées = pas de RAM) */
    MEM = VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    MEM = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (MEM == MAP_FAILED) MEM = NULL;
#endif
    if (!MEM) sa_fatal("impossible de réserver 4 Go d'espace d'adressage pour le jeu");
}

/* ---- tas : classes de tailles (puissances de 2, 16 o .. 1 Mo) + gros blocs (multiples de 64 Ko, meilleur ajustement,
 *      découpe et fusion, mémoire rendue au système à la libération). Protégé par un verrou : le jeu alloue
 *      depuis tous ses threads. En-tête : [taille demandée][classe ou 32][magie][capacité (gros blocs)] ---- */
#include <pthread.h>
#define HDR 16u
#define MAGIC 0x48454150u
#define BIGC 32u
#define SMALL_MAX 20                              /* classe max des petits blocs : 1 Mo */
static pthread_mutex_t hl = PTHREAD_MUTEX_INITIALIZER;
static uint32_t brk_ = G_HEAP_LO;
static uint32_t freel[32];
#define MAXBIG 4096
static struct { uint32_t a, n; } bigf[MAXBIG]; static int nbig;   /* gros blocs libres, triés par adresse */
static int cls_of(uint32_t n) { int c = 4; while ((1u << c) < n) c++; return c; }
static uint32_t bump(uint32_t n) {
    uint32_t p = (brk_ + 15) & ~15u;
    if ((uint64_t)p + n > G_HEAP_HI) { pthread_mutex_unlock(&hl); sa_fatal("tas du jeu épuisé (%u o demandés)", n); }
    brk_ = p + n; return p;
}
static void big_insert(uint32_t a, uint32_t n) {
    int i = 0; while (i < nbig && bigf[i].a < a) i++;
    if (i > 0 && bigf[i - 1].a + bigf[i - 1].n == a) { bigf[i - 1].n += n; if (i < nbig && bigf[i - 1].a + bigf[i - 1].n == bigf[i].a) { bigf[i - 1].n += bigf[i].n; memmove(&bigf[i], &bigf[i + 1], (size_t)(nbig - i - 1) * sizeof bigf[0]); nbig--; } return; }
    if (i < nbig && a + n == bigf[i].a) { bigf[i].a = a; bigf[i].n += n; return; }
    if (nbig >= MAXBIG) return;                   /* perdu (rare) */
    memmove(&bigf[i + 1], &bigf[i], (size_t)(nbig - i) * sizeof bigf[0]); bigf[i].a = a; bigf[i].n = n; nbig++;
}
static uint32_t big_alloc(uint32_t n) {
    int best = -1; for (int i = 0; i < nbig; i++) if (bigf[i].n >= n && (best < 0 || bigf[i].n < bigf[best].n)) best = i;
    if (best < 0) {                               /* bloc libre en fin de tas : on l'étend */
        if (nbig && bigf[nbig - 1].a + bigf[nbig - 1].n == brk_) { uint32_t a = bigf[nbig - 1].a; nbig--; brk_ = a; }
        uint32_t a = bump(n); wr32(a + 12, n); return a;
    }
    uint32_t a = bigf[best].a;
    if (bigf[best].n - n >= 0x10000u) { bigf[best].a += n; bigf[best].n -= n; }
    else { n = bigf[best].n; memmove(&bigf[best], &bigf[best + 1], (size_t)(nbig - best - 1) * sizeof bigf[0]); nbig--; }
    wr32(a + 12, n); return a;
}
uint32_t g_alloc(uint32_t size, int zero) {
    if (size == 0) size = 1;
    pthread_mutex_lock(&hl);
    uint32_t blk, c;
    if (size + HDR <= (1u << SMALL_MAX)) {
        c = (uint32_t)cls_of(size + HDR);
        if (freel[c]) { blk = freel[c]; freel[c] = rd32(blk + HDR); } else blk = bump(1u << c);
    } else {
        c = BIGC; uint32_t n = (size + HDR + 0xFFFFu) & ~0xFFFFu;
        blk = big_alloc(n);
    }
    wr32(blk, size); wr32(blk + 4, c); wr32(blk + 8, MAGIC);
    if (c == BIGC && getenv("SA_HEAPDBG")) sa_log("big alloc %08x n=%x size=%x", blk, rd32(blk + 12), size);
    pthread_mutex_unlock(&hl);
    if (zero) memset(MEM + blk + HDR, 0, size);
    return blk + HDR;
}
void g_free(uint32_t p) {
    if (!p) return;
    uint32_t blk = p - HDR;
    pthread_mutex_lock(&hl);
    if (rd32(blk + 8) != MAGIC) { pthread_mutex_unlock(&hl); sa_log("g_free : pointeur invalide 0x%08x (en-tête %08x %08x %08x %08x, appel de %08x)", p, rd32(blk), rd32(blk + 4), rd32(blk + 8), rd32(blk + 12), rd32(cpu.esp)); return; }
    uint32_t c = rd32(blk + 4);
    wr32(blk + 8, 0);
    if (c < 32) { wr32(p, freel[c]); freel[c] = blk; }
    else {
        uint32_t n = rd32(blk + 12);
        if (getenv("SA_HEAPDBG")) sa_log("big free %08x n=%x", blk, n);
#ifndef _WIN32
        uint32_t pg = (blk + 0x10000u) & ~0xFFFu, pe = (blk + n) & ~0xFFFu; if (pe > pg) madvise(MEM + pg, pe - pg, MADV_DONTNEED);   /* pages entières seulement */
#endif
        big_insert(blk, n);
    }
    pthread_mutex_unlock(&hl);
}
uint32_t g_size(uint32_t p) { return p ? rd32(p - HDR) : 0; }
uint32_t g_realloc(uint32_t p, uint32_t size) {
    if (!p) return g_alloc(size, 0);
    uint32_t old = g_size(p), c = rd32(p - HDR + 4);
    uint32_t cap = c < 32 ? (1u << c) : rd32(p - HDR + 12);
    if (size + HDR <= cap) { wr32(p - HDR, size); return p; }
    uint32_t q = g_alloc(size, 0);
    memcpy(MEM + q, MEM + p, old < size ? old : size);
    g_free(p); return q;
}

/* ---- zone statique de l'hôte (jamais libérée) ---- */
static uint32_t sbrk_ = G_STRINGS;
uint32_t g_static(uint32_t size) {
    pthread_mutex_lock(&hl); uint32_t p = (sbrk_ + 15) & ~15u; sbrk_ = p + size; pthread_mutex_unlock(&hl);
    if (sbrk_ > G_MAGIC) sa_fatal("zone statique épuisée");
    memset(MEM + p, 0, size); return p;
}
uint32_t g_strdup(const char *s) { uint32_t n = (uint32_t)strlen(s) + 1, p = g_static(n); memcpy(MEM + p, s, n); return p; }
const char *g_str(uint32_t p) { return p ? (const char *)(MEM + p) : NULL; }

/* UTF-8 <-> UTF-16 (BMP + paires) */
uint32_t g_wstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s), p = g_static(2 * n + 2), o = p;
    const unsigned char *u = (const unsigned char *)s;
    while (*u) {
        uint32_t c = *u++;
        if (c >= 0xF0) { c = (c & 7) << 18 | (u[0] & 63) << 12 | (u[1] & 63) << 6 | (u[2] & 63); u += 3; }
        else if (c >= 0xE0) { c = (c & 15) << 12 | (u[0] & 63) << 6 | (u[1] & 63); u += 2; }
        else if (c >= 0xC0) { c = (c & 31) << 6 | (u[0] & 63); u += 1; }
        if (c >= 0x10000) { c -= 0x10000; wr16(o, 0xD800 | (c >> 10)); o += 2; c = 0xDC00 | (c & 0x3FF); }
        wr16(o, (uint16_t)c); o += 2;
    }
    wr16(o, 0); return p;
}
int g_wstr_to_utf8(uint32_t p, char *out, int n) {
    int k = 0;
    if (!p) { if (n) out[0] = 0; return 0; }
    for (;; p += 2) {
        uint32_t c = rd16(p);
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00) { uint32_t d = rd16(p + 2); c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); p += 2; }
        char b[4]; int m;
        if (c < 0x80) { b[0] = (char)c; m = 1; }
        else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); m = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 63)); b[2] = (char)(0x80 | (c & 63)); m = 3; }
        else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | ((c >> 12) & 63)); b[2] = (char)(0x80 | ((c >> 6) & 63)); b[3] = (char)(0x80 | (c & 63)); m = 4; }
        if (k + m >= n) break;
        memcpy(out + k, b, m); k += m;
    }
    out[k] = 0; return k;
}
