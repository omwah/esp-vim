/*
 * esp_ble keyboards: see include/esp_ble.h.
 *
 * ESP-IDF's HID host (esp_hidh, NimBLE back end) does the connection, GATT
 * discovery and report parsing, and asks for encryption as soon as it
 * connects; bonding happens then, with the keys kept in NVS by NimBLE's store.
 * Input reports from a keyboard go to components/esp_kbd.
 *
 * Reports are decoded with the keyboard's own report map (esp_kbd_hid.h),
 * learnt when it connects, so a report that isn't the boot layout -- an N-key
 * rollover bitmap -- works too; reports the map doesn't describe as a
 * keyboard's fall back to the boot layout.
 *
 * A background task keeps reconnecting to the bonded keyboards, the last one
 * used first: a BLE keyboard that wakes up advertises, and the host has to
 * connect to it. It holds esp_ble__lock while it tries, and gives way when a
 * scan or a pairing cancels the attempt (esp_ble__cancel_connect).
 *
 * In NVS ("esp_kbd"): "paired" (any keyboard bonded), "last" (the address of
 * the last one connected) and "n" + its address in hex (each one's name).
 */

#include "esp_ble.h"
#include "esp_ble_priv.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED && CONFIG_BT_NIMBLE_ROLE_CENTRAL

#include "esp_attr.h"
#include "esp_hidh.h"
#include "esp_kbd.h"
#include "esp_log.h"
#include "esp_timer.h"
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
static volatile bool s_suspended;       /* asleep: don't reconnect (esp_ble_kbd_suspend) */
/* Until when a connection is a new pairing's, to be announced. (A time, not a
 * flag: the open event may come after esp_hidh_dev_open() returns, and a
 * failed pairing must not make a later reconnect look like one.) */
static volatile int64_t s_pairing_until;
static char s_notice[48];               /* "Paired: <name>", until Vim takes it */
static volatile bool s_notice_ready;

/* The connected keyboard's report layouts, one per report map (HID service).
 * In PSRAM where the build allows it: only tasks touch them. */
#define MAX_MAPS 2
static EXT_RAM_BSS_ATTR esp_kbd_layouts_t s_layouts[MAX_MAPS];
static int s_nmaps;
static char s_layout_desc[96];

/* The last reports, for seeing what a keyboard sends (esp_ble_kbd_reports). */
#define HIST 16
#define HIST_LEN 72
static EXT_RAM_BSS_ATTR char s_hist[HIST][HIST_LEN];
static unsigned s_hist_next;
static portMUX_TYPE s_hist_mux = portMUX_INITIALIZER_UNLOCKED;

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

/* ------------------------------------------------------ names and "last" -- */

static void name_key(char out[16], const uint8_t *b)
{
    snprintf(out, 16, "n%02x%02x%02x%02x%02x%02x", b[5], b[4], b[3], b[2], b[1], b[0]);
}

/* A keyboard connected: remember its name, and that it was the last one. */
static void remember(const uint8_t *bda, const char *name)
{
    nvs_handle_t h;
    char key[16];
    if (nvs_open("esp_kbd", NVS_READWRITE, &h) != ESP_OK)
        return;
    name_key(key, bda);
    char old[32] = "";
    size_t n = sizeof old;
    if (name && name[0] && (nvs_get_str(h, key, old, &n) != ESP_OK || strcmp(old, name) != 0))
        nvs_set_str(h, key, name);
    uint8_t last[6];
    n = sizeof last;
    if (nvs_get_blob(h, "last", last, &n) != ESP_OK || memcmp(last, bda, 6) != 0)
        nvs_set_blob(h, "last", bda, 6);
    nvs_commit(h);
    nvs_close(h);
}

static void recall_name(const uint8_t *bda, char *out, size_t len)
{
    nvs_handle_t h;
    char key[16];
    out[0] = '\0';
    if (nvs_open("esp_kbd", NVS_READONLY, &h) != ESP_OK)
        return;
    name_key(key, bda);
    if (nvs_get_str(h, key, out, &len) != ESP_OK)
        out[0] = '\0';
    nvs_close(h);
}

