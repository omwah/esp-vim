/*
 * esp_time: see include/esp_time.h.
 */

#include "esp_time.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "esp_touch.h"

static const char *TAG __attribute__((unused)) = "esp_time";

#define NVS_NS      "esp_time"
#define VALID_AFTER 1735689600          /* 2025-01-01: anything earlier was never set */

static SemaphoreHandle_t s_bus_lock;
static SemaphoreHandle_t s_lock;        /* the settings, from Vim's task or the web server's */
static bool s_ntp = true;
static char s_server[64] = CONFIG_ESP_VIM_NTP_SERVER;
static char s_tz[64] = CONFIG_ESP_VIM_TZ;
static char s_tz_posix[64];
static bool s_sntp_running;
static time_t s_synced;
static bool s_rtc_ok;
/* Where the time came from; kept through deep sleep and resets, like the time. */
static RTC_NOINIT_ATTR char s_source[8];

static void set_source(const char *src)
{
    snprintf(s_source, sizeof s_source, "%s", src);
}

/* ---------------------------------------------------------- time zones -- */

/* Names for the POSIX strings, which is all newlib understands: no tz
 * database on the device. Daylight saving rules as of 2026. */
static const char *const s_zones[][2] = {
    { "UTC",                 "UTC0" },
    { "US/Eastern",          "EST5EDT,M3.2.0,M11.1.0" },
    { "US/Central",          "CST6CDT,M3.2.0,M11.1.0" },
    { "US/Mountain",         "MST7MDT,M3.2.0,M11.1.0" },
    { "US/Arizona",          "MST7" },
    { "US/Pacific",          "PST8PDT,M3.2.0,M11.1.0" },
    { "US/Alaska",           "AKST9AKDT,M3.2.0,M11.1.0" },
    { "US/Hawaii",           "HST10" },
    { "America/New_York",    "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Chicago",     "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver",      "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Phoenix",     "MST7" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Anchorage",   "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Toronto",     "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Vancouver",   "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Mexico_City", "CST6" },
    { "America/Sao_Paulo",   "<-03>3" },
    { "Europe/London",       "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Dublin",       "IST-1GMT0,M10.5.0,M3.5.0/1" },
    { "Europe/Lisbon",       "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Europe/Paris",        "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Berlin",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Madrid",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome",         "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Amsterdam",    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Stockholm",    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Athens",       "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Helsinki",     "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Moscow",       "MSK-3" },
    { "Africa/Johannesburg", "SAST-2" },
    { "Asia/Dubai",          "<+04>-4" },
    { "Asia/Kolkata",        "IST-5:30" },
    { "Asia/Bangkok",        "<+07>-7" },
    { "Asia/Singapore",      "<+08>-8" },
    { "Asia/Shanghai",       "CST-8" },
    { "Asia/Hong_Kong",      "HKT-8" },
    { "Asia/Taipei",         "CST-8" },
    { "Asia/Seoul",          "KST-9" },
    { "Asia/Tokyo",          "JST-9" },
    { "Australia/Perth",     "AWST-8" },
    { "Australia/Brisbane",  "AEST-10" },
    { "Australia/Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Pacific/Auckland",    "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
#define NZONES (sizeof s_zones / sizeof s_zones[0])

const char *const *esp_time_zones(void)
{
    static const char *names[NZONES + 1];
    for (size_t i = 0; i < NZONES; i++)
        names[i] = s_zones[i][0];
    names[NZONES] = NULL;
    return names;
}

/* A zone name's POSIX string; a POSIX string as it is; NULL for neither. A
 * POSIX string is a name of 3+ letters (or <+03>-style), then an offset. */
static const char *posix_tz(const char *tz)
{
    for (size_t i = 0; i < NZONES; i++)
        if (strcasecmp(tz, s_zones[i][0]) == 0)
            return s_zones[i][1];
    const char *p = tz;
    if (*p == '<') {
        p = strchr(p, '>');
        if (p == NULL || p - tz < 2)
            return NULL;
        p++;
    } else {
        while (isalpha((unsigned char)*p))
            p++;
        if (p - tz < 3)
            return NULL;
    }
    if (*p == '+' || *p == '-')
        p++;
    return isdigit((unsigned char)*p) && strlen(tz) < sizeof s_tz_posix ? tz : NULL;
}

static void apply_tz(void)
{
    const char *p = posix_tz(s_tz);
    snprintf(s_tz_posix, sizeof s_tz_posix, "%s", p ? p : "UTC0");
    setenv("TZ", s_tz_posix, 1);
    tzset();
}

/* ------------------------------------------------------------ RTC chip -- */

void esp_time_bus_lock(void)
{
    if (s_bus_lock)
        xSemaphoreTake(s_bus_lock, portMAX_DELAY);
}

void esp_time_bus_unlock(void)
{
    if (s_bus_lock)
        xSemaphoreGive(s_bus_lock);
}

#if CONFIG_ESP_VIM_RTC_PCF85063

#define RTC_NAME    "PCF85063"
#define RTC_ADDR    0x51
#define RTC_SECONDS 0x04                /* then minutes, hours, days, weekdays, months, years */
#define OS_FLAG     0x80                /* in seconds: the oscillator stopped, the time is lost */

/* One transfer with the chip at {reg}: on the touch panel's bus if it has
 * these pins, else on a bus built for it, as :EspI2cScan does. */
static esp_err_t rtc_xfer(uint8_t reg, uint8_t *buf, size_t n, bool write)
{
    esp_time_bus_lock();
    i2c_master_bus_handle_t bus = esp_touch_i2c_bus(CONFIG_ESP_VIM_I2C_SDA, CONFIG_ESP_VIM_I2C_SCL);
    bool own = bus == NULL;
    esp_err_t e = ESP_OK;
    if (own) {
        i2c_master_bus_config_t bc = {
            .i2c_port = -1,
            .sda_io_num = CONFIG_ESP_VIM_I2C_SDA,
            .scl_io_num = CONFIG_ESP_VIM_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        e = i2c_new_master_bus(&bc, &bus);
    }
    i2c_master_dev_handle_t dev = NULL;
    if (e == ESP_OK) {
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                   .device_address = RTC_ADDR, .scl_speed_hz = 100000 };
        e = i2c_master_bus_add_device(bus, &dc, &dev);
    }
    if (e == ESP_OK) {
        if (write) {
            uint8_t b[16];
            b[0] = reg;
            memcpy(b + 1, buf, n);
            e = i2c_master_transmit(dev, b, n + 1, 100);
        } else {
            e = i2c_master_transmit_receive(dev, &reg, 1, buf, n, 100);
        }
    }
    if (dev)
        i2c_master_bus_rm_device(dev);
    if (own && bus)
        i2c_del_master_bus(bus);
    esp_time_bus_unlock();
    return e;
}

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t to_bcd(int v) { return (uint8_t)((v / 10) << 4 | v % 10); }

/* Days since 1970-01-01 of a civil date (proleptic Gregorian). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* The chip's time (kept in UTC), or -1: no answer, or it stopped. */
static time_t rtc_read(void)
{
    uint8_t r[7];
    if (rtc_xfer(RTC_SECONDS, r, sizeof r, false) != ESP_OK)
        return -1;
    s_rtc_ok = true;
    if (r[0] & OS_FLAG)
        return -1;
    int64_t days = days_from_civil(2000 + bcd(r[6]), bcd(r[5] & 0x1F), bcd(r[3] & 0x3F));
    return (time_t)(days * 86400 + bcd(r[2] & 0x3F) * 3600 + bcd(r[1] & 0x7F) * 60 + bcd(r[0] & 0x7F));
}

static void rtc_write(time_t t)
{
    struct tm u;
    gmtime_r(&t, &u);
    uint8_t r[7] = {
        to_bcd(u.tm_sec),               /* and OS clear: the time is good again */
        to_bcd(u.tm_min), to_bcd(u.tm_hour), to_bcd(u.tm_mday), (uint8_t)u.tm_wday,
        to_bcd(u.tm_mon + 1), to_bcd(u.tm_year % 100),
    };
    esp_err_t e = rtc_xfer(RTC_SECONDS, r, sizeof r, true);
    s_rtc_ok = e == ESP_OK;
    if (e != ESP_OK)
        ESP_LOGW(TAG, RTC_NAME ": %s", esp_err_to_name(e));
}

#else

#define RTC_NAME    ""
static time_t rtc_read(void) { return -1; }
static void rtc_write(time_t t) { (void)t; }

#endif

/* NTP's answer comes on lwIP's task; the chip is written from the timer
 * task, not to hold up the network while the bus is busy. */
static void rtc_write_now(void *arg, uint32_t unused)
{
    rtc_write(time(NULL));
}

/* ----------------------------------------------------------------- NTP -- */

static void on_sync(struct timeval *tv)
{
    s_synced = tv->tv_sec;
    set_source("ntp");
    xTimerPendFunctionCall(rtc_write_now, NULL, 0, 0);
}

static void sntp_start(void)
{
    if (s_sntp_running)
        return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_server);
    cfg.sync_cb = on_sync;
    if (esp_netif_sntp_init(&cfg) == ESP_OK)
        s_sntp_running = true;
}

static void sntp_stop(void)
{
    if (!s_sntp_running)
        return;
    esp_netif_sntp_deinit();
    s_sntp_running = false;
}

/* ---------------------------------------------------------------- API -- */

static void save_str(const char *key, const char *v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void esp_time_status(esp_time_status_t *st)
{
    memset(st, 0, sizeof *st);
    st->valid = time(NULL) >= VALID_AFTER;
    st->source = st->valid ? s_source : "";
    st->synced = s_synced;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st->ntp = s_ntp;
    snprintf(st->server, sizeof st->server, "%s", s_server);
    snprintf(st->tz, sizeof st->tz, "%s", s_tz);
    snprintf(st->tz_posix, sizeof st->tz_posix, "%s", s_tz_posix);
    xSemaphoreGive(s_lock);
    st->rtc = RTC_NAME;
    st->rtc_time = rtc_read();          /* sets rtc_ok as it goes */
    st->rtc_ok = s_rtc_ok;
}

int esp_time_set(time_t t, char *err, size_t errlen)
{
    if (t < VALID_AFTER)
        return snprintf(err, errlen, "that date is before 2025"), -1;
    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);
    set_source("manual");
    rtc_write(t);
    return 0;
}

int esp_time_set_local(const char *s, char *err, size_t errlen)
{
    struct tm tm = {0};
    char sep;
    int n = sscanf(s, "%d-%d-%d%c%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &sep,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    if (n < 6 || (sep != ' ' && sep != 'T') || tm.tm_mon < 1 || tm.tm_mon > 12
            || tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 60)
        return snprintf(err, errlen, "expected \"YYYY-MM-DD HH:MM[:SS]\" (local time), not \"%s\"", s), -1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;                   /* the zone's rules decide */
    return esp_time_set(mktime(&tm), err, errlen);
}

int esp_time_set_tz(const char *tz, char *err, size_t errlen)
{
    if (posix_tz(tz) == NULL)
        return snprintf(err, errlen, "unknown time zone \"%s\": a name (US/Pacific, "
                        "Europe/Paris, ...) or a POSIX TZ string (PST8PDT,M3.2.0,M11.1.0)", tz), -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_tz, sizeof s_tz, "%s", tz);
    apply_tz();
    xSemaphoreGive(s_lock);
    save_str("tz", tz);
    return 0;
}

int esp_time_set_ntp(bool on, const char *server, char *err, size_t errlen)
{
    if (server && strlen(server) >= sizeof s_server)
        return snprintf(err, errlen, "server name too long"), -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool restart = server && *server && strcmp(server, s_server) != 0;
    if (restart) {
        snprintf(s_server, sizeof s_server, "%s", server);
        save_str("server", server);
        sntp_stop();
    }
    s_ntp = on;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ntp", on);
        nvs_commit(h);
        nvs_close(h);
    }
    if (on)
        sntp_start();
    else
        sntp_stop();
    xSemaphoreGive(s_lock);
    return 0;
}

