/*
 * esp_kbd_tab5: see include/esp_kbd_tab5.h.
 *
 * Each key is given its usage and modifiers when it goes down, from the layer
 * and modifiers in effect then, and keeps them until it comes up: a key held
 * while Sym is let go still repeats what it began as.
 */

#include "esp_kbd_tab5.h"

#include <string.h>

#define SHIFT   0x02                    /* left Shift, as a report sets it */
#define CTRL    0x01
#define ALT     0x04

enum { LETTER, CHAR, OTHER, MOD };      /* how Aa acts on a key; MOD: a modifier key */

typedef struct {
    uint8_t kind;
    uint8_t usage, shift;               /* the plain legend: a usage, and whether it's shifted */
    uint8_t sym, sym_shift;             /* with Sym (0: as plain) */
} key_t_;

#define L(u)        { LETTER, u, 0, 0, 0 }
#define C(u, s)     { CHAR, u, s, 0, 0 }
#define CS(u, s, v, t) { CHAR, u, s, v, t }
#define O(u)        { OTHER, u, 0, 0, 0 }
#define OS(u, v)    { OTHER, u, 0, v, 0 }
#define M(bit)      { MOD, bit, 0, 0, 0 }

#define SYM_BIT 0x80                    /* M(): Sym, which is no report modifier */

/* HID usages (page 7): a-z 0x04-0x1d, 1-9 0x1e-0x26, 0 0x27, F1-F12 0x3a-0x45. */
static const key_t_ s_keys[ESP_KBD_TAB5_KEYS] = {
    /* Esc 1 2 3 4 5 6 7 8 9 0 - + Del; Sym: F1-F12, Insert */
    O(0x29), CS(0x1e, 0, 0x3a, 0), CS(0x1f, 0, 0x3b, 0), CS(0x20, 0, 0x3c, 0), CS(0x21, 0, 0x3d, 0),
    CS(0x22, 0, 0x3e, 0), CS(0x23, 0, 0x3f, 0), CS(0x24, 0, 0x40, 0), CS(0x25, 0, 0x41, 0),
    CS(0x26, 0, 0x42, 0), CS(0x27, 0, 0x43, 0), CS(0x2d, 0, 0x44, 0), CS(0x2e, 1, 0x45, 0), OS(0x4c, 0x49),
    /* ` ! @ # $ % ^ & * ( ) [ ] \; Sym: ~ ? . . . . . . / < > { } | */
    CS(0x35, 0, 0x35, 1), CS(0x1e, 1, 0x38, 1), C(0x1f, 1), C(0x20, 1), C(0x21, 1), C(0x22, 1),
    C(0x23, 1), C(0x24, 1), CS(0x25, 1, 0x38, 0), CS(0x26, 1, 0x36, 1), CS(0x27, 1, 0x37, 1),
    CS(0x2f, 0, 0x2f, 1), CS(0x30, 0, 0x30, 1), CS(0x31, 0, 0x31, 1),
    /* Tab q w e r t y u i o p ; ' BS; Sym: : " */
    O(0x2b), L(0x14), L(0x1a), L(0x08), L(0x15), L(0x17), L(0x1c), L(0x18), L(0x0c), L(0x12),
    L(0x13), CS(0x33, 0, 0x33, 1), CS(0x34, 0, 0x34, 1), O(0x2a),
    /* Sym Aa a s d f g h j k l Up _ Enter; Sym: PgUp = */
    M(SYM_BIT), M(SHIFT), L(0x04), L(0x16), L(0x07), L(0x09), L(0x0a), L(0x0b), L(0x0d), L(0x0e),
    L(0x0f), OS(0x52, 0x4b), CS(0x2d, 1, 0x2e, 0), O(0x28),
    /* Ctrl Alt z x c v b n m . Left Down Right Space; Sym: , Home PgDn End */
    M(CTRL), M(ALT), L(0x1d), L(0x1b), L(0x06), L(0x19), L(0x05), L(0x11), L(0x10),
    CS(0x37, 0, 0x36, 0), OS(0x50, 0x4a), OS(0x51, 0x4e), OS(0x4f, 0x4d), O(0x2c),
};

void esp_kbd_tab5_reset(esp_kbd_tab5_t *st)
{
    memset(st, 0, sizeof *st);
    st->tap = -1;
    st->newest = -1;
}

static void report(const esp_kbd_tab5_t *st, esp_kbd_keys_t *out)
{
    memset(out, 0, sizeof *out);
    out->has_mods = true;
    for (int i = 0; i < ESP_KBD_TAB5_KEYS && out->n < ESP_KBD_MAX_KEYS; i++)
        if (st->usage[i])
            out->key[out->n++] = st->usage[i];
    if (st->newest >= 0)
        out->mods = st->mods[st->newest];
}

/* A modifier key tapped alone: latch it for the next key, or let it go. Aa
 * goes on to Caps Lock. */
static void tapped(esp_kbd_tab5_t *st, uint8_t bit)
{
    if (bit == SYM_BIT) {
        st->sym_latched = !st->sym_latched;
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

bool esp_kbd_tab5_event(esp_kbd_tab5_t *st, int row, int col, bool pressed, esp_kbd_keys_t *out)
{
    if (row < 0 || row >= ESP_KBD_TAB5_ROWS || col < 0 || col >= ESP_KBD_TAB5_COLS)
        return false;
    int i = row * ESP_KBD_TAB5_COLS + col;
    const key_t_ *k = &s_keys[i];

    if (k->kind == MOD) {
        uint8_t bit = k->usage;
        if (pressed) {
            if (bit == SYM_BIT)
                st->sym_held = true;
            else
                st->held |= bit;
            st->tap = (int8_t)i;
        } else {
            if (bit == SYM_BIT)
                st->sym_held = false;
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
        bool sym = st->sym_held || st->sym_latched;
        uint8_t usage = k->usage, shift = k->shift;
        if (sym && k->sym)
            usage = k->sym, shift = k->sym_shift;
        uint8_t on = st->held | st->latched;
        uint8_t mods = on & (CTRL | ALT);
        int kind = k->kind;
        if (usage >= 0x39)
            kind = OTHER;               /* F-keys and the arrows' Sym keys aren't characters */
        if (kind == LETTER)
            shift = ((on & SHIFT) != 0) != st->caps;
        else if (kind == OTHER)
            shift = (on & SHIFT) != 0;
        if (shift)
            mods |= SHIFT;
        st->usage[i] = usage;
        st->mods[i] = mods;
        st->latched = 0;
        st->sym_latched = false;
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

unsigned esp_kbd_tab5_indicators(const esp_kbd_tab5_t *st)
{
    uint8_t on = st->held | st->latched;
    return (st->sym_held || st->sym_latched ? ESP_KBD_TAB5_IND_SYM : 0)
         | (on & SHIFT ? ESP_KBD_TAB5_IND_SHIFT : 0)
         | (st->caps ? ESP_KBD_TAB5_IND_CAPS : 0)
         | (on & CTRL ? ESP_KBD_TAB5_IND_CTRL : 0)
         | (on & ALT ? ESP_KBD_TAB5_IND_ALT : 0);
}
