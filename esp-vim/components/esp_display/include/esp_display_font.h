/* The console font, generated at build time by scripts/bdf2c.py. */

#pragma once

#include <stdint.h>

typedef struct {
    uint8_t width, height;          /* cell size in pixels; width <= 8 */
    uint16_t count;
    const uint32_t *codepoints;     /* sorted */
    const uint8_t *bitmaps;         /* count * height rows, MSB leftmost */
} esp_display_font_t;

extern const esp_display_font_t esp_display_font;
