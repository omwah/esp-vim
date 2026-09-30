/*
 * esp_pairui: see include/esp_pairui.h.
 *
 * The screen, in the overlay's big cells (26 x 10 on the ES3C28P):
 *
 *    Bluetooth keyboard                  <- title
 *   Scanning...                          <- status
 *    My Keyboard          -60            <- up to six keyboards: those in
 *    Old Keyboard      paired              range, and the paired ones (in
 *    ...                                    range or not); tap one to select it
 *   Tap your keyboard, then Pair.        <- hint
 *   [ Not now ]         [  Pair  ]       <- buttons: "Unpair" for a paired one
 *
 * The overlay's own task (an internal stack: pairing and unpairing write
 * flash) polls the keyboard status and handles taps and scan results, both
 * arriving through one queue, so a tap is answered at once. Scanning -- 3 s
 * windows every 7 s, leaving gaps in which a bonded keyboard can reconnect --
 * is a second task's (a PSRAM stack), started the first time the overlay is
 * shown; the first scan, which may start Bluetooth, is the overlay task's.
 */

#include "esp_pairui.h"

#include "sdkconfig.h"

#if CONFIG_ESP_VIM_DISPLAY && CONFIG_ESP_VIM_TOUCH && CONFIG_BT_NIMBLE_ROLE_CENTRAL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_ble.h"
#include "esp_display.h"
#include "esp_timer.h"
#include "esp_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BOOT_WAIT_US    (10 * 1000000LL)    /* no keyboard this long after boot */
#define GONE_WAIT_US    (15 * 1000000LL)    /* the keyboard gone this long */
#define SCAN_MS         3000
#define SCAN_EVERY_US   (7 * 1000000LL)     /* the rest is left for reconnecting */
#define MAX_KBD         6
#define ROW_STATUS      1
#define ROW_LIST        2
#define ROW_HINT        8
#define ROW_BUTTONS     9
#define TAP_SLOP        32                  /* a finger on the glass moves */

typedef enum { EV_TAP, EV_SCANNED } ev_kind_t;
typedef struct { ev_kind_t kind; int x, y; } tap_t;

static QueueHandle_t s_taps;                /* taps and finished scans */
static volatile bool s_scan_wanted;         /* the overlay is up: keep scanning */
static bool s_scanner_started;
static SemaphoreHandle_t s_found_lock;
static esp_ble_dev_t s_found[MAX_KBD];      /* the last scan's, for the overlay task */
static int s_nfound;
static int s_rows, s_cols;
static esp_ble_dev_t s_kbd[MAX_KBD];
static bool s_bonded[MAX_KBD], s_away[MAX_KBD];     /* paired; paired but not seen */
static int s_nkbd, s_sel = -1;

/* ------------------------------------------------------------------ touch -- */

/* On the touch task: a short touch that didn't wander is a tap. */
static void on_touch(esp_touch_event_t ev, int x, int y, void *ctx)
{
    static int x0, y0;
    if (ev == ESP_TOUCH_DOWN) {
        x0 = x, y0 = y;
    } else if (ev == ESP_TOUCH_UP && abs(x - x0) < TAP_SLOP && abs(y - y0) < TAP_SLOP) {
        tap_t t = { EV_TAP, x0, y0 };
        xQueueSend(s_taps, &t, 0);
    }
}

/* A keyboard is bonded (it may come back by itself). */
static bool paired(void)
{
    esp_ble_kbd_status_t st;
    esp_ble_kbd_status(&st);
    return st.paired;
}

/* ---------------------------------------------------------------- drawing -- */

/* A whole row: padded, so it replaces what was there. */
static void line(int row, const char *text, bool inverse)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%-*.*s", s_cols, s_cols, text);
    esp_display_overlay_text(row, 0, buf, inverse);
}

static void status(const char *text)
{
    line(ROW_STATUS, text, false);
}

static void draw_list(void)
{
    bool any_bonded = false;
    for (int i = 0; i < MAX_KBD; i++) {
        char buf[64] = "", rssi[8];
        if (i < s_nkbd) {
            snprintf(rssi, sizeof rssi, "%6d", s_kbd[i].rssi);
            snprintf(buf, sizeof buf, " %-17.17s %s", s_kbd[i].name[0] ? s_kbd[i].name : s_kbd[i].addr,
                     s_bonded[i] ? "paired" : rssi);
            any_bonded |= s_bonded[i];
        }
        line(ROW_LIST + i, buf, i == s_sel);
    }
    bool unpair = s_sel >= 0 && s_bonded[s_sel];
    line(ROW_HINT, !s_nkbd ? "Put your keyboard in pairing mode."
                   : unpair ? "Unpair forgets this one."
                   : any_bonded ? "Tap one: Pair or Unpair."
                   : "Tap your keyboard, then Pair.", false);
    char buttons[64];
    snprintf(buttons, sizeof buttons, "%-*s%s", s_cols - 10, "[ Not now ]",
             s_sel < 0 ? "" : unpair ? "[ Unpair ]" : "[  Pair  ]");
    line(ROW_BUTTONS, buttons, false);
}

