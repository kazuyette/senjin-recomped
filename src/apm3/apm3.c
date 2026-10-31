/* apm3.c - Senjin Aleste : remplacement natif de apm_x86.dll (Sega ALL.Net P-ras Multi Ver.3).
 * Réimplémentation en code PC (pas d'émulation) des 106 services APM3 : free play, pas de réseau
 * ALL.Net par défaut, entrées système lues sur la manette XInput, chemins locaux.
 * Au chargement : applique les réglages de aleste.ini [config] dans config.txt (lu par le jeu).
 * Version : voir version.h (généré par _kit/version/gen_version.py). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "version.h"

#define API __stdcall
typedef DWORD D;

/* ---------- réglages (aleste.ini, section [apm]) ---------- */
static wchar_t g_root[MAX_PATH];          /* dossier de l'exe, avec '\' final */
static char g_ini[MAX_PATH], g_cfg[MAX_PATH], g_log[MAX_PATH];
static int g_online = 0, g_log_on = 0;
static wchar_t g_gameid[16] = L"SBRF", g_keychip[64] = L"A00E01A0000000000", g_location[64] = L"PC";

static void logf_(const char *fmt, ...) {
    if (!g_log_on) return;
    FILE *f = fopen(g_log, "a"); if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

/* Remplace les lignes KEY=... de config.txt par les valeurs de aleste.ini [config].
 * Travail octet par octet : les commentaires Shift-JIS et les fins de ligne sont conservés. */
/* Boutons : le lanceur choisit quel bouton de manette (n° XInput 1-14) fait Tir / Power-up / Semi-auto / Blow /
 * Pause / Reset (g_padmap) ; config.txt reçoit des valeurs fixes = boutons borne 1 à 6 (entrées 6..11), comme la
 * version Linux native. Les menus de la borne valident au bouton borne 1, donc au bouton réglé en Tir. */
static int g_padmap[6] = { 11, 12, 13, 14, 6, 0 };
static const char *g_padkeys[6] = { "BUTTON_01", "BUTTON_02", "BUTTON_03", "BUTTON_04", "BUTTON_PAUSE", "BUTTON_RESET" };
static void sync_config(void) {
    static char raw[16384], keys[16384];
    DWORD rn = GetPrivateProfileSectionA("config", raw, sizeof raw, g_ini);
    /* copie des paires sans les BUTTON_xx (gardés dans g_padmap), puis valeurs fixes */
    DWORD n = 0;
    for (char *k = raw; rn && *k; k += strlen(k) + 1) {
        int pk = 0; for (int i = 0; i < 6; i++) { size_t l = strlen(g_padkeys[i]); if (!strncmp(k, g_padkeys[i], l) && k[l] == '=') { g_padmap[i] = atoi(k + l + 1); pk = 1; } }
        if (!strncmp(k, "BUTTON_UP=", 10) || !strncmp(k, "BUTTON_DOWN=", 12) || !strncmp(k, "BUTTON_LEFT=", 12) || !strncmp(k, "BUTTON_RIGHT=", 13)) pk = 1;
        if (!pk) { size_t l = strlen(k) + 1; memcpy(keys + n, k, l); n += (DWORD)l; }
    }
    static const char *fixed[] = { "BUTTON_UP=1", "BUTTON_DOWN=2", "BUTTON_LEFT=3", "BUTTON_RIGHT=4", "BUTTON_01=6", "BUTTON_02=7",
        "BUTTON_03=8", "BUTTON_04=9", "BUTTON_PAUSE=10", "BUTTON_RESET=11" };
    for (int i = 0; i < 10; i++) { size_t l = strlen(fixed[i]) + 1; memcpy(keys + n, fixed[i], l); n += (DWORD)l; }
    keys[n] = 0;
    logf_("manette : Tir=%d Power-up=%d Semi-auto=%d Blow=%d Pause=%d Reset=%d", g_padmap[0], g_padmap[1], g_padmap[2], g_padmap[3], g_padmap[4], g_padmap[5]);
    FILE *f = fopen(g_cfg, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc(sz + 1); fread(src, 1, sz, f); fclose(f); src[sz] = 0;
    size_t cap = sz + n * 2 + 64; char *dst = malloc(cap); size_t o = 0; int changed = 0;
    char *p = src;
    while (*p) {
        char *e = p; while (*e && *e != '\n') e++;
        int handled = 0;
        if (*p != ';') {
            char *eq = memchr(p, '=', e - p);
            if (eq) {
                for (char *k = keys; *k; k += strlen(k) + 1) {
                    char *ke = strchr(k, '='); if (!ke) continue;
                    size_t kl = ke - k;
                    if ((size_t)(eq - p) == kl && !memcmp(p, k, kl)) {
                        char *vend = eq + 1; while (vend < e && *vend != '\r' && *vend != ';') vend++;
                        size_t ol = vend - (eq + 1), nl = strlen(ke + 1);
                        if (ol != nl || memcmp(eq + 1, ke + 1, nl)) changed = 1;
                        memcpy(dst + o, p, kl + 1); o += kl + 1;
                        memcpy(dst + o, ke + 1, nl); o += nl;
                        memcpy(dst + o, vend, e - vend); o += e - vend;
                        handled = 1; break;
                    }
                }
            }
        }
        if (!handled) { memcpy(dst + o, p, e - p); o += e - p; }
        if (*e == '\n') dst[o++] = '\n', e++;
        p = e;
    }
    if (changed && (f = fopen(g_cfg, "wb"))) { fwrite(dst, 1, o, f); fclose(f); logf_("config.txt mis à jour depuis aleste.ini"); }
    free(src); free(dst);
}

static int g_aime;
static void load_settings(void) {
    char tmp[128];
    g_aime = GetPrivateProfileIntA("apm", "aime", 0, g_ini);
    g_online = GetPrivateProfileIntA("apm", "online", 0, g_ini);
    g_log_on = GetPrivateProfileIntA("apm", "log", 0, g_ini);
    if (GetPrivateProfileStringA("apm", "game_id", "", tmp, sizeof tmp, g_ini)) MultiByteToWideChar(CP_ACP, 0, tmp, -1, g_gameid, 16);
    if (GetPrivateProfileStringA("apm", "keychip", "", tmp, sizeof tmp, g_ini)) MultiByteToWideChar(CP_ACP, 0, tmp, -1, g_keychip, 64);
    if (GetPrivateProfileStringA("apm", "location", "", tmp, sizeof tmp, g_ini)) MultiByteToWideChar(CP_ACP, 0, tmp, -1, g_location, 64);
}

void sale_log(const char *fmt, ...) {
    if (!g_log_on) return;
    FILE *f = fopen(g_log, "a"); if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}
void video_init(const char *ini);

/* ---------- entrées système (XInput, manette 0) ---------- */
typedef struct { DWORD pkt; WORD btn; BYTE lt, rt; SHORT lx, ly, rx, ry; } XST;
static DWORD (WINAPI *pXInputGetState)(DWORD, XST *);
static DWORD g_cur, g_prev;
/* Entrées APM -> jeu (0x40ab05, table de 16 enregistrements {0, logique, 1, 0, apm}) :
 *   Input_isOn(0) -> touche logique 14, Input_isOn(1) -> 15 (Test / Service probables, non câblés),
 *   Input_isOn(2..15) -> boutons n° 1..14 de config.txt (BUTTON_xx). [vérifié : table ; sens Test/Service = hypothèse]
 * Numéros (liste de config.txt) : 1 haut 2 bas 3 gauche 4 droite 5 Start 6 Back 7 LS 8 RS 9 LB 10 RB 11 A 12 B 13 X 14 Y.
 * Côté DLL Sega (0x10010d50) : id 0 Test, 1 Service, 2-5 haut/bas/gauche/droite, 6 Start, 7-14 boutons 1-8, 15 spécial
 *   -> n° 5 = Start ; pas de pièce : free play. [vérifié : tables de apm_x86.sega.dll]
 * Clavier : Z X C V = boutons borne 1-4 (Tir, Power-up, Semi-auto, Blow), P = Pause, Entrée = Start, flèches, Échap. */
static int g_exit;
static const WORD xbit[15] = { 0, 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000, 0x4000, 0x8000 };
static const struct { int vk, n; } kmap[] = { { 'Z', 6 }, { 'X', 7 }, { 'C', 8 }, { 'V', 9 }, { 'P', 10 },
    { VK_RETURN, 5 }, { VK_UP, 1 }, { VK_DOWN, 2 }, { VK_LEFT, 3 }, { VK_RIGHT, 4 } };
static void press(int n) { if (n >= 1 && n <= 14) g_cur |= 1u << (n + 1); }
static void aime_skip(void);
static void poll(void) {
    g_prev = g_cur; g_cur = 0;
    XST s;
    if (pXInputGetState && !pXInputGetState(0, &s)) {
        if (s.btn & 1) press(1); if (s.btn & 2) press(2); if (s.btn & 4) press(3); if (s.btn & 8) press(4); if (s.btn & 0x10) press(5);
        for (int f = 0; f < 6; f++) { int x = g_padmap[f]; if (x >= 1 && x <= 14 && (s.btn & xbit[x])) press(6 + f); }
        if (s.ly > 16000) press(1); if (s.ly < -16000) press(2);
        if (s.lx < -16000) press(3); if (s.lx > 16000) press(4);
    }
    HWND fg = GetForegroundWindow(); DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId())
    {
        for (unsigned k = 0; k < sizeof kmap / sizeof *kmap; k++) if (GetAsyncKeyState(kmap[k].vk) & 0x8000) press(kmap[k].n);
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) g_exit = 1;
    }
    aime_skip();
    if (g_cur != g_prev) logf_("entrées APM : %04lx", g_cur);
}
/* Aime : pas de lecteur sur PC ; écrans « Aime を使用しますか ? » (état 2) et « ゲームを開始します » (état 0xf)
 * validés tout seuls (« sans Aime » puis « commencer »). [apm] aime=1 pour les garder. Variables du jeu
 * 0x8efa30 (état) / 0x8efa28 (curseur), décalées si Windows a déplacé l'exe (ASLR). */
