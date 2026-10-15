/* End-to-end sync test: two "phones" (separate SQLite databases) syncing through a real backend.
 *   MYFILES_TEST_SERVER=http://127.0.0.1:4555 ./test_sync
 * Skips (exit 0) when MYFILES_TEST_SERVER is not set. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "db.h"
#include "sync.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { failures++; SDL_Log("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); } } while (0)

/* Minimal JPEG (SOI/APP0/EOI): the server only checks the signature. */
static const unsigned char JPEG[] = {0xff, 0xd8, 0xff, 0xe0, 0, 0x10, 0x4a, 0x46, 0x49, 0x46, 0, 1, 0xff, 0xd9};

typedef struct {
    char db_path[1024];
    char pages_dir[1024];
    Db *db;
    SyncEngine *sync;
} Phone;

static void phone_open(Phone *p, const char *root, const char *name, Uint32 ev)
{
    SDL_snprintf(p->db_path, sizeof p->db_path, "%s%s.db", root, name);
    SDL_snprintf(p->pages_dir, sizeof p->pages_dir, "%s%s-pages/", root, name);
    SDL_CreateDirectory(p->pages_dir);
    p->db = db_open(p->db_path);
    p->sync = sync_create(p->db_path, p->pages_dir, platform_http, ev);
}

static void phone_close(Phone *p)
{
    sync_destroy(p->sync);
    db_close(p->db);
}

/* Waits for the background engine to reach a terminal state. */
static SyncState wait_done(Phone *p)
{
    char msg[256];
    for (int i = 0; i < 400; i++) {
        SyncState st = sync_status(p->sync, msg, sizeof msg);
        if (st != SYNC_IDLE && st != SYNC_RUNNING) {
            SDL_Log("  status: %s", msg);
            return st;
        }
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_EVENT_USER, SDL_EVENT_LAST);
        SDL_Delay(50);
    }
    return SYNC_RUNNING;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *server = SDL_getenv("MYFILES_TEST_SERVER");
    if (!server) {
        SDL_Log("MYFILES_TEST_SERVER not set - skipping end-to-end sync test");
        return 0;
    }
    SDL_Init(SDL_INIT_EVENTS);
    Uint32 ev = SDL_RegisterEvents(1);

    char root[1024];
    SDL_snprintf(root, sizeof root, "%se2e-%llu/", SDL_GetBasePath(), (unsigned long long)SDL_GetTicksNS());
    SDL_CreateDirectory(root);
    char email[128];
    SDL_snprintf(email, sizeof email, "e2e-%llu@example.com", (unsigned long long)SDL_GetTicksNS());

    Phone a, b;
    phone_open(&a, root, "a", ev);
    phone_open(&b, root, "b", ev);

    /* Phone A, offline: create an envelope with a scanned document. */
    Folder f = {0};
    SDL_strlcpy(f.file_number, "F-0042", sizeof f.file_number);
    SDL_strlcpy(f.title, "House papers", sizeof f.title);
    CHECK(db_folder_save(a.db, &f));
    Document d = {0};
    SDL_strlcpy(d.folder_id, f.id, sizeof d.folder_id);
    SDL_strlcpy(d.title, "Electricity bill", sizeof d.title);
    CHECK(db_document_save(a.db, &d));
    char pid[ID_LEN], file[ID_LEN + 8], path[1200];
    new_id(pid);
    SDL_snprintf(file, sizeof file, "%s.jpg", pid);
    SDL_snprintf(path, sizeof path, "%s%s", a.pages_dir, file);
    CHECK(SDL_SaveFile(path, JPEG, sizeof JPEG));
    CHECK(db_page_add(a.db, d.id, pid, file, 1000, 1400, "BESCOM consumer number 7781"));

    /* Not signed in yet: sync is a no-op that keeps data local. */
    CHECK(sync_run_now(a.sync) == SYNC_SIGNED_OUT);

    SDL_Log("phone A: create account + sync");
    sync_sign_in(a.sync, server, email, "correct horse battery", true);
    CHECK(wait_done(&a) == SYNC_OK);
    SyncCounts c;
    db_sync_counts(a.db, &c);
    CHECK(c.pending_rows == 0 && c.pending_images == 0);

    SDL_Log("phone B: sign in + sync");
    sync_sign_in(b.sync, server, email, "correct horse battery", false);
    CHECK(wait_done(&b) == SYNC_OK);

    Folder got;
    CHECK(db_folder_get(b.db, "F-42", &got));
    CHECK(SDL_strcmp(got.title, "House papers") == 0 && got.doc_count == 1);
    SearchHit hits[4];
    int n = db_search(b.db, "bescom", hits, 4);
    CHECK(n == 1 && SDL_strcmp(hits[0].file_number, "F-0042") == 0);
    Page pages[4];
    CHECK(db_page_list(b.db, d.id, pages, 4) == 1);
    if (pages[0].image_file[0]) {
        char bpath[1200];
        SDL_snprintf(bpath, sizeof bpath, "%s%s", b.pages_dir, pages[0].image_file);
        size_t len = 0;
        void *data = SDL_LoadFile(bpath, &len);
        CHECK(data && len == sizeof JPEG && SDL_memcmp(data, JPEG, len) == 0);
        SDL_free(data);
    } else {
        CHECK(!"page image was not downloaded to phone B");
    }

    SDL_Log("phone B edits, phone A receives");
    Document bd;
    CHECK(db_document_get(b.db, d.id, &bd));
    SDL_strlcpy(bd.title, "Electricity bill March", sizeof bd.title);
    SDL_Delay(5); /* distinct updated_at */
    CHECK(db_document_save(b.db, &bd));
    CHECK(sync_run_now(b.sync) == SYNC_OK);
    CHECK(sync_run_now(a.sync) == SYNC_OK);
    Document ad;
    CHECK(db_document_get(a.db, d.id, &ad) && SDL_strcmp(ad.title, "Electricity bill March") == 0);

    SDL_Log("phone A deletes the file, phone B loses it");
    CHECK(db_folder_delete(a.db, "F-42"));
    CHECK(sync_run_now(a.sync) == SYNC_OK);
    CHECK(sync_run_now(b.sync) == SYNC_OK);
    CHECK(!db_folder_get(b.db, "F-42", &got));
    CHECK(db_search(b.db, "bescom", hits, 4) == 0);
    char bpath[1200];
    SDL_snprintf(bpath, sizeof bpath, "%s%s", b.pages_dir, file);
    CHECK(!SDL_GetPathInfo(bpath, NULL)); /* local image removed */

    SDL_Log("wrong password is reported, not crashed on");
    Phone c3;
    phone_open(&c3, root, "c", ev);
    sync_sign_in(c3.sync, server, email, "wrong password!", false);
    CHECK(wait_done(&c3) == SYNC_AUTH_FAILED);
    phone_close(&c3);

    phone_close(&a);
    phone_close(&b);
    SDL_Log("%s (%d failures)", failures ? "FAILED" : "OK", failures);
    SDL_Quit();
    return failures ? 1 : 0;
}
