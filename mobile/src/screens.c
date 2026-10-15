#include "app.h"
#include "fileno.h"
#include "label.h"
#include "pdf.h"

#define PAD 16.0f
#define BAR_H 52.0f
#define BTN_H 48.0f
#define FIELD_H 46.0f
#define GAP 10.0f

/* ---- navigation ------------------------------------------------------------------------------- */

void app_navigate(App *app, ScreenId to)
{
    ui_unfocus(&app->ui);
    if (app->depth < (int)SDL_arraysize(app->stack)) app->stack[app->depth++] = app->screen;
    app->screen = to;
    app->scroll[to] = 0;
    app->confirm_delete = false;
    app->reload = true;
}

void app_back(App *app)
{
    ui_unfocus(&app->ui);
    app->confirm_delete = false;
    if (app->depth > 0) app->screen = app->stack[--app->depth];
    else app->screen = SCR_HOME;
    app->reload = true;
}

/* Replace the current screen instead of stacking on top of it (e.g. form -> result). */
static void app_replace(App *app, ScreenId to)
{
    ui_unfocus(&app->ui);
    app->screen = to;
    app->scroll[to] = 0;
    app->confirm_delete = false;
    app->reload = true;
}

static Uint32 SDLCALL wake_cb(void *userdata, SDL_TimerID id, Uint32 interval)
{
    (void)id; (void)interval;
    App *app = userdata;
    SDL_Event e;
    SDL_zero(e);
    e.type = app->ev_wake;
    SDL_PushEvent(&e);
    return 0;
}

void app_toast(App *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(app->toast, sizeof app->toast, fmt, ap);
    va_end(ap);
    app->toast_until = SDL_GetTicks() + 3500;
    SDL_AddTimer(3600, wake_cb, app); /* redraw to clear it */
}

static Uint32 SDLCALL autosync_cb(void *userdata, SDL_TimerID id, Uint32 interval)
{
    (void)id; (void)interval;
    App *app = userdata;
    sync_request(app->sync);
    return 0;
}

void app_changed(App *app)
{
    if (app->autosync_timer) SDL_RemoveTimer(app->autosync_timer);
    app->autosync_timer = SDL_AddTimer(4000, autosync_cb, app);
    app->reload = true;
}

void app_open_folder(App *app, const char *id)
{
    Folder f;
    if (!db_folder_get(app->db, id, &f)) {
        app_toast(app, "File %s not found", id);
        return;
    }
    app->folder = f;
    app_navigate(app, SCR_FOLDER);
}

void app_open_document(App *app, const char *id)
{
    Document d;
    if (!db_document_get(app->db, id, &d)) return;
    app->doc = d;
    app->show_text = false;
    app_navigate(app, SCR_DOCUMENT);
}

/* ---- shared widgets --------------------------------------------------------------------------- */

static float content_x(App *app) { return app->ui.safe.x + PAD; }
static float content_w(App *app) { return app->ui.safe.w - PAD * 2; }
static float bottom(App *app) { return app->ui.safe.y + app->ui.safe.h; }

/* Top bar; returns the y just below it. *action set when the right-hand button is tapped. */
static float header(App *app, const char *title, bool back, const char *action, bool *action_tapped)
{
    Ui *ui = &app->ui;
    const Theme *t = &ui->theme;
    float top = ui->safe.y;
    ui_fill(ui, (SDL_FRect){0, 0, ui->w, top + BAR_H}, t->surface);
    ui_fill(ui, (SDL_FRect){0, top + BAR_H - 1, ui->w, 1}, t->border);
    float x = ui->safe.x + 4;
    if (back) {
        if (ui_button(ui, (SDL_FRect){x, top + 4, 72, BAR_H - 8}, "< Back", BTN_GHOST)) app_back(app);
        x += 76;
    } else {
        x = content_x(app);
        /* App logo beside the title on top-level screens. */
        static char logo[1024];
        if (!logo[0]) {
            /* Assets sit next to fonts/: strip that suffix from the font dir ("" on Android = APK assets). */
            SDL_strlcpy(logo, platform_font_dir(), sizeof logo);
            size_t n = SDL_strlen(logo);
            if (n >= 6 && SDL_strcmp(logo + n - 6, "fonts/") == 0) logo[n - 6] = '\0';
            SDL_strlcat(logo, "images/logo.png", sizeof logo);
        }
        float s = BAR_H - 16;
        if (ui_image(ui, logo, (SDL_FRect){x, top + 8, s, s}, false)) x += s + 10;
    }
    float aw = 0;
    if (action) {
        ui_measure(ui, FONT_BOLD, action, &aw, NULL);
        aw += 28;
        bool tapped = ui_button(ui, (SDL_FRect){ui->safe.x + ui->safe.w - aw - 4, top + 4, aw, BAR_H - 8}, action, BTN_GHOST);
        if (action_tapped) *action_tapped = tapped;
    }
    float th = ui_font_height(ui, FONT_TITLE);
    ui_text(ui, FONT_TITLE, x, top + (BAR_H - th) / 2, title, t->text, ui->safe.x + ui->safe.w - aw - 8 - x);
    return top + BAR_H;
}

static void file_badge(App *app, float x, float y, const char *file_number, float max_w)
{
    Ui *ui = &app->ui;
    float w, h;
    ui_measure(ui, FONT_BOLD, file_number, &w, &h);
    if (w > max_w - 16) w = max_w - 16;
    ui_fill(ui, (SDL_FRect){x, y, w + 16, h + 6}, (SDL_Color){0x9A, 0x5B, 0x13, 0x22});
    ui_text(ui, FONT_BOLD, x + 8, y + 3, file_number, ui->theme.accent, max_w - 16);
}

#define CHIPS_H 26.0f

