/* sa_ole.c - minimal OLE32 / OLEAUT32. COM:
 * XAudio2 2.7 is provided by sa_xaudio2.c; other classes answer "class not registered". */
#include <string.h>
#include "sa_host.h"

static void guid_str(uint32_t p, char *o) {
    snprintf(o, 40, "%08x-%04x-%04x-%02x%02x", rd32(p), rd16(p + 4), rd16(p + 6), rd8(p + 8), rd8(p + 9));
}
static void CoInitialize_(void) { RET_STD(1, 0); }
static void CoInitializeEx_(void) { RET_STD(2, 0); }
static void CoUninitialize_(void) { RET_STD(0, 0); }
int xaudio2_create(uint32_t clsid, uint32_t ppv);
static void CoCreateInstance_(void) {
    char g[48]; guid_str(ARG(0), g);
    if (ARG(4) && xaudio2_create(ARG(0), ARG(4))) { RET_STD(5, 0); return; }
    sa_log("CoCreateInstance(%s) -> class not registered (no native COM yet)", g);
    if (ARG(4)) wr32(ARG(4), 0);
    RET_STD(5, 0x80040154u);
}
static void CoSetProxyBlanket_(void) { RET_STD(8, 0); }
static void CoTaskMemFree_(void) { RET_STD(1, 0); }
static void SysAllocString_(void) { RET_STD(1, 0); }   /* OLEAUT32 ord. 2 */
static void SysFreeString_(void) { RET_STD(1, 0); }    /* OLEAUT32 ord. 6 */

static const imp_def defs[] = {
    { "OLE32.DLL!CoInitialize", CoInitialize_ }, { "OLE32.DLL!CoInitializeEx", CoInitializeEx_ },
    { "OLE32.DLL!CoUninitialize", CoUninitialize_ }, { "OLE32.DLL!CoCreateInstance", CoCreateInstance_ },
    { "OLE32.DLL!CoSetProxyBlanket", CoSetProxyBlanket_ }, { "OLE32.DLL!CoTaskMemFree", CoTaskMemFree_ },
    { "OLEAUT32.DLL!Ordinal_2", SysAllocString_ }, { "OLEAUT32.DLL!Ordinal_6", SysFreeString_ },
    { NULL, NULL }
};
void ole_register(void) { imp_register(defs); }
