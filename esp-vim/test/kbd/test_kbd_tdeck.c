/*
 * Host test of components/esp_kbd/esp_kbd_tdeck.c: the LilyGo T-Deck's key
 * events, by their place in the ESP32-C3's matrix, to reports and then, as
 * esp_kbd.c does, to the bytes a new key sends (esp_kbd_key_bytes); and the
 * trackball's toggles to arrow keys. `pixi run kbd-test`.
 */

#include "esp_kbd_tdeck.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("  FAIL  line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static esp_kbd_tdeck_t st;
static esp_kbd_keys_t prev;

/* Column and row in the C3's matrix (its firmware's keyboard[col][row]). */
#define KEY_Q   0, 0
#define KEY_W   0, 1
#define SYM     0, 2
#define KEY_A   0, 3
#define ALT     0, 4
#define SPACE   0, 5
#define MIC     0, 6
#define KEY_E   1, 0
#define KEY_S   1, 1
#define KEY_P   1, 3
#define LSHIFT  1, 6
#define KEY_T   2, 2
#define RSHIFT  2, 3
#define KEY_H   3, 1
#define ENTER   3, 3
#define KEY_J   3, 6
#define KEY_O   4, 0
#define KEY_L   4, 1
#define BKSP    4, 3
#define DOLLAR  4, 4
#define KEY_K   4, 6

