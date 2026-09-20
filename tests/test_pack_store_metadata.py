"""Exercise the real pack_store.c against a host filesystem and mocked IDF.

The probe is a counted stub: archive integrity itself is covered elsewhere.
This tests cache trust boundaries, snapshots, overflow, and failed replacement.
"""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_metadata_cache_lifecycle(tmp_path):
    cc = shutil.which("cc")
    if cc is None:
        pytest.skip("no host C compiler")
    headers = {
        "esp_err.h": """#pragma once
 typedef int esp_err_t;
 #define ESP_OK 0
 #define ESP_FAIL -1
 #define ESP_ERR_NO_MEM 1
 #define ESP_ERR_INVALID_STATE 2
 #define ESP_ERR_INVALID_ARG 3
 #define ESP_ERR_NOT_FOUND 4
 """,
        "esp_log.h": "#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n",
        "wear_levelling.h": "#pragma once\ntypedef int wl_handle_t;\n#define WL_INVALID_HANDLE -1\n",
        "esp_vfs_fat.h": """#include <stdbool.h>
 #include <stdint.h>
 #include "esp_err.h"
 #include "wear_levelling.h"
 typedef struct { bool format_if_mount_failed; int max_files;
 int allocation_unit_size; bool disk_status_check_enable; bool use_one_fat;
 } esp_vfs_fat_mount_config_t;
 static inline int esp_vfs_fat_spiflash_mount_rw_wl(const char *a, const char *b,
 const esp_vfs_fat_mount_config_t *c, wl_handle_t *d) { return 0; }
 static inline int esp_vfs_fat_info(const char *a, uint64_t *t, uint64_t *u)
 { *t=1000000; *u=900000; return 0; }
 """,
        "freertos/FreeRTOS.h": "#define portMAX_DELAY 0\n#define pdTRUE 1\n",
        "freertos/semphr.h": """#include <pthread.h>
 typedef pthread_mutex_t *SemaphoreHandle_t;
 static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
 static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &test_mutex; }
 static inline int xSemaphoreTake(SemaphoreHandle_t m, int t)
 { return pthread_mutex_lock(m) == 0; }
 static inline void xSemaphoreGive(SemaphoreHandle_t m) { pthread_mutex_unlock(m); }
 """,
    }
    for name, body in headers.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)
    store = tmp_path / "packs"
    store.mkdir()
    exe = tmp_path / "metadata-test"
    cmd = [cc, "-std=c11", "-D_DEFAULT_SOURCE", "-pthread",
           '-DPW_PACK_DIR="./packs"', "-I", str(tmp_path),
           "-I", str(ROOT / "firmware/main"),
           str(ROOT / "tests/c_pack_store_metadata_harness.c"), "-o", str(exe)]
    built = subprocess.run(cmd, capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30, cwd=tmp_path)
    assert ran.returncode == 0, ran.stdout + ran.stderr
