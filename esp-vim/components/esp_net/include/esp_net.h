/*
 * esp_net: the device's network interface and its HTTP(S) client.
 *
 * Like esp_fs, this is its own component rather than part of the Vim one: the
 * interface lives for the whole boot (across Vim session restarts), and the web
 * server (Phase 6e) will use it from another task. It knows nothing about Vim.
 *
 * Interfaces, chosen in menuconfig ("esp-vim network"):
 *   - Ethernet: the chip's internal EMAC with a generic IEEE 802.3 PHY. On the
 *     ESP32-P4 this is also what esp-emu models, so networking is testable in
 *     the emulator with --net user (the host is reachable at the gateway).
 *   - none.
 * WiFi (native on the S3, through the C6 on the Tab5) is Phase 6f.
 *
 * Downloads write files only through esp_fs's path validation.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool up;                    /* link up and an IPv4 address assigned */
    char iface[12];             /* "ethernet", "none" */
    char ip[16], netmask[16], gw[16], dns[16];
    char mac[18];
} esp_net_status_t;

typedef struct {
    int status;                 /* HTTP status of the final response */
    uint64_t size;              /* body bytes received */
} esp_net_http_result_t;

/* Called during a transfer with the bytes so far; return false to stop. */
typedef bool (*esp_net_progress_cb)(void *ctx, uint64_t bytes);

/* Receives the body in chunks; return false to stop. */
typedef bool (*esp_net_sink_cb)(void *ctx, const char *data, size_t len);

/* Bring up the configured interface. Returns at once; DHCP continues in the
 * background. Call once at boot. */
esp_err_t esp_net_init(void);

void esp_net_get_status(esp_net_status_t *status);

/*
 * GET {url} (http or https; certificates checked against ESP-IDF's CA bundle;
 * up to 5 redirects followed), passing the body to {sink}. Only a 200 response
 * is a success. Returns 0, or -1 with a message in {err}.
 */
int esp_net_http_fetch(const char *url, esp_net_sink_cb sink, void *sink_ctx,
                       esp_net_progress_cb progress, void *progress_ctx,
                       esp_net_http_result_t *result, char *err, size_t errlen);

/*
 * GET {url} into the file {dest} (absolute; validated by esp_fs_check). The
 * body goes to "{dest}.part" first and replaces {dest} only when complete, so
 * an interrupted download never leaves a truncated file.
 */
int esp_net_http_download(const char *url, const char *dest,
                          esp_net_progress_cb progress, void *progress_ctx,
                          esp_net_http_result_t *result, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
