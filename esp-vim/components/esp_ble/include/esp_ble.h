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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char addr[18];              /* "f0:f1:f2:f3:f4:f5" */
    const char *addr_type;      /* "public", "random" */
    char name[32];              /* advertised name, "" if none */
    int rssi;                   /* dBm, strongest seen */
    bool connectable;
    bool hid;                   /* a keyboard, mouse or other HID device */
    uint16_t appearance;        /* GAP appearance, 0 if not advertised */
    char adv[63];               /* the last advertisement's raw bytes, hex */
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

/*
 * Keyboards (Phase 11): BLE HID keyboards bond once and then reconnect by
 * themselves; their keys go to the console through components/esp_kbd.
 */
typedef struct {
    bool connected;
    char addr[18];              /* the keyboard connected, or "" */
    char name[32];
    int battery;                /* percent, or -1 if not reported */
    bool paired;                /* a keyboard is bonded (reconnects at boot) */
    char last_report[64];       /* the last input report, "usage/len: hex" */
    char security[48];          /* the last encryption change: status, encrypted, ... */
} esp_ble_kbd_status_t;

/* At boot: if a keyboard is bonded, start Bluetooth and keep reconnecting to
 * it in the background. Otherwise nothing starts until it's asked for. */
void esp_ble_kbd_boot(void);

/*
 * Connect to the keyboard at {addr} ("f0:f1:f2:f3:f4:f5", as a scan reports it,
 * with its address type) and bond with it ("Just Works": running this is the
 * user's confirmation). Blocks until connected, up to 30 s.
 */
int esp_ble_kbd_pair(const char *addr, bool random, char *err, size_t errlen);

/* Disconnect and forget every bonded keyboard. */
int esp_ble_kbd_forget(char *err, size_t errlen);

void esp_ble_kbd_status(esp_ble_kbd_status_t *st);

#ifdef __cplusplus
}
#endif
