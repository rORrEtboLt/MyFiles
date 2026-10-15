/* MyFiles: an offline-first index of paper documents kept in numbered envelopes/files.
 * SDL3 main callbacks are used because iOS and Android own the run loop. The app redraws only when
 * an event arrives (SDL_HINT_MAIN_CALLBACK_RATE=waitevent), which keeps it idle on battery. */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "app.h"

#define AUTOSYNC_INTERVAL_MS (10 * 60 * 1000)

static Uint32 SDLCALL periodic_sync(void *userdata, SDL_TimerID id, Uint32 interval)
{
    (void)id;
    sync_request(((App *)userdata)->sync);
    return interval;
}

static bool make_dir(const char *base, const char *name, char *out, size_t cap)
{
    SDL_snprintf(out, cap, "%s%s/", base, name);
    return SDL_CreateDirectory(out);
}

static void request_redraw(App *app)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = app->ev_wake;
    SDL_PushEvent(&e);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    (void)argc; (void)argv;
    SDL_SetAppMetadata("MyFiles", "0.1.0", "com.example.myfiles");
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "waitevent");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    if (!TTF_Init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "TTF_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    App *app = SDL_calloc(1, sizeof *app);
    *appstate = app;

    const char *override = SDL_getenv("MYFILES_DATA_DIR"); /* development: use a throwaway data dir */
    if (override) {
        SDL_snprintf(app->base_dir, sizeof app->base_dir, "%s/", override);
        SDL_CreateDirectory(app->base_dir);
    } else {
        char *pref = SDL_GetPrefPath("example", "MyFiles");
        if (!pref) return SDL_APP_FAILURE;
        SDL_strlcpy(app->base_dir, pref, sizeof app->base_dir);
        SDL_free(pref);
    }
    if (!make_dir(app->base_dir, "pages", app->pages_dir, sizeof app->pages_dir) ||
        !make_dir(app->base_dir, "tmp", app->tmp_dir, sizeof app->tmp_dir) ||
        !make_dir(app->base_dir, "pdfs", app->pdfs_dir, sizeof app->pdfs_dir)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "storage: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    char db_path[1200];
    SDL_snprintf(db_path, sizeof db_path, "%smyfiles.db", app->base_dir);
    app->db = db_open(db_path);
    if (!app->db) return SDL_APP_FAILURE;

    Uint32 first = SDL_RegisterEvents(4);
    app->ev_sync = first;
    app->ev_scan = first + 1;
    app->ev_label = first + 2;
    app->ev_wake = first + 3;

#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_ANDROID)
    SDL_WindowFlags flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    int w = 0, h = 0;
#else
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    int w = 420, h = 860; /* phone-shaped window for development */
#endif
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    if (!SDL_CreateWindowAndRenderer("MyFiles", w, h, flags, &window, &renderer)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "window: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetRenderVSync(renderer, 1);
    if (!ui_init(&app->ui, window, renderer, platform_font_dir())) return SDL_APP_FAILURE;

    platform_init(window, app->tmp_dir, app->ev_scan, app->ev_label);
    app->sync = sync_create(db_path, app->pages_dir, platform_http, app->ev_sync);
    sync_request(app->sync);
    SDL_AddTimer(AUTOSYNC_INTERVAL_MS, periodic_sync, app);

    app->screen = SCR_HOME;
    app->reload = true;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    App *app = appstate;
    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        sync_request(app->sync);
        app->reload = true;
        break;
    case SDL_EVENT_KEY_DOWN:
        /* Android back button / Escape on desktop */
        if ((event->key.key == SDLK_AC_BACK || event->key.key == SDLK_ESCAPE) && app->screen != SCR_HOME) {
            app_back(app);
            return SDL_APP_CONTINUE;
        }
        break;
    default:
        break;
    }
    if (event->type == app->ev_sync) {
        app->reload = true;
    } else if (event->type == app->ev_scan) {
        ScanResult *r = event->user.data1;
        screens_on_scan(app, r);
        platform_free_scan(r);
    } else if (event->type == app->ev_label) {
        LabelResult *r = event->user.data1;
        screens_on_label(app, r);
        platform_free_label(r);
    }
    ui_event(&app->ui, event);
    return SDL_APP_CONTINUE;
}

/* Development hook: MYFILES_SCREENSHOT=out.png renders a few frames, saves one and quits.
 * MYFILES_QUERY sets the search text; MYFILES_OPEN=folder:<id> | doc:<id> | label:<id> opens a screen. */
static SDL_AppResult dev_screenshot(App *app)
{
    static int frames = 0;
    const char *out = SDL_getenv("MYFILES_SCREENSHOT");
    if (!out) return SDL_APP_CONTINUE;
    if (frames++ == 0) {
        const char *q = SDL_getenv("MYFILES_QUERY");
        const char *open = SDL_getenv("MYFILES_OPEN");
        if (q) SDL_strlcpy(app->query, q, sizeof app->query);
        if (open && SDL_strncmp(open, "folder:", 7) == 0) app_open_folder(app, open + 7);
        if (open && SDL_strncmp(open, "doc:", 4) == 0) app_open_document(app, open + 4);
        if (open && SDL_strncmp(open, "label:", 6) == 0) {
            app_open_folder(app, open + 6);
            app_navigate(app, SCR_LABEL);
        }
        if (open && SDL_strcmp(open, "settings") == 0) app_navigate(app, SCR_SETTINGS);
        if (open && SDL_strcmp(open, "new") == 0) {
            SDL_zero(app->edit_folder);
            db_folder_suggest_next(app->db, app->edit_folder.file_number, sizeof app->edit_folder.file_number);
            app->edit_folder_new = true;
            app_navigate(app, SCR_FOLDER_EDIT);
        }
    }
    if (frames < 4) {
        request_redraw(app);
        return SDL_APP_CONTINUE;
    }
    SDL_Surface *shot = SDL_RenderReadPixels(app->ui.renderer, NULL);
    if (shot) {
        IMG_SavePNG(shot, out);
        SDL_DestroySurface(shot);
    }
    return SDL_APP_SUCCESS;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    App *app = appstate;
    ui_begin(&app->ui);
    screens_draw(app);
    ui_end(&app->ui);
    SDL_AppResult dev = dev_screenshot(app);
    if (dev != SDL_APP_CONTINUE) return dev;
    SDL_RenderPresent(app->ui.renderer);
    /* Navigation happened during this frame: draw the new screen now rather than on the next event. */
    if (app->reload) request_redraw(app);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    (void)result;
    App *app = appstate;
    if (!app) return;
    sync_destroy(app->sync);
    SDL_free(app->ocr_text);
    if (app->ui.renderer) ui_shutdown(&app->ui);
    if (app->db) db_close(app->db);
    SDL_free(app);
    TTF_Quit();
}
