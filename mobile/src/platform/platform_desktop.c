/* Desktop (macOS/Linux/Windows) implementation, for developing the app without a phone.
 * "Scanning" opens a file picker for photos/scans; OCR runs the tesseract CLI when it is installed
 * (brew install tesseract). QR decoding is not available on desktop: type the file number instead. */
#include <SDL3_image/SDL_image.h>

#include "platform.h"

static SDL_Window *g_window;
static char g_tmp_dir[1024];
static Uint32 g_scan_event, g_label_event;

void platform_init(SDL_Window *window, const char *tmp_dir, Uint32 scan_event, Uint32 label_event)
{
    g_window = window;
    SDL_strlcpy(g_tmp_dir, tmp_dir, sizeof g_tmp_dir);
    g_scan_event = scan_event;
    g_label_event = label_event;
}

const char *platform_font_dir(void)
{
    static char dir[1024];
    if (!dir[0]) SDL_snprintf(dir, sizeof dir, "%sfonts/", SDL_GetBasePath());
    return dir;
}

/* tesseract <image> stdout; returns "" when tesseract is missing. */
static char *ocr_file(const char *path)
{
    const char *args[] = {"tesseract", path, "stdout", "--psm", "3", NULL};
    SDL_Process *p = SDL_CreateProcess(args, true);
    if (!p) return SDL_strdup("");
    size_t len = 0;
    int code = -1;
    char *out = SDL_ReadProcess(p, &len, &code);
    SDL_DestroyProcess(p);
    if (!out || code != 0) {
        SDL_free(out);
        return SDL_strdup("");
    }
    return out;
}

/* Normalise any picked image to a JPEG of at most 2400px on the long side, like a phone scan. */
static bool import_image(const char *src, ScanPage *page)
{
    SDL_Surface *s = IMG_Load(src);
    if (!s) return false;
    int w = s->w, h = s->h, longest = w > h ? w : h;
    if (longest > 2400) {
        w = w * 2400 / longest;
        h = h * 2400 / longest;
        SDL_Surface *scaled = SDL_ScaleSurface(s, w, h, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(s);
        if (!scaled) return false;
        s = scaled;
    }
    SDL_snprintf(page->path, sizeof page->path, "%sscan-%llu-%u.jpg", g_tmp_dir, (unsigned long long)SDL_GetTicksNS(), SDL_rand_bits());
    bool ok = IMG_SaveJPG(s, page->path, 85);
    page->width = s->w;
    page->height = s->h;
    SDL_DestroySurface(s);
    return ok;
}

typedef struct { char **files; int count; bool label; } Job;

static int SDLCALL scan_worker(void *data)
{
    Job *job = data;
    if (job->label) {
        LabelResult *r = label_result_new();
        r->text = ocr_file(job->files[0]);
        if (!r->text[0]) SDL_strlcpy(r->error, "No text recognised (is tesseract installed?)", sizeof r->error);
        platform_post(g_label_event, r);
    } else {
        ScanResult *r = scan_result_new(job->count);
        int n = 0;
        for (int i = 0; i < job->count; i++) {
            if (!import_image(job->files[i], &r->pages[n])) {
                SDL_snprintf(r->error, sizeof r->error, "Could not read %s", job->files[i]);
                continue;
            }
            r->pages[n].ocr_text = ocr_file(r->pages[n].path);
            n++;
        }
        r->count = n;
        platform_post(g_scan_event, r);
    }
    for (int i = 0; i < job->count; i++) SDL_free(job->files[i]);
    SDL_free(job->files);
    SDL_free(job);
    return 0;
}

static void SDLCALL on_files(void *userdata, const char *const *files, int filter)
{
    (void)filter;
    bool label = userdata != NULL;
    int count = 0;
    while (files && files[count]) count++;
    if (count == 0) {
        if (label) {
            LabelResult *r = label_result_new();
            r->cancelled = true;
            platform_post(g_label_event, r);
        } else {
            ScanResult *r = scan_result_new(0);
            r->cancelled = true;
            platform_post(g_scan_event, r);
        }
        return;
    }
    Job *job = SDL_calloc(1, sizeof *job);
    job->label = label;
    job->count = label ? 1 : count;
    job->files = SDL_calloc((size_t)job->count, sizeof *job->files);
    for (int i = 0; i < job->count; i++) job->files[i] = SDL_strdup(files[i]);
    SDL_DetachThread(SDL_CreateThread(scan_worker, "scan", job));
}

static const SDL_DialogFileFilter IMAGE_FILTERS[] = {{"Images", "jpg;jpeg;png"}};

void platform_scan_document(void)
{
    SDL_ShowOpenFileDialog(on_files, NULL, g_window, IMAGE_FILTERS, 1, NULL, true);
}

void platform_scan_label(void)
{
    SDL_ShowOpenFileDialog(on_files, (void *)1, g_window, IMAGE_FILTERS, 1, NULL, false);
}

void platform_share_file(const char *path, const char *mime)
{
    (void)mime;
    char url[1200];
    SDL_snprintf(url, sizeof url, "file://%s", path);
    SDL_OpenURL(url);
}
