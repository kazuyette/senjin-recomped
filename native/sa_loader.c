/* sa_loader.c - loader: places the original exe (data, resources) at its base address in MEM,
 * fills the IAT with the "magic" addresses of the native imports, sets up TLS and TEB.
 * The exe's code is not executed: its recompiled version (gen/) is what runs. */
#include <stdlib.h>
#include <string.h>
#include "sa_host.h"

uint32_t pe_image_base, pe_tls_index_addr, pe_tls_start, pe_tls_end;
char sa_exe_path[1024], sa_game_dir[1024];

extern const struct { uint32_t slot; const char *name; } rc_imports[];
extern const unsigned rc_imports_n;

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

uint32_t pe_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) sa_fatal("game exe not found: %s", path);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) sa_fatal("reading %s", path); fclose(f);
    uint32_t pe = u32(d + 0x3C);
    if (memcmp(d + pe, "PE\0\0", 4)) sa_fatal("%s is not a PE", path);
    const uint8_t *fh = d + pe + 4, *oh = fh + 20;
    uint16_t nsec = u16(fh + 2), ohsz = u16(fh + 16);
    pe_image_base = u32(oh + 28);
    uint32_t entry = pe_image_base + u32(oh + 16), hdrsz = u32(oh + 60);
    memcpy(MEM + pe_image_base, d, hdrsz);
    const uint8_t *sh = oh + ohsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t va = u32(sh + 12), vs = u32(sh + 8), raw = u32(sh + 20), rs = u32(sh + 16);
        memset(MEM + pe_image_base + va, 0, vs);
        memcpy(MEM + pe_image_base + va, d + raw, rs < vs ? rs : vs);
    }
    /* static TLS (MSVC): data template + index address */
    const uint8_t *dd = oh + 96;
    uint32_t tls_rva = u32(dd + 9 * 8);
    if (tls_rva) {
        uint32_t t = pe_image_base + tls_rva;
        pe_tls_start = rd32(t); pe_tls_end = rd32(t + 4); pe_tls_index_addr = rd32(t + 8);
        wr32(pe_tls_index_addr, 0);
        uint32_t cb = rd32(t + 12);
        if (cb && rd32(cb)) sa_log("loader: %s has TLS callbacks (0x%08x) - not called for now", path, rd32(cb));
    }
    /* IAT -> magic addresses */
    for (unsigned i = 0; i < rc_imports_n; i++) wr32(rc_imports[i].slot, imp_magic_for(rc_imports[i].name));
    free(d);
    sa_log("loader: image 0x%08x, entry point 0x%08x, %u imports", pe_image_base, entry, rc_imports_n);
    return entry;
}

/* minimal TEB: [0] SEH chain, [4] stack top, [8] stack bottom, [0x18] self, [0x24] thread id, [0x2C] TLS array */
uint32_t thread_setup_current(int idx) {
    uint32_t teb = G_TEBS + 0x1000u * (uint32_t)idx;
    uint32_t top = G_STACKS + G_STACK_SIZE * (uint32_t)(idx + 1) - 16, lim = G_STACKS + G_STACK_SIZE * (uint32_t)idx;
    memset(MEM + teb, 0, 0x1000);
    wr32(teb + 0x00, 0xFFFFFFFFu);
    wr32(teb + 0x04, top + 16); wr32(teb + 0x08, lim);
    wr32(teb + 0x18, teb); wr32(teb + 0x24, 1000 + idx);
    if (pe_tls_index_addr) {
        uint32_t sz = pe_tls_end - pe_tls_start, blk = g_alloc(sz + 64, 1), arr = teb + 0x800;
        memcpy(MEM + blk, MEM + pe_tls_start, sz);
        wr32(arr, blk); wr32(teb + 0x2C, arr);
    }
    cpu.fs_base = teb; cpu.esp = top; cpu.fcw = 0x27F; cpu.top = 0;
    return top;
}
