/*
 * esp_ble: see include/esp_ble.h.
 *
 * NimBLE runs its host in its own task and reports scan results through a GAP
 * callback there. Results are gathered into a table under a mutex and handed to
 * the caller only after the scan ends, on the caller's task -- so the caller
 * (Vim) never runs code on NimBLE's task.
 */

#include "esp_ble.h"

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

static SemaphoreHandle_t s_lock;        /* one scan at a time */
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

/* Start the stack once, on first use. Called with s_lock held. If the
 * controller doesn't answer in time, the next call waits for it again rather
 * than initialising twice. */
static int start(char *err, size_t errlen)
{
    if (s_synced_once)
        return 0;
    if (!s_inited) {
        /* NimBLE warns that it can't persist keys it doesn't need for
         * scanning; warnings would land on the editor's screen. */
        esp_log_level_set("NimBLE", ESP_LOG_ERROR);
        esp_err_t e = nimble_port_init();   /* also initialises the controller */
        if (e != ESP_OK)
            return snprintf(err, errlen, "Bluetooth start: %s", esp_err_to_name(e)), -1;
        ble_hs_cfg.sync_cb = on_sync;
        ble_hs_cfg.reset_cb = on_reset;
        nimble_port_freertos_init(host_task);
        s_inited = true;
    }
    if (xSemaphoreTake(s_synced, pdMS_TO_TICKS(5000)) != pdTRUE)
        return snprintf(err, errlen, "the Bluetooth controller did not respond"), -1;
    s_synced_once = true;
    return 0;
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
    struct ble_hs_adv_fields f;         /* the name may come in the scan response */
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0
            && f.name != NULL && f.name_len > 0) {
        size_t n = f.name_len < sizeof dev->name ? f.name_len : sizeof dev->name - 1;
        memcpy(dev->name, f.name, n);
        dev->name[n] = '\0';
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
    if (s_lock == NULL) {               /* first call; only the Vim task scans */
        s_lock = xSemaphoreCreateMutex();
        s_devs_lock = xSemaphoreCreateMutex();
        s_synced = xSemaphoreCreateBinary();
        if (!s_lock || !s_devs_lock || !s_synced)
            return snprintf(err, errlen, "out of memory"), -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = start(err, errlen);
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
