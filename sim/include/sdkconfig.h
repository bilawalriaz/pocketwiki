/* Simulator Kconfig: mirrors the defaults in firmware/main/Kconfig.projbuild
 * and firmware/sdkconfig.defaults. Override any value with -D on the compiler
 * line (tools/pwsim.py does this per target). */
#pragma once

#define PW_SIM 1

#ifndef CONFIG_POCKETWIKI_AP_SSID
#define CONFIG_POCKETWIKI_AP_SSID "PocketWiki"
#endif
#ifndef CONFIG_POCKETWIKI_AP_PASSWORD
#define CONFIG_POCKETWIKI_AP_PASSWORD ""
#endif
#ifndef CONFIG_POCKETWIKI_AP_IP
#define CONFIG_POCKETWIKI_AP_IP "192.168.4.1"
#endif
#ifndef CONFIG_POCKETWIKI_HOSTNAME
#define CONFIG_POCKETWIKI_HOSTNAME "pocketwiki"
#endif
#ifndef CONFIG_POCKETWIKI_HTTPD_MAX_SOCKETS
#define CONFIG_POCKETWIKI_HTTPD_MAX_SOCKETS 7
#endif
#ifndef CONFIG_POCKETWIKI_PACK_MAX_UPLOAD_KB
#define CONFIG_POCKETWIKI_PACK_MAX_UPLOAD_KB 2048
#endif
#ifndef CONFIG_POCKETWIKI_PACK_INDEX_RAM_CACHE
#define CONFIG_POCKETWIKI_PACK_INDEX_RAM_CACHE 0
#endif
/* esp_http_server Kconfig defaults (IDF 5.x). */
#ifndef CONFIG_HTTPD_MAX_URI_LEN
#define CONFIG_HTTPD_MAX_URI_LEN 512
#endif
#ifndef CONFIG_HTTPD_MAX_REQ_HDR_LEN
#define CONFIG_HTTPD_MAX_REQ_HDR_LEN 1024
#endif
#ifndef CONFIG_POCKETWIKI_BLE_ENABLE
#define CONFIG_POCKETWIKI_BLE_ENABLE 1
#endif
