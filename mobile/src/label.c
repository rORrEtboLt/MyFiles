#include "label.h"

#include <SDL3_image/SDL_image.h>
#include <qrcodegen.h>

#include "fileno.h"

static bool encode(const char *file_number, uint8_t qr[qrcodegen_BUFFER_LEN_MAX])
{
    char id[FILENO_MAX + 1], payload[FILENO_MAX + 16];
    fileno_normalize(file_number, id, sizeof id);
    SDL_snprintf(payload, sizeof payload, "%s%s", FILENO_QR_PREFIX, id);
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    return qrcodegen_encodeText(payload, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                qrcodegen_Mask_AUTO, true);
}

/* Display form printed on the label: trimmed and uppercased, zero padding kept (matches backend). */
static void display_number(const char *in, char *out, size_t cap)
{
    while (*in == ' ') in++;
    SDL_strlcpy(out, in, cap);
    size_t n = SDL_strlen(out);
    while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
    for (char *p = out; *p; p++) *p = (char)SDL_toupper((unsigned char)*p);
}

void label_draw(Ui *ui, SDL_FRect r, const char *file_number)
{
    const Theme *t = &ui->theme;
    ui_fill(ui, r, t->surface);
    ui_rect_outline(ui, r, t->border);

    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    float pad = 14;
    float qr_side = r.h - pad * 2;
    if (encode(file_number, qr)) {
        int size = qrcodegen_getSize(qr);
        int quiet = 2;
        float m = qr_side / (float)(size + quiet * 2);
        SDL_SetRenderDrawColor(ui->renderer, 0, 0, 0, 255);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                if (qrcodegen_getModule(qr, x, y)) {
                    SDL_FRect cell = {r.x + pad + (x + quiet) * m, r.y + pad + (y + quiet) * m, m + 0.5f, m + 0.5f};
                    SDL_RenderFillRect(ui->renderer, &cell);
                }
    }
    char shown[FILENO_MAX + 1];
    display_number(file_number, shown, sizeof shown);
    float tx = r.x + pad * 2 + qr_side;
    float avail = r.x + r.w - tx - pad;
    ui_text(ui, FONT_SMALL, tx, r.y + r.h / 2 - ui_font_height(ui, FONT_HUGE) / 2 - ui_font_height(ui, FONT_SMALL), "FILE NO.", t->text_muted, avail);
    ui_text(ui, FONT_HUGE, tx, r.y + r.h / 2 - ui_font_height(ui, FONT_HUGE) / 2, shown, t->text, avail);
}

bool label_save_png(Ui *ui, const char *file_number, const char *path)
{
    const int W = 1200, H = 600, PAD = 40;
    SDL_Surface *s = SDL_CreateSurface(W, H, SDL_PIXELFORMAT_RGBA32);
    if (!s) return false;
    const SDL_PixelFormatDetails *fmt = SDL_GetPixelFormatDetails(s->format);
    SDL_FillSurfaceRect(s, NULL, SDL_MapRGB(fmt, NULL, 255, 255, 255));

    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    int qr_side = H - PAD * 2;
    if (encode(file_number, qr)) {
        int size = qrcodegen_getSize(qr), quiet = 2;
        int m = qr_side / (size + quiet * 2);
        int offset = PAD + (qr_side - m * (size + quiet * 2)) / 2;
        Uint32 black = SDL_MapRGB(fmt, NULL, 0, 0, 0);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                if (qrcodegen_getModule(qr, x, y)) {
                    SDL_Rect cell = {offset + (x + quiet) * m, offset + (y + quiet) * m, m, m};
                    SDL_FillSurfaceRect(s, &cell, black);
                }
    }

    /* Text: size the number to fill the right-hand side. */
    char shown[FILENO_MAX + 1];
    display_number(file_number, shown, sizeof shown);
    int text_x = PAD * 2 + qr_side, avail = W - text_x - PAD;
    float pt = 150;
    TTF_Font *big = NULL;
    for (; pt >= 30; pt -= 10) {
        TTF_Font *f = TTF_CopyFont(ui->fonts[FONT_HUGE]);
        if (!f) break;
        TTF_SetFontSize(f, pt);
        int w = 0, h = 0;
        TTF_GetStringSize(f, shown, 0, &w, &h);
        if (w <= avail) {
            big = f;
            break;
        }
        TTF_CloseFont(f);
    }
    TTF_Font *small = TTF_CopyFont(ui->fonts[FONT_SMALL]);
    if (small) TTF_SetFontSize(small, 36);
    SDL_Color black = {0, 0, 0, 255}, grey = {90, 90, 90, 255};
    SDL_Surface *num = big ? TTF_RenderText_Blended(big, shown, 0, black) : NULL;
    SDL_Surface *cap = small ? TTF_RenderText_Blended(small, "FILE NO.", 0, grey) : NULL;
    int num_h = num ? num->h : 0, cap_h = cap ? cap->h : 0;
    int y = (H - num_h - cap_h) / 2;
    if (cap) {
        SDL_BlitSurface(cap, NULL, s, &(SDL_Rect){text_x, y, cap->w, cap->h});
        SDL_DestroySurface(cap);
    }
    if (num) {
        SDL_BlitSurface(num, NULL, s, &(SDL_Rect){text_x, y + cap_h, num->w, num->h});
        SDL_DestroySurface(num);
    }
    if (big) TTF_CloseFont(big);
    if (small) TTF_CloseFont(small);

    /* Cut line */
    Uint32 line = SDL_MapRGB(fmt, NULL, 160, 160, 160);
    for (int x = 0; x < W; x += 24) {
        SDL_FillSurfaceRect(s, &(SDL_Rect){x, 0, 12, 3}, line);
        SDL_FillSurfaceRect(s, &(SDL_Rect){x, H - 3, 12, 3}, line);
    }
    for (int yy = 0; yy < H; yy += 24) {
        SDL_FillSurfaceRect(s, &(SDL_Rect){0, yy, 3, 12}, line);
        SDL_FillSurfaceRect(s, &(SDL_Rect){W - 3, yy, 3, 12}, line);
    }

    bool ok = IMG_SavePNG(s, path);
    SDL_DestroySurface(s);
    return ok;
}
