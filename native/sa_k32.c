/* sa_k32.c - KERNEL32 rewritten natively for the recompiled code: process, memory, strings,
 * synchronization, threads, files, time. Everything is host code; no Windows DLL is loaded. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <dirent.h>
#include <fnmatch.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "sa_host.h"

/* ================= handle objects (HANDLE) ================= */
enum { H_FREE, H_FILE, H_EVENT, H_MUTEX, H_SEM, H_THREAD, H_FIND, H_HEAP, H_MODULE };
typedef struct {
    int type;
    pthread_mutex_t m; pthread_cond_t c;
    int manual, signaled; int count, max;      /* event / semaphore */
    pthread_t owner; int owner_set, rec;       /* mutex */
    FILE *fp;                                  /* file */
    DIR *dir; char pat[256], dpath[1024];      /* search */
    uint32_t start, param, tid; int done; uint32_t exitcode; pthread_t th;   /* thread */
} hobj;
#define HBASE 0x100u
#define HMAX 4096
static hobj *H[HMAX];
/* closed objects are recycled, never freed: their mutex/cond stay initialized, so a thread that still holds a
 * stale pointer (wait in progress, racing SetEvent) never touches freed memory */
static hobj *recyc[HMAX]; static unsigned nrecyc;
static pthread_mutex_t hlock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint32_t last_error;

static uint32_t h_new(int type) {
    pthread_mutex_lock(&hlock);
    for (unsigned i = 1; i < HMAX; i++) if (!H[i]) {
        hobj *o;
        if (nrecyc) {                             /* reuse a closed object: reset its state, keep m/c */
            o = recyc[--nrecyc];
            pthread_mutex_lock(&o->m);
            o->manual = o->signaled = o->count = o->max = 0; o->owner_set = o->rec = 0;
            o->fp = NULL; o->dir = NULL; o->pat[0] = o->dpath[0] = 0;
            o->start = o->param = o->tid = 0; o->done = 0; o->exitcode = 0;
            o->type = type;
            pthread_mutex_unlock(&o->m);
        } else {
            o = calloc(1, sizeof *o); o->type = type;
            pthread_mutex_init(&o->m, NULL); pthread_cond_init(&o->c, NULL);
        }
        H[i] = o; pthread_mutex_unlock(&hlock); return HBASE + 4 * i;
    }
    pthread_mutex_unlock(&hlock); sa_fatal("no more free handles");
    return 0;
}
static hobj *h_get(uint32_t h) {
    if (h < HBASE || (h - HBASE) % 4) return NULL;
    unsigned i = (h - HBASE) / 4; return i < HMAX ? H[i] : NULL;
}
/* closes a handle: releases the host resources and frees its slot. Thread objects are kept (thread_main
 * still writes to them, and other threads may still wait on them); heap/module handles are not slots. */
static int h_close(uint32_t h) {
    if (h < HBASE || (h - HBASE) % 4) return 0;
    unsigned i = (h - HBASE) / 4; if (i >= HMAX) return 0;
    pthread_mutex_lock(&hlock);
    hobj *o = H[i];
    if (!o) { pthread_mutex_unlock(&hlock); return 0; }
    if (o->type != H_FILE && o->type != H_FIND && o->type != H_EVENT && o->type != H_MUTEX && o->type != H_SEM) {
        pthread_mutex_unlock(&hlock); return 1;
    }
    pthread_mutex_lock(&o->m);
    if (o->fp) { fclose(o->fp); o->fp = NULL; }
    if (o->dir) { closedir(o->dir); o->dir = NULL; }
    o->type = H_FREE;
    pthread_mutex_unlock(&o->m);
    H[i] = NULL; recyc[nrecyc++] = o;             /* nrecyc < HMAX: each recycled object came from a slot */
    pthread_mutex_unlock(&hlock); return 1;
}

/* ================= paths: the game uses Windows paths relative to the game folder ================= */
static void host_path(const char *w, char *out, int n) {
    int k = 0;
    if (w[0] && w[1] == ':') w += 2;            /* the cabinet's "W:\" -> current folder */
    if (*w == '\\' || *w == '/') { w++; }
    for (; *w && k < n - 1; w++) out[k++] = (*w == '\\') ? '/' : *w;
    out[k] = 0;
    /* case insensitivity: kept as is; the game's files are lowercase */
}

