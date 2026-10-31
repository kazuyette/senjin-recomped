/* sa_dinput.c - DirectInput 8 rewritten natively, just what the game's input layer (MWinInput) uses:
 * IDirectInput8A (EnumDevices with no device: gamepads go through XInput) and the system keyboard
 * (GetDeviceState 256 bytes, DIK codes = PC scan codes) read from SDL. Without it, MWinInput does not
 * initialize and the game ignores the XInput gamepad and the keys set in its configuration. */
#include <string.h>
#include <SDL2/SDL.h>
#include "sa_host.h"

#define MAGIC 0x44494E38u   /* 'DIN8' */
static uint32_t vt_di, vt_kb, di_obj, kb_obj;

/* SDL_Scancode -> DIK (PC scan code set 1, 0xE0xx -> 0x80 | xx) */
static uint8_t dik_of(int sc) {
    static uint8_t t[SDL_NUM_SCANCODES]; static int init;
    if (!init) {
        init = 1;
        static const struct { int s; uint8_t d; } m[] = {
            { SDL_SCANCODE_ESCAPE, 0x01 }, { SDL_SCANCODE_1, 0x02 }, { SDL_SCANCODE_2, 0x03 }, { SDL_SCANCODE_3, 0x04 }, { SDL_SCANCODE_4, 0x05 },
            { SDL_SCANCODE_5, 0x06 }, { SDL_SCANCODE_6, 0x07 }, { SDL_SCANCODE_7, 0x08 }, { SDL_SCANCODE_8, 0x09 }, { SDL_SCANCODE_9, 0x0A },
            { SDL_SCANCODE_0, 0x0B }, { SDL_SCANCODE_MINUS, 0x0C }, { SDL_SCANCODE_EQUALS, 0x0D }, { SDL_SCANCODE_BACKSPACE, 0x0E }, { SDL_SCANCODE_TAB, 0x0F },
            { SDL_SCANCODE_Q, 0x10 }, { SDL_SCANCODE_W, 0x11 }, { SDL_SCANCODE_E, 0x12 }, { SDL_SCANCODE_R, 0x13 }, { SDL_SCANCODE_T, 0x14 },
            { SDL_SCANCODE_Y, 0x15 }, { SDL_SCANCODE_U, 0x16 }, { SDL_SCANCODE_I, 0x17 }, { SDL_SCANCODE_O, 0x18 }, { SDL_SCANCODE_P, 0x19 },
            { SDL_SCANCODE_LEFTBRACKET, 0x1A }, { SDL_SCANCODE_RIGHTBRACKET, 0x1B }, { SDL_SCANCODE_RETURN, 0x1C }, { SDL_SCANCODE_LCTRL, 0x1D },
            { SDL_SCANCODE_A, 0x1E }, { SDL_SCANCODE_S, 0x1F }, { SDL_SCANCODE_D, 0x20 }, { SDL_SCANCODE_F, 0x21 }, { SDL_SCANCODE_G, 0x22 },
            { SDL_SCANCODE_H, 0x23 }, { SDL_SCANCODE_J, 0x24 }, { SDL_SCANCODE_K, 0x25 }, { SDL_SCANCODE_L, 0x26 }, { SDL_SCANCODE_SEMICOLON, 0x27 },
            { SDL_SCANCODE_APOSTROPHE, 0x28 }, { SDL_SCANCODE_GRAVE, 0x29 }, { SDL_SCANCODE_LSHIFT, 0x2A }, { SDL_SCANCODE_BACKSLASH, 0x2B },
            { SDL_SCANCODE_Z, 0x2C }, { SDL_SCANCODE_X, 0x2D }, { SDL_SCANCODE_C, 0x2E }, { SDL_SCANCODE_V, 0x2F }, { SDL_SCANCODE_B, 0x30 },
            { SDL_SCANCODE_N, 0x31 }, { SDL_SCANCODE_M, 0x32 }, { SDL_SCANCODE_COMMA, 0x33 }, { SDL_SCANCODE_PERIOD, 0x34 }, { SDL_SCANCODE_SLASH, 0x35 },
            { SDL_SCANCODE_RSHIFT, 0x36 }, { SDL_SCANCODE_KP_MULTIPLY, 0x37 }, { SDL_SCANCODE_LALT, 0x38 }, { SDL_SCANCODE_SPACE, 0x39 },
            { SDL_SCANCODE_CAPSLOCK, 0x3A }, { SDL_SCANCODE_F1, 0x3B }, { SDL_SCANCODE_F2, 0x3C }, { SDL_SCANCODE_F3, 0x3D }, { SDL_SCANCODE_F4, 0x3E },
            { SDL_SCANCODE_F5, 0x3F }, { SDL_SCANCODE_F6, 0x40 }, { SDL_SCANCODE_F7, 0x41 }, { SDL_SCANCODE_F8, 0x42 }, { SDL_SCANCODE_F9, 0x43 },
            { SDL_SCANCODE_F10, 0x44 }, { SDL_SCANCODE_KP_7, 0x47 }, { SDL_SCANCODE_KP_8, 0x48 }, { SDL_SCANCODE_KP_9, 0x49 }, { SDL_SCANCODE_KP_MINUS, 0x4A },
            { SDL_SCANCODE_KP_4, 0x4B }, { SDL_SCANCODE_KP_5, 0x4C }, { SDL_SCANCODE_KP_6, 0x4D }, { SDL_SCANCODE_KP_PLUS, 0x4E }, { SDL_SCANCODE_KP_1, 0x4F },
            { SDL_SCANCODE_KP_2, 0x50 }, { SDL_SCANCODE_KP_3, 0x51 }, { SDL_SCANCODE_KP_0, 0x52 }, { SDL_SCANCODE_KP_PERIOD, 0x53 }, { SDL_SCANCODE_F11, 0x57 },
            { SDL_SCANCODE_F12, 0x58 }, { SDL_SCANCODE_KP_ENTER, 0x9C }, { SDL_SCANCODE_RCTRL, 0x9D }, { SDL_SCANCODE_KP_DIVIDE, 0xB5 },
            { SDL_SCANCODE_RALT, 0xB8 }, { SDL_SCANCODE_PAUSE, 0xC5 }, { SDL_SCANCODE_HOME, 0xC7 }, { SDL_SCANCODE_UP, 0xC8 }, { SDL_SCANCODE_PAGEUP, 0xC9 },
            { SDL_SCANCODE_LEFT, 0xCB }, { SDL_SCANCODE_RIGHT, 0xCD }, { SDL_SCANCODE_END, 0xCF }, { SDL_SCANCODE_DOWN, 0xD0 }, { SDL_SCANCODE_PAGEDOWN, 0xD1 },
            { SDL_SCANCODE_INSERT, 0xD2 }, { SDL_SCANCODE_DELETE, 0xD3 } };
        for (unsigned i = 0; i < sizeof m / sizeof *m; i++) t[m[i].s] = m[i].d;
    }
    return sc >= 0 && sc < SDL_NUM_SCANCODES ? t[sc] : 0;
}