static void aime_skip(void) {
    static int phase;
    if (g_aime) return;
    BYTE *base = (BYTE *)GetModuleHandleA(NULL); ptrdiff_t d = base - (BYTE *)0x400000;
    volatile int *st = (volatile int *)((BYTE *)0x8efa30 + d), *cur = (volatile int *)((BYTE *)0x8efa28 + d);
    if (*st != 2 && *st != 0xf) { phase = 0; return; }
    *cur = *st == 2 ? 1 : 0;
    if (++phase % 2 == 0) press(6);
}
static int bit(DWORD v, D i) { return i < 16 ? (v >> i) & 1 : 0; }

/* ---------- tampons renvoyés par pointeur ---------- */
static BYTE g_zero[1024];
static struct { int major, minor; } g_gamever = { 1, 0 };
static wchar_t g_keychipbuf[64];          /* jeu : lit une chaîne large à +0x22 */

/* ---------- Aime (lecteur de carte) : absent ---------- */
D API Aime_acceptConfirm(void) { return 0; }
D API Aime_cancel(void) { return 0; }
D API Aime_getAccessCode(D a) { return 0; }
D API Aime_getAimeId(D a) { return 0; }
D API Aime_getConfirm(void) { return 0; }
D API Aime_getErrorCategory(void) { return 0; }
D API Aime_hasConfirm(void) { return 0; }
D API Aime_hasError(void) { return 0; }
D API Aime_hasResult(void) { return 0; }
D API Aime_isBusy(void) { return 0; }
D API Aime_isDBAlive(void) { return 0; }
D API Aime_isMobile(void) { return 0; }
D API Aime_isReaderDetected(void) { return 0; }
D API Aime_sendLog(D a, D b, D c) { return 0; }
D API Aime_setLedError(void) { return 0; }
D API Aime_setLedSuccess(void) { return 0; }
D API Aime_start(void) { return 0; }