/* ================= process / misc ================= */
static void GetLastError_(void) { RET_STD(0, last_error); }
static void SetLastError_(void) { last_error = ARG(0); RET_STD(1, 0); }
static void GetCurrentProcess_(void) { RET_STD(0, 0xFFFFFFFFu); }
static void GetCurrentThread_(void) { RET_STD(0, 0xFFFFFFFEu); }
static void GetCurrentProcessId_(void) { RET_STD(0, 4242); }
static void GetCurrentThreadId_(void) { RET_STD(0, rd32(cpu.fs_base + 0x24)); }
static void IsDebuggerPresent_(void) { RET_STD(0, 0); }
static void IsProcessorFeaturePresent_(void) {
    uint32_t f = ARG(0);   /* 10 = SSE2, 6 = SSE, 13 = SSE3, 17 = NX, 23 = __fastfail */
    RET_STD(1, f == 6 || f == 10 || f == 13 || f == 17 || f == 23 || f == 3);
}
static void SetUnhandledExceptionFilter_(void) { RET_STD(1, 0); }
static void UnhandledExceptionFilter_(void) { RET_STD(1, 0); }
static void InitializeSListHead_(void) { wr32(ARG(0), 0); wr32(ARG(0) + 4, 0); RET_STD(1, 0); }
static void EncodePointer_(void) { RET_STD(1, ARG(0)); }
static void DecodePointer_(void) { RET_STD(1, ARG(0)); }
static void OutputDebugStringW_(void) { char b[512]; g_wstr_to_utf8(ARG(0), b, sizeof b); if (sa_verbose) sa_log("[game] %s", b); RET_STD(1, 0); }
static void ExitProcess_(void) { sa_log("ExitProcess(%u)", ARG(0)); fflush(NULL); exit((int)ARG(0)); }
static void TerminateProcess_(void) { sa_log("TerminateProcess(%u)", ARG(1)); fflush(NULL); exit((int)ARG(1)); }
static void GetStartupInfoW_(void) { uint32_t p = ARG(0); memset(MEM + p, 0, 68); wr32(p, 68); RET_STD(1, 0); }
static void GetStdHandle_(void) { RET_STD(1, 0); }
static void GetFileType_(void) { hobj *o = h_get(ARG(0)); RET_STD(1, o && o->fp ? 1 : 2); }   /* DISK for files, CHAR for the console */
/* "system" modules: dummy handle (GetProcAddress then looks in our native imports) */
static uint32_t module_for(const char *n) {
    static const char *known[] = { "kernel32", "kernelbase", "api-ms-win-core", "user32", "ntdll", "advapi32", NULL };
    for (int i = 0; known[i]; i++) if (!strncasecmp(n, known[i], strlen(known[i]))) return 0x7F800000u + 0x10000u * (uint32_t)i;
    return 0;
}
static void GetModuleHandleW_(void) { char b[256]; if (!ARG(0)) { RET_STD(1, pe_image_base); return; } g_wstr_to_utf8(ARG(0), b, sizeof b); RET_STD(1, module_for(b)); }
static void GetModuleHandleA_(void) { if (!ARG(0)) { RET_STD(1, pe_image_base); return; } RET_STD(1, module_for(g_str(ARG(0)))); }
static void GetModuleHandleExW_(void) { if (ARG(2)) wr32(ARG(2), pe_image_base); RET_STD(3, 1); }
static void GetModuleFileNameW_(void) {
    static uint32_t w; if (!w) w = g_wstrdup("W:\\senxin_aleste_apm3.exe");   /* allocated once */
    uint32_t buf = ARG(1), n = ARG(2); uint32_t k = 0;
    while (k + 1 < n && rd16(w + 2 * k)) { wr16(buf + 2 * k, rd16(w + 2 * k)); k++; }
    wr16(buf + 2 * k, 0); RET_STD(3, k);
}
static void GetModuleFileNameA_(void) {
    const char *s = "W:\\senxin_aleste_apm3.exe"; uint32_t buf = ARG(1), n = ARG(2), k = 0;
    while (k + 1 < n && s[k]) { wr8(buf + k, s[k]); k++; } wr8(buf + k, 0); RET_STD(3, k);
}
static void GetProcAddress_(void) {
    uint32_t name = ARG(1); char b[128];
    if (name < 0x10000) snprintf(b, sizeof b, "?!Ordinal_%u", name); else snprintf(b, sizeof b, "?!%s", g_str(name));
    /* look the name up in any DLL: the import table is indexed by "DLL!name" */
    static const char *dlls[] = { "KERNEL32.DLL", "USER32.DLL", "ADVAPI32.DLL", "OLE32.DLL", NULL };
    for (int i = 0; dlls[i]; i++) {
        char full[160]; snprintf(full, sizeof full, "%s!%s", dlls[i], b + 2);
        extern int imp_has_fn(const char *); if (imp_has_fn(full)) { RET_STD(2, imp_magic_for(full)); return; }
    }
    if (sa_verbose) sa_log("GetProcAddress(%s) -> NULL", b + 2);
    last_error = 127; RET_STD(2, 0);
}
static void LoadLibraryExW_(void) { char b[256]; g_wstr_to_utf8(ARG(0), b, sizeof b); if (sa_verbose) sa_log("LoadLibraryExW(%s) -> NULL", b); last_error = 126; RET_STD(3, 0); }
static void FreeLibrary_(void) { RET_STD(1, 1); }
static void GetVersionExA_(void) { uint32_t p = ARG(0); wr32(p + 4, 10); wr32(p + 8, 0); wr32(p + 12, 19045); wr32(p + 16, 2); RET_STD(1, 1); }
static void VerSetConditionMask_(void) { cpu.eax = ARG(0); cpu.edx = ARG(1); cpu.esp += 4 + 16; }
static void VerifyVersionInfoA_(void) { RET_STD(4, 1); }
static void GetSystemInfo_(void) {
    uint32_t p = ARG(0); memset(MEM + p, 0, 36);
    wr16(p, 0); wr32(p + 4, 4096); wr32(p + 8, 0x10000); wr32(p + 12, 0x7FFEFFFF); wr32(p + 16, 3); wr32(p + 20, 2);
    wr32(p + 24, 586); wr32(p + 28, 0x10000); wr16(p + 32, 6); RET_STD(1, 0);
}
static void GetNativeSystemInfo_(void) { GetSystemInfo_(); }
static void GetProcessAffinityMask_(void) { wr32(ARG(1), 3); wr32(ARG(2), 3); RET_STD(3, 1); }
static void IsWow64Process_(void) { wr32(ARG(1), 0); RET_STD(2, 1); }
static void SetThreadPriority_(void) { RET_STD(2, 1); }
static void SetThreadIdealProcessor_(void) { RET_STD(2, 0); }
static void SwitchToThread_(void) { sched_yield(); RET_STD(0, 1); }
static void Sleep_(void) { uint32_t ms = ARG(0); if (ms) usleep(ms * 1000); else sched_yield(); RET_STD(1, 0); }
static void SleepEx_(void) { uint32_t ms = ARG(0); if (ms) usleep(ms * 1000); RET_STD(2, 0); }
static void RaiseException_(void) {
    uint32_t code = ARG(0), n = ARG(2), args = ARG(3);
    if (code == 0xE06D7363u && n >= 3) {          /* MSVC C++ exception: print the type and the message */
        uint32_t objp = rd32(args + 4), ti = rd32(args + 8);
        uint32_t cta = ti ? rd32(ti + 12) : 0, ct0 = cta ? rd32(cta + 4) : 0, td = ct0 ? rd32(ct0 + 4) : 0;
        const char *tn = td ? g_str(td + 8) : "?";
        uint32_t msg = objp ? rd32(objp + 4) : 0;
        sa_log("C++ exception: type %s, object 0x%08x, message \"%s\"", tn, objp, (msg > 0x10000 && msg < 0x7F000000) ? g_str(msg) : "?");
    }
    sa_fatal("RaiseException(0x%08x): exceptions not handled yet (called from 0x%08x)", code, rd32(cpu.esp));
}
static void FormatMessageA_(void) { RET_STD(7, 0); }
static void LocalFree_(void) { RET_STD(1, 0); }
static void IsDBCSLeadByte_(void) { uint32_t c = ARG(0) & 0xFF; RET_STD(1, (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)); }
static void ExpandEnvironmentStringsA_(void) { const char *s = g_str(ARG(0)); uint32_t n = (uint32_t)strlen(s) + 1; if (ARG(2) >= n) memcpy(MEM + ARG(1), s, n); RET_STD(3, n); }
static void SetEnvironmentVariableW_(void) { RET_STD(2, 1); }

