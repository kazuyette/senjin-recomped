/* kit_launcher.c - lanceur Win32 commun, MÊME STYLE que le lanceur F355 / Soul Calibur :
   fond fenêtre système (blanc), police système, onglets natifs (Common Controls 6, DPI), libellés transparents,
   ligne d'état en bas, boutons  [Enregistrer]            [Jouer] [Quitter].
   Titre : "<nom> - Lanceur - version <X.Y.Z+bN>"  (version.h généré par gen_version.py).

   Piloté par launcher.ini (UTF-8) ; édite le .ini du jeu (API profil Windows) puis lance l'exe.
     [launcher]
     title=Soul Calibur              ; défaut : KIT_NAME
     game_ini=soulcalibur_recomp.ini
     exe=soulcalibur_recomp.exe      ; défaut : KIT_EXE
     args=
     [tab.Affichage]                 ; un onglet par section "tab.<nom>", dans l'ordre du fichier
     header=Texte d'intro en gras (optionnel)
     f1=video/fullscreen|bool|Plein écran
     f2=video/scale|choice|Échelle|1,2,3,4|3      ; ou "Libellé=valeur,..." (valeur en ASCII)
     f3=audio/volume|int|Volume (0-100)|0-100|100
     f4=paths/roms|path|Dossier des ROM
     f5=game/name|text|Pseudo
     f6=input/shot|pad|Tir||11     ; capture à la manette : clic puis appui sur un bouton (Échap annule, Suppr = aucun)
     [pad]                           ; table des boutons pour les champs "pad" : Nom:valeur écrite:masque XInput
     map=Haut:1:0x0001,Bas:2:0x0002,...,A:11:0x1000   ; masques 0x10000 = LT, 0x20000 = RT
   Icône optionnelle : icon.ico à la racine du projet (build.sh l'embarque, ressource n°2). */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include "kit_extract.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "version.h"

#define MAXF 96
#define MAXT 12
enum { K_BOOL, K_CHOICE, K_INT, K_PATH, K_TEXT, K_PAD };
typedef struct { int tab, kind, lo, hi, pv; char sec[48], key[48], opts[256], def[64]; wchar_t label[96]; HWND h, hb, hl; } Field;
typedef struct { wchar_t name[48]; wchar_t header[160]; HWND hh; } Tab;
static Field F[MAXF]; static int nf;
static Tab T[MAXT]; static int nt;
static char ini_game[MAX_PATH * 2]; static wchar_t exe_name[MAX_PATH], exe_args[512], dir[MAX_PATH], title[200], gameini_w[MAX_PATH];
static HWND hMain, hTab, hStatus; static HFONT hFont, hFontB; static HINSTANCE hInst; static int dpi = 96;
#define S(x) MulDiv((x), dpi, 96)
/* ---- [extract] : glisser-déposer de l'image du jeu ---- */
static int X_on; static char X_dir[260] = "data", X_accept[160] = "cue,bin,iso,img,chd,zip,7z", X_verify[512], X_set[100], X_cmd[700], X_mode[16] = "use";
static HWND hProg; static volatile LONG g_busy; static wchar_t g_msg[400];
#define WM_KX_PROG (WM_APP + 1)
#define WM_KX_DONE (WM_APP + 2)
enum { ID_TAB = 100, ID_PLAY, ID_SAVE, ID_QUIT, ID_BROWSE0 = 200, ID_PAD0 = 400 };
/* ---- champs "pad" : capture d'un bouton de manette XInput ---- */
#define MAXP 32
typedef struct { wchar_t name[24]; int val; DWORD mask; } PadKey;
static PadKey P[MAXP]; static int np_; static int cap = -1, cap_armed; static DWORD (WINAPI *pXGS)(DWORD, void *);
typedef struct { DWORD pkt; WORD btn; BYTE lt, rt; SHORT lx, ly, rx, ry; } KXState;