static void draw_all(void)
{
    esp_display_overlay_clear();
    line(0, " Bluetooth keyboard", true);
    status(paired() ? "Press a key on yours, or" : "");
    draw_list();
}

/* ------------------------------------------------------------------- scan -- */

typedef struct { esp_ble_dev_t d[MAX_KBD]; int n; } found_t;

static bool found(void *ctx, const esp_ble_dev_t *d)
{
    found_t *f = ctx;
    if (d->hid && f->n < MAX_KBD)
        f->d[f->n++] = *d;
    return true;
}

/* Mark the paired keyboards among those found, and list the others, away. */
static void add_bonds(void)
{
    esp_ble_bond_t b[4];
    int n = esp_ble_kbd_bonds(b, 4);
    for (int j = 0; j < n; j++) {
        int i = 0;
        while (i < s_nkbd && strcmp(s_kbd[i].addr, b[j].addr) != 0)
            i++;
        if (i == s_nkbd) {
            if (s_nkbd == MAX_KBD)
                continue;
            memset(&s_kbd[i], 0, sizeof s_kbd[i]);
            memcpy(s_kbd[i].addr, b[j].addr, sizeof s_kbd[i].addr);
            s_kbd[i].addr_type = b[j].random ? "random" : "public";
            s_away[i] = true;
            s_nkbd++;
        }
        s_bonded[i] = true;
        if (b[j].name[0] && !s_kbd[i].name[0])
            snprintf(s_kbd[i].name, sizeof s_kbd[i].name, "%s", b[j].name);
    }
}

/* A scan's keyboards onto the list: strongest first, then the paired ones
 * not in range; the selection kept if it is still there. */
static void show_found(const found_t *f)
{
    char was[18] = "";
    if (s_sel >= 0)
        memcpy(was, s_kbd[s_sel].addr, sizeof was);
    s_nkbd = f->n;
    for (int i = 0; i < s_nkbd; i++) {
        s_kbd[i] = f->d[i];
        s_bonded[i] = s_away[i] = false;
    }
    for (int i = 1; i < s_nkbd; i++)
        for (int j = i; j > 0 && s_kbd[j].rssi > s_kbd[j - 1].rssi; j--) {
            esp_ble_dev_t t = s_kbd[j]; s_kbd[j] = s_kbd[j - 1]; s_kbd[j - 1] = t;
        }
    add_bonds();                        /* after the sort: the away ones go last */
    s_sel = -1;
    for (int i = 0; i < s_nkbd; i++)
        if (was[0] && strcmp(s_kbd[i].addr, was) == 0)
            s_sel = i;
    draw_list();
}

/* A scan on this task: the first one, which may start Bluetooth. */
static void scan_here(void)
{
    char err[96];
    static found_t f;
    f.n = 0;
    status("Scanning...");
    if (esp_ble_scan(SCAN_MS, found, &f, err, sizeof err) != 0) {
        status(err);
        return;
    }
    status(paired() ? "Press a key on yours, or" : "");
    show_found(&f);
}

