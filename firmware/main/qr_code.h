/* Minimal QR Code encoder for on-device screens.
 *
 * Byte mode only, error-correction level L, versions 1..6. That ceiling covers
 * every payload the firmware generates (Wi-Fi join and reader URL) while
 * keeping the module matrix at 41 x 41, so a caller can hold one in a static
 * buffer instead of the heap.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Side length of the largest supported symbol (version 6). */
#define QR_MAX_MODULES (4 * 6 + 17)

/* Encode text into modules, which must hold QR_MAX_MODULES * QR_MAX_MODULES
 * bytes. On success the first n * n bytes hold a row-major matrix (1 = dark,
 * 0 = light) with no quiet zone, and n is returned. Returns 0 when text is
 * NULL, longer than version 6 can hold at ECC level L (134 bytes), or does
 * not fit the supplied buffer contract.
 *
 * Encoding is deterministic: the same text always yields the same matrix. */
int qr_encode(const char *text, unsigned char *modules);

#ifdef __cplusplus
}
#endif
