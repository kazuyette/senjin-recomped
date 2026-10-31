/* sa_xaudio2.c - XAudio2 2.7 réécrit en natif (CLSID 5a508685-...) : moteur, voix maîtresse / sous-mix / source,
 * mixage logiciel 48 kHz stéréo float dans un thread hôte, sortie SDL2 (sans périphérique : cadencé sur l'horloge).
 * Les rappels du jeu (IXAudio2VoiceCallback) sont exécutés depuis ce thread, avec sa propre pile/TEB invitée,
 * comme le fait le thread de traitement de XAudio2. Formats : PCM 8/16/24/32 bits, float 32, MS-ADPCM, xWMA
 * (décodé en PCM par ffmpeg, une fois par tampon, avec cache mémoire et cache disque ~/.cache/senjin_aleste). */
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include "sa_host.h"

extern void call_ind(uint32_t);
#define OUT_RATE 48000
#define QUANTUM  480                        /* 10 ms, comme XAudio2 */
#define MAXV     256
#define MAXQ     64
#define OBJ_MAGIC 0x58413237u               /* 'XA27' */

typedef struct { uint32_t flags, bytes, data, pbeg, plen, lbeg, llen, lcount, ctx; uint32_t loops; int started; int wq; const int16_t *pcm; uint32_t pcm_frames; } xbuf;
typedef struct {
    int used, kind;                         /* 1 maîtresse, 2 sous-mix, 3 source */
    uint32_t gobj, cb;                      /* objet invité, rappel invité (IXAudio2VoiceCallback*) */
    int tag, ch, rate, bits, align, spb;    /* format ; spb = échantillons par bloc ADPCM */
    float vol, chvol[8], mat[16]; int has_mat;
    float ratio; int playing, dest;         /* dest : index de la voix sous-mix destinataire (-1 = maîtresse) */
    xbuf q[MAXQ]; int qh, qn; double pos; uint64_t played;
    int destroyed, wma; uint8_t wfx[64]; int wfx_len;
} voice;
static voice V[MAXV];
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cblk;                /* récursif : DestroyVoice attend la fin des rappels en cours */
static uint32_t vt_engine, vt_master, vt_submix, vt_source, engine_obj, engine_cb[4];
static int engine_running = 1, master = -1;
static SDL_AudioDeviceID dev;
static pthread_t mixer; static int mixer_on;

