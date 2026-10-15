#ifndef MYFILES_SYNC_H
#define MYFILES_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "http.h"

/* Background sync with the MyFiles API, on its own thread and its own SQLite connection.
 *   1. push rows marked dirty            (POST /api/sync/push)
 *   2. upload page images not yet on the server (PUT /api/pages/:id/image)
 *   3. pull everything changed since the stored cursor (GET /api/sync/pull)
 *   4. download page images missing on this device, so every document is readable offline
 * When a run finishes it pushes an SDL event of type `done_event` so the UI can reload. */

typedef enum {
    SYNC_IDLE,
    SYNC_RUNNING,
    SYNC_OK,
    SYNC_OFFLINE,
    SYNC_SIGNED_OUT,
    SYNC_AUTH_FAILED,
    SYNC_ERROR,
} SyncState;

typedef struct SyncEngine SyncEngine;

SyncEngine *sync_create(const char *db_path, const char *pages_dir, HttpFn http, uint32_t done_event);
void sync_destroy(SyncEngine *s);

/* Queue a sync. If one is running, another run follows it. */
void sync_request(SyncEngine *s);
/* Sign in (or create an account) then sync. server_url like "https://files.example.com". */
void sync_sign_in(SyncEngine *s, const char *server_url, const char *email, const char *password, bool create_account);

SyncState sync_status(SyncEngine *s, char *message, int cap);

/* Runs one full sync synchronously on the calling thread (used by tests). */
SyncState sync_run_now(SyncEngine *s);

#endif
