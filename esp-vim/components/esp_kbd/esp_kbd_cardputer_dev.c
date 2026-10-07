/*
 * The M5Stack Cardputer ADV keyboard's driver (ESP_VIM_KBD_CARDPUTER).
 *
 * A TCA8418 key-scan controller at 0x34 on the board's I2C bus (SDA 8, SCL 9,
 * shared with the audio codec and the IMU), wired as 7 rows of 8 columns, with
 * its interrupt on GPIO11. Its registers, from the TCA8418 datasheet:
 *   0x01  CFG: bit 0 KE_IEN, an interrupt for key events
 *   0x02  INT_STAT: bit 0 K_INT; written 1 to clear
 *   0x03  KEY_LCK_EC: bits 3-0 the events queued (a FIFO of 10)
 *   0x04  KEY_EVENT_A: the next event: bit 7 down, bits 6-0 the key (1-80); 0 none
 *   0x1D-0x1F  KP_GPIO1-3: which row and column pins scan the matrix
 * A task waits for the interrupt (and looks every 20 ms regardless: on some
 * units the line isn't wired), reads the events, and hands esp_kbd_input()
 * what esp_kbd_cardputer.c makes of them. Under a launcher the chip may be
 * left mid-transaction by the app before: the first probe is tried again.
 */

#include "esp_kbd.h"
#include "esp_kbd_cardputer.h"

#include "sdkconfig.h"

#if CONFIG_ESP_VIM_KBD_CARDPUTER

#include <stdio.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "esp_kbd_cardputer";

#define ADDR            0x34
#define REG_CFG         0x01
#define REG_INT_STAT    0x02
#define REG_KEY_LCK_EC  0x03
#define REG_KEY_EVENT_A 0x04
#define REG_KP_GPIO1    0x1D
#define REG_KP_GPIO2    0x1E
#define REG_KP_GPIO3    0x1F
#define CFG_KE_IEN      0x01
#define INT_K_INT       0x01
#define REPORT_ID       0xCA8D          /* this keyboard's keys, among esp_kbd's reports */

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_irq;
static esp_kbd_cardputer_t s_st;

static esp_err_t rd(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, v, 1, 50);
}

static esp_err_t wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_dev, b, 2, 50);
}

static void on_int(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_irq, &woken);
    if (woken)
        portYIELD_FROM_ISR();
}

/* The matrix, key events on, and anything queued from before thrown away. */
static esp_err_t setup(void)
{
    esp_err_t e = wr(REG_KP_GPIO1, 0x7F);           /* rows 0-6 */
    if (e == ESP_OK)
        e = wr(REG_KP_GPIO2, 0xFF);                 /* columns 0-7 */
    if (e == ESP_OK)
        e = wr(REG_KP_GPIO3, 0x00);
    if (e == ESP_OK)
        e = wr(REG_CFG, CFG_KE_IEN);
    uint8_t ev;
    for (int i = 0; e == ESP_OK && i < 16; i++) {
        e = rd(REG_KEY_EVENT_A, &ev);
        if (ev == 0)
            break;
    }
    if (e == ESP_OK)
        e = wr(REG_INT_STAT, 0x1F);
    return e;
}

/* Read the events waiting. */
static void drain(void)
{
    uint8_t n = 0;
    if (rd(REG_KEY_LCK_EC, &n) != ESP_OK)
        return;
    n &= 0x0F;
    for (int i = 0; i < n; i++) {
        uint8_t ev;
        if (rd(REG_KEY_EVENT_A, &ev) != ESP_OK || ev == 0)
            break;
        int row, col;
        esp_kbd_keys_t r;
        if (esp_kbd_cardputer_key(ev & 0x7F, &row, &col)
                && esp_kbd_cardputer_event(&s_st, row, col, ev & 0x80, &r))
            esp_kbd_input(REPORT_ID, &r);
    }
    wr(REG_INT_STAT, INT_K_INT);
}

static void kbd_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_irq, pdMS_TO_TICKS(20));
        drain();
    }
}

esp_err_t esp_kbd_cardputer_start(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_ESP_VIM_KBD_CARDPUTER_SDA,
        .scl_io_num = CONFIG_ESP_VIM_KBD_CARDPUTER_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bc, &bus);
    if (e != ESP_OK)
        return e;
    /* Three tries: a launcher can leave it in the middle of a transfer. */
    for (int i = 0; i < 3; i++) {
        e = i2c_master_probe(bus, ADDR, 50);
        if (e == ESP_OK)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (e != ESP_OK) {
        printf("ESPVIM-KBD cardputer keyboard: no TCA8418 at 0x%02x\n", ADDR);
        return e;
    }
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR,
                               .scl_speed_hz = 400000 };
    e = i2c_master_bus_add_device(bus, &dc, &s_dev);
    if (e == ESP_OK)
        e = setup();
    if (e != ESP_OK)
        return e;
    esp_kbd_cardputer_reset(&s_st);

    s_irq = xSemaphoreCreateBinary();
    if (s_irq == NULL)
        return ESP_ERR_NO_MEM;
    gpio_config_t irq = {
        .pin_bit_mask = BIT64(CONFIG_ESP_VIM_KBD_CARDPUTER_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&irq);
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        return e;
    gpio_isr_handler_add(CONFIG_ESP_VIM_KBD_CARDPUTER_INT, on_int, NULL);

    if (xTaskCreatePinnedToCore(kbd_task, "cp_kbd", 2560, NULL, 5, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    esp_kbd_wired(ESP_KBD_WIRED_CARDPUTER, true);
    ESP_LOGI(TAG, "keyboard ready");
    return ESP_OK;
}

#else

esp_err_t esp_kbd_cardputer_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