/* ================= environment / code page ================= */
static void GetEnvironmentStringsW_(void) { uint32_t p = g_static(8); RET_STD(0, p); }
static void FreeEnvironmentStringsW_(void) { RET_STD(1, 1); }
static void GetACP_(void) { RET_STD(0, 932); }    /* Japanese game: Shift-JIS */
static void GetOEMCP_(void) { RET_STD(0, 932); }
static void IsValidCodePage_(void) { RET_STD(1, 1); }
static void GetCPInfo_(void) {
    uint32_t p = ARG(1); memset(MEM + p, 0, 20);
    if (ARG(0) == 932 || ARG(0) == 0 || ARG(0) == 3) { wr32(p, 2); wr8(p + 4, '?'); uint8_t lb[] = { 0x81, 0x9F, 0xE0, 0xFC }; memcpy(MEM + p + 6, lb, 4); }
    else { wr32(p, 1); wr8(p + 4, '?'); }
    RET_STD(2, 1);
}
/* MultiByteToWideChar / WideCharToMultiByte (Shift-JIS) are implemented in sa_gdi.c */
static void GetStringTypeW_(void) {
    uint32_t type = ARG(0), src = ARG(1), out = ARG(3); int32_t n = (int32_t)ARG(2);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    for (int i = 0; i < n; i++) {
        uint16_t c = rd16(src + 2 * i), t = 0;
        if (type == 1) {
            if (c >= 'A' && c <= 'Z') t |= 1 | 0x100; if (c >= 'a' && c <= 'z') t |= 2 | 0x100;
            if (c >= '0' && c <= '9') t |= 4 | 0x80; if (c == ' ' || (c >= 9 && c <= 13)) t |= 8; if (c < 32 || c == 127) t |= 0x20;
            if (c > 32 && c < 127 && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) t |= 0x10;
            if (c == ' ' || c == '\t') t |= 0x40;
        }
        wr16(out + 2 * i, t);
    }
    RET_STD(4, 1);
}
static void LCMapStringW_(void) {
    uint32_t fl = ARG(1), src = ARG(2), dst = ARG(4), dn = ARG(5); int32_t n = (int32_t)ARG(3);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    if (!dn) { RET_STD(6, n); return; }
    for (int i = 0; i < n && (uint32_t)i < dn; i++) {
        uint16_t c = rd16(src + 2 * i);
        if ((fl & 0x100) && c >= 'A' && c <= 'Z') c += 32;     /* LCMAP_LOWERCASE */
        if ((fl & 0x200) && c >= 'a' && c <= 'z') c -= 32;     /* LCMAP_UPPERCASE */
        wr16(dst + 2 * i, c);
    }
    RET_STD(6, n);
}
static void CompareStringW_(void) {
    uint32_t a = ARG(2), b = ARG(4); int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    if (na < 0) { na = 0; while (rd16(a + 2 * na)) na++; } if (nb < 0) { nb = 0; while (rd16(b + 2 * nb)) nb++; }
    int r = 0; for (int i = 0; i < na && i < nb && !r; i++) r = (int)rd16(a + 2 * i) - (int)rd16(b + 2 * i);
    if (!r) r = na - nb; RET_STD(6, r < 0 ? 1 : r > 0 ? 3 : 2);
}
static void GetUserDefaultLCID_(void) { RET_STD(0, 0x0411); }
static void IsValidLocale_(void) { RET_STD(2, 1); }
static void GetLocaleInfoW_(void) { if (ARG(3)) { if (ARG(2)) wr16(ARG(2), 0); } RET_STD(4, 1); }
static void EnumSystemLocalesW_(void) { RET_STD(2, 1); }

/* ================= heap ================= */
static void HeapAlloc_(void) { RET_STD(3, g_alloc(ARG(2), ARG(1) & 8)); }
static void HeapFree_(void) { gh_free(ARG(2)); RET_STD(3, 1); }
static void HeapReAlloc_(void) { RET_STD(4, gh_realloc(ARG(2), ARG(3))); }
static void HeapSize_(void) { RET_STD(3, g_size(ARG(2))); }

static void GetProcessHeap_(void) { RET_STD(0, 0x00110000u); }
static void HeapCreate_(void) { RET_STD(3, 0x00110000u); }
static void HeapDestroy_(void) { RET_STD(1, 1); }
static void GetCommandLineA_(void) { static uint32_t p; if (!p) p = gh_strdup("senxin_aleste_apm3.exe -a"); RET_STD(0, p); }
static void GetCommandLineW_(void) { static uint32_t p; if (!p) p = g_wstrdup("senxin_aleste_apm3.exe -a"); RET_STD(0, p); }
static void VirtualAlloc_(void) { uint32_t p = g_alloc(ARG(1) + 4096, 1); p = (p + 4095) & ~4095u; RET_STD(4, p); }
static void VirtualFree_(void) { RET_STD(3, 1); }
static void VirtualProtect_(void) { if (ARG(3)) wr32(ARG(3), 4); RET_STD(4, 1); }

/* ================= TLS (dynamic) ================= */
static uint32_t tls_next = 1;
static void TlsAlloc_(void) { RET_STD(0, tls_next < 64 ? tls_next++ : 0xFFFFFFFFu); }
static void TlsFree_(void) { RET_STD(1, 1); }
static void TlsGetValue_(void) { last_error = 0; RET_STD(1, rd32(cpu.fs_base + 0xE10 + 4 * (ARG(0) & 63))); }
static void TlsSetValue_(void) { wr32(cpu.fs_base + 0xE10 + 4 * (ARG(0) & 63), ARG(1)); RET_STD(2, 1); }

