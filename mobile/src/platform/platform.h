#ifndef MYFILES_PLATFORM_H
#define MYFILES_PLATFORM_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#include "../http.h"

/* Native capabilities SDL does not provide. Each platform implements this file:
 *   iOS      platform_ios.m      VisionKit document camera + DataScanner, Vision OCR (handles
 *                                handwriting), UIActivityViewController for share/print, NSURLSession
 *   Android  platform_android.c  JNI into MyFilesActivity.kt: ML Kit document scanner, text
 *                                recognition and barcode scanning, share intent, HttpURLConnection
 *   desktop  platform_desktop.c  development build: file picker for "scans", tesseract CLI for OCR
 *                                if installed, libcurl
 *
 * Scans finish asynchronously; results come back as SDL events (types given to platform_init) whose
 * data1 points to a ScanResult / LabelResult that the app frees with the matching free function. */

typedef struct {
    char path[1024];   /* JPEG written into the tmp dir; the app moves it into the pages dir */
    int width, height;
    char *ocr_text;    /* SDL_malloc'd, may be "" */
} ScanPage;

typedef struct {
    int count;
    ScanPage *pages;
    bool cancelled;
    char error[256];
} ScanResult;

typedef struct {
    char *text;        /* QR payload if from_qr, else recognised text (lines separated by \n) */
    bool from_qr;
    bool cancelled;
    char error[256];
} LabelResult;

void platform_init(SDL_Window *window, const char *tmp_dir, Uint32 scan_event, Uint32 label_event);

/* Photograph one or more pages of a document; OCR every page. */
void platform_scan_document(void);
/* Identify an envelope: read its QR label, or recognise a handwritten file number. */
void platform_scan_label(void);
/* Share sheet for a file (lets the user print a label via AirPrint / Android print services). */
void platform_share_file(const char *path, const char *mime);

/* Where bundled fonts live ("fonts/" inside the APK on Android). */
const char *platform_font_dir(void);

/* Helpers shared by implementations (platform_common.c). */
ScanResult *scan_result_new(int count);
void platform_free_scan(ScanResult *r);
LabelResult *label_result_new(void);
void platform_free_label(LabelResult *r);
void platform_post(Uint32 type, void *data);

#endif
