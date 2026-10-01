/*
 * esp_board: the board's own hardware, below the drivers that use it.
 *
 * Most boards need nothing here: their pins are in Kconfig and each driver
 * sets up its own. The M5Stack Tab5 is different. One I2C bus (GPIO31/32)
 * carries two IO expanders that switch the power of the ESP32-C6 (WiFi), the
 * USB-A port, the LCD, the touch controller and the speaker, and its panel is
 * one of three MIPI-DSI revisions, told apart by the touch controller. That
 * is set up once, here, through Espressif's board support package; the
 * display, touch, clock and :EspI2cScan share the bus through
 * esp_board_i2c_bus().
 *
 * Boards without a board layer get no-ops, ESP_ERR_NOT_SUPPORTED and NULL.
 */
#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* First thing in app_main, before the network (the C6 must be powered before
 * esp-hosted talks to it). Prints one ESPVIM-BOARD line saying what it found.
 * Carries on when parts of it fail: a board with a dead expander still gets a
 * serial console. */
esp_err_t esp_board_init(void);

/* "M5Stack Tab5", or NULL: a board without a board layer. */
const char *esp_board_name(void);

/* The board's I2C bus, if it has one on these pins (NULL otherwise): open for
 * the life of the program, shared. */
i2c_master_bus_handle_t esp_board_i2c_bus(int sda, int scl);

/* The panel, as the board wires it: created, reset, set up and switched on,
 * with a frame buffer (*width x *height pixels, RGB565) the caller draws into.
 * The panel's native orientation: the Tab5's is portrait, 720 x 1280. */
esp_err_t esp_board_panel_new(esp_lcd_panel_handle_t *panel, int *width, int *height);

/* The panel's controller: "ILI9881C", "ST7123", "ST7121"; "" before
 * esp_board_panel_new(), or when it found none. */
const char *esp_board_panel_name(void);

/* Backlight, 0 (off) to 100. */
esp_err_t esp_board_backlight(int percent);

/* The touch controller that goes with the panel. esp_board_touch_read(): 1 and
 * the first point, in the panel's native coordinates; 0 nothing touching; -1
 * the controller didn't answer. */
esp_err_t esp_board_touch_init(void);
int esp_board_touch_read(int *x, int *y);

/* The USB-A port's 5 V (the Tab5 switches it through an expander).
 * ESP_ERR_INVALID_STATE when the expander isn't there: then neither is the
 * port's power, and the USB host is better not started. */
esp_err_t esp_board_usb_power(bool on);

/* Whether the radio co-processor (the Tab5's C6) has its power. esp-hosted
 * 1.x aborts when the C6 never answers, so nothing asks it otherwise. True
 * on boards without one, where nothing asks. */
bool esp_board_coprocessor_powered(void);

#ifdef __cplusplus
}
#endif