/* ================= critical sections: one recursive host mutex per game structure ================= */
#define MAXCS 2048
static struct { uint32_t addr; pthread_mutex_t m; } CS[MAXCS]; static unsigned nCS;
static pthread_mutex_t *cs_of(uint32_t a) {
    pthread_mutex_lock(&hlock);
    for (unsigned i = 0; i < nCS; i++) if (CS[i].addr == a) { pthread_mutex_unlock(&hlock); return &CS[i].m; }
    if (nCS >= MAXCS) sa_fatal("too many critical sections");
    pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    CS[nCS].addr = a; pthread_mutex_init(&CS[nCS].m, &at);
    pthread_mutex_t *r = &CS[nCS++].m; pthread_mutex_unlock(&hlock); return r;
}
static void InitializeCriticalSection_(void) { cs_of(ARG(0)); RET_STD(1, 0); }
static void InitializeCriticalSectionAndSpinCount_(void) { cs_of(ARG(0)); RET_STD(2, 1); }
static void EnterCriticalSection_(void) { pthread_mutex_lock(cs_of(ARG(0))); RET_STD(1, 0); }
static void LeaveCriticalSection_(void) { pthread_mutex_unlock(cs_of(ARG(0))); RET_STD(1, 0); }
static void DeleteCriticalSection_(void) { RET_STD(1, 0); }

/* ================= events, mutexes, semaphores, waiting ================= */
static int dbg_sync = -1;
#define SYNC(...) do { if (dbg_sync < 0) dbg_sync = getenv("SA_SYNC") != NULL; if (dbg_sync) sa_log(__VA_ARGS__); } while (0)
static void CreateEvent_(void) { uint32_t h = h_new(H_EVENT); hobj *o = h_get(h); o->manual = ARG(1); o->signaled = ARG(2); SYNC("[%x] CreateEvent manual=%u init=%u -> %x (from %08x)", rd32(cpu.fs_base+0x24), ARG(1), ARG(2), h, rd32(cpu.esp)); RET_STD(4, h); }
static void SetEvent_(void) { SYNC("[%x] SetEvent(%x) (from %08x)", rd32(cpu.fs_base+0x24), ARG(0), rd32(cpu.esp)); hobj *o = h_get(ARG(0)); if (o) { pthread_mutex_lock(&o->m); o->signaled = 1; pthread_cond_broadcast(&o->c); pthread_mutex_unlock(&o->m); } RET_STD(1, o != NULL); }
static void ResetEvent_(void) { hobj *o = h_get(ARG(0)); if (o) { pthread_mutex_lock(&o->m); o->signaled = 0; pthread_mutex_unlock(&o->m); } RET_STD(1, o != NULL); }
static void CreateMutexA_(void) {
    uint32_t h = h_new(H_MUTEX); hobj *o = h_get(h);
    if (ARG(1)) { o->owner = pthread_self(); o->owner_set = 1; o->rec = 1; }
    RET_STD(3, h);
}
static void ReleaseMutex_(void) {
    hobj *o = h_get(ARG(0)); if (!o) { RET_STD(1, 0); return; }
    pthread_mutex_lock(&o->m);
    if (o->owner_set && pthread_equal(o->owner, pthread_self()) && --o->rec == 0) { o->owner_set = 0; pthread_cond_broadcast(&o->c); }
    pthread_mutex_unlock(&o->m); RET_STD(1, 1);
}
static void CreateSemaphoreA_(void) { uint32_t h = h_new(H_SEM); hobj *o = h_get(h); o->count = (int)ARG(1); o->max = (int)ARG(2); SYNC("[%x] CreateSemaphore %d/%d -> %x (from %08x)", rd32(cpu.fs_base+0x24), o->count, o->max, h, rd32(cpu.esp)); RET_STD(4, h); }
static void ReleaseSemaphore_(void) {
    hobj *o = h_get(ARG(0)); if (!o) { RET_STD(3, 0); return; }
    SYNC("[%x] ReleaseSemaphore(%x, %u) (from %08x)", rd32(cpu.fs_base+0x24), ARG(0), ARG(1), rd32(cpu.esp));
    pthread_mutex_lock(&o->m); if (ARG(2)) wr32(ARG(2), (uint32_t)o->count);
    o->count += (int)ARG(1); if (o->count > o->max) o->count = o->max; pthread_cond_broadcast(&o->c); pthread_mutex_unlock(&o->m); RET_STD(3, 1);
}
static int ready(hobj *o) {
    switch (o->type) {
    case H_EVENT: return o->signaled;
    case H_SEM: return o->count > 0;
    case H_MUTEX: return !o->owner_set || pthread_equal(o->owner, pthread_self());
    case H_THREAD: return o->done;
    default: return 1;
    }
}
static void consume(hobj *o) {
    if (o->type == H_EVENT && !o->manual) o->signaled = 0;
    if (o->type == H_SEM) o->count--;
    if (o->type == H_MUTEX) { o->owner = pthread_self(); o->owner_set = 1; o->rec++; }
}
static uint32_t wait_one(uint32_t h, uint32_t ms) {
    hobj *o = h_get(h); if (!o) return 0xFFFFFFFFu;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&o->m);
    while (!ready(o)) {
        if (ms == 0) { pthread_mutex_unlock(&o->m); return 0x102; }
        if (ms == 0xFFFFFFFFu) pthread_cond_wait(&o->c, &o->m);
        else if (pthread_cond_timedwait(&o->c, &o->m, &ts) == ETIMEDOUT && !ready(o)) { pthread_mutex_unlock(&o->m); return 0x102; }
    }
    consume(o); pthread_mutex_unlock(&o->m); return 0;
}
static void WaitForSingleObject_(void) { uint32_t h = ARG(0), ms = ARG(1), from = rd32(cpu.esp); uint32_t r = wait_one(h, ms); SYNC("[%x] Wait(%x, %d) -> %x (from %08x)", rd32(cpu.fs_base+0x24), h, (int)ms, r, from); RET_STD(2, r); }
static void WaitForSingleObjectEx_(void) { uint32_t h = ARG(0), ms = ARG(1); RET_STD(3, wait_one(h, ms)); }