/* Tags as small pills on one line; tags that do not fit collapse into "+N". tags is comma separated. */
static void tag_chips(App *app, float x, float y, float w, const char *tags)
{
    Ui *ui = &app->ui;
    char list[256];
    tags_distinct(tags, list, sizeof list);
    if (!list[0]) return;
    SDL_Color bg = ui->theme.primary, fg = ui->theme.primary;
    bg.a = 0x1C;
    float pad = 8, gap = 6, ch = ui_font_height(ui, FONT_SMALL) + 4;
    float right = x + w, cx = x;
    float plus_w;
    ui_measure(ui, FONT_SMALL, "+99", &plus_w, NULL);
    plus_w += pad * 2;

    int total = 1;
    for (const char *c = list; *c; c++) total += *c == ',';
    int shown = 0;
    char *save = NULL;
    for (char *tag = SDL_strtok_r(list, ",", &save); tag; tag = SDL_strtok_r(NULL, ",", &save)) {
        while (*tag == ' ') tag++;
        float tw;
        ui_measure(ui, FONT_SMALL, tag, &tw, NULL);
        float cw = tw + pad * 2;
        bool last = shown == total - 1;
        float reserve = last ? 0 : plus_w + gap;
        if (cx + cw > right - reserve) {
            /* First chip too long for the row: elide it rather than show nothing. */
            if (shown == 0) {
                cw = right - reserve - cx;
                if (cw < pad * 4) break;
            } else {
                break;
            }
        }
        ui_fill(ui, (SDL_FRect){cx, y, cw, ch}, bg);
        ui_text(ui, FONT_SMALL, cx + pad, y + 2, tag, fg, cw - pad * 2);
        cx += cw + gap;
        shown++;
    }
    if (shown < total) {
        char more[16];
        SDL_snprintf(more, sizeof more, "+%d", total - shown);
        float mw;
        ui_measure(ui, FONT_SMALL, more, &mw, NULL);
        ui_fill(ui, (SDL_FRect){cx, y, mw + pad * 2, ch}, bg);
        ui_text(ui, FONT_SMALL, cx + pad, y + 2, more, fg, 0);
    }
}

static float folder_row(App *app, float y, const Folder *f, bool *tapped)
{
    Ui *ui = &app->ui;
    const Theme *t = &ui->theme;
    float x = content_x(app), w = content_w(app);
    float h = (f->location[0] ? 98 : 76) + (f->tags[0] ? CHIPS_H + 4 : 0);
    *tapped = false;
    if (!ui_visible(ui, y, h)) return h + 8;
    SDL_FRect r = {x, y, w, h};
    ui_fill(ui, r, t->surface);
    ui_rect_outline(ui, r, t->border);
    file_badge(app, x + 12, y + 10, f->file_number, w * 0.6f);
    char count[48];
    if (f->doc_count) SDL_snprintf(count, sizeof count, "%d document%s", f->doc_count, f->doc_count == 1 ? "" : "s");
    else SDL_strlcpy(count, "empty", sizeof count);
    float cw;
    ui_measure(ui, FONT_SMALL, count, &cw, NULL);
    ui_text(ui, FONT_SMALL, x + w - 12 - cw, y + 16, count, t->text_muted, 0);
    ui_text(ui, FONT_BODY, x + 12, y + 44, f->title[0] ? f->title : "(no title)", f->title[0] ? t->text : t->text_muted, w - 24);
    if (f->location[0]) ui_text(ui, FONT_SMALL, x + 12, y + 70, f->location, t->text_muted, w - 24);
    if (f->tags[0]) tag_chips(app, x + 12, y + (f->location[0] ? 96 : 72), w - 24, f->tags);
    *tapped = ui_tap(ui, r);
    return h + 8;
}

static float section_title(App *app, float y, const char *title)
{
    ui_text(&app->ui, FONT_SMALL, content_x(app), y + 6, title, app->ui.theme.text_muted, 0);
    return ui_font_height(&app->ui, FONT_SMALL) + 12;
}

static float paragraph(App *app, float y, const char *text)
{
    return ui_text_wrapped(&app->ui, FONT_BODY, content_x(app), y, content_w(app), text, app->ui.theme.text_muted) + 8;
}

static void two_buttons(App *app, float y, const char *a, ButtonStyle sa, bool *ta, const char *b, ButtonStyle sb, bool *tb)
{
    float x = content_x(app), w = (content_w(app) - GAP) / 2;
    *ta = ui_button(&app->ui, (SDL_FRect){x, y, w, BTN_H}, a, sa);
    *tb = ui_button(&app->ui, (SDL_FRect){x + w + GAP, y, w, BTN_H}, b, sb);
}

static float field(App *app, float y, const char *label, char *buf, size_t cap, const char *placeholder, int kind)
{
    Ui *ui = &app->ui;
    ui_text(ui, FONT_SMALL, content_x(app), y, label, ui->theme.text_muted, 0);
    float lh = ui_font_height(ui, FONT_SMALL) + 4;
    ui_textfield(ui, (SDL_FRect){content_x(app), y + lh, content_w(app), FIELD_H}, buf, cap, placeholder, kind);
    return lh + FIELD_H + 14;
}

static void draw_toast(App *app)
{
    Ui *ui = &app->ui;
    if (!app->toast[0] || SDL_GetTicks() > app->toast_until) return;
    float w = content_w(app);
    float h = ui_text_wrapped_height(ui, FONT_BODY, w - 28, app->toast) + 24;
    SDL_FRect r = {content_x(app), bottom(app) - h - 16, w, h};
    ui_fill(ui, r, (SDL_Color){0x1D, 0x1D, 0x1B, 235});
    ui_text_wrapped(ui, FONT_BODY, r.x + 14, r.y + 12, w - 28, app->toast, (SDL_Color){255, 255, 255, 255});
}

static void start_scan(App *app, bool into_current_doc)
{
    if (app->scanning) return;
    app->scanning = true;
    app->scan_into_current_doc = into_current_doc;
    platform_scan_document();
}

/* ---- home ------------------------------------------------------------------------------------- */

static void reload_home(App *app)
{
    app->nfolders = db_folder_list(app->db, app->folders, MAX_LIST);
    app->nhits = app->nfolder_hits = 0;
    if (app->query[0]) {
        app->nfolder_hits = db_search_folders(app->db, app->query, app->folder_hits, (int)SDL_arraysize(app->folder_hits));
        app->nhits = db_search(app->db, app->query, app->hits, (int)SDL_arraysize(app->hits));
    }
    SDL_strlcpy(app->last_query, app->query, sizeof app->last_query);
}

static void new_folder(App *app, const char *file_number)
{
    SDL_zero(app->edit_folder);
    if (file_number) SDL_strlcpy(app->edit_folder.file_number, file_number, sizeof app->edit_folder.file_number);
    else db_folder_suggest_next(app->db, app->edit_folder.file_number, sizeof app->edit_folder.file_number);
    app->edit_folder_new = true;
    app_navigate(app, SCR_FOLDER_EDIT);
}

