/* Small string helpers: HTML escaping, strict URL decoding, strict parsing. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Escape & < > " ' as HTML entities into out (NUL-terminated, up to cap
 * bytes including NUL). Returns the number of bytes written (excluding NUL)
 * or 0 if the escaped text does not fit. */
size_t html_escape(const char *in, size_t in_len, char *out, size_t cap);

/* Strict percent-decoding (RFC 3986). '+' is NOT converted (path context).
 * Rejects malformed %XX, a trailing '%', and decoded control bytes < 0x20
 * and 0x7F. Bytes >= 0x80 pass through (UTF-8 payloads). out must hold
 * in_len + 1 bytes. Returns decoded length (excluding NUL) or -1. */
int url_decode_path(const char *in, size_t in_len, char *out, size_t cap);

/* Same rules but for query-string context: '+' decodes to space. */
int url_decode_query(const char *in, size_t in_len, char *out, size_t cap);

/* Strict decimal parsing of an ASCII digit string; rejects empty, leading
 * '+'/'-', whitespace and overflow. Returns true and sets *out. */
bool parse_u32_strict(const char *s, size_t len, uint32_t *out);

/* Format "crc-%08x" into out (13 bytes incl. NUL). */
void etag_from_crc(uint32_t crc, char *out);

#ifdef __cplusplus
}
#endif
