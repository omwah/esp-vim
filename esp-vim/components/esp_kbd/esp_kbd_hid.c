/*
 * esp_kbd_hid: see include/esp_kbd_hid.h. Pure C: built on the host too, for
 * esp-vim/test/kbd/.
 *
 * The report map is the HID class's item stream (HID 1.11, section 6.2.2):
 * one prefix byte (tag, type, data size) and up to four data bytes each.
 * Global items (usage page, report size/count/ID, logical minimum) persist;
 * local items (usages) belong to the next main item; each Input main item is a
 * field of report-count x report-size bits at the report's running offset.
 * Only fields of usage page 7 (Keyboard/Keypad) are kept; every other input
 * field still moves the offset on.
 */

#include "esp_kbd_hid.h"

#include <stdio.h>
#include <string.h>

#define PAGE_KEYBOARD 0x07
#define MAX_IDS       16
#define MAX_USAGES    16
#define STACK_DEPTH   4

typedef struct {
    uint32_t page;
    int32_t lmin;
    uint32_t size, count;
    uint8_t id;
} globals_t;

static esp_kbd_layout_t *layout_for(esp_kbd_layouts_t *out, uint8_t id)
{
    for (int i = 0; i < out->n; i++)
        if (out->report[i].id == id)
            return &out->report[i];
    if (out->n == ESP_KBD_MAX_REPORTS)
        return NULL;
    esp_kbd_layout_t *l = &out->report[out->n++];
    memset(l, 0, sizeof *l);
    l->id = id;
    return l;
}

int esp_kbd_parse_map(const uint8_t *map, size_t len, esp_kbd_layouts_t *out)
{
    globals_t g = { 0 }, stack[STACK_DEPTH];
    int depth = 0;
    uint32_t usage[MAX_USAGES], umin = 0, umax = 0;
    int nusage = 0;
    bool have_min = false, have_max = false;
    uint8_t ids[MAX_IDS] = { 0 };
    uint16_t offs[MAX_IDS] = { 0 };
    int nids = 1;                       /* id 0 is always there */

    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < len;) {
        uint8_t p = map[i];
        if (p == 0xfe) {                /* a long item: skip it */
            if (i + 1 >= len)
                break;
            i += 3 + map[i + 1];
            continue;
        }
        size_t n = (p & 3) == 3 ? 4 : (p & 3);
        if (i + 1 + n > len)
            break;
        uint32_t u = 0;
        for (size_t k = 0; k < n; k++)
            u |= (uint32_t)map[i + 1 + k] << (8 * k);
        int32_t s = n == 1 ? (int8_t)u : n == 2 ? (int16_t)u : (int32_t)u;
        uint8_t type = (p >> 2) & 3, tag = p >> 4;
        i += 1 + n;

        if (type == 1) {                /* global */
            switch (tag) {
            case 0:  g.page = u; break;
            case 1:  g.lmin = s; break;
            case 7:  g.size = u; break;
            case 8:  g.id = (uint8_t)u; break;
            case 9:  g.count = u; break;
            case 10: if (depth < STACK_DEPTH) stack[depth++] = g; break;
            case 11: if (depth > 0) g = stack[--depth]; break;
            default: break;
            }
            continue;
        }
        if (type == 2) {                /* local; 4 bytes carry their own page */
            uint32_t full = n == 4 ? u : (g.page << 16) | u;
            if (tag == 0 && nusage < MAX_USAGES)
                usage[nusage++] = full;
            else if (tag == 1)
                umin = full, have_min = true;
            else if (tag == 2)
                umax = full, have_max = true;
            continue;
        }
        if (type != 0)
            continue;

        if (tag == 8) {                 /* Input */
            int slot = 0;
            while (slot < nids && ids[slot] != g.id)
                slot++;
            if (slot == nids && nids < MAX_IDS)
                ids[nids++] = g.id;
            if (slot == MAX_IDS)
                slot = 0;
            uint32_t bits = g.size * g.count;
            uint32_t first = have_min ? umin : nusage ? usage[0] : g.page << 16;
            bool constant = u & 1, variable = u & 2;
            bool contiguous = true;
            for (int k = 1; k < nusage && k < (int)g.count; k++)
                contiguous &= usage[k] == usage[0] + (uint32_t)k;
            if (!constant && first >> 16 == PAGE_KEYBOARD && bits > 0
                    && (!variable || (g.size == 1 && (have_min || contiguous)))) {
                esp_kbd_layout_t *l = layout_for(out, g.id);
                if (l != NULL && l->nfields < ESP_KBD_MAX_FIELDS) {
                    esp_kbd_field_t *f = &l->field[l->nfields++];
                    f->bit = offs[slot];
                    f->count = (uint16_t)g.count;
                    f->size = (uint8_t)g.size;
                    f->array = !variable;
                    f->base = variable ? (int16_t)(first & 0xffff)
                                       : (int16_t)((first & 0xffff) - g.lmin);
                }
            }
            offs[slot] = (uint16_t)(offs[slot] + bits);
            for (int k = 0; k < out->n; k++)
                if (out->report[k].id == g.id)
                    out->report[k].bits = offs[slot];
        }
        if (tag == 8 || tag == 9 || tag == 11 || tag == 10) {
            nusage = 0;                 /* a main item ends the locals */
            have_min = have_max = false;
        }
    }
    (void)have_max;
    (void)umax;
    return out->n;
}