/* ================= threads ================= */
static int thread_slots = 1;
static int free_slots[G_MAXTHREADS], nfree;   /* slots of finished game threads, reused (stack + TEB) */
static pthread_mutex_t tslot = PTHREAD_MUTEX_INITIALIZER;
static int slot_take(void) {
    pthread_mutex_lock(&tslot); int idx = nfree ? free_slots[--nfree] : thread_slots++; pthread_mutex_unlock(&tslot);
    if (idx >= G_MAXTHREADS) sa_fatal("too many threads"); return idx;
}
static void slot_give(int idx) { pthread_mutex_lock(&tslot); if (nfree < G_MAXTHREADS) free_slots[nfree++] = idx; pthread_mutex_unlock(&tslot); }
extern void call_ind(uint32_t);
int thread_alloc_slot(void) {   /* stack/TEB slot for a host thread that runs game code (sound...) */
    return slot_take();
}
static void thread_finish(hobj *o, int idx, uint32_t code) {
    pthread_mutex_lock(&o->m); o->done = 1; o->exitcode = code; pthread_cond_broadcast(&o->c); pthread_mutex_unlock(&o->m);
    slot_give(idx);
}
static __thread hobj *cur_thread; static __thread int cur_slot = -1;
static void *thread_main(void *arg) {
    hobj *o = arg;
    int idx = slot_take();
    memset(&cpu, 0, sizeof cpu);
    thread_setup_current(idx); cur_thread = o; cur_slot = idx;
    wr32(cpu.fs_base + 0x24, o->tid);
    push32(o->param); push32(0xFFFFFFF0u);                     /* dummy return address */
    call_ind(o->start);
    thread_finish(o, idx, cpu.eax);
    return NULL;
}
static uint32_t next_tid = 2000;
static void CreateThread_(void) {
    uint32_t h = h_new(H_THREAD); hobj *o = h_get(h);
    o->start = ARG(2); o->param = ARG(3); o->tid = next_tid++;
    if (ARG(5)) wr32(ARG(5), o->tid);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8u << 20);
    pthread_create(&o->th, &at, thread_main, o);
    if (sa_verbose) sa_log("CreateThread(0x%08x, 0x%08x) -> tid %u", o->start, o->param, o->tid);
    RET_STD(6, h);
}
static void ResumeThread_(void) { RET_STD(1, 0); }
static void ExitThread_(void) { if (cur_thread) thread_finish(cur_thread, cur_slot, ARG(0)); pthread_exit(NULL); }
static void TerminateThread_(void) { RET_STD(2, 1); }
static void GetThreadTimes_(void) { for (int i = 1; i <= 4; i++) { wr32(ARG(i), 0); wr32(ARG(i) + 4, 0); } RET_STD(5, 1); }
static void CloseHandle_(void) { h_close(ARG(0)); RET_STD(1, 1); }

/* ================= time ================= */
static uint64_t now_100ns(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 10000000ull + t.tv_nsec / 100 + 116444736000000000ull; }
static void GetSystemTimeAsFileTime_(void) { uint64_t v = now_100ns(); wr32(ARG(0), (uint32_t)v); wr32(ARG(0) + 4, (uint32_t)(v >> 32)); RET_STD(1, 0); }
static void QueryPerformanceCounter_(void) { uint64_t v = sa_clock_ns() / 100; wr32(ARG(0), (uint32_t)v); wr32(ARG(0) + 4, (uint32_t)(v >> 32)); RET_STD(1, 1); }   /* 10 MHz, stands still while paused */
static void QueryPerformanceFrequency_(void) { wr32(ARG(0), 10000000u); wr32(ARG(0) + 4, 0); RET_STD(1, 1); }
static void GetTickCount_(void) { RET_STD(0, (uint32_t)(sa_clock_ns() / 1000000u)); }
static void put_systime(uint32_t p, struct tm *tm, int ms) {
    wr16(p, 1900 + tm->tm_year); wr16(p + 2, tm->tm_mon + 1); wr16(p + 4, tm->tm_wday); wr16(p + 6, tm->tm_mday);
    wr16(p + 8, tm->tm_hour); wr16(p + 10, tm->tm_min); wr16(p + 12, tm->tm_sec); wr16(p + 14, ms);
}
static void GetLocalTime_(void) { struct timeval tv; gettimeofday(&tv, NULL); time_t t = tv.tv_sec; struct tm tm; localtime_r(&t, &tm); put_systime(ARG(0), &tm, tv.tv_usec / 1000); RET_STD(1, 0); }
static void GetTimeZoneInformation_(void) { memset(MEM + ARG(0), 0, 172); RET_STD(1, 0); }
static void FileTimeToSystemTime_(void) {
    uint64_t v = rd32(ARG(0)) | (uint64_t)rd32(ARG(0) + 4) << 32; time_t t = (time_t)((v - 116444736000000000ull) / 10000000ull);
    struct tm tm; gmtime_r(&t, &tm); put_systime(ARG(1), &tm, (int)((v / 10000) % 1000)); RET_STD(2, 1);
}
static void SystemTimeToFileTime_(void) {   /* SYSTEMTIME (UTC) -> FILETIME, 100 ns since 1601 */
    uint32_t p = ARG(0); struct tm tm = { 0 };
    tm.tm_year = rd16(p) - 1900; tm.tm_mon = rd16(p + 2) - 1; tm.tm_mday = rd16(p + 6);
    tm.tm_hour = rd16(p + 8); tm.tm_min = rd16(p + 10); tm.tm_sec = rd16(p + 12);
    if (tm.tm_year < -299 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31) { RET_STD(2, 0); return; }
    uint64_t v = (uint64_t)((int64_t)timegm(&tm) * 10000000ll + 116444736000000000ll) + (uint64_t)rd16(p + 14) * 10000u;
    wr32(ARG(1), (uint32_t)v); wr32(ARG(1) + 4, (uint32_t)(v >> 32)); RET_STD(2, 1);
}
static void FileTimeToLocalFileTime_(void) { wr32(ARG(1), rd32(ARG(0))); wr32(ARG(1) + 4, rd32(ARG(0) + 4)); RET_STD(2, 1); }
static void SystemTimeToTzSpecificLocalTime_(void) { memcpy(MEM + ARG(2), MEM + ARG(1), 16); RET_STD(3, 1); }

