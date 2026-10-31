/* kit_extract.h - extracts a disc image (ISO9660) to a folder. Portable C (testable on Linux). */
#ifndef KIT_EXTRACT_H
#define KIT_EXTRACT_H
/* cb(ud, bytes_done, bytes_total, file_name); called often, the caller throttles the display */
typedef void (*kx_progress)(void *ud, unsigned long long done, unsigned long long total, const char *what);
/* Extracts the whole ISO9660 content of src (.iso/.bin 2048, 2336, 2352 mode1/mode2; .cue accepted) into dst (created if needed).
   Files already present with the right size are skipped (resume). XA form2 sectors (PSX audio/video): 2324 bytes.
   Paths in UTF-8. Returns 0 = OK; otherwise message in err. *nfiles = number of files. */
int kx_extract_image(const char *src, const char *dst, kx_progress cb, void *ud, char *err, int errn, int *nfiles);
#endif
