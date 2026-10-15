#ifndef MYFILES_LABEL_H
#define MYFILES_LABEL_H

#include <stdbool.h>

#include "ui.h"

/* Envelope labels: QR code (MYFILES:<id>) plus the file number in large type, so the app can scan it and
 * a person can read it. Same payload format as the backend's /api/labels/:n.svg. */

/* Draws a label into r on screen. */
void label_draw(Ui *ui, SDL_FRect r, const char *file_number);
/* Renders a print-ready 1200x600 PNG (about 100x50 mm at 300 dpi). */
bool label_save_png(Ui *ui, const char *file_number, const char *path);

#endif
