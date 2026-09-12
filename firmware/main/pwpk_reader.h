#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

typedef struct {
    uint64_t id;
    uint64_t unit_offset;
    uint32_t compressed_size;
    uint32_t raw_offset;
    uint32_t raw_size;
    uint32_t title_offset;
    uint16_t title_size;
    uint32_t crc32;
    uint32_t unit_raw_length;
} pwpk_entry_t;

typedef struct {
    FILE *file;
    uint32_t count, index_offset, index_length, titles_offset, titles_length;
    uint32_t dictionary_offset, dictionary_length, payload_offset, payload_length;
    uint8_t codec;
    uint64_t pack_id;
    bool valid;
} pwpk_reader_t;

esp_err_t pwpk_reader_open(pwpk_reader_t *reader, FILE *file);
/* Reopen a catalogue-selected file after its catalogue CRC check. */
esp_err_t pwpk_reader_open_trusted(pwpk_reader_t *reader, FILE *file);
esp_err_t pwpk_reader_lookup(const pwpk_reader_t *reader, uint64_t id, pwpk_entry_t *entry);
esp_err_t pwpk_reader_read_payload(const pwpk_reader_t *reader, uint64_t offset,
                                   void *buffer, size_t length);
esp_err_t pwpk_reader_read_title(const pwpk_reader_t *reader, const pwpk_entry_t *entry,
                                 char *buffer, size_t capacity, size_t *length);