static void draw_home(App *app)
{
    Ui *ui = &app->ui;
    const Theme *t = &ui->theme;
    if (app->reload || SDL_strcmp(app->query, app->last_query) != 0) reload_home(app);

    bool settings = false;
    float y = header(app, "MyFiles", false, "Settings", &settings);
    if (settings) {
        app_navigate(app, SCR_SETTINGS);
        return;
    }

    char status[256];
    SyncState st = sync_status(app->sync, status, sizeof status);
    if (!status[0]) SDL_strlcpy(status, "Tap to sync", sizeof status);
    SDL_FRect sr = {content_x(app), y + 6, content_w(app), 22};
    ui_text(ui, FONT_SMALL, sr.x, sr.y, status, st == SYNC_OK ? t->primary : t->text_muted, sr.w);
    if (ui_tap(ui, sr)) {
        if (st == SYNC_SIGNED_OUT || st == SYNC_AUTH_FAILED) app_navigate(app, SCR_SETTINGS);
        else sync_request(app->sync);
    }
    y += 32;

    ui_textfield(ui, (SDL_FRect){content_x(app), y, content_w(app), FIELD_H}, app->query, sizeof app->query,
                 "Search documents, text or file no.", SDL_TEXTINPUT_TYPE_TEXT);
    y += FIELD_H + GAP;

    bool scan_label = false, add = false;
    two_buttons(app, y, "Scan label", BTN_PRIMARY, &scan_label, "New file", BTN_SECONDARY, &add);
    if (scan_label) {
        app->scanning = true;
        platform_scan_label();
    }
    if (add) {
        new_folder(app, NULL);
        return;
    }
    y += BTN_H + GAP;

    SDL_FRect view = {0, y, ui->w, bottom(app) - y};
    float *scroll = &app->scroll[SCR_HOME];
    float cy = ui_scroll_begin(ui, view, scroll), start = cy;
    const char *open_folder = NULL, *open_doc = NULL;

    if (!app->query[0]) {
        if (app->nfolders == 0) {
            cy += 12;
            cy += paragraph(app, cy, "Give each envelope or box file a number, scan its documents into it, and find any paper "
                                     "later - even offline. Start with New file, then print or hand-write the number on the envelope.");
        } else {
            cy += section_title(app, cy, "FILES");
            for (int i = 0; i < app->nfolders; i++) {
                bool tapped;
                cy += folder_row(app, cy, &app->folders[i], &tapped);
                if (tapped) open_folder = app->folders[i].id;
            }
        }
    } else {
        if (app->nfolder_hits) {
            cy += section_title(app, cy, "FILES");
            for (int i = 0; i < app->nfolder_hits; i++) {
                bool tapped;
                cy += folder_row(app, cy, &app->folder_hits[i], &tapped);
                if (tapped) open_folder = app->folder_hits[i].id;
            }
        }
        cy += section_title(app, cy, app->nhits ? "DOCUMENTS" : "NO MATCHING DOCUMENTS");
        float x = content_x(app), w = content_w(app);
        for (int i = 0; i < app->nhits; i++) {
            const SearchHit *h = &app->hits[i];
            float rh = 92 + (h->tags[0] ? CHIPS_H + 4 : 0);
            if (ui_visible(ui, cy, rh)) {
                SDL_FRect r = {x, cy, w, rh};
                ui_fill(ui, r, t->surface);
                ui_rect_outline(ui, r, t->border);
                ui_text(ui, FONT_BOLD, x + 12, cy + 10, h->doc_title[0] ? h->doc_title : "(untitled)", t->text, w * 0.62f);
                char where[200];
                SDL_snprintf(where, sizeof where, "File %s", h->file_number);
                float bw;
                ui_measure(ui, FONT_BOLD, where, &bw, NULL);
                file_badge(app, x + w - 12 - SDL_min(bw + 16, w * 0.34f), cy + 8, where, w * 0.34f);
                ui_text(ui, FONT_SMALL, x + 12, cy + 38, h->folder_title, t->text_muted, w - 24);
                ui_text(ui, FONT_SMALL, x + 12, cy + 62, h->snippet, t->text, w - 24);
                if (h->tags[0]) tag_chips(app, x + 12, cy + 90, w - 24, h->tags);
                if (ui_tap(ui, r)) open_doc = h->doc_id;
            }
            cy += rh + 8;
        }
    }
    ui_scroll_end(ui, view, scroll, cy - start + 24);

    if (open_folder) app_open_folder(app, open_folder);
    else if (open_doc) app_open_document(app, open_doc);
}

/* ---- folder ----------------------------------------------------------------------------------- */

static void draw_folder(App *app)
{
    Ui *ui = &app->ui;
    const Theme *t = &ui->theme;
    if (app->reload) {
        if (!db_folder_get(app->db, app->folder.id, &app->folder)) {
            app_back(app);
            return;
        }
        app->ndocs = db_document_list(app->db, app->folder.id, app->docs, MAX_LIST);
    }
    char title[96];
    SDL_snprintf(title, sizeof title, "File %s", app->folder.file_number);
    bool edit = false;
    float y = header(app, title, true, "Edit", &edit);
    if (edit) {
        app->edit_folder = app->folder;
        app->edit_folder_new = false;
        app_navigate(app, SCR_FOLDER_EDIT);
        return;
    }

    SDL_FRect view = {0, y, ui->w, bottom(app) - y};
    float *scroll = &app->scroll[SCR_FOLDER];
    float cy = ui_scroll_begin(ui, view, scroll) + PAD, start = cy - PAD;
    float x = content_x(app), w = content_w(app);

    ui_text(ui, FONT_HUGE, x, cy, app->folder.file_number, t->accent, w);
    cy += ui_font_height(ui, FONT_HUGE) + 2;
    if (app->folder.title[0]) {
        ui_text(ui, FONT_TITLE, x, cy, app->folder.title, t->text, w);
        cy += ui_font_height(ui, FONT_TITLE) + 4;
    }
    if (app->folder.location[0]) {
        char loc[300];
        SDL_snprintf(loc, sizeof loc, "Kept at: %s", app->folder.location);
        cy += ui_text_wrapped(ui, FONT_BODY, x, cy, w, loc, t->text_muted) + 4;
    }
    if (app->folder.notes[0]) cy += ui_text_wrapped(ui, FONT_BODY, x, cy, w, app->folder.notes, t->text_muted) + 4;
    cy += GAP;

    bool scan = false, label = false;
    two_buttons(app, cy, "Scan document", BTN_PRIMARY, &scan, "Label", BTN_SECONDARY, &label);
    cy += BTN_H + GAP * 2;

    const char *open_doc = NULL;
    if (app->ndocs == 0) {
        cy += paragraph(app, cy, "No documents yet. Tap Scan document and photograph each page - the text is read on this "
                                 "phone so you can search for it later, even without internet.");
    } else {
        cy += section_title(app, cy, "DOCUMENTS");
        for (int i = 0; i < app->ndocs; i++) {
            const Document *d = &app->docs[i];
            float rh = 66 + (d->tags[0] ? CHIPS_H + 4 : 0);
            if (ui_visible(ui, cy, rh)) {
                SDL_FRect r = {x, cy, w, rh};
                ui_fill(ui, r, t->surface);
                ui_rect_outline(ui, r, t->border);
                ui_text(ui, FONT_BOLD, x + 12, cy + 10, d->title[0] ? d->title : "(untitled)", t->text, w - 24);
                char meta[200];
                SDL_snprintf(meta, sizeof meta, "%d page%s%s%s", d->page_count, d->page_count == 1 ? "" : "s",
                             d->doc_date[0] ? "  -  " : "", d->doc_date);
                ui_text(ui, FONT_SMALL, x + 12, cy + 38, meta, t->text_muted, w - 24);
                if (d->tags[0]) tag_chips(app, x + 12, cy + 64, w - 24, d->tags);
                if (ui_tap(ui, r)) open_doc = d->id;
            }
            cy += rh + 8;
        }
    }
    ui_scroll_end(ui, view, scroll, cy - start + 24);

    if (scan) start_scan(app, false);
    else if (label) app_navigate(app, SCR_LABEL);
    else if (open_doc) app_open_document(app, open_doc);
}

