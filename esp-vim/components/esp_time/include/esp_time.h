/*
 * esp_time: the date and time.
 *
 * The chip keeps the system time through deep sleep and resets, but not
 * through a loss of power, so it comes from:
 *   - an RTC chip, where the board has one (Kconfig ESP_VIM_RTC: the RLCD-4.2's
 *     PCF85063), read at boot, written whenever the time is set;
 *   - NTP, whenever there is a network -- on by default, and it can be off;
 *   - by hand (esp_time_set).
 * Everything that asks the C library (Vim's strftime(), file times on /fat,
 * the web page's listing) then has the right time. The time zone is a name
 * from a short list or a POSIX TZ string, kept in NVS like the rest.
 *
 * Its own component: the time outlives Vim sessions, and the web server's
 * task sets it too. It knows nothing about Vim.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Settings from NVS, the time zone, the RTC and NTP. Call once at boot, after
 * NVS and esp_net_init(), and before anything writes files. */
esp_err_t esp_time_init(void);

typedef struct {
    bool valid;                 /* the clock has been set */
    const char *source;         /* "rtc", "ntp", "manual", or "" */
    time_t synced;              /* the last NTP sync, 0: none since boot */
    bool ntp;                   /* NTP on */
    char server[64];            /* NTP server */
    char tz[64];                /* the time zone as given: a name or a POSIX string */
    char tz_posix[64];          /* ... as a POSIX string */
    const char *rtc;            /* the board's RTC chip, "" for none */
    bool rtc_ok;                /* it answers */
    time_t rtc_time;            /* its time now; -1: lost (or no chip) */
} esp_time_status_t;
void esp_time_status(esp_time_status_t *st);

/* Set the clock (and the RTC chip). */
int esp_time_set(time_t t, char *err, size_t errlen);

/* ... from a local time in the device's time zone: "YYYY-MM-DD HH:MM[:SS]",
 * or with a "T" between them (an HTML datetime-local field's). */
int esp_time_set_local(const char *s, char *err, size_t errlen);

/* The time zone: one of esp_time_zones(), or a POSIX TZ string. */
int esp_time_set_tz(const char *tz, char *err, size_t errlen);

/* NTP on or off, and its server (NULL or "" leaves it). */
int esp_time_set_ntp(bool on, const char *server, char *err, size_t errlen);

/* Ask the NTP server now and wait for the answer, up to {timeout_s}; works
 * whether NTP is on or not. */
int esp_time_sync(int timeout_s, char *err, size_t errlen);

/* The time zone names esp_time_set_tz() knows, NULL-terminated. */
const char *const *esp_time_zones(void);

/* The RTC chip shares the I2C bus of :EspI2cScan and :EspSensors: whoever
 * builds a bus on those pins takes this first. */
void esp_time_bus_lock(void);
void esp_time_bus_unlock(void);

#ifdef __cplusplus
}
#endif
