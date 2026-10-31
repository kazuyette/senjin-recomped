/* sa_misc.c - secondary imports rewritten natively: SHELL32, WINMM, network (disabled), crypto (disabled),
 * DirectInput (absent: the gamepad goes through XInput/SDL), XInput on SDL_GameController, D3DX math,
 * and the APM3 cabinet services (same logic as the project's native apm_x86.dll DLL: free play, offline). */
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "sa_host.h"

/* ---------- SHELL32 ---------- */
static void mkdir_p(char *p) { for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0777); *s = '/'; } mkdir(p, 0777); }
static void SHCreateDirectoryExW_(void) {
    char b[1024], p[1024]; g_wstr_to_utf8(ARG(1), b, sizeof b); int k = 0; const char *s = b;
    if (s[0] && s[1] == ':') s += 2; if (*s == '\\') s++;
    for (; *s && k < 1023; s++) p[k++] = *s == '\\' ? '/' : *s; p[k] = 0; if (k) mkdir_p(p);
    RET_STD(3, 0);
}
static void SHGetSpecialFolderPathW_(void) {     /* "AppData" folder -> the game's userdata subfolder */
    static uint32_t w; if (!w) w = g_wstrdup("W:\\userdata");   /* allocated once */
    uint32_t b = ARG(1), k = 0; while (rd16(w + 2 * k)) { wr16(b + 2 * k, rd16(w + 2 * k)); k++; } wr16(b + 2 * k, 0);
    mkdir("userdata", 0777); RET_STD(4, 1);
}
/* ---------- WINMM ---------- */
static void timeGetTime_(void) { RET_STD(0, (uint32_t)(sa_clock_ns() / 1000000u)); }
static void timeBeginPeriod_(void) { RET_STD(1, 0); }
static void PlaySoundA_(void) { RET_STD(3, 1); }
/* ---------- network: everything fails cleanly (offline game) ---------- */
static void ws_ok0(void) { RET_STD(0, 0); }
static void WSAStartup_(void) { memset(MEM + ARG(1), 0, 400); wr16(ARG(1), 0x0202); wr16(ARG(1) + 2, 0x0202); RET_STD(2, 0); }
static void WSAGetLastError_(void) { RET_STD(0, 10050); }       /* WSAENETDOWN */
static void WSASetLastError_(void) { RET_STD(1, 0); }
#define WSFAIL(name, n) static void name(void) { RET_STD(n, 0xFFFFFFFFu); }
WSFAIL(socket_, 3) WSFAIL(connect_, 3) WSFAIL(send_, 4) WSFAIL(recv_, 4) WSFAIL(bind_, 3) WSFAIL(getpeername_, 3) WSFAIL(getsockname_, 3)
WSFAIL(getsockopt_, 5) WSFAIL(setsockopt_, 5) WSFAIL(select_, 5) WSFAIL(ioctlsocket_, 3) WSFAIL(WSAIoctl_, 9)
static void closesocket_(void) { RET_STD(1, 0); }
static void htons_(void) { uint32_t v = ARG(0) & 0xFFFF; RET_STD(1, ((v & 0xFF) << 8) | (v >> 8)); }
static void FDIsSet_(void) { RET_STD(2, 0); }
static void getaddrinfo_(void) { if (ARG(3)) wr32(ARG(3), 0); RET_STD(4, 11001); }  /* WSAHOST_NOT_FOUND */
static void freeaddrinfo_(void) { RET_STD(1, 0); }
static void Netbios_(void) { RET_STD(1, 1); }
static void GetAdaptersInfo_(void) { RET_STD(2, 232); }                              /* ERROR_NO_DATA */
/* ---------- crypto (ALL.Net): unavailable ---------- */
#define FAILN(name, n) static void name(void) { RET_STD(n, 0); }
FAILN(CryptAcquireContextA_, 5) FAILN(CryptCreateHash_, 5) FAILN(CryptHashData_, 4) FAILN(CryptGetHashParam_, 5) FAILN(CryptDestroyHash_, 1) FAILN(CryptReleaseContext_, 2)
#define BFAIL(name, n) static void name(void) { RET_STD(n, 0xC0000001u); }
BFAIL(BCryptOpenAlgorithmProvider_, 4) BFAIL(BCryptCloseAlgorithmProvider_, 2) BFAIL(BCryptEncrypt_, 10) BFAIL(BCryptDecrypt_, 10)
BFAIL(BCryptDestroyKey_, 1) BFAIL(BCryptGenerateSymmetricKey_, 7) BFAIL(BCryptSetProperty_, 5)
/* ---------- DirectInput: not provided ---------- */
static void DirectInput8Create_(void) { if (ARG(3)) wr32(ARG(3), 0); RET_STD(5, 0x80004005u); }
/* ---------- XInput on SDL ---------- */
static SDL_GameController *pad;
/* "network" gamepad: under WSL2 the USB gamepad stays on the Windows side; aleste_pad.exe (Windows) reads XInput and sends
 * the state over TCP to localhost:SA_PADNET (16-byte packet: 'APAD', buttons, triggers, 4 axes) */