static void draw_folder_edit(App *app)
{
    Ui *ui = &app->ui;
    Folder *f = &app->edit_folder;
    bool save = false;
    float y = header(app, app->edit_folder_new ? "New file" : "Edit file", true, "Save", &save);

    SDL_FRect view = {0, y, ui->w, bottom(app) - y};
    float *scroll = &app->scroll[SCR_FOLDER_EDIT];
    float cy = ui_scroll_begin(ui, view, scroll) + PAD, start = cy - PAD;
    cy += field(app, cy, "FILE NUMBER - write this on the envelope, or print a label", f->file_number, sizeof f->file_number, "e.g. F-0001", SDL_TEXTINPUT_TYPE_TEXT);
    cy += field(app, cy, "TITLE", f->title, sizeof f->title, "e.g. House papers", SDL_TEXTINPUT_TYPE_TEXT);
    cy += field(app, cy, "WHERE IT IS KEPT", f->location, sizeof f->location, "e.g. Steel cupboard, shelf 2", SDL_TEXTINPUT_TYPE_TEXT);
    cy += field(app, cy, "NOTES", f->notes, sizeof f->notes, "", SDL_TEXTINPUT_TYPE_TEXT);

    bool scan = false;
    if (app->edit_folder_new) {
        /* Most new envelopes are created with papers in hand: save and go straight to the scanner. */
        scan = ui_button(ui, (SDL_FRect){content_x(app), cy, content_w(app), BTN_H}, "Save & scan pages", BTN_PRIMARY);
        cy += BTN_H + GAP;
        if (ui_button(ui, (SDL_FRect){content_x(app), cy, content_w(app), BTN_H}, "Save without scanning", BTN_SECONDARY)) save = true;
        cy += BTN_H + GAP;
        cy += ui_text_wrapped(ui, FONT_SMALL, content_x(app), cy, content_w(app),
                              "Scan as many pages as you like. They are read for search and saved as one PDF in this file.",
                              ui->theme.text_muted) + GAP;
    } else {
        if (ui_button(ui, (SDL_FRect){content_x(app), cy, content_w(app), BTN_H}, "Save", BTN_PRIMARY)) save = true;
        cy += BTN_H + GAP;
    }
    bool del = false;
    if (!app->edit_folder_new) {
        del = ui_button(ui, (SDL_FRect){content_x(app), cy, content_w(app), BTN_H},
                        app->confirm_delete ? "Tap again to delete file and its documents" : "Delete file", BTN_DANGER);
        cy += BTN_H + GAP;
    }
    ui_scroll_end(ui, view, scroll, cy - start + 24);

    if (save || scan) {
        char id[FILENO_MAX + 1];
        if (fileno_normalize(f->file_number, id, sizeof id) == 0) {
            app_toast(app, "Enter a file number with letters or digits");
            return;
        }
        Folder existing;
        bool clash = db_folder_get(app->db, id, &existing) && (app->edit_folder_new || SDL_strcmp(id, f->id) != 0);
        if (clash && app->edit_folder_new) {
            app_toast(app, scan ? "File %s already exists - scanning into it" : "File %s already exists - opened it", existing.file_number);
            app->folder = existing;
            app_replace(app, SCR_FOLDER);
            if (scan) start_scan(app, false);
            return;
        }
        if (clash) {
            app_toast(app, "Another file already uses number %s", existing.file_number);
            return;
        }
        if (!db_folder_save(app->db, f)) {
            app_toast(app, "Could not save");
            return;
        }
        app->folder = *f;
        app_changed(app);
        if (app->edit_folder_new) app_replace(app, SCR_FOLDER);
        else app_back(app);
        if (scan) start_scan(app, false);
    } else if (del) {
        if (!app->confirm_delete) {
            app->confirm_delete = true;
            return;
        }
        db_folder_delete(app->db, f->id);
        app_changed(app);
        app->depth = 0;
        app_replace(app, SCR_HOME);
        app_toast(app, "File %s deleted", f->file_number);
    }
}

/* ---- document --------------------------------------------------------------------------------- */

static void page_path(App *app, const Page *p, char *out, size_t cap)
{
    SDL_snprintf(out, cap, "%s%s", app->pages_dir, p->image_file);
}

static void pdf_path(App *app, const char *doc_id, char *out, size_t cap)
{
    SDL_snprintf(out, cap, "%s%s.pdf", app->pdfs_dir, doc_id);
}

/* Rebuilds the document's PDF from the page images on this device. Pages whose image has not been
 * downloaded yet are counted in *missing. Returns the number of pages in the PDF. */
