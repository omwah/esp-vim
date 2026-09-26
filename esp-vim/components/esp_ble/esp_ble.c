/*
 * esp_ble: see include/esp_ble.h.
 *
 * NimBLE runs its host in its own task and reports scan results through a GAP
 * callback there. Results are gathered into a table under a mutex and handed to
 * the caller only after the scan ends, on the caller's task -- so the caller
 * (Vim) never runs code on NimBLE's task.
 */

#include "esp_ble.h"
#include "esp_ble_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#define MAX_DEVICES 64

static const char *TAG = "esp_ble";

SemaphoreHandle_t esp_ble__lock;        /* one scan, pairing or reconnect at a time */
#define s_lock esp_ble__lock
static SemaphoreHandle_t s_devs_lock;   /* s_devs: filled on NimBLE's task */
static SemaphoreHandle_t s_synced;      /* host and controller in sync */
static bool s_inited, s_synced_once;
static uint8_t s_own_addr_type;

static esp_ble_dev_t *s_devs;
static int s_ndevs;

static void on_sync(void)
{
    /* The chip's public address, or a random static one when the controller
     * has none (as a virtual controller in the emulator may not). */
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0)
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    xSemaphoreGive(s_synced);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();                  /* returns only at nimble_port_stop() */
    nimble_port_freertos_deinit();
}

bool esp_ble__init_locks(void)
{
    if (s_lock == NULL) {               /* first call, from one task at boot or Vim */
        s_lock = xSemaphoreCreateMutex();
        s_devs_lock = xSemaphoreCreateMutex();
        s_synced = xSemaphoreCreateBinary();
    }
    return s_lock && s_devs_lock && s_synced;
}

/* Start the stack once, on first use. Called with s_lock held. If the
 * controller doesn't answer in time, the next call waits for it again rather
 * than initialising twice. */
int esp_ble__start(char *err, size_t errlen)
{
    if (s_synced_once)
        return 0;
    if (!s_inited) {
        /* Their logs would land in the middle of the editor's screen on the
         * console -- e.g. a scan cancelling a background reconnect is an
         * "error" to them. What matters is in esp_ble_kbd_status(). */
        esp_log_level_set("NimBLE", ESP_LOG_NONE);
        esp_log_level_set("NIMBLE_HIDH", ESP_LOG_NONE);
        esp_log_level_set("ESP_HIDH", ESP_LOG_NONE);
        esp_err_t e = nimble_port_init();   /* also initialises the controller */
        if (e != ESP_OK)
            return snprintf(err, errlen, "Bluetooth start: %s", esp_err_to_name(e)), -1;
        ble_hs_cfg.sync_cb = on_sync;
        ble_hs_cfg.reset_cb = on_reset;
        esp_ble__config_security();      /* bonding, for keyboards (esp_ble_kbd.c) */
        nimble_port_freertos_init(host_task);
        s_inited = true;
    }
    if (xSemaphoreTake(s_synced, pdMS_TO_TICKS(5000)) != pdTRUE)
        return snprintf(err, errlen, "the Bluetooth controller did not respond"), -1;
    s_synced_once = true;
    return 0;
}

uint8_t esp_ble__own_addr_type(void)
{
    return s_own_addr_type;
}

static void fmt_addr(char out[18], const ble_addr_t *a)
{
    const uint8_t *v = a->val;          /* little-endian: print most significant first */
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", v[5], v[4], v[3], v[2], v[1], v[0]);
}