static uint32_t get_bits(const uint8_t *d, size_t len, uint32_t bit, uint8_t size)
{
    uint32_t v = 0;
    for (uint8_t k = 0; k < size; k++, bit++) {
        if (bit / 8 >= len)
            break;
        v |= (uint32_t)((d[bit / 8] >> (bit % 8)) & 1) << k;
    }
    return v;
}

/* One usage held: a modifier, or a key (0-3 are "no key" and errors). */
static bool held(esp_kbd_keys_t *out, int32_t usage)
{
    if (usage == 0x01)
        return false;                   /* ErrorRollOver */
    if (usage >= 0xe0 && usage <= 0xe7)
        out->mods |= (uint8_t)(1 << (usage - 0xe0));
    else if (usage >= 0x04 && usage <= 0xdf && out->n < ESP_KBD_MAX_KEYS)
        out->key[out->n++] = (uint8_t)usage;
    return true;
}

bool esp_kbd_decode(const esp_kbd_layouts_t *l, uint8_t id, const uint8_t *data, size_t len,
                    esp_kbd_keys_t *out)
{
    const esp_kbd_layout_t *r = NULL;
    for (int i = 0; i < l->n; i++)
        if (l->report[i].id == id)
            r = &l->report[i];
    if (r == NULL || r->nfields == 0)
        return false;
    memset(out, 0, sizeof *out);
    for (int f = 0; f < r->nfields; f++) {
        const esp_kbd_field_t *x = &r->field[f];
        for (uint32_t i = 0; i < x->count; i++) {
            if (x->array) {
                uint32_t v = get_bits(data, len, x->bit + i * x->size, x->size);
                if (v != 0 && !held(out, (int32_t)v + x->base))
                    return false;
            } else if (get_bits(data, len, x->bit + i, 1)) {
                held(out, x->base + (int32_t)i);
            }
        }
    }
    return true;
}

bool esp_kbd_decode_boot(const uint8_t *r, size_t len, esp_kbd_keys_t *out)
{
    if (len < 3)
        return false;
    memset(out, 0, sizeof *out);
    out->mods = r[0];
    /* Some keyboards leave the reserved byte out. */
    const uint8_t *keys = len >= 8 ? r + 2 : r + 1;
    size_t n = len >= 8 ? 6 : len - 1;
    if (n > 6)
        n = 6;
    for (size_t i = 0; i < n; i++)
        if (keys[i] != 0 && !held(out, keys[i]))
            return false;
    return true;
}

/* ------------------------------------------------------------ the bytes -- */

/* Printable keys by usage, 0x04..0x38: unshifted and shifted. (0x32 is the
 * non-US '#' key, which a US layout doesn't have.) */
