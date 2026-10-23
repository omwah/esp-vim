/*
 * The LilyGo T-Deck's keyboard and trackball driver (ESP_VIM_KBD_TDECK).
 *
 * The keyboard is an ESP32-C3 at 0x55 on the board's I2C bus (SDA 18, SCL 8,
 * shared with the touch panel), running LilyGo's Keyboard_ESP32C3 firmware.
 * Written 0x03, that firmware (since June 2025) answers each read with the
 * matrix itself: five bytes, a column each, a bit per row. Older firmware has
 * only its key mode: one byte, the character last typed (0: none), with no
 * Esc, Ctrl or arrows -- it ignores 0x03 and pads a longer read. A raw reply
 * no keyboard could make (bit 7 set, or five equal bytes with keys in them)
 * means that firmware, and the keys are then read as characters. The C3
 * never drives its interrupt line (GPIO46), so it is read every POLL_MS.
 *
 * The keys' backlight is the C3's too: written 0x01 and a brightness (0-255).
 * It comes on with a key or a roll of the trackball, and goes off after
 * :EspKeyLight's seconds without one (esp_kbd_light_set, kept in NVS). Only a
 * change is written. The C3's firmware also toggles it on Alt-B by itself; Alt
 * is Esc here, so that only happens holding Esc down while pressing b.
 *
 * The trackball's sensors toggle GPIO 3, 15, 1 and 2 (up, down, left, right)
 * as it rolls; each toggle is counted in an interrupt, and the task turns the
 * counts into arrow keys. Its click is GPIO0 (BOOT), left to esp_power as the
 * wake key.
 */

#include "esp_kbd.h"
#include "esp_kbd_tdeck.h"

#include "sdkconfig.h"

#if CONFIG_ESP_VIM_KBD_TDECK

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "esp_kbd_tdeck";

#define CMD_LIGHT       0x01            /* then the brightness, 0-255 */
#define CMD_RAW         0x03
#define NVS_NS          "esp_kbd"
#define POLL_MS         20
#define REPORT_ID       0x7DEC          /* this keyboard's keys, among esp_kbd's reports */

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_wake;
static esp_kbd_tdeck_t s_st;
static esp_kbd_tdeck_ball_t s_ball;
static uint8_t s_matrix[ESP_KBD_TDECK_COLS];
static bool s_raw = true;
static volatile int s_toggles[4];
static volatile int s_light_sec = CONFIG_ESP_VIM_KBD_TDECK_LIGHT_SEC;     /* 0: off */
static volatile int s_light_level = CONFIG_ESP_VIM_KBD_TDECK_LIGHT_LEVEL;
static volatile bool s_light_dirty;     /* the setting changed: write it again */
static int64_t s_last_key_us;           /* the last key or roll; 0 none yet */
static int s_lit = -1;                  /* the brightness written last; -1 unknown */

static void light_write(int level)
{
    if (s_dev == NULL || level == s_lit)
        return;
    uint8_t b[2] = { CMD_LIGHT, (uint8_t)level };
    if (i2c_master_transmit(s_dev, b, sizeof b, 50) == ESP_OK)
        s_lit = level;
}

/* On while keys come, off once they've stopped for the setting's seconds. */
static void light(void)
{
    int sec = s_light_sec;
    bool on = sec > 0 && s_last_key_us != 0
              && esp_timer_get_time() - s_last_key_us < (int64_t)sec * 1000000;
    if (s_light_dirty) {
        s_light_dirty = false;
        s_lit = -1;
    }
    light_write(on ? s_light_level : 0);
}

static void active(void)
{
    s_last_key_us = esp_timer_get_time();
}

static const int s_ball_pins[4] = {
    CONFIG_ESP_VIM_KBD_TDECK_BALL_UP, CONFIG_ESP_VIM_KBD_TDECK_BALL_DOWN,
    CONFIG_ESP_VIM_KBD_TDECK_BALL_LEFT, CONFIG_ESP_VIM_KBD_TDECK_BALL_RIGHT,
};

static void on_ball(void *arg)
{
    int dir = (int)(intptr_t)arg;
    s_toggles[dir]++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_wake, &woken);
    if (woken)
        portYIELD_FROM_ISR();
}

static void ball(void)
{
    for (int dir = 0; dir < 4; dir++) {
        int t = __atomic_exchange_n(&s_toggles[dir], 0, __ATOMIC_RELAXED);
        int n = esp_kbd_tdeck_ball(&s_ball, dir, t, CONFIG_ESP_VIM_KBD_TDECK_BALL_STEP);
        char b[16];
        size_t len = esp_kbd_key_bytes(esp_kbd_tdeck_ball_usage(dir), 0, false, b);
        if (n > 0)
            active();
        while (n-- > 0)
            esp_kbd_push(b, len);
    }
}

/* A character from the key-mode firmware. */
static void typed(uint8_t c)
{
    active();
    char b[16];
    size_t n;
    if (c == 0x08) {                    /* Backspace, as the key would send it */
        n = esp_kbd_key_bytes(0x2a, 0, false, b);
    } else {
        b[0] = (char)c;
        n = 1;
    }
    esp_kbd_push(b, n);
}

static bool impossible(const uint8_t m[ESP_KBD_TDECK_COLS])
{
    bool same = true;
    for (int c = 0; c < ESP_KBD_TDECK_COLS; c++) {
        if (m[c] & 0x80)
            return true;
        same = same && m[c] == m[0];
    }
    return same && m[0] != 0;
}

