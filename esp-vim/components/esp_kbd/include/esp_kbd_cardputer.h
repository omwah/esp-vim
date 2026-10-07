/*
 * esp_kbd_cardputer: the M5Stack Cardputer ADV's built-in keys, as HID usages.
 *
 * The keyboard is 4 rows of 14 keys behind a TCA8418 (esp_kbd_cardputer_dev.c
 * reads it). This turns each key going down and up into the keys-held reports
 * esp_kbd_input() takes, so it types exactly as a USB or Bluetooth keyboard
 * does (the same bytes, xterm sequences and key repeat). Pure: no I/O, so the
 * host test (esp-vim/test/kbd) runs it.
 *
 * The layout, as printed on the keys (row, then column):
 *
 *   `   1 2 3 4 5 6 7 8 9 0 - = Bksp
 *   Tab q w e r t y u i o p [ ] \
 *   Fn  Shift a s d f g h j k l ; ' Enter
 *   Ctrl Opt Alt z x c v b n m , . / Space
 *
 * There is no Esc key, and Vim needs one all the time: ` is Esc, Shift-` is
 * ~ as printed, and Fn-` is `. The arrows printed on ; , . / work the same
 * way: plain, the keys are Up, Left, Down and Right; Shift gives : < > ?, and
 * Fn the characters themselves. Elsewhere Shift gives the printed second
 * legends, as on any US keyboard, and Fn what the keyboard lacks: F1-F10 on
 * 1..0, F11 and F12 on - and =, Delete on Bksp, and PgUp and PgDn on [ and ].
 * Alt is Meta in Vim; Opt does nothing yet.
 *
 * Each modifier works held, as a chord, or tapped: a tap latches it for the
 * next key only, and a second tap drops it -- Ctrl then W, one finger at a
 * time. Shift tapped twice is Caps Lock, and a third tap ends it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_kbd_hid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_KBD_CARDPUTER_ROWS 4
#define ESP_KBD_CARDPUTER_COLS 14
#define ESP_KBD_CARDPUTER_KEYS (ESP_KBD_CARDPUTER_ROWS * ESP_KBD_CARDPUTER_COLS)

typedef struct {
    uint8_t usage[ESP_KBD_CARDPUTER_KEYS];  /* each key held: the usage it pressed (0: up) */
    uint8_t mods[ESP_KBD_CARDPUTER_KEYS];   /* ... and the modifiers it went down with */
    uint8_t held;                           /* modifier keys held: ESP_KBD_MOD_* bits */
    uint8_t latched;                        /* tapped, for the next key */
    bool fn_held, fn_latched;
    bool caps;
    int8_t tap;                             /* a modifier key down with no key since; -1 none */
    int8_t newest;                          /* the key pressed last, still held; -1 none */
} esp_kbd_cardputer_t;

void esp_kbd_cardputer_reset(esp_kbd_cardputer_t *st);

/* One key event. True when the keys held changed: *out is then the report to
 * hand to esp_kbd_input(). Out-of-range rows and columns are ignored. */
bool esp_kbd_cardputer_event(esp_kbd_cardputer_t *st, int row, int col, bool pressed,
                             esp_kbd_keys_t *out);

/* The TCA8418's key number (1-80, from its event FIFO: row * 10 + column + 1
 * of its 7x8 matrix) as the row and column printed above. False if it isn't
 * one of the 56 keys. */
bool esp_kbd_cardputer_key(int tca_key, int *row, int *col);

#ifdef __cplusplus
}
#endif
