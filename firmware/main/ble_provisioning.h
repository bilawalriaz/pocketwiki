#pragma once

#include "esp_err.h"

/* GATT service for Wi-Fi provisioning and complete pack management. Pack
 * uploads are offset-framed, CRC-checked, validated, and atomically renamed. */
esp_err_t ble_provisioning_init(void);
/* The C3 has enough heap for either the BLE controller or a full TLS record,
 * but not both while a web download is active. Suspend and resume are used
 * around station-mode HTTPS transfers; a connected BLE client is rejected. */
esp_err_t ble_provisioning_suspend(void);
esp_err_t ble_provisioning_resume(void);
