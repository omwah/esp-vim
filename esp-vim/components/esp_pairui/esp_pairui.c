/*
 * esp_pairui: see include/esp_pairui.h.
 *
 * The screen, in the overlay's big cells (26 x 10 on the ES3C28P):
 *
 *    Bluetooth keyboard                  <- title
 *   Scanning...                          <- status
 *    My Keyboard          -60            <- up to six keyboards: those in
 *    Old Keyboard      paired              range, and the paired ones (in
 *    ...                                    range or not); tap one to select
 *   Tap your keyboard, then Pair.        <- it, hold a paired one to forget it
 *   [ Not now ]         [  Pair  ]       <- buttons
 *
 * Everything runs on the overlay's own task: it polls the keyboard status,
 * scans in 3-second windows (leaving gaps in which a bonded keyboard can
 * reconnect), and takes taps from the touch task through a queue.
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
#define TAP_SLOP        16
#define HOLD_SLOP       40                  /* a held finger drifts more */
#define HOLD_US         (800 * 1000)        /* a touch this long is a hold */

typedef struct { int x, y; bool hold; } tap_t;

static QueueHandle_t s_taps;
static int s_rows, s_cols;
static esp_ble_dev_t s_kbd[MAX_KBD];
static bool s_bonded[MAX_KBD], s_away[MAX_KBD];     /* paired; paired but not seen */
static int s_nkbd, s_sel = -1;

/* ------------------------------------------------------------------ touch -- */

/* On the touch task: a short touch that didn't wander is a tap; one that
 * stays down 0.8 s is a hold, reported then -- while the finger is still
 * down, so the screen answers it at once -- and its lifting is nothing. */
static void on_touch(esp_touch_event_t ev, int x, int y, void *ctx)
{
    static int x0, y0;
    static int64_t t0;
    static bool held;
    if (ev == ESP_TOUCH_DOWN) {
        x0 = x, y0 = y, t0 = esp_timer_get_time(), held = false;
    } else if (ev == ESP_TOUCH_MOVE && !held && abs(x - x0) < HOLD_SLOP && abs(y - y0) < HOLD_SLOP
               && esp_timer_get_time() - t0 >= HOLD_US) {
        held = true;
        tap_t t = { x0, y0, true };
        xQueueSend(s_taps, &t, 0);
    } else if (ev == ESP_TOUCH_UP && !held && abs(x - x0) < TAP_SLOP && abs(y - y0) < TAP_SLOP) {
        tap_t t = { x0, y0, false };
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
    line(ROW_HINT, !s_nkbd ? "Put your keyboard in pairing mode."
                   : any_bonded ? "Hold paired to forget it."
                   : "Tap your keyboard, then Pair.", false);
    char buttons[64];
    snprintf(buttons, sizeof buttons, "%-*s%s", s_cols - 10, "[ Not now ]",
             s_sel >= 0 ? "[  Pair  ]" : "");
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

static bool found(void *ctx, const esp_ble_dev_t *d)
{
    if (!d->hid || s_nkbd == MAX_KBD)
        return true;
    s_bonded[s_nkbd] = s_away[s_nkbd] = false;
    s_kbd[s_nkbd++] = *d;
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

static void scan(void)
{
    char err[96], was[18] = "";
    if (s_sel >= 0)
        memcpy(was, s_kbd[s_sel].addr, sizeof was);
    status("Scanning...");
    s_nkbd = 0;
    if (esp_ble_scan(SCAN_MS, found, NULL, err, sizeof err) != 0) {
        status(err);
        return;
    }
    /* strongest first, and keep the selection if it's still there */
    for (int i = 1; i < s_nkbd; i++)
        for (int j = i; j > 0 && s_kbd[j].rssi > s_kbd[j - 1].rssi; j--) {
            esp_ble_dev_t t = s_kbd[j]; s_kbd[j] = s_kbd[j - 1]; s_kbd[j - 1] = t;
        }
    add_bonds();                        /* after the sort: the away ones go last */
    s_sel = -1;
    for (int i = 0; i < s_nkbd; i++)
        if (was[0] && strcmp(s_kbd[i].addr, was) == 0)
            s_sel = i;
    status(paired() ? "Press a key on yours, or" : "");
    draw_list();
}

/* -------------------------------------------------------------------- UI -- */

enum { KEEP, CLOSE };

static int tap(tap_t t)
{
    int row, col;
    esp_display_overlay_cell_at(t.x, t.y, &row, &col);
    if (row >= ROW_LIST && row < ROW_LIST + s_nkbd && t.hold && s_bonded[row - ROW_LIST]) {
        char err[96], msg[64];
        esp_ble_dev_t k = s_kbd[row - ROW_LIST];
        snprintf(msg, sizeof msg, "Forgetting %s...", k.name[0] ? k.name : k.addr);
        status(msg);                    /* it may wait for a reconnect attempt */
        if (esp_ble_kbd_forget_one(k.addr, err, sizeof err) == 0) {
            snprintf(msg, sizeof msg, "Forgot %s.", k.name[0] ? k.name : k.addr);
            status(msg);
            int i = row - ROW_LIST;         /* off the list */
            memmove(&s_kbd[i], &s_kbd[i + 1], (s_nkbd - i - 1) * sizeof s_kbd[0]);
            memmove(&s_bonded[i], &s_bonded[i + 1], (s_nkbd - i - 1) * sizeof s_bonded[0]);
            memmove(&s_away[i], &s_away[i + 1], (s_nkbd - i - 1) * sizeof s_away[0]);
            s_nkbd--;
            s_sel = -1;
            draw_list();
        } else {
            status(err);
        }
    } else if (row >= ROW_LIST && row < ROW_LIST + s_nkbd) {
        s_sel = row - ROW_LIST;
        draw_list();
    } else if (row == ROW_BUTTONS && col < 12) {
        return CLOSE;                                   /* Not now */
    } else if (row == ROW_BUTTONS && col >= s_cols - 10 && s_sel >= 0) {
        char err[96], msg[64];
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
    int64_t last_scan = 0;
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
            s_nkbd = 0, s_sel = -1, last_scan = 0;
            add_bonds();                        /* the paired ones at once, before a scan */
            xQueueReset(s_taps);
            esp_touch_set_handler(on_touch, NULL);
            draw_all();
        }
        if (!shown) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (now - last_scan > SCAN_EVERY_US) {
            scan();
            last_scan = esp_timer_get_time();
        }
        tap_t t;
        if (xQueueReceive(s_taps, &t, pdMS_TO_TICKS(250)) == pdTRUE && tap(t) == CLOSE) {
            dismissed = true;
            esp_display_overlay_end();
            esp_touch_set_handler((esp_touch_handler_t)esp_display_touch, NULL);
            shown = false;
        }
    }
}

void esp_pairui_start(void)
{
    s_taps = xQueueCreate(8, sizeof(tap_t));
    /* An internal stack: pairing may start Bluetooth, which reads flash --
     * unless the program runs from PSRAM (ESP_VIM_STACK_IN_PSRAM). */
#if CONFIG_ESP_VIM_STACK_IN_PSRAM
    if (s_taps)
        xTaskCreatePinnedToCoreWithCaps(pairui_task, "pairui", 4096, NULL, 3, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    if (s_taps)
        xTaskCreatePinnedToCore(pairui_task, "pairui", 4096, NULL, 3, NULL, 1);
#endif
}

#else   /* no display, touch panel or Bluetooth keyboards */

void esp_pairui_start(void) {}

#endif
