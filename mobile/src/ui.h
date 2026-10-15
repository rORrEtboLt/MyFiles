#ifndef MYFILES_UI_H
#define MYFILES_UI_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdbool.h>

/* A small immediate-mode UI on SDL_Renderer + SDL_ttf, sized for touch.
 * Coordinates are in points (SDL window coordinates); text and images are rasterised at the display's
 * pixel density so they stay sharp. Taps are recognised on release and cancelled by a drag, so lists
 * can be scrolled with a finger without triggering rows. */

typedef enum { FONT_SMALL, FONT_BODY, FONT_BOLD, FONT_TITLE, FONT_HUGE, FONT_COUNT } FontId;

typedef enum { BTN_PRIMARY, BTN_SECONDARY, BTN_DANGER, BTN_GHOST } ButtonStyle;

typedef struct {
    SDL_Color bg, surface, surface_alt, border, text, text_muted, primary, primary_text, danger, accent;
} Theme;

typedef struct TextCacheEntry TextCacheEntry;
typedef struct ImageCacheEntry ImageCacheEntry;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *fonts[FONT_COUNT];
    float density;
    float w, h;      /* window size, points */
    SDL_FRect safe;  /* area clear of notches / home indicator */
    Theme theme;

    /* pointer */
    float px, py;
    bool down, pressed, released;
    float press_x, press_y;
    bool dragging, was_drag;
    float drag_dy;   /* vertical movement this frame while dragging */
    float wheel;

    /* text input */
    char *focus;     /* buffer being edited, NULL if none */
    bool focus_seen; /* focused field was drawn this frame */
    bool field_tapped; /* some text field was tapped this frame */
    char typed[64];
    int backspaces;
    bool enter;

    /* clipping for the active scroll view */
    bool clipping;
    SDL_FRect clip;
    float scroll_view_top, scroll_view_bottom;

    Uint64 frame;
    TextCacheEntry *text_cache;
    ImageCacheEntry *image_cache;
} Ui;

bool ui_init(Ui *ui, SDL_Window *window, SDL_Renderer *renderer, const char *font_dir);
void ui_shutdown(Ui *ui);
/* Returns true if the event should cause a redraw. */
bool ui_event(Ui *ui, const SDL_Event *e);
void ui_begin(Ui *ui);
void ui_end(Ui *ui);

void ui_fill(Ui *ui, SDL_FRect r, SDL_Color c);
void ui_rect_outline(Ui *ui, SDL_FRect r, SDL_Color c);
void ui_measure(Ui *ui, FontId f, const char *s, float *w, float *h);
float ui_font_height(Ui *ui, FontId f);
/* Draws one line, elided with "..." to fit max_w (0 = no limit). Returns width drawn. */
float ui_text(Ui *ui, FontId f, float x, float y, const char *s, SDL_Color c, float max_w);
/* Draws wrapped text; returns its height. */
float ui_text_wrapped(Ui *ui, FontId f, float x, float y, float w, const char *s, SDL_Color c);
/* Text height for layout without drawing. */
float ui_text_wrapped_height(Ui *ui, FontId f, float w, const char *s);

bool ui_tap(Ui *ui, SDL_FRect r);
bool ui_button(Ui *ui, SDL_FRect r, const char *label, ButtonStyle style);
/* Single-line text field. kind: SDL_TextInputType (e.g. SDL_TEXTINPUT_TYPE_TEXT_EMAIL). Returns true when text changed. */
bool ui_textfield(Ui *ui, SDL_FRect r, char *buf, size_t cap, const char *placeholder, int kind);
bool ui_focused(Ui *ui, const char *buf);
void ui_unfocus(Ui *ui);
/* Enter pressed in the focused field this frame. */
bool ui_submitted(Ui *ui, const char *buf);

/* Vertical scroll view. Draw content between begin/end, offset by the returned y origin.
 * ui_visible() lets callers skip rows outside the view. */
float ui_scroll_begin(Ui *ui, SDL_FRect view, float *scroll);
void ui_scroll_end(Ui *ui, SDL_FRect view, float *scroll, float content_h);
bool ui_visible(Ui *ui, float y, float h);

/* Draws an image file scaled to fit inside r (aspect preserved). Returns false if it could not load. */
bool ui_image(Ui *ui, const char *path, SDL_FRect r, bool cover);
void ui_image_forget(Ui *ui, const char *path);

#endif
