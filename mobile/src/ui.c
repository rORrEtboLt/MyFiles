#include "ui.h"

#include <SDL3_image/SDL_image.h>

#define TEXT_CACHE_SIZE 384
#define IMAGE_CACHE_SIZE 24
#define TAP_SLOP 10.0f
#define MAX_IMAGE_PX 1800

struct TextCacheEntry {
    char *s;
    FontId font;
    Uint32 color;
    int wrap;        /* wrap width in pixels, 0 = single line */
    SDL_Texture *tex;
    float w, h;      /* points */
    Uint64 used;
};

struct ImageCacheEntry {
    char path[1024];
    SDL_Texture *tex; /* NULL + failed = could not load */
    bool failed;
    float w, h;
    Uint64 used;
};

static const float FONT_PT[FONT_COUNT] = {13, 16, 16, 21, 38};

static Uint32 pack(SDL_Color c) { return (Uint32)c.r << 24 | (Uint32)c.g << 16 | (Uint32)c.b << 8 | c.a; }

static SDL_Color rgb(Uint32 hex) { return (SDL_Color){(Uint8)(hex >> 16), (Uint8)(hex >> 8), (Uint8)hex, 255}; }

static void update_metrics(Ui *ui)
{
    int pw = 0, ph = 0;
    SDL_GetCurrentRenderOutputSize(ui->renderer, &pw, &ph);
    ui->w = (float)pw / ui->density;
    ui->h = (float)ph / ui->density;
    SDL_Rect safe;
    float px_per_window_unit = SDL_GetWindowPixelDensity(ui->window);
    if (SDL_GetWindowSafeArea(ui->window, &safe)) {
        float k = px_per_window_unit / ui->density;
        ui->safe = (SDL_FRect){safe.x * k, safe.y * k, safe.w * k, safe.h * k};
    } else {
        ui->safe = (SDL_FRect){0, 0, ui->w, ui->h};
    }
}

static bool open_fonts(Ui *ui, const char *font_dir)
{
    char regular[1024], bold[1024];
    SDL_snprintf(regular, sizeof regular, "%sNotoSans-Regular.ttf", font_dir);
    SDL_snprintf(bold, sizeof bold, "%sNotoSans-Bold.ttf", font_dir);
    for (int i = 0; i < FONT_COUNT; i++) {
        if (ui->fonts[i]) TTF_CloseFont(ui->fonts[i]);
        bool is_bold = i == FONT_BOLD || i == FONT_TITLE || i == FONT_HUGE;
        ui->fonts[i] = TTF_OpenFont(is_bold ? bold : regular, FONT_PT[i] * ui->density);
        if (!ui->fonts[i]) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "font: %s", SDL_GetError());
            return false;
        }
    }
    return true;
}

static void clear_text_cache(Ui *ui)
{
    for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
        TextCacheEntry *e = &ui->text_cache[i];
        if (e->tex) SDL_DestroyTexture(e->tex);
        SDL_free(e->s);
        SDL_zerop(e);
    }
}

static char g_font_dir[1024];

bool ui_init(Ui *ui, SDL_Window *window, SDL_Renderer *renderer, const char *font_dir)
{
    SDL_zerop(ui);
    ui->window = window;
    ui->renderer = renderer;
    ui->density = SDL_GetWindowDisplayScale(window);
    if (ui->density <= 0) ui->density = 1;
    SDL_SetRenderScale(renderer, ui->density, ui->density);
    SDL_strlcpy(g_font_dir, font_dir, sizeof g_font_dir);
    ui->text_cache = SDL_calloc(TEXT_CACHE_SIZE, sizeof *ui->text_cache);
    ui->image_cache = SDL_calloc(IMAGE_CACHE_SIZE, sizeof *ui->image_cache);
    ui->theme = (Theme){
        .bg = rgb(0xF4F1EA), .surface = rgb(0xFFFFFF), .surface_alt = rgb(0xEBE6DA), .border = rgb(0xDAD4C6),
        .text = rgb(0x1D1D1B), .text_muted = rgb(0x6B6A63), .primary = rgb(0x1F5F5B), .primary_text = rgb(0xFFFFFF),
        .danger = rgb(0xB3261E), .accent = rgb(0x9A5B13),
    };
    update_metrics(ui);
    return open_fonts(ui, font_dir);
}

void ui_shutdown(Ui *ui)
{
    clear_text_cache(ui);
    for (int i = 0; i < IMAGE_CACHE_SIZE; i++)
        if (ui->image_cache[i].tex) SDL_DestroyTexture(ui->image_cache[i].tex);
    SDL_free(ui->text_cache);
    SDL_free(ui->image_cache);
    for (int i = 0; i < FONT_COUNT; i++)
        if (ui->fonts[i]) TTF_CloseFont(ui->fonts[i]);
}

