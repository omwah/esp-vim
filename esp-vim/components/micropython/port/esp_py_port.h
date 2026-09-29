/*
 * What esp_py.c needs from ESP-IDF, implemented in mphal_esp.c (which is not
 * read by the qstr pass, so it may include ESP-IDF's headers).
 */

#ifndef ESP_PY_PORT_H
#define ESP_PY_PORT_H

#include <stddef.h>

/* Bytes of the current task's stack below `here`. */
size_t esp_py_port_stack_avail(void *here);

/* The Python heap: one block of Kconfig ESP_VIM_PY_HEAP_KB, from PSRAM when
 * there is some. Sets *size either way; NULL if it can't be had. */
void  *esp_py_port_alloc_heap(size_t *size);

void   esp_py_poll(void);

#endif