static bool recall_last(uint8_t out[6])
{
    nvs_handle_t h;
    size_t n = 6;
    if (nvs_open("esp_kbd", NVS_READONLY, &h) != ESP_OK)
        return false;
    bool ok = nvs_get_blob(h, "last", out, &n) == ESP_OK && n == 6;
    nvs_close(h);
    return ok;
}

static void erase_names(const uint8_t *bda)     /* NULL: every keyboard's */
{
    nvs_handle_t h;
    if (nvs_open("esp_kbd", NVS_READWRITE, &h) != ESP_OK)
        return;
    uint8_t last[6];
    size_t n = sizeof last;
    if (bda == NULL || (nvs_get_blob(h, "last", last, &n) == ESP_OK && memcmp(last, bda, 6) == 0))
        nvs_erase_key(h, "last");
    if (bda != NULL) {
        char key[16];
        name_key(key, bda);
        nvs_erase_key(h, key);
    } else {
        nvs_iterator_t it = NULL;
        char keys[8][16];
        int nk = 0;
        esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, "esp_kbd", NVS_TYPE_STR, &it);
        while (e == ESP_OK && nk < 8) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (info.key[0] == 'n')
                snprintf(keys[nk++], 16, "%s", info.key);
            e = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        for (int i = 0; i < nk; i++)
            nvs_erase_key(h, keys[i]);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* The bonded peers, the last one used first. With the host running. */
static int bonded(ble_addr_t *peers, int max)
{
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, max) != 0)
        return 0;
    uint8_t last[6];
    if (recall_last(last))
        for (int i = 1; i < n; i++)
            if (memcmp(peers[i].val, last, 6) == 0) {
                ble_addr_t t = peers[i];
                memmove(&peers[1], &peers[0], i * sizeof peers[0]);
                peers[0] = t;
            }
    return n;
}

/* -------------------------------------------------------------- hid host -- */

static void fmt_bda(char out[18], const uint8_t *b)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", b[5], b[4], b[3], b[2], b[1], b[0]);
}

/* The keyboard's report maps, parsed once as it connects. */
static void learn_layouts(esp_hidh_dev_t *dev)
{
    size_t n = 0;
    esp_hid_raw_report_map_t *maps = NULL;
    s_nmaps = 0;
    s_layout_desc[0] = '\0';
    if (esp_hidh_dev_report_maps_get(dev, &n, &maps) != ESP_OK || maps == NULL)
        return;
    for (size_t i = 0; i < n && i < MAX_MAPS; i++) {
        esp_kbd_parse_map(maps[i].data, maps[i].len, &s_layouts[i]);
        if (s_layouts[i].n && !s_layout_desc[0])
            esp_kbd_describe(&s_layouts[i], s_layout_desc, sizeof s_layout_desc);
    }
    s_nmaps = n < MAX_MAPS ? (int)n : MAX_MAPS;
}

static bool map_has(const esp_kbd_layouts_t *l, uint16_t id)
{
    for (int i = 0; i < l->n; i++)
        if (l->report[i].id == id && l->report[i].nfields)
            return true;
    return false;
}

static void input(const esp_hidh_event_data_t *p)
{
    esp_kbd_keys_t k;
    int m = p->input.map_index;
    if (m < s_nmaps && map_has(&s_layouts[m], p->input.report_id)) {
        if (esp_kbd_decode(&s_layouts[m], (uint8_t)p->input.report_id, p->input.data,
                           p->input.length, &k))
            esp_kbd_input((uint16_t)(m << 8 | p->input.report_id), &k);
    } else if (p->input.usage == ESP_HID_USAGE_KEYBOARD) {
        esp_kbd_report(p->input.data, p->input.length);    /* boot protocol, or no map */
    }
}