bool ui_event(Ui *ui, const SDL_Event *ev)
{
    SDL_Event e = *ev;
    SDL_ConvertEventToRenderCoordinates(ui->renderer, &e);
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button != SDL_BUTTON_LEFT) return false;
        ui->down = ui->pressed = true;
        ui->px = ui->press_x = e.button.x;
        ui->py = ui->press_y = e.button.y;
        ui->dragging = false;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button != SDL_BUTTON_LEFT) return false;
        ui->down = false;
        ui->released = true;
        ui->was_drag = ui->dragging;
        ui->px = e.button.x;
        ui->py = e.button.y;
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        if (ui->down) {
            if (!ui->dragging && (SDL_fabsf(e.motion.y - ui->press_y) > TAP_SLOP || SDL_fabsf(e.motion.x - ui->press_x) > TAP_SLOP))
                ui->dragging = true;
            if (ui->dragging) ui->drag_dy += e.motion.y - ui->py;
        }
        ui->px = e.motion.x;
        ui->py = e.motion.y;
        return ui->down;
    case SDL_EVENT_MOUSE_WHEEL:
        ui->wheel += e.wheel.y;
        return true;
    case SDL_EVENT_TEXT_INPUT:
        SDL_strlcat(ui->typed, e.text.text, sizeof ui->typed);
        return true;
    case SDL_EVENT_KEY_DOWN:
        if (!ui->focus) return false;
        if (e.key.key == SDLK_BACKSPACE) ui->backspaces++;
        else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) ui->enter = true;
        else return false;
        return true;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
        update_metrics(ui);
        return true;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
        float d = SDL_GetWindowDisplayScale(ui->window);
        if (d > 0 && d != ui->density) {
            ui->density = d;
            SDL_SetRenderScale(ui->renderer, d, d);
            clear_text_cache(ui);
            open_fonts(ui, g_font_dir);
        }
        update_metrics(ui);
        return true;
    }
    default:
        return false;
    }
}

void ui_begin(Ui *ui)
{
    ui->frame++;
    ui->focus_seen = false;
    SDL_Color bg = ui->theme.bg;
    SDL_SetRenderDrawColor(ui->renderer, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(ui->renderer);
}

void ui_end(Ui *ui)
{
    /* A tap that landed on no text field, or a field that vanished, ends editing. */
    if (ui->focus && (!ui->focus_seen || (ui->released && !ui->was_drag && !ui->field_tapped))) ui_unfocus(ui);
    ui->field_tapped = false;
    ui->pressed = ui->released = false;
    ui->was_drag = false;
    ui->drag_dy = 0;
    ui->wheel = 0;
    ui->typed[0] = '\0';
    ui->backspaces = 0;
    ui->enter = false;

    /* Evict text textures unused for a while. */
    for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
        TextCacheEntry *e = &ui->text_cache[i];
        if (e->s && ui->frame - e->used > 240) {
            SDL_DestroyTexture(e->tex);
            SDL_free(e->s);
            SDL_zerop(e);
        }
    }
}

void ui_fill(Ui *ui, SDL_FRect r, SDL_Color c)
{
    SDL_SetRenderDrawBlendMode(ui->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ui->renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(ui->renderer, &r);
}

void ui_rect_outline(Ui *ui, SDL_FRect r, SDL_Color c)
{
    SDL_SetRenderDrawColor(ui->renderer, c.r, c.g, c.b, c.a);
    SDL_RenderRect(ui->renderer, &r);
}

/* ---- text ------------------------------------------------------------------------------------- */

static TextCacheEntry *text_entry(Ui *ui, FontId f, const char *s, SDL_Color c, int wrap_px)
{
    Uint32 col = pack(c);
    TextCacheEntry *slot = NULL;
    for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
        TextCacheEntry *e = &ui->text_cache[i];
        if (e->s && e->font == f && e->color == col && e->wrap == wrap_px && SDL_strcmp(e->s, s) == 0) {
            e->used = ui->frame;
            return e;
        }
        if (!slot || (!e->s && slot->s) || (e->s && slot->s && e->used < slot->used)) slot = e;
    }
    if (slot->s) {
        SDL_DestroyTexture(slot->tex);
        SDL_free(slot->s);
        SDL_zerop(slot);
    }
    SDL_Surface *surf = wrap_px > 0 ? TTF_RenderText_Blended_Wrapped(ui->fonts[f], s, 0, c, wrap_px)
                                    : TTF_RenderText_Blended(ui->fonts[f], s, 0, c);
    if (!surf) return NULL;
    slot->tex = SDL_CreateTextureFromSurface(ui->renderer, surf);
    slot->w = (float)surf->w / ui->density;
    slot->h = (float)surf->h / ui->density;
    SDL_DestroySurface(surf);
    slot->s = SDL_strdup(s);
    slot->font = f;
    slot->color = col;
    slot->wrap = wrap_px;
    slot->used = ui->frame;
    return slot;
}

