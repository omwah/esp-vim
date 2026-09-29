/*
 * esp_kbd_hid: keyboard reports, from any keyboard, to terminal bytes.
 *
 * Pure C with no ESP-IDF in it, so that it builds and is tested on the host
 * too (esp-vim/test/kbd/, pixi run kbd-test).
 *
 *  - A keyboard describes its reports in its HID report map. The boot layout
 *    (a modifier byte, a reserved byte, six key codes) is only one of them:
 *    many keyboards also send an "N-key rollover" report, a bitmap with one bit
 *    per key. esp_kbd_parse_map() learns where a keyboard's key fields are,
 *    and esp_kbd_decode() reads any of its reports into one form: modifiers
 *    and the keys held.
 *  - esp_kbd_key_bytes() is what a terminal sends for one key press: xterm's
 *    sequences, with its modifier forms (ESC [ 1 ; 5 C is Ctrl-Right), which
 *    Vim's builtin xterm termcap decodes.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Modifier bits, as in the boot report's first byte. */
#define ESP_KBD_MOD_CTRL   (0x01 | 0x10)
#define ESP_KBD_MOD_SHIFT  (0x02 | 0x20)
#define ESP_KBD_MOD_ALT    (0x04 | 0x40)

#define ESP_KBD_MAX_KEYS     16     /* held at once; more are ignored */
#define ESP_KBD_MAX_FIELDS   4      /* key fields in one report */
#define ESP_KBD_MAX_REPORTS  6      /* keyboard reports in one map */

/* One field of Keyboard/Keypad usages (usage page 7) in a report. */
typedef struct {
    uint16_t bit;           /* offset in the report, after any report ID */
    uint16_t count;
    uint8_t  size;          /* bits per entry */
    bool     array;         /* entries are key codes; else one bit per usage */
    int16_t  base;          /* bitmaps: the usage of the first bit; arrays:
                               added to each entry (usage minimum - logical
                               minimum, 0 on every keyboard seen) */
} esp_kbd_field_t;

typedef struct {
    uint8_t id;             /* report ID, 0 if the map uses none */
    uint16_t bits;          /* the report's length */
    uint8_t nfields;
    esp_kbd_field_t field[ESP_KBD_MAX_FIELDS];
} esp_kbd_layout_t;

typedef struct {
    uint8_t n;
    esp_kbd_layout_t report[ESP_KBD_MAX_REPORTS];
} esp_kbd_layouts_t;

/* The state a report describes. */
typedef struct {
    uint8_t mods;
    uint8_t n;
    uint8_t key[ESP_KBD_MAX_KEYS];
} esp_kbd_keys_t;

/* Learn the keyboard input reports from a HID report map. Returns how many
 * were found (0: none; decode with esp_kbd_decode_boot()). */
int esp_kbd_parse_map(const uint8_t *map, size_t len, esp_kbd_layouts_t *out);

/* Decode report {id}'s data (without the ID byte). False if the map has no
 * keyboard fields in that report, or the keyboard says too many keys are down
 * (ErrorRollOver): keep the last state then. */
bool esp_kbd_decode(const esp_kbd_layouts_t *l, uint8_t id, const uint8_t *data, size_t len,
                    esp_kbd_keys_t *out);

/* The boot layout, with or without its reserved byte. */
bool esp_kbd_decode_boot(const uint8_t *r, size_t len, esp_kbd_keys_t *out);

/* The bytes a press of {usage} sends, with {mods} held and Caps Lock {caps}:
 * at most 16. 0: the key sends nothing. */
size_t esp_kbd_key_bytes(uint8_t usage, uint8_t mods, bool caps, char out[16]);

/* A short description of the layouts, for status displays:
 * "1: mods@0 keys6@16; 2: mods@0 map0+152@8". */
void esp_kbd_describe(const esp_kbd_layouts_t *l, char *out, size_t outlen);

#ifdef __cplusplus
}
#endif