static void on_hidh(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_hidh_event_data_t *p = data;
    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
        if (p->open.status == ESP_OK) {
            learn_layouts(p->open.dev);
            s_dev = p->open.dev;
            paired_set(true);
            const uint8_t *b = esp_hidh_dev_bda_get(p->open.dev);
            const char *name = esp_hidh_dev_name_get(p->open.dev);
            if (b)
                remember(b, name);
            if (esp_timer_get_time() < s_pairing_until) {
                char a[18] = "";
                if (b)
                    fmt_bda(a, b);
                snprintf(s_notice, sizeof s_notice, "Paired: %s", name && name[0] ? name : a);
                s_notice_ready = true;
                s_pairing_until = 0;
            }
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
        char h[HIST_LEN];
        int m = snprintf(h, sizeof h, "%u.%u/%u:", p->input.map_index, p->input.report_id,
                         p->input.length);
        for (int i = 0; i < p->input.length && m < (int)sizeof h - 3; i++)
            m += snprintf(h + m, sizeof h - m, " %02x", p->input.data[i]);
        taskENTER_CRITICAL(&s_hist_mux);
        memcpy(s_hist[s_hist_next++ % HIST], h, sizeof h);
        taskEXIT_CRITICAL(&s_hist_mux);
        input(p);
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
        if (s_dev == NULL && paired_get() && !s_suspended) {
            xSemaphoreTake(esp_ble__lock, portMAX_DELAY);
            if (ready(err, sizeof err) == 0) {
                ble_addr_t peers[4];
                int n = bonded(peers, 4);   /* the last one used first */
                for (int i = 0; i < n && s_dev == NULL && !s_suspended; i++)
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
        s_pairing_until = esp_timer_get_time() + 40 * 1000000LL;   /* 30 s to connect */
        esp_hidh_dev_t *dev = esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BLE,
                                                random ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC);
        if (dev == NULL) {
            s_pairing_until = 0;
            snprintf(err, errlen, "could not connect to %s (is it in pairing mode?)", addr);
            rc = -1;
        }
    }
    xSemaphoreGive(esp_ble__lock);
    if (rc == 0) {
        start_reconnecting();           /* from now on it comes back by itself */
        if (s_pairing_until)            /* the open event, if it is still to come */
            s_pairing_until = esp_timer_get_time() + 10 * 1000000LL;
    }
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
        erase_names(NULL);
    }
    xSemaphoreGive(esp_ble__lock);
    return rc;
}

int esp_ble_kbd_forget_one(const char *addr, char *err, size_t errlen)
{
    uint8_t bda[6];
    if (!parse_addr(addr, bda))
        return snprintf(err, errlen, "not an address: %s", addr), -1;
    if (!esp_ble__init_locks())
        return snprintf(err, errlen, "out of memory"), -1;
    esp_ble__cancel_connect();
    xSemaphoreTake(esp_ble__lock, portMAX_DELAY);
    int rc = ready(err, errlen);
    if (rc == 0) {
        ble_addr_t peers[4];
        int n = 0, found = -1;
        ble_store_util_bonded_peers(peers, &n, 4);
        for (int i = 0; i < n; i++)
            if (memcmp(peers[i].val, bda, 6) == 0)
                found = i;
        if (found < 0) {
            snprintf(err, errlen, "%s is not a paired keyboard", addr);
            rc = -1;
        } else {
            esp_hidh_dev_t *dev = s_dev;
            const uint8_t *b = dev ? esp_hidh_dev_bda_get(dev) : NULL;
            if (b && memcmp(b, bda, 6) == 0) {
                esp_hidh_dev_close(dev);
                for (int i = 0; i < 40 && s_dev != NULL; i++)
                    vTaskDelay(pdMS_TO_TICKS(50));  /* the close event: at most 2 s */
            }
            ble_gap_unpair(&peers[found]);  /* its keys, from the store too */
            erase_names(bda);
            if (n == 1)
                paired_set(false);
        }
    }
    xSemaphoreGive(esp_ble__lock);
    return rc;
}

