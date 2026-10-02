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
 *   - WiFi station: the chip's own radio (ESP32-S3), or on the ESP32-P4 an
 *     ESP32-C6 co-processor reached through esp_wifi_remote + esp-hosted, which
 *     re-export the same esp_wifi_* API -- so both use one code path here.
 *   - none.
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
    char iface[12];             /* "wifi", "ethernet", "none" */
    char ip[16], netmask[16], gw[16], dns[16];
    char mac[18];
    char ssid[33];              /* WiFi: the network joined (or being joined) */
    int rssi;                   /* WiFi: signal, dBm; 0 when not connected */
} esp_net_status_t;

typedef struct {
    char ssid[33];
    int rssi;
    int channel;
    const char *auth;           /* "open", "wpa2", "wpa3", ... */
} esp_net_ap_t;

typedef bool (*esp_net_ap_cb)(void *ctx, const esp_net_ap_t *ap);

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

/* Sleep: WiFi's radio off; resume turns it on and rejoins the network it was
 * on. Nothing to do without WiFi, or when it was never started. */
void esp_net_suspend(void);
void esp_net_resume(void);

/* WiFi's modem sleep: the radio dozes between the access point's beacons,
 * which saves power and costs every reply up to a beacon interval (on the
 * Tab5, pings of 60-140 ms). On by default; esp_power turns it off while
 * USB powers the board. Kept, and applied whenever WiFi starts. */
void esp_net_power_save(bool on);

/* WiFi builds only (else -1, "no WiFi"). Scan blocks for about two seconds.
 * Connect stores the network (NVS), to be joined again at every boot, and
 * returns at once; watch the status. A NULL password rejoins the stored
 * network with its stored password; ssid is then that network's name, or NULL.
 * Disconnect leaves the network until the next connect or boot; forget also
 * erases it. Saved gives the stored network's name ("" if none), never its
 * password. */
int esp_net_wifi_scan(esp_net_ap_cb cb, void *ctx, char *err, size_t errlen);
int esp_net_wifi_connect(const char *ssid, const char *password, char *err, size_t errlen);
int esp_net_wifi_disconnect(char *err, size_t errlen);
int esp_net_wifi_forget(char *err, size_t errlen);
int esp_net_wifi_saved(char *ssid, size_t n, char *err, size_t errlen);

/*
 * The radio co-processor: the Tab5's ESP32-C6, reached through esp-hosted
 * (ESP_VIM_NET_WIFI_REMOTE builds; others have none: present false).
 */
typedef struct {
    bool present;               /* this build talks to one */
    bool link;                  /* it answered */
    char host[16];              /* esp-hosted on this side, "2.12.13" */
    char version[16];           /* esp-hosted on the co-processor */
    char chip[16];              /* "esp32c6" */
} esp_net_cp_info_t;

typedef struct {
    char from[16], to[32];      /* its firmware before; the image's version */
    char project[32];           /* the image's project, "network_adapter" */
    uint64_t bytes;             /* sent */
    bool activated;             /* it boots the new firmware now (else at its next reset) */
} esp_net_cp_update_t;

/* Called after each chunk sent; return false to stop. */
typedef bool (*esp_net_cp_progress_cb)(void *ctx, uint64_t bytes, uint64_t total);

/* Brings the link up if it isn't (esp-hosted resets the co-processor and
 * waits for it). -1 with {err} if it doesn't answer. */
int esp_net_cp_info(esp_net_cp_info_t *info, char *err, size_t errlen);

/* The co-processor stopped answering: esp-hosted (patched) marked its link
 * failed instead of restarting the board, and it stays so until a restart.
 * Nothing that needs it is tried meanwhile. False without one. */
bool esp_net_cp_failed(void);

/* Update its firmware from {path}, an ESP32-C6 app image (the co-processor
 * build's network_adapter.bin; validated by esp_fs_check). Blocks for the
 * transfer. The link doesn't survive the co-processor restarting into the
 * new firmware: restart this chip afterwards. */
int esp_net_cp_update(const char *path, esp_net_cp_progress_cb progress, void *ctx,
                      esp_net_cp_update_t *out, char *err, size_t errlen);

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