static int build_pdf(App *app, const char *doc_id, int *missing)
{
    Page *pages = SDL_malloc(sizeof *pages * MAX_PAGES);
    int n = db_page_list(app->db, doc_id, pages, MAX_PAGES);
    char (*paths)[1200] = SDL_malloc(sizeof *paths * (size_t)(n ? n : 1));
    const char **ptrs = SDL_malloc(sizeof *ptrs * (size_t)(n ? n : 1));
    int have = 0;
    for (int i = 0; i < n; i++) {
        if (!pages[i].image_file[0]) continue;
        page_path(app, &pages[i], paths[have], sizeof paths[have]);
        ptrs[have] = paths[have];
        have++;
    }
    if (missing) *missing = n - have;
    char out[1200];
    pdf_path(app, doc_id, out, sizeof out);
    int written = have ? pdf_from_jpegs(ptrs, have, out) : 0;
    SDL_free(ptrs);
    SDL_free(paths);
    SDL_free(pages);
    return written;
}

/* Shares the PDF under a readable name, e.g. "F-0042 Electricity bill.pdf". */
static void share_pdf(App *app)
{
    int missing = 0;
    int pages = build_pdf(app, app->doc.id, &missing);
    if (pages == 0) {
        app_toast(app, missing ? "The scans are not on this phone yet - sync first" : "This document has no pages yet");
        return;
    }
    char name[300], src[1200], dest[1400];
    SDL_snprintf(name, sizeof name, "%s %s", app->folder.file_number, app->doc.title[0] ? app->doc.title : "Document");
    for (char *c = name; *c; c++)
        if (SDL_strchr("/\\:*?\"<>|", *c)) *c = '-';
    pdf_path(app, app->doc.id, src, sizeof src);
    SDL_snprintf(dest, sizeof dest, "%s%s.pdf", app->tmp_dir, name);
    if (!SDL_CopyFile(src, dest)) {
        app_toast(app, "Could not prepare the PDF");
        return;
    }
    platform_share_file(dest, "application/pdf");
    if (missing) app_toast(app, "%d page%s not downloaded yet and left out", missing, missing == 1 ? "" : "s");
}

static void draw_document(App *app)
{
    Ui *ui = &app->ui;
    const Theme *t = &ui->theme;
    if (app->reload) {
        if (!db_document_get(app->db, app->doc.id, &app->doc)) {
            app_back(app);
            return;
        }
        app->npages = db_page_list(app->db, app->doc.id, app->pages, MAX_PAGES);
        SDL_free(app->ocr_text);
        app->ocr_text = db_document_ocr_text(app->db, app->doc.id);
        Folder f;
        if (db_folder_get(app->db, app->doc.folder_id, &f)) app->folder = f;
    }
    bool edit = false;
    float y = header(app, app->doc.title[0] ? app->doc.title : "Document", true, "Edit", &edit);
    if (edit) {
        app->edit_doc = app->doc;
        SDL_strlcpy(app->edit_doc_file, app->folder.file_number, sizeof app->edit_doc_file);
        app_navigate(app, SCR_DOCUMENT_EDIT);
        return;
    }

    SDL_FRect view = {0, y, ui->w, bottom(app) - y};
    float *scroll = &app->scroll[SCR_DOCUMENT];
    float cy = ui_scroll_begin(ui, view, scroll) + PAD, start = cy - PAD;
    float x = content_x(app), w = content_w(app);

    /* Where the paper is: the most important fact on this screen. */
    SDL_FRect where = {x, cy, w, 64};
    ui_fill(ui, where, t->surface);
    ui_rect_outline(ui, where, t->border);
    ui_text(ui, FONT_SMALL, x + 12, cy + 8, "PAPER COPY IS IN FILE", t->text_muted, w - 24);
    char ftitle[400];
    SDL_snprintf(ftitle, sizeof ftitle, "%s%s%s", app->folder.file_number, app->folder.title[0] ? "  -  " : "", app->folder.title);
    ui_text(ui, FONT_BOLD, x + 12, cy + 32, ftitle, t->accent, w - 24);
    bool open_folder = ui_tap(ui, where);
    cy += 64 + GAP;

    if (app->doc.doc_date[0] || app->doc.tags[0]) {
        char meta[400];
        SDL_snprintf(meta, sizeof meta, "%s%s%s", app->doc.doc_date, app->doc.doc_date[0] && app->doc.tags[0] ? "  -  " : "", app->doc.tags);
        cy += ui_text_wrapped(ui, FONT_BODY, x, cy, w, meta, t->text_muted) + GAP;
    }

    bool add = false, toggle = false;
    two_buttons(app, cy, "Add pages", BTN_PRIMARY, &add, app->show_text ? "Show pages" : "Show text", BTN_SECONDARY, &toggle);
    cy += BTN_H + GAP;
    bool pdf = false;
    if (app->npages > 0) {
        char lbl[64];
        SDL_snprintf(lbl, sizeof lbl, "View / share PDF (%d page%s)", app->npages, app->npages == 1 ? "" : "s");
        pdf = ui_button(ui, (SDL_FRect){x, cy, w, BTN_H}, lbl, BTN_SECONDARY);
        cy += BTN_H + GAP;
    }
    cy += GAP;

    int open_page = -1;
    if (app->show_text) {
        const char *text = app->ocr_text && app->ocr_text[0] ? app->ocr_text : "No text was recognised on these pages.";
        /* Very long text would exceed GPU texture limits as one image; show the first part. */
        char *part = NULL;
        if (SDL_strlen(text) > 6000) {
            SDL_asprintf(&part, "%.6000s\n\n... (search still covers the full text)", text);
            text = part;
        }
        cy += ui_text_wrapped(ui, FONT_BODY, x, cy, w, text, t->text) + GAP;
        SDL_free(part);
    } else {
        float cw = (w - GAP) / 2, ch = cw * 1.35f;
        for (int i = 0; i < app->npages; i++) {
            const Page *p = &app->pages[i];
            float px = x + (i % 2) * (cw + GAP), py = cy + (float)(i / 2) * (ch + GAP + 22);
            if (ui_visible(ui, py, ch + 22)) {
                SDL_FRect r = {px, py, cw, ch};
                ui_fill(ui, r, t->surface_alt);
                bool shown = false;
                if (p->image_file[0]) {
                    char path[1200];
                    page_path(app, p, path, sizeof path);
                    shown = ui_image(ui, path, r, true);
                }
                if (!shown) {
                    const char *msg = p->has_remote_image ? "On server -\nsync to download" : "Image missing";
                    ui_text_wrapped(ui, FONT_SMALL, r.x + 10, r.y + r.h / 2 - 16, r.w - 20, msg, t->text_muted);
                }
                ui_rect_outline(ui, r, t->border);
                char lbl[32];
                SDL_snprintf(lbl, sizeof lbl, "Page %d", p->page_no);
                ui_text(ui, FONT_SMALL, px, py + ch + 3, lbl, t->text_muted, cw);
                if (ui_tap(ui, r)) open_page = i;
            }
        }
        cy += (float)((app->npages + 1) / 2) * (ch + GAP + 22);
        if (app->npages == 0) cy += paragraph(app, cy, "No pages yet. Tap Add pages to scan them.");
    }
    ui_scroll_end(ui, view, scroll, cy - start + 24);

    if (open_folder) app_open_folder(app, app->folder.id);
    else if (pdf) share_pdf(app);
    else if (add) start_scan(app, true);
    else if (toggle) {
        app->show_text = !app->show_text;
        app->scroll[SCR_DOCUMENT] = 0;
    } else if (open_page >= 0) {
        app->page_index = open_page;
        app_navigate(app, SCR_PAGE);
    }
}

