/*
 * esp_kbd: see include/esp_kbd.h.
 *
 * The keys held are compared with the last report's: a usage that appears is
 * a key press, and its bytes (esp_kbd_key_bytes(), esp_kbd_hid.c) are queued
 * at once; the newest pressed key then repeats (after 500 ms, 30 times a
 * second) until it is released or another key is pressed -- as a PC keyboard
 * behaves. US layout.
 */

#include "esp_kbd.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"

#define QUEUE_SIZE     1024
#define REPEAT_DELAY_US  (500 * 1000)
#define REPEAT_RATE_US   (33 * 1000)

static StreamBufferHandle_t s_queue;
static SemaphoreHandle_t s_lock;        /* the queue has one writer at a time */
static SemaphoreHandle_t s_input_lock;  /* ... and the key state: USB, Bluetooth and the Tab5's keyboard each have a task */
static esp_timer_handle_t s_repeat;
static esp_kbd_keys_t s_prev;           /* the keys held, as of the last report */
static esp_kbd_state_t s_state;         /* each report's keys (esp_kbd_merge) */
static uint8_t s_repeat_key, s_repeat_mods;
static bool s_caps;

static void queue(const char *s, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xStreamBufferSend(s_queue, s, n, 0);    /* a full queue drops keys */
    xSemaphoreGive(s_lock);
}

/* The bytes for one key press; false if the key sends nothing. */
static bool send_key(uint8_t usage, uint8_t mods)
{
    char buf[16];
    size_t n = esp_kbd_key_bytes(usage, mods, s_caps, buf);
    if (n == 0)
        return false;
    queue(buf, n);
    return true;
}

void esp_kbd_push(const char *bytes, size_t len)
{
    if (s_queue != NULL)
        queue(bytes, len);
}

static void on_repeat(void *arg)
{
    if (s_repeat_key)
        send_key(s_repeat_key, s_repeat_mods);
}

static bool holds(const esp_kbd_keys_t *k, uint8_t usage)
{
    return memchr(k->key, usage, k->n) != NULL;
}

static void input(uint16_t id, const esp_kbd_keys_t *report)
{
    esp_kbd_keys_t state, *now = &state;
    esp_kbd_merge(&s_state, id, report, &state);    /* with the other reports' keys */
    uint8_t newest = 0;
    for (int i = 0; i < now->n; i++) {
        uint8_t k = now->key[i];
        if (holds(&s_prev, k))
            continue;
        if (k == 0x39) {                /* Caps Lock */
            s_caps = !s_caps;
            continue;
        }
        if (send_key(k, now->mods))
            newest = k;
    }
    s_prev = *now;

    if (newest) {                       /* start repeating the newest key */
        s_repeat_key = newest;
        s_repeat_mods = now->mods;
        esp_timer_stop(s_repeat);
        esp_timer_start_once(s_repeat, REPEAT_DELAY_US);
    } else if (s_repeat_key && !holds(now, s_repeat_key)) {
        s_repeat_key = 0;               /* the repeating key was released */
        esp_timer_stop(s_repeat);
    } else {
        s_repeat_mods = now->mods;
    }
}

void esp_kbd_input(uint16_t id, const esp_kbd_keys_t *report)
{
    if (s_queue == NULL)
        return;
    xSemaphoreTake(s_input_lock, portMAX_DELAY);
    input(id, report);
    xSemaphoreGive(s_input_lock);
}

void esp_kbd_report(const uint8_t *r, size_t len)
{
    esp_kbd_keys_t k;
    if (esp_kbd_decode_boot(r, len, &k))
        esp_kbd_input(0, &k);
}

/* One-shot timer re-armed at the repeat rate while the key stays down. */
static void on_repeat_timer(void *arg)
{
    if (s_repeat_key == 0)
        return;
    on_repeat(arg);
    esp_timer_start_once(s_repeat, REPEAT_RATE_US);
}

void esp_kbd_release_all(void)
{
    if (s_input_lock)
        xSemaphoreTake(s_input_lock, portMAX_DELAY);
    s_repeat_key = 0;
    if (s_repeat)
        esp_timer_stop(s_repeat);
    memset(&s_prev, 0, sizeof s_prev);
    memset(&s_state, 0, sizeof s_state);
    if (s_input_lock)
        xSemaphoreGive(s_input_lock);
}

static volatile unsigned s_wired;

void esp_kbd_wired(unsigned which, bool attached)
{
    if (attached)
        __atomic_or_fetch(&s_wired, which, __ATOMIC_RELAXED);
    else
        __atomic_and_fetch(&s_wired, ~which, __ATOMIC_RELAXED);
}

bool esp_kbd_wired_attached(void)
{
    return s_wired != 0;
}

bool esp_kbd_pending(void)
{
    return s_queue != NULL && xStreamBufferBytesAvailable(s_queue) > 0;
}

int esp_kbd_read(void *buf, size_t len)
{
    if (s_queue == NULL)
        return 0;
    return (int)xStreamBufferReceive(s_queue, buf, len, 0);
}

esp_err_t esp_kbd_init(void)
{
    if (s_queue != NULL)
        return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_input_lock = xSemaphoreCreateMutex();
    s_queue = xStreamBufferCreateWithCaps(QUEUE_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const esp_timer_create_args_t t = { .callback = on_repeat_timer, .name = "kbd_repeat" };
    if (s_lock == NULL || s_input_lock == NULL || s_queue == NULL || esp_timer_create(&t, &s_repeat) != ESP_OK)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
