/* kit_crt.h - filtre CRT commun, repris À L'IDENTIQUE de Soul Calibur (plat_win32.c) :
   scanlines + masque de phosphores (triades RGB) + vignette, calculé en logiciel à la taille d'affichage.
   Niveaux : 0 = aucun, 1 = léger, 2 = moyen, 3 = fort.  En jeu : F9 fait défiler 0->1->2->3->0 (kit_crt_next).
   Entrée / sortie : pixels 32 bits 0x00RRGGBB (ordre R<<16|G<<8|B). Clé du .ini du jeu : [video] crt=0..3 .
   Utilisable seulement si la zone d'affichage est au moins aussi grande que l'image (dw>=w && dh>=h) : sinon afficher sans filtre.

   Usage type (fenêtre GDI) :
     static uint32_t *buf; static int bw, bh;
     if (kit_crt_usable(lvl, w, h, dw, dh)) {
         if (bw != dw || bh != dh) { free(buf); buf = malloc((size_t)dw*dh*4); bw = dw; bh = dh; }
         kit_crt_render(src, w, h, buf, dw, dh, lvl);   // puis SetDIBitsToDevice / SDL_UpdateTexture / glTexSubImage2D
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
