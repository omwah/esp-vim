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
 * themselves; their keys go to the console through components/esp_kbd. Up to
 * four can be bonded; one is connected at a time (each connection costs the
 * S3 internal RAM), the last one used tried first. Each keyboard's reports
 * are decoded from its own report map, so N-key rollover reports work too.
 */
typedef struct {
    bool connected;
    char addr[18];              /* the keyboard connected, or "" */
    char name[32];
    int battery;                /* percent, or -1 if not reported */
    bool paired;                /* a keyboard is bonded (reconnects at boot) */
    char last_report[64];       /* the last input report, "usage/len: hex" */
    char security[48];          /* the last encryption change: status, encrypted, ... */
    char layout[96];            /* its keyboard reports, as esp_kbd_describe() has them */
    int bonds;                  /* keyboards bonded (-1: Bluetooth not started) */
} esp_ble_kbd_status_t;

/* A bonded keyboard. */
typedef struct {
    char addr[18];              /* its identity address */
    bool random;                /* the address type */
    char name[32];              /* as it was when last connected, "" if never */
    bool connected;
    bool last;                  /* the one connected last: tried first */
} esp_ble_bond_t;

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

/* Forget one ({addr}, as esp_ble_kbd_bonds() gives it), disconnecting it if it
 * is the one connected. */
int esp_ble_kbd_forget_one(const char *addr, char *err, size_t errlen);

/* The bonded keyboards, the last one used first; at most {max}. 0 when
 * Bluetooth hasn't started (it starts at boot when a keyboard is bonded). */
int esp_ble_kbd_bonds(esp_ble_bond_t *out, int max);

/* The last input reports (at most 16, oldest first), as
 * "map.id/len: hex bytes", for seeing what a keyboard sends. */
int esp_ble_kbd_reports(char (*out)[72], int max);

/* "Paired: <name>", once, after a pairing (by command or on the touch
 * screen) connects. False when there is none. */
bool esp_ble_kbd_notice(char *out, size_t len);

void esp_ble_kbd_status(esp_ble_kbd_status_t *st);

/* Sleep: drop the keyboard's connection and stop reconnecting, so the radio
 * is quiet; resume reconnects at once. Any task; suspend waits up to 2 s. */
void esp_ble_kbd_suspend(void);
void esp_ble_kbd_resume(void);

#ifdef __cplusplus
}
#endif