static volatile int netpad_sock = -1;
static struct { volatile int on; volatile uint32_t b; volatile uint8_t lt, rt; volatile int16_t ax[4]; } netpad;
static void *netpad_main(void *arg) {
    int port = (int)(intptr_t)arg, ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { sa_log("network gamepad: port %d unavailable", port); return NULL; }
    sa_log("network gamepad: waiting on port %d", port);
    for (;;) {
        int c = accept(ls, NULL, NULL); if (c < 0) continue;
        sa_log("network gamepad: connected"); uint8_t pk[16]; size_t got = 0; netpad_sock = c;
        for (;;) {
            ssize_t r = recv(c, pk + got, sizeof pk - got, 0); if (r <= 0) break; got += (size_t)r;
            if (got < sizeof pk) continue; got = 0;
            if (memcmp(pk, "APAD", 4)) continue;
            netpad.b = (uint32_t)(pk[4] | pk[5] << 8); netpad.lt = pk[6]; netpad.rt = pk[7];
            for (int i = 0; i < 4; i++) netpad.ax[i] = (int16_t)(pk[8 + 2 * i] | pk[9 + 2 * i] << 8);
            netpad.on = 1;
        }
        netpad.on = 0; netpad_sock = -1; close(c); sa_log("network gamepad: disconnected");
    }
    return NULL;
}
/* SDL: Y axis positive = down; XInput: positive = up. -(-32768) does not fit in 16 bits: full up read as full down. */
static int16_t neg16(int v) { return (int16_t)(v <= -32767 ? 32767 : -v); }
static int pad_state(uint32_t *btn, int16_t ax[4], uint8_t *lt, uint8_t *rt) {
    static int init; if (!init) { init = 1; const char *e = getenv("SA_PADNET"); if (e && atoi(e) > 0) { pthread_t t; pthread_create(&t, NULL, netpad_main, (void *)(intptr_t)atoi(e)); } }
    if (netpad.on) { *btn = netpad.b; for (int i = 0; i < 4; i++) ax[i] = netpad.ax[i]; *lt = netpad.lt; *rt = netpad.rt; return 1; }
    if (pad && !SDL_GameControllerGetAttached(pad)) { SDL_GameControllerClose(pad); pad = NULL; sa_log("gamepad unplugged"); }
    if (!pad) for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) sa_log("gamepad: %s", SDL_GameControllerName(pad)); break; }
    if (!pad) return 0;
    SDL_GameControllerUpdate();
    static const struct { int b; uint32_t m; } map[] = { { SDL_CONTROLLER_BUTTON_DPAD_UP, 1 }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 2 }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 4 },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 8 }, { SDL_CONTROLLER_BUTTON_START, 0x10 }, { SDL_CONTROLLER_BUTTON_BACK, 0x20 }, { SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x40 },
        { SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x80 }, { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x100 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x200 },
        { SDL_CONTROLLER_BUTTON_A, 0x1000 }, { SDL_CONTROLLER_BUTTON_B, 0x2000 }, { SDL_CONTROLLER_BUTTON_X, 0x4000 }, { SDL_CONTROLLER_BUTTON_Y, 0x8000 } };
    *btn = 0; for (unsigned i = 0; i < sizeof map / sizeof *map; i++) if (SDL_GameControllerGetButton(pad, map[i].b)) *btn |= map[i].m;
    ax[0] = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX); ax[1] = neg16(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY));
    ax[2] = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX); ax[3] = neg16(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY));
    *lt = (uint8_t)(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7); *rt = (uint8_t)(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
    return 1;
}
static void XInputGetState_(void) {
    uint32_t idx = ARG(0), st = ARG(1); uint32_t b; int16_t ax[4]; uint8_t lt, rt;
    /* gamepad 0 always "plugged in": the game only assigns its gamepads at startup (the WSL2 relay or a
     * gamepad plugged in later would never be picked up); with no gamepad, idle state */
    /* the gamepad goes entirely through the cabinet inputs (apm_poll): the game's own XInput reading
     * would reinterpret the config.txt numbers; so it stays "unplugged" (SA_XINPUT=1 to re-enable it) */
    static int xi = -1; if (xi < 0) xi = getenv("SA_XINPUT") != NULL;
    if (idx != 0 || !xi || !pad_state(&b, ax, &lt, &rt)) { RET_STD(2, 1167); return; }
    static uint32_t pkt; wr32(st, ++pkt); wr16(st + 4, (uint16_t)b); wr8(st + 6, lt); wr8(st + 7, rt);
    for (int i = 0; i < 4; i++) wr16(st + 8 + 2 * i, (uint16_t)ax[i]);
    RET_STD(2, 0);
}
static void XInputSetState_(void) {   /* rumble: SDL for a local gamepad, Windows relay for the network gamepad */
    uint32_t v = ARG(1); uint16_t lo = v ? rd16(v) : 0, hi = v ? rd16(v + 2) : 0;
    if (ARG(0) != 0) { RET_STD(2, 1167); return; }
    if (netpad.on) { int s = netpad_sock; if (s >= 0) { uint8_t pk[8] = { 'R', 'U', 'M', 'B', (uint8_t)lo, (uint8_t)(lo >> 8), (uint8_t)hi, (uint8_t)(hi >> 8) }; if (send(s, pk, 8, MSG_NOSIGNAL)) {} } RET_STD(2, 0); return; }
    if (pad) { SDL_GameControllerRumble(pad, lo, hi, 1000); RET_STD(2, 0); return; }
    RET_STD(2, 1167);
}

