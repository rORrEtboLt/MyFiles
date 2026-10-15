#include "sync.h"

#include <SDL3/SDL.h>
#include <cJSON.h>
#include <sqlite3.h>

#include "db.h"

struct SyncEngine {
    char *db_path;
    char *pages_dir;
    HttpFn http;
    uint32_t done_event;

    SDL_Thread *thread;
    SDL_Mutex *lock;
    SDL_Condition *wake;
    bool quit;
    bool want_sync;
    bool want_auth;
    char auth_url[512], auth_email[256], auth_password[256];
    bool auth_create;

    SyncState state;
    char message[256];
};

/* Everything a run needs; lives on the worker thread. */
typedef struct {
    SyncEngine *s;
    sqlite3 *db;
    char base[512];
    char token[2048];
} Run;

typedef struct {
    const char *name;
    const char *cols;     /* columns sent on push / received on pull, after id/created_at/updated_at/deleted */
    const char *upsert;   /* pull upsert, binds: id, created_at, updated_at, deleted, then cols in order */
} Table;

/* Pull: the server row wins unless this device has an unpushed edit that is newer. */
static const Table TABLES[] = {
    {"folders", "file_number,title,location,notes",
     "INSERT INTO folders(id, created_at, updated_at, deleted, file_number, title, location, notes, dirty)"
     " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 0)"
     " ON CONFLICT(id) DO UPDATE SET created_at = excluded.created_at, updated_at = excluded.updated_at,"
     "  deleted = excluded.deleted, file_number = excluded.file_number, title = excluded.title,"
     "  location = excluded.location, notes = excluded.notes, dirty = 0"
     " WHERE folders.dirty = 0 OR excluded.updated_at >= folders.updated_at"},
    {"documents", "folder_id,title,tags,doc_date,ocr_text",
     "INSERT INTO documents(id, created_at, updated_at, deleted, folder_id, title, tags, doc_date, ocr_text, dirty)"
     " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, 0)"
     " ON CONFLICT(id) DO UPDATE SET created_at = excluded.created_at, updated_at = excluded.updated_at,"
     "  deleted = excluded.deleted, folder_id = excluded.folder_id, title = excluded.title, tags = excluded.tags,"
     "  doc_date = excluded.doc_date, ocr_text = excluded.ocr_text, dirty = 0"
     " WHERE documents.dirty = 0 OR excluded.updated_at >= documents.updated_at"},
    {"pages", "document_id,page_no,ocr_text,width,height",
     /* ?10 = has_image (server-owned, pull only). image_file is local and never overwritten. */
     "INSERT INTO pages(id, created_at, updated_at, deleted, document_id, page_no, ocr_text, width, height, has_image, dirty)"
     " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 0)"
     " ON CONFLICT(id) DO UPDATE SET created_at = excluded.created_at, updated_at = excluded.updated_at,"
     "  deleted = excluded.deleted, document_id = excluded.document_id, page_no = excluded.page_no,"
     "  ocr_text = excluded.ocr_text, width = excluded.width, height = excluded.height,"
     "  has_image = max(pages.has_image, excluded.has_image), dirty = 0"
     " WHERE pages.dirty = 0 OR excluded.updated_at >= pages.updated_at"},
};

static const char *INT_COLS[] = {"page_no", "width", "height"};

static bool is_int_col(const char *c)
{
    for (size_t i = 0; i < SDL_arraysize(INT_COLS); i++)
        if (SDL_strcmp(c, INT_COLS[i]) == 0) return true;
    return false;
}

static void notify(SyncEngine *s)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = s->done_event;
    SDL_PushEvent(&e);
}

/* Every status change wakes the UI so progress is visible. */
static void set_status(SyncEngine *s, SyncState state, const char *fmt, ...)
{
    SDL_LockMutex(s->lock);
    s->state = state;
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(s->message, sizeof s->message, fmt, ap);
    va_end(ap);
    SDL_UnlockMutex(s->lock);
    notify(s);
}

static void meta_get(sqlite3 *db, const char *key, char *out, int cap) { db_meta_get(db, key, out, cap); }

/* Splits "a,b,c" into an array of pointers into buf. */
static int split_cols(const char *cols, char *buf, size_t cap, const char *out[], int max)
{
    SDL_strlcpy(buf, cols, cap);
    int n = 0;
    char *save = NULL;
    for (char *t = SDL_strtok_r(buf, ",", &save); t && n < max; t = SDL_strtok_r(NULL, ",", &save)) out[n++] = t;
    return n;
}