void ui_measure(Ui *ui, FontId f, const char *s, float *w, float *h)
{
    int pw = 0, ph = 0;
    if (s && *s) TTF_GetStringSize(ui->fonts[f], s, 0, &pw, &ph);
    else ph = TTF_GetFontHeight(ui->fonts[f]);
    if (w) *w = (float)pw / ui->density;
    if (h) *h = (float)ph / ui->density;
}

float ui_font_height(Ui *ui, FontId f) { return (float)TTF_GetFontHeight(ui->fonts[f]) / ui->density; }

float ui_text(Ui *ui, FontId f, float x, float y, const char *s, SDL_Color c, float max_w)
{
    if (!s || !*s) return 0;
    char elided[512];
    const char *draw = s;
    if (max_w > 0) {
        int fit_w = 0;
        size_t fit_len = 0;
        TTF_MeasureString(ui->fonts[f], s, 0, (int)(max_w * ui->density), &fit_w, &fit_len);
        if (s[fit_len] != '\0') {
            float dots_w;
            ui_measure(ui, f, "...", &dots_w, NULL);
            TTF_MeasureString(ui->fonts[f], s, 0, (int)((max_w - dots_w) * ui->density), &fit_w, &fit_len);
            if (fit_len > sizeof elided - 4) fit_len = sizeof elided - 4;
            SDL_memcpy(elided, s, fit_len);
            SDL_strlcpy(elided + fit_len, "...", 4);
            draw = elided;
        }
    }
    TextCacheEntry *e = text_entry(ui, f, draw, c, 0);
    if (!e) return 0;
    SDL_FRect dst = {SDL_roundf(x * ui->density) / ui->density, SDL_roundf(y * ui->density) / ui->density, e->w, e->h};
    SDL_RenderTexture(ui->renderer, e->tex, NULL, &dst);
    return e->w;
}

float ui_text_wrapped_height(Ui *ui, FontId f, float w, const char *s)
{
    if (!s || !*s) return 0;
    int pw = 0, ph = 0;
    TTF_GetStringSizeWrapped(ui->fonts[f], s, 0, (int)(w * ui->density), &pw, &ph);
    return (float)ph / ui->density;
}

float ui_text_wrapped(Ui *ui, FontId f, float x, float y, float w, const char *s, SDL_Color c)
{
    if (!s || !*s) return 0;
    TextCacheEntry *e = text_entry(ui, f, s, c, (int)(w * ui->density));
    if (!e) return 0;
    SDL_FRect dst = {x, y, e->w, e->h};
    SDL_RenderTexture(ui->renderer, e->tex, NULL, &dst);
    return e->h;
}

/* ---- interaction ------------------------------------------------------------------------------ */

