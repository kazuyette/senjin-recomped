/* sa_host.h - Senjin Aleste, native port: common services of the host layer
 * (PE loader, game memory, Windows imports rewritten natively). */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "x86rt.h"

/* ---- layout of the game's 32-bit address space (inside MEM) ---- */
#define G_STACKS     0x02000000u   /* thread stacks: 1 MB each */
#define G_STACK_SIZE 0x00100000u
#define G_MAXTHREADS 64
#define G_TEBS       0x7F000000u   /* one TEB (4 KB) per thread */
#define G_HEAP_LO    0x10000000u   /* game heap */
#define G_HEAP_HI    0x7E000000u
#define G_STRINGS    0x7E000000u   /* static strings and structures provided by the host */
#define G_MAGIC      0xFFF00000u   /* "magic" import addresses: G_MAGIC + 16 * index */

/* ---- log ---- */
extern int sa_verbose;
void sa_log(const char *fmt, ...);
void sa_fatal(const char *fmt, ...);

/* ---- game memory ---- */
void mem_init(void);
uint32_t g_alloc(uint32_t size, int zero);            /* game heap (HeapAlloc & co) */
void g_free(uint32_t p);
uint32_t g_size(uint32_t p);
uint32_t g_realloc(uint32_t p, uint32_t size);
uint32_t g_static(uint32_t size);                      /* permanent host area */
uint32_t g_strdup(const char *s);
uint32_t g_wstrdup(const char *s);                     /* UTF-8 -> UTF-16 in game memory */
uint32_t g_wstrdup_reuse(uint32_t *buf, uint32_t *cap, const char *s);   /* same, reusing *buf when large enough */
const char *g_str(uint32_t p);                         /* game char* -> host pointer */
int g_wstr_to_utf8(uint32_t p, char *out, int n);      /* game wchar_t* -> host UTF-8 */

/* ---- pause (sa_misc.c) ---- */
extern volatile int sa_paused;
uint64_t sa_clock_ns(void);                            /* game clock: monotonic, stands still while paused */
void sa_pause_set(int on);
int sa_pause_button(void);                             /* pause control held (gamepad "Pause" button, P, Pause key) */

/* ---- loader ---- */
uint32_t pe_load(const char *path);                    /* returns the entry point */
extern uint32_t pe_image_base, pe_tls_index_addr, pe_tls_start, pe_tls_end;
extern char sa_exe_path[1024], sa_game_dir[1024];

/* ---- threads ---- */
uint32_t thread_setup_current(int idx);                /* TEB + stack for the current thread, returns the top of stack */
int thread_alloc_slot(void);                            /* free slot for a host thread */
//                /* TEB + stack for the current thread, returns the top of stack */

/* ---- imports ---- */
#define ARG(n)   rd32(cpu.esp + 4 + 4 * (n))
#define RET_STD(nargs, v) do { cpu.eax = (uint32_t)(v); cpu.esp += 4 + 4 * (nargs); } while (0)
#define RET_C(v) do { cpu.eax = (uint32_t)(v); cpu.esp += 4; } while (0)
typedef struct { const char *name; void (*fn)(void); } imp_def;
void imports_init(void);
uint32_t imp_magic_for(const char *name);              /* for GetProcAddress */
void imp_register(const imp_def *defs);               /* each import module registers itself */