static void draw_document_edit(App *app)
{
    Ui *ui = &app->ui;
    Document *d = &app->edit_doc;
    bool save = false;
    float y = header(app, "Edit document", true, "Save", &save);
    SDL_FRect view = {0, y, ui->w, bottom(app) - y};
    float *scroll = &app->scroll[SCR_DOCUMENT_EDIT];
    float cy = ui_scroll_begin(ui, view, scroll) + PAD, start = cy - PAD;
    cy += field(app, cy, "TITLE", d->title, sizeof d->title, "e.g. Electricity bill", SDL_TEXTINPUT_TYPE_TEXT);
    cy += field(app, cy, "DATE ON THE DOCUMENT", d->doc_date, sizeof d->doc_date, "e.g. 2026-03-14", SDL_TEXTINPUT_TYPE_TEXT);
    cy += field(app, cy, "TAGS", d->tags, sizeof d->tags, "e.g. tax, insurance", SDL_TEXTINPUT_TYPE_TEXT);
    cy += field(app, cy, "FILE NUMBER - change to move it to another envelope", app->edit_doc_file, sizeof app->edit_doc_file, "", SDL_TEXTINPUT_TYPE_TEXT);
    if (ui_button(ui, (SDL_FRect){content_x(app), cy, content_w(app), BTN_H}, "Save", BTN_PRIMARY)) save = true;
    cy += BTN_H + GAP;
    bool del = ui_button(ui, (SDL_FRect){content_x(app), cy, content_w(app), BTN_H},
                         app->confirm_delete ? "Tap again to delete document" : "Delete document", BTN_DANGER);
    cy += BTN_H + GAP;
    ui_scroll_end(ui, view, scroll, cy - start + 24);

    if (save) {
        char id[FILENO_MAX + 1];
        if (fileno_normalize(app->edit_doc_file, id, sizeof id) == 0) {
            app_toast(app, "Enter the file number it is kept in");
            return;
        }
        Folder f;
        if (!db_folder_get(app->db, id, &f)) {
            /* Moving into an envelope that is not registered yet: create it. */
            SDL_zero(f);
            SDL_strlcpy(f.file_number, app->edit_doc_file, sizeof f.file_number);
            db_folder_save(app->db, &f);
            app_toast(app, "Created file %s", f.file_number);
        }
        SDL_strlcpy(d->folder_id, f.id, sizeof d->folder_id);
        db_document_save(app->db, d);
        app->doc = *d;
        app_changed(app);
        app_back(app);
    } else if (del) {
        if (!app->confirm_delete) {
            app->confirm_delete = true;
            return;
        }
        db_document_delete(app->db, d->id);
        char pdf[1200];
        pdf_path(app, d->id, pdf, sizeof pdf);
        SDL_RemovePath(pdf);
        app_changed(app);
        app_back(app); /* document view */
        app_back(app); /* whatever opened it */
        app_toast(app, "Document deleted");
    }
}

static void draw_page(App *app)
{
    Ui *ui = &app->ui;
    if (app->npages == 0) {
        app_back(app);
        return;
    }
    if (app->page_index >= app->npages) app->page_index = app->npages - 1;
    const Page *p = &app->pages[app->page_index];
    char title[64];
    SDL_snprintf(title, sizeof title, "Page %d of %d", app->page_index + 1, app->npages);
    float y = header(app, title, true, NULL, NULL);
    float bh = BTN_H + PAD * 2;
    SDL_FRect r = {ui->safe.x + 8, y + 8, ui->safe.w - 16, bottom(app) - y - bh - 8};
    bool shown = false;
    if (p->image_file[0]) {
        char path[1200];
        page_path(app, p, path, sizeof path);
        shown = ui_image(ui, path, r, false);
    }
    if (!shown) ui_text_wrapped(ui, FONT_BODY, r.x + 16, r.y + r.h / 2, r.w - 32, "This page's image is not on this phone yet. Sync to download it.", ui->theme.text_muted);
    bool prev = false, next = false;
    two_buttons(app, bottom(app) - BTN_H - PAD, "Previous", BTN_SECONDARY, &prev, "Next", BTN_SECONDARY, &next);
    if (prev && app->page_index > 0) app->page_index--;
    if (next && app->page_index < app->npages - 1) app->page_index++;
}

/* ---- labels ----------------------------------------------------------------------------------- */

static void draw_label(App *app)
{
    Ui *ui = &app->ui;
    float y = header(app, "Label", true, NULL, NULL) + PAD;
    float w = content_w(app), h = SDL_min(w * 0.5f, 220);
    label_draw(ui, (SDL_FRect){content_x(app), y, w, h}, app->folder.file_number);
    y += h + PAD;
    y += paragraph(app, y, "Print this and stick it on the envelope, or just write the number on it in big letters. "
                           "Scan label on the home screen reads either and opens this file.");
    y += GAP;
    if (ui_button(ui, (SDL_FRect){content_x(app), y, w, BTN_H}, "Share / print label", BTN_PRIMARY)) {
        char path[1200];
        SDL_snprintf(path, sizeof path, "%slabel-%s.png", app->tmp_dir, app->folder.id);
        if (label_save_png(ui, app->folder.file_number, path)) platform_share_file(path, "image/png");
        else app_toast(app, "Could not create the label image");
    }
}

/* Candidate folder ids from recognised label text: each line, and each word that contains a digit,
 * plus a variant fixing common handwriting misreads in the numeric part (O->0, I/L->1). */
