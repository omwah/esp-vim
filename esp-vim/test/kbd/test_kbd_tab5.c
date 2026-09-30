/*
 * Host test of components/esp_kbd/esp_kbd_tab5.c: the Tab5 keyboard's row
 * and column events, turned into reports and then, as esp_kbd.c does, into the
 * bytes a new key sends (esp_kbd_key_bytes). `pixi run kbd-test`.
 */

#include "esp_kbd_tab5.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("  FAIL  line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static esp_kbd_tab5_t st;
static esp_kbd_keys_t prev;

/* What esp_kbd would send for this event: the bytes of each usage that
 * appeared, with the report's modifiers. */
static const char *event(int row, int col, bool down)
{
    static char out[64];
    size_t n = 0;
    esp_kbd_keys_t r;
    if (esp_kbd_tab5_event(&st, row, col, down, &r)) {
        for (int i = 0; i < r.n; i++) {
            if (memchr(prev.key, r.key[i], prev.n))
                continue;
            n += esp_kbd_key_bytes(r.key[i], r.mods, false, out + n);
        }
        prev = r;
    }
    out[n] = '\0';
    return out;
}

/* A key pressed and let go: what it sent. */
static const char *tap(int row, int col)
{
    static char got[64];
    snprintf(got, sizeof got, "%s", event(row, col, true));
    event(row, col, false);
    return got;
}

/* A key pressed with a modifier held. */
static const char *chord(int mrow, int mcol, int row, int col)
{
    static char got[64];
    event(mrow, mcol, true);
    snprintf(got, sizeof got, "%s", event(row, col, true));
    event(row, col, false);
    event(mrow, mcol, false);
    return got;
}

#define SYM   3, 0
#define AA    3, 1
#define CTRL  4, 0
#define ALT   4, 1

static void is(const char *got, const char *want, const char *what)
{
    CHECK(strcmp(got, want) == 0, "%s: got \"%s\", want \"%s\"", what, got, want);
}

