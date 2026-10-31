/* sa_main.c - Senjin Aleste, native port (static recompilation): host entry point.
 * Usage: senjin_aleste [game_dir] [-v|-vv]   (folder containing senxin_aleste_apm3.exe and windata/) */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "sa_host.h"
#include "version.h"

void k32_register(void); void ole_register(void); void user32_register(void); void misc_register(void); void d3d9_register(void); void bcrypt_register(void); void dinput_register(void); void gdi_register(void);
void imports_report(void);
extern void call_ind(uint32_t);
static void on_usr1(int s) { (void)s; imports_report(); }
static void on_term(int s) { (void)s; _exit(130); }

int main(int argc, char **argv) {
    const char *dir = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) sa_verbose = 1; else if (!strcmp(argv[i], "-vv")) sa_verbose = 2; else dir = argv[i];
    }
    sa_log("Senjin Aleste native - version %s", SALE_VERSION_FULL);
    if (chdir(dir)) sa_fatal("game folder not found: %s", dir);
    snprintf(sa_game_dir, sizeof sa_game_dir, "%s", dir);
    { void config_sync(void); config_sync(); }
    mem_init();
    k32_register(); ole_register(); user32_register(); misc_register(); d3d9_register(); bcrypt_register(); dinput_register(); gdi_register();
    uint32_t entry = pe_load("senxin_aleste_apm3.exe");
    thread_setup_current(0);
    wr32(cpu.fs_base + 0x24, 1000);
    atexit(imports_report); signal(SIGUSR1, on_usr1); signal(SIGINT, on_term); signal(SIGTERM, on_term);
    push32(0xFFFFFFF0u);           /* return from the entry point: end */
    call_ind(entry);
    sa_log("entry point finished, eax=%u", cpu.eax);
    return (int)cpu.eax;
}