typedef enum { REQ_OK, REQ_OFFLINE, REQ_UNAUTHORIZED, REQ_FAILED } ReqResult;

static ReqResult request(Run *r, const char *method, const char *path, const char *ctype, const void *body, size_t len, HttpResponse *res)
{
    char url[1024];
    SDL_snprintf(url, sizeof url, "%s%s", r->base, path);
    HttpRequest req = {method, url, r->token[0] ? r->token : NULL, ctype, body, len};
    SDL_zerop(res);
    if (!r->s->http(&req, res)) return REQ_OFFLINE;
    if (res->status == 401) return REQ_UNAUTHORIZED;
    if (res->status < 200 || res->status >= 300) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s %s -> %d %s", method, path, res->status, res->body ? res->body : "");
        return REQ_FAILED;
    }
    return REQ_OK;
}

/* ---- push ------------------------------------------------------------------------------------- */

typedef struct { int table; char id[ID_LEN + FILENO_MAX]; int64_t updated_at; } Sent;

static ReqResult push(Run *r, int *pushed)
{
    *pushed = 0;
    for (;;) {
        cJSON *root = cJSON_CreateObject();
        cJSON *changes = cJSON_AddObjectToObject(root, "changes");
        Sent *sent = NULL;
        int nsent = 0, capsent = 0;

        for (int t = 0; t < (int)SDL_arraysize(TABLES); t++) {
            char buf[256], sql[512];
            const char *cols[8];
            int ncols = split_cols(TABLES[t].cols, buf, sizeof buf, cols, 8);
            SDL_snprintf(sql, sizeof sql, "SELECT id, created_at, updated_at, deleted, %s FROM %s WHERE dirty = 1 LIMIT 1000",
                         TABLES[t].cols, TABLES[t].name);
            cJSON *arr = cJSON_AddArrayToObject(changes, TABLES[t].name);
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(r->db, sql, -1, &st, NULL) != SQLITE_OK) continue;
            while (sqlite3_step(st) == SQLITE_ROW) {
                cJSON *row = cJSON_CreateObject();
                const char *id = (const char *)sqlite3_column_text(st, 0);
                int64_t updated = sqlite3_column_int64(st, 2);
                cJSON_AddStringToObject(row, "id", id);
                cJSON_AddNumberToObject(row, "created_at", (double)sqlite3_column_int64(st, 1));
                cJSON_AddNumberToObject(row, "updated_at", (double)updated);
                cJSON_AddBoolToObject(row, "deleted", sqlite3_column_int(st, 3));
                for (int c = 0; c < ncols; c++) {
                    if (is_int_col(cols[c])) cJSON_AddNumberToObject(row, cols[c], sqlite3_column_int(st, 4 + c));
                    else cJSON_AddStringToObject(row, cols[c], (const char *)sqlite3_column_text(st, 4 + c));
                }
                cJSON_AddItemToArray(arr, row);
                if (nsent == capsent) {
                    capsent = capsent ? capsent * 2 : 64;
                    sent = SDL_realloc(sent, sizeof *sent * (size_t)capsent);
                }
                sent[nsent].table = t;
                SDL_strlcpy(sent[nsent].id, id, sizeof sent[nsent].id);
                sent[nsent].updated_at = updated;
                nsent++;
            }
            sqlite3_finalize(st);
        }

        if (nsent == 0) {
            cJSON_Delete(root);
            return REQ_OK;
        }

        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        HttpResponse res;
        ReqResult rr = request(r, "POST", "/api/sync/push", "application/json", body, SDL_strlen(body), &res);
        cJSON_free(body);
        SDL_free(res.body);
        if (rr != REQ_OK) {
            SDL_free(sent);
            return rr;
        }

        /* Clear dirty only if the row was not edited again while the request was in flight. Rows the
         * server rejected as stale are cleared too: the next pull brings the winning version. */
        sqlite3_exec(r->db, "BEGIN", NULL, NULL, NULL);
        for (int i = 0; i < nsent; i++) {
            char sql[128];
            SDL_snprintf(sql, sizeof sql, "UPDATE %s SET dirty = 0 WHERE id = ? AND updated_at = ?", TABLES[sent[i].table].name);
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(r->db, sql, -1, &st, NULL) != SQLITE_OK) continue;
            sqlite3_bind_text(st, 1, sent[i].id, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 2, sent[i].updated_at);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        sqlite3_exec(r->db, "COMMIT", NULL, NULL, NULL);
        *pushed += nsent;
        SDL_free(sent);
    }
}

