/*
 * esp_touch: a capacitive touch panel (FocalTech FT6336G / FT6x06, as on the
 * Hosyond ES3C28P), in screen coordinates.
 *
 * One handler receives touches -- the display's touch-as-mouse (esp_display)
 * or, while it's up, the Bluetooth pairing overlay -- on the touch task. Its
 * own component: it outlives Vim sessions and knows nothing about Vim.
 * Builds without CONFIG_ESP_VIM_TOUCH get stubs.
 */

#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_TOUCH_DOWN,             /* finger down */
    ESP_TOUCH_MOVE,             /* still down: reported every poll (~60 Hz), moved or not */
    ESP_TOUCH_UP,               /* finger lifted (x, y: where it was last) */
} esp_touch_event_t;

typedef void (*esp_touch_handler_t)(esp_touch_event_t ev, int x, int y, void *ctx);

/* Reset the panel and start the touch task. Call once at boot. */
esp_err_t esp_touch_init(void);

bool esp_touch_available(void);

/* Where touches go; replaces the previous handler (NULL: nowhere). */
void esp_touch_set_handler(esp_touch_handler_t handler, void *ctx);

/* The touch panel's I2C bus if it is on {sda}/{scl}, else NULL -- so the I2C
 * commands can share it rather than claim the same pins twice. */
i2c_master_bus_handle_t esp_touch_i2c_bus(int sda, int scl);

#ifdef __cplusplus
}
#endif
