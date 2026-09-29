/*
 * Host test of components/esp_kbd/esp_kbd_hid.c: report maps parsed, reports
 * decoded, and the bytes a key press sends. `pixi run kbd-test`.
 *
 * The maps are the shapes keyboards send: the HID spec's boot keyboard
 * (Appendix B.1), a composite keyboard with report IDs whose second report is
 * an N-key rollover bitmap (a modifier byte and one bit per usage 0x00-0x97:
 * 20 bytes), with consumer keys and a mouse beside it, one that lists its
 * modifier usages one by one, and one with 4-byte (page-qualified) usages.
 */

#include "esp_kbd_hid.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("  FAIL  line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static const uint8_t boot_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                             /* reserved */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,   /* LEDs (output) */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xc0,
};

static const uint8_t composite_map[] = {
    /* ID 1: boot layout */
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2a, 0xff, 0x00,
    0x81, 0x00,
    0xc0,
    /* ID 2: N-key rollover, modifiers + a bitmap of usages 0x00-0x97 */
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x02,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x19, 0x00, 0x29, 0x97, 0x95, 0x98, 0x75, 0x01, 0x81, 0x02,
    0xc0,
    /* ID 3: consumer keys (volume...), not ours */
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x03,
    0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xc0,
    /* ID 4: a mouse (a touchpad on the keyboard) */
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x04, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xc0, 0xc0,
};

static const uint8_t listed_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x05, 0x07, 0x09, 0xe0, 0x09, 0xe1, 0x09, 0xe2, 0x09, 0xe3, 0x09, 0xe4, 0x09, 0xe5, 0x09, 0xe6,
    0x09, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x91, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x00,
    0xc0,
};

static const uint8_t extended_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,       /* page stays Generic Desktop */
    0x1b, 0xe0, 0x00, 0x07, 0x00, 0x2b, 0xe7, 0x00, 0x07, 0x00,   /* usages 7:E0..7:E7 */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x1b, 0x04, 0x00, 0x07, 0x00, 0x2b, 0x13, 0x00, 0x07, 0x00,   /* 7:04..7:13, a bitmap */
    0x95, 0x10, 0x81, 0x02,
    0xc0,
};

/* The Air75 BT5.0 (seen on the board, 2026-09-29): report 1 is modifiers, a
 * reserved byte and five keys (8 bytes); report 2 is a 160-usage bitmap with
 * NO modifier bits, holding only the keys beyond report 1's five. */
static const uint8_t air75_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0x75, 0x08, 0x95, 0x05, 0x15, 0x00, 0x25, 0xff, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0xc0,
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x02,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x9f, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0xa0, 0x81, 0x02,
    0xc0,
};

/* What it sent for Shift held with a s d f g h j, then all let go
 * (esp_bt_keyboard().reports on the board): {report id, 8 or 20 bytes}. */
typedef struct { uint8_t id; uint8_t len; uint8_t d[20]; } seen_t;
static const seen_t air75_seen[] = {
    { 1, 8, { 0x02, 0, 0x04, 0x16, 0x07, 0, 0, 0 } },
    { 1, 8, { 0x02, 0, 0x04, 0x16, 0x07, 0x09, 0, 0 } },
    { 1, 8, { 0x02, 0, 0x04, 0x16, 0x07, 0x09, 0x0a, 0 } },
    { 2, 20, { 0, 0x08 } },                             /* h: the sixth */
    { 2, 20, { 0, 0x28 } },                             /* h j */
    { 1, 8, { 0x02, 0, 0x04, 0x09, 0, 0, 0, 0 } },      /* s d g up */
    { 1, 8, { 0x02, 0, 0, 0, 0, 0, 0, 0 } },            /* a f up */
    { 2, 20, { 0, 0x20 } },                             /* h up */
    { 1, 8, { 0 } },                                    /* Shift up */
    { 2, 20, { 0 } },                                   /* j up */
    { 1, 8, { 0, 0, 0x01, 0, 0, 0, 0, 0 } },            /* and then these */
    { 1, 8, { 0 } },
};

/* Replay reports as esp_kbd.c does, and collect what is typed: each key
 * that becomes held, with Shift, as its character. */