/* ---- images ----------------------------------------------------------------------------------- */

static ReqResult upload_images(Run *r, int *uploaded)
{
    *uploaded = 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(r->db, "SELECT id, image_file FROM pages WHERE deleted = 0 AND has_image = 0 AND image_file <> '' AND dirty = 0", -1, &st, NULL);
    ReqResult result = REQ_OK;
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        char id[ID_LEN], file[1024];
        SDL_strlcpy(id, (const char *)sqlite3_column_text(st, 0), sizeof id);
        SDL_snprintf(file, sizeof file, "%s%s", r->s->pages_dir, (const char *)sqlite3_column_text(st, 1));
        size_t len = 0;
        void *data = SDL_LoadFile(file, &len);
        if (!data) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "missing page image %s", file);
            continue;
        }
        char path[128];
        SDL_snprintf(path, sizeof path, "/api/pages/%s/image", id);
        HttpResponse res;
        ReqResult rr = request(r, "PUT", path, "image/jpeg", data, len, &res);
        SDL_free(data);
        SDL_free(res.body);
        if (rr == REQ_OFFLINE || rr == REQ_UNAUTHORIZED) {
            result = rr;
            break;
        }
        if (rr == REQ_OK) {
            sqlite3_stmt *up = NULL;
            sqlite3_prepare_v2(r->db, "UPDATE pages SET has_image = 1 WHERE id = ?", -1, &up, NULL);
            sqlite3_bind_text(up, 1, id, -1, SQLITE_STATIC);
            sqlite3_step(up);
            sqlite3_finalize(up);
            (*uploaded)++;
        }
    }
    sqlite3_finalize(st);
    return result;
}

static ReqResult download_images(Run *r, int *downloaded)
{
    *downloaded = 0;
    /* Collect ids first so the statement is not held open across slow network calls. */
    char (*ids)[ID_LEN] = NULL;
    int n = 0, cap = 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(r->db, "SELECT id FROM pages WHERE deleted = 0 AND has_image = 1 AND image_file = ''", -1, &st, NULL);
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            ids = SDL_realloc(ids, sizeof *ids * (size_t)cap);
        }
        SDL_strlcpy(ids[n++], (const char *)sqlite3_column_text(st, 0), ID_LEN);
    }
    sqlite3_finalize(st);

    ReqResult result = REQ_OK;
    for (int i = 0; i < n; i++) {
        SDL_LockMutex(r->s->lock);
        bool quit = r->s->quit;
        SDL_UnlockMutex(r->s->lock);
        if (quit) break;

        char path[128];
        SDL_snprintf(path, sizeof path, "/api/pages/%s/image", ids[i]);
        HttpResponse res;
        ReqResult rr = request(r, "GET", path, NULL, NULL, 0, &res);
        if (rr == REQ_OK) {
            char name[ID_LEN + 8], file[1024];
            SDL_snprintf(name, sizeof name, "%s.jpg", ids[i]);
            SDL_snprintf(file, sizeof file, "%s%s", r->s->pages_dir, name);
            if (SDL_SaveFile(file, res.body, res.len)) {
                sqlite3_stmt *up = NULL;
                sqlite3_prepare_v2(r->db, "UPDATE pages SET image_file = ?2 WHERE id = ?1", -1, &up, NULL);
                sqlite3_bind_text(up, 1, ids[i], -1, SQLITE_STATIC);
                sqlite3_bind_text(up, 2, name, -1, SQLITE_STATIC);
                sqlite3_step(up);
                sqlite3_finalize(up);
                (*downloaded)++;
            }
        }
        SDL_free(res.body);
        if (rr == REQ_OFFLINE || rr == REQ_UNAUTHORIZED) {
            result = rr;
            break;
        }
    }
    SDL_free(ids);
    return result;
}

/* ---- pull ------------------------------------------------------------------------------------- */

