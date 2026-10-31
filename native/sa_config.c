/* sa_config.c - réglages du lanceur commun : applique aleste.ini [config] dans config.txt avant le démarrage
 * (même logique que la DLL apm_x86.dll sous Windows : lignes CLE=valeur remplacées, commentaires Shift-JIS gardés). */
#include <stdlib.h>
#include <string.h>
#include "sa_host.h"

static char *slurp(const char *path, long *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)*n + 1); if (fread(b, 1, (size_t)*n, f) != (size_t)*n) {} fclose(f); b[*n] = 0; return b;
}
/* boutons de manette choisis dans le lanceur (n° XInput 1-14) pour : Tir, Power-up, Semi-auto, Blow, Pause, Reset */
int sa_padmap[6] = { 11, 12, 13, 14, 6, 0 };
static const char *padkeys[6] = { "BUTTON_01", "BUTTON_02", "BUTTON_03", "BUTTON_04", "BUTTON_PAUSE", "BUTTON_RESET" };
/* valeurs fixées dans config.txt : le jeu lit chaque fonction sur un bouton borne (n° = Input_isOn - 1) */
static const char *fixkeys[10] = { "BUTTON_UP", "BUTTON_DOWN", "BUTTON_LEFT", "BUTTON_RIGHT", "BUTTON_01", "BUTTON_02", "BUTTON_03", "BUTTON_04", "BUTTON_PAUSE", "BUTTON_RESET" };
static const char *fixvals[10] = { "1", "2", "3", "4", "6", "7", "8", "9", "10", "11" };
void config_sync(void) {
    long ni, nc; char *ini = slurp("aleste.ini", &ni);
    if (!ini) { ini = malloc(1); ini[0] = 0; ni = 0; }
    char *cfg = slurp("config.txt", &nc); if (!cfg) { free(ini); return; }
    /* paires CLE=valeur de la section [config] */
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
    sa_log("manette : Tir=%d Power-up=%d Semi-auto=%d Blow=%d Pause=%d Reset=%d (n° de bouton du lanceur)", sa_padmap[0], sa_padmap[1], sa_padmap[2], sa_padmap[3], sa_padmap[4], sa_padmap[5]);
    char *out = malloc((size_t)nc + (size_t)ni + 64); size_t o = 0; int changed = 0;
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
    if (changed) { FILE *f = fopen("config.txt", "wb"); if (f) { fwrite(out, 1, o, f); fclose(f); sa_log("config.txt mis à jour depuis aleste.ini (lanceur)"); } }
    free(out); free(cfg); free(ini);
}
