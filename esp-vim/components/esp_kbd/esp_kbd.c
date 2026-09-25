/*
 * esp_kbd: see include/esp_kbd.h.
 *
 * A report is compared with the previous one: a usage that appears is a key
 * press, and its bytes are queued at once; the newest pressed key then
 * repeats (after 500 ms, 30 times a second) until it is released or another
 * key is pressed -- as a PC keyboard behaves. US layout.
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

/* Modifier bits of byte 0. */
#define MOD_CTRL   (0x01 | 0x10)
#define MOD_SHIFT  (0x02 | 0x20)
#define MOD_ALT    (0x04 | 0x40)

static StreamBufferHandle_t s_queue;
static SemaphoreHandle_t s_lock;        /* the queue has one writer at a time */
static esp_timer_handle_t s_repeat;
static uint8_t s_prev[6];               /* key usages held in the last report */
static uint8_t s_repeat_key, s_repeat_mods;
static bool s_caps;

/* Printable keys by usage, 0x04..0x38: unshifted and shifted. */
static const char s_plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\x7f\t -=[]\\#;'`,./";
static const char s_shift[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\x7f\t _+{}|~:\"~<>?";

/* Keys that send escape sequences, as xterm does with the cursor keys in
 * application mode -- which Vim switches on (t_ks). */
static const char *special(uint8_t usage)
{
    switch (usage) {
    case 0x3a: return "\033OP";         /* F1..F4 */
    case 0x3b: return "\033OQ";
    case 0x3c: return "\033OR";
    case 0x3d: return "\033OS";
    case 0x3e: return "\033[15~";       /* F5..F12 */
    case 0x3f: return "\033[17~";
    case 0x40: return "\033[18~";
    case 0x41: return "\033[19~";
    case 0x42: return "\033[20~";
    case 0x43: return "\033[21~";
    case 0x44: return "\033[23~";
    case 0x45: return "\033[24~";
    case 0x49: return "\033[2~";        /* Insert */
    case 0x4a: return "\033OH";         /* Home */
    case 0x4b: return "\033[5~";        /* Page Up */
    case 0x4c: return "\033[3~";        /* Delete */
    case 0x4d: return "\033OF";         /* End */
    case 0x4e: return "\033[6~";        /* Page Down */
    case 0x4f: return "\033OC";         /* Right */
    case 0x50: return "\033OD";         /* Left */
    case 0x51: return "\033OB";         /* Down */
    case 0x52: return "\033OA";         /* Up */
    default:   return NULL;
    }
}

static void queue(const char *s, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xStreamBufferSend(s_queue, s, n, 0);    /* a full queue drops keys */
    xSemaphoreGive(s_lock);
}

/* The bytes for one key press; false if the key sends nothing. */
static bool send_key(uint8_t usage, uint8_t mods)
{
    char buf[8];
    size_t n = 0;
    const char *seq = special(usage);
    if (seq != NULL) {
        queue(seq, strlen(seq));
        return true;
    }
    if (usage < 0x04 || usage > 0x38 || usage == 0x32)
        return false;                   /* 0x32 is the non-US '#' key */
    int i = usage - 0x04;
    bool shift = (mods & MOD_SHIFT) != 0;
    if (usage <= 0x1d && s_caps)        /* Caps Lock shifts letters only */
        shift = !shift;
    char c = shift ? s_shift[i] : s_plain[i];
    if (mods & MOD_CTRL) {
        if (c >= 'a' && c <= 'z')
            c = c - 'a' + 1;
        else if (c >= '@' && c <= '_')  /* ^@ ^[ ^\ ^] ^^ ^_ and capitals */
            c = c - '@';
        else if (c == ' ' || c == '2')
            c = 0;
        else if (c == '6')
            c = 0x1e;
        else if (c == '-')
            c = 0x1f;
    }
    if (mods & MOD_ALT)
        buf[n++] = '\033';              /* Meta sends ESC first, as xterm does */
    buf[n++] = c;
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

void esp_kbd_report(const uint8_t *r, size_t len)
{
    if (s_queue == NULL || len < 3)
        return;
    uint8_t mods = r[0];
    /* Boot layout: modifiers, reserved, six keys. Some keyboards leave the
     * reserved byte out. */
    const uint8_t *keys = len >= 8 ? r + 2 : r + 1;
    size_t nkeys = len >= 8 ? 6 : len - 1;
    if (nkeys > 6)
        nkeys = 6;

    uint8_t now[6] = {0};
    memcpy(now, keys, nkeys);
    if (now[0] == 0x01)                 /* ErrorRollOver: too many keys; ignore */
        return;

    uint8_t newest = 0;
    for (size_t i = 0; i < 6; i++) {
        uint8_t k = now[i];
        if (k == 0 || memchr(s_prev, k, sizeof s_prev) != NULL)
            continue;
        if (k == 0x39) {                /* Caps Lock */
            s_caps = !s_caps;
            continue;
        }
        if (send_key(k, mods))
            newest = k;
    }
    memcpy(s_prev, now, sizeof s_prev);

    if (newest) {                       /* start repeating the newest key */
        s_repeat_key = newest;
        s_repeat_mods = mods;
        esp_timer_stop(s_repeat);
        esp_timer_start_once(s_repeat, REPEAT_DELAY_US);
    } else if (s_repeat_key && memchr(now, s_repeat_key, sizeof now) == NULL) {
        s_repeat_key = 0;               /* the repeating key was released */
        esp_timer_stop(s_repeat);
    } else {
        s_repeat_mods = mods;
    }
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
    s_repeat_key = 0;
    if (s_repeat)
        esp_timer_stop(s_repeat);
    memset(s_prev, 0, sizeof s_prev);
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
    s_queue = xStreamBufferCreateWithCaps(QUEUE_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const esp_timer_create_args_t t = { .callback = on_repeat_timer, .name = "kbd_repeat" };
    if (s_lock == NULL || s_queue == NULL || esp_timer_create(&t, &s_repeat) != ESP_OK)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
