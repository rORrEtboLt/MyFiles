#include "platform.h"

ScanResult *scan_result_new(int count)
{
    ScanResult *r = SDL_calloc(1, sizeof *r);
    if (count > 0) r->pages = SDL_calloc((size_t)count, sizeof *r->pages);
    r->count = count;
    return r;
}

void platform_free_scan(ScanResult *r)
{
    if (!r) return;
    for (int i = 0; i < r->count; i++) SDL_free(r->pages[i].ocr_text);
    SDL_free(r->pages);
    SDL_free(r);
}

LabelResult *label_result_new(void) { return SDL_calloc(1, sizeof(LabelResult)); }

void platform_free_label(LabelResult *r)
{
    if (!r) return;
    SDL_free(r->text);
    SDL_free(r);
}

void platform_post(Uint32 type, void *data)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    e.user.data1 = data;
    SDL_PushEvent(&e);
}
