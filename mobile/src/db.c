#include "db.h"

#include <SDL3/SDL.h>
#include <sqlite3.h>

static const char *SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA foreign_keys=OFF;"
    "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    /* seq is a stable INTEGER PRIMARY KEY so it can be the FTS rowid (a TEXT key's rowid can change on VACUUM). */
    "CREATE TABLE IF NOT EXISTS folders ("
    "  seq INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, file_number TEXT NOT NULL,"
    "  title TEXT NOT NULL DEFAULT '', location TEXT NOT NULL DEFAULT '', notes TEXT NOT NULL DEFAULT '',"
    "  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,"
    "  deleted INTEGER NOT NULL DEFAULT 0, dirty INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS documents ("
    "  seq INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, folder_id TEXT NOT NULL,"
    "  title TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '', doc_date TEXT NOT NULL DEFAULT '',"
    "  ocr_text TEXT NOT NULL DEFAULT '',"
    "  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,"
    "  deleted INTEGER NOT NULL DEFAULT 0, dirty INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS documents_folder ON documents(folder_id);"
    "CREATE TABLE IF NOT EXISTS pages ("
    "  seq INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, document_id TEXT NOT NULL,"
    "  page_no INTEGER NOT NULL DEFAULT 1, ocr_text TEXT NOT NULL DEFAULT '',"
    "  width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0,"
    "  image_file TEXT NOT NULL DEFAULT '',"      /* local copy, relative to the pages dir */
    "  has_image INTEGER NOT NULL DEFAULT 0,"     /* server has the image */
    "  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,"
    "  deleted INTEGER NOT NULL DEFAULT 0, dirty INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS pages_document ON pages(document_id);"
    "CREATE VIRTUAL TABLE IF NOT EXISTS search_fts USING fts5("
    "  title, tags, ocr_text, fileno, folder_title, tokenize='unicode61 remove_diacritics 2');"
    /* Keep search_fts in step with documents and their folder. fileno holds both the display form and
     * the normalized id so "F-0042", "F 42" and "f42"-style queries all hit. */
    "CREATE TRIGGER IF NOT EXISTS documents_ai AFTER INSERT ON documents WHEN new.deleted = 0 BEGIN"
    "  INSERT INTO search_fts(rowid, title, tags, ocr_text, fileno, folder_title)"
    "  SELECT new.seq, new.title, new.tags, new.ocr_text,"
    "         coalesce(f.file_number || ' ' || f.id, new.folder_id), coalesce(f.title, '')"
    "  FROM (SELECT 1) LEFT JOIN folders f ON f.id = new.folder_id;"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS documents_au AFTER UPDATE ON documents BEGIN"
    "  DELETE FROM search_fts WHERE rowid = old.seq;"
    "  INSERT INTO search_fts(rowid, title, tags, ocr_text, fileno, folder_title)"
    "  SELECT new.seq, new.title, new.tags, new.ocr_text,"
    "         coalesce(f.file_number || ' ' || f.id, new.folder_id), coalesce(f.title, '')"
    "  FROM (SELECT 1) LEFT JOIN folders f ON f.id = new.folder_id WHERE new.deleted = 0;"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS documents_ad AFTER DELETE ON documents BEGIN"
    "  DELETE FROM search_fts WHERE rowid = old.seq;"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS folders_ai AFTER INSERT ON folders BEGIN"
    "  DELETE FROM search_fts WHERE rowid IN (SELECT seq FROM documents WHERE folder_id = new.id);"
    "  INSERT INTO search_fts(rowid, title, tags, ocr_text, fileno, folder_title)"
    "  SELECT d.seq, d.title, d.tags, d.ocr_text, new.file_number || ' ' || new.id, new.title"
    "  FROM documents d WHERE d.folder_id = new.id AND d.deleted = 0 AND new.deleted = 0;"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS folders_au AFTER UPDATE OF file_number, title, deleted ON folders BEGIN"
    "  DELETE FROM search_fts WHERE rowid IN (SELECT seq FROM documents WHERE folder_id = new.id);"
    "  INSERT INTO search_fts(rowid, title, tags, ocr_text, fileno, folder_title)"
    "  SELECT d.seq, d.title, d.tags, d.ocr_text, new.file_number || ' ' || new.id, new.title"
    "  FROM documents d WHERE d.folder_id = new.id AND d.deleted = 0 AND new.deleted = 0;"
    "END;";

