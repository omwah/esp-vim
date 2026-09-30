/*
 * esp_usbkbd: see include/esp_usbkbd.h.
 *
 * Two tasks of ours beside the HID driver's own: one runs the USB Host
 * Library's events, the other opens each keyboard as the driver finds it (the
 * driver's callback may not open devices itself, Espressif's example says).
 * Reports arrive on the driver's task and go straight to esp_kbd_input().
 */

#include "esp_usbkbd.h"

#include "sdkconfig.h"

#if CONFIG_ESP_VIM_USB_KBD

#include <stdio.h>
#include <string.h>
#include "esp_kbd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

static const char *TAG = "esp_usbkbd";

#define MAX_KBDS    4
#define REPORT_ID(i) (0x0B00 + (i))     /* each keyboard's keys, among esp_kbd's reports */

/* One keyboard: boot reports, or its own layout from its report descriptor. */
typedef struct {
    hid_host_device_handle_t handle;
    bool boot;
    esp_kbd_layouts_t layouts;
} kbd_t;

static kbd_t s_kbd[MAX_KBDS];
static QueueHandle_t s_found;

/* Whether any keyboard is plugged in, for esp_kbd_wired(). */
static void update_wired(void)
{
    bool any = false;
    for (int i = 0; i < MAX_KBDS; i++)
        any |= s_kbd[i].handle != NULL;
    esp_kbd_wired(ESP_KBD_WIRED_USB, any);
}

static int slot_of(hid_host_device_handle_t h)
{
    for (int i = 0; i < MAX_KBDS; i++)
        if (s_kbd[i].handle == h)
            return i;
    return -1;
}

static void on_report(int i, const uint8_t *data, size_t len)
{
    kbd_t *k = &s_kbd[i];
    esp_kbd_keys_t keys;
    bool ok;
    if (k->boot) {
        ok = esp_kbd_decode_boot(data, len, &keys);
    } else {
        /* With report IDs the first byte is the ID. */
        bool ids = k->layouts.n > 0 && k->layouts.report[0].id != 0;
        ok = ids ? len > 0 && esp_kbd_decode(&k->layouts, data[0], data + 1, len - 1, &keys)
                 : esp_kbd_decode(&k->layouts, 0, data, len, &keys);
    }
    if (ok)
        esp_kbd_input(REPORT_ID(i), &keys);
}

/* On the HID driver's task. */
static void on_interface(hid_host_device_handle_t h, const hid_host_interface_event_t ev, void *arg)
{
    int i = slot_of(h);
    if (i < 0)
        return;
    switch (ev) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[64];
        size_t len = 0;
        if (hid_host_device_get_raw_input_report_data(h, data, sizeof data, &len) == ESP_OK)
            on_report(i, data, len);
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED: {
        esp_kbd_keys_t none = { .has_mods = true };
        esp_kbd_input(REPORT_ID(i), &none);     /* what it held is let go */
        hid_host_device_close(h);
        s_kbd[i].handle = NULL;
        update_wired();
        printf("ESPVIM-KBD USB keyboard %d: unplugged\n", i);
        break;
    }
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "keyboard %d: transfer error", i);
        break;
    default:
        break;
    }
}

/* On the HID driver's task: a HID interface appeared. Opened on ours. */
static void on_driver(hid_host_device_handle_t h, const hid_host_driver_event_t ev, void *arg)
{
    if (ev == HID_HOST_DRIVER_EVENT_CONNECTED)
        xQueueSend(s_found, &h, 0);
}

static void open_keyboard(hid_host_device_handle_t h)
{
    hid_host_dev_params_t p;
    if (hid_host_device_get_params(h, &p) != ESP_OK)
        return;
    if (p.proto != HID_PROTOCOL_KEYBOARD && p.proto != HID_PROTOCOL_NONE)
        return;                         /* a mouse: not ours */
    int i = slot_of(NULL);
    if (i < 0) {
        ESP_LOGW(TAG, "more than %d keyboards: this one is left alone", MAX_KBDS);
        return;
    }
    kbd_t *k = &s_kbd[i];
    memset(k, 0, sizeof *k);
    const hid_host_device_config_t cfg = { .callback = on_interface };
    k->handle = h;                      /* before any of its events can come */
    if (hid_host_device_open(h, &cfg) != ESP_OK) {
        k->handle = NULL;
        return;
    }
    k->boot = p.sub_class == HID_SUBCLASS_BOOT_INTERFACE && p.proto == HID_PROTOCOL_KEYBOARD;
    if (k->boot) {
        hid_class_request_set_protocol(h, HID_REPORT_PROTOCOL_BOOT);
        hid_class_request_set_idle(h, 0, 0);    /* reports on change only; we repeat */
    } else {
        size_t n = 0;
        const uint8_t *map = hid_host_get_report_descriptor(h, &n);
        if (map == NULL || esp_kbd_parse_map(map, n, &k->layouts) == 0) {
            hid_host_device_close(h);   /* no keyboard reports: a consumer-keys interface */
            k->handle = NULL;
            return;
        }
    }
    if (hid_host_device_start(h) != ESP_OK) {
        hid_host_device_close(h);
        k->handle = NULL;
        return;
    }
    update_wired();
    hid_host_dev_info_t info = {0};
    hid_host_get_device_info(h, &info);
    printf("ESPVIM-KBD USB keyboard %d: %04x:%04x, %s reports\n", i, info.VID, info.PID,
           k->boot ? "boot" : "its own");
}

static void found_task(void *arg)
{
    hid_host_device_handle_t h;
    for (;;)
        if (xQueueReceive(s_found, &h, portMAX_DELAY) == pdTRUE)
            open_keyboard(h);
}

static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

esp_err_t esp_usbkbd_start(void)
{
    s_found = xQueueCreate(4, sizeof(hid_host_device_handle_t));
    if (s_found == NULL)
        return ESP_ERR_NO_MEM;
    const usb_host_config_t host = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LOWMED };
    esp_err_t e = usb_host_install(&host);
    if (e != ESP_OK)
        return e;
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL, 1) != pdPASS
            || xTaskCreatePinnedToCore(found_task, "usb_kbd", 4096, NULL, 5, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    const hid_host_driver_config_t hid = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 1,
        .callback = on_driver,
    };
    e = hid_host_install(&hid);
    if (e == ESP_OK)
        ESP_LOGI(TAG, "USB host: waiting for keyboards");
    return e;
}

#else

esp_err_t esp_usbkbd_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
