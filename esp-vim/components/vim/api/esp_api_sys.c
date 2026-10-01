/*
 * esp_info(), esp_heap(), esp_tasks(), esp_reboot(): the system builtins
 * behind :EspInfo, :EspHeap, :EspTasks and :EspReboot.
 */

#include "vim.h"
#include "version.h"          /* VIM_VERSION_SHORT */
#include "esp_vim_api.h"
#include "esp_vim_port.h"
#include "esp_board.h"
#include "esp_display.h"
#include "esp_power.h"
#include "esp_time.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *reset_reason(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "unknown";
    }
}

/*
 * esp_info() -> Dict: chip, revision, cores, cpu_mhz, features, flash, psram,
 * mac, idf, vim, uptime_ms, reset.
 */
void f_esp_info(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    dict_T *d = rettv->vval.v_dict;

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    char buf[48];

    dict_add_string(d, "chip", (char_u *)ESP_VIM_CHIP);
    if (esp_board_name() != NULL) {                     /* a board with a board layer */
        dict_add_string(d, "board", (char_u *)esp_board_name());
        if (*esp_board_panel_name())
            dict_add_string(d, "panel", (char_u *)esp_board_panel_name());
    }
    snprintf(buf, sizeof buf, "v%d.%d", ci.revision / 100, ci.revision % 100);
    dict_add_string(d, "revision", (char_u *)buf);
    dict_add_number(d, "cores", ci.cores);
    dict_add_number(d, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

    list_T *feat = list_alloc();
    if (feat != NULL) {
        if (ci.features & CHIP_FEATURE_WIFI_BGN)   list_append_string(feat, (char_u *)"wifi", -1);
        if (ci.features & CHIP_FEATURE_BLE)        list_append_string(feat, (char_u *)"ble", -1);
        if (ci.features & CHIP_FEATURE_BT)         list_append_string(feat, (char_u *)"bt", -1);
        if (ci.features & CHIP_FEATURE_IEEE802154) list_append_string(feat, (char_u *)"802.15.4", -1);
        if (ci.features & CHIP_FEATURE_EMB_FLASH)  list_append_string(feat, (char_u *)"embedded flash", -1);
        if (ci.features & CHIP_FEATURE_EMB_PSRAM)  list_append_string(feat, (char_u *)"embedded psram", -1);
        dict_add_list(d, "features", feat);
    }

    uint32_t flash = 0;
    if (esp_flash_get_size(NULL, &flash) == ESP_OK)
        dict_add_number(d, "flash", flash);
    dict_add_number(d, "psram", esp_psram_is_initialized() ? (varnumber_T)esp_psram_get_size() : 0);

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK) {
        snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        dict_add_string(d, "mac", (char_u *)buf);
    }

    dict_add_string(d, "idf", (char_u *)esp_get_idf_version());
    snprintf(buf, sizeof buf, "%s.%d", VIM_VERSION_SHORT, highest_patch());
    dict_add_string(d, "vim", (char_u *)buf);
    dict_add_number(d, "uptime_ms", (varnumber_T)(esp_timer_get_time() / 1000));
    dict_add_string(d, "reset", (char_u *)reset_reason(esp_reset_reason()));
}

static void add_heap(dict_T *d, const char *name, uint32_t caps)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    dict_T *h = dict_alloc();
    if (h == NULL)
        return;
    dict_add_number(h, "free", info.total_free_bytes);
    dict_add_number(h, "largest", info.largest_free_block);
    dict_add_number(h, "min_free", info.minimum_free_bytes);
    dict_add_number(h, "total", info.total_free_bytes + info.total_allocated_bytes);
    dict_add_dict(d, name, h);
}

/*
 * esp_heap() -> Dict of Dicts. "internal", "psram" and "dma" each have free,
 * largest, min_free and total (bytes); "vim" has used, peak and budget.
 */