static const char s_plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\x7f\t -=[]\\#;'`,./";
static const char s_shift[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\x7f\t _+{}|~:\"~<>?";

/* Keys that send escape sequences: {xterm's unmodified form, the CSI number,
 * the final byte}. With modifiers xterm sends CSI {n} ; {m} {final} (CSI 1 ;
 * {m} for the ones without a number), m = 1 + Shift 1 + Alt 2 + Ctrl 4. The
 * unmodified cursor keys are in application mode, which Vim switches on (t_ks). */
typedef struct {
    uint8_t usage;
    const char *plain;
    uint8_t num;
    char final;
} special_t;

static const special_t s_special[] = {
    { 0x3a, "\033OP",   1, 'P' },   /* F1..F4 */
    { 0x3b, "\033OQ",   1, 'Q' },
    { 0x3c, "\033OR",   1, 'R' },
    { 0x3d, "\033OS",   1, 'S' },
    { 0x3e, "\033[15~", 15, '~' },  /* F5..F12 */
    { 0x3f, "\033[17~", 17, '~' },
    { 0x40, "\033[18~", 18, '~' },
    { 0x41, "\033[19~", 19, '~' },
    { 0x42, "\033[20~", 20, '~' },
    { 0x43, "\033[21~", 21, '~' },
    { 0x44, "\033[23~", 23, '~' },
    { 0x45, "\033[24~", 24, '~' },
    { 0x49, "\033[2~",  2, '~' },   /* Insert */
    { 0x4a, "\033OH",   1, 'H' },   /* Home */
    { 0x4b, "\033[5~",  5, '~' },   /* Page Up */
    { 0x4c, "\033[3~",  3, '~' },   /* Delete */
    { 0x4d, "\033OF",   1, 'F' },   /* End */
    { 0x4e, "\033[6~",  6, '~' },   /* Page Down */
    { 0x4f, "\033OC",   1, 'C' },   /* Right */
    { 0x50, "\033OD",   1, 'D' },   /* Left */
    { 0x51, "\033OB",   1, 'B' },   /* Down */
    { 0x52, "\033OA",   1, 'A' },   /* Up */
};

/* The keypad, with Num Lock on (keyboards without one send these). */
static char keypad(uint8_t usage)
{
    static const char pad[] = "/*-+\r1234567890.";      /* 0x54..0x63 */
    if (usage >= 0x54 && usage <= 0x63)
        return pad[usage - 0x54];
    if (usage == 0x67)
        return '=';
    return 0;
}

/* Ctrl with a character, as xterm sends it. -1: no control form. */
static int ctrl_of(char c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 1;
    if (c >= '@' && c <= '_')           /* ^@ ^[ ^\ ^] ^^ ^_ and capitals */
        return c - '@';
    switch (c) {
    case ' ': case '2': return 0;
    case '3': return 0x1b;
    case '4': return 0x1c;
    case '5': return 0x1d;
    case '6': return 0x1e;
    case '7': case '/': case '-': return 0x1f;
    case '8': case '?': return 0x7f;
    case 0x7f: return 0x08;             /* Ctrl-Backspace */
    default: return -1;
    }
}

size_t esp_kbd_key_bytes(uint8_t usage, uint8_t mods, bool caps, char out[16])
{
    bool shift = (mods & ESP_KBD_MOD_SHIFT) != 0;
    bool ctrl = (mods & ESP_KBD_MOD_CTRL) != 0;
    bool alt = (mods & ESP_KBD_MOD_ALT) != 0;

    for (size_t i = 0; i < sizeof s_special / sizeof s_special[0]; i++) {
        const special_t *k = &s_special[i];
        if (k->usage != usage)
            continue;
        int m = 1 + shift + 2 * alt + 4 * ctrl;
        if (m == 1) {
            size_t n = strlen(k->plain);
            memcpy(out, k->plain, n);
            return n;
        }
        return (size_t)snprintf(out, 16, "\033[%u;%d%c", k->num, m, k->final);
    }

    if (usage == 0x2b && shift && !ctrl) {      /* Shift-Tab: back tab */
        size_t n = 0;
        if (alt)
            out[n++] = '\033';
        memcpy(out + n, "\033[Z", 3);
        return n + 3;
    }

    char c = keypad(usage);
    if (c == 0) {
        if (usage == 0x64)                      /* the non-US \| key */
            c = shift ? '|' : '\\';
        else if (usage < 0x04 || usage > 0x38 || usage == 0x32)
            return 0;
        else {
            int i = usage - 0x04;
            bool up = shift;
            if (usage <= 0x1d && caps)          /* Caps Lock shifts letters only */
                up = !up;
            c = up ? s_shift[i] : s_plain[i];
        }
    }
    if (ctrl) {
        int cc = ctrl_of(c);
        if (cc >= 0)
            c = (char)cc;
    }
    size_t n = 0;
    if (alt)
        out[n++] = '\033';                      /* Meta sends ESC first, as xterm does */
    out[n++] = c;
    return n;
}

void esp_kbd_describe(const esp_kbd_layouts_t *l, char *out, size_t outlen)
{
    size_t n = 0;
    out[0] = '\0';
    for (int r = 0; r < l->n && n < outlen; r++) {
        const esp_kbd_layout_t *x = &l->report[r];
        n += (size_t)snprintf(out + n, outlen - n, "%s%u:", r ? "; " : "", x->id);
        for (int f = 0; f < x->nfields && n < outlen; f++) {
            const esp_kbd_field_t *y = &x->field[f];
            if (y->array)
                n += (size_t)snprintf(out + n, outlen - n, " keys%u@%u", y->count, y->bit);
            else if (y->base == 0xe0 && y->count == 8)
                n += (size_t)snprintf(out + n, outlen - n, " mods@%u", y->bit);
            else
                n += (size_t)snprintf(out + n, outlen - n, " map%d+%u@%u", y->base, y->count, y->bit);
        }
    }
}