int64_t now_ms(void)
{
    SDL_Time t = 0;
    SDL_GetCurrentTime(&t);
    return (int64_t)(t / 1000000);
}

void new_id(char out[ID_LEN])
{
    /* 48 bits of time + 80 random bits, hex: sortable-ish, collision-safe across devices. */
    static Uint64 state = 0;
    if (state == 0) state = SDL_GetPerformanceCounter() ^ (Uint64)SDL_GetTicksNS() ^ ((Uint64)(uintptr_t)&state << 16);
    Uint64 ms = (Uint64)now_ms();
    Uint32 a = SDL_rand_bits_r(&state), b = SDL_rand_bits_r(&state), c = SDL_rand_bits_r(&state);
    SDL_snprintf(out, ID_LEN, "%012llx%08x%08x%04x", (unsigned long long)(ms & 0xffffffffffffULL), a, b, c & 0xffff);
}

static void copy_col(sqlite3_stmt *st, int col, char *out, size_t cap)
{
    const unsigned char *s = sqlite3_column_text(st, col);
    SDL_strlcpy(out, s ? (const char *)s : "", cap);
}

static sqlite3_stmt *prep(Db *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "sqlite prepare: %s\n  in: %s", sqlite3_errmsg(db), sql);
        return NULL;
    }
    return st;
}

static bool exec_done(Db *db, sqlite3_stmt *st)
{
    if (!st) return false;
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "sqlite step: %s", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return rc == SQLITE_DONE || rc == SQLITE_ROW;
}

