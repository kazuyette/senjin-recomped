/* kit_extract.h - extraction d'une image disque (ISO9660) vers un dossier. C portable (testable sous Linux). */
#ifndef KIT_EXTRACT_H
#define KIT_EXTRACT_H
/* cb(ud, octets_faits, octets_total, nom_fichier) ; appelé souvent, l'appelant limite l'affichage */
typedef void (*kx_progress)(void *ud, unsigned long long done, unsigned long long total, const char *what);
/* Extrait tout le contenu ISO9660 de src (.iso/.bin 2048, 2336, 2352 mode1/mode2 ; .cue accepté) dans dst (créé si besoin).
   Les fichiers déjà présents avec la bonne taille sont sautés (reprise). Secteurs XA form2 (audio/vidéo PSX) : 2324 octets.
   Chemins en UTF-8. Retour 0 = OK ; sinon message dans err. *nfiles = nombre de fichiers. */
int kx_extract_image(const char *src, const char *dst, kx_progress cb, void *ud, char *err, int errn, int *nfiles);
#endif
