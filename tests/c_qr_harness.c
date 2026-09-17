/* Host-side harness for the firmware's qr_code.c.
 *
 * Usage: c_qr_harness
 *   stdin lines: one payload per line (the newline is not part of the payload)
 *   stdout: for each payload, "<size>" followed by <size> rows of '#' (dark)
 *           and '.' (light), or "0" when the encoder rejects the payload.
 */
#include <stdio.h>
#include <string.h>

#include "qr_code.h"

int main(void)
{
    static unsigned char modules[QR_MAX_MODULES * QR_MAX_MODULES];
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        int size = qr_encode(line, modules);
        printf("%d\n", size);
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) putchar(modules[y * size + x] ? '#' : '.');
            putchar('\n');
        }
        fflush(stdout);
    }
    return 0;
}
