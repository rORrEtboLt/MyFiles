/* platform_http for desktop builds (and the end-to-end sync test). */
#include <SDL3/SDL.h>
#include <curl/curl.h>

#include "../http.h"

typedef struct { char *data; size_t len, cap; } Buf;

static size_t on_data(char *ptr, size_t size, size_t n, void *user)
{
    Buf *b = user;
    size_t add = size * n;
    if (b->len + add + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + add + 1) cap *= 2;
        char *p = SDL_realloc(b->data, cap);
        if (!p) return 0;
        b->data = p;
        b->cap = cap;
    }
    SDL_memcpy(b->data + b->len, ptr, add);
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}

bool platform_http(const HttpRequest *req, HttpResponse *res)
{
    static SDL_InitState init;
    if (SDL_ShouldInit(&init)) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        SDL_SetInitialized(&init, true);
    }
    SDL_zerop(res);
    CURL *c = curl_easy_init();
    if (!c) return false;

    struct curl_slist *headers = NULL;
    char line[2200];
    if (req->bearer) {
        SDL_snprintf(line, sizeof line, "Authorization: Bearer %s", req->bearer);
        headers = curl_slist_append(headers, line);
    }
    if (req->content_type) {
        SDL_snprintf(line, sizeof line, "Content-Type: %s", req->content_type);
        headers = curl_slist_append(headers, line);
    }
    Buf b = {0};
    curl_easy_setopt(c, CURLOPT_URL, req->url);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req->method);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (req->body) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req->body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_len);
    }
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        SDL_free(b.data);
        return false;
    }
    res->status = (int)status;
    res->body = b.data ? b.data : SDL_strdup("");
    res->len = b.len;
    return true;
}
