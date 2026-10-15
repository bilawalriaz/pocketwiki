/* Simulator-wide settings, filled from the command line by sim_main.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char log_level;                 /* E W I D V */
    const char *content_path;       /* built-in content partition image */
    const char *index_path;         /* built-in index partition image */
    uint32_t content_part_size;
    uint32_t index_part_size;
    uint64_t fat_total_bytes;       /* FAT-usable bytes of the packs partition */
    uint32_t fat_cluster;
    bool sta_connected;             /* station uplink up (catalogue sync, installs) */
    unsigned flash_read_us;         /* added latency per raw partition read */
} sim_config_t;

extern sim_config_t g_sim;