static void W(const char *u, wchar_t *w, int n) { MultiByteToWideChar(CP_UTF8, 0, u, -1, w, n); }
static void a2w(const char *a, wchar_t *w, int n) { MultiByteToWideChar(CP_ACP, 0, a, -1, w, n); }
static char *trim(char *s) { while (*s == ' ' || *s == '\t') s++; char *e = s + strlen(s); while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0; return s; }

/* ---- launcher.ini : lecteur UTF-8 minimal (garde l'ordre du fichier ; lignes commençant par ; ou # = commentaires) ---- */
static void parse_field(int t, const char *val) {
    char buf[600]; strncpy(buf, val, 599); buf[599] = 0;
    char *part[6] = { 0 }; int np = 0; part[np++] = buf;
    for (char *p = buf; *p && np < 6; p++) if (*p == '|') { *p = 0; part[np++] = p + 1; }
    if (np < 3 || nf >= MAXF) return;
    char *sl = strchr(part[0], '/'); if (!sl) return; *sl = 0;
    Field *f = &F[nf]; memset(f, 0, sizeof *f); f->tab = t;
    strncpy(f->sec, trim(part[0]), 47); strncpy(f->key, trim(sl + 1), 47);
    char *k = trim(part[1]);
    f->kind = !strcmp(k, "bool") ? K_BOOL : !strcmp(k, "choice") ? K_CHOICE : !strcmp(k, "int") ? K_INT : !strcmp(k, "path") ? K_PATH : !strcmp(k, "pad") ? K_PAD : K_TEXT;
    W(trim(part[2]), f->label, 96);
    if (np > 3) strncpy(f->opts, trim(part[3]), 255);
    if (np > 4) strncpy(f->def, trim(part[4]), 63);
    if (f->kind == K_INT) { f->lo = 0; f->hi = 100; sscanf(f->opts, "%d-%d", &f->lo, &f->hi); }
    nf++;
}
static char L_title[160], L_game[MAX_PATH], L_exe[MAX_PATH], L_args[512];
static void load_launcher_ini(const wchar_t *path) {
    FILE *fp = _wfopen(path, L"rb"); if (!fp) return;
    char line[800], sec[64] = ""; int tab = -1;
    while (fgets(line, sizeof line, fp)) {
        char *s = line; if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB) s += 3;
        s = trim(s); if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') { char *e = strchr(s, ']'); if (!e) continue; *e = 0; strncpy(sec, s + 1, 63); tab = -1;
            if (!strncmp(sec, "tab.", 4) && nt < MAXT) { W(sec + 4, T[nt].name, 48); tab = nt++; } continue; }
        char *eq = strchr(s, '='); if (!eq) continue; *eq = 0; char *k = trim(s), *v = trim(eq + 1);
        if (!strcmp(sec, "extract")) { X_on = 1;
            if (!strcmp(k, "dir")) strncpy(X_dir, v, 259); else if (!strcmp(k, "accept")) strncpy(X_accept, v, 159);
            else if (!strcmp(k, "verify")) strncpy(X_verify, v, 511); else if (!strcmp(k, "set")) strncpy(X_set, v, 99); else if (!strcmp(k, "cmd")) strncpy(X_cmd, v, 699); else if (!strcmp(k, "mode")) strncpy(X_mode, v, 15);
        } else if (!strcmp(sec, "launcher")) {
            if (!strcmp(k, "title")) strncpy(L_title, v, 159); else if (!strcmp(k, "game_ini")) strncpy(L_game, v, MAX_PATH - 1);
            else if (!strcmp(k, "exe")) strncpy(L_exe, v, MAX_PATH - 1); else if (!strcmp(k, "args")) strncpy(L_args, v, 511);
        } else if (!strcmp(sec, "pad") && !strcmp(k, "map")) {
            for (char *t = strtok(v, ","); t && np_ < MAXP; t = strtok(NULL, ",")) {
                char nm[64] = ""; int val = 0; unsigned mk_ = 0;
                char *c1 = strchr(t, ':'); if (!c1) continue; *c1 = 0; strncpy(nm, trim(t), 63);
                if (sscanf(c1 + 1, "%d:%i", &val, &mk_) != 2) continue;
                W(nm, P[np_].name, 24); P[np_].val = val; P[np_].mask = mk_; np_++; }
        } else if (tab >= 0) { if (!strcmp(k, "header")) W(v, T[tab].header, 160); else parse_field(tab, v); }
    }
    fclose(fp);
}

