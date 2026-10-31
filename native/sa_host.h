/* sa_host.h - Senjin Aleste, portage natif : services communs de la couche hôte
 * (chargeur du PE, mémoire du jeu, imports Windows réécrits en natif). */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "x86rt.h"

/* ---- disposition de l'espace d'adressage 32 bits du jeu (dans MEM) ---- */
#define G_STACKS     0x02000000u   /* piles des threads : 1 Mo chacune */
#define G_STACK_SIZE 0x00100000u
#define G_MAXTHREADS 64
#define G_TEBS       0x7F000000u   /* un TEB (4 Ko) par thread */
#define G_HEAP_LO    0x10000000u   /* tas du jeu */
#define G_HEAP_HI    0x7E000000u
#define G_STRINGS    0x7E000000u   /* chaînes et structures statiques fournies par l'hôte */
#define G_MAGIC      0xFFF00000u   /* adresses « magiques » des imports : G_MAGIC + 16 * index */

/* ---- journal ---- */
extern int sa_verbose;
void sa_log(const char *fmt, ...);
void sa_fatal(const char *fmt, ...);

/* ---- mémoire du jeu ---- */
void mem_init(void);
uint32_t g_alloc(uint32_t size, int zero);            /* tas du jeu (HeapAlloc & co) */
void g_free(uint32_t p);
uint32_t g_size(uint32_t p);
uint32_t g_realloc(uint32_t p, uint32_t size);
uint32_t g_static(uint32_t size);                      /* zone hôte permanente */
uint32_t g_strdup(const char *s);
uint32_t g_wstrdup(const char *s);                     /* UTF-8 -> UTF-16 dans la mémoire du jeu */
const char *g_str(uint32_t p);                         /* char* du jeu -> pointeur hôte */
int g_wstr_to_utf8(uint32_t p, char *out, int n);      /* wchar_t* du jeu -> UTF-8 hôte */

/* ---- chargeur ---- */
uint32_t pe_load(const char *path);                    /* renvoie le point d'entrée */
extern uint32_t pe_image_base, pe_tls_index_addr, pe_tls_start, pe_tls_end;
extern char sa_exe_path[1024], sa_game_dir[1024];

/* ---- threads ---- */
uint32_t thread_setup_current(int idx);                /* TEB + pile pour le thread courant, renvoie le haut de pile */
int thread_alloc_slot(void);                            /* emplacement libre pour un thread hôte */
//                /* TEB + pile pour le thread courant, renvoie le haut de pile */

/* ---- imports ---- */
#define ARG(n)   rd32(cpu.esp + 4 + 4 * (n))
#define RET_STD(nargs, v) do { cpu.eax = (uint32_t)(v); cpu.esp += 4 + 4 * (nargs); } while (0)
#define RET_C(v) do { cpu.eax = (uint32_t)(v); cpu.esp += 4; } while (0)
typedef struct { const char *name; void (*fn)(void); } imp_def;
void imports_init(void);
uint32_t imp_magic_for(const char *name);              /* pour GetProcAddress */
void imp_register(const imp_def *defs);               /* chaque module d'imports s'enregistre */