Db *db_open(const char *path)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "open %s: %s", path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    char *err = NULL;
    if (sqlite3_exec(db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "schema: %s", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

void db_close(Db *db) { sqlite3_close(db); }

bool db_meta_get(Db *db, const char *key, char *out, int cap)
{
    sqlite3_stmt *st = prep(db, "SELECT value FROM meta WHERE key = ?");
    if (!st) return false;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) copy_col(st, 0, out, (size_t)cap);
    else if (cap > 0) out[0] = '\0';
    sqlite3_finalize(st);
    return found;
}

bool db_meta_set(Db *db, const char *key, const char *value)
{
    sqlite3_stmt *st = prep(db, "INSERT INTO meta(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!st) return false;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
    return exec_done(db, st);
}

/* ---- folders ---------------------------------------------------------------------------------- */

#define FOLDER_COLS "f.id, f.file_number, f.title, f.location, f.notes, f.updated_at," \
    " (SELECT count(*) FROM documents d WHERE d.folder_id = f.id AND d.deleted = 0)," \
    " (SELECT group_concat(d.tags, ',') FROM documents d WHERE d.folder_id = f.id AND d.deleted = 0 AND d.tags <> '')"

void tags_distinct(const char *in, char *out, size_t cap)
{
    if (cap == 0) return;
    out[0] = '\0';
    size_t n = 0;
    const char *p = in ? in : "";
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        size_t len = (size_t)(end - start);
        if (len == 0) continue;
        /* already present? compare against each tag in out */
        bool dup = false;
        const char *q = out;
        while (*q && !dup) {
            const char *qe = SDL_strstr(q, ", ");
            size_t ql = qe ? (size_t)(qe - q) : SDL_strlen(q);
            dup = ql == len && SDL_strncasecmp(q, start, len) == 0;
            q = qe ? qe + 2 : q + ql;
        }
        if (dup) continue;
        size_t need = len + (n ? 2 : 0);
        if (n + need >= cap) break;
        if (n) { out[n++] = ','; out[n++] = ' '; }
        SDL_memcpy(out + n, start, len);
        n += len;
        out[n] = '\0';
    }
}

static void read_folder(sqlite3_stmt *st, Folder *f)
{
    copy_col(st, 0, f->id, sizeof f->id);
    copy_col(st, 1, f->file_number, sizeof f->file_number);
    copy_col(st, 2, f->title, sizeof f->title);
    copy_col(st, 3, f->location, sizeof f->location);
    copy_col(st, 4, f->notes, sizeof f->notes);
    f->updated_at = sqlite3_column_int64(st, 5);
    f->doc_count = sqlite3_column_int(st, 6);
    const unsigned char *tags = sqlite3_column_text(st, 7);
    tags_distinct(tags ? (const char *)tags : "", f->tags, sizeof f->tags);
}

bool db_folder_save(Db *db, Folder *f)
{
    char id[FILENO_MAX + 1];
    if (fileno_normalize(f->file_number, id, sizeof id) == 0) return false;
    int64_t now = now_ms();

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    sqlite3_stmt *st = prep(db,
        "INSERT INTO folders(id, file_number, title, location, notes, created_at, updated_at, deleted, dirty)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?6, 0, 1)"
        " ON CONFLICT(id) DO UPDATE SET file_number = excluded.file_number, title = excluded.title,"
        "  location = excluded.location, notes = excluded.notes, updated_at = excluded.updated_at, deleted = 0, dirty = 1");
    bool ok = st != NULL;
    if (ok) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, f->file_number, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, f->title, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, f->location, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 5, f->notes, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 6, now);
        ok = exec_done(db, st);
    }
    /* Renumbering an envelope: move its documents to the new id and tombstone the old record. */
    if (ok && f->id[0] && SDL_strcmp(f->id, id) != 0) {
        st = prep(db, "UPDATE documents SET folder_id = ?1, updated_at = ?3, dirty = 1 WHERE folder_id = ?2");
        if ((ok = st != NULL)) {
            sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, f->id, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 3, now);
            ok = exec_done(db, st);
        }
        st = prep(db, "UPDATE folders SET deleted = 1, updated_at = ?2, dirty = 1 WHERE id = ?1");
        if (ok && (ok = st != NULL)) {
            sqlite3_bind_text(st, 1, f->id, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 2, now);
            ok = exec_done(db, st);
        }
    }
    sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    if (ok) {
        SDL_strlcpy(f->id, id, sizeof f->id);
        f->updated_at = now;
    }
    return ok;
}

bool db_folder_get(Db *db, const char *id, Folder *out)
{
    sqlite3_stmt *st = prep(db, "SELECT " FOLDER_COLS " FROM folders f WHERE f.id = ? AND f.deleted = 0");
    if (!st) return false;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) read_folder(st, out);
    sqlite3_finalize(st);
    return found;
}

int db_folder_list(Db *db, Folder *out, int max)
{
    sqlite3_stmt *st = prep(db, "SELECT " FOLDER_COLS " FROM folders f WHERE f.deleted = 0 ORDER BY f.updated_at DESC LIMIT ?");
    if (!st) return 0;
    sqlite3_bind_int(st, 1, max);
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) read_folder(st, &out[n++]);
    sqlite3_finalize(st);
    return n;
}

bool db_folder_delete(Db *db, const char *id)
{
    int64_t now = now_ms();
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    const char *sqls[] = {
        ("UPDATE pages SET deleted = 1, updated_at = ?2, dirty = 1"
         " WHERE deleted = 0 AND document_id IN (SELECT id FROM documents WHERE folder_id = ?1)"),
        "UPDATE documents SET deleted = 1, updated_at = ?2, dirty = 1 WHERE deleted = 0 AND folder_id = ?1",
        "UPDATE folders SET deleted = 1, updated_at = ?2, dirty = 1 WHERE id = ?1",
    };
    bool ok = true;
    for (size_t i = 0; ok && i < SDL_arraysize(sqls); i++) {
        sqlite3_stmt *st = prep(db, sqls[i]);
        if (!(ok = st != NULL)) break;
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, now);
        ok = exec_done(db, st);
    }
    sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    return ok;
}