static float f32(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static uint32_t u32f(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static voice *me(void) { uint32_t o = ARG(0); uint32_t i = rd32(o + 4); return (rd32(o + 8) == OBJ_MAGIC && i < MAXV && V[i].used) ? &V[i] : NULL; }
static int vidx(voice *v) { return (int)(v - V); }
static int voice_of_obj(uint32_t o) { if (!o || rd32(o + 8) != OBJ_MAGIC) return -1; uint32_t i = rd32(o + 4); return i < MAXV && V[i].used ? (int)i : -1; }

static int new_voice(int kind, uint32_t vt) {
    for (int i = 0; i < MAXV; i++) if (!V[i].used) {
        voice *v = &V[i]; memset(v, 0, sizeof *v); v->used = 1; v->kind = kind; v->vol = 1; v->ratio = 1; v->dest = -1;
        for (int c = 0; c < 8; c++) v->chvol[c] = 1;
        v->gobj = g_alloc(16, 1); wr32(v->gobj, vt); wr32(v->gobj + 4, (uint32_t)i); wr32(v->gobj + 8, OBJ_MAGIC);
        return i;
    }
    return -1;
}

/* ---------- appel de code invité depuis le thread de mixage ---------- */
static void gcall(uint32_t fn, int n, const uint32_t *a) {
    if (!fn || (fn & 0xFFF00000u) == G_MAGIC) return;
    uint32_t esp = cpu.esp;
    for (int i = n - 1; i >= 0; i--) push32(a[i]);
    push32(0xFFFFFFF0u); call_ind(fn);
    cpu.esp = esp;
}
static void vcb(uint32_t cb, int slot, int n, uint32_t a1, uint32_t a2) {   /* méthode n° slot de l'interface de rappel */
    if (!cb) return; uint32_t a[3] = { cb, a1, a2 }; gcall(rd32(rd32(cb) + 4u * (uint32_t)slot), n + 1, a);
}

/* ---------- lecture d'un échantillon (frame f, canal c) ---------- */
static float adpcm_sample(voice *v, const xbuf *b, uint32_t f, int c);
static float sample(voice *v, const xbuf *b, uint32_t f, int c) {
    if (b->pcm) return f < b->pcm_frames ? b->pcm[f * (uint32_t)v->ch + (uint32_t)c] / 32768.0f : 0;
    const uint8_t *p = MEM + b->data;
    switch (v->tag) {
    case 1: {
        uint32_t o = f * (uint32_t)v->align + (uint32_t)c * (uint32_t)(v->bits / 8);
        if (o + (uint32_t)(v->bits / 8) > b->bytes) return 0;
        if (v->bits == 16) return (int16_t)(p[o] | p[o + 1] << 8) / 32768.0f;
        if (v->bits == 8) return (p[o] - 128) / 128.0f;
        if (v->bits == 24) return (int32_t)((uint32_t)p[o] << 8 | (uint32_t)p[o + 1] << 16 | (uint32_t)p[o + 2] << 24) / 2147483648.0f;
        if (v->bits == 32) { int32_t x; memcpy(&x, p + o, 4); return x / 2147483648.0f; }
        return 0; }
    case 3: { uint32_t o = f * (uint32_t)v->align + 4u * (uint32_t)c; if (o + 4 > b->bytes) return 0; float x; memcpy(&x, p + o, 4); return x; }
    case 2: return adpcm_sample(v, b, f, c);
    }
    return 0;
}
static uint32_t frames_in(voice *v, const xbuf *b) {
    if (b->wq) return b->pcm_frames;
    if (v->tag == 2) return (b->bytes / (uint32_t)v->align) * (uint32_t)v->spb;
    return v->align ? b->bytes / (uint32_t)v->align : 0;
}

/* MS-ADPCM : décodage d'un bloc complet, mis en cache (un bloc par voix) */
static const int adpt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
static const int co1[7] = { 256, 512, 0, 192, 240, 460, 392 }, co2[7] = { 0, -256, 0, 64, 0, -208, -232 };
static struct { uint32_t data, blk; int ch; int16_t s[2 * 2048]; } acache[MAXV];
static float adpcm_sample(voice *v, const xbuf *b, uint32_t f, int c) {
    uint32_t blk = f / (uint32_t)v->spb, k = f % (uint32_t)v->spb; int ch = v->ch, id = vidx(v);
    if (blk * (uint32_t)v->align + (uint32_t)v->align > b->bytes || v->spb > 2048 || ch > 2) return 0;
    if (acache[id].data != b->data || acache[id].blk != blk + 1) {
        const uint8_t *p = MEM + b->data + blk * (uint32_t)v->align; int16_t *o = acache[id].s;
        int pred[2], del[2], s1[2], s2[2];
        for (int i = 0; i < ch; i++) { pred[i] = p[i] < 7 ? p[i] : 0; }
        p += ch; for (int i = 0; i < ch; i++) { del[i] = (int16_t)(p[0] | p[1] << 8); p += 2; }
        for (int i = 0; i < ch; i++) { s1[i] = (int16_t)(p[0] | p[1] << 8); p += 2; }
        for (int i = 0; i < ch; i++) { s2[i] = (int16_t)(p[0] | p[1] << 8); p += 2; }
        for (int i = 0; i < ch; i++) { o[i] = (int16_t)s2[i]; o[ch + i] = (int16_t)s1[i]; }
        int n = 2 * ch, total = v->spb * ch, hi = 1;
        while (n < total) {
            int nib = hi ? (*p >> 4) : (*p & 15); if (!hi) p++; hi = !hi;
            int i = n % ch, sn = nib & 8 ? nib - 16 : nib;
            int pr = (s1[i] * co1[pred[i]] + s2[i] * co2[pred[i]]) / 256 + sn * del[i];
            if (pr > 32767) pr = 32767; if (pr < -32768) pr = -32768;
            s2[i] = s1[i]; s1[i] = pr; o[n++] = (int16_t)pr;
            del[i] = adpt[nib] * del[i] / 256; if (del[i] < 16) del[i] = 16;
        }
        acache[id].data = b->data; acache[id].blk = blk + 1;
    }
    return acache[id].s[k * (uint32_t)ch + (uint32_t)c] / 32768.0f;
}


/* ---------- xWMA : décodage par ffmpeg (tampon entier), caches mémoire (références) et disque ---------- */
#define MAXW 512
static struct { uint64_t h; uint32_t bytes, frames; int16_t *pcm; int refs; uint64_t use; } WC[MAXW];
static pthread_mutex_t wlk = PTHREAD_MUTEX_INITIALIZER; static uint64_t wclock; static size_t wtotal;
static uint64_t fnv(const uint8_t *p, uint32_t n, uint64_t h) { for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }
static void cache_dir(char *d, size_t n) {
    const char *x = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (x && *x) snprintf(d, n, "%s/senjin_aleste", x); else snprintf(d, n, "%s/.cache/senjin_aleste", home ? home : "/tmp");
    mkdir(d, 0755); size_t l = strlen(d); snprintf(d + l, n - l, "/xwma"); mkdir(d, 0755);
}
static int16_t *ffmpeg_decode(const voice *v, uint32_t data, uint32_t bytes, uint32_t tbl, uint32_t cnt, uint64_t h, uint32_t *frames) {
    char dir[512], in[600], out[600]; cache_dir(dir, sizeof dir);
    snprintf(out, sizeof out, "%s/%016llx.s16", dir, (unsigned long long)h);
    FILE *f = fopen(out, "rb");
    if (!f) {
        snprintf(in, sizeof in, "%s/%016llx.xwma", dir, (unsigned long long)h);
        FILE *w = fopen(in, "wb"); if (!w) return NULL;
        uint32_t fl = (uint32_t)v->wfx_len, riff = 4 + 8 + fl + 8 + 4 * cnt + 8 + bytes;
        fwrite("RIFF", 1, 4, w); fwrite(&riff, 4, 1, w); fwrite("XWMAfmt ", 1, 8, w); fwrite(&fl, 4, 1, w); fwrite(v->wfx, 1, fl, w);
        uint32_t dn = 4 * cnt; fwrite("dpds", 1, 4, w); fwrite(&dn, 4, 1, w); fwrite(MEM + tbl, 4, cnt, w);
        fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(MEM + data, 1, bytes, w); fclose(w);
        char tmp[620]; snprintf(tmp, sizeof tmp, "%s.part", out);
        char *argv[] = { "ffmpeg", "-v", "error", "-nostdin", "-y", "-f", "xwma", "-i", in, "-f", "s16le", "-acodec", "pcm_s16le", tmp, NULL };
        extern char **environ; pid_t pid; int st = -1;
        if (posix_spawnp(&pid, "ffmpeg", NULL, NULL, argv, environ) == 0) waitpid(pid, &st, 0);
        remove(in);
        if (st != 0) { static int warned; if (!warned++) sa_log("XAudio2 : décodage xWMA impossible (ffmpeg absent ?) : son muet"); remove(tmp); return NULL; }
        rename(tmp, out); f = fopen(out, "rb"); if (!f) return NULL;
    }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    int16_t *pcm = malloc((size_t)n + 2); if (!pcm) { fclose(f); return NULL; }
    if (fread(pcm, 1, (size_t)n, f) != (size_t)n) {} fclose(f);
    *frames = (uint32_t)(n / (2 * v->ch)); return pcm;
}
static int wma_get(const voice *v, uint32_t data, uint32_t bytes, uint32_t tbl, uint32_t cnt) {   /* -> index cache (référence prise) */
    uint64_t h = fnv(MEM + data, bytes, fnv(v->wfx, (uint32_t)v->wfx_len, 1469598103934665603ull));
    pthread_mutex_lock(&wlk);
    for (int i = 0; i < MAXW; i++) if (WC[i].pcm && WC[i].h == h && WC[i].bytes == bytes) { WC[i].refs++; WC[i].use = ++wclock; pthread_mutex_unlock(&wlk); return i; }
    pthread_mutex_unlock(&wlk);
    uint32_t fr = 0; int16_t *pcm = ffmpeg_decode(v, data, bytes, tbl, cnt, h, &fr);
    if (!pcm) { fr = cnt ? rd32(tbl + 4 * (cnt - 1)) / (uint32_t)(2 * v->ch) : 0; pcm = calloc((size_t)fr * (size_t)v->ch + 1, 2); }
    pthread_mutex_lock(&wlk);
    while (wtotal > (512u << 20)) {                 /* éviction LRU des entrées non référencées */
        int o = -1; for (int i = 0; i < MAXW; i++) if (WC[i].pcm && !WC[i].refs && (o < 0 || WC[i].use < WC[o].use)) o = i;
        if (o < 0) break; wtotal -= (size_t)WC[o].frames * 4; free(WC[o].pcm); WC[o].pcm = NULL;
    }
    int k = -1; for (int i = 0; i < MAXW; i++) if (!WC[i].pcm) { k = i; break; }
    if (k < 0) { pthread_mutex_unlock(&wlk); free(pcm); return -1; }
    WC[k].h = h; WC[k].bytes = bytes; WC[k].frames = fr; WC[k].pcm = pcm; WC[k].refs = 1; WC[k].use = ++wclock; wtotal += (size_t)fr * 4;
    pthread_mutex_unlock(&wlk); return k;
}
static void wma_put(xbuf *b) { if (!b->wq) return; pthread_mutex_lock(&wlk); int k = b->wq - 1; if (k >= 0 && k < MAXW && WC[k].refs > 0) WC[k].refs--; pthread_mutex_unlock(&wlk); b->wq = 0; }

/* ---------- mixage ---------- */
typedef struct { uint32_t cb; int slot; uint32_t arg; } ev;
static float mixbuf[QUANTUM * 2];
static ev pend[MAXV * 4]; static int npend;   /* OnBufferEnd des tampons vidés par FlushSourceBuffers (rendus au passage suivant) */
static float dest_gain(int d) { float g = 1; for (int hop = 0; d >= 0 && hop < 8; hop++) { g *= V[d].vol; d = V[d].dest; } return g; }
static void mix_voice(voice *v, ev *evs, int *nev, int maxev) {
    double step = (double)v->ratio * v->rate / OUT_RATE;
    float g = v->vol * dest_gain(v->dest) * (master >= 0 ? V[master].vol : 1);
    float m[16];
    if (v->has_mat) memcpy(m, v->mat, sizeof m);
    else { memset(m, 0, sizeof m); if (v->ch == 1) { m[0] = m[1] = 1; } else { for (int c = 0; c < v->ch && c < 8; c++) { m[2 * c] = (c % 2 == 0) ? 1 : 0; m[2 * c + 1] = (c % 2 == 1) ? 1 : 0; if (c >= 2) m[2 * c] = m[2 * c + 1] = 0.5f; } } }
#define EV(slot_, arg_) do { if (v->cb && *nev < maxev) { evs[*nev].cb = v->cb; evs[*nev].slot = (slot_); evs[*nev].arg = (arg_); (*nev)++; } } while (0)
    for (int i = 0; i < QUANTUM && v->qn; i++) {
        xbuf *b = &v->q[v->qh];
        uint32_t nfr = frames_in(v, b), beg = b->pbeg, end = b->plen ? b->pbeg + b->plen : nfr; if (end > nfr) end = nfr;
        if (!b->started) { b->started = 1; v->pos = beg; EV(3, b->ctx); }
        uint32_t lend = b->lcount ? (b->llen ? b->lbeg + b->llen : end) : end;
        if (v->pos >= (b->lcount && b->loops < b->lcount ? lend : end)) {
            if (b->lcount && (b->loops < b->lcount || b->lcount == 255) && lend > b->lbeg) {
                v->pos -= lend - b->lbeg; if (b->lcount != 255) b->loops++; EV(5, b->ctx); i--; continue;
            }
            EV(4, b->ctx); if (b->flags & 0x40) EV(2, 0);
            double over = v->pos - end; wma_put(b); v->qh = (v->qh + 1) % MAXQ; v->qn--;
            if (v->qn) { xbuf *nb = &v->q[v->qh]; nb->started = 1; v->pos = nb->pbeg + (over > 0 ? over : 0); EV(3, nb->ctx); }
            i--; continue;
        }
        uint32_t f0 = (uint32_t)v->pos; float fr = (float)(v->pos - f0);
        uint32_t f1 = f0 + 1 < end ? f0 + 1 : f0;
        for (int c = 0; c < v->ch && c < 8; c++) {
            float s = sample(v, b, f0, c); if (fr > 0 && f1 != f0) s += (sample(v, b, f1, c) - s) * fr;
            s *= g * v->chvol[c];
            mixbuf[2 * i] += s * m[2 * c]; mixbuf[2 * i + 1] += s * m[2 * c + 1];
        }
        v->pos += step; v->played++;
    }
#undef EV
}
static void mix_pass(void) {
    static ev evs[MAXV * 8]; int nev = 0;
    memset(mixbuf, 0, sizeof mixbuf);
    pthread_mutex_lock(&cblk);
    if (engine_cb[0]) for (int k = 0; k < 4; k++) if (engine_cb[k]) vcb(engine_cb[k], 0, 0, 0, 0);
    /* OnVoiceProcessingPassStart(octets requis) */
    for (int i = 0; i < MAXV; i++) {
        pthread_mutex_lock(&lk); voice *v = &V[i]; uint32_t cb = 0, need = 0;
        if (v->used && v->kind == 3 && v->playing && v->cb && engine_running) {
            double step = (double)v->ratio * v->rate / OUT_RATE; double want = step * QUANTUM, have = 0;
            for (int k = 0; k < v->qn; k++) { xbuf *b = &v->q[(v->qh + k) % MAXQ]; uint32_t nfr = frames_in(v, b); uint32_t e = b->plen ? b->pbeg + b->plen : nfr; have += (double)e - (k == 0 && b->started ? v->pos : b->pbeg); }
            need = want > have ? (uint32_t)ceil(want - have) * (uint32_t)(v->tag == 2 ? 2 * v->ch : v->align) : 0;
            cb = v->cb;
        }
        pthread_mutex_unlock(&lk);
        if (cb) vcb(cb, 0, 1, need, 0);
    }
    pthread_mutex_lock(&lk);
    for (int k = 0; k < npend && nev < MAXV * 8; k++) evs[nev++] = pend[k]; npend = 0;
    if (engine_running) for (int i = 0; i < MAXV; i++) { voice *v = &V[i]; if (v->used && v->kind == 3 && v->playing && !v->destroyed) mix_voice(v, evs, &nev, MAXV * 8); }
    pthread_mutex_unlock(&lk);
    for (int k = 0; k < nev; k++) vcb(evs[k].cb, evs[k].slot, evs[k].slot == 2 ? 0 : 1, evs[k].arg, 0);
    for (int i = 0; i < MAXV; i++) { pthread_mutex_lock(&lk); uint32_t cb = (V[i].used && V[i].kind == 3 && V[i].playing) ? V[i].cb : 0; pthread_mutex_unlock(&lk); if (cb) vcb(cb, 1, 0, 0, 0); }
    if (engine_cb[0]) for (int k = 0; k < 4; k++) if (engine_cb[k]) vcb(engine_cb[k], 1, 0, 0, 0);
    pthread_mutex_unlock(&cblk);
    float pk = 0; for (int i = 0; i < QUANTUM * 2; i++) { float x = mixbuf[i]; if (fabsf(x) > pk) pk = fabsf(x); mixbuf[i] = x > 1 ? 1 : x < -1 ? -1 : x; }
    static int heard; if (!heard && pk > 0.01f) { heard = 1; sa_log("XAudio2 natif : premier son mixé (crête %.2f)", pk); }
    static float pmax; static int np; if (pk > pmax) pmax = pk; if (getenv("SA_AUDIO_LOG") && ++np % 500 == 0) { sa_log("XAudio2 : crête 5 s = %.3f", pmax); pmax = 0; }
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void *mixer_main(void *arg) {
    (void)arg; memset(&cpu, 0, sizeof cpu);
    thread_setup_current(thread_alloc_slot()); wr32(cpu.fs_base + 0x24, 3000);
    double t0 = now_s(); uint64_t done = 0;
    for (;;) {
        if (dev) {
            if (SDL_GetQueuedAudioSize(dev) > 4 * QUANTUM * 2 * sizeof(float)) { usleep(2000); continue; }
            mix_pass(); SDL_QueueAudio(dev, mixbuf, sizeof mixbuf);
        } else {
            if ((double)done / OUT_RATE > now_s() - t0) { usleep(2000); continue; }
            mix_pass(); done += QUANTUM;
        }
    }
    return NULL;
}
static void start_output(void) {
    if (mixer_on) return; mixer_on = 1;
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&cblk, &a);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want = { 0 }, have; want.freq = OUT_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 512;
        dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (dev) SDL_PauseAudioDevice(dev, 0);
    }
    sa_log("XAudio2 natif : sortie %s", dev ? SDL_GetCurrentAudioDriver() : "aucune (mixage cadencé sur l'horloge)");
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8u << 20);
    pthread_create(&mixer, &at, mixer_main, NULL);
}

