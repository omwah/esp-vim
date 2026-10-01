/*
 * The M5Stack Tab5 keyboard accessory's driver (ESP_VIM_KBD_TAB5).
 *
 * The keyboard is an STM32 at 0x6D, on its own I2C bus (Ext.Port1, SDA 0,
 * SCL 1), with an interrupt line (GPIO50, low when an event waits). Its
 * registers, from M5Stack's driver (M5Tab5-Keyboard-UserDemo):
 *   0x01  interrupt status: bit 0 a normal-mode key event waits; written 0 to clear
 *   0x02  events queued; written 0 to empty the queue
 *   0x10  mode: 0 normal (row and column), 1 HID, 2 characters
 *   0x11  LED mode: 1 set by us
 *   0x20  the next event: bit 7 down, bits 6-4 row, 3-0 column; 0xFF none
 *   0x60  LED 1 blue, green, red; 0x64 LED 2 the same
 *   0xFE  firmware version
 * A task waits for the interrupt (and looks every 100 ms regardless), reads
 * the events, and hands esp_kbd_input() what esp_kbd_tab5.c makes of them.
 * With no keyboard it looks for one every two seconds: it clips on and off.
 */

#include "esp_kbd.h"
#include "esp_kbd_tab5.h"

#include "sdkconfig.h"

#if CONFIG_ESP_VIM_KBD_TAB5

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "esp_kbd_tab5";

#define ADDR        0x6D
#define REG_INT_STA 0x01
#define REG_EVENTS  0x02
#define REG_MODE    0x10
#define REG_LED_MODE 0x11
#define REG_EVENT   0x20
#define REG_LED1    0x60
#define REG_LED2    0x64
#define REG_VERSION 0xFE
#define REPORT_ID   0x7AB5              /* this keyboard's keys, among esp_kbd's reports */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_irq;
static esp_kbd_tab5_t s_st;

static esp_err_t rd(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, v, 1, 50);
}

static esp_err_t wr(uint8_t reg, const uint8_t *v, size_t n)
{
    uint8_t b[8] = { reg };
    memcpy(b + 1, v, n);
    return i2c_master_transmit(s_dev, b, n + 1, 50);
}

static esp_err_t wr1(uint8_t reg, uint8_t v)
{
    return wr(reg, &v, 1);
}

static void on_int(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_irq, &woken);
    if (woken)
        portYIELD_FROM_ISR();
}

/* The LEDs: the first Sym (blue), Aa (amber) or Caps Lock (red); the second
 * Ctrl (cyan) or Alt (white). Off when nothing is in effect. */
static void leds(unsigned ind)
{
    uint8_t a[3] = { 0, 0, 0 }, b[3] = { 0, 0, 0 };     /* blue, green, red */
    if (ind & ESP_KBD_TAB5_IND_SYM)
        a[0] = 0x60;
    else if (ind & ESP_KBD_TAB5_IND_CAPS)
        a[2] = 0x60;
    else if (ind & ESP_KBD_TAB5_IND_SHIFT)
        a[1] = 0x20, a[2] = 0x60;
    if (ind & ESP_KBD_TAB5_IND_CTRL)
        b[0] = 0x60, b[1] = 0x60;
    else if (ind & ESP_KBD_TAB5_IND_ALT)
        b[0] = b[1] = b[2] = 0x40;
    wr(REG_LED1, a, 3);
    wr(REG_LED2, b, 3);
}

/* Whether it answers. Asked every two seconds while it's away, and the I2C
 * driver logs an error for each probe that times out (as it does with nothing
 * on the port to pull the lines up): that would land on Vim's screen. */
static bool answers(void)
{
    esp_log_level_t was = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_err_t e = i2c_master_probe(s_bus, ADDR, 50);
    esp_log_level_set("i2c.master", was);
    return e == ESP_OK;
}