/* ================= files ================= */
static void open_file(const char *wpath, uint32_t access, uint32_t disp, int nargs) {
    char p[1024]; host_path(wpath, p, sizeof p);
    const char *mode = "rb";
    if (access & 0x40000000u) {           /* GENERIC_WRITE */
        if (disp == 2 || disp == 1) mode = (access & 0x80000000u) ? "w+b" : "wb";        /* CREATE_ALWAYS / CREATE_NEW */
        else if (disp == 4) { FILE *t = fopen(p, "ab"); if (t) fclose(t); mode = "r+b"; }  /* OPEN_ALWAYS */
        else mode = "r+b";
    }
    FILE *fp = fopen(p, mode);
    if (sa_verbose) sa_log("CreateFile(%s, %s, acc=0x%x disp=%u) -> %s", p, mode, access, disp, fp ? "ok" : "FAILED");
    if (!fp) { last_error = 2; RET_STD(nargs, 0xFFFFFFFFu); return; }
    if (mode[0] != 'r' || mode[1] == '+') setvbuf(fp, NULL, _IONBF, 0);   /* writing: unbuffered (several handles on the same file) */
    uint32_t h = h_new(H_FILE); h_get(h)->fp = fp; last_error = 0; RET_STD(nargs, h);
}
static void CreateFileA_(void) { open_file(g_str(ARG(0)), ARG(1), ARG(4), 7); }
static void CreateFileW_(void) { char b[1024]; g_wstr_to_utf8(ARG(0), b, sizeof b); open_file(b, ARG(1), ARG(4), 7); }
static void ReadFile_(void) {
    hobj *o = h_get(ARG(0)); uint32_t n = 0, ov = ARG(4);
    if (o && o->fp) {
        if (ov) fseek(o->fp, (long)(rd32(ov + 8) | (uint64_t)rd32(ov + 12) << 32), SEEK_SET);   /* "asynchronous" I/O: done immediately */
        n = (uint32_t)fread(MEM + ARG(1), 1, ARG(2), o->fp);
    }
    if (ARG(3)) wr32(ARG(3), n);
    if (ov) { wr32(ov, 0); wr32(ov + 4, n); hobj *e = h_get(rd32(ov + 16)); if (e) { pthread_mutex_lock(&e->m); e->signaled = 1; pthread_cond_broadcast(&e->c); pthread_mutex_unlock(&e->m); } }
    RET_STD(5, o && o->fp ? 1 : 0);
}
static void GetOverlappedResult_(void) { uint32_t ov = ARG(1); if (ARG(2)) wr32(ARG(2), rd32(ov + 4)); RET_STD(4, 1); }
static void CancelIo_(void) { RET_STD(1, 1); }
static void WriteFile_(void) {
    uint32_t h = ARG(0); hobj *o = h_get(h); uint32_t n = ARG(2);
    if (sa_verbose > 1) sa_log("WriteFile(%u, %u) @%ld : %.*s", h, n, o && o->fp ? ftell(o->fp) : -1L, (int)(n < 200 ? n : 200), (const char *)MEM + ARG(1));
    if (o && o->fp) n = (uint32_t)fwrite(MEM + ARG(1), 1, ARG(2), o->fp); else fwrite(MEM + ARG(1), 1, ARG(2), stderr);
    if (ARG(3)) wr32(ARG(3), n); RET_STD(5, 1);
}
static long fsize(FILE *fp) { long c = ftell(fp); fseek(fp, 0, SEEK_END); long s = ftell(fp); fseek(fp, c, SEEK_SET); return s; }
static void GetFileSize_(void) { hobj *o = h_get(ARG(0)); long s = o && o->fp ? fsize(o->fp) : 0; if (ARG(1)) wr32(ARG(1), 0); RET_STD(2, (uint32_t)s); }
static void GetFileSizeEx_(void) { hobj *o = h_get(ARG(0)); long s = o && o->fp ? fsize(o->fp) : 0; wr32(ARG(1), (uint32_t)s); wr32(ARG(1) + 4, 0); RET_STD(2, 1); }
static void SetFilePointer_(void) {
    hobj *o = h_get(ARG(0)); int32_t d = (int32_t)ARG(1); int w = (int)ARG(3);
    if (!o || !o->fp) { RET_STD(4, 0xFFFFFFFFu); return; }
    fseek(o->fp, d, w == 0 ? SEEK_SET : w == 1 ? SEEK_CUR : SEEK_END); RET_STD(4, (uint32_t)ftell(o->fp));
}
static void SetFilePointerEx_(void) {
    hobj *o = h_get(ARG(0)); int64_t d = (int64_t)(ARG(1) | (uint64_t)ARG(2) << 32); int w = (int)ARG(4);
    if (!o || !o->fp) { RET_STD(5, 0); return; }
    fseek(o->fp, (long)d, w == 0 ? SEEK_SET : w == 1 ? SEEK_CUR : SEEK_END);
    if (ARG(3)) { wr32(ARG(3), (uint32_t)ftell(o->fp)); wr32(ARG(3) + 4, 0); } RET_STD(5, 1);
}
static void SetEndOfFile_(void) { hobj *o = h_get(ARG(0)); if (o && o->fp) { fflush(o->fp); if (ftruncate(fileno(o->fp), ftell(o->fp))) {} } RET_STD(1, 1); }
static void FlushFileBuffers_(void) { hobj *o = h_get(ARG(0)); if (o && o->fp) fflush(o->fp); RET_STD(1, 1); }
static void GetFileInformationByHandle_(void) {
    hobj *o = h_get(ARG(0)); uint32_t p = ARG(1); memset(MEM + p, 0, 52);
    if (o && o->fp) { wr32(p, 0x80); wr32(p + 36, (uint32_t)fsize(o->fp)); wr32(p + 40, 1); }
    RET_STD(2, o && o->fp);
}
static void w2h(uint32_t w, char *out) { char b[1024]; g_wstr_to_utf8(w, b, sizeof b); host_path(b, out, 1024); }
static void CreateDirectoryW_(void) { char p[1024]; w2h(ARG(0), p); RET_STD(2, mkdir(p, 0777) == 0 || errno == EEXIST); }
static void RemoveDirectoryW_(void) { char p[1024]; w2h(ARG(0), p); RET_STD(1, rmdir(p) == 0); }
static void DeleteFileW_(void) { char p[1024]; w2h(ARG(0), p); RET_STD(1, unlink(p) == 0); }
static void GetDriveTypeW_(void) { RET_STD(1, 3); }
static void GetFullPathNameW_(void) {
    uint32_t src = ARG(0), n = ARG(1), buf = ARG(2); uint32_t k = 0;
    while (rd16(src + 2 * k)) k++;
    if (k + 1 > n) { RET_STD(4, k + 1); return; }
    memcpy(MEM + buf, MEM + src, 2 * k + 2); if (ARG(3)) wr32(ARG(3), 0); RET_STD(4, k);
}
static void GetCurrentDirectoryA_(void) { const char *s = "W:"; uint32_t n = ARG(0); if (n > 3) memcpy(MEM + ARG(1), s, 3); RET_STD(2, 2); }
static void GetCurrentDirectoryW_(void) { uint32_t n = ARG(0), b = ARG(1); if (n > 3) { wr16(b, 'W'); wr16(b + 2, ':'); wr16(b + 4, 0); } RET_STD(2, 2); }
static void SetCurrentDirectoryA_(void) { RET_STD(1, 1); }
static void SetCurrentDirectoryW_(void) { RET_STD(1, 1); }
/* file search: FindFirstFileExW(name, level, WIN32_FIND_DATAW*, ...) */
static int find_next(hobj *o, uint32_t fd) {
    struct dirent *e;
    while ((e = readdir(o->dir))) {
        if (fnmatch(o->pat, e->d_name, FNM_CASEFOLD)) continue;
        char full[1400]; snprintf(full, sizeof full, "%s/%s", o->dpath, e->d_name); struct stat st; stat(full, &st);
        memset(MEM + fd, 0, 592);
        wr32(fd, S_ISDIR(st.st_mode) ? 0x10 : 0x80); wr32(fd + 32, (uint32_t)st.st_size);
        static _Thread_local uint32_t wb, wcap;   /* one conversion buffer per thread, reused across calls */
        uint32_t w = g_wstrdup_reuse(&wb, &wcap, e->d_name); uint32_t k = 0; while (rd16(w + 2 * k) && k < 259) { wr16(fd + 44 + 2 * k, rd16(w + 2 * k)); k++; }
        return 1;
    }
    return 0;
}
static void FindFirstFileExW_(void) {
    char p[1024]; w2h(ARG(0), p);
    char *sl = strrchr(p, '/'); hobj tmp = { 0 };
    if (sl) { *sl = 0; snprintf(tmp.dpath, sizeof tmp.dpath, "%s", p[0] ? p : "."); snprintf(tmp.pat, sizeof tmp.pat, "%s", sl + 1); }
    else { strcpy(tmp.dpath, "."); snprintf(tmp.pat, sizeof tmp.pat, "%s", p); }
    DIR *d = opendir(tmp.dpath);
    if (!d) { last_error = 2; RET_STD(6, 0xFFFFFFFFu); return; }
    uint32_t h = h_new(H_FIND); hobj *o = h_get(h); o->dir = d; strcpy(o->dpath, tmp.dpath); strcpy(o->pat, tmp.pat);
    if (!find_next(o, ARG(2))) { h_close(h); last_error = 2; RET_STD(6, 0xFFFFFFFFu); return; }
    RET_STD(6, h);
}
static void FindNextFileW_(void) { hobj *o = h_get(ARG(0)); int r = o && o->dir && find_next(o, ARG(1)); if (!r) last_error = 18; RET_STD(2, r); }
static void FindClose_(void) { h_close(ARG(0)); RET_STD(1, 1); }
static void GetPrivateProfileStringA_(void) {
    /* reading a game .ini (section, key, default, buffer, size, file) */
    const char *sec = g_str(ARG(0)), *key = g_str(ARG(1)), *def = ARG(2) ? g_str(ARG(2)) : "";
    uint32_t out = ARG(3), n = ARG(4); char path[1024]; host_path(g_str(ARG(5)), path, sizeof path);
    char val[1024]; snprintf(val, sizeof val, "%s", def);
    FILE *f = fopen(path, "rb");
    if (f && sec && key) {
        char line[1024], cur[256] = "";
        while (fgets(line, sizeof line, f)) {
            char *s = line; while (*s == ' ' || *s == '\t') s++;
            if (*s == '[') { char *e = strchr(s, ']'); if (e) { *e = 0; snprintf(cur, sizeof cur, "%s", s + 1); } continue; }
            char *eq = strchr(s, '='); if (!eq || strcasecmp(cur, sec)) continue;
            *eq = 0; char *k = s, *v = eq + 1; char *e = k + strlen(k); while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
            if (strcasecmp(k, key)) continue;
            e = v + strlen(v); while (e > v && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) *--e = 0;
            snprintf(val, sizeof val, "%s", v); break;
        }
    }
    if (f) fclose(f);
    uint32_t k = 0; while (k + 1 < n && val[k]) { wr8(out + k, val[k]); k++; } if (n) wr8(out + k, 0);
    RET_STD(6, k);
}
static void PeekNamedPipe_(void) { RET_STD(6, 0); }
static void GetConsoleMode_(void) { RET_STD(2, 0); }
static void GetConsoleOutputCP_(void) { RET_STD(0, 65001); }
static void WriteConsoleW_(void) { RET_STD(5, 0); }
static void ReadConsoleW_(void) { RET_STD(5, 0); }
static void SetStdHandle_(void) { RET_STD(2, 1); }
static void lstrlenA_(void) { RET_STD(1, ARG(0) ? (uint32_t)strlen(g_str(ARG(0))) : 0); }
static void GetDateFormatW_(void) { RET_STD(6, 0); }
static void GetTimeFormatW_(void) { RET_STD(6, 0); }
/* resources: read from the .rsrc section of the loaded image (TEXT 201 = window settings, icons...) */
static uint32_t rsrc_root(void) {
    uint32_t pe = pe_image_base + rd32(pe_image_base + 0x3C); return rd32(pe + 0x18 + 0x60 + 8 * 2) ? pe_image_base + rd32(pe + 0x18 + 0x60 + 8 * 2) : 0;
}
static uint32_t rsrc_find(uint32_t root, uint32_t dir, uint32_t key) {   /* key: integer (<0x10000) or game ASCII string */
    uint16_t nn = rd16(dir + 12), ni = rd16(dir + 14); uint32_t e = dir + 16;
    for (uint32_t i = 0; i < (uint32_t)nn + ni; i++, e += 8) {
        uint32_t name = rd32(e), off = rd32(e + 4);
        if (key < 0x10000) { if (!(name & 0x80000000u) && name == key) return off; continue; }
        if (!(name & 0x80000000u)) continue;
        uint32_t ws = root + (name & 0x7FFFFFFFu); uint16_t len = rd16(ws); const char *k = g_str(key); int ok = (int)strlen(k) == len;
        for (uint16_t j = 0; ok && j < len; j++) { int c = k[j]; if (c >= 'a' && c <= 'z') c -= 32; ok = rd16(ws + 2 + 2u * j) == (uint16_t)c; }
        if (ok) return off;
    }
    return 0xFFFFFFFFu;
}
static void FindResourceA_(void) {
    uint32_t root = rsrc_root(), r = 0;
    if (root) {
        uint32_t t = rsrc_find(root, root, ARG(2));
        if (t != 0xFFFFFFFFu && (t & 0x80000000u)) {
            uint32_t n = rsrc_find(root, root + (t & 0x7FFFFFFFu), ARG(1));
            if (n != 0xFFFFFFFFu && (n & 0x80000000u)) { uint32_t l = root + (n & 0x7FFFFFFFu); uint32_t d = rd32(l + 16 + 4); if (!(d & 0x80000000u)) r = root + d; }
        }
    }
    if (sa_verbose) sa_log("FindResourceA(%u, %u) -> 0x%x", ARG(1), ARG(2), r);
    RET_STD(3, r);
}
static void LoadResource_(void) { uint32_t h = ARG(1); RET_STD(2, h ? pe_image_base + rd32(h) : 0); }
static void LockResource_(void) { RET_STD(1, ARG(0)); }
static void SizeofResource_(void) { uint32_t h = ARG(1); RET_STD(2, h ? rd32(h + 4) : 0); }
static void FreeResource_(void) { RET_STD(1, 1); }
/* process diagnostics (used by the cabinet's protection / logs) */
static void K32EnumProcessModulesEx_(void) { if (ARG(3)) wr32(ARG(3), 0); RET_STD(5, 1); }
static void K32GetModuleBaseNameA_(void) { RET_STD(4, 0); }
static void K32GetModuleInformation_(void) { RET_STD(4, 0); }
static void VirtualQueryEx_(void) { RET_STD(4, 0); }
static void ReadProcessMemory_(void) { RET_STD(5, 0); }
static void FreeLibraryAndExitThread_(void) { pthread_exit(NULL); }
static void RtlUnwind_(void) { sa_fatal("RtlUnwind: SEH exceptions not handled yet (from 0x%08x)", rd32(cpu.esp)); }