/* ---------- ALL.Net ---------- */
D API AllnetAccounting_beginPlay(D a) { return 0; }
D API AllnetAccounting_endPlay(D a, D b, D c) { return 0; }
const wchar_t *API AllnetAuth_getAbaasGsServerName(void) { return L""; }
const wchar_t *API AllnetAuth_getAbaasLinkServerName(void) { return L""; }
const wchar_t *API AllnetAuth_getCountryCode(void) { return L"JPN"; }
D API AllnetAuth_getLocationId(void) { return 0; }
const wchar_t *API AllnetAuth_getLocationName(void) { return g_location; }
const wchar_t *API AllnetAuth_getLocationNickname(D a) { return g_location; }
D API AllnetAuth_getLocationNicknamePartCount(void) { return 1; }
D API AllnetAuth_getRegionCode(void) { return 0; }
const wchar_t *API AllnetAuth_getRegionName(D a) { return L""; }
D API AllnetAuth_getRegionNamePartCount(void) { return 0; }
D API AllnetAuth_isDevelop(void) { return 0; }
D API AllnetAuth_isGood(void) { return g_online; }

/* ---------- réglages borne ---------- */
D API ApmSystemSetting_getAdvertizeSound(D p) { return 1; }
D API ApmSystemSetting_getClosingTimes(D p) { return 0; }
D API ApmSystemSetting_getEmoneySetting(D p) { return 0; }
D API ApmSystemSetting_getFixedTitle(D p) { return 1; }   /* le jeu initialise *p = 0 lui-même */
D API ApmSystemSetting_getGamePadSetting(D p) { return 0; }
D API ApmSystemSetting_getMatchingGroup(D p) { return 0; }
D API ApmSystemSetting_getTimeToClosingTime(D p) { if (p) *(int *)p = -1; return 1; }   /* -1 : pas d'heure de fermeture */
D API ApmSystemSetting_getUiSetting(D p) { return 0; }

