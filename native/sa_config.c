/* sa_config.c - common launcher settings: applies aleste.ini [config] to config.txt before startup
 * (same logic as the apm_x86.dll DLL on Windows: KEY=value lines replaced, Shift-JIS comments kept). */
#include <stdlib.h>
#include <string.h>
#include "sa_host.h"

static char *slurp(const char *path, long *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)*n + 1); if (fread(b, 1, (size_t)*n, f) != (size_t)*n) {} fclose(f); b[*n] = 0; return b;
}
/* gamepad buttons chosen in the launcher (XInput no. 1-14) for: Shot, Power-up, Semi-auto, Blow, Pause, Reset */
int sa_padmap[6] = { 11, 12, 13, 14, 6, 0 };
static const char *padkeys[6] = { "BUTTON_01", "BUTTON_02", "BUTTON_03", "BUTTON_04", "BUTTON_PAUSE", "BUTTON_RESET" };
/* values fixed in config.txt: the game reads each function from a cabinet button (no. = Input_isOn - 1) */
static const char *fixkeys[10] = { "BUTTON_UP", "BUTTON_DOWN", "BUTTON_LEFT", "BUTTON_RIGHT", "BUTTON_01", "BUTTON_02", "BUTTON_03", "BUTTON_04", "BUTTON_PAUSE", "BUTTON_RESET" };
static const char *fixvals[10] = { "1", "2", "3", "4", "6", "7", "8", "9", "10", "11" };
void config_sync(void) {
    long ni, nc; char *ini = slurp("aleste.ini", &ni);
    if (!ini) { ini = malloc(1); ini[0] = 0; ni = 0; }
    char *cfg = slurp("config.txt", &nc); if (!cfg) { free(ini); return; }
    /* KEY=value pairs from the [config] section */
    char *keys[256], *vals[256]; int nk = 0, in = 0;
    for (char *l = strtok(ini, "\r\n"); l; l = strtok(NULL, "\r\n")) {
        while (*l == ' ' || *l == '\t') l++;
        if (*l == '[') { in = !strncmp(l, "[config]", 8); continue; }
        if (!in || *l == ';' || *l == '#') continue;
        char *eq = strchr(l, '='); if (!eq || nk >= 256) continue;
        *eq = 0;
        int pk = 0; for (int i = 0; i < 6; i++) if (!strcmp(l, padkeys[i])) { sa_padmap[i] = atoi(eq + 1); pk = 1; }
        if (!pk) { keys[nk] = l; vals[nk] = eq + 1; nk++; }
    }
    for (int i = 0; i < 10 && nk < 256; i++) { keys[nk] = (char *)fixkeys[i]; vals[nk] = (char *)fixvals[i]; nk++; }
    sa_log("gamepad: Shot=%d Power-up=%d Semi-auto=%d Blow=%d Pause=%d Reset=%d (launcher button no.)", sa_padmap[0], sa_padmap[1], sa_padmap[2], sa_padmap[3], sa_padmap[4], sa_padmap[5]);
    /* worst case: every line of config.txt is a replaced key that gets the longest value */
    size_t lines = 1; for (long i = 0; i < nc; i++) if (cfg[i] == '\n') lines++;
    size_t maxv = 0; for (int k = 0; k < nk; k++) { size_t l = strlen(vals[k]); if (l > maxv) maxv = l; }
    char *out = malloc((size_t)nc + lines * (maxv + 1) + 64); size_t o = 0; int changed = 0;
    for (char *p = cfg; *p; ) {
        char *e = p; while (*e && *e != '\n') e++;
        int done = 0;
        if (*p != ';') {
            char *eq = memchr(p, '=', (size_t)(e - p));
            if (eq) for (int k = 0; k < nk; k++) {
                size_t kl = strlen(keys[k]);
                if ((size_t)(eq - p) != kl || memcmp(p, keys[k], kl)) continue;
                char *ve = eq + 1; while (ve < e && *ve != '\r' && *ve != ';') ve++;
                size_t ol = (size_t)(ve - eq - 1), nl = strlen(vals[k]);
                if (ol != nl || memcmp(eq + 1, vals[k], nl)) changed = 1;
                memcpy(out + o, p, kl + 1); o += kl + 1; memcpy(out + o, vals[k], nl); o += nl;
                memcpy(out + o, ve, (size_t)(e - ve)); o += (size_t)(e - ve); done = 1; break;
            }
        }
        if (!done) { memcpy(out + o, p, (size_t)(e - p)); o += (size_t)(e - p); }
        if (*e == '\n') out[o++] = '\n', e++;
        p = e;
    }
    if (changed) { FILE *f = fopen("config.txt", "wb"); if (f) { fwrite(out, 1, o, f); fclose(f); sa_log("config.txt updated from aleste.ini (launcher)"); } }
    free(out); free(cfg); free(ini);
}
