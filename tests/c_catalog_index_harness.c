/* Host harness for the firmware's streaming catalogue reader.
 *
 * Reads a catalogue document on stdin and answers one question with it, with a
 * configurable read size (argv[1], default 256; 1 exercises every token
 * boundary). The Python test checks the answers.
 *
 *   harness <chunk> header
 *   harness <chunk> find <id> <url>      (either may be empty)
 *   harness <chunk> names <id> [id...]
 *   harness <chunk> validate
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_index.h"

typedef struct {
    const char *data;
    size_t len;
    size_t pos;
    size_t chunk;
} source_t;

static int read_bytes(void *ctx, char *buf, size_t cap)
{
    source_t *s = ctx;
    if (cap > s->chunk) cap = s->chunk;
    if (s->pos >= s->len) return 0;
    size_t n = s->len - s->pos;
    if (n > cap) n = cap;
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return (int)n;
}

/* Print a value with control characters escaped, so one value stays one line. */
static void print_escaped(const char *value)
{
    for (const unsigned char *p = (const unsigned char *)value; *p != 0; p++) {
        switch (*p) {
        case '\n': fputs("\\n", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        default: fputc(*p, stdout); break;
        }
    }
}

static void source_init(source_t *s, const char *data, size_t len, size_t chunk)
{
    s->data = data;
    s->len = len;
    s->pos = 0;
    s->chunk = chunk;
}

int main(int argc, char **argv)
{
    size_t chunk = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 256;
    if (chunk == 0) chunk = 1;
    const char *mode = argc > 2 ? argv[2] : "header";

    size_t cap = 1 << 20;
    char *doc = malloc(cap);
    if (doc == NULL) return 2;
    size_t len = fread(doc, 1, cap, stdin);
    doc[len] = '\0';

    source_t src;
    source_init(&src, doc, len, chunk);

    if (strcmp(mode, "header") == 0) {
        pw_catalog_header_t hdr;
        bool ok = pw_catalog_read_header(read_bytes, &src, &hdr);
        printf("ok=%d version=%d at=", ok ? 1 : 0, hdr.version);
        print_escaped(hdr.generated_at);
        fputc('\n', stdout);
    } else if (strcmp(mode, "validate") == 0) {
        printf("ok=%d\n", pw_catalog_validate(read_bytes, &src) ? 1 : 0);
    } else if (strcmp(mode, "find") == 0) {
        const char *id = argc > 3 && argv[3][0] != '\0' ? argv[3] : NULL;
        const char *url = argc > 4 && argv[4][0] != '\0' ? argv[4] : NULL;
        pw_catalog_pack_t pack;
        bool ok = pw_catalog_find(read_bytes, &src, id, url, &pack);
        printf("ok=%d\n", ok ? 1 : 0);
        if (ok) {
            print_escaped(pack.id); fputc('|', stdout);
            print_escaped(pack.name); fputc('|', stdout);
            print_escaped(pack.url); fputc('|', stdout);
            print_escaped(pack.sha256);
            printf("|%lld|%d|%d\n", (long long)pack.bytes, pack.version, pack.articles);
        }
    } else if (strcmp(mode, "names") == 0) {
        int count = argc - 3;
        if (count <= 0) return 3;
        pw_catalog_name_slot_t *slots = calloc((size_t)count, sizeof *slots);
        char *names = calloc((size_t)count, PW_CATALOG_NAME_CAP);
        if (slots == NULL || names == NULL) return 2;
        for (int i = 0; i < count; i++) {
            slots[i].id = argv[3 + i];
            slots[i].out = names + (size_t)i * PW_CATALOG_NAME_CAP;
            slots[i].cap = PW_CATALOG_NAME_CAP;
            names[(size_t)i * PW_CATALOG_NAME_CAP] = '\0';
        }
        int filled = pw_catalog_names(read_bytes, &src, slots, count);
        printf("filled=%d\n", filled);
        for (int i = 0; i < count; i++) {
            printf("N ");
            print_escaped(argv[3 + i]);
            fputc('|', stdout);
            print_escaped(names + (size_t)i * PW_CATALOG_NAME_CAP);
            fputc('\n', stdout);
        }
        free(slots);
        free(names);
    } else {
        free(doc);
        return 3;
    }

    free(doc);
    return 0;
}