static bool in(SDL_FRect r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static bool press_allowed(Ui *ui)
{
    return !ui->clipping || in(ui->clip, ui->press_x, ui->press_y);
}

bool ui_tap(Ui *ui, SDL_FRect r)
{
    return ui->released && !ui->was_drag && press_allowed(ui) && in(r, ui->press_x, ui->press_y) && in(r, ui->px, ui->py);
}

static bool held(Ui *ui, SDL_FRect r)
{
    return ui->down && !ui->dragging && press_allowed(ui) && in(r, ui->press_x, ui->press_y) && in(r, ui->px, ui->py);
}

bool ui_button(Ui *ui, SDL_FRect r, const char *label, ButtonStyle style)
{
    const Theme *t = &ui->theme;
    SDL_Color bg, fg;
    switch (style) {
    case BTN_PRIMARY: bg = t->primary; fg = t->primary_text; break;
    case BTN_DANGER: bg = t->danger; fg = t->primary_text; break;
    case BTN_GHOST: bg = (SDL_Color){0, 0, 0, 0}; fg = t->primary; break;
    default: bg = t->surface; fg = t->primary; break;
    }
    if (held(ui, r)) {
        if (style == BTN_GHOST || style == BTN_SECONDARY) bg = t->surface_alt;
        else bg = (SDL_Color){(Uint8)(bg.r * 0.8f), (Uint8)(bg.g * 0.8f), (Uint8)(bg.b * 0.8f), 255};
    }
    if (bg.a) ui_fill(ui, r, bg);
    if (style == BTN_SECONDARY) ui_rect_outline(ui, r, t->border);
    float tw, th;
    ui_measure(ui, FONT_BOLD, label, &tw, &th);
    if (tw > r.w - 12) tw = r.w - 12;
    ui_text(ui, FONT_BOLD, r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, label, fg, r.w - 12);
    return ui_tap(ui, r);
}

/* ---- text fields ------------------------------------------------------------------------------ */

static void focus_field(Ui *ui, char *buf, SDL_FRect r, int kind)
{
    if (ui->focus == buf) return;
    if (ui->focus) SDL_StopTextInput(ui->window);
    ui->focus = buf;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, kind);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER,
                          kind == SDL_TEXTINPUT_TYPE_TEXT ? SDL_CAPITALIZE_SENTENCES : SDL_CAPITALIZE_NONE);
    SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, kind == SDL_TEXTINPUT_TYPE_TEXT);
    SDL_StartTextInputWithProperties(ui->window, props);
    SDL_DestroyProperties(props);
    float k = ui->density / SDL_GetWindowPixelDensity(ui->window);
    SDL_Rect area = {(int)(r.x * k), (int)(r.y * k), (int)(r.w * k), (int)(r.h * k)};
    SDL_SetTextInputArea(ui->window, &area, 0);
}

void ui_unfocus(Ui *ui)
{
    if (!ui->focus) return;
    ui->focus = NULL;
    SDL_StopTextInput(ui->window);
}

bool ui_focused(Ui *ui, const char *buf) { return ui->focus == buf; }

bool ui_submitted(Ui *ui, const char *buf) { return ui->focus == buf && ui->enter; }