int esp_ble_kbd_bonds(esp_ble_bond_t *out, int max)
{
    if (!esp_ble__running())
        return 0;
    ble_addr_t peers[4];
    int n = bonded(peers, max < 4 ? max : 4);
    esp_hidh_dev_t *dev = s_dev;
    const uint8_t *cur = dev ? esp_hidh_dev_bda_get(dev) : NULL;
    uint8_t last[6];
    bool have_last = recall_last(last);
    for (int i = 0; i < n; i++) {
        esp_ble_bond_t *b = &out[i];
        memset(b, 0, sizeof *b);
        fmt_bda(b->addr, peers[i].val);
        b->random = peers[i].type != BLE_ADDR_PUBLIC;
        recall_name(peers[i].val, b->name, sizeof b->name);
        b->connected = cur && memcmp(cur, peers[i].val, 6) == 0;
        b->last = have_last && memcmp(last, peers[i].val, 6) == 0;
    }
    return n;
}

int esp_ble_kbd_reports(char (*out)[72], int max)
{
    int n = 0;
    taskENTER_CRITICAL(&s_hist_mux);
    unsigned total = s_hist_next;
    for (unsigned i = total > HIST ? total - HIST : 0; i < total && n < max; i++)
        memcpy(out[n++], s_hist[i % HIST], HIST_LEN);
    taskEXIT_CRITICAL(&s_hist_mux);
    return n;
}

bool esp_ble_kbd_notice(char *out, size_t len)
{
    if (!s_notice_ready)
        return false;
    s_notice_ready = false;
    snprintf(out, len, "%s", s_notice);
    return true;
}

void esp_ble_kbd_suspend(void)
{
    s_suspended = true;
    if (!s_task_started)
        return;                         /* no keyboard: Bluetooth never started */
    esp_ble__cancel_connect();          /* a reconnect attempt waiting to connect */
    xSemaphoreTake(esp_ble__lock, portMAX_DELAY);
    esp_hidh_dev_t *dev = s_dev;
    if (dev)
        esp_hidh_dev_close(dev);
    xSemaphoreGive(esp_ble__lock);
    for (int i = 0; i < 40 && s_dev != NULL; i++)
        vTaskDelay(pdMS_TO_TICKS(50));  /* the close event: at most 2 s */
}

void esp_ble_kbd_resume(void)
{
    s_suspended = false;
    if (s_wake)
        xSemaphoreGive(s_wake);         /* reconnect now */
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
    st->bonds = -1;
    if (esp_ble__running()) {
        ble_addr_t peers[4];
        int n = 0;
        st->bonds = ble_store_util_bonded_peers(peers, &n, 4) == 0 ? n : -1;
    }
    if (dev) {
        snprintf(st->layout, sizeof st->layout, "%s", s_layout_desc);
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
void esp_ble_kbd_suspend(void) {}
void esp_ble_kbd_resume(void) {}

int esp_ble_kbd_pair(const char *addr, bool random, char *err, size_t errlen)
{
    return snprintf(err, errlen, "this build has no Bluetooth keyboard support"), -1;
}

int esp_ble_kbd_forget(char *err, size_t errlen)
{
    return snprintf(err, errlen, "this build has no Bluetooth keyboard support"), -1;
}

int esp_ble_kbd_forget_one(const char *addr, char *err, size_t errlen)
{
    return snprintf(err, errlen, "this build has no Bluetooth keyboard support"), -1;
}

int esp_ble_kbd_bonds(esp_ble_bond_t *out, int max)
{
    return 0;
}

bool esp_ble_kbd_notice(char *out, size_t len)
{
    return false;
}

int esp_ble_kbd_reports(char (*out)[72], int max)
{
    return 0;
}

void esp_ble_kbd_status(esp_ble_kbd_status_t *st)
{
    memset(st, 0, sizeof *st);
    st->battery = -1;
    st->bonds = -1;
}

#endif
