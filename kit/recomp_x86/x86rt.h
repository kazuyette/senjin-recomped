/* x86rt.h - kit recomp_x86: runtime for the C code generated from a 32-bit x86 exe.
 * Principle: the game's 32-bit address space lives in a MEM block (4 GB reserved); the processor
 * registers and flags are C variables (one copy per thread). Each function of the exe
 * becomes a C function `void f_XXXXXXXX(void)`. This is not an interpreter: it is compiled code.
 * Version: see the VERSION of the project that uses it. */
#ifndef X86RT_H
#define X86RT_H
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef union { uint8_t b[16]; uint16_t w[8]; uint32_t d[4]; uint64_t q[2]; float f[4]; double g[2]; } xmm_t;
typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t cf, zf, sf, of, pf, df;          /* flags (0/1) */
    xmm_t xmm[8];
    double st[8]; int top; uint16_t fsw, fcw;  /* x87: stack as double */
    uint32_t fs_base;                         /* TEB (fs:[0] = SEH chain) */
} CPU;
extern _Thread_local CPU cpu;
extern uint8_t *MEM;

/* ---- game memory ---- */
static inline uint8_t  rd8 (uint32_t a) { return MEM[a]; }
static inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, MEM + a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, MEM + a, 4); return v; }
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, MEM + a, 8); return v; }
static inline float    rdf32(uint32_t a) { float v; memcpy(&v, MEM + a, 4); return v; }
static inline double   rdf64(uint32_t a) { double v; memcpy(&v, MEM + a, 8); return v; }
static inline void wr8 (uint32_t a, uint8_t v)  { MEM[a] = v; }
static inline void wr16(uint32_t a, uint16_t v) { memcpy(MEM + a, &v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(MEM + a, &v, 4); }
static inline void wr64(uint32_t a, uint64_t v) { memcpy(MEM + a, &v, 8); }
static inline void wrf32(uint32_t a, float v)   { memcpy(MEM + a, &v, 4); }
static inline void wrf64(uint32_t a, double v)  { memcpy(MEM + a, &v, 8); }
static inline void rd128(uint32_t a, xmm_t *x) { memcpy(x, MEM + a, 16); }
static inline void wr128(uint32_t a, const xmm_t *x) { memcpy(MEM + a, x, 16); }

/* ---- 8/16-bit registers ---- */
#define LO8(r)  ((uint8_t)(r))
#define HI8(r)  ((uint8_t)((r) >> 8))
#define LO16(r) ((uint16_t)(r))
#define SETLO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))
#define SETHI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(v) << 8))
#define SETLO16(r, v) ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))

/* ---- stack ---- */
static inline void push32(uint32_t v) { cpu.esp -= 4; wr32(cpu.esp, v); }
static inline uint32_t pop32(void) { uint32_t v = rd32(cpu.esp); cpu.esp += 4; return v; }

/* ---- flags ---- */
static inline uint32_t parity8(uint32_t v) { v &= 0xFF; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (~v) & 1; }
#define MSB(v, bits) (((v) >> ((bits) - 1)) & 1)
#define MASK(bits) ((bits) == 32 ? 0xFFFFFFFFu : ((1u << (bits)) - 1))
static inline void fl_logic(uint32_t r, int bits) { r &= MASK(bits); cpu.cf = 0; cpu.of = 0; cpu.zf = r == 0; cpu.sf = MSB(r, bits); cpu.pf = parity8(r); }
static inline void fl_add(uint32_t a, uint32_t b, uint32_t c, int bits) {   /* r = a + b + c */
    uint64_t m = MASK(bits); uint64_t r = ((uint64_t)a & m) + ((uint64_t)b & m) + c; uint32_t rr = (uint32_t)(r & m);
    cpu.cf = (r >> bits) & 1; cpu.zf = rr == 0; cpu.sf = MSB(rr, bits); cpu.pf = parity8(rr);
    cpu.of = MSB((~(a ^ b)) & (a ^ rr), bits);
}
static inline void fl_sub(uint32_t a, uint32_t b, uint32_t c, int bits) {   /* r = a - b - c */
    uint32_t m = MASK(bits); uint32_t rr = (a - b - c) & m;
    cpu.cf = ((uint64_t)(a & m) < (uint64_t)(b & m) + c); cpu.zf = rr == 0; cpu.sf = MSB(rr, bits); cpu.pf = parity8(rr);
    cpu.of = MSB((a ^ b) & (a ^ rr), bits);
}
static inline void fl_incdec(uint32_t r, uint32_t a, int inc, int bits) {   /* CF unchanged */
    uint32_t m = MASK(bits); r &= m; cpu.zf = r == 0; cpu.sf = MSB(r, bits); cpu.pf = parity8(r);
    cpu.of = inc ? (r == (1u << (bits - 1))) : ((a & m) == (1u << (bits - 1)));
}
/* conditions */
#define CC_O   (cpu.of)
#define CC_NO  (!cpu.of)
#define CC_B   (cpu.cf)
#define CC_AE  (!cpu.cf)
#define CC_E   (cpu.zf)
#define CC_NE  (!cpu.zf)
#define CC_BE  (cpu.cf || cpu.zf)
#define CC_A   (!cpu.cf && !cpu.zf)
#define CC_S   (cpu.sf)
#define CC_NS  (!cpu.sf)
#define CC_P   (cpu.pf)
#define CC_NP  (!cpu.pf)
#define CC_L   (cpu.sf != cpu.of)
#define CC_GE  (cpu.sf == cpu.of)
#define CC_LE  (cpu.zf || cpu.sf != cpu.of)
#define CC_G   (!cpu.zf && cpu.sf == cpu.of)

/* ---- x87 (stack as double) ---- */
#define ST(i) cpu.st[(cpu.top + (i)) & 7]
static inline void fpush(double v) { cpu.top = (cpu.top - 1) & 7; cpu.st[cpu.top] = v; }
static inline double fpop(void) { double v = cpu.st[cpu.top]; cpu.top = (cpu.top + 1) & 7; return v; }
static inline void fcom_set(double a, double b) {          /* C0=bit8, C2=bit10, C3=bit14 */
    cpu.fsw &= ~0x4500;
    if (a != a || b != b) cpu.fsw |= 0x4500; else if (a < b) cpu.fsw |= 0x0100; else if (a == b) cpu.fsw |= 0x4000;
}
static inline void fcomi_set(double a, double b) {
    if (a != a || b != b) { cpu.zf = cpu.pf = cpu.cf = 1; } else { cpu.zf = a == b; cpu.pf = 0; cpu.cf = a < b; }
    cpu.of = cpu.sf = 0;
}
static inline uint16_t fsw_get(void) { return (uint16_t)((cpu.fsw & ~0x3800) | ((cpu.top & 7) << 11)); }
/* 80-bit reals (fld/fstp tbyte, fnsave) and x87 environment (28 bytes, 32-bit protected-mode format) */
static inline double rdf80(uint32_t a) {
    uint64_t m; uint16_t se; memcpy(&m, MEM + a, 8); memcpy(&se, MEM + a + 8, 2);
    int e = se & 0x7FFF; double s = (se & 0x8000) ? -1.0 : 1.0;
    if (e == 0 && m == 0) return s * 0.0;
    if (e == 0x7FFF) return (m << 1) ? (double)NAN : s * (double)INFINITY;
    return s * ldexp((double)m, e - 16383 - 63);
}
static inline void wrf80(uint32_t a, double v) {
    uint64_t m = 0; uint16_t se = 0;
    if (v != v) { m = 0xC000000000000000ull; se = 0x7FFF; }
    else { if (signbit(v)) { se = 0x8000; v = -v; }
        if (isinf(v)) { m = 0x8000000000000000ull; se |= 0x7FFF; }
        else if (v != 0) { int e; double f = frexp(v, &e); m = (uint64_t)ldexp(f, 64); se |= (uint16_t)(e - 1 + 16383); } }
    memcpy(MEM + a, &m, 8); memcpy(MEM + a + 8, &se, 2);
}
static inline void fxam_set(double v) {   /* C3 C2 C0 = class, C1 = sign (empty stack is not tracked) */
    cpu.fsw &= ~0x4700; if (signbit(v)) cpu.fsw |= 0x200;
    if (v != v) cpu.fsw |= 0x100; else if (isinf(v)) cpu.fsw |= 0x500; else if (v == 0) cpu.fsw |= 0x4000;
    else if (fabs(v) < 2.2250738585072014e-308) cpu.fsw |= 0x4400; else cpu.fsw |= 0x400;
}
static inline void fenv_store(uint32_t a) {
    memset(MEM + a, 0, 28); wr16(a, cpu.fcw); wr16(a + 4, fsw_get()); wr16(a + 8, 0xFFFF);
}
static inline void fenv_load(uint32_t a) { cpu.fcw = rd16(a); uint16_t w = rd16(a + 4); cpu.fsw = w & ~0x3800; cpu.top = (w >> 11) & 7; }
static inline void ucomis_set(double a, double b) { fcomi_set(a, b); }

/* ---- calls ---- */
typedef void (*gfn)(void);
void call_ind(uint32_t target);          /* indirect call: recompiled function or native import */
void jmp_ind(uint32_t target);           /* indirect jump leaving the function (tail call) */
void x86_unimpl(uint32_t addr, const char *what);
void x86_int3(uint32_t addr);
#define UNIMPL(a, s) x86_unimpl((a), (s))

/* SSE conversions (truncation / current rounding) with Intel's "integer indefinite" value */
static inline int32_t cvtt_i32(double v) { return (v != v || v >= 2147483648.0 || v < -2147483648.0) ? (int32_t)0x80000000 : (int32_t)v; }
static inline int32_t cvt_i32(double v) { return cvtt_i32(nearbyint(v)); }
static inline int64_t cvtt_i64(double v) { return (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? (int64_t)0x8000000000000000ull : (int64_t)v; }
#endif
