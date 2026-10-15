// File numbers are what humans write on envelopes ("f 42", "F-0042") and what
// the QR label encodes. Normalizing gives one canonical id per physical file:
// uppercase, any run of non-alphanumerics becomes one "-", and leading zeros of
// each number are dropped, so "f 42", "F-042" and "F-0042" are all "F-42".
//
// Must stay identical to fileno_normalize() in mobile/src/fileno.c.

export const QR_PREFIX = 'MYFILES:';

export function normalizeFileNumber(raw) {
  return String(raw)
    .toUpperCase()
    .replace(/[^A-Z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')
    .replace(/\d+/g, (digits) => digits.replace(/^0+(?=\d)/, ''));
}

// What to print on a label: the number as the user typed it, tidied up but with its zero padding kept.
export function displayFileNumber(raw) {
  return String(raw).trim().toUpperCase().replace(/\s+/g, ' ');
}

export function qrPayload(fileNumber) {
  return QR_PREFIX + normalizeFileNumber(fileNumber);
}