int main(void)
{
    esp_kbd_tab5_reset(&st);

    /* Plain keys, one of each row. */
    is(tap(0, 0), "\033", "Esc");
    is(tap(0, 1), "1", "1");
    is(tap(0, 12), "+", "+");
    is(tap(0, 13), "\033[3~", "Del");
    is(tap(1, 1), "!", "!");
    is(tap(1, 6), "^", "^");
    is(tap(1, 13), "\\", "backslash");
    is(tap(2, 0), "\t", "Tab");
    is(tap(2, 1), "q", "q");
    is(tap(2, 13), "\177", "BS");
    is(tap(3, 2), "a", "a");
    is(tap(3, 11), "\033OA", "Up");
    is(tap(3, 12), "_", "_");
    is(tap(3, 13), "\r", "Enter");
    is(tap(4, 9), ".", ".");
    is(tap(4, 10), "\033OD", "Left");
    is(tap(4, 13), " ", "Space");

    /* Sym: second legends and the missing keys. */
    is(chord(SYM, 1, 0), "~", "Sym `");
    is(chord(SYM, 1, 1), "?", "Sym !");
    is(chord(SYM, 1, 8), "/", "Sym *");
    is(chord(SYM, 1, 9), "<", "Sym (");
    is(chord(SYM, 1, 12), "}", "Sym ]");
    is(chord(SYM, 1, 13), "|", "Sym \\");
    is(chord(SYM, 2, 11), ":", "Sym ;");
    is(chord(SYM, 2, 12), "\"", "Sym '");
    is(chord(SYM, 3, 12), "=", "Sym _");
    is(chord(SYM, 4, 9), ",", "Sym .");
    is(chord(SYM, 0, 1), "\033OP", "Sym 1: F1");
    is(chord(SYM, 0, 5), "\033[15~", "Sym 5: F5");
    is(chord(SYM, 0, 12), "\033[24~", "Sym +: F12");
    is(chord(SYM, 0, 13), "\033[2~", "Sym Del: Insert");
    is(chord(SYM, 3, 11), "\033[5~", "Sym Up: PgUp");
    is(chord(SYM, 4, 11), "\033[6~", "Sym Down: PgDn");
    is(chord(SYM, 4, 10), "\033OH", "Sym Left: Home");
    is(chord(SYM, 4, 12), "\033OF", "Sym Right: End");
    is(chord(SYM, 2, 1), "q", "Sym q: q");

    /* Aa: letters and non-characters shift; symbols send their legend. */
    is(chord(AA, 2, 1), "Q", "Aa q");
    is(chord(AA, 0, 1), "1", "Aa 1");
    is(chord(AA, 1, 1), "!", "Aa !");
    is(chord(AA, 2, 0), "\033[Z", "Aa Tab: back tab");
    is(chord(AA, 4, 12), "\033[1;2C", "Aa Right");

    /* Ctrl and Alt. */
    is(chord(CTRL, 1, 11), "\033", "Ctrl [: Esc");
    is(chord(CTRL, 1, 12), "\035", "Ctrl ]");
    is(chord(CTRL, 1, 6), "\036", "Ctrl ^");
    is(chord(CTRL, 2, 2), "\027", "Ctrl w");
    is(chord(CTRL, 3, 12), "\037", "Ctrl _");
    is(chord(ALT, 2, 3), "\033e", "Alt e");
    is(chord(CTRL, 4, 12), "\033[1;5C", "Ctrl Right");

    /* Sticky: a tapped modifier holds for the next key only. */
    tap(CTRL);
    CHECK(esp_kbd_tab5_indicators(&st) == ESP_KBD_TAB5_IND_CTRL, "Ctrl latched shows");
    is(tap(2, 4), "\022", "tapped Ctrl, r");
    is(tap(2, 4), "r", "then r alone");
    tap(CTRL);
    tap(CTRL);
    is(tap(2, 4), "r", "Ctrl tapped twice: off again");
    tap(SYM);
    is(tap(0, 2), "\033OQ", "tapped Sym, 2: F2");
    is(tap(0, 2), "2", "then 2");

    /* Aa: once for one capital, twice for Caps Lock, three times off. */
    tap(AA);
    is(tap(3, 2), "A", "tapped Aa, a");
    is(tap(3, 2), "a", "then a");
    tap(AA);
    tap(AA);
    CHECK(esp_kbd_tab5_indicators(&st) & ESP_KBD_TAB5_IND_CAPS, "Caps Lock shows");
    is(tap(3, 2), "A", "Caps Lock a");
    is(tap(3, 2), "A", "still");
    is(tap(0, 3), "3", "Caps Lock leaves digits");
    is(chord(AA, 3, 2), "a", "Caps Lock with Aa held: a");
    tap(AA);
    is(tap(3, 2), "a", "Caps Lock off");

    /* A modifier held across keys is a chord, not a tap: no latch after. */
    event(CTRL, true);
    is(tap(2, 3), "\005", "Ctrl held, e");
    is(tap(2, 3), "\005", "Ctrl still held, e");
    event(CTRL, false);
    is(tap(2, 3), "e", "Ctrl let go: e");

    /* A key keeps what it went down as: Sym released while 1 is held. */
    event(SYM, true);
    const char *f1 = event(0, 1, true);
    is(f1, "\033OP", "Sym 1 down");
    event(SYM, false);
    esp_kbd_keys_t r;
    CHECK(esp_kbd_tab5_event(&st, 0, 1, false, &r) && r.n == 0, "F1 up: nothing held");
    prev = r;

    /* Two keys at once: both in the report, the newest's mods. */
    event(2, 1, true);
    is(event(2, 2, true), "w", "q then w held");
    CHECK(esp_kbd_tab5_event(&st, 2, 1, false, &r) && r.n == 1 && r.key[0] == 0x1a,
          "q up: w still held");
    prev = r;
    event(2, 2, false);

    /* Out of range is ignored. */
    CHECK(!esp_kbd_tab5_event(&st, 5, 0, true, &r), "row 5 ignored");
    CHECK(!esp_kbd_tab5_event(&st, 0, 14, true, &r), "column 14 ignored");

    printf("kbd_tab5: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