static void ok1(void) { RET_STD(1, 0); } static void ok2(void) { RET_STD(2, 0); } static void ok3(void) { RET_STD(3, 0); }
static void ok4(void) { RET_STD(4, 0); } static void ok5(void) { RET_STD(5, 0); } static void ok6(void) { RET_STD(6, 0); }
static void QI(void) { if (ARG(2)) wr32(ARG(2), ARG(0)); RET_STD(3, 0); }
static void AddRef(void) { RET_STD(1, 1); }
static void Release(void) { RET_STD(1, 0); }
static const uint8_t guid_kb[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static void DI_CreateDevice(void) {   /* this, rguid, lplpDevice, pUnkOuter */
    if (!memcmp(MEM + ARG(1), guid_kb, 16)) { wr32(ARG(2), kb_obj); RET_STD(4, 0); return; }
    wr32(ARG(2), 0); RET_STD(4, 0x80040154u);       /* REGDB_E_CLASSNOTREG: no other device */
}
static void DI_EnumDevices(void) { RET_STD(5, 0); }              /* none: gamepads are read via XInput */
static void DI_GetDeviceStatus(void) { RET_STD(2, 1); }          /* DI_NOTATTACHED */
static void KB_GetDeviceState(void) {   /* this, cbData, lpvData */
    uint32_t n = ARG(1), p = ARG(2); if (n > 256) n = 256;
    memset(MEM + p, 0, n);
    int cnt = 0; const Uint8 *ks = SDL_GetKeyboardState(&cnt);
    for (int sc = 0; sc < cnt; sc++) if (ks[sc]) { uint8_t d = dik_of(sc); if (d && d < n) wr8(p + d, 0x80); }
    RET_STD(3, 0);
}
static void KB_GetDeviceData(void) { if (ARG(3)) wr32(ARG(3), 0); RET_STD(5, 0); }   /* no event buffer */
static void KB_GetCapabilities(void) { uint32_t p = ARG(1); wr32(p + 4, 1); wr32(p + 8, 0x13); RET_STD(2, 0); }
static void KB_Poll(void) { RET_STD(1, 1); }                     /* DI_NOEFFECT */

typedef struct { const char *name; void (*fn)(void); } ent;
static uint32_t build_vt(const char *iface, const ent *e, int n) {
    uint32_t vt = g_static(4 * (uint32_t)n);
    for (int i = 0; i < n; i++) {
        char full[128]; snprintf(full, sizeof full, "DINPUT8!%s::%s", iface, e[i].name);
        imp_def d[2] = { { strdup(full), e[i].fn }, { NULL, NULL } }; imp_register(d);
        wr32(vt + 4 * (uint32_t)i, imp_magic_for(full));
    }
    return vt;
}
static void build(void) {
    if (vt_di) return;
    static const ent di[] = { { "QueryInterface", QI }, { "AddRef", AddRef }, { "Release", Release }, { "CreateDevice", DI_CreateDevice },
        { "EnumDevices", DI_EnumDevices }, { "GetDeviceStatus", DI_GetDeviceStatus }, { "RunControlPanel", ok3 }, { "Initialize", ok3 },
        { "FindDevice", ok4 }, { "EnumDevicesBySemantics", ok6 }, { "ConfigureDevices", ok5 } };
    static const ent kb[] = { { "QueryInterface", QI }, { "AddRef", AddRef }, { "Release", Release }, { "GetCapabilities", KB_GetCapabilities },
        { "EnumObjects", ok4 }, { "GetProperty", ok3 }, { "SetProperty", ok3 }, { "Acquire", ok1 }, { "Unacquire", ok1 },
        { "GetDeviceState", KB_GetDeviceState }, { "GetDeviceData", KB_GetDeviceData }, { "SetDataFormat", ok2 }, { "SetEventNotification", ok2 },
        { "SetCooperativeLevel", ok3 }, { "GetObjectInfo", ok4 }, { "GetDeviceInfo", ok2 }, { "RunControlPanel", ok3 }, { "Initialize", ok4 },
        { "CreateEffect", ok5 }, { "EnumEffects", ok4 }, { "GetEffectInfo", ok3 }, { "GetForceFeedbackState", ok2 },
        { "SendForceFeedbackCommand", ok2 }, { "EnumCreatedEffectObjects", ok4 }, { "Escape", ok2 }, { "Poll", KB_Poll },
        { "SendDeviceData", ok5 }, { "EnumEffectsInFile", ok5 }, { "WriteEffectToFile", ok5 }, { "BuildActionMap", ok4 },
        { "SetActionMap", ok4 }, { "GetImageInfo", ok2 } };
    vt_di = build_vt("IDirectInput8A", di, (int)(sizeof di / sizeof *di));
    vt_kb = build_vt("IDirectInputDevice8A", kb, (int)(sizeof kb / sizeof *kb));
    di_obj = g_static(16); wr32(di_obj, vt_di); wr32(di_obj + 8, MAGIC);
    kb_obj = g_static(16); wr32(kb_obj, vt_kb); wr32(kb_obj + 8, MAGIC);
}
static void DirectInput8Create_(void) {   /* hinst, dwVersion, riidltf, ppvOut, punkOuter */
    build(); wr32(ARG(3), di_obj); RET_STD(5, 0);
}
static const imp_def defs[] = { { "DINPUT8.DLL!DirectInput8Create", DirectInput8Create_ }, { NULL, NULL } };
void dinput_register(void) { imp_register(defs); }
