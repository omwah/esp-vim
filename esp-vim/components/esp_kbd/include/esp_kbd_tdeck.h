/*
 * esp_kbd_tdeck: the LilyGo T-Deck's (and T-Deck Plus's) keyboard and
 * trackball, as HID usages.
 *
 * The keyboard is 35 keys in a 5 x 7 matrix, scanned by an ESP32-C3 that
 * answers on I2C (esp_kbd_tdeck_dev.c reads it): in its raw mode, five bytes,
 * one per matrix column, a bit per row. This turns each key going down and up
 * into the keys-held reports esp_kbd_input() takes, so it types exactly as a
 * USB or Bluetooth keyboard does. Pure: no I/O, so the host test
 * (esp-vim/test/kbd) runs it.
 *
 * The layout, as printed on the keys:
 *
 *   q w e r t y u i o p
 *   a s d f g h j k l Bksp
 *   Alt z x c v b n m $ Enter
 *   Shift Mic Space Sym Shift
 *
 * There is no Esc, Ctrl or Tab, and Vim needs all three: Mic is Ctrl, Alt is
 * Esc, and Sym-Space is Tab. Sym gives the second legends printed on the
 * keys (Sym-Mic is 0), and Delete on Bksp. Sym with Shift gives what isn't
 * printed anywhere:
 *
 *   q ~   w `   e ^   r %   t [   y ]   u {   i }   o =   p &
 *   a |   s \   d <   f >   h j k l: Left Down Up Right
 *   b PgUp   n PgDn   g Home   m End
 *
 * Each modifier (Shift, Mic as Ctrl, Sym) works held, as a chord, or tapped:
 * a tap latches it for the next key only, and a second tap drops it -- Mic
 * then w, one finger at a time. Shift tapped twice is Caps Lock, and a third
 * tap ends it.
 *
 * The trackball's four hall sensors each toggle a pin as it rolls; every
 * ESP_VIM_KBD_TDECK_BALL_STEP toggles one way is one arrow key.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_kbd_hid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_KBD_TDECK_COLS 5            /* the C3's matrix: a byte per column */
#define ESP_KBD_TDECK_ROWS 7            /* ... a bit per row */
#define ESP_KBD_TDECK_KEYS (ESP_KBD_TDECK_COLS * ESP_KBD_TDECK_ROWS)

typedef struct {
    uint8_t usage[ESP_KBD_TDECK_KEYS];  /* each key held: the usage it pressed (0: up) */
    uint8_t mods[ESP_KBD_TDECK_KEYS];   /* ... and the modifiers it went down with */
    uint8_t held;                       /* modifier keys held: Ctrl, Shift bits */
    uint8_t latched;                    /* tapped, for the next key */
    uint8_t shifts;                     /* Shift keys down (there are two) */
    bool sym_held, sym_latched;
    bool caps;
    int8_t tap;                         /* a modifier key down with no key since; -1 none */
    int8_t newest;                      /* the key pressed last, still held; -1 none */
} esp_kbd_tdeck_t;

void esp_kbd_tdeck_reset(esp_kbd_tdeck_t *st);

/* One key event, by its place in the C3's matrix. True when the keys held
 * changed: *out is then the report to hand to esp_kbd_input(). Out-of-range
 * columns and rows are ignored. */
bool esp_kbd_tdeck_event(esp_kbd_tdeck_t *st, int col, int row, bool pressed,
                         esp_kbd_keys_t *out);

/* The trackball. */
enum { ESP_KBD_TDECK_UP, ESP_KBD_TDECK_DOWN, ESP_KBD_TDECK_LEFT, ESP_KBD_TDECK_RIGHT };

typedef struct {
    int16_t count[4];                   /* toggles each way, since the last step */
} esp_kbd_tdeck_ball_t;

/* {toggles} more of one sensor ({dir}, ESP_KBD_TDECK_*): how many arrow key
 * presses that makes, every {step} toggles. Rolling one way forgets what was
 * counted the other way, so a wobble doesn't add up to a step. */
int esp_kbd_tdeck_ball(esp_kbd_tdeck_ball_t *b, int dir, int toggles, int step);

/* The arrow key's usage for a trackball direction. */
uint8_t esp_kbd_tdeck_ball_usage(int dir);

#ifdef __cplusplus
}
#endif
