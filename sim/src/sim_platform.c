/* Host stand-ins for the ESP-IDF services the PocketWiki firmware calls:
 * logging, CRC, raw partitions, the FAT pack store, FreeRTOS primitives,
 * NVS, the task watchdog, SHA-256 and the slice of cJSON the server uses.
 *
 * The pack store is a host directory, but its capacity is the device's: free
 * space is the FAT-usable size of the target's `packs` partition minus the
 * clusters the files in it occupy, so "not enough storage" happens here at the
 * same point it happens on a board.
 */
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "sim.h"

/* ---- configuration ------------------------------------------------------- */

sim_config_t g_sim = {
    .log_level = 'I',
    .fat_total_bytes = 2347008,       /* ESP32-C3 partitions.csv, IDF FAT geometry */
    .fat_cluster = 4096,
    .content_part_size = 0x20000,
    .index_part_size = 0x4000,
    .sta_connected = true,
    .flash_read_us = 0,
};

/* ---- logging ------------------------------------------------------------- */

static int level_rank(char l)
{
    switch (l) {
    case 'E': return 1;
    case 'W': return 2;
    case 'I': return 3;
    case 'D': return 4;
    default: return 5;
    }
}

void sim_log(char level, const char *tag, const char *fmt, ...)
{
    if (level_rank(level) > level_rank(g_sim.log_level)) return;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    fprintf(stderr, "%c (%lld) %s: ", level, (long long)(esp_timer_get_time() / 1000), tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&lock);
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_CRC: return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_INVALID_VERSION: return "ESP_ERR_INVALID_VERSION";
    default: return "ESP_ERR_UNKNOWN";
    }
}

/* ---- CRC ----------------------------------------------------------------- */

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- raw partitions ------------------------------------------------------ */

static esp_partition_t s_parts[2];

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, int subtype,
                                                 const char *label)
{
    (void)type;
    const char *path = NULL;
    uint32_t size = 0;
    int slot;
    if (subtype == 0x40 && label && strcmp(label, "content") == 0) {
        path = g_sim.content_path; size = g_sim.content_part_size; slot = 0;
    } else if (subtype == 0x41 && label && strcmp(label, "index") == 0) {
        path = g_sim.index_path; size = g_sim.index_part_size; slot = 1;
    } else {
        return NULL;
    }
    esp_partition_t *p = &s_parts[slot];
    p->type = ESP_PARTITION_TYPE_DATA;
    p->subtype = subtype;
    p->size = size;
    p->path = path;
    snprintf(p->label, sizeof p->label, "%s", label);
    return p;
}

/* Reads past the end of the image return 0xFF, as erased flash does. */
esp_err_t esp_partition_read(const esp_partition_t *part, size_t off, void *dst, size_t len)
{
    if (off > part->size || len > part->size - off) return ESP_ERR_INVALID_ARG;
    if (g_sim.flash_read_us > 0) usleep(g_sim.flash_read_us);
    memset(dst, 0xFF, len);
    if (part->path == NULL) return ESP_OK;
    FILE *fh = fopen(part->path, "rb");
    if (fh == NULL) return ESP_OK;
    if (fseek(fh, (long)off, SEEK_SET) == 0) {
        size_t got = fread(dst, 1, len, fh);
        (void)got;
    }
    fclose(fh);
    return ESP_OK;
}

/* ---- FAT pack store ------------------------------------------------------ */

esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base_path, const char *label,
                                           const esp_vfs_fat_mount_config_t *conf,
                                           wl_handle_t *handle)
{
    (void)label;
    (void)conf;
    if (mkdir(base_path, 0755) != 0 && errno != EEXIST) return ESP_FAIL;
    *handle = 0;
    return ESP_OK;
}

static uint64_t dir_usage(const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL) return 0;
    uint64_t used = g_sim.fat_cluster;          /* the directory's own cluster */
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char child[1024];
        snprintf(child, sizeof child, "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            used += dir_usage(child);
        } else {
            uint64_t c = g_sim.fat_cluster;
            used += ((uint64_t)st.st_size + c - 1) / c * c;
        }
    }
    closedir(d);
    return used;
}

esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total, uint64_t *free_bytes)
{
    uint64_t used = dir_usage(base_path);
    *total = g_sim.fat_total_bytes;
    *free_bytes = used >= g_sim.fat_total_bytes ? 0 : g_sim.fat_total_bytes - used;
    return ESP_OK;
}

/* ---- system -------------------------------------------------------------- */

/* Microseconds since the simulated device booted. */
int64_t esp_timer_get_time(void)
{
    static int64_t boot;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    if (boot == 0) boot = now;
    return now - boot;
}