static void apply_rows(Run *r, int t, cJSON *rows)
{
    char buf[256];
    const char *cols[8];
    int ncols = split_cols(TABLES[t].cols, buf, sizeof buf, cols, 8);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(r->db, TABLES[t].upsert, -1, &st, NULL) != SQLITE_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "pull upsert %s: %s", TABLES[t].name, sqlite3_errmsg(r->db));
        return;
    }
    bool is_pages = SDL_strcmp(TABLES[t].name, "pages") == 0;
    cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(row, "id"));
        if (!id) continue;
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)cJSON_GetNumberValue(cJSON_GetObjectItem(row, "created_at")));
        sqlite3_bind_int64(st, 3, (sqlite3_int64)cJSON_GetNumberValue(cJSON_GetObjectItem(row, "updated_at")));
        bool deleted = cJSON_IsTrue(cJSON_GetObjectItem(row, "deleted"));
        sqlite3_bind_int(st, 4, deleted);
        for (int c = 0; c < ncols; c++) {
            cJSON *v = cJSON_GetObjectItem(row, cols[c]);
            if (is_int_col(cols[c])) sqlite3_bind_int(st, 5 + c, (int)cJSON_GetNumberValue(v));
            else sqlite3_bind_text(st, 5 + c, cJSON_IsString(v) ? v->valuestring : "", -1, SQLITE_TRANSIENT);
        }
        if (is_pages) sqlite3_bind_int(st, 5 + ncols, cJSON_IsTrue(cJSON_GetObjectItem(row, "has_image")));
        if (sqlite3_step(st) != SQLITE_DONE)
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "pull %s %s: %s", TABLES[t].name, id, sqlite3_errmsg(r->db));

        /* A page deleted elsewhere: free the local image. */
        if (is_pages && deleted) {
            sqlite3_stmt *q = NULL;
            sqlite3_prepare_v2(r->db, "SELECT image_file FROM pages WHERE id = ? AND deleted = 1 AND image_file <> ''", -1, &q, NULL);
            sqlite3_bind_text(q, 1, id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(q) == SQLITE_ROW) {
                char file[1024];
                SDL_snprintf(file, sizeof file, "%s%s", r->s->pages_dir, (const char *)sqlite3_column_text(q, 0));
                SDL_RemovePath(file);
            }
            sqlite3_finalize(q);
            sqlite3_prepare_v2(r->db, "UPDATE pages SET image_file = '' WHERE id = ? AND deleted = 1", -1, &q, NULL);
            sqlite3_bind_text(q, 1, id, -1, SQLITE_TRANSIENT);
            sqlite3_step(q);
            sqlite3_finalize(q);
        }
    }
    sqlite3_finalize(st);
}

static ReqResult pull(Run *r, int *pulled)
{
    *pulled = 0;
    for (;;) {
        char cursor[32] = "0", path[128];
        meta_get(r->db, "cursor", cursor, sizeof cursor);
        if (!cursor[0]) SDL_strlcpy(cursor, "0", sizeof cursor);
        SDL_snprintf(path, sizeof path, "/api/sync/pull?cursor=%s", cursor);
        HttpResponse res;
        ReqResult rr = request(r, "GET", path, NULL, NULL, 0, &res);
        if (rr != REQ_OK) {
            SDL_free(res.body);
            return rr;
        }
        cJSON *root = cJSON_ParseWithLength(res.body, res.len);
        SDL_free(res.body);
        if (!root) return REQ_FAILED;

        cJSON *changes = cJSON_GetObjectItem(root, "changes");
        sqlite3_exec(r->db, "BEGIN", NULL, NULL, NULL);
        /* Folders first so the documents' search rows pick up their folder's file number. */
        for (int t = 0; t < (int)SDL_arraysize(TABLES); t++) {
            cJSON *rows = cJSON_GetObjectItem(changes, TABLES[t].name);
            *pulled += cJSON_GetArraySize(rows);
            apply_rows(r, t, rows);
        }
        char next[32];
        SDL_snprintf(next, sizeof next, "%.0f", cJSON_GetNumberValue(cJSON_GetObjectItem(root, "cursor")));
        db_meta_set(r->db, "cursor", next);
        sqlite3_exec(r->db, "COMMIT", NULL, NULL, NULL);

        bool more = cJSON_IsTrue(cJSON_GetObjectItem(root, "hasMore"));
        cJSON_Delete(root);
        if (!more) return REQ_OK;
    }
}

/* ---- run -------------------------------------------------------------------------------------- */

static int pending_count(sqlite3 *db)
{
    SyncCounts c;
    db_sync_counts(db, &c);
    return c.pending_rows + c.pending_images;
}

static SyncState finish(Run *r, ReqResult rr, const char *what)
{
    int pending = pending_count(r->db);
    switch (rr) {
    case REQ_OFFLINE:
        set_status(r->s, SYNC_OFFLINE, pending ? "Offline - %d change(s) saved on this phone" : "Offline - everything is saved on this phone", pending);
        return SYNC_OFFLINE;
    case REQ_UNAUTHORIZED:
        set_status(r->s, SYNC_AUTH_FAILED, "Session expired - sign in again");
        return SYNC_AUTH_FAILED;
    default:
        set_status(r->s, SYNC_ERROR, "Sync failed while %s; will retry", what);
        return SYNC_ERROR;
    }
}