int esp_time_sync(int timeout_s, char *err, size_t errlen)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_sntp_running;
    if (was)
        esp_netif_sntp_start();         /* ask again now (on lwIP's task) */
    else
        sntp_start();
    xSemaphoreGive(s_lock);
    esp_err_t e = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_s * 1000));
    if (!was) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_ntp)
            sntp_stop();                /* just the once */
        xSemaphoreGive(s_lock);
    }
    if (e != ESP_OK)
        return snprintf(err, errlen, "no answer from %s within %d s (is the network up?)",
                        s_server, timeout_s), -1;
    return 0;
}

esp_err_t esp_time_init(void)
{
    s_bus_lock = xSemaphoreCreateMutex();
    s_lock = xSemaphoreCreateMutex();
    if (s_bus_lock == NULL || s_lock == NULL)
        return ESP_ERR_NO_MEM;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof s_tz;
        nvs_get_str(h, "tz", s_tz, &n);
        n = sizeof s_server;
        nvs_get_str(h, "server", s_server, &n);
        uint8_t on = 1;
        if (nvs_get_u8(h, "ntp", &on) == ESP_OK)
            s_ntp = on;
        nvs_close(h);
    }
    apply_tz();

    /* The system time survives deep sleep and resets; after a power-up it's
     * 1970, and the RTC chip has it, if it kept running. */
    if (time(NULL) < VALID_AFTER) {
        set_source("");
        time_t t = rtc_read();
        if (t >= VALID_AFTER) {
            struct timeval tv = { .tv_sec = t };
            settimeofday(&tv, NULL);
            set_source("rtc");
        }
    } else {
        rtc_read();                     /* only to learn whether it answers */
        if (strcmp(s_source, "rtc") && strcmp(s_source, "ntp") && strcmp(s_source, "manual"))
            set_source("");             /* RTC_NOINIT: not one we wrote */
    }
    if (s_ntp)
        sntp_start();                   /* it waits for the network by itself */
    return ESP_OK;
}
