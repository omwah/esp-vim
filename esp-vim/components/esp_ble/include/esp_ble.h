/*
 * esp_ble: Bluetooth LE for :EspBleScan (and, in Phase 11, keyboards).
 *
 * Its own component, like esp_net: the Bluetooth stack outlives Vim sessions,
 * and knows nothing about Vim. NimBLE is the host. The controller is the chip's
 * own (ESP32-S3); builds without Bluetooth get errors from every call.
 *
 * The stack starts on first use, not at boot: Bluetooth costs RAM that an
 * editor session that never scans shouldn't pay.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char addr[18];              /* "f0:f1:f2:f3:f4:f5" */
    const char *addr_type;      /* "public", "random" */
    char name[32];              /* advertised name, "" if none */
    int rssi;                   /* dBm, strongest seen */
    bool connectable;
} esp_ble_dev_t;

/* Receives each device found; return false to stop. */
typedef bool (*esp_ble_dev_cb)(void *ctx, const esp_ble_dev_t *dev);

/* True when this build has Bluetooth. */
bool esp_ble_available(void);

/*
 * Scan for {ms} milliseconds (1 to 30000), blocking, then pass each device
 * found to {cb} on the calling task, at most once per address. Returns 0, or
 * -1 with a message in {err}.
 */
int esp_ble_scan(unsigned ms, esp_ble_dev_cb cb, void *ctx, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
