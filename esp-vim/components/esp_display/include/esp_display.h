/*
 * esp_display: the console on an LCD (docs/PLAN.md Phases 10 and 10b).
 *
 * Vim still believes it's talking to an xterm. Its output is also fed here;
 * libvterm (Vim's own bundled copy) keeps the cell grid, and a task on the
 * other core paints the damaged rows to the panel. Input stays with the console
 * (serial for now; keyboards in later phases).
 *
 * Its own component, like esp_net: the panel outlives Vim sessions, and it
 * knows nothing about Vim. Builds without CONFIG_ESP_VIM_DISPLAY get stubs.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the panel and the terminal. Call once at boot. */
esp_err_t esp_display_init(void);

/* True once esp_display_init() has succeeded. */
bool esp_display_active(void);

/* The terminal's size in character cells. */
void esp_display_size(int *rows, int *cols);

/* Console output to show. Blocks only while the display falls behind. */
void esp_display_write(const void *buf, size_t len);

/*
 * A touch on the panel, as a mouse in the terminal -- only while the program on
 * it (Vim) has asked for mouse reports, so a stray tap never types anything:
 *   tap                          a click
 *   swipe up or down             the scroll wheel, a step per 3 rows moved
 *   slide sideways, or press,    a drag (selects, in Vim)
 *   hold still, then move
 * Reports go to the console input (esp_kbd) as xterm SGR mouse sequences. For
 * esp_touch_set_handler(): ev is an esp_touch_event_t.
 */
void esp_display_touch(int ev, int x, int y, void *ctx);

/*
 * A full-screen overlay in a big font (12x24 cells: 26x10 on the ES3C28P), for
 * the Bluetooth pairing screen. While it's up the terminal isn't drawn, though
 * libvterm keeps its screen current; ending it repaints the terminal. Any task.
 */
bool esp_display_overlay_begin(int *rows, int *cols);   /* false without a display */
void esp_display_overlay_text(int row, int col, const char *text, bool inverse);  /* ASCII */
void esp_display_overlay_clear(void);
void esp_display_overlay_cell_at(int x, int y, int *row, int *col);
void esp_display_overlay_end(void);

#ifdef __cplusplus
}
#endif