#define K(n) { "KERNEL32.DLL!" #n, n##_ }
static const imp_def defs[] = {
    K(GetLastError), K(SetLastError), K(GetCurrentProcess), K(GetCurrentThread), K(GetCurrentProcessId), K(GetCurrentThreadId),
    K(IsDebuggerPresent), K(IsProcessorFeaturePresent), K(SetUnhandledExceptionFilter), K(UnhandledExceptionFilter),
    K(InitializeSListHead), K(EncodePointer), K(DecodePointer), K(OutputDebugStringW), K(ExitProcess), K(TerminateProcess),
    K(GetStartupInfoW), K(GetStdHandle), K(GetFileType), K(GetModuleHandleW), K(GetModuleHandleA), K(GetModuleHandleExW),
    K(GetModuleFileNameW), K(GetModuleFileNameA), K(GetProcAddress), K(LoadLibraryExW), K(FreeLibrary), K(GetVersionExA),
    K(VerSetConditionMask), K(VerifyVersionInfoA), K(GetSystemInfo), K(GetNativeSystemInfo), K(GetProcessAffinityMask),
    K(IsWow64Process), K(SetThreadPriority), K(SetThreadIdealProcessor), K(SwitchToThread), K(Sleep), K(SleepEx),
    K(RaiseException), K(FormatMessageA), K(LocalFree), K(IsDBCSLeadByte), K(ExpandEnvironmentStringsA), K(SetEnvironmentVariableW),
    K(GetEnvironmentStringsW), K(FreeEnvironmentStringsW), K(GetACP), K(GetOEMCP), K(IsValidCodePage), K(GetCPInfo),
    K(GetStringTypeW), K(LCMapStringW), K(CompareStringW),
    K(GetUserDefaultLCID), K(IsValidLocale), K(GetLocaleInfoW), K(EnumSystemLocalesW),
    K(GetProcessHeap), K(HeapCreate), K(HeapDestroy), K(GetCommandLineA), K(GetCommandLineW), K(VirtualAlloc), K(VirtualFree), K(VirtualProtect),
    K(HeapAlloc), K(HeapFree), K(HeapReAlloc), K(HeapSize), K(TlsAlloc), K(TlsFree), K(TlsGetValue), K(TlsSetValue),
    K(InitializeCriticalSection), K(InitializeCriticalSectionAndSpinCount), K(EnterCriticalSection), K(LeaveCriticalSection), K(DeleteCriticalSection),
    { "KERNEL32.DLL!CreateEventA", CreateEvent_ }, { "KERNEL32.DLL!CreateEventW", CreateEvent_ },
    K(SetEvent), K(ResetEvent), K(CreateMutexA), K(ReleaseMutex), K(CreateSemaphoreA), K(ReleaseSemaphore),
    K(WaitForSingleObject), K(WaitForSingleObjectEx), K(CreateThread), K(ResumeThread), K(ExitThread), K(TerminateThread),
    K(GetThreadTimes), K(CloseHandle), K(GetSystemTimeAsFileTime), K(QueryPerformanceCounter), K(QueryPerformanceFrequency),
    K(GetTickCount), K(GetLocalTime), K(GetTimeZoneInformation), K(FileTimeToSystemTime), K(SystemTimeToFileTime), K(FileTimeToLocalFileTime),
    K(SystemTimeToTzSpecificLocalTime), K(CreateFileA), K(CreateFileW), K(ReadFile), K(GetOverlappedResult), K(CancelIo), K(WriteFile), K(GetFileSize), K(GetFileSizeEx),
    K(SetFilePointer), K(SetFilePointerEx), K(SetEndOfFile), K(FlushFileBuffers), K(GetFileInformationByHandle),
    K(CreateDirectoryW), K(RemoveDirectoryW), K(DeleteFileW), K(GetDriveTypeW), K(GetFullPathNameW),
    K(GetCurrentDirectoryA), K(GetCurrentDirectoryW), K(SetCurrentDirectoryA), K(SetCurrentDirectoryW),
    K(FindFirstFileExW), K(FindNextFileW), K(FindClose), K(GetPrivateProfileStringA), K(PeekNamedPipe), K(GetConsoleMode),
    K(GetConsoleOutputCP), K(WriteConsoleW), K(ReadConsoleW), K(SetStdHandle), K(lstrlenA), K(GetDateFormatW), K(GetTimeFormatW),
    K(FindResourceA), K(LoadResource), K(LockResource), K(SizeofResource), K(FreeResource),
    K(K32EnumProcessModulesEx), K(K32GetModuleBaseNameA), K(K32GetModuleInformation), K(VirtualQueryEx), K(ReadProcessMemory),
    K(FreeLibraryAndExitThread), K(RtlUnwind),
    { NULL, NULL }
};
void k32_register(void) { imp_register(defs); }
