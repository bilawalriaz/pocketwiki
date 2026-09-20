/* Streaming catalogue reader: see catalog_index.h for why this is not a general
 * JSON parser. It tokenises just enough to deliver the fields the device serves
 * and installs, in one pass, with no allocation and one entry of fixed storage. */
#include "catalog_index.h"

#include <string.h>

/* Longest single value captured in one token. */
#define TOKEN_CAP 96
/* Nesting the published catalogue uses: root object, packs array, pack object. */
#define DEPTH_MAX 8

typedef struct {
    bool failed;
    bool done;                  /* the answer is complete; stop reading */

    bool in_string;
    bool escape;
    bool overflow;              /* the current string token does not fit */
    size_t token_len;
    char token[TOKEN_CAP];
    bool pending_string;        /* a string closed; ':' would make it a key */

    int depth;
    bool in_packs;
    int packs_depth;
    bool in_pack;

    char key[TOKEN_CAP];
    bool have_key;

    pw_catalog_pack_t pack;

    /* Find pass. */
    const char *want_id;
    const char *want_url;
    pw_catalog_pack_t *found;

    /* Names pass. */
    pw_catalog_name_slot_t *slots;
    int slot_count;
    int filled;

    /* Validate pass. */
    int complete_entries;

    bool header_only;

    int schema;
    int catalog_version;
    char generated_at[PW_CATALOG_STAMP_CAP];

    bool number_active;
    bool number_negative;
    bool number_overflow;
    int64_t number;
} scanner_t;

static bool copy_bounded(char *dst, size_t cap, const char *src, size_t len)
{
    if (len >= cap) return false;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

static void set_key(scanner_t *s)
{
    copy_bounded(s->key, sizeof s->key, s->token, s->token_len);
    s->have_key = true;
    s->pending_string = false;
}

static void handle_string_value(scanner_t *s)
{
    if (s->depth == 1) {
        if (strcmp(s->key, "generated_at") == 0) {
            copy_bounded(s->generated_at, sizeof s->generated_at, s->token, s->token_len);
        }
        return;                              /* header metadata */
    }
    if (!s->in_pack) return;                 /* a nested object's strings */

    if (strcmp(s->key, "id") == 0) {
        if (s->overflow || !copy_bounded(s->pack.id, sizeof s->pack.id, s->token, s->token_len)) s->failed = true;
    } else if (strcmp(s->key, "name") == 0) {
        if (s->overflow || !copy_bounded(s->pack.name, sizeof s->pack.name, s->token, s->token_len)) s->failed = true;
    } else if (strcmp(s->key, "url") == 0) {
        if (s->overflow || !copy_bounded(s->pack.url, sizeof s->pack.url, s->token, s->token_len)) s->failed = true;
    } else if (strcmp(s->key, "sha256") == 0) {
        if (s->overflow || !copy_bounded(s->pack.sha256, sizeof s->pack.sha256, s->token, s->token_len)) s->failed = true;
    }
}

static void handle_number_value(scanner_t *s)
{
    if (s->number_overflow) {
        if (s->in_pack) s->failed = true;
        return;
    }
    int64_t value = s->number_negative ? -s->number : s->number;
    if (s->depth == 1) {
        if (strcmp(s->key, "schema") == 0) {
            s->schema = (int)value;
        } else if (strcmp(s->key, "catalog_version") == 0) {
            s->catalog_version = (int)value;
        }
        return;
    }
    if (!s->in_pack) return;
    if (strcmp(s->key, "bytes") == 0) s->pack.bytes = value;
    else if (strcmp(s->key, "version") == 0) s->pack.version = (int)value;
    else if (strcmp(s->key, "articles") == 0) s->pack.articles = (int)value;
}

/* The pack object closed. An entry is accepted only when the fields the device
 * installs from are all present, which is the rule the previous general-purpose
 * parser enforced; a document with a broken entry is unusable as a whole. */
static void close_pack(scanner_t *s)
{
    pw_catalog_pack_t *pack = &s->pack;
    s->in_pack = false;
    if (pack->id[0] == '\0' || pack->url[0] == '\0' ||
        pack->sha256[0] == '\0' || pack->bytes < 0) {
        s->failed = true;
        return;
    }
    pack->complete = true;
    s->complete_entries++;

    if (s->found != NULL) {
        bool match = (s->want_id != NULL && strcmp(pack->id, s->want_id) == 0) ||
                     (s->want_url != NULL && strcmp(pack->url, s->want_url) == 0);
        if (match) {
            *s->found = *pack;
            s->done = true;
        }
        return;
    }

    for (int i = 0; i < s->slot_count; i++) {
        pw_catalog_name_slot_t *slot = &s->slots[i];
        if (slot->id == NULL || strcmp(slot->id, pack->id) != 0) continue;
        if (pack->name[0] != '\0' && !copy_bounded(slot->out, slot->cap, pack->name, strlen(pack->name))) {
            s->failed = true;                /* the caller's buffer is too small */
            return;
        }
        s->filled++;
        if (s->filled == s->slot_count) s->done = true;
    }
}

static void start_container(scanner_t *s, char kind)
{
    s->depth++;
    if (kind == '[' && s->have_key && strcmp(s->key, "packs") == 0 && s->depth == 2) {
        s->in_packs = true;
        s->packs_depth = s->depth;
    } else if (kind == '{' && s->in_packs && s->depth == s->packs_depth + 1 &&
               !s->header_only && !s->done) {
        s->in_pack = true;
        memset(&s->pack, 0, sizeof s->pack);
        s->pack.bytes = -1;
    }
}

static void end_container(scanner_t *s, char kind)
{
    if (s->depth > 0) s->depth--;
    if (kind == '}' && s->in_pack && s->depth == s->packs_depth) {
        close_pack(s);
    } else if (kind == ']' && s->in_packs && s->depth == s->packs_depth - 1) {
        s->in_packs = false;
    }
}

/* A non-':' delimiter ends a pending string, so it was a value. */
static void flush_pending_string(scanner_t *s)
{
    if (!s->pending_string) return;
    handle_string_value(s);
    s->pending_string = false;
    s->have_key = false;
}

static void feed(scanner_t *s, char c)
{
    if (s->failed) return;

    if (s->in_string) {
        if (s->escape) {
            s->escape = false;
            char decoded = c;
            switch (c) {
            case 'n': decoded = '\n'; break;
            case 't': decoded = '\t'; break;
            case 'r': decoded = '\r'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            default: break;                 /* \" \\ \/ and \uXXXX keep the byte */
            }
            if (!s->overflow) {
                if (s->token_len < TOKEN_CAP - 1) s->token[s->token_len++] = decoded;
                else s->overflow = true;
            } else {
                s->token_len++;
            }
            return;
        }
        if (c == '\\') { s->escape = true; return; }
        if (c == '"') {
            s->in_string = false;
            s->token[s->token_len < TOKEN_CAP ? s->token_len : TOKEN_CAP - 1] = '\0';
            s->pending_string = true;
            return;
        }
        if (s->token_len < TOKEN_CAP - 1 && !s->overflow) s->token[s->token_len++] = c;
        else { s->overflow = true; s->token_len++; }
        return;
    }

    if (s->number_active) {
        if (c >= '0' && c <= '9') {
            if (s->number > (INT64_MAX - (c - '0')) / 10) s->number_overflow = true;
            else s->number = s->number * 10 + (c - '0');
            return;
        }
        s->number_active = false;
        handle_number_value(s);
        if (s->failed) return;
    }

    switch (c) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
        return;
    case '"':
        s->in_string = true;
        s->escape = false;
        s->overflow = false;
        s->token_len = 0;
        s->token[0] = '\0';
        return;
    case ':':
        if (s->pending_string) set_key(s);
        return;
    case '{':
    case '[':
        flush_pending_string(s);
        start_container(s, c);
        s->have_key = false;
        return;
    case '}':
    case ']':
        flush_pending_string(s);
        end_container(s, c);
        s->have_key = false;
        return;
    case ',':
        flush_pending_string(s);
        s->have_key = false;
        return;
    case '-':
        s->number_active = true;
        s->number_negative = true;
        s->number = 0;
        s->number_overflow = false;
        return;
    default:
        if (c >= '0' && c <= '9') {
            s->number_active = true;
            s->number_negative = false;
            s->number = (int64_t)(c - '0');
            s->number_overflow = false;
        }
        /* true/false/null carry no field we keep. */
        return;
    }
}