void f_esp_heap(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    dict_T *d = rettv->vval.v_dict;
    add_heap(d, "internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    add_heap(d, "psram", MALLOC_CAP_SPIRAM);
    add_heap(d, "dma", MALLOC_CAP_DMA);

    size_t used, peak, budget;
    esp_vim_heap_stats(&used, &peak, &budget);
    dict_T *v = dict_alloc();
    if (v != NULL) {
        dict_add_number(v, "used", used);
        dict_add_number(v, "peak", peak);
        dict_add_number(v, "budget", budget);
        dict_add_dict(d, "vim", v);
    }
}

static const char *task_state(eTaskState s)
{
    switch (s) {
    case eRunning:   return "running";
    case eReady:     return "ready";
    case eBlocked:   return "blocked";
    case eSuspended: return "suspended";
    case eDeleted:   return "deleted";
    default:         return "?";
    }
}

/*
 * esp_tasks() -> List of Dicts: name, state, priority, core (-1: either),
 * stack_free (the lowest it has been, in bytes).
 */
void f_esp_tasks(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;       /* tasks may start meanwhile */
    TaskStatus_t *st = malloc(n * sizeof *st);
    if (st == NULL)
        return;
    n = uxTaskGetSystemState(st, n, NULL);
    for (UBaseType_t i = 0; i < n; i++) {
        dict_T *t = dict_alloc();
        if (t == NULL)
            break;
        dict_add_string(t, "name", (char_u *)st[i].pcTaskName);
        dict_add_string(t, "state", (char_u *)task_state(st[i].eCurrentState));
        dict_add_number(t, "priority", st[i].uxCurrentPriority);
        BaseType_t core = xTaskGetCoreID(st[i].xHandle);
        dict_add_number(t, "core", core == tskNO_AFFINITY ? -1 : (varnumber_T)core);
        /* ESP-IDF's StackType_t is a byte, so the high-water mark is in bytes. */
        dict_add_number(t, "stack_free", st[i].usStackHighWaterMark);
        list_append_dict(rettv->vval.v_list, t);
    }
    free(st);
}

/* esp_reboot(): restart the chip now. Checking for unsaved work is :EspReboot's job. */
void f_esp_reboot(typval_T *argvars UNUSED, typval_T *rettv UNUSED)
{
    out_flush();
    esp_restart();
}

/* For the port (esp_shims.c), which doesn't include vim.h. */
void esp_vim__redraw_all(void)
{
    redraw_later(UPD_CLEAR);
}

bool esp_vim__any_modified(void)
{
    return anyBufIsChanged();
}

/*
 * esp_power([{settings}]) -> Dict: source ("usb", "battery", "unknown"),
 * battery (the board has a battery input), battery_mv and battery_pct (-1:
 * none; battery_pct -1 too when the reading is too low for a battery: none
 * fitted; measured afresh when the last reading is over 2 s old), reader (reader mode available),
 * idle_min, deep_min, idle_s, sleeps, deep_sleeps, last_wake ("key", "timer",
 * ""). {settings}: idle_min and/or deep_min to change, kept in NVS.
 */
void f_esp_power(typval_T *argvars, typval_T *rettv)
{
    if (argvars[0].v_type == VAR_DICT && argvars[0].vval.v_dict != NULL) {
        dict_T *d = argvars[0].vval.v_dict;
        int idle = dict_has_key(d, "idle_min") ? (int)dict_get_number(d, "idle_min") : -1;
        int deep = dict_has_key(d, "deep_min") ? (int)dict_get_number(d, "deep_min") : -1;
        esp_power_set(idle, deep);
    }
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    esp_power_status_t st;
    esp_power_status(&st);
    dict_T *d = rettv->vval.v_dict;
    dict_add_string(d, "source", (char_u *)st.source);
    dict_add_bool(d, "battery", st.battery);
    dict_add_number(d, "battery_mv", st.battery_mv);
    dict_add_number(d, "battery_pct", st.battery_pct);
    dict_add_bool(d, "reader", st.reader);
    dict_add_number(d, "idle_min", st.idle_min);
    dict_add_number(d, "deep_min", st.deep_min);
    dict_add_number(d, "idle_s", st.idle_s);
    dict_add_number(d, "sleeps", st.sleeps);
    dict_add_number(d, "presses", st.presses);
    dict_add_number(d, "deep_sleeps", st.deep_sleeps);
    dict_add_string(d, "last_wake", (char_u *)st.last_wake);
}

/*
 * esp_sleep([{deep} [, {seconds}]]) -> Bool: sleep now. Reader mode (light
 * sleep: Vim carries on where it was, once the wake button is pressed), or
 * with {deep} a deep sleep, which doesn't return -- waking is a boot. With
 * {seconds}, a timer wakes it too. The caller checks for unsaved changes
 * (:EspSleep does); in reader mode they only keep it from turning into a
 * deep sleep.
 */
void f_esp_sleep(typval_T *argvars, typval_T *rettv)
{
    int error = FALSE;
    bool deep = argvars[0].v_type != VAR_UNKNOWN && tv_get_bool_chk(&argvars[0], &error);
    int secs = argvars[0].v_type != VAR_UNKNOWN && argvars[1].v_type != VAR_UNKNOWN
               ? (int)tv_get_number_chk(&argvars[1], &error) : 0;
    rettv->v_type = VAR_BOOL;
    rettv->vval.v_number = VVAL_FALSE;
    if (error)
        return;
    out_flush();                        /* the screen shows everything first */
    esp_err_t e = esp_power_sleep(deep, secs, anyBufIsChanged());
    if (e == ESP_ERR_NOT_SUPPORTED) {
        emsg("esp_sleep(): no wake button on this board: nothing would wake it but a reset (deep sleep)");
        return;
    }
    if (e != ESP_OK) {
        semsg("esp_sleep(): %s", esp_err_to_name(e));
        return;
    }
    rettv->vval.v_number = VVAL_TRUE;
}

/*
 * esp_console_output([{on}]) -> Bool: whether Vim's output also goes to the
 * serial console. With {on}: switch it (off only where a display shows the
 * console). Kept across restarts; a key typed on the serial console turns it
 * back on.
 */
void f_esp_console_output(typval_T *argvars, typval_T *rettv)
{
    if (argvars[0].v_type != VAR_UNKNOWN) {
        int error = FALSE;
        varnumber_T on = tv_get_bool_chk(&argvars[0], &error);
        if (error)
            return;
        if (esp_vim_set_console_output(on != 0) != 0) {
            emsg("esp_console_output(): no display -- the serial console is the only screen");
            return;
        }
    }
    rettv->v_type = VAR_BOOL;
    rettv->vval.v_number = esp_vim_console_output() ? VVAL_TRUE : VVAL_FALSE;
}

/* A font's screen size as :EspFont names it, "80x24". */
static void screen_size(const esp_display_font_info_t *f, char *buf, size_t len)
{
    snprintf(buf, len, "%dx%d", f->cols, f->rows);
}

/* esp_display() -> Dict: active (a display shows the console), mono (it's
 * black and white), flip (upside down), rows, cols, font (the name of the one in use, "terminus-10x20") and fonts, a List of
 * Dicts: size (the screen size it gives, "80x24"), font, width, height (its
 * cell in pixels), rows, cols. The system vimrc uses it to set 'background' and
 * colours for the panel. */
void f_esp_display(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    int rows = 0, cols = 0;
    bool on = esp_display_active();
    if (on)
        esp_display_size(&rows, &cols);
    dict_add_bool(rettv->vval.v_dict, "active", on);
    dict_add_bool(rettv->vval.v_dict, "mono", esp_display_mono());
    dict_add_bool(rettv->vval.v_dict, "flip", esp_display_flip());
    dict_add_number(rettv->vval.v_dict, "rows", rows);
    dict_add_number(rettv->vval.v_dict, "cols", cols);
    esp_display_font_info_t f;
    dict_add_string(rettv->vval.v_dict, "font",
                    (char_u *)(esp_display_font_info(esp_display_font(), &f) ? f.name : ""));
    list_T *fonts = list_alloc();
    if (fonts == NULL)
        return;
    for (int i = 0; esp_display_font_info(i, &f); i++) {
        dict_T *d = dict_alloc();
        if (d == NULL)
            break;
        char size[16];
        screen_size(&f, size, sizeof size);
        dict_add_string(d, "size", (char_u *)size);
        dict_add_string(d, "font", (char_u *)f.name);
        dict_add_number(d, "width", f.width);
        dict_add_number(d, "height", f.height);
        dict_add_number(d, "rows", f.rows);
        dict_add_number(d, "cols", f.cols);
        list_append_dict(fonts, d);
    }
    dict_add_list(rettv->vval.v_dict, "fonts", fonts);
}

/*
 * esp_display_flip([{on}]) -> Bool: whether the display is upside down. With
 * {on}: turn it so, repainted at once and kept for the next start.
 */
void f_esp_display_flip(typval_T *argvars, typval_T *rettv)
{
    bool on = esp_display_flip();
    if (argvars[0].v_type != VAR_UNKNOWN) {
        int error = FALSE;
        on = tv_get_bool_chk(&argvars[0], &error) != 0;
        if (error)
            return;
        if (esp_display_set_flip(on) != ESP_OK) {
            emsg("esp_display_flip(): no display");
            return;
        }
    }
    rettv->v_type = VAR_BOOL;
    rettv->vval.v_number = on ? VVAL_TRUE : VVAL_FALSE;
}

/*
 * esp_display_font([{size}]) -> String: the display's screen size, as its font
 * gives it ("80x24"); '' without a display. With {size}: switch to the first
 * font giving that size -- or to a font by name ("terminus-10x20"), which tells
 * apart two that give the same one -- keep it for the next start, and resize
 * Vim to it. There is no SIGWINCH: $LINES and $COLUMNS are where
 * mch_get_shellsize() finds the size, as at startup.
 */
void f_esp_display_font(typval_T *argvars, typval_T *rettv)
{
    esp_display_font_info_t f;
    char size[16];
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = NULL;
    if (argvars[0].v_type != VAR_UNKNOWN) {
        char_u *want = tv_get_string_chk(&argvars[0]);
        if (want == NULL)
            return;
        if (!esp_display_active()) {
            emsg("esp_display_font(): no display");
            return;
        }
        int found = -1;
        for (int i = 0; found < 0 && esp_display_font_info(i, &f); i++)
            if (STRCMP(f.name, want) == 0)
                found = i;
        for (int i = 0; found < 0 && esp_display_font_info(i, &f); i++) {
            screen_size(&f, size, sizeof size);
            if (STRCMP(size, want) == 0)
                found = i;
        }
        if (found < 0) {
            semsg("esp_display_font(): no font gives \"%s\"", want);
            return;
        }
        esp_display_font_info(found, &f);
        esp_err_t e = esp_display_set_font(found);
        if (e != ESP_OK) {
            semsg("esp_display_font(): %s", esp_err_to_name(e));
            return;
        }
        char v[8];
        snprintf(v, sizeof v, "%d", f.rows);
        setenv("LINES", v, 1);
        snprintf(v, sizeof v, "%d", f.cols);
        setenv("COLUMNS", v, 1);
        shell_resized();
    }
    if (esp_display_font_info(esp_display_font(), &f)) {
        screen_size(&f, size, sizeof size);
        rettv->vval.v_string = vim_strsave((char_u *)size);
    }
}

/*
 * esp_time([{settings}]) -> Dict: valid (the clock has been set), now (seconds
 * since 1970), local ("2026-09-26 19:40:12"), zone (its abbreviation), tz (as
 * given), tz_posix, source ("rtc", "ntp", "manual", ""), synced (the last NTP
 * sync, 0: none), ntp, server, rtc (the RTC chip, "" for none), rtc_ok,
 * rtc_time (the chip's time now, -1: lost), zones
 * (the names tz takes). {settings}, applied in this order and kept in NVS:
 * tz, server, ntp (Bool), set (local "YYYY-MM-DD HH:MM[:SS]", or seconds since
 * 1970), sync (seconds to wait for an NTP answer).
 */
void f_esp_time(typval_T *argvars, typval_T *rettv)
{
    char err[160];
    if (argvars[0].v_type == VAR_DICT && argvars[0].vval.v_dict != NULL) {
        dict_T *d = argvars[0].vval.v_dict;
        esp_time_status_t cur;
        esp_time_status(&cur);
        int rc = 0;
        if (dict_has_key(d, "tz"))
            rc = esp_time_set_tz((char *)dict_get_string(d, "tz", FALSE), err, sizeof err);
        if (rc == 0 && (dict_has_key(d, "ntp") || dict_has_key(d, "server")))
            rc = esp_time_set_ntp(dict_has_key(d, "ntp") ? dict_get_bool(d, "ntp", cur.ntp) : cur.ntp,
                                  dict_has_key(d, "server") ? (char *)dict_get_string(d, "server", FALSE) : NULL,
                                  err, sizeof err);
        if (rc == 0 && dict_has_key(d, "set")) {
            dictitem_T *di = dict_find(d, (char_u *)"set", -1);
            rc = di->di_tv.v_type == VAR_NUMBER
                ? esp_time_set((time_t)di->di_tv.vval.v_number, err, sizeof err)
                : esp_time_set_local((const char *)tv_get_string(&di->di_tv), err, sizeof err);
        }
        if (rc == 0 && dict_has_key(d, "sync")) {
            varnumber_T secs = dict_get_number(d, "sync");
            rc = esp_time_sync(secs > 0 ? (int)secs : 10, err, sizeof err);
        }
        if (rc != 0) {
            semsg("esp_time(): %s", err);
            return;
        }
    }
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    esp_time_status_t st;
    esp_time_status(&st);
    dict_T *d = rettv->vval.v_dict;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char buf[40], zone[16];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &lt);
    strftime(zone, sizeof zone, "%Z", &lt);
    dict_add_bool(d, "valid", st.valid);
    dict_add_number(d, "now", (varnumber_T)now);
    dict_add_string(d, "local", (char_u *)buf);
    dict_add_string(d, "zone", (char_u *)zone);
    dict_add_string(d, "tz", (char_u *)st.tz);
    dict_add_string(d, "tz_posix", (char_u *)st.tz_posix);
    dict_add_string(d, "source", (char_u *)st.source);
    dict_add_number(d, "synced", (varnumber_T)st.synced);
    dict_add_bool(d, "ntp", st.ntp);
    dict_add_string(d, "server", (char_u *)st.server);
    dict_add_string(d, "rtc", (char_u *)st.rtc);
    dict_add_bool(d, "rtc_ok", st.rtc_ok);
    dict_add_number(d, "rtc_time", (varnumber_T)st.rtc_time);
    list_T *zones = list_alloc();
    if (zones != NULL) {
        for (const char *const *z = esp_time_zones(); *z; z++)
            list_append_string(zones, (char_u *)*z, -1);
        dict_add_list(d, "zones", zones);
    }
}