/* ---- contrôles ---- */
static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD st, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | st, S(x), S(y), S(w), S(h), hMain, (HMENU)(INT_PTR)id, hInst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)hFont, TRUE); return c;
}
static void pad_text(int i) {
    Field *f = &F[i]; wchar_t t[80];
    if (cap == i) wcscpy(t, L"Appuie sur un bouton...");
    else if (!f->pv) wcscpy(t, L"(aucun)");
    else { swprintf(t, 80, L"n° %d", f->pv); for (int k = 0; k < np_; k++) if (P[k].val == f->pv) { swprintf(t, 80, L"%s  (%d)", P[k].name, f->pv); break; } }
    SetWindowTextW(f->h, t);
}
static void pad_stop(void) { int i = cap; cap = -1; KillTimer(hMain, 1); if (i >= 0) pad_text(i); }
static void pad_start(int i) {
    if (cap >= 0) pad_stop();
    if (!pXGS) { HMODULE x = LoadLibraryW(L"xinput1_4.dll"); if (!x) x = LoadLibraryW(L"xinput1_3.dll"); if (!x) x = LoadLibraryW(L"xinput9_1_0.dll"); if (x) pXGS = (void *)GetProcAddress(x, "XInputGetState"); }
    if (!pXGS) { SetWindowTextW(hStatus, L"XInput introuvable : saisie à la manette impossible."); return; }
    cap = i; cap_armed = 0; pad_text(i); SetTimer(hMain, 1, 30, NULL);
    SetWindowTextW(hStatus, L"Appuie sur le bouton de la manette. Échap : annuler, Suppr : aucun bouton.");
}
static void pad_poll(void) {
    if (cap < 0) return;
    DWORD any = 0;
    for (DWORD u = 0; u < 4; u++) { KXState s; if (pXGS(u, &s)) continue;
        any |= s.btn; if (s.lt > 100) any |= 0x10000; if (s.rt > 100) any |= 0x20000; }
    if (!cap_armed) { if (!any) cap_armed = 1; return; }          /* attendre que tout soit relâché */
    if (!any) return;
    for (int k = 0; k < np_; k++) if (any & P[k].mask) {
        F[cap].pv = P[k].val; wchar_t m[120]; swprintf(m, 120, L"%s : %s", F[cap].label, P[k].name); SetWindowTextW(hStatus, m);
        pad_stop(); return; }
    SetWindowTextW(hStatus, L"Ce bouton n'est pas dans la table [pad] de launcher.ini.");
}
static void build_ui(void) {
    int y0[MAXT];
    for (int t = 0; t < nt; t++) {
        y0[t] = T[t].header[0] ? 74 : 48;
        if (T[t].header[0]) { T[t].hh = mk(L"STATIC", T[t].header, 0, 26, 44, 540, 22, 0); SendMessage(T[t].hh, WM_SETFONT, (WPARAM)hFontB, TRUE); }
    }
    for (int i = 0; i < nf; i++) {
        Field *f = &F[i]; int y = y0[f->tab]; y0[f->tab] += 32;
        char cur[512]; GetPrivateProfileStringA(f->sec, f->key, f->def, cur, sizeof cur, ini_game);
        wchar_t cw[512]; a2w(cur, cw, 512);
        if (f->kind == K_BOOL) { f->h = mk(L"BUTTON", f->label, BS_AUTOCHECKBOX | WS_TABSTOP, 26, y, 460, 22, 0); SendMessage(f->h, BM_SETCHECK, atoi(cur) ? BST_CHECKED : BST_UNCHECKED, 0); continue; }
        f->hl = mk(L"STATIC", f->label, 0, 26, y + 3, 220, 22, 0);
        if (f->kind == K_PAD) { f->pv = atoi(cur); f->h = mk(L"BUTTON", L"", WS_TABSTOP, 260, y, 200, 24, ID_PAD0 + i); pad_text(i); continue; }
        if (f->kind == K_CHOICE) {
            f->h = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 260, y, 200, 300, 0);
            char o[256]; strcpy(o, f->opts); int sel = 0, n = 0;
            for (char *p = strtok(o, ","); p; p = strtok(NULL, ",")) { p = trim(p); char *eq = strchr(p, '='); char *val = eq ? trim(eq + 1) : p; if (eq) *eq = 0;   /* "Libellé=valeur" ou "valeur" */
                wchar_t lw[100]; W(trim(p), lw, 100); SendMessageW(f->h, CB_ADDSTRING, 0, (LPARAM)lw); if (!strcmp(val, cur)) sel = n; n++; }
            SendMessage(f->h, CB_SETCURSEL, sel, 0);
        } else {
            DWORD es = f->kind == K_INT ? ES_NUMBER : ES_AUTOHSCROLL;
            f->h = mk(L"EDIT", cw, es | WS_TABSTOP, 260, y, f->kind == K_INT ? 80 : f->kind == K_PATH ? 290 : 200, 22, 0);
            SetWindowLongPtr(f->h, GWL_EXSTYLE, GetWindowLongPtr(f->h, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);
            SetWindowPos(f->h, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            if (f->kind == K_PATH) f->hb = mk(L"BUTTON", L"...", WS_TABSTOP, 556, y, 32, 22, ID_BROWSE0 + i);
        }
    }
}
static void show_tab(int t) {
    for (int i = 0; i < nt; i++) if (T[i].hh) ShowWindow(T[i].hh, i == t ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < nf; i++) { int v = F[i].tab == t ? SW_SHOW : SW_HIDE; ShowWindow(F[i].h, v); if (F[i].hb) ShowWindow(F[i].hb, v); if (F[i].hl) ShowWindow(F[i].hl, v); }
}
static void save(void) {
    for (int i = 0; i < nf; i++) {
        Field *f = &F[i]; char v[512] = ""; wchar_t w[512] = L"";
        if (f->kind == K_PAD) sprintf(v, "%d", f->pv);
        else if (f->kind == K_BOOL) strcpy(v, SendMessage(f->h, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
        else if (f->kind == K_CHOICE) { int s = (int)SendMessage(f->h, CB_GETCURSEL, 0, 0); char o[256]; strcpy(o, f->opts); int n = 0;
            for (char *p = strtok(o, ","); p; p = strtok(NULL, ","), n++) if (n == s) { p = trim(p); char *eq = strchr(p, '='); strcpy(v, eq ? trim(eq + 1) : p); break; } }
        else {
            GetWindowTextW(f->h, w, 512); WideCharToMultiByte(CP_ACP, 0, w, -1, v, sizeof v, NULL, NULL);
            if (f->kind == K_INT) { int n = atoi(v); if (n < f->lo) n = f->lo; if (n > f->hi) n = f->hi; sprintf(v, "%d", n); SetWindowTextA(f->h, v); }
        }
        WritePrivateProfileStringA(f->sec, f->key, v, ini_game);
    }
}
static int play(void) {
    save(); wchar_t cmd[1400], full[MAX_PATH * 2]; swprintf(full, MAX_PATH * 2, L"%s\\%s", dir, exe_name);
    swprintf(cmd, 1400, L"\"%s\" %s", full, exe_args);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return 1; }
    wchar_t m[700]; swprintf(m, 700, L"Impossible de lancer :\n%s", full); MessageBoxW(hMain, m, L"Lanceur", MB_ICONERROR); return 0;
}
static void browse(int i) {
    BROWSEINFOW bi = { 0 }; bi.hwndOwner = hMain; bi.lpszTitle = F[i].label; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST id = SHBrowseForFolderW(&bi); if (!id) return;
    wchar_t p[MAX_PATH]; if (SHGetPathFromIDListW(id, p)) SetWindowTextW(F[i].h, p); CoTaskMemFree(id);
}
/* ---- extraction (thread) ---- */
typedef struct { wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2]; } Job; static Job g_job;
static void u8(const wchar_t *w, char *o, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, o, n, NULL, NULL); }
static int run_wait(wchar_t *cmd, const wchar_t *cwd) {
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE); DWORD ec = 1; GetExitCodeProcess(pi.hProcess, &ec); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return (int)ec;
}
static void kx_cb(void *ud, unsigned long long d, unsigned long long t, const char *what) {
    static int last = -1; int pc = t ? (int)(d * 100 / t) : 0;
    if (pc != last) { last = pc; PostMessage(hMain, WM_KX_PROG, pc, 0); }
}
static int ext_is(const wchar_t *f, const wchar_t *e) { size_t n = wcslen(f), m = wcslen(e); return n > m && !_wcsicmp(f + n - m, e); }
static int image_extract(const wchar_t *src, const wchar_t *dst, char *err, int en) {
    char s8[1100], d8[1100]; u8(src, s8, sizeof s8); u8(dst, d8, sizeof d8); int nf = 0;
    for (char *p = s8; *p; p++) if (*p == '\\') *p = '/'; for (char *p = d8; *p; p++) if (*p == '\\') *p = '/';
    return kx_extract_image(s8, d8, kx_cb, NULL, err, en, &nf);
}
static int find_first_image(const wchar_t *dir, wchar_t *out) {          /* .cue d'abord, puis .iso, .chd */
    static const wchar_t *ex[] = { L"*.cue", L"*.iso", L"*.chd" };
    for (int i = 0; i < 3; i++) { wchar_t pat[MAX_PATH * 2]; swprintf(pat, MAX_PATH * 2, L"%s\\%s", dir, ex[i]); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) { swprintf(out, MAX_PATH * 2, L"%s\\%s", dir, fd.cFileName); FindClose(h); return 1; } }
    return 0;
}
static DWORD WINAPI worker(LPVOID p) {
    Job *j = &g_job; char err[300] = ""; int rc = 0; wchar_t cmd[2400], img[MAX_PATH * 2];
    SHCreateDirectoryExW(NULL, j->dst, NULL);
    if (X_cmd[0]) {                                                          /* extracteur propre au projet : {src} {dst} */
        wchar_t t[900], *a; W(X_cmd, t, 900); cmd[0] = 0;
        for (wchar_t *q = t; *q;) { if (!wcsncmp(q, L"{src}", 5)) { wcscat(cmd, j->src); q += 5; } else if (!wcsncmp(q, L"{dst}", 5)) { wcscat(cmd, j->dst); q += 5; } else { size_t n = wcslen(cmd); cmd[n] = *q++; cmd[n + 1] = 0; } }
        (void)a; rc = run_wait(cmd, dir); if (rc) snprintf(err, sizeof err, "L'extracteur du projet a échoué (code %d)", rc);
    } else if (ext_is(j->src, L".zip") || ext_is(j->src, L".7z") || ext_is(j->src, L".tar") || ext_is(j->src, L".rar")) {
        swprintf(cmd, 2400, L"tar.exe -xf \"%s\" -C \"%s\"", j->src, j->dst);       /* bsdtar de Windows 10+ */
        rc = run_wait(cmd, dir); if (rc) snprintf(err, sizeof err, "tar.exe a échoué (code %d)", rc);
        else if (find_first_image(j->dst, img)) { wchar_t d2[MAX_PATH * 2]; swprintf(d2, MAX_PATH * 2, L"%s\\files", j->dst); rc = image_extract(img, d2, err, sizeof err); }
    } else if (ext_is(j->src, L".chd")) {
        wchar_t tmp[MAX_PATH * 2], cue[MAX_PATH * 2], bin[MAX_PATH * 2]; swprintf(tmp, MAX_PATH * 2, L"%s\\_tmp", j->dst); SHCreateDirectoryExW(NULL, tmp, NULL);
        swprintf(cue, MAX_PATH * 2, L"%s\\disc.cue", tmp); swprintf(bin, MAX_PATH * 2, L"%s\\disc.bin", tmp);
        swprintf(cmd, 2400, L"chdman.exe extractcd -f -i \"%s\" -o \"%s\" -ob \"%s\"", j->src, cue, bin);
        rc = run_wait(cmd, dir); if (rc) snprintf(err, sizeof err, "chdman.exe introuvable ou en échec (le mettre à côté du lanceur)");
        else { rc = image_extract(cue, j->dst, err, sizeof err); DeleteFileW(cue); DeleteFileW(bin); RemoveDirectoryW(tmp); }
    } else rc = image_extract(j->src, j->dst, err, sizeof err);
    if (rc) { wchar_t w[300]; W(err[0] ? err : "Échec de l'extraction", w, 300); swprintf(g_msg, 400, L"Échec : %s", w); }
    PostMessage(hMain, WM_KX_DONE, rc, 0); return 0;
}
static int has_file(const wchar_t *dir0, const wchar_t *name, int depth) {      /* recherche récursive (insensible à la casse) */
    wchar_t pat[MAX_PATH * 2]; swprintf(pat, MAX_PATH * 2, L"%s\\*", dir0); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); int ok = 0; if (h == INVALID_HANDLE_VALUE) return 0;
    do { if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (!_wcsicmp(fd.cFileName, name)) { ok = 1; break; }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && depth < 4) { wchar_t sub[MAX_PATH * 2]; swprintf(sub, MAX_PATH * 2, L"%s\\%s", dir0, fd.cFileName); if (has_file(sub, name, depth + 1)) { ok = 1; break; } }
    } while (FindNextFileW(h, &fd));
    FindClose(h); return ok;
}
static int accepted(const wchar_t *f) {
    if (X_cmd[0] || !X_accept[0]) return 1; wchar_t a[200]; W(X_accept, a, 200);
    for (wchar_t *t = wcstok(a, L", "); t; t = wcstok(NULL, L", ")) { wchar_t e[40]; swprintf(e, 40, L".%s", t); if (ext_is(f, e)) return 1; }
    return 0;
}
static void start_extract(const wchar_t *src) {
    if (!X_on) return;
    if (g_busy) { SetWindowTextW(hStatus, L"Une extraction est déjà en cours."); return; }
    if (!accepted(src)) { wchar_t m[300]; swprintf(m, 300, L"Format non pris en charge (attendu : %S).", X_accept); SetWindowTextW(hStatus, m); return; }
    if (_stricmp(X_mode, "extract")) {                                       /* mode "use" (défaut) : on retient l'image, on n'extrait rien */
        if (X_set[0]) { char k[100]; strcpy(k, X_set); char *sl = strchr(k, '/'); if (sl) { *sl = 0; char a8[1100]; WideCharToMultiByte(CP_ACP, 0, src, -1, a8, sizeof a8, NULL, NULL); WritePrivateProfileStringA(k, sl + 1, a8, ini_game);
            for (int i = 0; i < nf; i++) if (!strcmp(F[i].sec, k) && !strcmp(F[i].key, sl + 1)) SetWindowTextA(F[i].h, a8); } }
        wchar_t m[MAX_PATH * 2 + 40]; swprintf(m, MAX_PATH * 2 + 40, L"Image enregistrée (utilisée telle quelle, sans extraction) : %s", src); SetWindowTextW(hStatus, m); return;
    }
    wchar_t stem[MAX_PATH]; const wchar_t *b = wcsrchr(src, L'\\'); wcscpy(stem, b ? b + 1 : src); wchar_t *dot = wcsrchr(stem, L'.'); if (dot) *dot = 0;
    wchar_t xd[260]; W(X_dir, xd, 260);
    wcscpy(g_job.src, src); swprintf(g_job.dst, MAX_PATH * 2, L"%s\\%s\\%s", dir, xd, stem);
    swprintf(g_msg, 400, L"Extraction de %s ...", stem); SetWindowTextW(hStatus, g_msg);
    SendMessage(hProg, PBM_SETPOS, 0, 0); ShowWindow(hProg, SW_SHOW); InterlockedExchange(&g_busy, 1);
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL); if (t) CloseHandle(t);
}
static void extract_done(int rc) {
    InterlockedExchange(&g_busy, 0); ShowWindow(hProg, SW_HIDE);
    if (rc) { SetWindowTextW(hStatus, g_msg); return; }
    wchar_t msg[800]; swprintf(msg, 800, L"Extraction terminée dans : %s", g_job.dst);
    char v[512]; strcpy(v, X_verify); wchar_t miss[300] = L"";
    for (char *t = strtok(v, ","); t; t = strtok(NULL, ",")) { wchar_t w[120]; W(trim(t), w, 120); if (*w && !has_file(g_job.dst, w, 0)) { wcscat(miss, w); wcscat(miss, L" "); } }
    if (miss[0]) swprintf(msg, 800, L"Extraction terminée, mais fichiers attendus absents : %s(mauvaise image ?)", miss);
    else if (X_set[0]) {                                                       /* mémorise le dossier dans le .ini du jeu */
        char k[100]; strcpy(k, X_set); char *sl = strchr(k, '/'); if (sl) { *sl = 0; char d8[1100]; WideCharToMultiByte(CP_ACP, 0, g_job.dst, -1, d8, sizeof d8, NULL, NULL); WritePrivateProfileStringA(k, sl + 1, d8, ini_game);
            for (int i = 0; i < nf; i++) if (!strcmp(F[i].sec, k) && !strcmp(F[i].key, sl + 1)) SetWindowTextA(F[i].h, d8); }
    }
    SetWindowTextW(hStatus, msg);
}

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_PLAY) { if (play()) PostMessage(h, WM_CLOSE, 0, 0); }
        else if (id == ID_SAVE) { save(); SetWindowTextW(hStatus, L"Réglages enregistrés."); }
        else if (id == ID_QUIT) PostMessage(h, WM_CLOSE, 0, 0);
        else if (id >= ID_BROWSE0 && id < ID_BROWSE0 + MAXF) browse(id - ID_BROWSE0);
        else if (id >= ID_PAD0 && id < ID_PAD0 + MAXF) pad_start(id - ID_PAD0);
        return 0; }
    case WM_TIMER: if (w == 1) pad_poll(); return 0;
    case WM_DROPFILES: { wchar_t f[MAX_PATH * 2]; if (DragQueryFileW((HDROP)w, 0, f, MAX_PATH * 2)) start_extract(f); DragFinish((HDROP)w); return 0; }
    case WM_KX_PROG: SendMessage(hProg, PBM_SETPOS, w, 0); return 0;
    case WM_KX_DONE: extract_done((int)w); return 0;
    case WM_NOTIFY: if (((NMHDR *)l)->idFrom == ID_TAB && ((NMHDR *)l)->code == TCN_SELCHANGE) pad_stop(), show_tab(TabCtrl_GetCurSel(hTab)); return 0;
    case WM_CTLCOLORSTATIC:   /* libellés posés sur l'onglet : fond transparent ; ligne d'état : fond fenêtre */
        SetBkMode((HDC)w, TRANSPARENT); return (HWND)l == hStatus ? (LRESULT)GetSysColorBrush(COLOR_WINDOW) : (LRESULT)GetStockObject(NULL_BRUSH);
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cl, int sh) {
    hInst = hi; INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS }; InitCommonControlsEx(&ic); CoInitialize(NULL);
    HDC dc = GetDC(NULL); dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(NULL, dc);
    GetModuleFileNameW(NULL, dir, MAX_PATH); *wcsrchr(dir, L'\\') = 0;
    wchar_t lp[MAX_PATH * 2]; swprintf(lp, MAX_PATH * 2, L"%s\\launcher.ini", dir); load_launcher_ini(lp);
    if (!L_game[0]) strcpy(L_game, "game.ini");
    a2w(L_game, gameini_w, MAX_PATH);
    { wchar_t g[MAX_PATH * 2]; swprintf(g, MAX_PATH * 2, L"%s\\%s", dir, gameini_w); WideCharToMultiByte(CP_ACP, 0, g, -1, ini_game, sizeof ini_game, NULL, NULL); }
    { wchar_t nm[160]; W(L_title[0] ? L_title : KIT_NAME, nm, 160); W(L_exe[0] ? L_exe : KIT_EXE, exe_name, MAX_PATH); W(L_args, exe_args, 512);
      swprintf(title, 200, L"%s - Lanceur - version %S", nm, KIT_VERSION_FULL); }
    NONCLIENTMETRICSW nm = { sizeof nm }; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
    hFont = CreateFontIndirectW(&nm.lfMessageFont); nm.lfMessageFont.lfWeight = FW_BOLD; hFontB = CreateFontIndirectW(&nm.lfMessageFont);
    WNDCLASSEXW wc = { sizeof wc }; wc.lpfnWndProc = wp; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW); wc.lpszClassName = L"KitLanceur"; wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(2)); RegisterClassExW(&wc);
    DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX; RECT r = { 0, 0, S(610), S(512) }; AdjustWindowRect(&r, st, FALSE);
    hMain = CreateWindowExW(0, L"KitLanceur", title, st, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, hi, 0);
    hTab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, S(10), S(10), S(590), S(410), hMain, (HMENU)(INT_PTR)ID_TAB, hi, 0);
    SendMessage(hTab, WM_SETFONT, (WPARAM)hFont, TRUE);
    for (int i = 0; i < nt; i++) { TCITEMW ti = { 0 }; ti.mask = TCIF_TEXT; ti.pszText = T[i].name; SendMessageW(hTab, TCM_INSERTITEMW, i, (LPARAM)&ti); }
    build_ui();
    wchar_t sttxt[400]; swprintf(sttxt, 400, L"Version %S. Réglages enregistrés dans %s.%s", KIT_VERSION_FULL, gameini_w, X_on ? L" Glisse l'image du jeu sur cette fenêtre." : L"");
    hStatus = mk(L"STATIC", sttxt, WS_VISIBLE, 12, 428, 586, 36, 0);
    hProg = mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, 12, 456, 586, 12, 0); SendMessage(hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    if (X_on) DragAcceptFiles(hMain, TRUE);
    mk(L"BUTTON", L"Enregistrer", WS_VISIBLE | WS_TABSTOP, 12, 472, 110, 30, ID_SAVE);
    mk(L"BUTTON", L"Jouer", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 376, 472, 105, 30, ID_PLAY);
    mk(L"BUTTON", L"Quitter", WS_VISIBLE | WS_TABSTOP, 492, 472, 105, 30, ID_QUIT);
    show_tab(0); ShowWindow(hMain, sh);
    { int ac; wchar_t **av = CommandLineToArgvW(GetCommandLineW(), &ac); if (av && ac > 1) start_extract(av[1]); }   /* image déposée sur l'icône du .exe */
    MSG msg; while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (cap >= 0 && msg.message == WM_KEYDOWN && (msg.wParam == VK_ESCAPE || msg.wParam == VK_DELETE || msg.wParam == VK_BACK)) {
            if (msg.wParam != VK_ESCAPE) F[cap].pv = 0;
            SetWindowTextW(hStatus, msg.wParam == VK_ESCAPE ? L"Saisie annulée." : L"Aucun bouton."); pad_stop(); continue; } if (!IsDialogMessage(hMain, &msg)) { TranslateMessage(&msg); DispatchMessage(&msg); } }
    return 0;
}
