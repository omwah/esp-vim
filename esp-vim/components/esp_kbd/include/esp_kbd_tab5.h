/*
 * esp_kbd_tab5: the M5Stack Tab5 keyboard accessory's keys, as HID usages.
 *
 * The keyboard (an STM32 on I2C, in its "normal" mode) reports each key's
 * row and column as it goes down and up: 5 rows of 14, no F-key row, and four
 * modifier keys of its own -- Sym, Aa, Ctrl, Alt. This turns those events
 * into the keys-held reports esp_kbd_input() takes, so the Tab5 keyboard types
 * exactly as a USB or Bluetooth one does (the same bytes, xterm sequences and
 * key repeat). Pure: no I/O, so the host test (esp-vim/test/kbd) runs it.
 *
 * The layout, as printed on the keys:
 *
 *   Esc 1 2 3 4 5 6 7 8 9 0 - + Del
 *   `  ! @ # $ % ^ & * ( ) [ ] \
 *   Tab q w e r t y u i o p ; ' BS
 *   Sym Aa a s d f g h j k l Up _ Enter
 *   Ctrl Alt z x c v b n m . Left Down Right Space
 *
 * Sym gives the second legends -- ~ ? / < > { } | : " = , -- and what the
 * keyboard lacks: F1-F12 on 1..0 - +, Insert on Del, and on the arrows PgUp,
 * Home, PgDn and End. Aa shifts letters and the keys that aren't characters
 * (Shift-Tab, Shift-arrows); a symbol key sends its own legend either way.
 *
 * Each modifier works held, as a chord, or tapped: a tap latches it for the
 * next key only, and a second tap drops it. Aa tapped twice is Caps Lock, and a
 * third tap ends it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_kbd_hid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_KBD_TAB5_ROWS 5
#define ESP_KBD_TAB5_COLS 14
#define ESP_KBD_TAB5_KEYS (ESP_KBD_TAB5_ROWS * ESP_KBD_TAB5_COLS)

/* Indicator bits, for the keyboard's two LEDs. */
#define ESP_KBD_TAB5_IND_SYM   0x01
#define ESP_KBD_TAB5_IND_SHIFT 0x02     /* Aa held or latched */
#define ESP_KBD_TAB5_IND_CAPS  0x04
#define ESP_KBD_TAB5_IND_CTRL  0x08
#define ESP_KBD_TAB5_IND_ALT   0x10

typedef struct {
    uint8_t usage[ESP_KBD_TAB5_KEYS];   /* each key held: the usage it pressed (0: up) */
    uint8_t mods[ESP_KBD_TAB5_KEYS];    /* ... and the modifiers it went down with */
    uint8_t held;                       /* modifier keys held: ESP_KBD_MOD_* bits */
    uint8_t latched;                    /* tapped, for the next key */
    bool sym_held, sym_latched;
    bool caps;
    int8_t tap;                         /* a modifier key down with no key since; -1 none */
    int8_t newest;                      /* the key pressed last, still held; -1 none */
} esp_kbd_tab5_t;

void esp_kbd_tab5_reset(esp_kbd_tab5_t *st);

/* One key event. True when the keys held changed: *out is then the report to
 * hand to esp_kbd_input(). Out-of-range rows and columns are ignored. */
bool esp_kbd_tab5_event(esp_kbd_tab5_t *st, int row, int col, bool pressed, esp_kbd_keys_t *out);

/* ESP_KBD_TAB5_IND_* bits: which modifiers are in effect for the next key. */
unsigned esp_kbd_tab5_indicators(const esp_kbd_tab5_t *st);

#ifdef __cplusplus
}
#endif
