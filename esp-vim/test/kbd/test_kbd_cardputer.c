/*
 * Host test of components/esp_kbd/esp_kbd_cardputer.c: the M5Stack Cardputer
 * ADV's key events, from the TCA8418's key numbers to reports and then, as
 * esp_kbd.c does, to the bytes a new key sends (esp_kbd_key_bytes).
 * `pixi run kbd-test`.
 */

#include "esp_kbd_cardputer.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { fails++; printf("  FAIL  line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

static esp_kbd_cardputer_t st;
static esp_kbd_keys_t prev;

/* Rows and columns as printed (esp_kbd_cardputer.h). */
enum { R0, R1, R2, R3 };
#define GRAVE   R0, 0
#define ONE     R0, 1
#define BKSP    R0, 13
#define TAB     R1, 0
#define LBRACK  R1, 11
#define FN      R2, 0
#define SHIFT   R2, 1
#define KEY_A   R2, 2
#define SEMI    R2, 11
#define ENTER   R2, 13
#define CTRL    R3, 0
#define OPT     R3, 1
#define ALT     R3, 2
#define KEY_X   R3, 4
#define KEY_W   R1, 2
#define COMMA   R3, 10
#define SLASH   R3, 12
#define SPACE   R3, 13

/* What esp_kbd would send for this event: the bytes of each usage that
 * appeared, with the report's modifiers. */
static const char *event(int row, int col, bool down)
{
    static char out[64];
    size_t n = 0;
    esp_kbd_keys_t r;
    if (esp_kbd_cardputer_event(&st, row, col, down, &r)) {
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

static const char *tap(int row, int col)
{
    static char got[64];
    snprintf(got, sizeof got, "%s", event(row, col, true));
    event(row, col, false);
    return got;
}

static const char *chord(int mrow, int mcol, int row, int col)
{
    static char got[64];
    event(mrow, mcol, true);
    snprintf(got, sizeof got, "%s", event(row, col, true));
    event(row, col, false);
    event(mrow, mcol, false);
    return got;
}

static void key_at(int tca, int row, int col)
{
    int r = -1, c = -1;
    bool ok = esp_kbd_cardputer_key(tca, &r, &c);
    CHECK(ok && r == row && c == col, "TCA key %d is row %d col %d, got %d,%d", tca, row, col, r, c);
}

int main(void)
{
    /* The TCA8418's numbering (row * 10 + column + 1 of its 7x8 matrix). */
    key_at(1, GRAVE);
    key_at(2, TAB);
    key_at(3, FN);
    key_at(4, CTRL);
    key_at(5, ONE);
    key_at(7, SHIFT);
    key_at(8, OPT);
    key_at(68, SPACE);                  /* matrix row 6, column 7: the last key */
    int r, c;
    CHECK(!esp_kbd_cardputer_key(9, &r, &c), "column 8 isn't wired");
    CHECK(!esp_kbd_cardputer_key(71, &r, &c), "row 7 isn't wired");
    CHECK(!esp_kbd_cardputer_key(0, &r, &c), "0 is no key");

    esp_kbd_cardputer_reset(&st);
    CHECK(!strcmp(tap(KEY_A), "a"), "a");
    CHECK(!strcmp(tap(ONE), "1"), "1");
    CHECK(!strcmp(tap(SPACE), " "), "space");
    CHECK(!strcmp(tap(ENTER), "\r"), "Enter");
    CHECK(!strcmp(tap(TAB), "\t"), "Tab");
    CHECK(!strcmp(tap(BKSP), "\x7f"), "Backspace, got %s", tap(BKSP));

    /* ` is Esc; Shift-` is ~ as printed; Fn-` is `. */
    CHECK(!strcmp(tap(GRAVE), "\x1b"), "` sends Esc");
    CHECK(!strcmp(chord(SHIFT, GRAVE), "~"), "Shift-` is ~");
    CHECK(!strcmp(chord(FN, GRAVE), "`"), "Fn-` is `");

    /* Shift: the printed second legends. */
    CHECK(!strcmp(chord(SHIFT, KEY_A), "A"), "Shift-a");
    CHECK(!strcmp(chord(SHIFT, ONE), "!"), "Shift-1");
    CHECK(!strcmp(chord(SHIFT, SEMI), ":"), "Shift-;");
    CHECK(!strcmp(chord(SHIFT, SLASH), "?"), "Shift-/");
    CHECK(!strcmp(chord(SHIFT, COMMA), "<"), "Shift-,");

    /* The arrows printed on ; , . / are the plain keys (application cursor
     * keys, as Vim asks for), and Fn gives the characters. */
    CHECK(!strcmp(tap(SEMI), "\x1bOA"), "; is Up");
    CHECK(!strcmp(tap(COMMA), "\x1bOD"), ", is Left");
    CHECK(!strcmp(tap(SLASH), "\x1bOC"), "/ is Right");
    CHECK(!strcmp(chord(FN, SEMI), ";"), "Fn-; is ;");
    CHECK(!strcmp(chord(FN, COMMA), ","), "Fn-, is ,");
    CHECK(!strcmp(chord(FN, SLASH), "/"), "Fn-/ is /");

    /* The Fn layer. */
    CHECK(!strcmp(chord(FN, ONE), "\x1bOP"), "Fn-1 is F1, got %s", chord(FN, ONE));
    CHECK(!strcmp(chord(FN, BKSP), "\x1b[3~"), "Fn-Bksp is Delete");
    CHECK(!strcmp(chord(FN, LBRACK), "\x1b[5~"), "Fn-[ is PgUp");

    /* Ctrl and Alt, held. */
    CHECK(!strcmp(chord(CTRL, KEY_W), "\x17"), "Ctrl-W");
    CHECK(!strcmp(chord(ALT, KEY_X), "\x1bx"), "Alt-x is Meta, got %s", chord(ALT, KEY_X));

    /* Tapped modifiers latch for one key. */
    tap(CTRL);
    CHECK(!strcmp(tap(KEY_W), "\x17"), "Ctrl tapped, then w");
    CHECK(!strcmp(tap(KEY_W), "w"), "... for one key only");
    tap(FN);
    CHECK(!strcmp(tap(SEMI), ";"), "Fn tapped, then ;");
    CHECK(!strcmp(tap(SEMI), "\x1bOA"), "... for one key only");
    tap(CTRL);
    tap(CTRL);
    CHECK(!strcmp(tap(KEY_W), "w"), "Ctrl tapped twice: dropped");

    /* Shift twice is Caps Lock, three times ends it. */
    tap(SHIFT);
    tap(SHIFT);
    CHECK(!strcmp(tap(KEY_A), "A"), "Caps Lock");
    CHECK(!strcmp(tap(KEY_A), "A"), "... stays on");
    CHECK(!strcmp(tap(ONE), "1"), "... letters only");
    tap(SHIFT);
    CHECK(!strcmp(tap(KEY_A), "a"), "Caps Lock off");

    /* Opt does nothing yet, and doesn't upset the next key. */
    CHECK(!strcmp(chord(OPT, KEY_A), "a"), "Opt-a is a");

    /* A key held while Fn is let go keeps its Fn meaning (; stays ;). */
    event(FN, true);
    event(SEMI, true);
    event(FN, false);
    esp_kbd_keys_t rr;
    bool changed = esp_kbd_cardputer_event(&st, SEMI, false, &rr);
    CHECK(changed && rr.n == 0, "Up let go");
    prev = rr;

    printf("cardputer keyboard: %d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
