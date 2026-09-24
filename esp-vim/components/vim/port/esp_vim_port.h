/*
 * Hooks the rest of the firmware uses to plug into the Vim port.
 */
#pragma once

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
