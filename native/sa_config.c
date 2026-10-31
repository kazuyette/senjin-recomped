/* sa_config.c - common launcher settings: aleste.ini [config] <-> config.txt at startup (3-way merge, below;
 * same logic as the apm_x86.dll DLL on Windows: KEY=value lines replaced, Shift-JIS comments kept). */
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
/* ---- settings shared between the launcher and the game (3-way merge) ----
 * aleste.ini [config]  = values chosen in the launcher; config.txt = the game's own file, which it rewrites itself
 * (menu cursors: STORY_*, TRAINING_*...); aleste.ini [config.sync] = values applied at the previous start.
 * For each key: changed in the launcher since last time (aleste != sync) -> written to config.txt; otherwise, if
 * the game changed it (config.txt != aleste) -> copied back to aleste.ini, so the launcher shows it. Keys only found
 * in config.txt are added to aleste.ini. BUTTON_xx are handled apart (gamepad map + fixed cabinet numbers). */
typedef struct { char k[64]; char v[96]; } kv;
static int kv_find(kv *a, int n, const char *k) { for (int i = 0; i < n; i++) if (!strcmp(a[i].k, k)) return i; return -1; }
static int is_button(const char *k) { return !strncmp(k, "BUTTON_", 7); }
static void trimv(char *v) { size_t l = strlen(v); while (l && (v[l - 1] == ' ' || v[l - 1] == '\t' || v[l - 1] == '\r')) v[--l] = 0; }
#define MAXKV 256
void config_sync(void) {
    long ni, nc; char *ini = slurp("aleste.ini", &ni);
    if (!ini) { ini = malloc(1); ini[0] = 0; ni = 0; }
    char *cfg = slurp("config.txt", &nc); if (!cfg) { free(ini); return; }
    static kv A[MAXKV], S[MAXKV], C[MAXKV]; int na = 0, ns = 0, nC = 0;
    /* aleste.ini: [config] and [config.sync]; pad keys go to sa_padmap */
    { char *cp = strdup(ini); int sec = 0;
      for (char *l = strtok(cp, "\r\n"); l; l = strtok(NULL, "\r\n")) {
        while (*l == ' ' || *l == '\t') l++;
        if (*l == '[') { sec = !strncmp(l, "[config]", 8) ? 1 : !strncmp(l, "[config.sync]", 13) ? 2 : 0; continue; }
        if (!sec || *l == ';' || *l == '#') continue;
        char *eq = strchr(l, '='); if (!eq) continue; *eq = 0; char *v = eq + 1; trimv(v); trimv(l);
        if (sec == 1) { for (int i = 0; i < 6; i++) if (!strcmp(l, padkeys[i])) sa_padmap[i] = atoi(v);
                        if (!is_button(l) && na < MAXKV) { snprintf(A[na].k, 64, "%s", l); snprintf(A[na].v, 96, "%s", v); na++; } }
        else if (ns < MAXKV) { snprintf(S[ns].k, 64, "%s", l); snprintf(S[ns].v, 96, "%s", v); ns++; }
      }
      free(cp); }
    /* config.txt: KEY=value (comments start with ';') */
    for (char *p = cfg; *p; ) {
        char *e = p; while (*e && *e != '\n') e++;
        if (*p != ';' && *p != '\r' && p < e) {
            char *eq = memchr(p, '=', (size_t)(e - p));
            if (eq && eq - p < 63 && nC < MAXKV) { memcpy(C[nC].k, p, (size_t)(eq - p)); C[nC].k[eq - p] = 0;
                char *ve = eq + 1; while (ve < e && *ve != '\r' && *ve != ';') ve++;
                size_t vl = (size_t)(ve - eq - 1); if (vl > 95) vl = 95; memcpy(C[nC].v, eq + 1, vl); C[nC].v[vl] = 0; trimv(C[nC].v);
                if (!is_button(C[nC].k)) nC++; }
        }
        p = *e ? e + 1 : e;
    }
    /* merge */
    int ini_changed = 0, imported = 0;
    for (int i = 0; i < na; i++) {
        int si = kv_find(S, ns, A[i].k), ci = kv_find(C, nC, A[i].k);
        if (si >= 0 && !strcmp(S[si].v, A[i].v) && ci >= 0 && strcmp(C[ci].v, A[i].v)) { snprintf(A[i].v, 96, "%s", C[ci].v); ini_changed = 1; imported++; }
    }
    for (int c = 0; c < nC; c++) if (kv_find(A, na, C[c].k) < 0 && na < MAXKV) { A[na++] = C[c]; ini_changed = 1; imported++; }
    for (int i = 0; i < na; i++) { int si = kv_find(S, ns, A[i].k); if (si < 0 || strcmp(S[si].v, A[i].v)) ini_changed = 1; }
    if (imported) sa_log("config: %d setting(s) taken from config.txt (changed in the game)", imported);
    /* aleste.ini rewritten: other sections kept, [config] = launcher keys + buttons, [config.sync] = applied values */
    if (ini_changed) {
        size_t cap = (size_t)ni + (size_t)na * 2 * 170 + 256, o = 0; char *out = malloc(cap); int sec = 0, wrote = 0;
        for (char *p = ini; *p; ) {
            char *e = p; while (*e && *e != '\n') e++; char *n = *e ? e + 1 : e;
            char *l = p; while (l < e && (*l == ' ' || *l == '\t')) l++;
            if (*l == '[') sec = !strncmp(l, "[config]", 8) ? 1 : !strncmp(l, "[config.sync]", 13) ? 2 : 0;
            int keep = sec == 0 || (sec == 1 && (l == e || *l == ';' || *l == '#' || *l == '[' || is_button(l)));
            if (sec == 1 && *l == '[') keep = 1;
            if (sec == 2) keep = 0;
            if (keep) { memcpy(out + o, p, (size_t)(n - p)); o += (size_t)(n - p); if (n == e && e > p) { out[o++] = '\r'; out[o++] = '\n'; } }
            if (sec == 1 && *l == '[' && !wrote) { for (int i = 0; i < na; i++) o += (size_t)sprintf(out + o, "%s=%s\r\n", A[i].k, A[i].v); wrote = 1; }
            p = n;
        }
        if (!wrote) { o += (size_t)sprintf(out + o, "[config]\r\n"); for (int i = 0; i < na; i++) o += (size_t)sprintf(out + o, "%s=%s\r\n", A[i].k, A[i].v); }
        o += (size_t)sprintf(out + o, "[config.sync]\r\n"); for (int i = 0; i < na; i++) o += (size_t)sprintf(out + o, "%s=%s\r\n", A[i].k, A[i].v);
        FILE *f = fopen("aleste.ini", "wb"); if (f) { fwrite(out, 1, o, f); fclose(f); }
        free(out);
    }
    /* config.txt: launcher values + fixed button numbers */
    const char *keys[MAXKV + 10]; const char *vals[MAXKV + 10]; int nk = 0;
    for (int i = 0; i < na; i++) { keys[nk] = A[i].k; vals[nk] = A[i].v; nk++; }
    for (int i = 0; i < 10; i++) { keys[nk] = fixkeys[i]; vals[nk] = fixvals[i]; nk++; }
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
