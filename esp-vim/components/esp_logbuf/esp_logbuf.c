/*
 * esp_logbuf: see include/esp_logbuf.h.
 */

#include "esp_logbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define LINE    192                     /* longer lines are cut there */

static char *s_buf;                     /* s_size bytes, a ring */
static size_t s_size;
static size_t s_head, s_len;            /* where the next byte goes; bytes held */
static unsigned s_count;
static volatile bool s_capture;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_console;        /* what the log used before: the console */

static int hook(const char *fmt, va_list ap)
{
    if (s_buf == NULL)
        return s_console(fmt, ap);
    if (!s_capture) {                   /* kept, and printed */
        va_list again;
        va_copy(again, ap);
        s_console(fmt, again);
        va_end(again);
    }
    char line[LINE];
    int n = vsnprintf(line, sizeof line, fmt, ap);
    if (n < 0)
        return n;
    size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
    if (len && line[len - 1] != '\n' && (size_t)n >= sizeof line)
        line[len - 1] = '\n';           /* cut short: still a line */
    taskENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < len; i++) {
        s_buf[s_head] = line[i];
        s_head = (s_head + 1) % s_size;
    }
    s_len = s_len + len > s_size ? s_size : s_len + len;
    if (memchr(line, '\n', len))
        s_count++;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

void esp_logbuf_init(void)
{
    if (s_console != NULL)
        return;
    s_size = 16 * 1024;
    s_buf = heap_caps_malloc(s_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {                /* no PSRAM: a smaller one */
        s_size = 4 * 1024;
        s_buf = heap_caps_malloc(s_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    s_console = esp_log_set_vprintf(hook);
}

void esp_logbuf_capture(bool on)
{
    s_capture = on;
}

size_t esp_logbuf_copy(char *out, size_t n)
{
    if (n == 0)
        return 0;
    if (s_buf == NULL) {
        out[0] = '\0';
        return 0;
    }
    size_t len = 0;
    taskENTER_CRITICAL(&s_mux);
    size_t held = s_len < n - 1 ? s_len : n - 1;
    size_t start = (s_head + s_size - held) % s_size;
    for (size_t i = 0; i < held; i++)
        out[len++] = s_buf[(start + i) % s_size];
    bool whole = held == s_len && s_len < s_size;
    taskEXIT_CRITICAL(&s_mux);
    out[len] = '\0';
    if (!whole) {                       /* the ring wrapped: drop the cut first line */
        char *nl = memchr(out, '\n', len);
        size_t skip = nl ? (size_t)(nl - out) + 1 : len;
        memmove(out, out + skip, len - skip + 1);
        len -= skip;
    }
    return len;
}

void esp_logbuf_clear(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_len = 0;
    taskEXIT_CRITICAL(&s_mux);
}

unsigned esp_logbuf_count(void)
{
    return s_count;
}