static void add_candidate(App *app, const char *raw)
{
    char id[FILENO_MAX + 1];
    if (fileno_normalize(raw, id, sizeof id) == 0) return;
    bool has_digit = false;
    for (const char *p = id; *p; p++) has_digit |= SDL_isdigit((unsigned char)*p) != 0;
    if (!has_digit) return;
    for (int i = 0; i < app->ncandidates; i++)
        if (SDL_strcmp(app->candidates[i], id) == 0) return;
    if (app->ncandidates < MAX_CANDIDATES) SDL_strlcpy(app->candidates[app->ncandidates++], id, FILENO_MAX + 1);
}

static void misread_variant(const char *in, char *out, size_t cap)
{
    SDL_strlcpy(out, in, cap);
    char *p = out;
    while (*p && !SDL_isdigit((unsigned char)*p)) p++;
    for (; *p; p++) {
        char c = (char)SDL_toupper((unsigned char)*p);
        if (c == 'O') *p = '0';
        else if (c == 'I' || c == 'L') *p = '1';
    }
}

static void collect_candidates(App *app, const char *text)
{
    app->ncandidates = 0;
    char *copy = SDL_strdup(text);
    char *save_line = NULL;
    for (char *line = SDL_strtok_r(copy, "\n", &save_line); line; line = SDL_strtok_r(NULL, "\n", &save_line)) {
        char *words = SDL_strdup(line);
        char *save_w = NULL;
        for (char *w = SDL_strtok_r(words, " \t:", &save_w); w; w = SDL_strtok_r(NULL, " \t:", &save_w)) {
            char v[FILENO_MAX + 1];
            add_candidate(app, w);
            misread_variant(w, v, sizeof v);
            add_candidate(app, v);
        }
        SDL_free(words);
        add_candidate(app, line);
    }
    SDL_free(copy);
}

void screens_on_label(App *app, LabelResult *r)
{
    app->scanning = false;
    if (r->cancelled) return;
    if (!r->text || !r->text[0]) {
        app_toast(app, "%s", r->error[0] ? r->error : "Nothing recognised - try again closer, with more light");
        return;
    }
    char id[FILENO_MAX + 1];
    Folder f;
    if (r->from_qr || SDL_strncmp(r->text, FILENO_QR_PREFIX, SDL_strlen(FILENO_QR_PREFIX)) == 0) {
        if (!fileno_from_qr(r->text, id, sizeof id)) {
            app_toast(app, "That QR code is not a MyFiles label");
            return;
        }
        if (db_folder_get(app->db, id, &f)) app_open_folder(app, id);
        else new_folder(app, id); /* label printed elsewhere, or not synced yet */
        return;
    }
    collect_candidates(app, r->text);
    for (int i = 0; i < app->ncandidates; i++) {
        if (db_folder_get(app->db, app->candidates[i], &f)) {
            app_open_folder(app, f.id);
            return;
        }
    }
    SDL_strlcpy(app->label_raw, r->text, sizeof app->label_raw);
    app_navigate(app, SCR_LABEL_MATCH);
}

static void draw_label_match(App *app)
{
    Ui *ui = &app->ui;
    float y = header(app, "Which file?", true, NULL, NULL) + PAD;
    y += paragraph(app, y, "No existing file matched the number on this envelope. Read from the label:");
    y += ui_text_wrapped(ui, FONT_BOLD, content_x(app), y, content_w(app), app->label_raw, ui->theme.text) + PAD;
    const char *create = NULL;
    for (int i = 0; i < app->ncandidates; i++) {
        char lbl[96];
        SDL_snprintf(lbl, sizeof lbl, "Create file %s", app->candidates[i]);
        if (ui_button(ui, (SDL_FRect){content_x(app), y, content_w(app), BTN_H}, lbl, i == 0 ? BTN_PRIMARY : BTN_SECONDARY))
            create = app->candidates[i];
        y += BTN_H + GAP;
    }
    bool type = ui_button(ui, (SDL_FRect){content_x(app), y, content_w(app), BTN_H}, "Type the number instead", BTN_GHOST);
    if (create) {
        char id[FILENO_MAX + 1];
        SDL_strlcpy(id, create, sizeof id);
        app_back(app);
        new_folder(app, id);
    } else if (type) {
        app_back(app);
        SDL_zero(app->edit_folder);
        app->edit_folder_new = true;
        app_navigate(app, SCR_FOLDER_EDIT);
    }
}

/* ---- scans ------------------------------------------------------------------------------------ */

/* First line of OCR text that looks like a title (has 3+ letters), trimmed to 60 chars. */
static void guess_title(const char *ocr, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = ocr ? ocr : "";
    while (*p) {
        const char *end = SDL_strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : SDL_strlen(p);
        int letters = 0;
        for (size_t i = 0; i < len; i++) letters += SDL_isalpha((unsigned char)p[i]) != 0;
        if (letters >= 3) {
            while (len && (*p == ' ' || *p == '\t')) { p++; len--; }
            if (len > 60) len = 60;
            if (len >= cap) len = cap - 1;
            SDL_memcpy(out, p, len);
            out[len] = '\0';
            return;
        }
        if (!end) break;
        p = end + 1;
    }
    SDL_DateTime dt;
    SDL_Time t;
    SDL_GetCurrentTime(&t);
    SDL_TimeToDateTime(t, &dt, true);
    static const char *MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    SDL_snprintf(out, cap, "Scan %d %s %d", dt.day, MON[(dt.month - 1) % 12], dt.year);
}