void db_folder_suggest_next(Db *db, char *out, int cap)
{
    SDL_strlcpy(out, "F-0001", (size_t)cap);
    sqlite3_stmt *st = prep(db, "SELECT file_number FROM folders ORDER BY created_at DESC, seq DESC LIMIT 1");
    if (!st) return;
    if (sqlite3_step(st) == SQLITE_ROW) {
        char last[FILENO_MAX + 1];
        copy_col(st, 0, last, sizeof last);
        /* Increment the last run of digits, keeping its zero padding: F-0042 -> F-0043, B-99 -> B-100. */
        int end = (int)SDL_strlen(last) - 1;
        while (end >= 0 && !SDL_isdigit((unsigned char)last[end])) end--;
        if (end >= 0) {
            int start = end;
            while (start > 0 && SDL_isdigit((unsigned char)last[start - 1])) start--;
            char digits[32] = {0};
            int width = end - start + 1;
            if (width < (int)sizeof digits) {
                SDL_memcpy(digits, last + start, (size_t)width);
                unsigned long long n = SDL_strtoull(digits, NULL, 10) + 1;
                char num[32];
                SDL_snprintf(num, sizeof num, "%0*llu", width, n);
                SDL_snprintf(out, (size_t)cap, "%.*s%s%s", start, last, num, last + end + 1);
            }
        }
    }
    sqlite3_finalize(st);
}

/* ---- documents -------------------------------------------------------------------------------- */

#define DOC_COLS "d.id, d.folder_id, d.title, d.tags, d.doc_date, d.updated_at," \
    " (SELECT count(*) FROM pages p WHERE p.document_id = d.id AND p.deleted = 0)"

static void read_doc(sqlite3_stmt *st, Document *d)
{
    copy_col(st, 0, d->id, sizeof d->id);
    copy_col(st, 1, d->folder_id, sizeof d->folder_id);
    copy_col(st, 2, d->title, sizeof d->title);
    copy_col(st, 3, d->tags, sizeof d->tags);
    copy_col(st, 4, d->doc_date, sizeof d->doc_date);
    d->updated_at = sqlite3_column_int64(st, 5);
    d->page_count = sqlite3_column_int(st, 6);
}

bool db_document_save(Db *db, Document *d)
{
    if (!d->id[0]) new_id(d->id);
    int64_t now = now_ms();
    sqlite3_stmt *st = prep(db,
        "INSERT INTO documents(id, folder_id, title, tags, doc_date, created_at, updated_at, deleted, dirty)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?6, 0, 1)"
        " ON CONFLICT(id) DO UPDATE SET folder_id = excluded.folder_id, title = excluded.title, tags = excluded.tags,"
        "  doc_date = excluded.doc_date, updated_at = excluded.updated_at, dirty = 1");
    if (!st) return false;
    sqlite3_bind_text(st, 1, d->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, d->folder_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, d->title, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, d->tags, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 5, d->doc_date, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, now);
    bool ok = exec_done(db, st);
    if (ok) d->updated_at = now;
    return ok;
}

bool db_document_get(Db *db, const char *id, Document *out)
{
    sqlite3_stmt *st = prep(db, "SELECT " DOC_COLS " FROM documents d WHERE d.id = ? AND d.deleted = 0");
    if (!st) return false;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) read_doc(st, out);
    sqlite3_finalize(st);
    return found;
}

int db_document_list(Db *db, const char *folder_id, Document *out, int max)
{
    sqlite3_stmt *st = prep(db, "SELECT " DOC_COLS " FROM documents d WHERE d.folder_id = ? AND d.deleted = 0"
                                " ORDER BY d.updated_at DESC LIMIT ?");
    if (!st) return 0;
    sqlite3_bind_text(st, 1, folder_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, max);
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) read_doc(st, &out[n++]);
    sqlite3_finalize(st);
    return n;
}

