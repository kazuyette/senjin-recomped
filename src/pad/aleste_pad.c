/* aleste_pad.c - Senjin Aleste: Windows gamepad relay -> native Linux version under WSL2.
 * WSL2 does not see Windows USB gamepads: this small Windows program reads XInput (first gamepad found)
 * and sends its state ~250 times/s over TCP to localhost:PORT, where the native game listens (SA_PADNET=PORT).
 * 16-byte packet: "APAD", buttons (16 bits), left trigger, right trigger, left stick x/y, right stick x/y (signed 16 bits).
 * Usage: aleste_pad.exe [port]   (default 47011); exits when the game closes the connection. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "version.h"
typedef struct { DWORD pkt; WORD btn; BYTE lt, rt; SHORT lx, ly, rx, ry; } XST;
typedef DWORD (WINAPI *XGET)(DWORD, XST *);
typedef DWORD (WINAPI *XSET)(DWORD, WORD *);
int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 47011;
    const char *dll[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" }; XGET get = NULL; XSET set = NULL;
    for (int i = 0; i < 3 && !get; i++) { HMODULE h = LoadLibraryA(dll[i]); if (h) { get = (XGET)GetProcAddress(h, "XInputGetState"); set = (XSET)GetProcAddress(h, "XInputSetState"); } }
    if (!get) { printf("aleste_pad: XInput not found\n"); return 1; }
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    printf("aleste_pad %s: gamepad relay -> localhost:%d\n", SALE_VERSION_FULL, port);
    int was = 0;
    for (int tries = 0; tries < 600; tries++) {            /* waits for the game for up to ~10 min */
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); if (was) break; Sleep(1000); continue; }
        int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);                /* non-blocking read of rumble commands */
        unsigned char rb[8]; int rn = 0; DWORD used = 0;
        printf("aleste_pad: connected to the game\n"); was = 1;
        for (int alive = 1; alive;) {
            XST st; int found = 0;
            for (DWORD i = 0; i < 4 && !found; i++) if (get(i, &st) == 0) { found = 1; used = i; }
            for (;;) {                                               /* "RUMB" + left motor + right motor */
                int r = recv(s, (char *)rb + rn, 8 - rn, 0);
                if (r == 0 || (r < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) { alive = 0; break; }   /* closed / error */
                if (r < 0) break;                                    /* no data for now */
                rn += r;
                if (rn == 8) { rn = 0; if (!memcmp(rb, "RUMB", 4) && set && found) { WORD v[2] = { (WORD)(rb[4] | rb[5] << 8), (WORD)(rb[6] | rb[7] << 8) }; set(used, v); } }
            }
            if (!alive) break;
            if (!found) memset(&st, 0, sizeof st);
            unsigned char p[16]; memcpy(p, "APAD", 4);
            p[4] = (unsigned char)st.btn; p[5] = (unsigned char)(st.btn >> 8); p[6] = st.lt; p[7] = st.rt;
            SHORT ax[4] = { st.lx, st.ly, st.rx, st.ry }; for (int i = 0; i < 4; i++) { p[8 + 2 * i] = (unsigned char)ax[i]; p[9 + 2 * i] = (unsigned char)(ax[i] >> 8); }
            /* a packet that is started must be finished (16-byte framing); one that cannot start is dropped */
            int sent = 0, waits = 0;
            while (sent < 16 && alive) {
                int n = send(s, (const char *)p + sent, 16 - sent, 0);
                if (n > 0) { sent += n; continue; }
                if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) { if (!sent) break; if (++waits > 2000) alive = 0; else Sleep(1); continue; }
                alive = 0;                                           /* connection lost */
            }
            if (!alive) break;
            Sleep(4);
        }
        if (set) { WORD z[2] = { 0, 0 }; set(used, z); }
        closesocket(s); printf("aleste_pad: game closed\n"); break;
    }
    return 0;
}
