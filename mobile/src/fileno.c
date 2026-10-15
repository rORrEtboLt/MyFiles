#include "fileno.h"

#include <string.h>

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alnum_upper(char c) { return is_digit(c) || (c >= 'A' && c <= 'Z'); }

size_t fileno_normalize(const char *in, char *out, size_t cap)
{
    if (cap == 0) return 0;
    size_t n = 0;
    bool pending_dash = false;
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!is_alnum_upper(c)) {
            pending_dash = n > 0;
            continue;
        }
        if (pending_dash) {
            if (n + 1 >= cap) break;
            out[n++] = '-';
            pending_dash = false;
        }
        /* Drop a leading zero when it starts a number and another digit follows. */
        bool starts_number = n == 0 || !is_digit(out[n - 1]);
        if (c == '0' && starts_number) {
            const char *q = p + 1;
            if (is_digit(*q)) continue;
        }
        if (n + 1 >= cap) break;
        out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

bool fileno_from_qr(const char *payload, char *out, size_t cap)
{
    size_t plen = strlen(FILENO_QR_PREFIX);
    if (strncmp(payload, FILENO_QR_PREFIX, plen) != 0) return false;
    return fileno_normalize(payload + plen, out, cap) > 0;
}