/* ---------- IXAudio2 ---------- */
static void X_QI(void) { wr32(ARG(2), ARG(0)); RET_STD(3, 0); }
static void X_AddRef(void) { RET_STD(1, 1); }
static void X_Release(void) { RET_STD(1, 0); }
static void X_GetDeviceCount(void) { wr32(ARG(1), 1); RET_STD(2, 0); }
static void X_GetDeviceDetails(void) {
    uint32_t p = ARG(2); memset(MEM + p, 0, 1068);
    const char *n = "Senjin Aleste (SDL)"; for (int i = 0; n[i]; i++) { wr16(p + 2 * (uint32_t)i, (uint16_t)n[i]); wr16(p + 512 + 2 * (uint32_t)i, (uint16_t)n[i]); }
    wr32(p + 1024, 0xF);                                   /* GlobalDefaultDevice */
    uint32_t w = p + 1028; wr16(w, 0xFFFE); wr16(w + 2, 2); wr32(w + 4, OUT_RATE); wr32(w + 8, OUT_RATE * 8); wr16(w + 12, 8); wr16(w + 14, 32); wr16(w + 16, 22);
    wr16(w + 18, 32); wr32(w + 20, 3); wr32(w + 24, 3); wr16(w + 28, 0); wr16(w + 30, 0x10); /* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT */
    wr32(w + 32, 0xAA000080u); wr32(w + 36, 0x719B3800u);
    RET_STD(3, 0);
}
static void X_Initialize(void) { RET_STD(3, 0); }
static void X_RegisterForCallbacks(void) { for (int k = 0; k < 4; k++) if (!engine_cb[k]) { engine_cb[k] = ARG(1); break; } RET_STD(2, 0); }
static void X_UnregisterForCallbacks(void) { for (int k = 0; k < 4; k++) if (engine_cb[k] == ARG(1)) engine_cb[k] = 0; RET_STD(2, 0); }
static int read_format(voice *v, uint32_t wf) {
    v->tag = rd16(wf); v->ch = rd16(wf + 2); v->rate = (int)rd32(wf + 4); v->align = rd16(wf + 12); v->bits = rd16(wf + 14);
    if (v->tag == 0xFFFE) { uint32_t sub = rd32(wf + 24); v->tag = (int)sub; }
    if (v->tag == 0x161 || v->tag == 0x162) {      /* xWMA : décodé en PCM 16 bits */
        v->wfx_len = 18 + rd16(wf + 16); if (v->wfx_len > 64) v->wfx_len = 64; memcpy(v->wfx, MEM + wf, (size_t)v->wfx_len);
        v->wma = 1; v->tag = 1; v->bits = 16; v->align = 2 * v->ch; return v->ch >= 1 && v->ch <= 8;
    }
    if (v->tag == 2) { v->spb = rd16(wf + 18); if (!v->spb) v->spb = (v->align - 7 * v->ch) * 8 / (4 * v->ch) + 2; }
    if (v->ch < 1 || v->ch > 8 || v->rate <= 0 || (v->tag != 1 && v->tag != 2 && v->tag != 3)) return 0;
    if (!v->align) v->align = v->ch * v->bits / 8;
    return 1;
}
static void apply_sends(voice *v, uint32_t sends) {
    if (!sends || !rd32(sends)) return;
    uint32_t d = rd32(sends + 4); int k = voice_of_obj(rd32(d + 4)); if (k >= 0 && V[k].kind == 2) v->dest = k;
}
static void X_CreateSourceVoice(void) {
    uint32_t pp = ARG(1), wf = ARG(2), cb = ARG(5), sends = ARG(6);
    pthread_mutex_lock(&lk);
    int i = new_voice(3, vt_source);
    if (i < 0) { pthread_mutex_unlock(&lk); RET_STD(8, 0x8007000Eu); return; }
    voice *v = &V[i]; v->cb = cb;
    if (!read_format(v, wf)) { sa_log("XAudio2 : format non géré tag=%u ch=%u %u Hz %u bits", rd16(wf), rd16(wf + 2), rd32(wf + 4), rd16(wf + 14)); v->used = 0; pthread_mutex_unlock(&lk); RET_STD(8, 0x88960001u); return; }
    apply_sends(v, sends);
    static int nlog; if (sa_verbose && nlog++ < 16) sa_log("XAudio2 : voix source %d tag=%d %d ch %d Hz %d bits rappel=%08x", i, v->tag, v->ch, v->rate, v->bits, cb);
    wr32(pp, v->gobj); pthread_mutex_unlock(&lk); RET_STD(8, 0);
}
static void X_CreateSubmixVoice(void) {
    pthread_mutex_lock(&lk); int i = new_voice(2, vt_submix);
    if (i >= 0) { V[i].ch = (int)ARG(2); V[i].rate = (int)ARG(3); apply_sends(&V[i], ARG(6)); wr32(ARG(1), V[i].gobj); }
    pthread_mutex_unlock(&lk); RET_STD(8, i >= 0 ? 0 : 0x8007000Eu);
}
static void X_CreateMasteringVoice(void) {
    pthread_mutex_lock(&lk); int i = new_voice(1, vt_master);
    if (i >= 0) { V[i].ch = 2; V[i].rate = OUT_RATE; master = i; wr32(ARG(1), V[i].gobj); }
    pthread_mutex_unlock(&lk); start_output(); RET_STD(7, i >= 0 ? 0 : 0x8007000Eu);
}
static void X_StartEngine(void) { engine_running = 1; RET_STD(1, 0); }
static void X_StopEngine(void) { engine_running = 0; RET_STD(1, 0); }
static void X_CommitChanges(void) { RET_STD(2, 0); }
static void X_GetPerformanceData(void) { memset(MEM + ARG(1), 0, 64); RET_STD(2, 0); }
static void X_SetDebugConfiguration(void) { RET_STD(3, 0); }

