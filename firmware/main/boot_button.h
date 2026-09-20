/* Boot-button screen cycle.
 *
 * With an OLED attached, each short press of the BOOT button advances the
 * display one step: live status -> Wi-Fi join QR -> reader address QR ->
 * live status. The join symbol carries the "WIFI:" payload that phones offer
 * to connect with, so a user can reach the reader without typing a password;
 * the reader symbol opens http://<AP IP>/ directly.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the button GPIO and start the polling task. Returns an error when
 * the feature is disabled, the pin collides with the OLED I2C bus, or the
 * task cannot be created. */
esp_err_t boot_button_init(void);

#ifdef __cplusplus
}
#endif
