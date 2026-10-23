/*
 * esp_kbd_tdeck: see include/esp_kbd_tdeck.h.
 *
 * As esp_kbd_cardputer.c: each key is given its usage and modifiers when it
 * goes down, from the layer and modifiers in effect then, and keeps them until
 * it comes up -- a key held while Sym is let go still repeats what it began as.
 */

#include "esp_kbd_tdeck.h"

#include <string.h>

#define SHIFT   0x02                    /* left Shift, as a report sets it */
#define CTRL    0x01

enum { LETTER, CHAR, OTHER, MOD };      /* how Shift acts on a key; MOD: a modifier key */

#define S(u)    (0x100 | (u))           /* a layer's usage, typed with Shift */

typedef struct {
    uint8_t kind;
    uint8_t usage;                      /* the plain key (MOD: its bit) */
    uint16_t sym;                       /* with Sym (0: as plain); a MOD key types it */
    uint16_t sym2;                      /* with Sym and Shift (0: as with Sym) */
} key_t_;

#define L(u, s, s2) { LETTER, u, s, s2 }
#define O(u, s, s2) { OTHER, u, s, s2 }
#define M(bit)      { MOD, bit, 0, 0 }
#define MS(bit, s)  { MOD, bit, s, 0 } /* a modifier that types {s} with Sym */

#define SYM_BIT 0x80                    /* M(): Sym, which is no report modifier */

/* HID usages (page 7): a-z 0x04-0x1d, 1-9 0x1e-0x26, 0 0x27, Enter 0x28,
 * Esc 0x29, Bksp 0x2a, Tab 0x2b, Space 0x2c, - = [ ] \ 0x2d-0x31, ; ' ` , . /
 * 0x33-0x38, Home 0x4a, PgUp 0x4b, Delete 0x4c, End 0x4d, PgDn 0x4e,
 * Right/Left/Down/Up 0x4f-0x52. In the C3's matrix order: column, then row
 * (its firmware's keyboard[col][row]). */
static const key_t_ s_keys[ESP_KBD_TDECK_KEYS] = {
    /* column 0: q w Sym a Alt Space Mic; Alt is Esc, Mic is Ctrl */
    L(0x14, S(0x20), S(0x35)),          /* q  # ~ */
    L(0x1a, 0x1e, 0x35),                /* w  1 ` */
    M(SYM_BIT),
    L(0x04, S(0x25), S(0x31)),          /* a  * | */
    O(0x29, 0, 0),                      /* Alt is Esc, on every layer */
    O(0x2c, 0x2b, 0),                   /* Space; Sym: Tab */
    MS(CTRL, 0x27),                     /* Mic is Ctrl; Sym: 0 */
    /* column 1: e s d p x z LShift */
    L(0x08, 0x1f, S(0x23)),             /* e  2 ^ */
    L(0x16, 0x21, 0x31),                /* s  4 \ */
    L(0x07, 0x22, S(0x36)),             /* d  5 < */
    L(0x13, S(0x1f), S(0x24)),          /* p  @ & */
    L(0x1b, 0x25, 0),                   /* x  8 */
    L(0x1d, 0x24, 0),                   /* z  7 */
    M(SHIFT),
    /* column 2: r g t RShift v c f */
    L(0x15, 0x20, S(0x22)),             /* r  3 % */
    L(0x0a, 0x38, 0x4a),                /* g  / Home */
    L(0x17, S(0x26), 0x2f),             /* t  ( [ */
    M(SHIFT),
    L(0x19, S(0x38), 0),                /* v  ? */
    L(0x06, 0x26, 0),                   /* c  9 */
    L(0x09, 0x23, S(0x37)),             /* f  6 > */
    /* column 3: u h y Enter b n j */
    L(0x18, S(0x2d), S(0x2f)),          /* u  _ { */
    L(0x0b, S(0x33), 0x50),             /* h  : Left */
    L(0x1c, S(0x27), 0x30),             /* y  ) ] */
    O(0x28, 0, 0),                      /* Enter */
    L(0x05, S(0x1e), 0x4b),             /* b  ! PgUp */
    L(0x11, 0x36, 0x4e),                /* n  , PgDn */
    L(0x0d, 0x33, 0x51),                /* j  ; Down */
    /* column 4: o l i Bksp $ m k */
    L(0x12, S(0x2e), 0x2e),             /* o  + = */
    L(0x0f, S(0x34), 0x4f),             /* l  " Right */
    L(0x0c, 0x2d, S(0x30)),             /* i  - } */
    O(0x2a, 0x4c, 0),                   /* Bksp; Sym: Delete */
    { CHAR, 0x21, 0, 0 },               /* $, Shift-4, whatever else is held */
    L(0x10, 0x37, 0x4d),                /* m  . End */
    L(0x0e, 0x34, 0x52),                /* k  ' Up */
};