static size_t utf8_prev(const char *s, size_t len)
{
    if (len == 0) return 0;
    size_t i = len - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

bool ui_textfield(Ui *ui, SDL_FRect r, char *buf, size_t cap, const char *placeholder, int kind)
{
    const Theme *t = &ui->theme;
    bool changed = false;
    if (ui_tap(ui, r)) {
        ui->field_tapped = true;
        focus_field(ui, buf, r, kind);
    }
    bool focused = ui->focus == buf;
    if (focused) {
        ui->focus_seen = true;
        size_t len = SDL_strlen(buf);
        for (int i = 0; i < ui->backspaces && len > 0; i++) {
            len = utf8_prev(buf, len);
            buf[len] = '\0';
            changed = true;
        }
        if (ui->typed[0]) {
            size_t add = SDL_strlen(ui->typed);
            if (len + add < cap) {
                SDL_memcpy(buf + len, ui->typed, add + 1);
                changed = true;
            }
        }
    }

    ui_fill(ui, r, t->surface);
    ui_rect_outline(ui, r, focused ? t->primary : t->border);
    if (focused) ui_rect_outline(ui, (SDL_FRect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, t->primary);

    float pad = 12, fh = ui_font_height(ui, FONT_BODY);
    float ty = r.y + (r.h - fh) / 2;
    char shown[256];
    const char *text = buf;
    if (kind == SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN && buf[0]) {
        size_t n = 0;
        for (const char *p = buf; *p && n < 60; p++)
            if (((unsigned char)*p & 0xC0) != 0x80) shown[n++] = '*';
        shown[n] = '\0';
        text = shown;
    }
    float avail = r.w - pad * 2;
    float tw = 0;
    if (text[0]) {
        /* Show the tail of long input while editing so the caret stays visible. */
        const char *tail = text;
        float w;
        ui_measure(ui, FONT_BODY, tail, &w, NULL);
        while (focused && w > avail - 4 && *tail) {
            tail++;
            while (((unsigned char)*tail & 0xC0) == 0x80) tail++;
            ui_measure(ui, FONT_BODY, tail, &w, NULL);
        }
        tw = ui_text(ui, FONT_BODY, r.x + pad, ty, tail, t->text, focused ? 0 : avail);
    } else if (placeholder && !focused) {
        ui_text(ui, FONT_BODY, r.x + pad, ty, placeholder, t->text_muted, avail);
    }
    if (focused) /* steady caret: the app only redraws on events */
        ui_fill(ui, (SDL_FRect){r.x + pad + tw + 1, ty + 2, 2, fh - 4}, t->primary);
    return changed;
}

/* ---- scrolling -------------------------------------------------------------------------------- */

float ui_scroll_begin(Ui *ui, SDL_FRect view, float *scroll)
{
    if (ui->dragging && in(view, ui->press_x, ui->press_y)) *scroll -= ui->drag_dy;
    if (ui->wheel != 0 && in(view, ui->px, ui->py)) *scroll -= ui->wheel * 48;
    if (*scroll < 0) *scroll = 0;
    ui->clipping = true;
    ui->clip = view;
    ui->scroll_view_top = view.y;
    ui->scroll_view_bottom = view.y + view.h;
    SDL_Rect clip = {(int)view.x, (int)view.y, (int)SDL_ceilf(view.w), (int)SDL_ceilf(view.h)};
    SDL_SetRenderClipRect(ui->renderer, &clip);
    return view.y - *scroll;
}

void ui_scroll_end(Ui *ui, SDL_FRect view, float *scroll, float content_h)
{
    float max = content_h - view.h;
    if (max < 0) max = 0;
    if (*scroll > max) *scroll = max;
    if (*scroll < 0) *scroll = 0;
    SDL_SetRenderClipRect(ui->renderer, NULL);
    ui->clipping = false;
    /* Scroll indicator */
    if (content_h > view.h + 1) {
        float bar_h = SDL_max(24.0f, view.h * view.h / content_h);
        float bar_y = view.y + (view.h - bar_h) * (*scroll / max);
        ui_fill(ui, (SDL_FRect){view.x + view.w - 4, bar_y, 3, bar_h}, (SDL_Color){0, 0, 0, 60});
    }
}

bool ui_visible(Ui *ui, float y, float h)
{
    if (!ui->clipping) return true;
    return y + h >= ui->scroll_view_top && y <= ui->scroll_view_bottom;
}

/* ---- images ----------------------------------------------------------------------------------- */

static ImageCacheEntry *image_entry(Ui *ui, const char *path)
{
    ImageCacheEntry *slot = NULL;
    for (int i = 0; i < IMAGE_CACHE_SIZE; i++) {
        ImageCacheEntry *e = &ui->image_cache[i];
        if (e->path[0] && SDL_strcmp(e->path, path) == 0) {
            e->used = ui->frame;
            return e;
        }
        if (!slot || (!e->path[0] && slot->path[0]) || (e->path[0] && slot->path[0] && e->used < slot->used)) slot = e;
    }
    if (slot->tex) SDL_DestroyTexture(slot->tex);
    SDL_zerop(slot);
    SDL_strlcpy(slot->path, path, sizeof slot->path);
    slot->used = ui->frame;
    SDL_Surface *s = IMG_Load(path);
    if (!s) {
        slot->failed = true;
        return slot;
    }
    int longest = SDL_max(s->w, s->h);
    if (longest > MAX_IMAGE_PX) {
        SDL_Surface *scaled = SDL_ScaleSurface(s, s->w * MAX_IMAGE_PX / longest, s->h * MAX_IMAGE_PX / longest, SDL_SCALEMODE_LINEAR);
        if (scaled) {
            SDL_DestroySurface(s);
            s = scaled;
        }
    }
    slot->tex = SDL_CreateTextureFromSurface(ui->renderer, s);
    slot->w = (float)s->w;
    slot->h = (float)s->h;
    slot->failed = slot->tex == NULL;
    SDL_DestroySurface(s);
    return slot;
}

bool ui_image(Ui *ui, const char *path, SDL_FRect r, bool cover)
{
    ImageCacheEntry *e = image_entry(ui, path);
    if (!e->tex) return false;
    float sx = r.w / e->w, sy = r.h / e->h;
    float s = cover ? SDL_max(sx, sy) : SDL_min(sx, sy);
    float w = e->w * s, h = e->h * s;
    if (cover) {
        /* Crop the source to r's aspect ratio. */
        SDL_FRect src = {(e->w - r.w / s) / 2, 0, r.w / s, r.h / s}; /* top-aligned: titles live at the top */
        SDL_RenderTexture(ui->renderer, e->tex, &src, &r);
    } else {
        SDL_FRect dst = {r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
        SDL_RenderTexture(ui->renderer, e->tex, NULL, &dst);
    }
    return true;
}

void ui_image_forget(Ui *ui, const char *path)
{
    for (int i = 0; i < IMAGE_CACHE_SIZE; i++) {
        ImageCacheEntry *e = &ui->image_cache[i];
        if (e->path[0] && SDL_strcmp(e->path, path) == 0) {
            if (e->tex) SDL_DestroyTexture(e->tex);
            SDL_zerop(e);
        }
    }
}
