/*
 * esp_touch: a capacitive touch panel (FocalTech FT6336G / FT6x06, as on the
 * Hosyond ES3C28P; a GT911; or the board's own), in screen coordinates.
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

/* What the panel is and how its points are turned to the screen: the swap
 * comes first, then the mirrors. addr, config_version and switch1 are -1 where
 * they don't apply (config_version and switch1: a GT911's configuration). */
typedef struct {
    bool available;
    const char *chip;           /* "GT911", "FT6336", "board" */
    int addr;
    int config_version;
    int switch1;                /* Module_Switch1: its own axis reversal bits */
    bool swap, mirror_x, mirror_y;
    bool saved;                 /* the turn is :EspTouch's, from NVS, not the board's */
} esp_touch_info_t;

void esp_touch_get_info(esp_touch_info_t *info);

/* Turn points this way from now on, and at every start (NVS). */
esp_err_t esp_touch_set_turn(bool swap, bool mirror_x, bool mirror_y);

/* Back to the board's own turn; the saved one is forgotten. */
esp_err_t esp_touch_reset_turn(void);

#ifdef __cplusplus
}
#endif