/* ---------- sauvegarde APM (non importée par le jeu : il gère save.dat lui-même) ---------- */
D API Backup_getRecordStatus(D a) { return 0; }
D API Backup_isSetupSucceeded(void) { return 1; }
D API Backup_saveRecord(D a) { return 1; }
D API Backup_saveRecordByAddress(D a) { return 1; }
D API Backup_setupRecords(D a, D b) { return 1; }

/* ---------- cœur ---------- */
D API Core_execute(void) { poll(); return 1; }             /* appelé à chaque image */
D API Core_exitGame(D a) { logf_("Core_exitGame(%lu)", a); ExitProcess(0); return 0; }
D API Core_hookException(void) { return 0; }
D API Core_isExitNeeded(void) { return g_exit; }   /* Échap */
D API Core_resetCount(void) { return 0; }
D API Core_resetException(void) { return 0; }

/* ---------- crédits : free play ---------- */
static D g_coinhook;
D API Credit_getAddableCoin(void) { return 0; }
const void *API Credit_getCoinMultipliers(void) { return g_zero; }
D API Credit_getCoinToCredit(void) { return 1; }
D API Credit_getCredit(void) { return 0; }
D API Credit_getRemain(void) { return 0; }
D API Credit_isFreePlay(void) { return 1; }
D API Credit_isGameCostEnough(D a) { return 1; }
D API Credit_isZero(void) { return 0; }
D API Credit_payGameCost(D a) { return 1; }
D API Credit_resetCoinInHook(void) { g_coinhook = 0; return 0; }
D API Credit_setCoinInHook(D fn) { g_coinhook = fn; return 1; }
const wchar_t *API Credit_toString(void) { return L"FREE PLAY"; }

/* ---------- monnaie électronique : absente ---------- */
D API Emoney_close(void) { return 0; }
D API Emoney_isOpenMainWindow(void) { return 0; }
D API Emoney_launch(void) { return 0; }
D API Emoney_setSoundHook(D a) { return 0; }
D API Emoney_update(D a) { return 0; }

D API Error_isOccurred(void) { return 0; }
D API Error_setDeviceLost(void) { return 0; }

/* ---------- entrées ---------- */
const void *API Input_getGamepadInfo(void) { return g_zero; }
D API Input_isFlipNow(D i) { return bit(g_cur ^ g_prev, i); }
D API Input_isGamepadDetect(void) { XST s; return pXInputGetState && !pXInputGetState(0, &s); }
D API Input_isOffNow(D i) { return bit(~g_cur & g_prev, i); }
D API Input_isOn(D i) { return bit(g_cur, i); }
D API Input_isOnNow(D i) { return bit(g_cur & ~g_prev, i); }
D API Input_isOpenPewviewWindow(void) { return 0; }
D API Input_setGamepadConfig(D a) { return 1; }

const wchar_t *API NetworkProperty_getAddressString(void) { return L"127.0.0.1"; }
D API NetworkProperty_getAddressValue(void) { return 0x7F000001; }

/* ---------- séquence de jeu ---------- */
D API Sequence_beginPlay(D a, D b, D c, D d, D e) { return 1; }
D API Sequence_clearBackup(void) { return 1; }
D API Sequence_continuePlay(D a, D b, D c, D d, D e, D f, D g) { return 1; }
D API Sequence_endPlay(D a, D b, D c, D d) { return 1; }
const void *API Sequence_getBookkeeping(void) { return g_zero; }
const void *API Sequence_getPlayingAimeId(void) { return g_zero; }
D API Sequence_isAccountingPlaying(void) { return 0; }
D API Sequence_isPlaying(void) { return 0; }
D API Sequence_isTest(void) { return 0; }