bool db_document_delete(Db *db, const char *id)
{
    int64_t now = now_ms();
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    const char *sqls[] = {
        "UPDATE pages SET deleted = 1, updated_at = ?2, dirty = 1 WHERE deleted = 0 AND document_id = ?1",
        "UPDATE documents SET deleted = 1, updated_at = ?2, dirty = 1 WHERE id = ?1",
    };
    bool ok = true;
    for (size_t i = 0; ok && i < SDL_arraysize(sqls); i++) {
        sqlite3_stmt *st = prep(db, sqls[i]);
        if (!(ok = st != NULL)) break;
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, now);
        ok = exec_done(db, st);
    }
    sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    return ok;
}

char *db_document_ocr_text(Db *db, const char *id)
{
    sqlite3_stmt *st = prep(db, "SELECT ocr_text FROM documents WHERE id = ?");
    if (!st) return NULL;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    char *text = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);
        text = SDL_strdup(s ? (const char *)s : "");
    }
    sqlite3_finalize(st);
    return text;
}

bool db_document_move(Db *db, const char *id, const char *folder_id)
{
    sqlite3_stmt *st = prep(db, "UPDATE documents SET folder_id = ?2, updated_at = ?3, dirty = 1 WHERE id = ?1");
    if (!st) return false;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, folder_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, now_ms());
    return exec_done(db, st);
}

/* ---- pages ------------------------------------------------------------------------------------ */

bool db_page_add(Db *db, const char *document_id, const char *page_id, const char *image_file, int w, int h, const char *ocr_text)
{
    int64_t now = now_ms();
    if (!ocr_text) ocr_text = "";
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    sqlite3_stmt *st = prep(db,
        "INSERT INTO pages(id, document_id, page_no, ocr_text, width, height, image_file, created_at, updated_at, dirty)"
        " VALUES(?1, ?2, (SELECT coalesce(max(page_no), 0) + 1 FROM pages WHERE document_id = ?2 AND deleted = 0),"
        "        ?3, ?4, ?5, ?6, ?7, ?7, 1)");
    bool ok = st != NULL;
    if (ok) {
        sqlite3_bind_text(st, 1, page_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, document_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, ocr_text, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 4, w);
        sqlite3_bind_int(st, 5, h);
        sqlite3_bind_text(st, 6, image_file, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 7, now);
        ok = exec_done(db, st);
    }
    if (ok && ocr_text[0]) {
        st = prep(db, "UPDATE documents SET ocr_text = CASE WHEN ocr_text = '' THEN ?2 ELSE ocr_text || char(10) || ?2 END,"
                      " updated_at = ?3, dirty = 1 WHERE id = ?1");
        if ((ok = st != NULL)) {
            sqlite3_bind_text(st, 1, document_id, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, ocr_text, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 3, now);
            ok = exec_done(db, st);
        }
    }
    sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    return ok;
}

int db_page_list(Db *db, const char *document_id, Page *out, int max)
{
    sqlite3_stmt *st = prep(db, "SELECT id, document_id, page_no, width, height, image_file, has_image FROM pages"
                                " WHERE document_id = ? AND deleted = 0 ORDER BY page_no LIMIT ?");
    if (!st) return 0;
    sqlite3_bind_text(st, 1, document_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, max);
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        Page *p = &out[n++];
        copy_col(st, 0, p->id, sizeof p->id);
        copy_col(st, 1, p->document_id, sizeof p->document_id);
        p->page_no = sqlite3_column_int(st, 2);
        p->width = sqlite3_column_int(st, 3);
        p->height = sqlite3_column_int(st, 4);
        copy_col(st, 5, p->image_file, sizeof p->image_file);
        p->has_remote_image = sqlite3_column_int(st, 6) != 0;
    }
    sqlite3_finalize(st);
    return n;
}

/* ---- search ----------------------------------------------------------------------------------- */

/* Turns free text into an FTS5 query: every word becomes a quoted prefix term, all ANDed. Quoting
 * neutralises FTS operators typed by the user. Non-ASCII bytes stay inside words (UTF-8 text). */