static void record(const struct ble_gap_disc_desc *d)
{
    char addr[18];
    fmt_addr(addr, &d->addr);
    esp_ble_dev_t *dev = NULL;
    for (int i = 0; i < s_ndevs; i++)
        if (strcmp(s_devs[i].addr, addr) == 0)
            dev = &s_devs[i];
    if (dev == NULL) {
        if (s_ndevs == MAX_DEVICES)
            return;
        dev = &s_devs[s_ndevs++];
        memset(dev, 0, sizeof *dev);
        memcpy(dev->addr, addr, sizeof addr);
        dev->addr_type = d->addr.type == BLE_ADDR_PUBLIC ? "public" : "random";
        dev->rssi = d->rssi;
    }
    if (d->rssi > dev->rssi)
        dev->rssi = d->rssi;
    if (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND
            || d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND)
        dev->connectable = true;
    /* The AD structures, parsed here rather than by ble_hs_adv_parse_fields(),
     * which rejects a whole packet over one field it doesn't like. The name
     * may come in the scan response. */
    const uint8_t *a = d->data;
    int len = d->length_data;
    for (int i = 0, h = 0; i < len && h < (int)sizeof dev->adv - 2; i++, h += 2)
        snprintf(dev->adv + h, 3, "%02x", a[i]);
    for (int i = 0; i + 1 < len; ) {
        int n = a[i];                   /* length of type + data */
        if (n == 0 || i + 1 + n > len)
            break;
        uint8_t type = a[i + 1];
        const uint8_t *v = a + i + 2;
        int vlen = n - 1;
        if ((type == 0x08 || type == 0x09) && vlen > 0) {           /* short/complete name */
            size_t m = vlen < (int)sizeof dev->name ? vlen : sizeof dev->name - 1;
            memcpy(dev->name, v, m);
            dev->name[m] = '\0';
        } else if (type == 0x02 || type == 0x03) {                  /* 16-bit service UUIDs */
            for (int k = 0; k + 1 < vlen; k += 2)
                if ((v[k] | v[k + 1] << 8) == 0x1812)               /* HID */
                    dev->hid = true;
        } else if (type == 0x19 && vlen >= 2) {                     /* appearance */
            dev->appearance = v[0] | v[1] << 8;
            if ((dev->appearance & 0xFFC0) == 0x03C0)               /* keyboard, mouse, ... */
                dev->hid = true;
        }
        i += 1 + n;
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    if (ev->type == BLE_GAP_EVENT_DISC) {
        xSemaphoreTake(s_devs_lock, portMAX_DELAY);
        if (s_devs != NULL)
            record(&ev->disc);
        xSemaphoreGive(s_devs_lock);
    }
    return 0;
}

bool esp_ble_available(void)
{
    return true;
}

int esp_ble_scan(unsigned ms, esp_ble_dev_cb cb, void *ctx, char *err, size_t errlen)
{
    if (ms < 1 || ms > 30000)
        return snprintf(err, errlen, "scan time must be 1 ms to 30 s"), -1;
    if (!esp_ble__init_locks())
        return snprintf(err, errlen, "out of memory"), -1;
    esp_ble__cancel_connect();          /* a background reconnect gives way */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = esp_ble__start(err, errlen);
    esp_ble_dev_t *devs = NULL;
    int n = 0;
    if (rc == 0) {
        esp_ble_dev_t *table = calloc(MAX_DEVICES, sizeof *table);
        xSemaphoreTake(s_devs_lock, portMAX_DELAY);
        s_devs = table;
        s_ndevs = 0;
        xSemaphoreGive(s_devs_lock);
        struct ble_gap_disc_params p = { .filter_duplicates = 0, .passive = 0 };
        /* Scan with no time limit, wait here, then cancel: NimBLE's own
         * duration timer never completed a scan in the emulator. */
        int e = table ? ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL)
                       : BLE_HS_ENOMEM;
        if (e != 0) {
            snprintf(err, errlen, "scan: NimBLE error %d", e);
            rc = -1;
        } else {
            vTaskDelay(pdMS_TO_TICKS(ms));
            ble_gap_disc_cancel();
        }
        xSemaphoreTake(s_devs_lock, portMAX_DELAY);
        devs = s_devs;                  /* callbacks run after the locks are released */
        n = s_ndevs;
        s_devs = NULL;
        xSemaphoreGive(s_devs_lock);
    }
    xSemaphoreGive(s_lock);
    for (int i = 0; rc == 0 && i < n; i++)
        if (!cb(ctx, &devs[i]))
            break;
    free(devs);
    return rc;
}

#else   /* no Bluetooth in this build */

bool esp_ble_available(void)
{
    return false;
}

int esp_ble_scan(unsigned ms, esp_ble_dev_cb cb, void *ctx, char *err, size_t errlen)
{
    return snprintf(err, errlen, "this build has no Bluetooth"), -1;
}

#endif