static void keys(void)
{
    uint8_t m[ESP_KBD_TDECK_COLS];
    if (s_dev == NULL)                  /* no keyboard: the trackball alone */
        return;
    if (!s_raw) {
        if (i2c_master_receive(s_dev, m, 1, 50) == ESP_OK && m[0] != 0)
            typed(m[0]);
        return;
    }
    if (i2c_master_receive(s_dev, m, sizeof m, 50) != ESP_OK)
        return;
    if (impossible(m)) {
        s_raw = false;
        printf("ESPVIM-KBD tdeck keyboard: its firmware has no raw mode -- typing "
               "characters only, without Esc, Ctrl or arrows (flash LilyGo's "
               "T-Keyboard_Keyboard_ESP32C3_250620.bin or later to the C3 for them)\n");
        if (m[0] != 0 && !(m[0] & 0x80))
            typed(m[0]);
        return;
    }
    for (int c = 0; c < ESP_KBD_TDECK_COLS; c++) {
        uint8_t changed = (m[c] ^ s_matrix[c]) & 0x7F;
        for (int r = 0; changed && r < ESP_KBD_TDECK_ROWS; r++) {
            if (!(changed & 1u << r))
                continue;
            esp_kbd_keys_t rep;
            if (m[c] & 1u << r)
                active();
            if (esp_kbd_tdeck_event(&s_st, c, r, m[c] & 1u << r, &rep))
                esp_kbd_input(REPORT_ID, &rep);
        }
        s_matrix[c] = m[c];
    }
}

static void kbd_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(POLL_MS));
        ball();
        keys();
        light();
    }
}

static esp_err_t ball_init(void)
{
    uint64_t pins = 0;
    for (int i = 0; i < 4; i++)
        pins |= BIT64(s_ball_pins[i]);
    gpio_config_t g = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t e = gpio_config(&g);
    if (e != ESP_OK)
        return e;
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        return e;
    for (int i = 0; i < 4; i++)
        gpio_isr_handler_add(s_ball_pins[i], on_ball, (void *)(intptr_t)i);
    esp_gpio_reserve(pins);
    return ESP_OK;
}

esp_err_t esp_kbd_tdeck_start(void)
{
    /* The touch panel's bus when it has one on these pins, else our own. */
    i2c_master_bus_handle_t bus = esp_touch_i2c_bus(CONFIG_ESP_VIM_KBD_TDECK_SDA,
                                                    CONFIG_ESP_VIM_KBD_TDECK_SCL);
    if (bus == NULL) {
        i2c_master_bus_config_t bc = {
            .i2c_port = -1,
            .sda_io_num = CONFIG_ESP_VIM_KBD_TDECK_SDA,
            .scl_io_num = CONFIG_ESP_VIM_KBD_TDECK_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t e = i2c_new_master_bus(&bc, &bus);
        if (e != ESP_OK)
            return e;
    }
    s_wake = xSemaphoreCreateBinary();
    if (s_wake == NULL)
        return ESP_ERR_NO_MEM;
    esp_err_t e = ball_init();
    if (e != ESP_OK)
        ESP_LOGW(TAG, "trackball: %s", esp_err_to_name(e));

    /* The C3 starts with the board's peripheral power (esp_board): give it a
     * second to answer. */
    for (int i = 0; i < 10; i++) {
        e = i2c_master_probe(bus, CONFIG_ESP_VIM_KBD_TDECK_ADDR, 50);
        if (e == ESP_OK)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (e != ESP_OK) {
        printf("ESPVIM-KBD tdeck keyboard: no ESP32-C3 at 0x%02x\n", CONFIG_ESP_VIM_KBD_TDECK_ADDR);
    } else {
        /* The C3's firmware is an Arduino I2C slave at 100 kHz. */
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                   .device_address = CONFIG_ESP_VIM_KBD_TDECK_ADDR,
                                   .scl_speed_hz = 100000 };
        e = i2c_master_bus_add_device(bus, &dc, &s_dev);
        if (e == ESP_OK) {
            uint8_t cmd = CMD_RAW;
            e = i2c_master_transmit(s_dev, &cmd, 1, 50);
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "keyboard: %s", esp_err_to_name(e));
            s_dev = NULL;
        }
    }
    esp_kbd_tdeck_reset(&s_st);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint16_t sec;
        uint8_t level;
        if (nvs_get_u16(h, "light_sec", &sec) == ESP_OK)
            s_light_sec = sec;
        if (nvs_get_u8(h, "light_level", &level) == ESP_OK)
            s_light_level = level;
        nvs_close(h);
    }
    if (xTaskCreatePinnedToCore(kbd_task, "td_kbd", 2560, NULL, 5, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (s_dev != NULL) {
        esp_kbd_wired(ESP_KBD_WIRED_TDECK, true);
        ESP_LOGI(TAG, "keyboard ready");
    }
    return s_dev != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t esp_kbd_light_get(int *seconds, int *level)
{
    *seconds = s_light_sec;
    *level = s_light_level;
    return s_dev != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t esp_kbd_light_set(int seconds, int level)
{
    if (s_dev == NULL)
        return ESP_ERR_NOT_FOUND;
    if (seconds < 0 || seconds > 65535 || level < 1 || level > 255)
        return ESP_ERR_INVALID_ARG;
    s_light_sec = seconds;
    s_light_level = level;
    s_light_dirty = true;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_u16(h, "light_sec", (uint16_t)seconds);
    if (e == ESP_OK)
        e = nvs_set_u8(h, "light_level", (uint8_t)level);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e;
}

#else

esp_err_t esp_kbd_tdeck_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_kbd_light_get(int *seconds, int *level)
{
    *seconds = 0;
    *level = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_kbd_light_set(int seconds, int level)
{
    (void)seconds;
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