/* ---------- pause ----------
 * The "Pause" function of the launcher (gamepad, default Back) and the P / Pause keys freeze the game: sa_d3d9.c holds
 * the main thread in Present, the game clocks (QueryPerformanceCounter, GetTickCount, timeGetTime) stand still and the
 * XAudio2 mixer stops. The game's own pause input (cabinet button 5) has no visible effect on this build: not sent. */
volatile int sa_paused;
static uint64_t pause_total, pause_start;
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
uint64_t sa_clock_ns(void) { uint64_t n = sa_paused ? pause_start : mono_ns(); return n - pause_total; }
void sa_pause_set(int on) {
    if (!on == !sa_paused) return;
    if (on) { pause_start = mono_ns(); sa_paused = 1; }
    else { pause_total += mono_ns() - pause_start; sa_paused = 0; }
    if (on) {   /* stop the rumble */
        if (netpad.on) { int s = netpad_sock; if (s >= 0) { uint8_t pk[8] = { 'R', 'U', 'M', 'B', 0, 0, 0, 0 }; if (send(s, pk, 8, MSG_NOSIGNAL)) {} } }
        else if (pad) SDL_GameControllerRumble(pad, 0, 0, 0);
    }
    sa_log("pause: %s", on ? "on" : "off");
}
int sa_pause_button(void) {
    extern uint8_t sa_keys[256]; extern int sa_padmap[6];
    static const uint16_t xb[15] = { 0, 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000, 0x4000, 0x8000 };
    if (sa_keys['P'] || sa_keys[0x13]) return 1;
    uint32_t b; int16_t ax[4]; uint8_t lt, rt; int x = sa_padmap[4];
    return x >= 1 && x <= 14 && pad_state(&b, ax, &lt, &rt) && (b & xb[x]);
}