/* ---------- IXAudio2Voice ---------- */
static void V_GetVoiceDetails(void) { voice *v = me(); uint32_t p = ARG(1); wr32(p, 0); wr32(p + 4, v ? (uint32_t)v->ch : 2); wr32(p + 8, v ? (uint32_t)v->rate : OUT_RATE); RET_STD(2, 0); }
static void V_SetOutputVoices(void) { pthread_mutex_lock(&lk); voice *v = me(); if (v) { v->dest = -1; apply_sends(v, ARG(1)); } pthread_mutex_unlock(&lk); RET_STD(2, 0); }
static void V_ok2(void) { RET_STD(2, 0); } static void V_ok3(void) { RET_STD(3, 0); } static void V_ok4(void) { RET_STD(4, 0); }
static void V_ok5(void) { RET_STD(5, 0); }
static void V_GetEffectState(void) { if (ARG(2)) wr32(ARG(2), 0); RET_STD(3, 0); }
static void V_GetFilterParameters(void) { uint32_t p = ARG(1); wr32(p, 0); wr32(p + 4, u32f(1)); wr32(p + 8, u32f(1)); RET_STD(2, 0); }
static void V_GetOutputFilterParameters(void) { uint32_t p = ARG(2); wr32(p, 0); wr32(p + 4, u32f(1)); wr32(p + 8, u32f(1)); RET_STD(3, 0); }
static void V_SetVolume(void) { voice *v = me(); if (v) v->vol = f32(ARG(1)); RET_STD(3, 0); }
static void V_GetVolume(void) { voice *v = me(); wr32(ARG(1), u32f(v ? v->vol : 1)); RET_STD(2, 0); }
static void V_SetChannelVolumes(void) { voice *v = me(); uint32_t n = ARG(1), p = ARG(2); if (v) for (uint32_t c = 0; c < n && c < 8; c++) v->chvol[c] = f32(rd32(p + 4 * c)); RET_STD(4, 0); }
static void V_GetChannelVolumes(void) { voice *v = me(); uint32_t n = ARG(1), p = ARG(2); for (uint32_t c = 0; c < n; c++) wr32(p + 4 * c, u32f(v && c < 8 ? v->chvol[c] : 1)); RET_STD(3, 0); }
static void V_SetOutputMatrix(void) {
    voice *v = me(); uint32_t sc = ARG(2), dc = ARG(3), p = ARG(4);
    if (getenv("SA_AUDIO_LOG")) sa_log("XAudio2 : matrice %ux%u : %.3f %.3f", sc, dc, f32(rd32(p)), dc > 1 ? f32(rd32(p + 4)) : 0.0f);
    if (v && dc >= 1 && sc <= 8) { memset(v->mat, 0, sizeof v->mat);
        for (uint32_t s = 0; s < sc; s++) { v->mat[2 * s] = f32(rd32(p + 4 * (s * dc + 0))); v->mat[2 * s + 1] = dc > 1 ? f32(rd32(p + 4 * (s * dc + 1))) : v->mat[2 * s]; }
        v->has_mat = v->kind == 3; }
    RET_STD(6, 0);
}
static void V_GetOutputMatrix(void) { voice *v = me(); uint32_t sc = ARG(2), dc = ARG(3), p = ARG(4); for (uint32_t s = 0; s < sc; s++) for (uint32_t d = 0; d < dc; d++) wr32(p + 4 * (s * dc + d), u32f(v && v->has_mat && s < 8 && d < 2 ? v->mat[2 * s + d] : (s == d ? 1 : 0))); RET_STD(5, 0); }
static void V_DestroyVoice(void) {
    uint32_t o = ARG(0);
    pthread_mutex_lock(&cblk); pthread_mutex_lock(&lk);
    int i = voice_of_obj(o);
    if (i >= 0) { for (int k = 0; k < V[i].qn; k++) wma_put(&V[i].q[(V[i].qh + k) % MAXQ]); if (i == master) master = -1; V[i].used = 0; wr32(o + 8, 0); acache[i].data = 0; }
    pthread_mutex_unlock(&lk); pthread_mutex_unlock(&cblk);
    if (i >= 0) g_free(o);
    RET_STD(1, 0);
}
/* ---------- IXAudio2SourceVoice ---------- */
static void S_Start(void) { voice *v = me(); if (v) v->playing = 1; RET_STD(3, 0); }
static void S_Stop(void) { voice *v = me(); if (v) v->playing = 0; RET_STD(3, 0); }
static void S_SubmitSourceBuffer(void) {
    uint32_t p = ARG(1), pw = ARG(2); int wq = 0; const int16_t *pcm = NULL; uint32_t pf = 0;
    pthread_mutex_lock(&lk); voice *v = me(); int wma = v ? v->wma : 0; voice tmp; if (v) tmp = *v; pthread_mutex_unlock(&lk);
    if (wma && pw) {                                /* décodage hors verrou (peut prendre du temps la première fois) */
        int k = wma_get(&tmp, rd32(p + 8), rd32(p + 4), rd32(pw), rd32(pw + 4));
        if (k >= 0) { wq = k + 1; pcm = WC[k].pcm; pf = WC[k].frames; }
    }
    pthread_mutex_lock(&lk); v = me();
    if (!v || v->qn >= MAXQ) { pthread_mutex_unlock(&lk); if (wq) { xbuf t = { 0 }; t.wq = wq; wma_put(&t); } RET_STD(3, 0x88960001u); return; }
    xbuf *b = &v->q[(v->qh + v->qn) % MAXQ]; memset(b, 0, sizeof *b);
    b->flags = rd32(p); b->bytes = rd32(p + 4); b->data = rd32(p + 8); b->pbeg = rd32(p + 12); b->plen = rd32(p + 16);
    b->lbeg = rd32(p + 20); b->llen = rd32(p + 24); b->lcount = rd32(p + 28); b->ctx = rd32(p + 32);
    b->wq = wq; b->pcm = pcm; b->pcm_frames = pf;
    if (v->wma && !wq) { b->wq = 0; b->pcm = NULL; b->bytes = 0; }   /* rien à jouer */
    v->qn++; pthread_mutex_unlock(&lk); RET_STD(3, 0);
}
static void S_FlushSourceBuffers(void) {
    pthread_mutex_lock(&lk); voice *v = me();
    if (v) { int keep = (v->playing && v->qn && v->q[v->qh].started) ? 1 : 0;     /* le tampon en cours reste */
        for (int k = keep; k < v->qn; k++) { xbuf *b = &v->q[(v->qh + k) % MAXQ]; if (v->cb && npend < MAXV * 4) { pend[npend].cb = v->cb; pend[npend].slot = 4; pend[npend].arg = b->ctx; npend++; } wma_put(b); }
        v->qn = keep; }
    pthread_mutex_unlock(&lk);
    RET_STD(1, 0);
}
static void S_Discontinuity(void) { voice *v = me(); if (v && v->qn) v->q[(v->qh + v->qn - 1) % MAXQ].flags |= 0x40; RET_STD(1, 0); }
static void S_ExitLoop(void) { pthread_mutex_lock(&lk); voice *v = me(); if (v && v->qn) v->q[v->qh].lcount = v->q[v->qh].loops; pthread_mutex_unlock(&lk); RET_STD(2, 0); }
static void S_GetState(void) {
    pthread_mutex_lock(&lk); voice *v = me(); uint32_t p = ARG(1);
    wr32(p, v && v->qn ? v->q[v->qh].ctx : 0); wr32(p + 4, v ? (uint32_t)v->qn : 0);
    uint64_t pl = v ? v->played : 0; wr32(p + 8, (uint32_t)pl); wr32(p + 12, (uint32_t)(pl >> 32));
    pthread_mutex_unlock(&lk); RET_STD(2, 0);
}
static void S_SetFrequencyRatio(void) { voice *v = me(); float r = f32(ARG(1)); if (v && r > 0 && r < 1024) v->ratio = r; RET_STD(3, 0); }
static void S_GetFrequencyRatio(void) { voice *v = me(); wr32(ARG(1), u32f(v ? v->ratio : 1)); RET_STD(2, 0); }