/* Representative ESP32-C3 figures with Wi-Fi, BLE and httpd running. */
uint32_t esp_get_free_heap_size(void) { return 98304; }
uint32_t esp_get_minimum_free_heap_size(void) { return 81920; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 45056; }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 98304; }
void esp_restart(void) { exit(3); }

esp_err_t esp_task_wdt_status(TaskHandle_t task) { (void)task; return ESP_ERR_NOT_FOUND; }
esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
esp_err_t esp_task_wdt_add(TaskHandle_t task) { (void)task; return ESP_OK; }
esp_err_t esp_task_wdt_delete(TaskHandle_t task) { (void)task; return ESP_OK; }
esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }

/* ---- FreeRTOS ------------------------------------------------------------ */

typedef struct {
    TaskFunction_t fn;
    void *arg;
} task_start_t;

static void *task_trampoline(void *p)
{
    task_start_t start = *(task_start_t *)p;
    free(p);
    start.fn(start.arg);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *out)
{
    (void)name; (void)stack; (void)prio;
    task_start_t *start = malloc(sizeof *start);
    if (start == NULL) return pdFALSE;
    start->fn = fn;
    start->arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, task_trampoline, start) != 0) {
        free(start);
        return pdFALSE;
    }
    pthread_detach(t);
    if (out) *out = (TaskHandle_t)t;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    if (task == NULL) pthread_exit(NULL);
}

void vTaskDelay(TickType_t ticks) { usleep((useconds_t)ticks * 1000); }
void taskYIELD(void) { sched_yield(); }

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *m = malloc(sizeof *m);
    if (m != NULL) pthread_mutex_init(m, NULL);
    return m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    pthread_mutex_t *m = s;
    if (wait == portMAX_DELAY) return pthread_mutex_lock(m) == 0 ? pdTRUE : pdFALSE;
    for (TickType_t waited = 0;; waited++) {
        if (pthread_mutex_trylock(m) == 0) return pdTRUE;
        if (waited >= wait) return pdFALSE;
        usleep(1000);
    }
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    return pthread_mutex_unlock((pthread_mutex_t *)s) == 0 ? pdTRUE : pdFALSE;
}

/* ---- NVS: an in-memory string store ------------------------------------- */

#define NVS_SLOTS 16
static struct { char key[16]; char *value; } s_nvs[NVS_SLOTS];
static pthread_mutex_t s_nvs_lock = PTHREAD_MUTEX_INITIALIZER;

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)ns; (void)mode;
    *out = 1;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    (void)h;
    esp_err_t err = ESP_ERR_NVS_NOT_FOUND;
    pthread_mutex_lock(&s_nvs_lock);
    for (int i = 0; i < NVS_SLOTS; i++) {
        if (s_nvs[i].value && strcmp(s_nvs[i].key, key) == 0) {
            size_t need = strlen(s_nvs[i].value) + 1;
            if (out == NULL) { *len = need; err = ESP_OK; }
            else if (*len < need) err = ESP_ERR_INVALID_SIZE;
            else { memcpy(out, s_nvs[i].value, need); *len = need; err = ESP_OK; }
            break;
        }
    }
    pthread_mutex_unlock(&s_nvs_lock);
    return err;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    (void)h;
    pthread_mutex_lock(&s_nvs_lock);
    int slot = -1;
    for (int i = 0; i < NVS_SLOTS; i++) {
        if (s_nvs[i].value && strcmp(s_nvs[i].key, key) == 0) { slot = i; break; }
        if (slot < 0 && s_nvs[i].value == NULL) slot = i;
    }
    esp_err_t err = ESP_ERR_NO_MEM;
    if (slot >= 0) {
        free(s_nvs[slot].value);
        snprintf(s_nvs[slot].key, sizeof s_nvs[slot].key, "%s", key);
        s_nvs[slot].value = strdup(value);
        err = ESP_OK;
    }
    pthread_mutex_unlock(&s_nvs_lock);
    return err;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    pthread_mutex_lock(&s_nvs_lock);
    for (int i = 0; i < NVS_SLOTS; i++) {
        if (s_nvs[i].value && strcmp(s_nvs[i].key, key) == 0) {
            free(s_nvs[i].value);
            s_nvs[i].value = NULL;
        }
    }
    pthread_mutex_unlock(&s_nvs_lock);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

/* ---- SHA-256 behind the PSA hash API ------------------------------------ */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t st[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }

psa_status_t psa_hash_setup(psa_hash_operation_t *op, psa_algorithm_t alg)
{
    if (alg != PSA_ALG_SHA_256 || op->active) return PSA_ERROR_GENERIC_ERROR;
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(op->state, iv, sizeof iv);
    op->bits = 0;
    op->used = 0;
    op->active = 1;
    return PSA_SUCCESS;
}