static SyncState run_sync(SyncEngine *s)
{
    Run r = {.s = s};
    if (sqlite3_open(s->db_path, &r.db) != SQLITE_OK) {
        set_status(s, SYNC_ERROR, "Cannot open local database");
        sqlite3_close(r.db);
        return SYNC_ERROR;
    }
    sqlite3_busy_timeout(r.db, 10000);
    meta_get(r.db, "server_url", r.base, sizeof r.base);
    meta_get(r.db, "token", r.token, sizeof r.token);
    SyncState out;
    if (!r.base[0] || !r.token[0]) {
        set_status(s, SYNC_SIGNED_OUT, "Not signed in - documents are kept on this phone only");
        out = SYNC_SIGNED_OUT;
        goto done;
    }

    set_status(s, SYNC_RUNNING, "Sending changes...");
    int pushed = 0, uploaded = 0, pulled = 0, downloaded = 0;
    ReqResult rr = push(&r, &pushed);
    if (rr != REQ_OK) { out = finish(&r, rr, "sending changes"); goto done; }

    set_status(s, SYNC_RUNNING, "Uploading scans...");
    rr = upload_images(&r, &uploaded);
    if (rr != REQ_OK) { out = finish(&r, rr, "uploading scans"); goto done; }

    set_status(s, SYNC_RUNNING, "Receiving changes...");
    rr = pull(&r, &pulled);
    if (rr != REQ_OK) { out = finish(&r, rr, "receiving changes"); goto done; }

    set_status(s, SYNC_RUNNING, "Downloading scans...");
    rr = download_images(&r, &downloaded);
    if (rr != REQ_OK) { out = finish(&r, rr, "downloading scans"); goto done; }

    char when[16];
    SDL_DateTime dt;
    SDL_Time t;
    SDL_GetCurrentTime(&t);
    SDL_TimeToDateTime(t, &dt, true);
    SDL_snprintf(when, sizeof when, "%02d:%02d", dt.hour, dt.minute);
    set_status(s, SYNC_OK, "Synced at %s (sent %d, received %d)", when, pushed + uploaded, pulled + downloaded);
    out = SYNC_OK;

done:
    sqlite3_close(r.db);
    return out;
}

static SyncState run_auth(SyncEngine *s, const char *url, const char *email, const char *password, bool create)
{
    set_status(s, SYNC_RUNNING, create ? "Creating account..." : "Signing in...");
    char base[512];
    SDL_strlcpy(base, url, sizeof base);
    size_t bl = SDL_strlen(base);
    while (bl > 0 && base[bl - 1] == '/') base[--bl] = '\0';

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "email", email);
    cJSON_AddStringToObject(body, "password", password);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    Run r = {.s = s};
    SDL_strlcpy(r.base, base, sizeof r.base);
    HttpResponse res;
    ReqResult rr = request(&r, "POST", create ? "/api/auth/register" : "/api/auth/login", "application/json", json, SDL_strlen(json), &res);
    cJSON_free(json);

    SyncState out = SYNC_ERROR;
    if (rr == REQ_OFFLINE) {
        set_status(s, SYNC_OFFLINE, "Cannot reach %s", base);
        out = SYNC_OFFLINE;
    } else if (rr == REQ_UNAUTHORIZED) {
        set_status(s, SYNC_AUTH_FAILED, "Wrong email or password");
        out = SYNC_AUTH_FAILED;
    } else if (rr != REQ_OK) {
        set_status(s, SYNC_AUTH_FAILED, res.status == 409 ? "That email already has an account - sign in instead"
                                                          : "Sign-in failed (HTTP %d)", res.status);
        out = SYNC_AUTH_FAILED;
    } else {
        cJSON *root = cJSON_ParseWithLength(res.body, res.len);
        const char *token = cJSON_GetStringValue(cJSON_GetObjectItem(root, "token"));
        if (token && sqlite3_open(s->db_path, &r.db) == SQLITE_OK) {
            sqlite3_busy_timeout(r.db, 10000);
            char prev_url[512] = "", prev_email[256] = "";
            meta_get(r.db, "server_url", prev_url, sizeof prev_url);
            meta_get(r.db, "email", prev_email, sizeof prev_email);
            /* Different server or account: start the pull from scratch. */
            if (SDL_strcmp(prev_url, base) != 0 || SDL_strcasecmp(prev_email, email) != 0) db_meta_set(r.db, "cursor", "0");
            db_meta_set(r.db, "server_url", base);
            db_meta_set(r.db, "email", email);
            db_meta_set(r.db, "token", token);
            out = SYNC_IDLE;
        } else {
            set_status(s, SYNC_ERROR, "Unexpected sign-in response");
        }
        sqlite3_close(r.db);
        cJSON_Delete(root);
    }
    SDL_free(res.body);
    return out;
}

