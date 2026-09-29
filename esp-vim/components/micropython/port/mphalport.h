/*
 * MicroPython's hardware layer for esp-vim. Implemented in mphal_esp.c (the
 * clock, sleep, randomness) and esp_py.c (output, which goes to Vim).
 *
 * Read by the qstr preprocessing pass too: no ESP-IDF headers here.
 */

#ifndef ESP_VIM_MPHALPORT_H
#define ESP_VIM_MPHALPORT_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* There is no console for Python: CTRL-C is Vim's (esp_py_poll). */
static inline void mp_hal_set_interrupt_char(int c) {
    (void)c;
}

void mp_hal_get_random(size_t n, uint8_t *buf);

/* As the unix port has it: VfsPosix retries a call that EINTR interrupts. */
#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) { \
        for (;;) { \
            ret = syscall; \
            if (ret == -1) { \
                int err = errno; \
                if (err == EINTR) { \
                    mp_handle_pending(true); \
                    continue; \
                } \
                raise; \
            } \
            break; \
        } \
}

#endif
