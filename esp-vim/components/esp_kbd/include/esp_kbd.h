/*
 * esp_kbd: keyboards as console input (docs/PLAN.md Phases 10 and 11).
 *
 * Keyboards deliver HID reports; this turns them into the bytes a terminal
 * would send -- characters, control codes, and the xterm escape sequences
 * Vim's builtin xterm termcap expects -- and queues them for the console. The
 * Vim port reads the queue alongside the serial console, so keys from either
 * reach Vim. Typematic repeat is done here: HID keyboards don't repeat keys
 * themselves, the host does.
 *
 * Its own component: it outlives Vim sessions and knows nothing about Vim.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_kbd_init(void);

/*
 * One HID keyboard input report: a modifier byte, then up to six key usages
 * (the boot-protocol layout, with or without its reserved byte). Called from
 * whatever task the keyboard's driver runs on.
 */
void esp_kbd_report(const uint8_t *report, size_t len);

/* Bytes for the console as they are: other input devices (touch-as-mouse
 * sends xterm mouse reports). Dropped if the queue is full. */
void esp_kbd_push(const char *bytes, size_t len);

/* All keys released (the keyboard went away): stop any repeat. */
void esp_kbd_release_all(void);

/* For the console: is a key waiting, and take up to {len} bytes of keys. */
bool esp_kbd_pending(void);
int esp_kbd_read(void *buf, size_t len);

#ifdef __cplusplus
}
#endif
