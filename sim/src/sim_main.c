/* PocketWiki device simulator.
 *
 * Boots the real firmware modules - archive validation, pack store, catalogue
 * reader and web server - on the host, in the order app_main.c does, with the
 * built-in archive read from partition images and the pack store in a
 * directory. Everything the firmware decides is decided by firmware code;
 * only the radio, flash driver and HTTP transport are stand-ins.
 *
 *   pwsim --data DIR --content content.part --index index.part [--port N]
 *         [--target c3|s3] [--no-uplink] [--log E|W|I|D] [--flash-read-us N]
 *
 * DIR holds the pack store (DIR/packs). Prints "PWSIM READY port=<n>" once
 * the server is listening, which is what tools/pwsim.py and the tests wait for.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "content_archive.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "pack_store.h"
#include "sim.h"
#include "web_server.h"

static const char *TAG = "pwsim";

static void usage(void)
{
    fprintf(stderr,
            "usage: pwsim --data DIR --content FILE --index FILE [--port N]\n"
            "             [--target c3|s3] [--no-uplink] [--log E|W|I|D]\n"
            "             [--flash-read-us N]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *data = NULL;
    int port = 8080;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--data") == 0 && v) { data = v; i++; }
        else if (strcmp(a, "--content") == 0 && v) { g_sim.content_path = realpath(v, NULL); i++; }
        else if (strcmp(a, "--index") == 0 && v) { g_sim.index_path = realpath(v, NULL); i++; }
        else if (strcmp(a, "--port") == 0 && v) { port = atoi(v); i++; }
        else if (strcmp(a, "--log") == 0 && v) { g_sim.log_level = v[0]; i++; }
        else if (strcmp(a, "--flash-read-us") == 0 && v) { g_sim.flash_read_us = (unsigned)atoi(v); i++; }
        else if (strcmp(a, "--no-uplink") == 0) { g_sim.sta_connected = false; }
        else if (strcmp(a, "--target") == 0 && v) {
            if (strcmp(v, "s3") == 0) {
                /* partitions_16mb.csv */
                g_sim.fat_total_bytes = 14823424;
            } else if (strcmp(v, "c3") != 0) {
                usage();
            }
            i++;
        } else {
            usage();
        }
    }
    if (data == NULL || g_sim.content_path == NULL || g_sim.index_path == NULL) usage();
    if (chdir(data) != 0) {
        perror(data);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* lwIP reports a closed peer as a send error; POSIX raises SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    /* app_main.c order: archive, pack store, web server. A bad built-in
     * archive is not fatal: the diagnostic routes still run. */
    esp_err_t err = ca_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "built-in archive invalid: %s", esp_err_to_name(err));
    err = pack_store_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pack store init failed: %s", esp_err_to_name(err));
        return 1;
    }
    sim_httpd_set_port((uint16_t)port);
    err = web_server_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web server init failed: %s", esp_err_to_name(err));
        return 1;
    }
    printf("PWSIM READY port=%u\n", sim_httpd_bound_port());
    for (;;) pause();
}
