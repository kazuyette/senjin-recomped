/* sa_imports.c - imports Windows de l'exe : table nom -> implémentation native, adresses magiques.
 * Un import appelé sans implémentation arrête le programme en donnant son nom (méthode : on les écrit
 * au fur et à mesure que le jeu les demande). */
#include <stdlib.h>
#include <string.h>
#include "sa_host.h"

#define MAXIMP 1024
static struct { char name[96]; void (*fn)(void); unsigned calls; } T[MAXIMP];
static unsigned nT;

static int find(const char *name) {
    for (unsigned i = 0; i < nT; i++) if (!strcmp(T[i].name, name)) return (int)i;
    return -1;
}
static int add(const char *name) {
    int i = find(name);
    if (i >= 0) return i;
    if (nT >= MAXIMP) sa_fatal("trop d'imports");
    strncpy(T[nT].name, name, 95); return (int)nT++;
}
uint32_t imp_magic_for(const char *name) { return G_MAGIC + 16u * (uint32_t)add(name); }
void imp_register(const imp_def *d) {
    for (; d->name; d++) { int i = add(d->name); T[i].fn = d->fn; }
}
int x86_import_call(uint32_t target) {
    if ((target & 0xFFF00000u) != G_MAGIC) return 0;
    unsigned i = (target - G_MAGIC) / 16;
    if (i >= nT) return 0;
    T[i].calls++;
    if (sa_verbose > 1) sa_log("import %s", T[i].name);
    if (!T[i].fn) sa_fatal("import non implémenté : %s (appelé depuis 0x%08x)", T[i].name, rd32(cpu.esp));
    T[i].fn();
    return 1;
}
void imports_report(void) {
    for (unsigned i = 0; i < nT; i++) if (T[i].calls) sa_log("  %-50s %u", T[i].name, T[i].calls);
}
int imp_has_fn(const char *name) { int i = find(name); return i >= 0 && T[i].fn != NULL; }