psa_status_t psa_hash_update(psa_hash_operation_t *op, const uint8_t *in, size_t len)
{
    if (!op->active) return PSA_ERROR_GENERIC_ERROR;
    op->bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t n = 64 - op->used < len ? 64 - op->used : len;
        memcpy(op->buf + op->used, in, n);
        op->used += n;
        in += n;
        len -= n;
        if (op->used == 64) {
            sha256_block(op->state, op->buf);
            op->used = 0;
        }
    }
    return PSA_SUCCESS;
}

psa_status_t psa_hash_finish(psa_hash_operation_t *op, uint8_t *out, size_t cap, size_t *olen)
{
    if (!op->active || cap < 32) return PSA_ERROR_GENERIC_ERROR;
    uint64_t bits = op->bits;
    uint8_t pad = 0x80;
    psa_hash_update(op, &pad, 1);
    uint8_t zero = 0;
    while (op->used != 56) psa_hash_update(op, &zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
    psa_hash_update(op, len_be, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(op->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(op->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(op->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)op->state[i];
    }
    *olen = 32;
    op->active = 0;
    return PSA_SUCCESS;
}

psa_status_t psa_hash_abort(psa_hash_operation_t *op)
{
    memset(op, 0, sizeof *op);
    return PSA_SUCCESS;
}

/* ---- cJSON: a flat object of string members ----------------------------- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static void put_utf8(char **o, unsigned cp)
{
    if (cp < 0x80) *(*o)++ = (char)cp;
    else if (cp < 0x800) { *(*o)++ = (char)(0xC0 | cp >> 6); *(*o)++ = (char)(0x80 | (cp & 0x3F)); }
    else {
        *(*o)++ = (char)(0xE0 | cp >> 12);
        *(*o)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *(*o)++ = (char)(0x80 | (cp & 0x3F));
    }
}

static char *parse_string(const char **pp)
{
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;
    char *out = malloc(strlen(p) * 3 + 1);
    if (out == NULL) return NULL;
    char *o = out;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': *o++ = '\n'; break;
            case 't': *o++ = '\t'; break;
            case 'r': *o++ = '\r'; break;
            case 'b': *o++ = '\b'; break;
            case 'f': *o++ = '\f'; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 1; i <= 4; i++) {
                    char c = p[i];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                    else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') cp |= (unsigned)((c | 0x20) - 'a' + 10);
                    else { free(out); return NULL; }
                }
                put_utf8(&o, cp);
                p += 4;
                break;
            }
            case '\0': free(out); return NULL;
            default: *o++ = *p; break;
            }
            p++;
        } else {
            *o++ = *p++;
        }
    }
    if (*p != '"') { free(out); return NULL; }
    *o = '\0';
    *pp = p + 1;
    return out;
}

cJSON *cJSON_Parse(const char *text)
{
    if (text == NULL) return NULL;
    const char *p = skip_ws(text);
    if (*p++ != '{') return NULL;
    cJSON *root = calloc(1, sizeof *root);
    if (root == NULL) return NULL;
    cJSON **tail = &root->child;
    p = skip_ws(p);
    if (*p == '}') return root;
    for (;;) {
        p = skip_ws(p);
        cJSON *item = calloc(1, sizeof *item);
        if (item == NULL) break;
        *tail = item;
        tail = &item->next;
        item->string = parse_string(&p);
        if (item->string == NULL) break;
        p = skip_ws(p);
        if (*p++ != ':') break;
        p = skip_ws(p);
        if (*p == '"') {
            item->valuestring = parse_string(&p);
            if (item->valuestring == NULL) break;
            item->is_string = 1;
        } else {
            while (*p && *p != ',' && *p != '}') {
                if (*p == '{' || *p == '[' || *p == '"') goto fail;
                p++;
            }
        }
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') {
            p = skip_ws(p + 1);
            if (*p != '\0') break;
            return root;
        }
        break;
    }
fail:
    cJSON_Delete(root);
    return NULL;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *name)
{
    if (object == NULL || name == NULL) return NULL;
    for (cJSON *c = object->child; c != NULL; c = c->next) {
        if (c->string && strcmp(c->string, name) == 0) return c;
    }
    return NULL;
}

bool cJSON_IsString(const cJSON *item) { return item != NULL && item->is_string; }

void cJSON_Delete(cJSON *item)
{
    while (item != NULL) {
        cJSON *next = item->next;
        cJSON_Delete(item->child);
        free(item->string);
        free(item->valuestring);
        free(item);
        item = next;
    }
}