/* ---------- vtables, CoCreateInstance ---------- */
typedef struct { const char *name; void (*fn)(void); } ent;
static uint32_t build_vt(const char *iface, const ent *e, int n) {
    uint32_t vt = g_static(4 * (uint32_t)n);
    for (int i = 0; i < n; i++) {
        char full[128]; snprintf(full, sizeof full, "XAUDIO2!%s::%s", iface, e[i].name);
        imp_def d[2] = { { strdup(full), e[i].fn }, { NULL, NULL } }; imp_register(d);
        wr32(vt + 4 * (uint32_t)i, imp_magic_for(full));
    }
    return vt;
}
#define VOICE_METHODS \
    { "GetVoiceDetails", V_GetVoiceDetails }, { "SetOutputVoices", V_SetOutputVoices }, { "SetEffectChain", V_ok2 }, { "EnableEffect", V_ok3 }, \
    { "DisableEffect", V_ok3 }, { "GetEffectState", V_GetEffectState }, { "SetEffectParameters", V_ok5 }, { "GetEffectParameters", V_ok4 }, \
    { "SetFilterParameters", V_ok3 }, { "GetFilterParameters", V_GetFilterParameters }, { "SetOutputFilterParameters", V_ok4 }, \
    { "GetOutputFilterParameters", V_GetOutputFilterParameters }, { "SetVolume", V_SetVolume }, { "GetVolume", V_GetVolume }, \
    { "SetChannelVolumes", V_SetChannelVolumes }, { "GetChannelVolumes", V_GetChannelVolumes }, { "SetOutputMatrix", V_SetOutputMatrix }, \
    { "GetOutputMatrix", V_GetOutputMatrix }, { "DestroyVoice", V_DestroyVoice }
