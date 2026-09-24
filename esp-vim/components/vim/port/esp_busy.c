/*
 * Busy indicator: a small spinner at the bottom right of the screen
 * while Vim is working rather than waiting for you.
 *
 * On a microcontroller, sourcing a syntax file, opening help or reading a large
 * file can take a noticeable time, and a terminal that shows nothing at all
 * looks exactly like a hung device.
 *
 * How "busy" is detected, without touching Vim's sources: Vim waits for input
 * with select() and a timeout (or none), and during long work it polls for
 * CTRL-C with select() and a ZERO timeout (mch_breakcheck). The select wrapper
 * in esp_shims.c calls in here for both:
 *
 *   esp_vim_busy_poll()   zero-timeout poll: Vim is working. After BUSY_AFTER_US
 *                         since it last waited, draw the next spinner frame.
 *   esp_vim_busy_idle()   Vim is about to wait for input: put back whatever the
 *                         spinner covered, then remember when the wait ended.
 *
 * Both run on the Vim task, from inside Vim's own call chain, so the spinner
 * goes through Vim's output buffer in order with everything else Vim writes --
 * it can never land in the middle of one of Vim's escape sequences, which a
 * separate task writing to the UART could.
 *
 * Drawing does not disturb Vim's idea of the screen: DECSC/DECRC (ESC 7/ESC 8)
 * save and restore the cursor and attributes around the frame. Erasing redraws
 * the covered cell from Vim's own screen model (screen_char), then puts the
 * cursor back where Vim had it.
 *
 * Off with ":let g:esp_busy = 0".
 */

#include "vim.h"
#include "esp_timer.h"
#include "esp_vim_port.h"

#define BUSY_AFTER_US   300000      /* quiet for short operations */
#define FRAME_US        120000

static int64_t s_idle_end;          /* when Vim last stopped waiting for input */
static int64_t s_last_frame;
static int s_frame;
static bool s_shown;
static int s_row, s_col;            /* where the spinner was drawn */

static bool screen_usable(void)
{
    return ScreenLines != NULL && LineOffset != NULL && full_screen
        && starting == 0 && !exiting && screen_Rows > 0 && screen_Columns > 0;
}

static bool enabled(void)
{
    char_u *v = get_var_value((char_u *)"g:esp_busy");
    return v == NULL || atoi((char *)v) != 0;
}

void esp_vim_busy_poll(void)
{
    int64_t now = esp_timer_get_time();
    if (s_idle_end == 0 || now - s_idle_end < BUSY_AFTER_US
            || now - s_last_frame < FRAME_US || !screen_usable() || !enabled())
        return;

    static const char frames[] = "|/-\\";
    char buf[48];
    /* Bottom row, one in from the right. Not the very last cell: writing that
     * one scrolls some terminals, and Vim itself will not redraw it unless
     * the terminal has "xn" -- builtin_xterm does not -- so it could never
     * be put back. */
    s_row = screen_Rows - 1;
    s_col = screen_Columns >= 2 ? screen_Columns - 2 : 0;
    /* ESC 7: save cursor+attrs; reverse video; ESC 8: restore both. */
    snprintf(buf, sizeof buf, "\0337\033[%d;%dH\033[7m%c\033[27m\0338",
             s_row + 1, s_col + 1, frames[s_frame++ & 3]);
    out_str_nf((char_u *)buf);
    out_flush();
    s_last_frame = now;
    s_shown = true;
}

void esp_vim_busy_idle(void)
{
    if (s_shown) {
        s_shown = false;
        if (screen_usable() && s_row < screen_Rows && s_col < screen_Columns) {
            int row = screen_cur_row, col = screen_cur_col;
            screen_char(LineOffset[s_row] + s_col, s_row, s_col);
            windgoto(row, col);
            out_flush();
        }
    }
    s_idle_end = 0;                 /* not busy while waiting */
}

void esp_vim_busy_wait_done(void)
{
    s_idle_end = esp_timer_get_time();
    s_frame = 0;
}
