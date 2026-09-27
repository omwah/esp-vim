/*
 * esp_power: the battery, and sleep.
 *
 * Two kinds of sleep:
 *   reader mode  light sleep. RAM is kept, so Vim carries on exactly where it
 *                was; a reflective screen keeps showing it, with a small badge.
 *                Only with a wake button (Kconfig ESP_VIM_WAKE_KEY_GPIO): it
 *                wakes the board, and pressed while awake puts it to sleep.
 *   deep sleep   the chip stops; waking is a fresh boot. A reflective screen
 *                shows a sleep screen meanwhile (/fat/sleep.pbm, or a hippo).
 *                The wake button wakes it, or the reset button, or a timer.
 *
 * On battery the board goes to reader mode by itself after a while with no
 * input -- only while Vim is waiting for a key, never in the middle of a
 * command -- and from there to deep sleep after longer, if nothing is unsaved,
 * or when the battery runs low. "On battery" is: no computer on the USB port
 * (the S3's USB Serial/JTAG sees none), and a battery to measure.
 *
 * Its own component, like esp_net: it outlives Vim sessions and knows nothing
 * about Vim, which tells it what it needs to (esp_power_vim_waiting).
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The battery ADC, the wake button and the idle timer. Call once at boot,
 * after NVS and the display. */
esp_err_t esp_power_init(void);

typedef struct {
    const char *source;         /* "usb", "battery" or "unknown" */
    bool battery;               /* the board has a battery input */
    int battery_mv;             /* -1: no battery measured */
    int battery_pct;            /* rough, from the voltage; -1 as above */
    bool reader;                /* reader mode available (a wake button) */
    int idle_min;               /* minutes idle on battery before reader mode, 0: never */
    int deep_min;               /* minutes in reader mode before deep sleep, 0: never */
    int idle_s;                 /* seconds since the last key */
    unsigned sleeps;            /* reader-mode sleeps since boot */
    unsigned presses;           /* wake button presses seen while awake */
    unsigned deep_sleeps;       /* deep sleeps, kept across them */
    const char *last_wake;      /* "key", "timer", "" */
} esp_power_status_t;
void esp_power_status(esp_power_status_t *st);

/* Settings, kept in NVS; negative leaves one as it is. */
void esp_power_set(int idle_min, int deep_min);

/*
 * Sleep now. Light: reader mode, returning on the wake button (or after
 * {wake_s} seconds, if not 0). Deep: doesn't return; waking is a reboot.
 * {unsaved}: something would be lost by a deep sleep -- reader mode then won't
 * turn into one. From any task; a sleep already under way makes it return
 * ESP_ERR_INVALID_STATE, and light sleep without a wake button (or timer)
 * ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t esp_power_sleep(bool deep, int wake_s, bool unsaved);

/* From the console: a key arrived (any source). */
void esp_power_input(void);

/* From Vim: it starts or stops waiting for a key -- the only time it may be
 * put to sleep -- and whether a buffer has unsaved changes. */
void esp_power_vim_waiting(bool waiting, bool unsaved);

#ifdef __cplusplus
}
#endif
