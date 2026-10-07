/*
 * esp_kbd_cardputer: see include/esp_kbd_cardputer.h.
 *
 * As esp_kbd_tab5.c: each key is given its usage and modifiers when it goes
 * down, from the layer and modifiers in effect then, and keeps them until it
 * comes up -- a key held while Fn is let go still repeats what it began as.
 */

#include "esp_kbd_cardputer.h"

#include <string.h>

#define SHIFT   0x02                    /* left Shift, as a report sets it */
#define CTRL    0x01
#define ALT     0x04

enum { LETTER, CHAR, OTHER, MOD };      /* how Shift acts on a key; MOD: a modifier key */

typedef struct {
    uint8_t kind;
    uint8_t usage;                      /* the plain key */
    uint8_t fn;                         /* with Fn (0: as plain), never shifted */
    uint8_t shifted;                    /* with Shift, if not the plain usage shifted */
} key_t_;

#define L(u)        { LETTER, u, 0, 0 }
#define C(u)        { CHAR, u, 0, 0 }
#define CF(u, f)    { CHAR, u, f, 0 }
#define OF(u, f)    { OTHER, u, f, 0 }
#define M(bit)      { MOD, bit, 0, 0 }
/* A key whose printed character is its second meaning: plain, the arrow (or
 * Esc) printed beside it; Shift, its shifted character; Fn, the character. */
#define ARROW(u, c) { OTHER, u, c, c }

#define FN_BIT  0x80                    /* M(): Fn, which is no report modifier */
#define NONE    0x00                    /* M(NONE): Opt, which does nothing yet */

/* HID usages (page 7): a-z 0x04-0x1d, 1-9 0x1e-0x26, 0 0x27, F1-F12 0x3a-0x45,
 * Delete 0x4c, PgUp 0x4b, PgDn 0x4e, Right/Left/Down/Up 0x4f-0x52. */
static const key_t_ s_keys[ESP_KBD_CARDPUTER_KEYS] = {
    /* ` 1 2 3 4 5 6 7 8 9 0 - = Bksp; ` is Esc, Shift-` ~, Fn-` `;
     * Fn: F1-F12, Delete */
    ARROW(0x29, 0x35),
    CF(0x1e, 0x3a), CF(0x1f, 0x3b), CF(0x20, 0x3c), CF(0x21, 0x3d), CF(0x22, 0x3e),
    CF(0x23, 0x3f), CF(0x24, 0x40), CF(0x25, 0x41), CF(0x26, 0x42), CF(0x27, 0x43),
    CF(0x2d, 0x44), CF(0x2e, 0x45), OF(0x2a, 0x4c),
    /* Tab q w e r t y u i o p [ ] \; Fn: PgUp PgDn on [ ] */
    OF(0x2b, 0), L(0x14), L(0x1a), L(0x08), L(0x15), L(0x17), L(0x1c), L(0x18), L(0x0c),
    L(0x12), L(0x13), CF(0x2f, 0x4b), CF(0x30, 0x4e), C(0x31),
    /* Fn Shift a s d f g h j k l ; ' Enter; ; is Up, Shift-; :, Fn-; ; */
    M(FN_BIT), M(SHIFT), L(0x04), L(0x16), L(0x07), L(0x09), L(0x0a), L(0x0b), L(0x0d),
    L(0x0e), L(0x0f), ARROW(0x52, 0x33), C(0x34), OF(0x28, 0),
    /* Ctrl Opt Alt z x c v b n m , . / Space; , . / are Left Down Right, as
     * printed on them; Shift gives < > ?, Fn , . / */
    M(CTRL), M(NONE), M(ALT), L(0x1d), L(0x1b), L(0x06), L(0x19), L(0x05), L(0x11),
    L(0x10), ARROW(0x50, 0x36), ARROW(0x51, 0x37), ARROW(0x4f, 0x38), OF(0x2c, 0),
};

void esp_kbd_cardputer_reset(esp_kbd_cardputer_t *st)
{
    memset(st, 0, sizeof *st);
    st->tap = -1;
    st->newest = -1;
}

bool esp_kbd_cardputer_key(int tca_key, int *row, int *col)
{
    /* The TCA8418 numbers its keys row * 10 + column + 1 (rows 0-7, columns
     * 0-9). The Cardputer wires 7 rows of 8 columns, and M5Stack's library
     * maps them to the printed grid as below. */
    int k = tca_key - 1;
    int r = k / 10, c = k % 10;
    if (k < 0 || r >= 7 || c >= 8)
        return false;
    int idx = r * 8 + c;
    *row = idx % 4;
    *col = idx / 4;
    return true;
}

static void report(const esp_kbd_cardputer_t *st, esp_kbd_keys_t *out)
{
    memset(out, 0, sizeof *out);
    out->has_mods = true;
    for (int i = 0; i < ESP_KBD_CARDPUTER_KEYS && out->n < ESP_KBD_MAX_KEYS; i++)
        if (st->usage[i])
            out->key[out->n++] = st->usage[i];
    if (st->newest >= 0)
        out->mods = st->mods[st->newest];
}

/* A modifier key tapped alone: latch it for the next key, or let it go. Shift
 * goes on to Caps Lock. */
static void tapped(esp_kbd_cardputer_t *st, uint8_t bit)
{
    if (bit == FN_BIT) {
        st->fn_latched = !st->fn_latched;
    } else if (bit == SHIFT) {
        if (st->caps) {
            st->caps = false;
        } else if (st->latched & SHIFT) {
            st->latched &= ~SHIFT;
            st->caps = true;
        } else {
            st->latched |= SHIFT;
        }
    } else {
        st->latched ^= bit;
    }
}

bool esp_kbd_cardputer_event(esp_kbd_cardputer_t *st, int row, int col, bool pressed,
                             esp_kbd_keys_t *out)
{
    if (row < 0 || row >= ESP_KBD_CARDPUTER_ROWS || col < 0 || col >= ESP_KBD_CARDPUTER_COLS)
        return false;
    int i = row * ESP_KBD_CARDPUTER_COLS + col;
    const key_t_ *k = &s_keys[i];

    if (k->kind == MOD) {
        uint8_t bit = k->usage;
        if (bit == NONE)
            return false;
        if (pressed) {
            if (bit == FN_BIT)
                st->fn_held = true;
            else
                st->held |= bit;
            st->tap = (int8_t)i;
        } else {
            if (bit == FN_BIT)
                st->fn_held = false;
            else
                st->held &= ~bit;
            if (st->tap == i)
                tapped(st, bit);
            st->tap = -1;
        }
        return false;
    }

    if (pressed) {
        if (st->usage[i])
            return false;               /* down twice: nothing new */
        st->tap = -1;                   /* a modifier held for this key is no tap */
        uint8_t on = st->held | st->latched;
        uint8_t mods = on & (CTRL | ALT);
        uint8_t usage = k->usage;
        if ((st->fn_held || st->fn_latched) && k->fn) {
            usage = k->fn;              /* the Fn layer: never shifted */
        } else if (k->kind == LETTER) {
            if (((on & SHIFT) != 0) != st->caps)
                mods |= SHIFT;
        } else if (on & SHIFT) {
            if (k->shifted)
                usage = k->shifted;
            mods |= SHIFT;
        }
        st->usage[i] = usage;
        st->mods[i] = mods;
        st->latched = 0;
        st->fn_latched = false;
        st->newest = (int8_t)i;
    } else {
        if (!st->usage[i])
            return false;
        st->usage[i] = 0;
        if (st->newest == i)
            st->newest = -1;
    }
    report(st, out);
    return true;
}
