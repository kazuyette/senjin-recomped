/* kit_crt.h - common CRT filter, taken AS IS from Soul Calibur (plat_win32.c):
   scanlines + phosphor mask (RGB triads) + vignette, computed in software at display size.
   Levels: 0 = none, 1 = light, 2 = medium, 3 = strong.  In game: F9 cycles 0->1->2->3->0 (kit_crt_next).
   Input / output: 32-bit pixels 0x00RRGGBB (order R<<16|G<<8|B). Game .ini key: [video] crt=0..3 .
   Usable only if the display area is at least as large as the image (dw>=w && dh>=h): otherwise display unfiltered.

   Typical usage (GDI window):
     static uint32_t *buf; static int bw, bh;
     if (kit_crt_usable(lvl, w, h, dw, dh)) {
         if (bw != dw || bh != dh) { free(buf); buf = malloc((size_t)dw*dh*4); bw = dw; bh = dh; }
         kit_crt_render(src, w, h, buf, dw, dh, lvl);   // then SetDIBitsToDevice / SDL_UpdateTexture / glTexSubImage2D
     }
*/
#ifndef KIT_CRT_H
#define KIT_CRT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void kit_crt_render(const uint32_t *src, int w, int h, uint32_t *dst, int dw, int dh, int lvl);
static inline int kit_crt_usable(int lvl, int w, int h, int dw, int dh) { return lvl > 0 && dw >= w && dh >= h; }
static inline int kit_crt_next(int lvl) { return (lvl + 1) & 3; }
#ifdef __cplusplus
}
#endif
#endif