/* The scanner task: while the overlay is up, a scan every SCAN_EVERY_US. */
static void scanner_task(void *arg)
{
    static found_t f;
    char err[96];
    for (;;) {
        if (!s_scan_wanted || !esp_ble_running()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        f.n = 0;
        if (esp_ble_scan(SCAN_MS, found, &f, err, sizeof err) == 0 && s_scan_wanted) {
            xSemaphoreTake(s_found_lock, portMAX_DELAY);
            memcpy(s_found, f.d, sizeof s_found);
            s_nfound = f.n;
            xSemaphoreGive(s_found_lock);
            tap_t ev = { EV_SCANNED, 0, 0 };
            xQueueSend(s_taps, &ev, 0);
        }
        for (int64_t t = 0; t < SCAN_EVERY_US - SCAN_MS * 1000LL && s_scan_wanted; t += 250000)
            vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void start_scanning(void)
{
    s_scan_wanted = true;
    if (s_scanner_started)
        return;
    /* PSRAM: it only scans once Bluetooth runs, and never touches flash. */
    s_scanner_started = xTaskCreatePinnedToCoreWithCaps(scanner_task, "pairscan", 3072, NULL, 3,
                                                        NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                        == pdPASS;
}

/* -------------------------------------------------------------------- UI -- */

enum { KEEP, CLOSE };

static int tap(tap_t t)
{
    int row, col;
    esp_display_overlay_cell_at(t.x, t.y, &row, &col);
    if (row >= ROW_LIST && row < ROW_LIST + s_nkbd) {
        s_sel = row - ROW_LIST;
        draw_list();
    } else if (row == ROW_BUTTONS && col < 12) {
        return CLOSE;                                   /* Not now */
    } else if (row == ROW_BUTTONS && col >= s_cols - 10 && s_sel >= 0 && s_bonded[s_sel]) {
        char err[96], msg[64];                          /* Unpair */
        int i = s_sel;
        esp_ble_dev_t k = s_kbd[i];
        snprintf(msg, sizeof msg, "Unpairing %s...", k.name[0] ? k.name : k.addr);
        status(msg);
        if (esp_ble_kbd_forget_one(k.addr, err, sizeof err) == 0) {
            snprintf(msg, sizeof msg, "Unpaired %s.", k.name[0] ? k.name : k.addr);
            status(msg);
            if (s_away[i]) {                            /* not in range: off the list */
                memmove(&s_kbd[i], &s_kbd[i + 1], (s_nkbd - i - 1) * sizeof s_kbd[0]);
                memmove(&s_bonded[i], &s_bonded[i + 1], (s_nkbd - i - 1) * sizeof s_bonded[0]);
                memmove(&s_away[i], &s_away[i + 1], (s_nkbd - i - 1) * sizeof s_away[0]);
                s_nkbd--;
            } else {
                s_bonded[i] = false;                    /* in range: can be paired again */
            }
            s_sel = -1;
            draw_list();
        } else {
            status(err);
        }
    } else if (row == ROW_BUTTONS && col >= s_cols - 10 && s_sel >= 0) {
        char err[96], msg[64];                          /* Pair */
        esp_ble_dev_t k = s_kbd[s_sel];
        snprintf(msg, sizeof msg, "Pairing %s...", k.name[0] ? k.name : k.addr);
        status(msg);
        if (esp_ble_kbd_pair(k.addr, k.addr_type && strcmp(k.addr_type, "random") == 0,
                             err, sizeof err) == 0) {
            snprintf(msg, sizeof msg, "Paired: %s", k.name[0] ? k.name : k.addr);
            status(msg);
        } else {
            status(err);
        }
    }
    return KEEP;
}

static void pairui_task(void *arg)
{
    int64_t boot = esp_timer_get_time(), last_seen = 0;
    bool ever = false, dismissed = false, shown = false;
    for (;;) {
        esp_ble_kbd_status_t st;
        esp_ble_kbd_status(&st);
        int64_t now = esp_timer_get_time();
        if (st.connected) {
            ever = true;
            last_seen = now;
            dismissed = false;                  /* a later disconnect may show it */
            if (shown) {
                esp_display_overlay_end();
                esp_touch_set_handler((esp_touch_handler_t)esp_display_touch, NULL);
                shown = false;
            }
        } else if (!shown && !dismissed
                   && ((!ever && now - boot > BOOT_WAIT_US) || (ever && now - last_seen > GONE_WAIT_US))
                   && esp_display_overlay_begin(&s_rows, &s_cols)) {
            shown = true;
            s_nkbd = 0, s_sel = -1;
            add_bonds();                        /* the paired ones at once, before a scan */
            xQueueReset(s_taps);
            esp_touch_set_handler(on_touch, NULL);
            draw_all();
            if (!esp_ble_running())
                scan_here();                    /* starts Bluetooth: from this task */
            start_scanning();
        }
        if (!shown) {
            s_scan_wanted = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        tap_t t;
        if (xQueueReceive(s_taps, &t, pdMS_TO_TICKS(250)) != pdTRUE)
            continue;
        if (t.kind == EV_SCANNED) {
            static found_t f;
            xSemaphoreTake(s_found_lock, portMAX_DELAY);
            memcpy(f.d, s_found, sizeof f.d);
            f.n = s_nfound;
            xSemaphoreGive(s_found_lock);
            show_found(&f);
        } else if (tap(t) == CLOSE) {
            dismissed = true;
            s_scan_wanted = false;
            esp_display_overlay_end();
            esp_touch_set_handler((esp_touch_handler_t)esp_display_touch, NULL);
            shown = false;
        }
    }
}

void esp_pairui_start(void)
{
    s_taps = xQueueCreate(8, sizeof(tap_t));
    s_found_lock = xSemaphoreCreateMutex();
    /* An internal stack: pairing may start Bluetooth, which reads flash --
     * unless the program runs from PSRAM (ESP_VIM_STACK_IN_PSRAM). */
#if CONFIG_ESP_VIM_STACK_IN_PSRAM
    if (s_taps && s_found_lock)
        xTaskCreatePinnedToCoreWithCaps(pairui_task, "pairui", 4096, NULL, 3, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    if (s_taps && s_found_lock)
        xTaskCreatePinnedToCore(pairui_task, "pairui", 4096, NULL, 3, NULL, 1);
#endif
}

#else   /* no display, touch panel or Bluetooth keyboards */

void esp_pairui_start(void) {}

#endif