void screens_on_scan(App *app, ScanResult *r)
{
    app->scanning = false;
    if (r->cancelled) return;
    if (r->count == 0) {
        app_toast(app, "%s", r->error[0] ? r->error : "No pages were scanned");
        return;
    }
    bool into_doc = app->scan_into_current_doc && (app->screen == SCR_DOCUMENT) && app->doc.id[0];
    if (!into_doc) {
        if (!app->folder.id[0]) {
            app_toast(app, "Open a file first, then scan into it");
            return;
        }
        Document d;
        SDL_zero(d);
        SDL_strlcpy(d.folder_id, app->folder.id, sizeof d.folder_id);
        guess_title(r->pages[0].ocr_text, d.title, sizeof d.title);
        if (!db_document_save(app->db, &d)) {
            app_toast(app, "Could not save the document");
            return;
        }
        app->doc = d;
    }
    int saved = 0;
    for (int i = 0; i < r->count; i++) {
        ScanPage *sp = &r->pages[i];
        char id[ID_LEN], file[ID_LEN + 8], dest[1200];
        new_id(id);
        SDL_snprintf(file, sizeof file, "%s.jpg", id);
        SDL_snprintf(dest, sizeof dest, "%s%s", app->pages_dir, file);
        if (!SDL_RenamePath(sp->path, dest)) {
            if (!SDL_CopyFile(sp->path, dest)) continue;
            SDL_RemovePath(sp->path);
        }
        if (db_page_add(app->db, app->doc.id, id, file, sp->width, sp->height, sp->ocr_text)) saved++;
    }
    app_changed(app);
    int missing = 0;
    int in_pdf = build_pdf(app, app->doc.id, &missing);
    if (into_doc) app->reload = true;
    else app_open_document(app, app->doc.id);
    if (in_pdf > 0) app_toast(app, "Saved %d page%s - PDF has %d page%s%s", saved, saved == 1 ? "" : "s", in_pdf,
                              in_pdf == 1 ? "" : "s", r->error[0] ? " (some failed)" : "");
    else app_toast(app, "Saved %d page%s%s", saved, saved == 1 ? "" : "s", r->error[0] ? " (some failed)" : "");
}

/* ---- settings --------------------------------------------------------------------------------- */

static void draw_settings(App *app)
{
    Ui *ui = &app->ui;
    const Theme *t = &ui->theme;
    if (app->reload) {
        db_meta_get(app->db, "server_url", app->server_url, sizeof app->server_url);
        db_meta_get(app->db, "email", app->email, sizeof app->email);
    }
    float y = header(app, "Settings", true, NULL, NULL);
    SDL_FRect view = {0, y, ui->w, bottom(app) - y};
    float *scroll = &app->scroll[SCR_SETTINGS];
    float cy = ui_scroll_begin(ui, view, scroll) + PAD, start = cy - PAD;
    float x = content_x(app), w = content_w(app);

    char token[16] = "";
    db_meta_get(app->db, "token", token, sizeof token);
    bool signed_in = token[0] != '\0';

    char status[256];
    sync_status(app->sync, status, sizeof status);
    SyncCounts c;
    db_sync_counts(app->db, &c);
    char counts[200];
    SDL_snprintf(counts, sizeof counts, "Waiting to sync: %d change%s, %d scan%s to upload, %d to download",
                 c.pending_rows, c.pending_rows == 1 ? "" : "s", c.pending_images, c.pending_images == 1 ? "" : "s", c.missing_images);
    cy += section_title(app, cy, "SYNC");
    if (status[0]) cy += ui_text_wrapped(ui, FONT_BODY, x, cy, w, status, t->text) + 4;
    cy += ui_text_wrapped(ui, FONT_SMALL, x, cy, w, counts, t->text_muted) + GAP;

    bool sign_in = false, create = false, sync_now = false, sign_out = false, sheet = false;
    if (signed_in) {
        char who[300];
        SDL_snprintf(who, sizeof who, "Signed in as %s on %s", app->email, app->server_url);
        cy += ui_text_wrapped(ui, FONT_BODY, x, cy, w, who, t->text) + GAP;
        two_buttons(app, cy, "Sync now", BTN_PRIMARY, &sync_now, "Sign out", BTN_SECONDARY, &sign_out);
        cy += BTN_H + GAP;
        sheet = ui_button(ui, (SDL_FRect){x, cy, w, BTN_H}, "Print a sheet of numbered labels", BTN_SECONDARY);
        cy += BTN_H + GAP * 2;
    } else {
        cy += paragraph(app, cy, "Everything works offline. Sign in to back up your scans and see them on your other devices.");
        cy += field(app, cy, "SERVER", app->server_url, sizeof app->server_url, "https://files.example.com", SDL_TEXTINPUT_TYPE_TEXT_USERNAME);
        cy += field(app, cy, "EMAIL", app->email, sizeof app->email, "you@example.com", SDL_TEXTINPUT_TYPE_TEXT_EMAIL);
        cy += field(app, cy, "PASSWORD (8+ characters)", app->password, sizeof app->password, "", SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN);
        two_buttons(app, cy, "Sign in", BTN_PRIMARY, &sign_in, "Create account", BTN_SECONDARY, &create);
        cy += BTN_H + GAP * 2;
    }
    cy += section_title(app, cy, "ABOUT");
    cy += paragraph(app, cy, "Documents, scans and the search index are stored on this phone. File numbers ignore case, "
                             "spaces and leading zeros, so F-0042, f 42 and F42-style labels all find the same file.");
    ui_scroll_end(ui, view, scroll, cy - start + 24);

    if (sign_in || create) {
        if (!app->server_url[0] || !app->email[0] || SDL_strlen(app->password) < 8) {
            app_toast(app, "Enter the server, your email and a password of at least 8 characters");
            return;
        }
        ui_unfocus(ui);
        sync_sign_in(app->sync, app->server_url, app->email, app->password, create);
        SDL_memset(app->password, 0, sizeof app->password);
    } else if (sync_now) {
        sync_request(app->sync);
    } else if (sign_out) {
        db_meta_set(app->db, "token", "");
        app_toast(app, "Signed out. Your documents stay on this phone.");
        app->reload = true;
    } else if (sheet) {
        char url[512];
        SDL_snprintf(url, sizeof url, "%s/api/labels/sheet.svg?prefix=F-&from=1&count=10&pad=4", app->server_url);
        SDL_OpenURL(url);
    }
}

/* ---- dispatch --------------------------------------------------------------------------------- */

void screens_draw(App *app)
{
    ScreenId before = app->screen;
    switch (app->screen) {
    case SCR_HOME: draw_home(app); break;
    case SCR_FOLDER: draw_folder(app); break;
    case SCR_FOLDER_EDIT: draw_folder_edit(app); break;
    case SCR_DOCUMENT: draw_document(app); break;
    case SCR_DOCUMENT_EDIT: draw_document_edit(app); break;
    case SCR_PAGE: draw_page(app); break;
    case SCR_LABEL: draw_label(app); break;
    case SCR_LABEL_MATCH: draw_label_match(app); break;
    case SCR_SETTINGS: draw_settings(app); break;
    default: app->screen = SCR_HOME; break;
    }
    /* Keep reload set across a navigation so the new screen loads its data on its first frame. */
    if (app->screen == before) app->reload = false;
    draw_toast(app);
}
