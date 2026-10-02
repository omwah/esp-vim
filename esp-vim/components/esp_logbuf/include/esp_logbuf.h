/*
 * esp_logbuf: ESP-IDF's log while Vim has the console.
 *
 * Log lines (ESP_LOGx, from any task) normally go to the console, which is
 * where Vim draws its screen: over a serial console they land in the middle
 * of it. While capture is on -- a Vim session -- they go to a ring buffer in
 * PSRAM instead, the most recent 16 KB (4 KB without PSRAM), which :EspLog
 * shows. Off -- boot, and between sessions -- they print as before, and are
 * kept too. A panic's output doesn't come through
 * here and always reaches the console.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the hook. Once, early in app_main. */
void esp_logbuf_init(void);

/* Capture on (into the buffer) or off (to the console). */
void esp_logbuf_capture(bool on);

/* The buffer's text, whole lines, oldest first, NUL-terminated: at most n - 1
 * bytes. Returns its length. */
size_t esp_logbuf_copy(char *out, size_t n);

/* Empty it. */
void esp_logbuf_clear(void);

/* Lines captured since boot, including those since dropped. */
unsigned esp_logbuf_count(void);

#ifdef __cplusplus
}
#endif