static int SDLCALL worker(void *data)
{
    SyncEngine *s = data;
    SDL_LockMutex(s->lock);
    for (;;) {
        while (!s->quit && !s->want_sync && !s->want_auth) SDL_WaitCondition(s->wake, s->lock);
        if (s->quit) break;

        if (s->want_auth) {
            char url[512], email[256], password[256];
            SDL_strlcpy(url, s->auth_url, sizeof url);
            SDL_strlcpy(email, s->auth_email, sizeof email);
            SDL_strlcpy(password, s->auth_password, sizeof password);
            SDL_memset(s->auth_password, 0, sizeof s->auth_password);
            bool create = s->auth_create;
            s->want_auth = false;
            SDL_UnlockMutex(s->lock);
            SyncState st = run_auth(s, url, email, password, create);
            SDL_memset(password, 0, sizeof password);
            SDL_LockMutex(s->lock);
            if (st == SYNC_IDLE) s->want_sync = true;
            continue;
        }

        s->want_sync = false;
        SDL_UnlockMutex(s->lock);
        run_sync(s);
        SDL_LockMutex(s->lock);
    }
    SDL_UnlockMutex(s->lock);
    return 0;
}

SyncEngine *sync_create(const char *db_path, const char *pages_dir, HttpFn http, uint32_t done_event)
{
    SyncEngine *s = SDL_calloc(1, sizeof *s);
    s->db_path = SDL_strdup(db_path);
    s->pages_dir = SDL_strdup(pages_dir);
    s->http = http;
    s->done_event = done_event;
    s->lock = SDL_CreateMutex();
    s->wake = SDL_CreateCondition();
    s->state = SYNC_IDLE;
    s->thread = SDL_CreateThread(worker, "sync", s);
    return s;
}

void sync_destroy(SyncEngine *s)
{
    if (!s) return;
    SDL_LockMutex(s->lock);
    s->quit = true;
    SDL_SignalCondition(s->wake);
    SDL_UnlockMutex(s->lock);
    SDL_WaitThread(s->thread, NULL);
    SDL_DestroyCondition(s->wake);
    SDL_DestroyMutex(s->lock);
    SDL_free(s->db_path);
    SDL_free(s->pages_dir);
    SDL_free(s);
}

void sync_request(SyncEngine *s)
{
    SDL_LockMutex(s->lock);
    s->want_sync = true;
    /* Report "running" from the moment work is queued, so nobody reads the previous run's result. */
    if (s->state != SYNC_RUNNING) {
        s->state = SYNC_RUNNING;
        SDL_strlcpy(s->message, "Syncing...", sizeof s->message);
    }
    SDL_SignalCondition(s->wake);
    SDL_UnlockMutex(s->lock);
}

void sync_sign_in(SyncEngine *s, const char *server_url, const char *email, const char *password, bool create_account)
{
    SDL_LockMutex(s->lock);
    SDL_strlcpy(s->auth_url, server_url, sizeof s->auth_url);
    SDL_strlcpy(s->auth_email, email, sizeof s->auth_email);
    SDL_strlcpy(s->auth_password, password, sizeof s->auth_password);
    s->auth_create = create_account;
    s->want_auth = true;
    s->state = SYNC_RUNNING;
    SDL_strlcpy(s->message, create_account ? "Creating account..." : "Signing in...", sizeof s->message);
    SDL_SignalCondition(s->wake);
    SDL_UnlockMutex(s->lock);
}

SyncState sync_status(SyncEngine *s, char *message, int cap)
{
    SDL_LockMutex(s->lock);
    SyncState st = s->state;
    if (message) SDL_strlcpy(message, s->message, (size_t)cap);
    SDL_UnlockMutex(s->lock);
    return st;
}

SyncState sync_run_now(SyncEngine *s) { return run_sync(s); }
