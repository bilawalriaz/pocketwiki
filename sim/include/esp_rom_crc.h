#pragma once
#include <stdint.h>
#include <stddef.h>
/* Same chaining contract as the ROM routine: crc32_le(0, ...) is zlib's crc32. */
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);
