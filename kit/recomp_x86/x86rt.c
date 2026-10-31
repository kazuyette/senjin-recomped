/* x86rt.c - kit recomp_x86: common runtime (indirect call dispatch, diagnostics).
 * Loading the image (PE sections into MEM) and the native imports are provided by the project. */
#include <stdio.h>
#include <stdlib.h>
#include "x86rt.h"

_Thread_local CPU cpu;
uint8_t *MEM;

extern const struct { uint32_t addr; void (*fn)(void); } rc_table[];
extern const unsigned rc_table_n;

/* The project provides: x86_import_call(slot_value) for a "magic" import address (returns 1 if handled). */
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
    fprintf(stderr, "x86rt: indirect call to unknown 0x%08x\n", target); abort();
}
void jmp_ind(uint32_t target) {
    /* outgoing jump: the caller's return address is already on the stack -> same handling as a call
       whose return is "consumed": we leave, then the target function will do its own ret */
    gfn f = lookup(target);
    if (f) { f(); return; }
    if (x86_import_call(target)) return;
    fprintf(stderr, "x86rt: indirect jump to unknown 0x%08x\n", target); abort();
}
void x86_unimpl(uint32_t addr, const char *what) {
    fprintf(stderr, "x86rt: unhandled instruction at 0x%08x: %s\n", addr, what); abort();
}
void x86_int3(uint32_t addr) { fprintf(stderr, "x86rt: int3 at 0x%08x\n", addr); abort(); }
/* CPUID: report an SSE2 processor without SSE4/AVX, so that the MSVC CRT picks its SSE2 paths */
void x86_cpuid(void) {
    uint32_t leaf = cpu.eax;
    if (leaf == 0) { cpu.eax = 1; cpu.ebx = 0x756e6547; cpu.edx = 0x49656e69; cpu.ecx = 0x6c65746e; return; }  /* GenuineIntel */
    if (leaf == 1) { cpu.eax = 0x000006F6; cpu.ebx = 0; cpu.ecx = 0x00000001; cpu.edx = 0x078BFBFF; return; }  /* SSE3 + SSE2 */
    cpu.eax = cpu.ebx = cpu.ecx = cpu.edx = 0;
}
void x86_rdtsc(void) {
    static uint64_t t; t += 1000; cpu.eax = (uint32_t)t; cpu.edx = (uint32_t)(t >> 32);
}
