#ifndef MYFILES_DB_H
#define MYFILES_DB_H

#include <stdbool.h>
#include <stdint.h>

#include "fileno.h"

/* Local SQLite store. Every synced row has updated_at (epoch ms), deleted (tombstone) and dirty
 * (changed locally, not yet pushed). Documents are indexed in an FTS5 table, kept current by
 * triggers, so search works fully offline. */

typedef struct sqlite3 Db;

#define ID_LEN 40

typedef struct {
    char id[FILENO_MAX + 1];
    char file_number[FILENO_MAX + 1];
    char title[256];
    char location[256];
    char notes[1024];
    int doc_count;
    char tags[256]; /* distinct tags of the documents inside, comma separated */
    int64_t updated_at;
} Folder;

typedef struct {
    char id[ID_LEN];
    char folder_id[FILENO_MAX + 1];
    char title[256];
    char tags[256];
    char doc_date[64];
    int page_count;
    int64_t updated_at;
} Document;

typedef struct {
    char id[ID_LEN];
    char document_id[ID_LEN];
    int page_no;
    int width, height;
    char image_file[ID_LEN + 8]; /* "<id>.jpg" inside the pages dir; empty until on this device */
    bool has_remote_image;
} Page;

typedef struct {
    char doc_id[ID_LEN];
    char doc_title[256];
    char folder_id[FILENO_MAX + 1];
    char file_number[FILENO_MAX + 1];
    char folder_title[256];
    char snippet[320];
    char tags[256];
} SearchHit;

typedef struct {
    int pending_rows;    /* rows changed locally, not yet pushed */
    int pending_images;  /* page images not yet uploaded */
    int missing_images;  /* page images on the server, not yet downloaded */
} SyncCounts;

int64_t now_ms(void);
void new_id(char out[ID_LEN]);

Db *db_open(const char *path);
void db_close(Db *db);

/* key/value settings: server_url, token, email, cursor */
bool db_meta_get(Db *db, const char *key, char *out, int cap);
bool db_meta_set(Db *db, const char *key, const char *value);

/* Folders. Saving a folder whose file number normalizes to an existing (or deleted) folder's id
 * updates that folder: one physical envelope, one record. */
bool db_folder_save(Db *db, Folder *f); /* fills f->id */
bool db_folder_get(Db *db, const char *id, Folder *out);
int db_folder_list(Db *db, Folder *out, int max);
bool db_folder_delete(Db *db, const char *id); /* also tombstones its documents and pages */
/* Suggests the next number in the same series as the most recent folder, e.g. F-0042 -> F-0043. */
void db_folder_suggest_next(Db *db, char *out, int cap);

/* Documents */
bool db_document_save(Db *db, Document *d); /* new if d->id empty */
bool db_document_get(Db *db, const char *id, Document *out);
int db_document_list(Db *db, const char *folder_id, Document *out, int max);
bool db_document_delete(Db *db, const char *id);
char *db_document_ocr_text(Db *db, const char *id); /* SDL_free() result */
bool db_document_move(Db *db, const char *id, const char *folder_id);

/* Pages. The caller picks page_id (new_id) and has already put the JPEG at <pages dir>/<image_file>.
 * Adding a page appends its OCR text to the document's searchable text. */
bool db_page_add(Db *db, const char *document_id, const char *page_id, const char *image_file, int w, int h, const char *ocr_text);
int db_page_list(Db *db, const char *document_id, Page *out, int max);

/* Search: full-text over title, tags, OCR text, file number and folder title. Prefix-matches each word. */
int db_search(Db *db, const char *query, SearchHit *out, int max);
/* Folders whose file number, title or location matches. */
int db_search_folders(Db *db, const char *query, Folder *out, int max);

void db_sync_counts(Db *db, SyncCounts *out);

/* Merges comma-separated tag lists into distinct tags (case-insensitive, first spelling kept, in
 * order of first appearance): "tax, Bank,tax , ,house" -> "tax, Bank, house". */
void tags_distinct(const char *in, char *out, size_t cap);

#endif
