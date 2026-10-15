#ifndef MYFILES_HTTP_H
#define MYFILES_HTTP_H

#include <stdbool.h>
#include <stddef.h>

/* Blocking HTTP, called only from the sync worker thread. Implemented per platform:
 * libcurl (desktop), NSURLSession (iOS), HttpURLConnection via JNI (Android). */

typedef struct {
    const char *method;       /* "GET", "POST", "PUT" */
    const char *url;
    const char *bearer;       /* may be NULL */
    const char *content_type; /* may be NULL */
    const void *body;
    size_t body_len;
} HttpRequest;

typedef struct {
    int status;
    char *body;   /* NUL-terminated, free with SDL_free */
    size_t len;
} HttpResponse;

/* Returns false on network failure (no HTTP status). */
typedef bool (*HttpFn)(const HttpRequest *req, HttpResponse *res);

bool platform_http(const HttpRequest *req, HttpResponse *res);

#endif
