/*
 * esp_ble keyboards: see include/esp_ble.h.
 *
 * ESP-IDF's HID host (esp_hidh, NimBLE back end) does the connection, GATT
 * discovery and report parsing, and asks for encryption as soon as it
 * connects; bonding happens then, with the keys kept in NVS by NimBLE's store.
 * Input reports from a keyboard go to components/esp_kbd.
 *
 * A background task keeps reconnecting to the bonded keyboard: a BLE keyboard
 * that wakes up advertises, and the host has to connect to it. It holds
 * esp_ble__lock while it tries, and gives way when a scan or a pairing
 * cancels the attempt (esp_ble__cancel_connect).
 */

#include "esp_ble.h"
#include "esp_ble_priv.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED && CONFIG_BT_NIMBLE_ROLE_CENTRAL

#include "esp_hidh.h"
#include "esp_kbd.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nvs.h"

static const char *TAG = "esp_ble_kbd";

void ble_store_config_init(void);       /* NimBLE's NVS key store; no public header */

static esp_hidh_dev_t *volatile s_dev;  /* the connected keyboard */
static int s_battery = -1;
static char s_last[64];
static bool s_hidh_ready;
static SemaphoreHandle_t s_wake;        /* the reconnect task: look again now */
static bool s_task_started;

/* ------------------------------------------------------------- the flag -- */

/* "A keyboard is bonded", so boot can skip Bluetooth entirely when not. Kept
 * in NVS, read once at boot and cached. */
static bool s_paired;

static bool paired_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("esp_kbd", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "paired", &v);
        nvs_close(h);
    }
    s_paired = v != 0;
    return s_paired;
}

static bool paired_get(void)
{
    return s_paired;
}

/* From tasks with internal stacks only (Vim's, the HID host's event task). */
static void paired_set(bool on)
{
    nvs_handle_t h;
    if (s_paired == on)
        return;
    s_paired = on;
    if (nvs_open("esp_kbd", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "paired", on);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* -------------------------------------------------------------- hid host -- */

static void fmt_bda(char out[18], const uint8_t *b)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", b[5], b[4], b[3], b[2], b[1], b[0]);
}

static void on_hidh(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_hidh_event_data_t *p = data;
    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
        if (p->open.status == ESP_OK) {
            s_dev = p->open.dev;
            paired_set(true);
        }
        break;
    case ESP_HIDH_BATTERY_EVENT:
        s_battery = p->battery.level;
        break;
    case ESP_HIDH_INPUT_EVENT: {
        int n = snprintf(s_last, sizeof s_last, "%s/%u:", esp_hid_usage_str(p->input.usage),
                         p->input.length);
        for (int i = 0; i < p->input.length && n < (int)sizeof s_last - 3; i++)
            n += snprintf(s_last + n, sizeof s_last - n, " %02x", p->input.data[i]);
        if (p->input.usage == ESP_HID_USAGE_KEYBOARD)
            esp_kbd_report(p->input.data, p->input.length);
        break;
    }
    case ESP_HIDH_CLOSE_EVENT:
        if (p->close.dev == s_dev) {
            s_dev = NULL;
            esp_kbd_release_all();
            if (s_wake)
                xSemaphoreGive(s_wake);     /* reconnect */
        }
        break;
    default:
        break;
    }
}

/* Security outcomes, for the status (a listener sees them for every
 * connection, including the HID host's). */
static char s_sec[48];
static struct ble_gap_event_listener s_listener;

static int on_gap(struct ble_gap_event *ev, void *arg)
{
    if (ev->type == BLE_GAP_EVENT_ENC_CHANGE) {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0)
            snprintf(s_sec, sizeof s_sec, "status %d enc %d auth %d bond %d", ev->enc_change.status,
                     d.sec_state.encrypted, d.sec_state.authenticated, d.sec_state.bonded);
        else
            snprintf(s_sec, sizeof s_sec, "status %d", ev->enc_change.status);
    }
    return 0;
}

static esp_err_t hidh_init(void)
{
    if (s_hidh_ready)
        return ESP_OK;
    ble_gap_event_listener_register(&s_listener, on_gap, NULL);
    esp_hidh_config_t cfg = { .callback = on_hidh, .event_stack_size = 4096 };
    esp_err_t e = esp_hidh_init(&cfg);
    s_hidh_ready = e == ESP_OK;
    return e;
}

/* Called by NimBLE's setup (esp_ble.c) before the host starts: bond, with
 * "Just Works" -- what many keyboards do (some reject a passkey
 * with "confirm value failed"). Asking to pair is the user's confirmation. */
void esp_ble__config_security(void)
{
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
}

void esp_ble__cancel_connect(void)
{
    if (s_task_started)
        ble_gap_conn_cancel();          /* harmless when none is pending */
}

/* Start Bluetooth and the HID host, with esp_ble__lock held. */
static int ready(char *err, size_t errlen)
{
    if (esp_ble__start(err, errlen) != 0)
        return -1;
    esp_err_t e = hidh_init();
    if (e != ESP_OK)
        return snprintf(err, errlen, "HID host: %s", esp_err_to_name(e)), -1;
    return 0;
}

/* ------------------------------------------------------------ reconnect -- */