static void replay(const esp_kbd_layouts_t *l, const seen_t *r, int n, char *typed, size_t len)
{
    esp_kbd_state_t st;
    esp_kbd_keys_t k, now, prev = { 0 };
    memset(&st, 0, sizeof st);
    size_t t = 0;
    for (int i = 0; i < n; i++) {
        if (!esp_kbd_decode(l, r[i].id, r[i].d, r[i].len, &k))
            continue;
        esp_kbd_merge(&st, r[i].id, &k, &now);
        for (int j = 0; j < now.n; j++)
            if (memchr(prev.key, now.key[j], prev.n) == NULL && t + 2 < len) {
                char b[16];
                size_t m = esp_kbd_key_bytes(now.key[j], now.mods, false, b);
                if (m == 1)
                    typed[t++] = b[0];
            }
        prev = now;
    }
    typed[t] = '\0';
}

static void keys_are(const esp_kbd_keys_t *k, uint8_t mods, const char *want, int line)
{
    char got[64] = "";
    for (int i = 0; i < k->n; i++)
        snprintf(got + strlen(got), sizeof got - strlen(got), "%s%02x", i ? " " : "", k->key[i]);
    checks++;
    if (k->mods != mods || strcmp(got, want) != 0) {
        fails++;
        printf("  FAIL  line %d: mods %02x keys [%s], wanted mods %02x keys [%s]\n",
               line, k->mods, got, mods, want);
    }
}
#define KEYS(k, mods, want) keys_are(k, mods, want, __LINE__)

static void bytes_are(uint8_t usage, uint8_t mods, int caps, const char *want, size_t wantn, int line)
{
    char out[16];
    size_t n = esp_kbd_key_bytes(usage, mods, caps, out);
    checks++;
    if (n != wantn || memcmp(out, want, n) != 0) {
        fails++;
        printf("  FAIL  line %d: usage %02x mods %02x: got", line, usage, mods);
        for (size_t i = 0; i < n; i++)
            printf(" %02x", (uint8_t)out[i]);
        printf(" (%zu bytes)\n", n);
    }
}
#define BYTES(u, m, caps, want) bytes_are(u, m, caps, want, sizeof(want) - 1, __LINE__)