static void build(void) {
    if (vt_engine) return;
    static const ent xe[] = { { "QueryInterface", X_QI }, { "AddRef", X_AddRef }, { "Release", X_Release }, { "GetDeviceCount", X_GetDeviceCount },
        { "GetDeviceDetails", X_GetDeviceDetails }, { "Initialize", X_Initialize }, { "RegisterForCallbacks", X_RegisterForCallbacks },
        { "UnregisterForCallbacks", X_UnregisterForCallbacks }, { "CreateSourceVoice", X_CreateSourceVoice }, { "CreateSubmixVoice", X_CreateSubmixVoice },
        { "CreateMasteringVoice", X_CreateMasteringVoice }, { "StartEngine", X_StartEngine }, { "StopEngine", X_StopEngine },
        { "CommitChanges", X_CommitChanges }, { "GetPerformanceData", X_GetPerformanceData }, { "SetDebugConfiguration", X_SetDebugConfiguration } };
    static const ent vm[] = { VOICE_METHODS };
    static const ent vs[] = { VOICE_METHODS, { "Start", S_Start }, { "Stop", S_Stop }, { "SubmitSourceBuffer", S_SubmitSourceBuffer },
        { "FlushSourceBuffers", S_FlushSourceBuffers }, { "Discontinuity", S_Discontinuity }, { "ExitLoop", S_ExitLoop }, { "GetState", S_GetState },
        { "SetFrequencyRatio", S_SetFrequencyRatio }, { "GetFrequencyRatio", S_GetFrequencyRatio } };
    vt_engine = build_vt("IXAudio2", xe, 16);
    vt_master = build_vt("IXAudio2MasteringVoice", vm, 19);
    vt_submix = build_vt("IXAudio2SubmixVoice", vm, 19);
    vt_source = build_vt("IXAudio2SourceVoice", vs, 28);
}
/* appelé par CoCreateInstance : 1 si la classe est XAudio2 2.7 (objet écrit dans *ppv) */
int xaudio2_create(uint32_t clsid, uint32_t ppv) {
    if (rd32(clsid) != 0x5a508685u) return 0;
    build();
    if (!engine_obj) { engine_obj = g_alloc(16, 1); wr32(engine_obj, vt_engine); wr32(engine_obj + 8, OBJ_MAGIC); }
    wr32(ppv, engine_obj);
    return 1;
}