static bool build_fts_query(const char *q, char *out, size_t cap)
{
    size_t n = 0;
    bool any = false;
    const char *p = q;
    while (*p) {
        while (*p && (unsigned char)*p < 0x80 && !SDL_isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && ((unsigned char)*p >= 0x80 || SDL_isalnum((unsigned char)*p))) p++;
        size_t len = (size_t)(p - start);
        if (n + len + 6 >= cap) break;
        if (any) out[n++] = ' ';
        out[n++] = '"';
        SDL_memcpy(out + n, start, len);
        n += len;
        out[n++] = '"';
        out[n++] = '*';
        any = true;
    }
    out[n] = '\0';
    return any;
}

int db_search(Db *db, const char *query, SearchHit *out, int max)
{
    char fts[512];
    if (!build_fts_query(query, fts, sizeof fts)) return 0;
    sqlite3_stmt *st = prep(db,
        "SELECT d.id, d.title, d.folder_id, coalesce(f.file_number, d.folder_id), coalesce(f.title, ''),"
        "       replace(replace(snippet(search_fts, -1, '[', ']', '...', 10), char(13), ''), char(10), '  '), d.tags"
        " FROM search_fts JOIN documents d ON d.seq = search_fts.rowid"
        " LEFT JOIN folders f ON f.id = d.folder_id"
        " WHERE search_fts MATCH ? AND d.deleted = 0"
        " ORDER BY bm25(search_fts, 10.0, 5.0, 1.0, 8.0, 3.0) LIMIT ?");
    if (!st) return 0;
    sqlite3_bind_text(st, 1, fts, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, max);
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        SearchHit *h = &out[n++];
        copy_col(st, 0, h->doc_id, sizeof h->doc_id);
        copy_col(st, 1, h->doc_title, sizeof h->doc_title);
        copy_col(st, 2, h->folder_id, sizeof h->folder_id);
        copy_col(st, 3, h->file_number, sizeof h->file_number);
        copy_col(st, 4, h->folder_title, sizeof h->folder_title);
        copy_col(st, 5, h->snippet, sizeof h->snippet);
        const unsigned char *tags = sqlite3_column_text(st, 6);
        tags_distinct(tags ? (const char *)tags : "", h->tags, sizeof h->tags);
    }
    sqlite3_finalize(st);
    return n;
}

int db_search_folders(Db *db, const char *query, Folder *out, int max)
{
    char id[FILENO_MAX + 1];
    fileno_normalize(query, id, sizeof id);
    char like[300];
    SDL_snprintf(like, sizeof like, "%%%s%%", query);
    sqlite3_stmt *st = prep(db,
        "SELECT " FOLDER_COLS " FROM folders f WHERE f.deleted = 0 AND"
        " (f.id = ?1 OR (?1 <> '' AND f.id LIKE ?1 || '%') OR f.title LIKE ?2 OR f.location LIKE ?2)"
        " ORDER BY (f.id = ?1) DESC, f.updated_at DESC LIMIT ?3");
    if (!st) return 0;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, like, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, max);
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) read_folder(st, &out[n++]);
    sqlite3_finalize(st);
    return n;
}

void db_sync_counts(Db *db, SyncCounts *out)
{
    SDL_zerop(out);
    sqlite3_stmt *st = prep(db,
        "SELECT (SELECT count(*) FROM folders WHERE dirty = 1) + (SELECT count(*) FROM documents WHERE dirty = 1)"
        "     + (SELECT count(*) FROM pages WHERE dirty = 1),"
        " (SELECT count(*) FROM pages WHERE deleted = 0 AND has_image = 0 AND image_file <> ''),"
        " (SELECT count(*) FROM pages WHERE deleted = 0 AND has_image = 1 AND image_file = '')");
    if (!st) return;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->pending_rows = sqlite3_column_int(st, 0);
        out->pending_images = sqlite3_column_int(st, 1);
        out->missing_images = sqlite3_column_int(st, 2);
    }
    sqlite3_finalize(st);
}
