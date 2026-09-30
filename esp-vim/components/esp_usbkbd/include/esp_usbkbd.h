/*
 * esp_usbkbd: keyboards on the USB host port (ESP_VIM_USB_KBD).
 *
 * Their keys go to components/esp_kbd, as a Bluetooth keyboard's do. The
 * port's 5 V is the board's business (esp_board: the Tab5 switches it).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the USB Host Library and the HID class driver, and take keyboards as
 * they are plugged in. ESP_ERR_NOT_SUPPORTED in builds without it. */
esp_err_t esp_usbkbd_start(void);

#ifdef __cplusplus
}
#endif