static const char *event(int col, int row, bool down)
{
    static char out[64];
    size_t n = 0;
    esp_kbd_keys_t r;
    if (esp_kbd_tdeck_event(&st, col, row, down, &r)) {
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

static const char *tap(int col, int row)
{
    static char got[64];
    snprintf(got, sizeof got, "%s", event(col, row, true));
    event(col, row, false);
    return got;
}

static const char *chord(int mcol, int mrow, int col, int row)
{
    static char got[64];
    event(mcol, mrow, true);
    snprintf(got, sizeof got, "%s", event(col, row, true));
    event(col, row, false);
    event(mcol, mrow, false);
    return got;
}

/* Sym and Shift held, then the key. */
static const char *sym2(int col, int row)
{
    static char got[64];
    event(SYM, true);
    event(LSHIFT, true);
    snprintf(got, sizeof got, "%s", event(col, row, true));
    event(col, row, false);
    event(LSHIFT, false);
    event(SYM, false);
    return got;
}

#define IS(got, want, what) CHECK(!strcmp((got), (want)), "%s", what)

int main(void)
{
    esp_kbd_tdeck_reset(&st);
    IS(tap(KEY_A), "a", "a");
    IS(tap(SPACE), " ", "space");
    IS(tap(ENTER), "\r", "Enter");
    IS(tap(BKSP), "\x7f", "Backspace");
    IS(tap(DOLLAR), "$", "$");
    IS(tap(ALT), "\x1b", "Alt is Esc");
    IS(chord(SYM, ALT), "\x1b", "Sym-Alt is still Esc");

    /* Shift, either one; $ stays $. */
    IS(chord(LSHIFT, KEY_A), "A", "Shift-a");
    IS(chord(RSHIFT, KEY_Q), "Q", "right Shift-q");
    IS(chord(LSHIFT, DOLLAR), "$", "Shift-$");
    IS(chord(LSHIFT, ENTER), "\r", "Shift-Enter");

    /* Mic is Ctrl. */
    IS(chord(MIC, KEY_W), "\x17", "Mic-w is Ctrl-W");
    IS(chord(MIC, KEY_O), "\x0f", "Mic-o is Ctrl-O");

    /* Sym: the printed legends. */
    IS(chord(SYM, KEY_Q), "#", "Sym-q");
    IS(chord(SYM, KEY_W), "1", "Sym-w");
    IS(chord(SYM, KEY_E), "2", "Sym-e");
    IS(chord(SYM, KEY_P), "@", "Sym-p");
    IS(chord(SYM, KEY_A), "*", "Sym-a");
    IS(chord(SYM, KEY_H), ":", "Sym-h");
    IS(chord(SYM, KEY_J), ";", "Sym-j");
    IS(chord(SYM, KEY_L), "\"", "Sym-l");
    IS(chord(SYM, KEY_O), "+", "Sym-o");
    IS(chord(SYM, MIC), "0", "Sym-Mic");
    IS(chord(SYM, SPACE), "\t", "Sym-Space is Tab");
    IS(chord(SYM, BKSP), "\033[3~", "Sym-Bksp is Delete");

    /* Sym and Shift: what isn't printed. */
    IS(sym2(KEY_Q), "~", "Sym-Shift-q");
    IS(sym2(KEY_W), "`", "Sym-Shift-w");
    IS(sym2(KEY_T), "[", "Sym-Shift-t");
    IS(sym2(KEY_S), "\\", "Sym-Shift-s");
    IS(sym2(KEY_A), "|", "Sym-Shift-a");
    IS(sym2(KEY_O), "=", "Sym-Shift-o");
    IS(sym2(KEY_H), "\033OD", "Sym-Shift-h is Left");
    IS(sym2(KEY_J), "\033OB", "Sym-Shift-j is Down");
    IS(sym2(KEY_K), "\033OA", "Sym-Shift-k is Up");
    IS(sym2(KEY_L), "\033OC", "Sym-Shift-l is Right");
    IS(sym2(MIC), "0", "Sym-Shift-Mic falls back to Sym's 0");

    /* Tapped modifiers latch for one key. */
    IS(tap(MIC), "", "Mic alone sends nothing");
    IS(tap(KEY_W), "\x17", "tapped Mic, then w");
    IS(tap(KEY_W), "w", "the latch is used up");
    tap(MIC);
    tap(MIC);
    IS(tap(KEY_W), "w", "Mic tapped twice is dropped");

    /* Sym held, Mic types 0 and is no Ctrl; let go, Mic is Ctrl again. */
    event(SYM, true);
    IS(event(MIC, true), "0", "Sym-Mic down types 0");
    event(SYM, false);
    IS(event(MIC, false), "", "Mic up");
    IS(chord(MIC, KEY_W), "\x17", "Mic is Ctrl again");
    tap(SYM);
    IS(tap(KEY_Q), "#", "tapped Sym, then q");
    IS(tap(KEY_Q), "q", "Sym's latch is used up");
    tap(SYM);
    tap(LSHIFT);
    IS(tap(KEY_T), "[", "tapped Sym and Shift, then t");

    /* Shift tapped twice is Caps Lock; again ends it. */
    tap(LSHIFT);
    tap(LSHIFT);
    IS(tap(KEY_A), "A", "Caps Lock");
    IS(tap(KEY_A), "A", "Caps Lock stays");
    IS(chord(LSHIFT, KEY_A), "a", "Shift under Caps Lock");
    tap(LSHIFT);
    IS(tap(KEY_A), "a", "Caps Lock off");

    /* Both Shifts down, one let go: still shifted. */
    event(LSHIFT, true);
    event(RSHIFT, true);
    event(RSHIFT, false);
    IS(tap(KEY_A), "A", "one Shift still held");
    event(LSHIFT, false);
    IS(tap(KEY_A), "a", "both Shifts up");

    /* A key keeps what it went down as. */
    event(SYM, true);
    IS(event(KEY_Q, true), "#", "Sym-q down");
    event(SYM, false);
    esp_kbd_keys_t r;
    CHECK(esp_kbd_tdeck_event(&st, KEY_Q, false, &r) && r.n == 0, "q up: nothing held");
    prev = r;
    CHECK(!esp_kbd_tdeck_event(&st, 5, 0, true, &r), "column 5 is out of range");
    CHECK(!esp_kbd_tdeck_event(&st, 0, 7, true, &r), "row 7 is out of range");

    /* The trackball. */
    esp_kbd_tdeck_ball_t b = { 0 };
    CHECK(esp_kbd_tdeck_ball(&b, ESP_KBD_TDECK_UP, 1, 2) == 0, "one toggle: no step yet");
    CHECK(esp_kbd_tdeck_ball(&b, ESP_KBD_TDECK_UP, 1, 2) == 1, "two toggles: one step");
    CHECK(esp_kbd_tdeck_ball(&b, ESP_KBD_TDECK_UP, 5, 2) == 2, "five: two, one left");
    CHECK(esp_kbd_tdeck_ball(&b, ESP_KBD_TDECK_DOWN, 1, 2) == 0, "down once");
    CHECK(esp_kbd_tdeck_ball(&b, ESP_KBD_TDECK_UP, 1, 2) == 0, "rolling down forgot up's remainder");
    CHECK(esp_kbd_tdeck_ball(&b, ESP_KBD_TDECK_LEFT, 3, 1) == 3, "step 1: a key a toggle");
    CHECK(esp_kbd_tdeck_ball(&b, 4, 3, 1) == 0, "no such direction");
    CHECK(esp_kbd_tdeck_ball_usage(ESP_KBD_TDECK_UP) == 0x52, "up is Up");
    CHECK(esp_kbd_tdeck_ball_usage(ESP_KBD_TDECK_RIGHT) == 0x4f, "right is Right");

    printf("test_kbd_tdeck: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
