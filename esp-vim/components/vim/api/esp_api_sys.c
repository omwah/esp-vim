/*
 * esp_info(), esp_heap(), esp_tasks(), esp_reboot(): the system builtins
 * behind :EspInfo, :EspHeap, :EspTasks and :EspReboot.
 */

#include "vim.h"
#include "version.h"          /* VIM_VERSION_SHORT */
#include "esp_vim_api.h"
#include "esp_vim_port.h"

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
