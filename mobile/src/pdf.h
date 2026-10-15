#ifndef MYFILES_PDF_H
#define MYFILES_PDF_H

#include <stdbool.h>

/* Writes a PDF with one page per JPEG, embedding each JPEG unchanged (DCTDecode), so it is fast
 * and lossless relative to the scan. Pages are A4 width with the image's aspect ratio.
 * Must stay equivalent to backend/src/lib/pdf.js. Returns the number of pages written (0 on failure). */
int pdf_from_jpegs(const char *const *jpeg_paths, int count, const char *out_path);

#endif
