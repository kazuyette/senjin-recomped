/* x86rt.c - kit recomp_x86 : runtime commun (dispatch des appels indirects, diagnostics).
 * Le chargement de l'image (sections du PE dans MEM) et les imports natifs sont fournis par le projet. */
#include <stdio.h>
#include <stdlib.h>
#include "x86rt.h"

_Thread_local CPU cpu;
uint8_t *MEM;

extern const struct { uint32_t addr; void (*fn)(void); } rc_table[];
extern const unsigned rc_table_n;

/* Le projet fournit : x86_import_call(slot_value) pour une adresse « magique » d'import (renvoie 1 si gérée). */
int x86_import_call(uint32_t target);

static gfn lookup(uint32_t t) {
    unsigned lo = 0, hi = rc_table_n;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (rc_table[m].addr < t) lo = m + 1; else hi = m; }
    return (lo < rc_table_n && rc_table[lo].addr == t) ? rc_table[lo].fn : NULL;
}
void call_ind(uint32_t target) {
    gfn f = lookup(target);
    if (f) { f(); return; }
    if (x86_import_call(target)) return;
    fprintf(stderr, "x86rt : appel indirect vers 0x%08x inconnu\n", target); abort();
}
void jmp_ind(uint32_t target) {
    /* saut sortant : l'adresse de retour de l'appelant est déjà sur la pile -> même traitement qu'un appel
       dont on « consomme » le retour : on retire puis la fonction cible fera son propre ret */
    gfn f = lookup(target);
    if (f) { f(); return; }
    if (x86_import_call(target)) return;
    fprintf(stderr, "x86rt : saut indirect vers 0x%08x inconnu\n", target); abort();
}
void x86_unimpl(uint32_t addr, const char *what) {
    fprintf(stderr, "x86rt : instruction non gérée à 0x%08x : %s\n", addr, what); abort();
}
void x86_int3(uint32_t addr) { fprintf(stderr, "x86rt : int3 à 0x%08x\n", addr); abort(); }
/* CPUID : on annonce un processeur SSE2 sans SSE4/AVX, pour que la CRT MSVC choisisse ses chemins SSE2 */
void x86_cpuid(void) {
    uint32_t leaf = cpu.eax;
    if (leaf == 0) { cpu.eax = 1; cpu.ebx = 0x756e6547; cpu.edx = 0x49656e69; cpu.ecx = 0x6c65746e; return; }  /* GenuineIntel */
    if (leaf == 1) { cpu.eax = 0x000006F6; cpu.ebx = 0; cpu.ecx = 0x00000001; cpu.edx = 0x078BFBFF; return; }  /* SSE3 + SSE2 */
    cpu.eax = cpu.ebx = cpu.ecx = cpu.edx = 0;
}
void x86_rdtsc(void) {
    static uint64_t t; t += 1000; cpu.eax = (uint32_t)t; cpu.edx = (uint32_t)(t >> 32);
}