void esp_kbd_tdeck_reset(esp_kbd_tdeck_t *st)
{
    memset(st, 0, sizeof *st);
    st->tap = -1;
    st->newest = -1;
}

static void report(const esp_kbd_tdeck_t *st, esp_kbd_keys_t *out)
{
    memset(out, 0, sizeof *out);
    out->has_mods = true;
    for (int i = 0; i < ESP_KBD_TDECK_KEYS && out->n < ESP_KBD_MAX_KEYS; i++)
        if (st->usage[i])
            out->key[out->n++] = st->usage[i];
    if (st->newest >= 0)
        out->mods = st->mods[st->newest];
}

/* A modifier key tapped alone: latch it for the next key, or let it go. Shift
 * goes on to Caps Lock. */
static void tapped(esp_kbd_tdeck_t *st, uint8_t bit)
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

static void modifier(esp_kbd_tdeck_t *st, int i, uint8_t bit, bool pressed)
{
    if (pressed) {
        if (bit == SYM_BIT)
            st->sym_held = true;
        else if (bit == SHIFT && st->shifts++)
            ;                           /* the other Shift is down already */
        else
            st->held |= bit;
        st->tap = (int8_t)i;
    } else {
        if (bit == SYM_BIT)
            st->sym_held = false;
        else if (bit == SHIFT && st->shifts > 0 && --st->shifts)
            ;                           /* the other Shift is still down */
        else
            st->held &= ~bit;
        if (st->tap == i)
            tapped(st, bit);
        st->tap = -1;
    }
}

bool esp_kbd_tdeck_event(esp_kbd_tdeck_t *st, int col, int row, bool pressed,
                         esp_kbd_keys_t *out)
{
    if (col < 0 || col >= ESP_KBD_TDECK_COLS || row < 0 || row >= ESP_KBD_TDECK_ROWS)
        return false;
    int i = col * ESP_KBD_TDECK_ROWS + row;
    const key_t_ *k = &s_keys[i];

    /* A modifier that types with Sym (Mic: 0) is a key while it does. */
    bool sym = st->sym_held || st->sym_latched;
    bool as_key = k->kind == MOD && (pressed ? sym && k->sym : st->usage[i] != 0);
    if (k->kind == MOD && !as_key) {
        modifier(st, i, k->usage, pressed);
        return false;
    }

    if (pressed) {
        if (st->usage[i])
            return false;               /* down twice: nothing new */
        st->tap = -1;                   /* a modifier held for this key is no tap */
        uint8_t on = st->held | st->latched;
        bool shift = (on & SHIFT) != 0;
        uint8_t mods = on & CTRL;
        uint16_t layer = 0;
        if (sym)
            layer = shift && k->sym2 ? k->sym2 : k->sym;
        uint8_t usage = k->usage;
        if (layer) {
            usage = layer & 0xFF;       /* Shift chose the layer: it isn't passed on */
            if (layer & 0x100)
                mods |= SHIFT;
        } else if (k->kind == LETTER) {
            if (shift != st->caps)
                mods |= SHIFT;
        } else if (k->kind == CHAR) {
            mods |= SHIFT;              /* $ is Shift-4, Shift or not */
        } else if (shift) {
            mods |= SHIFT;
        }
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

int esp_kbd_tdeck_ball(esp_kbd_tdeck_ball_t *b, int dir, int toggles, int step)
{
    if (dir < 0 || dir > 3 || toggles <= 0)
        return 0;
    if (step < 1)
        step = 1;
    b->count[dir ^ 1] = 0;              /* UP/DOWN and LEFT/RIGHT are pairs */
    int n = b->count[dir] + toggles;
    b->count[dir] = (int16_t)(n % step);
    return n / step;
}

uint8_t esp_kbd_tdeck_ball_usage(int dir)
{
    static const uint8_t u[4] = { 0x52, 0x51, 0x50, 0x4f };
    return dir >= 0 && dir < 4 ? u[dir] : 0;
}