/* Read the document to the end. The published catalogue sorts its keys, so
 * `schema` can follow `packs`: nothing may be decided before the last byte. A
 * header pass ignores entries entirely, so a broken entry cannot fail it, and a
 * pass that has its answer stops parsing entries but still reads the header. */
static bool run(pw_catalog_read_fn read_fn, void *ctx, scanner_t *s)
{
    char buf[256];
    for (;;) {
        int n = read_fn(ctx, buf, sizeof buf);
        if (n < 0) return false;
        if (n == 0) break;
        for (int i = 0; i < n; i++) {
            feed(s, buf[i]);
            if (s->failed) return false;
        }
    }
    if (s->in_string || s->depth != 0 || s->in_pack) return false;   /* truncated */
    if (s->failed) return false;
    return s->schema == 1;
}

bool pw_catalog_read_header(pw_catalog_read_fn read_fn, void *ctx, pw_catalog_header_t *out)
{
    if (read_fn == NULL || out == NULL) return false;
    scanner_t s;
    memset(&s, 0, sizeof s);
    s.header_only = true;
    bool ok = run(read_fn, ctx, &s);
    out->ok = ok;
    out->version = s.catalog_version;
    copy_bounded(out->generated_at, sizeof out->generated_at, s.generated_at, strlen(s.generated_at));
    return ok;
}

bool pw_catalog_find(pw_catalog_read_fn read_fn, void *ctx,
                     const char *id, const char *url, pw_catalog_pack_t *out)
{
    if (read_fn == NULL || out == NULL || (id == NULL && url == NULL)) return false;
    scanner_t s;
    memset(&s, 0, sizeof s);
    memset(out, 0, sizeof *out);
    s.want_id = id;
    s.want_url = url;
    s.found = out;
    if (!run(read_fn, ctx, &s)) return false;
    return s.done && out->complete;
}

int pw_catalog_names(pw_catalog_read_fn read_fn, void *ctx,
                     pw_catalog_name_slot_t *slots, int count)
{
    if (read_fn == NULL || slots == NULL || count <= 0) return 0;
    scanner_t s;
    memset(&s, 0, sizeof s);
    s.slots = slots;
    s.slot_count = count;
    if (!run(read_fn, ctx, &s)) return 0;
    return s.filled;
}

bool pw_catalog_validate(pw_catalog_read_fn read_fn, void *ctx)
{
    if (read_fn == NULL) return false;
    scanner_t s;
    memset(&s, 0, sizeof s);
    if (!run(read_fn, ctx, &s)) return false;
    return s.complete_entries > 0;
}