static void reconnect_task(void *arg)
{
    char err[96];
    for (;;) {
        if (s_dev == NULL && paired_get()) {
            xSemaphoreTake(esp_ble__lock, portMAX_DELAY);
            if (ready(err, sizeof err) == 0) {
                ble_addr_t peers[4];
                int n = 0;
                ble_store_util_bonded_peers(peers, &n, 4);
                for (int i = 0; i < n && s_dev == NULL; i++)
                    esp_hidh_dev_open(peers[i].val, ESP_HID_TRANSPORT_BLE, peers[i].type);
            }
            xSemaphoreGive(esp_ble__lock);
        }
        /* Look again after a pause, or at once when the keyboard drops. */
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(s_dev ? portMAX_DELAY : 3000));
    }
}

static void start_reconnecting(void)
{
    if (s_task_started)
        return;
    s_wake = xSemaphoreCreateBinary();
    /* An internal stack, not PSRAM: the first attempt starts the Bluetooth
     * stack, which reads flash (calibration, stored bonds), and ESP-IDF
     * refuses flash access from a task with a PSRAM stack -- unless the
     * program runs from PSRAM (ESP_VIM_STACK_IN_PSRAM). */
#if CONFIG_ESP_VIM_STACK_IN_PSRAM
    if (s_wake && xTaskCreatePinnedToCoreWithCaps(reconnect_task, "kbd_reconnect", 4096, NULL, 3,
                                                  NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS)
#else
    if (s_wake && xTaskCreatePinnedToCore(reconnect_task, "kbd_reconnect", 4096, NULL, 3,
                                          NULL, 1) == pdPASS)
#endif
        s_task_started = true;
}

/* ---------------------------------------------------------------- the API -- */

void esp_ble_kbd_boot(void)
{
    if (esp_kbd_init() != ESP_OK || !esp_ble__init_locks())
        return;
    if (paired_load())
        start_reconnecting();
}

static bool parse_addr(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[5], &v[4], &v[3], &v[2], &v[1], &v[0]) != 6)
        return false;
    for (int i = 0; i < 6; i++)
        out[i] = (uint8_t)v[i];         /* NimBLE's order: least significant first */
    return true;
}

int esp_ble_kbd_pair(const char *addr, bool random, char *err, size_t errlen)
{
    uint8_t bda[6];
    if (!parse_addr(addr, bda))
        return snprintf(err, errlen, "not an address: %s", addr), -1;
    if (esp_kbd_init() != ESP_OK || !esp_ble__init_locks())
        return snprintf(err, errlen, "out of memory"), -1;
    esp_ble__cancel_connect();
    xSemaphoreTake(esp_ble__lock, portMAX_DELAY);
    int rc = ready(err, errlen);
    if (rc == 0) {
        if (s_dev != NULL) {            /* one keyboard at a time */
            esp_hidh_dev_close(s_dev);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        esp_hidh_dev_t *dev = esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BLE,
                                                random ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC);
        if (dev == NULL) {
            snprintf(err, errlen, "could not connect to %s (is it in pairing mode?)", addr);
            rc = -1;
        }
    }
    xSemaphoreGive(esp_ble__lock);
    if (rc == 0)
        start_reconnecting();           /* from now on it comes back by itself */
    return rc;
}

int esp_ble_kbd_forget(char *err, size_t errlen)
{
    if (!esp_ble__init_locks())
        return snprintf(err, errlen, "out of memory"), -1;
    paired_set(false);
    esp_ble__cancel_connect();
    xSemaphoreTake(esp_ble__lock, portMAX_DELAY);
    int rc = ready(err, errlen);
    if (rc == 0) {
        if (s_dev != NULL)
            esp_hidh_dev_close(s_dev);
        ble_store_clear();
    }
    xSemaphoreGive(esp_ble__lock);
    return rc;
}

void esp_ble_kbd_status(esp_ble_kbd_status_t *st)
{
    memset(st, 0, sizeof *st);
    esp_hidh_dev_t *dev = s_dev;
    st->connected = dev != NULL;
    st->battery = dev ? s_battery : -1;
    st->paired = paired_get();
    snprintf(st->last_report, sizeof st->last_report, "%s", s_last);
    snprintf(st->security, sizeof st->security, "%s", s_sec);
    if (dev) {
        const uint8_t *b = esp_hidh_dev_bda_get(dev);
        if (b)
            fmt_bda(st->addr, b);
        const char *name = esp_hidh_dev_name_get(dev);
        snprintf(st->name, sizeof st->name, "%s", name ? name : "");
    }
}

#else   /* no BLE central role in this build */

void esp_ble__config_security(void) {}
void esp_ble__cancel_connect(void) {}
void esp_ble_kbd_boot(void) {}

int esp_ble_kbd_pair(const char *addr, bool random, char *err, size_t errlen)
{
    return snprintf(err, errlen, "this build has no Bluetooth keyboard support"), -1;
}

int esp_ble_kbd_forget(char *err, size_t errlen)
{
    return snprintf(err, errlen, "this build has no Bluetooth keyboard support"), -1;
}

void esp_ble_kbd_status(esp_ble_kbd_status_t *st)
{
    memset(st, 0, sizeof *st);
    st->battery = -1;
}

#endif