/* It answers: set it up. */
static bool attach(void)
{
    if (!answers())
        return false;
    uint8_t ver = 0;
    esp_err_t e = rd(REG_VERSION, &ver);
    if (e == ESP_OK)
        e = wr1(REG_MODE, 0);           /* normal: rows and columns */
    if (e == ESP_OK)
        e = wr1(REG_EVENTS, 0);
    if (e == ESP_OK)
        e = wr1(REG_INT_STA, 0);
    if (e == ESP_OK)
        e = wr1(REG_LED_MODE, 1);       /* ours to set */
    /* A failure once, not at every retry: something answering at 0x6D that
     * then won't talk would print a line every two seconds. */
    static esp_err_t s_failed = ESP_OK;
    if (e == ESP_OK || e != s_failed)
        printf("ESPVIM-KBD tab5 keyboard: firmware 0x%02x, %s\n", ver, esp_err_to_name(e));
    s_failed = e;
    if (e != ESP_OK)
        return false;
    esp_kbd_tab5_reset(&s_st);
    leds(0);
    esp_kbd_wired(ESP_KBD_WIRED_TAB5, true);
    return true;
}

/* Gone: whatever it held is let go. */
static void detach(void)
{
    esp_kbd_keys_t none = { .has_mods = true };
    esp_kbd_input(REPORT_ID, &none);
    esp_kbd_tab5_reset(&s_st);
    esp_kbd_wired(ESP_KBD_WIRED_TAB5, false);
    printf("ESPVIM-KBD tab5 keyboard: detached\n");
}

/* Read the events waiting; false if the keyboard stopped answering. */
static bool drain(void)
{
    uint8_t sta;
    if (rd(REG_INT_STA, &sta) != ESP_OK)
        return false;
    if (!(sta & 0x01))
        return true;
    uint8_t count = 0;
    if (rd(REG_EVENTS, &count) != ESP_OK)
        return false;
    unsigned ind = esp_kbd_tab5_indicators(&s_st);
    for (int i = 0; i < count && i < 32; i++) {
        uint8_t ev;
        if (rd(REG_EVENT, &ev) != ESP_OK)
            return false;
        if (ev == 0xFF)
            break;
        esp_kbd_keys_t r;
        if (esp_kbd_tab5_event(&s_st, (ev >> 4) & 0x07, ev & 0x0F, ev & 0x80, &r))
            esp_kbd_input(REPORT_ID, &r);
    }
    wr1(REG_INT_STA, 0);
    if (esp_kbd_tab5_indicators(&s_st) != ind)
        leds(esp_kbd_tab5_indicators(&s_st));
    return true;
}

static void kbd_task(void *arg)
{
    bool present = false;
    for (;;) {
        if (!present) {
            present = attach();
            if (!present) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }
        xSemaphoreTake(s_irq, pdMS_TO_TICKS(100));
        if (!drain()) {
            present = false;
            detach();
        }
    }
}

esp_err_t esp_kbd_tab5_start(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_ESP_VIM_KBD_TAB5_SDA,
        .scl_io_num = CONFIG_ESP_VIM_KBD_TAB5_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bc, &s_bus);
    if (e != ESP_OK)
        return e;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR,
                               .scl_speed_hz = 100000 };
    e = i2c_master_bus_add_device(s_bus, &dc, &s_dev);
    if (e != ESP_OK)
        return e;
    esp_gpio_reserve(BIT64(CONFIG_ESP_VIM_KBD_TAB5_SDA) | BIT64(CONFIG_ESP_VIM_KBD_TAB5_SCL)
                     | BIT64(CONFIG_ESP_VIM_KBD_TAB5_INT));

    s_irq = xSemaphoreCreateBinary();
    if (s_irq == NULL)
        return ESP_ERR_NO_MEM;
    gpio_config_t irq = {
        .pin_bit_mask = BIT64(CONFIG_ESP_VIM_KBD_TAB5_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&irq);
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        return e;
    gpio_isr_handler_add(CONFIG_ESP_VIM_KBD_TAB5_INT, on_int, NULL);

    /* Its stack in PSRAM: it does I2C, never flash. */
    if (xTaskCreatePinnedToCoreWithCaps(kbd_task, "tab5_kbd", 3072, NULL, 5, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "looking for the keyboard on SDA %d / SCL %d", CONFIG_ESP_VIM_KBD_TAB5_SDA,
             CONFIG_ESP_VIM_KBD_TAB5_SCL);
    return ESP_OK;
}

#else

esp_err_t esp_kbd_tab5_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
