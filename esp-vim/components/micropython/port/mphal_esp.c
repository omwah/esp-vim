/*
 * MicroPython's hardware layer on ESP-IDF: the clock, sleep, randomness, the
 * task and its stack, the heap block, and finding the GC's roots.
 *
 * Kept out of the qstr pass (CMakeLists.txt): it includes ESP-IDF's headers,
 * and must not use MP_QSTR_ names.
 */

#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "py/gc.h"
#include "py/mphal.h"
#include "py/mpstate.h"
#include "py/runtime.h"

#include "esp_py_port.h"

mp_uint_t mp_hal_ticks_ms(void) {
    return (mp_uint_t)(esp_timer_get_time() / 1000);
}

mp_uint_t mp_hal_ticks_us(void) {
    return (mp_uint_t)esp_timer_get_time();
}

mp_uint_t mp_hal_ticks_cpu(void) {
    return (mp_uint_t)esp_cpu_get_cycle_count();
}

uint64_t mp_hal_time_ns(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
}

/* Sleep in slices, so CTRL-C (esp_py_poll) and other pending exceptions are
 * seen within one: time.sleep(60) must not hold the editor for a minute. */
void mp_hal_delay_ms(mp_uint_t ms) {
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    for (;;) {
        int64_t left = end - esp_timer_get_time();
        if (left <= 0) {
            break;
        }
        int64_t slice = left < 20000 ? left : 20000;
        vTaskDelay(slice >= portTICK_PERIOD_MS * 1000 ? pdMS_TO_TICKS(slice / 1000) : 1);
        esp_py_poll();
        mp_handle_pending(true);
    }
}

void mp_hal_delay_us(mp_uint_t us) {
    if (us >= 10000) {
        mp_hal_delay_ms(us / 1000);
        return;
    }
    int64_t end = esp_timer_get_time() + us;
    while (esp_timer_get_time() < end) {
    }
}

void mp_hal_get_random(size_t n, uint8_t *buf) {
    esp_fill_random(buf, n);
}

uint32_t esp_py_random_seed(void) {
    return esp_random();
}

size_t esp_py_port_stack_avail(void *here) {
    uint8_t *lo = pxTaskGetStackStart(NULL);
    return (uint8_t *)here > lo ? (size_t)((uint8_t *)here - lo) : 0;
}

void *esp_py_port_alloc_heap(size_t *size) {
    *size = (size_t)CONFIG_ESP_VIM_PY_HEAP_KB * 1024;
    void *p = NULL;
#if CONFIG_SPIRAM
    p = heap_caps_malloc(*size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (p == NULL) {
        p = heap_caps_malloc(*size, MALLOC_CAP_8BIT);
    }
    return p;
}

/*
 * The GC's roots: the registers and the stack, from here up to where Python
 * was entered (MP_STATE_THREAD(stack_top), set by esp_py.c). After
 * MicroPython's ESP32 port (ports/esp32/gccollect.c, MIT).
 */
#if CONFIG_IDF_TARGET_ARCH_XTENSA

#include "xtensa/hal.h"

/* Recurse deep enough that the register windows spill every caller's
 * registers onto the stack; level is volatile so the recursion stays. */
static void gc_collect_inner(volatile unsigned int level) {
    if (level < XCHAL_NUM_AREGS / 8) {
        gc_collect_inner(level + 1);
    } else {
        volatile uint32_t sp = (uint32_t)esp_cpu_get_sp();
        gc_collect_root((void **)sp,
            ((mp_uint_t)MP_STATE_THREAD(stack_top) - sp) / sizeof(uint32_t));
    }
}

void gc_collect(void) {
    gc_collect_start();
    gc_collect_inner(0);
    gc_collect_end();
}

#else

#include "shared/runtime/gchelper.h"

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

#endif
