/*
 * Hooks the rest of the firmware uses to plug into the Vim port.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register a non-blocking "is input waiting?" check for a console fd.
 *
 * ESP-IDF's select() rounds a zero timeout up to one FreeRTOS tick, so each of
 * Vim's constant "any key yet?" polls slept for up to a tick -- measured at
 * 2 ms each, ~45x a plain loop iteration. For fds registered here the port
 * answers a zero-timeout select() immediately instead. Unregistered fds, and
 * any select() with a real timeout, go to ESP-IDF unchanged.
 *
 * The UART console registers in app_main; the Tab5 display console (Phase 10)
 * will register its own.
 */
void esp_vim_register_input_poll(int fd, bool (*input_pending)(void));

/*
 * Show console output somewhere else as well: every write() to stdout -- Vim's
 * screen, and the supervisor's messages between sessions -- is also passed to
 * {mirror} (the display console, Phase 10b), before it goes to the console.
 * NULL turns it off.
 */
void esp_vim_set_output_mirror(void (*mirror)(const void *buf, size_t len));

/*
 * A second source of console input -- keyboards (components/esp_kbd) -- read
 * alongside the console on fd 0: read() takes its bytes first, and select()
 * wakes for either. {pending} must not block; {read} returns what it has.
 * Like the others, re-registered at each session start. NULL turns it off.
 */
void esp_vim_set_extra_input(bool (*pending)(void), int (*read)(void *buf, size_t len));

/*
 * Vim sessions: restarting Vim in place after :q, without rebooting.
 *
 *   esp_vim_session_t s;
 *   esp_vim_session_init(&s, heap_budget);    // once, before any Vim code runs
 *   for (;;) {
 *       esp_vim_session_begin(&s);            // power-on state, empty heap
 *       ... vim_main() ...                     // leaves via esp_vim_session_exit()
 *   }
 *
 * The session object must live OUTSIDE Vim (it is what survives the reset).
 */
typedef struct {
    void   *data_snapshot;      /* Vim's .data as it was at power-on */
    size_t  data_size;
    size_t  heap_budget;        /* most PSRAM Vim may hold; 0 = unlimited */
} esp_vim_session_t;

esp_err_t esp_vim_session_init(esp_vim_session_t *sess, size_t heap_budget);
void      esp_vim_session_begin(const esp_vim_session_t *sess);

/*
 * Called on the Vim task when Vim exits (via exit()), after the ESPVIM-EXIT
 * line is printed. The port's weak default parks the task; the firmware
 * provides a strong definition that starts a new session.
 */
void esp_vim_session_exit(int status) __attribute__((noreturn));

/* Bytes Vim holds now, its high-water mark, and its budget. */
void esp_vim_heap_stats(size_t *used, size_t *peak, size_t *total);

/*
 * Busy indicator (port/esp_busy.c), driven by the select() wrapper on the Vim
 * task: a zero-timeout console poll means Vim is working, a real wait means it
 * is idle. Not for use elsewhere.
 */
void esp_vim_busy_poll(void);

void esp_vim_busy_idle(void);
void esp_vim_busy_wait_done(void);

#ifdef __cplusplus
}
#endif