/* ---------- D3DX: math (4x4 float matrices, row-major) ---------- */
static void ld(uint32_t p, float m[16]) { memcpy(m, MEM + p, 64); }
static void st(uint32_t p, const float m[16]) { memcpy(MEM + p, m, 64); }
static void mul(const float a[16], const float b[16], float r[16]) { float t[16]; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; t[i * 4 + j] = s; } memcpy(r, t, 64); }
static void D3DXMatrixMultiply_(void) { float a[16], b[16], r[16]; ld(ARG(1), a); ld(ARG(2), b); mul(a, b, r); st(ARG(0), r); RET_STD(3, ARG(0)); }
static void D3DXMatrixScaling_(void) { float r[16] = { 0 }; memcpy(&r[0], MEM + cpu.esp + 8, 4); memcpy(&r[5], MEM + cpu.esp + 12, 4); memcpy(&r[10], MEM + cpu.esp + 16, 4); r[15] = 1; st(ARG(0), r); RET_STD(4, ARG(0)); }
static void D3DXMatrixTranspose_(void) { float a[16], r[16]; ld(ARG(1), a); for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r[i * 4 + j] = a[j * 4 + i]; st(ARG(0), r); RET_STD(2, ARG(0)); }
static void D3DXMatrixInverse_(void) {
    float m[16], inv[16]; ld(ARG(2), m);
    inv[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (ARG(1)) memcpy(MEM + ARG(1), &det, 4);
    if (det == 0) { RET_STD(3, 0); return; }
    for (int i = 0; i < 16; i++) inv[i] /= det; st(ARG(0), inv); RET_STD(3, ARG(0));
}
static void D3DXVec4Transform_(void) {
    float v[4], m[16], r[4]; memcpy(v, MEM + ARG(1), 16); ld(ARG(2), m);
    for (int j = 0; j < 4; j++) r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    memcpy(MEM + ARG(0), r, 16); RET_STD(3, ARG(0));
}
static void D3DXVec3Project_(void) {   /* out, v, viewport, proj, view, world */
    float v[3], w[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }, vi[16], p[16], m[16], r[4];
    memcpy(v, MEM + ARG(1), 12);
    if (ARG(5)) ld(ARG(5), w); memcpy(m, w, 64);
    if (ARG(4)) { ld(ARG(4), vi); mul(m, vi, m); } if (ARG(3)) { ld(ARG(3), p); mul(m, p, m); }
    for (int j = 0; j < 4; j++) r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + m[12 + j];
    if (r[3] != 0) { r[0] /= r[3]; r[1] /= r[3]; r[2] /= r[3]; }
    if (ARG(2)) { uint32_t vp = ARG(2); float x = (float)rd32(vp), y = (float)rd32(vp + 4), wd = (float)rd32(vp + 8), ht = (float)rd32(vp + 12), mn, mx;
        memcpy(&mn, MEM + vp + 16, 4); memcpy(&mx, MEM + vp + 20, 4);
        r[0] = x + (1 + r[0]) * wd / 2; r[1] = y + (1 - r[1]) * ht / 2; r[2] = mn + r[2] * (mx - mn); }
    memcpy(MEM + ARG(0), r, 12); RET_STD(6, ARG(0));
}

/* ---------- APM3 (cabinet): same behavior as the native apm_x86.dll DLL ---------- */
extern uint8_t sa_keys[256];
static uint32_t apm_cur, apm_prev; static int apm_exit;
static uint32_t wstr(const char *s) { return g_wstrdup(s); }
static void apm_poll(void) {
    apm_prev = apm_cur; apm_cur = 0; uint32_t b; int16_t ax[4]; uint8_t lt, rt;
    static const uint16_t xb[15] = { 0, 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000, 0x4000, 0x8000 };
#define PRESS(n) (apm_cur |= 1u << ((n) + 1))
    /* Cabinet inputs (Input_isOn(n+1)): 1-4 D-pad, 5 Start, 6-13 buttons 1 to 8 ("決定 : ボタン1" = 6).
     * The game reads Shot / Power-up / Semi-auto / Blow / Pause / Reset from cabinet buttons 1 to 6 (config.txt fixed
     * by sa_config.c); the launcher chooses which gamepad button triggers each function (pad_map). */
    extern int sa_padmap[6];
    if (pad_state(&b, ax, &lt, &rt)) {
        if (b & 1) PRESS(1); if (b & 2) PRESS(2); if (b & 4) PRESS(3); if (b & 8) PRESS(4); if (b & 0x10) PRESS(5);
        for (int f = 0; f < 6; f++) { int x = sa_padmap[f]; if (f != 4 && x >= 1 && x <= 14 && (b & xb[x])) PRESS(6 + f); }   /* f 4 = Pause: handled by the port */
        /* left stick = D-pad (same directions) */
        if (ax[1] > 16000) PRESS(1); if (ax[1] < -16000) PRESS(2); if (ax[0] < -16000) PRESS(3); if (ax[0] > 16000) PRESS(4);
    }
    /* keyboard: arrows = D-pad, Enter = Start, Z X C V = Shot / Power-up / Semi-auto / Blow, P = Pause, Esc = quit */
    static const struct { int vk, n; } km[] = { { 0x0D, 5 }, { 0x26, 1 }, { 0x28, 2 }, { 0x25, 3 }, { 0x27, 4 },
        { 'Z', 6 }, { 'X', 7 }, { 'C', 8 }, { 'V', 9 } };   /* P = pause of the port (sa_pause_button) */
    for (unsigned k = 0; k < sizeof km / sizeof *km; k++) if (sa_keys[km[k].vk]) PRESS(km[k].n);
    if (sa_keys[0x1B]) apm_exit = 1;
    static int kt = -1; if (kt < 0) kt = getenv("SA_KEYTEST") != NULL;
    if (kt) for (int f = 0; f < 12; f++) if (sa_keys[0x70 + f]) PRESS(3 + f);   /* test: F1..F12 -> no. 3..14 */               /* Esc: the game quits (Core_isExitNeeded) */
}
static int bit(uint32_t v, uint32_t i) { return i < 16 ? (v >> i) & 1 : 0; }
#define A0(name, val) static void name(void) { RET_STD(0, val); }
#define A1(name, val) static void name(void) { RET_STD(1, val); }
A0(apm_zero0, 0) A0(apm_one0, 1) A1(apm_zero1, 0) A1(apm_one1, 1)
static void apm_zero2(void) { RET_STD(2, 0); } static void apm_one2(void) { RET_STD(2, 1); }
static void apm_zero3(void) { RET_STD(3, 0); } static void apm_one4(void) { RET_STD(4, 1); }
static void apm_one5(void) { RET_STD(5, 1); } static void apm_one7(void) { RET_STD(7, 1); }
static void Core_isExitNeeded(void) { RET_STD(0, apm_exit); }
/* arcade closing time: -1 = none (otherwise the game shows "closing in 0 min") */
static void getTimeToClosingTime(void) { if (ARG(0)) wr32(ARG(0), 0xFFFFFFFFu); RET_STD(1, 1); }
/* Aime (player card): no reader on PC. The two screens "Aime を使用しますか ?" then "ゲームを開始します"
 * are confirmed automatically (choice "without Aime", then "start"); [apm] aime=1 in aleste.ini to keep them.
 * Screen state: 0x8efa30 (2 = Aime choice, 0xf = confirmation), cursor: 0x8efa28 (FUN_00433190). */
static void aime_skip(void) {
    static int on = -1, phase;
    if (on < 0) { on = 1; FILE *f = fopen("aleste.ini", "rb"); char l[256]; int sec = 0;
        if (f) { while (fgets(l, sizeof l, f)) { if (l[0] == '[') sec = !strncmp(l, "[apm]", 5); else if (sec && !strncmp(l, "aime=", 5)) on = atoi(l + 5) == 0; } fclose(f); } }
    if (!on) return;
    uint32_t st = rd32(0x8efa30);
    if (st != 2 && st != 0xf) { phase = 0; return; }
    wr32(0x8efa28, st == 2 ? 1 : 0);                       /* "without Aime" / "start" */
    if (++phase % 2 == 0) PRESS(6);                         /* button 1, every other frame (rising edge) */
}
static void Core_execute(void) { apm_poll(); aime_skip(); RET_STD(0, 1); }
static void Core_exitGame(void) { sa_log("APM: Core_exitGame(%u) (called from %08x)", ARG(0), rd32(cpu.esp)); fflush(NULL); exit(0); }
static void Input_isOn(void) { RET_STD(1, bit(apm_cur, ARG(0))); }
static void Credit_toString(void) { static uint32_t s; if (!s) s = wstr("FREE PLAY"); RET_STD(0, s); }
static void str_empty(void) { static uint32_t s; if (!s) s = wstr(""); RET_STD(0, s); }
static void str_loc(void) { static uint32_t s; if (!s) s = wstr("PC"); RET_STD(0, s); }
static void System_getGameId(void) { static uint32_t s; if (!s) s = wstr("SBRF"); RET_STD(0, s); }
static void System_getAppRootPath(void) { static uint32_t s; if (!s) s = wstr("W:\\"); RET_STD(0, s); }
static void System_getGameVersion(void) { static uint32_t s; if (!s) { s = g_static(8); wr32(s, 1); wr32(s + 4, 0); } RET_STD(0, s); }
static void System_getKeychipId(void) {   /* +0 full id, +0x22 11-character serial number (required by the game) */
    static uint32_t s; if (!s) { s = g_static(128); const char *k = "A00E01A0000000000"; for (int i = 0; i < 16; i++) wr16(s + 2 * i, k[i]); for (int i = 0; i < 11; i++) wr16(s + 0x22 + 2 * i, k[i]); }
    RET_STD(0, s);
}
#define AP(n, f) { "APM_X86.DLL!" n, f }
static const imp_def defs[] = {
    { "SHELL32.DLL!SHCreateDirectoryExW", SHCreateDirectoryExW_ }, { "SHELL32.DLL!SHGetSpecialFolderPathW", SHGetSpecialFolderPathW_ },
    { "WINMM.DLL!timeGetTime", timeGetTime_ }, { "WINMM.DLL!timeBeginPeriod", timeBeginPeriod_ }, { "WINMM.DLL!PlaySoundA", PlaySoundA_ },
    { "WS2_32.DLL!Ordinal_115", WSAStartup_ }, { "WS2_32.DLL!Ordinal_116", ws_ok0 }, { "WS2_32.DLL!Ordinal_111", WSAGetLastError_ },
    { "WS2_32.DLL!Ordinal_112", WSASetLastError_ }, { "WS2_32.DLL!Ordinal_151", FDIsSet_ }, { "WS2_32.DLL!Ordinal_4", connect_ },
    { "WS2_32.DLL!Ordinal_19", send_ }, { "WS2_32.DLL!Ordinal_16", recv_ }, { "WS2_32.DLL!Ordinal_2", bind_ }, { "WS2_32.DLL!Ordinal_3", closesocket_ },
    { "WS2_32.DLL!Ordinal_5", getpeername_ }, { "WS2_32.DLL!Ordinal_6", getsockname_ }, { "WS2_32.DLL!Ordinal_7", getsockopt_ },
    { "WS2_32.DLL!Ordinal_9", htons_ }, { "WS2_32.DLL!Ordinal_15", htons_ }, { "WS2_32.DLL!Ordinal_21", setsockopt_ }, { "WS2_32.DLL!Ordinal_23", socket_ },
    { "WS2_32.DLL!Ordinal_18", select_ }, { "WS2_32.DLL!Ordinal_10", ioctlsocket_ }, { "WS2_32.DLL!WSAIoctl", WSAIoctl_ },
    { "WS2_32.DLL!getaddrinfo", getaddrinfo_ }, { "WS2_32.DLL!freeaddrinfo", freeaddrinfo_ },
    { "NETAPI32.DLL!Netbios", Netbios_ }, { "IPHLPAPI.DLL!GetAdaptersInfo", GetAdaptersInfo_ },
    { "ADVAPI32.DLL!CryptAcquireContextA", CryptAcquireContextA_ }, { "ADVAPI32.DLL!CryptCreateHash", CryptCreateHash_ }, { "ADVAPI32.DLL!CryptHashData", CryptHashData_ },
    { "ADVAPI32.DLL!CryptGetHashParam", CryptGetHashParam_ }, { "ADVAPI32.DLL!CryptDestroyHash", CryptDestroyHash_ }, { "ADVAPI32.DLL!CryptReleaseContext", CryptReleaseContext_ },
    { "BCRYPT.DLL!BCryptOpenAlgorithmProvider", BCryptOpenAlgorithmProvider_ }, { "BCRYPT.DLL!BCryptCloseAlgorithmProvider", BCryptCloseAlgorithmProvider_ },
    { "BCRYPT.DLL!BCryptEncrypt", BCryptEncrypt_ }, { "BCRYPT.DLL!BCryptDecrypt", BCryptDecrypt_ }, { "BCRYPT.DLL!BCryptDestroyKey", BCryptDestroyKey_ },
    { "BCRYPT.DLL!BCryptGenerateSymmetricKey", BCryptGenerateSymmetricKey_ }, { "BCRYPT.DLL!BCryptSetProperty", BCryptSetProperty_ },
    { "DINPUT8.DLL!DirectInput8Create", DirectInput8Create_ }, { "XINPUT1_3.DLL!Ordinal_2", XInputGetState_ }, { "XINPUT1_3.DLL!Ordinal_3", XInputSetState_ },
    { "D3DX9_43.DLL!D3DXMatrixMultiply", D3DXMatrixMultiply_ }, { "D3DX9_43.DLL!D3DXMatrixScaling", D3DXMatrixScaling_ },
    { "D3DX9_43.DLL!D3DXMatrixTranspose", D3DXMatrixTranspose_ }, { "D3DX9_43.DLL!D3DXMatrixInverse", D3DXMatrixInverse_ },
    { "D3DX9_43.DLL!D3DXVec4Transform", D3DXVec4Transform_ }, { "D3DX9_43.DLL!D3DXVec3Project", D3DXVec3Project_ },
    AP("_ApmSystemSetting_getFixedTitle@4", apm_one1), AP("_ApmSystemSetting_getAdvertizeSound@4", apm_one1), AP("_ApmSystemSetting_getTimeToClosingTime@4", getTimeToClosingTime),
    AP("_Credit_toString@0", Credit_toString), AP("_Credit_isFreePlay@0", apm_one0), AP("_Credit_getCredit@0", apm_zero0), AP("_Credit_isZero@0", apm_zero0),
    AP("_Credit_isGameCostEnough@4", apm_one1), AP("_Credit_setCoinInHook@4", apm_one1),
    AP("_Sequence_endPlay@16", apm_one4), AP("_Sequence_beginPlay@20", apm_one5), AP("_Sequence_continuePlay@28", apm_one7),
    AP("_Sequence_clearBackup@0", apm_one0), AP("_Sequence_getBookkeeping@0", System_getGameVersion),
    AP("_AllnetAuth_getLocationName@0", str_loc), AP("_AllnetAuth_getAbaasGsServerName@0", str_empty), AP("_AllnetAuth_getLocationId@0", apm_zero0),
    AP("_AllnetAuth_isGood@0", apm_zero0), AP("_System_getGameVersion@0", System_getGameVersion), AP("_System_getKeychipId@0", System_getKeychipId),
    AP("_System_getGameId@0", System_getGameId), AP("_System_getAppRootPath@0", System_getAppRootPath),
    AP("_Error_isOccurred@0", apm_zero0), AP("_Core_isExitNeeded@0", Core_isExitNeeded), AP("_Core_execute@0", Core_execute), AP("_Core_exitGame@4", Core_exitGame),
    AP("_Input_isOn@4", Input_isOn),
    AP("_Aime_hasConfirm@0", apm_zero0), AP("_Aime_getAimeId@4", apm_zero1), AP("_Aime_getAccessCode@4", apm_zero1), AP("_Aime_setLedSuccess@0", apm_zero0),
    AP("_Aime_getConfirm@0", apm_zero0), AP("_Aime_cancel@0", apm_zero0), AP("_Aime_isBusy@0", apm_zero0), AP("_Aime_isDBAlive@0", apm_zero0),
    AP("_Aime_hasError@0", apm_zero0), AP("_Aime_getErrorCategory@0", apm_zero0), AP("_Aime_setLedError@0", apm_zero0), AP("_Aime_start@0", apm_zero0),
    AP("_Aime_hasResult@0", apm_zero0), AP("_Aime_acceptConfirm@0", apm_zero0),
    { NULL, NULL }
};
void misc_register(void) { (void)apm_zero2; (void)apm_one2; (void)apm_zero3; imp_register(defs); }
