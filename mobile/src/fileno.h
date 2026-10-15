#ifndef MYFILES_FILENO_H
#define MYFILES_FILENO_H

#include <stdbool.h>
#include <stddef.h>

#define FILENO_MAX 64
#define FILENO_QR_PREFIX "MYFILES:"

/* Canonical folder id for a human-written file number: uppercase, runs of
 * non-alphanumerics become one '-', no leading/trailing '-', and leading zeros
 * of each number are dropped ("f 0042" -> "F-42"). Must match
 * normalizeFileNumber() in backend/src/lib/fileNumber.js.
 * Returns the length written (0 if nothing usable). */
size_t fileno_normalize(const char *in, char *out, size_t cap);

/* If payload is a MyFiles QR label ("MYFILES:F-42"), writes the folder id. */
bool fileno_from_qr(const char *payload, char *out, size_t cap);

#endif