/* ---------- système ---------- */
const wchar_t *API System_getAppRootPath(void) { return g_root; }
const wchar_t *API System_getBoardId(void) { return L"PC"; }
const wchar_t *API System_getGameId(void) { return g_gameid; }
const void *API System_getGameVersion(void) { return &g_gamever; }
const void *API System_getKeychipId(void) { return g_keychipbuf; }
D API System_getRegionCode(void) { return 1; }
const void *API System_getResolution(void) { static int r[2] = { 1920, 1080 }; return r; }
D API System_isDevelop(void) { return 0; }

/* ---------- apmGamepad ---------- */
D API apmGamepadConfig(D a, D b, D c, D d, D e) { return 1; }
D API apmGamepadGetApmInput(D a) { return 0; }
const void *API apmGamepadGetConfig(void) { return g_zero; }
D API apmGamepadGetDeviceCaps(D a) { return 0; }
D API apmGamepadGetInfo(D a) { return 0; }
D API apmGamepadGetRawInput(D a) { return 0; }
D API apmGamepadInitConfig(void) { return 1; }
D API apmGamepadIsDetected(void) { return Input_isGamepadDetect(); }
D API apmGamepadIsXInputDevice(void) { return 1; }
D API apmGamepadUpdate(void) { return 1; }

/* version lisible par un outil externe */
__declspec(dllexport) const char *SALE_apmVersion(void) { return SALE_VERSION_FULL; }

/* Lecture XInput propre au jeu coupée (import par ordinal n° 2 de xinput1_3) : la manette passe par les entrées
 * borne (poll), sinon le jeu réinterpréterait les n° fixes de config.txt comme des boutons XInput. */
static DWORD WINAPI no_xinput(DWORD i, void *st) { (void)i; (void)st; return 1167; }   /* ERROR_DEVICE_NOT_CONNECTED */
static void hook_xinput(void) {
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *id = (void *)(base + dd.VirtualAddress); id->Name; id++) {
        if (_strnicmp((char *)base + id->Name, "xinput", 6)) continue;
        IMAGE_THUNK_DATA *on = (void *)(base + id->OriginalFirstThunk), *ft = (void *)(base + id->FirstThunk);
        for (; on->u1.AddressOfData; on++, ft++) {
            int is_get = IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal) ? IMAGE_ORDINAL(on->u1.Ordinal) == 2
                       : !strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + on->u1.AddressOfData))->Name, "XInputGetState");
            if (!is_get) continue;
            DWORD old; VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            ft->u1.Function = (ULONG_PTR)no_xinput; VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
            logf_("lecture XInput du jeu coupée (manette via les entrées borne)");
        }
    }
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    if (why != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *s = strrchr(exe, '\\'); if (s) s[1] = 0;
    snprintf(g_ini, MAX_PATH, "%saleste.ini", exe);
    snprintf(g_cfg, MAX_PATH, "%sconfig.txt", exe);
    snprintf(g_log, MAX_PATH, "%sapm3_log.txt", exe);
    MultiByteToWideChar(CP_ACP, 0, exe, -1, g_root, MAX_PATH);
    load_settings();
    /* +0 : id complet (16 car. max) ; +0x22 : n° de série de 11 car., longueur exigée par le jeu (0x53f1b0 : cmp len, 0xb) */
    wcsncpy(g_keychipbuf, g_keychip, 16); g_keychipbuf[16] = 0;
    wcsncpy(g_keychipbuf + 17, g_keychip, 11); g_keychipbuf[28] = 0;
    HMODULE x = LoadLibraryA("xinput1_4.dll"); if (!x) x = LoadLibraryA("xinput1_3.dll"); if (!x) x = LoadLibraryA("xinput9_1_0.dll");
    if (x) pXInputGetState = (void *)GetProcAddress(x, "XInputGetState");
    logf_("apm_x86.dll Senjin Aleste v%s (online=%d)", SALE_VERSION_FULL, g_online);
    sync_config();
    video_init(g_ini);
    hook_xinput();
    return TRUE;
}
