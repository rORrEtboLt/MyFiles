#ifndef MYFILES_APP_H
#define MYFILES_APP_H

#include <SDL3/SDL.h>

#include "db.h"
#include "platform/platform.h"
#include "sync.h"
#include "ui.h"

typedef enum {
    SCR_HOME,
    SCR_FOLDER,
    SCR_FOLDER_EDIT,
    SCR_DOCUMENT,
    SCR_DOCUMENT_EDIT,
    SCR_PAGE,
    SCR_LABEL,
    SCR_LABEL_MATCH,
    SCR_SETTINGS,
    SCR_COUNT
} ScreenId;

#define MAX_LIST 300
#define MAX_PAGES 200
#define MAX_CANDIDATES 8

typedef struct {
    Ui ui;
    Db *db;
    SyncEngine *sync;
    char base_dir[1024];
    char pages_dir[1024];
    char tmp_dir[1024];
    char pdfs_dir[1024]; /* one PDF per document: <doc id>.pdf, rebuilt from the page JPEGs */

    Uint32 ev_sync, ev_scan, ev_label, ev_wake;
    SDL_TimerID autosync_timer;

    ScreenId screen;
    ScreenId stack[16];
    int depth;
    float scroll[SCR_COUNT];
    bool reload; /* re-query the current screen's data before drawing */

    /* home */
    char query[160];
    char last_query[160];
    SearchHit hits[60];
    int nhits;
    Folder folder_hits[30];
    int nfolder_hits;
    Folder folders[MAX_LIST];
    int nfolders;

    /* folder */
    Folder folder;
    Document docs[MAX_LIST];
    int ndocs;

    /* document */
    Document doc;
    Page pages[MAX_PAGES];
    int npages;
    char *ocr_text;
    bool show_text;
    int page_index;

    /* forms */
    Folder edit_folder;
    bool edit_folder_new;
    Document edit_doc;
    char edit_doc_file[FILENO_MAX + 1];
    bool confirm_delete;

    /* scans: where new pages go */
    bool scanning;
    bool scan_into_current_doc;

    /* label scan matching */
    char candidates[MAX_CANDIDATES][FILENO_MAX + 1];
    int ncandidates;
    char label_raw[512];

    /* settings form */
    char server_url[256];
    char email[160];
    char password[160];

    char toast[200];
    Uint64 toast_until;
} App;

void app_navigate(App *app, ScreenId to);
void app_back(App *app);
void app_toast(App *app, const char *fmt, ...);
/* Local data changed: schedule a sync a few seconds from now. */
void app_changed(App *app);

void app_open_folder(App *app, const char *id);
void app_open_document(App *app, const char *id);

void screens_draw(App *app);
void screens_on_scan(App *app, ScanResult *r);
void screens_on_label(App *app, LabelResult *r);

#endif
