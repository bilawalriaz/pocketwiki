#pragma once
/* The slice of cJSON the firmware uses: a flat object of string members. */
#include <stdbool.h>
typedef struct cJSON {
    struct cJSON *next;
    char *string;
    char *valuestring;
    int is_string;
    struct cJSON *child;
} cJSON;
cJSON *cJSON_Parse(const char *text);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *name);
bool cJSON_IsString(const cJSON *item);
void cJSON_Delete(cJSON *item);
