#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct {
 const char *name;
 const uint8_t *data;
 size_t compressed, raw;
 const uint8_t *dictionary;
 size_t dictionary_size;
 uint32_t crc32;
} pwpk_serial_fixture_t;
extern const pwpk_serial_fixture_t pwpk_serial_fixtures[];
extern const size_t pwpk_serial_fixture_count;
void pwpk_serial_benchmark_start(void);