int main(void)
{
    esp_kbd_layouts_t l;
    esp_kbd_keys_t k;
    char desc[128];

    /* -- the boot keyboard ------------------------------------------ */
    CHECK(esp_kbd_parse_map(boot_map, sizeof boot_map, &l) == 1, "boot map: one report");
    esp_kbd_describe(&l, desc, sizeof desc);
    CHECK(strcmp(desc, "0: mods@0 keys6@16") == 0, "boot map layout: %s", desc);
    CHECK(l.report[0].bits == 64, "boot report is 64 bits, got %u", l.report[0].bits);
    const uint8_t r1[] = { 0x02, 0x00, 0x04, 0x05, 0, 0, 0, 0 };
    CHECK(esp_kbd_decode(&l, 0, r1, sizeof r1, &k), "boot report decodes");
    KEYS(&k, 0x02, "04 05");
    const uint8_t over[] = { 0x02, 0, 1, 1, 1, 1, 1, 1 };
    CHECK(esp_kbd_decode(&l, 0, over, sizeof over, &k) && k.rollover && k.n == 0
          && k.has_mods && k.mods == 0x02, "ErrorRollOver: keys unknown, modifiers kept");
    CHECK(esp_kbd_decode_boot(r1, sizeof r1, &k), "the boot decoder");
    KEYS(&k, 0x02, "04 05");
    const uint8_t short_r[] = { 0x01, 0x06, 0x07 };         /* no reserved byte */
    CHECK(esp_kbd_decode_boot(short_r, sizeof short_r, &k), "a short boot report");
    KEYS(&k, 0x01, "06 07");

    /* -- a composite keyboard: boot, N-key rollover, consumer, mouse -- */
    CHECK(esp_kbd_parse_map(composite_map, sizeof composite_map, &l) == 2,
          "composite: two keyboard reports (consumer and mouse left out), got %d", l.n);
    esp_kbd_describe(&l, desc, sizeof desc);
    CHECK(strcmp(desc, "1: mods@0 keys6@16; 2: mods@0 map0+152@8") == 0, "composite layout: %s", desc);
    CHECK(l.report[1].bits == 160, "NKRO report is 20 bytes, got %u bits", l.report[1].bits);
    uint8_t nkro[20] = { 0x01 };                        /* Left Ctrl */
    nkro[1 + 0x04 / 8] |= 1 << (0x04 % 8);              /* a */
    nkro[1 + 0x52 / 8] |= 1 << (0x52 % 8);              /* Up */
    nkro[1 + 0x4f / 8] |= 1 << (0x4f % 8);              /* Right */
    CHECK(esp_kbd_decode(&l, 2, nkro, sizeof nkro, &k), "NKRO report decodes");
    KEYS(&k, 0x01, "04 4f 52");
    uint8_t many[20] = { 0 };
    for (int u = 0x04; u < 0x04 + 20; u++)
        many[1 + u / 8] |= 1 << (u % 8);
    CHECK(esp_kbd_decode(&l, 2, many, sizeof many, &k) && k.n == ESP_KBD_MAX_KEYS,
          "20 keys held: the first %d kept, got %d", ESP_KBD_MAX_KEYS, k.n);
    const uint8_t boot1[] = { 0x20, 0x00, 0x1d, 0, 0, 0, 0, 0 };    /* Right Shift + z */
    CHECK(esp_kbd_decode(&l, 1, boot1, sizeof boot1, &k), "report 1 decodes");
    KEYS(&k, 0x20, "1d");
    const uint8_t vol[] = { 0xe9, 0x00 };
    CHECK(!esp_kbd_decode(&l, 3, vol, sizeof vol, &k), "the consumer report is not a keyboard's");
    const uint8_t mouse[] = { 0x01, 0x05, 0xfb };
    CHECK(!esp_kbd_decode(&l, 4, mouse, sizeof mouse, &k), "the mouse report is not a keyboard's");
    const uint8_t cut[] = { 0x02, 0x10 };               /* a report cut short: no crash */
    CHECK(esp_kbd_decode(&l, 2, cut, sizeof cut, &k), "a short NKRO report decodes what it has");
    KEYS(&k, 0x02, "04");

    /* -- the Air75: keys spread over two reports -------------------- */
    CHECK(esp_kbd_parse_map(air75_map, sizeof air75_map, &l) == 2, "Air75: two keyboard reports");
    esp_kbd_describe(&l, desc, sizeof desc);
    CHECK(strcmp(desc, "1: mods@0 keys5@16; 2: map0+160@0") == 0, "Air75 layout (as the board saw it): %s", desc);
    char typed[32];
    replay(&l, air75_seen, sizeof air75_seen / sizeof air75_seen[0], typed, sizeof typed);
    CHECK(strcmp(typed, "ASDFGHJ") == 0, "Air75: Shift + seven keys types each once, in capitals: %s", typed);
    /* The other way keyboards do it: report 1 says "too many", the bitmap has
     * every key -- and no modifiers. */
    const seen_t all_in_map[] = {
        { 1, 8, { 0x02, 0, 0x04, 0x05, 0x06, 0x07, 0x08, 0 } },
        { 1, 8, { 0x02, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0 } },
        { 2, 20, { 0xf0, 0x03 } },                      /* a..f: usages 4..9 */
        { 2, 20, { 0 } },
        { 1, 8, { 0 } },
    };
    replay(&l, all_in_map, 5, typed, sizeof typed);
    CHECK(strcmp(typed, "ABCDEF") == 0, "a bitmap with every key, and rollover in report 1: %s", typed);
    esp_kbd_state_t st;
    memset(&st, 0, sizeof st);
    esp_kbd_keys_t full;
    const uint8_t over1[] = { 0x02, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0 };
    esp_kbd_decode(&l, 1, over1, sizeof over1, &k);
    esp_kbd_merge(&st, 1, &k, &full);
    KEYS(&full, 0x02, "");                              /* rollover: modifiers only */

    /* -- usages listed one by one ------------------------------------- */
    CHECK(esp_kbd_parse_map(listed_map, sizeof listed_map, &l) == 1, "listed usages: one report");
    esp_kbd_describe(&l, desc, sizeof desc);
    CHECK(strcmp(desc, "0: mods@0 keys4@16") == 0, "listed layout: %s", desc);
    const uint8_t r3[] = { 0x44, 0x00, 0x2c, 0x28, 0, 0 };      /* Alt-Gr + Left Alt, Space, Enter */
    CHECK(esp_kbd_decode(&l, 0, r3, sizeof r3, &k), "listed report decodes");
    KEYS(&k, 0x44, "2c 28");

    /* -- 4-byte usages (page in the usage) ---------------------------- */
    CHECK(esp_kbd_parse_map(extended_map, sizeof extended_map, &l) == 1, "extended usages: one report");
    esp_kbd_describe(&l, desc, sizeof desc);
    CHECK(strcmp(desc, "0: mods@0 map4+16@8") == 0, "extended layout: %s", desc);
    const uint8_t r4[] = { 0x10, 0x01, 0x80 };          /* Right Ctrl, a (bit 0), t (bit 15) */
    CHECK(esp_kbd_decode(&l, 0, r4, sizeof r4, &k), "extended report decodes");
    KEYS(&k, 0x10, "04 13");

    /* -- garbage does not crash ---------------------------------------- */
    uint8_t junk[64];
    for (size_t i = 0; i < sizeof junk; i++)
        junk[i] = (uint8_t)(i * 37 + 11);
    esp_kbd_parse_map(junk, sizeof junk, &l);
    esp_kbd_parse_map(boot_map, 7, &l);                 /* cut in the middle of an item */
    CHECK(1, "garbage maps parse without a crash");

    /* -- bytes ------------------------------------------------------------ */
    BYTES(0x04, 0, 0, "a");
    BYTES(0x04, 0x02, 0, "A");
    BYTES(0x04, 0, 1, "A");                             /* Caps Lock */
    BYTES(0x04, 0x20, 1, "a");                          /* Caps Lock + Shift */
    BYTES(0x1e, 0, 1, "1");                             /* Caps Lock leaves digits */
    BYTES(0x04, 0x01, 0, "\x01");                       /* Ctrl-A */
    BYTES(0x04, 0x04, 0, "\033a");                      /* Alt-A */
    BYTES(0x2f, 0x10, 0, "\033");                       /* Ctrl-[ */
    BYTES(0x1f, 0x01, 0, "\0");                         /* Ctrl-2: NUL */
    BYTES(0x23, 0x01, 0, "\x1e");                       /* Ctrl-6 */
    BYTES(0x2a, 0, 0, "\x7f");                          /* Backspace */
    BYTES(0x2a, 0x01, 0, "\x08");                       /* Ctrl-Backspace */
    BYTES(0x28, 0, 0, "\r");
    BYTES(0x52, 0, 0, "\033OA");                        /* Up */
    BYTES(0x4f, 0x01, 0, "\033[1;5C");                  /* Ctrl-Right */
    BYTES(0x50, 0x02, 0, "\033[1;2D");                  /* Shift-Left */
    BYTES(0x4a, 0x03, 0, "\033[1;6H");                  /* Ctrl-Shift-Home */
    BYTES(0x4d, 0x04, 0, "\033[1;3F");                  /* Alt-End */
    BYTES(0x3a, 0x01, 0, "\033[1;5P");                  /* Ctrl-F1 */
    BYTES(0x3e, 0x02, 0, "\033[15;2~");                 /* Shift-F5 */
    BYTES(0x45, 0x05, 0, "\033[24;7~");                 /* Ctrl-Alt-F12 */
    BYTES(0x4c, 0x04, 0, "\033[3;3~");                  /* Alt-Delete */
    BYTES(0x4b, 0x01, 0, "\033[5;5~");                  /* Ctrl-PageUp */
    BYTES(0x2b, 0, 0, "\t");
    BYTES(0x2b, 0x02, 0, "\033[Z");                     /* Shift-Tab */
    BYTES(0x59, 0, 0, "1");                             /* keypad 1 */
    BYTES(0x58, 0, 0, "\r");                            /* keypad Enter */
    BYTES(0x55, 0x02, 0, "*");
    BYTES(0x64, 0x02, 0, "|");                          /* the non-US \| key */
    BYTES(0x32, 0, 0, "");                              /* non-US #: nothing */
    BYTES(0x39, 0, 0, "");                              /* Caps Lock itself */
    BYTES(0xe0, 0, 0, "");

    printf("kbd-test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
