/*
 * esp_touch: see include/esp_touch.h.
 *
 * The FT6336G holds its INT line low while a finger is down (its default
 * "polling" interrupt mode); a GT911 pulses it with each new report. Either
 * way the touch task sleeps until INT falls, then reads the first touch point
 * every 16 ms until the panel reports no touches, handing DOWN, MOVE... and UP
 * to the handler. Only the first point is used.
 */

#include "esp_touch.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "esp_touch";

#define REG_TD_STATUS 0x02              /* FT6336: touches (low nibble), then point 1 */
#define GT_STATUS     0x814E            /* GT911: ready (bit 7), touches; then points */
#define GT_X_MAX      0x8048            /* GT911 configuration: the reported range */
#define POLL_MS       16

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_irq;
static volatile esp_touch_handler_t s_handler;
static void *volatile s_ctx;
static bool s_ready;

static void on_int(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_irq, &woken);
    if (woken)
        portYIELD_FROM_ISR();
}

#if CONFIG_ESP_VIM_TOUCH_GT911

static int s_x_max = CONFIG_ESP_VIM_TOUCH_WIDTH, s_y_max = CONFIG_ESP_VIM_TOUCH_HEIGHT;

static esp_err_t gt_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t r[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(s_dev, r, 2, buf, len, 50);
}

static void gt_write(uint16_t reg, uint8_t val)
{
    uint8_t b[3] = { reg >> 8, reg & 0xFF, val };
    i2c_master_transmit(s_dev, b, sizeof b, 50);
}

/* The first point in the panel's own coordinates: 1 touching, 0 not, -1 no
 * new report yet (the last one still holds). */
static int read_raw(int *rx, int *ry)
{
    uint8_t st, p[5];
    if (gt_read(GT_STATUS, &st, 1) != ESP_OK)
        return 0;
    if (!(st & 0x80))
        return -1;
    int n = st & 0x0F;
    bool ok = n >= 1 && n <= 5 && gt_read(GT_STATUS + 1, p, sizeof p) == ESP_OK;
    gt_write(GT_STATUS, 0);             /* taken: the next report may come */
    if (!ok)
        return 0;
    /* Scaled from the range its configuration reports in to the screen's. */
    *rx = (p[1] | p[2] << 8) * CONFIG_ESP_VIM_TOUCH_WIDTH / s_x_max;
    *ry = (p[3] | p[4] << 8) * CONFIG_ESP_VIM_TOUCH_HEIGHT / s_y_max;
    return 1;
}

#else

static int read_raw(int *rx, int *ry)
{
    uint8_t reg = REG_TD_STATUS, b[5];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof b, 50) != ESP_OK)
        return 0;
    int n = b[0] & 0x0F;
    if (n < 1 || n > 2)
        return 0;
    *rx = (b[1] & 0x0F) << 8 | b[2];
    *ry = (b[3] & 0x0F) << 8 | b[4];
    return 1;
}

#endif

/* The first touch point, in screen coordinates; false when nothing touches. */
static bool read_point(int *x, int *y)
{
    static bool down;
    static int rx, ry;
    int r = read_raw(&rx, &ry);
    if (r >= 0)
        down = r;
    if (!down)
        return false;
#if CONFIG_ESP_VIM_TOUCH_SWAP_XY
    int sx = ry, sy = rx;
#else
    int sx = rx, sy = ry;
#endif
#if CONFIG_ESP_VIM_TOUCH_MIRROR_X
    sx = CONFIG_ESP_VIM_TOUCH_WIDTH - 1 - sx;
#endif
#if CONFIG_ESP_VIM_TOUCH_MIRROR_Y
    sy = CONFIG_ESP_VIM_TOUCH_HEIGHT - 1 - sy;
#endif
    *x = sx < 0 ? 0 : sx >= CONFIG_ESP_VIM_TOUCH_WIDTH ? CONFIG_ESP_VIM_TOUCH_WIDTH - 1 : sx;
    *y = sy < 0 ? 0 : sy >= CONFIG_ESP_VIM_TOUCH_HEIGHT ? CONFIG_ESP_VIM_TOUCH_HEIGHT - 1 : sy;
    return true;
}

static void deliver(esp_touch_event_t ev, int x, int y)
{
    esp_touch_handler_t h = s_handler;
    if (h)
        h(ev, x, y, s_ctx);
}

static void touch_task(void *arg)
{
    for (;;) {
        /* Wait for a touch; also look now and then, in case an edge was missed. */
        xSemaphoreTake(s_irq, pdMS_TO_TICKS(1000));
        int x, y;
        if (!read_point(&x, &y))
            continue;
        deliver(ESP_TOUCH_DOWN, x, y);
        int lx = x, ly = y;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            if (!read_point(&x, &y))
                break;
            lx = x, ly = y;
            deliver(ESP_TOUCH_MOVE, x, y);
        }
        deliver(ESP_TOUCH_UP, lx, ly);
        xSemaphoreTake(s_irq, 0);       /* edges from this touch are done with */
    }
}

esp_err_t esp_touch_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_ESP_VIM_TOUCH_SDA,
        .scl_io_num = CONFIG_ESP_VIM_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bus, &s_bus);
    if (e != ESP_OK)
        return e;
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_ESP_VIM_TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    e = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    if (e != ESP_OK)
        return e;

    /* The pins are the panel's: keep :EspGpio off them. (The I2C commands use
     * the bus through esp_touch_i2c_bus().) */
    uint64_t pins = BIT64(CONFIG_ESP_VIM_TOUCH_SDA) | BIT64(CONFIG_ESP_VIM_TOUCH_SCL)
                  | BIT64(CONFIG_ESP_VIM_TOUCH_INT);
#if CONFIG_ESP_VIM_TOUCH_RST >= 0
    pins |= BIT64(CONFIG_ESP_VIM_TOUCH_RST);
    gpio_config_t rst = { .pin_bit_mask = BIT64(CONFIG_ESP_VIM_TOUCH_RST), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(CONFIG_ESP_VIM_TOUCH_RST, 0);    /* reset before the first read */
# if CONFIG_ESP_VIM_TOUCH_GT911
    /* INT's level as reset ends picks the address: low 0x5D, high 0x14. */
    gpio_config_t sel = { .pin_bit_mask = BIT64(CONFIG_ESP_VIM_TOUCH_INT), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&sel);
    gpio_set_level(CONFIG_ESP_VIM_TOUCH_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CONFIG_ESP_VIM_TOUCH_INT, CONFIG_ESP_VIM_TOUCH_ADDR == 0x14);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(CONFIG_ESP_VIM_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(CONFIG_ESP_VIM_TOUCH_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_direction(CONFIG_ESP_VIM_TOUCH_INT, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(50));
# else
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CONFIG_ESP_VIM_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
# endif
#endif
    esp_gpio_reserve(pins);

#if CONFIG_ESP_VIM_TOUCH_GT911
    /* The range it reports in, from its configuration; the screen's if unset. */
    uint8_t m[4];
    e = gt_read(GT_X_MAX, m, sizeof m);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no GT911 at 0x%02x", CONFIG_ESP_VIM_TOUCH_ADDR);
        return e;
    }
    if (m[0] | m[1])
        s_x_max = m[0] | m[1] << 8;
    if (m[2] | m[3])
        s_y_max = m[2] | m[3] << 8;
    gt_write(GT_STATUS, 0);
#endif

    s_irq = xSemaphoreCreateBinary();
    if (s_irq == NULL)
        return ESP_ERR_NO_MEM;
    gpio_config_t irq = {
        .pin_bit_mask = BIT64(CONFIG_ESP_VIM_TOUCH_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&irq);
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        return e;
    gpio_isr_handler_add(CONFIG_ESP_VIM_TOUCH_INT, on_int, NULL);

    /* Its stack in PSRAM: it does I2C, never flash. */
    if (xTaskCreatePinnedToCoreWithCaps(touch_task, "touch", 3072, NULL, 4, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    s_ready = true;
#if CONFIG_ESP_VIM_TOUCH_GT911
    ESP_LOGI(TAG, "GT911 at 0x%02x, %dx%d", CONFIG_ESP_VIM_TOUCH_ADDR, s_x_max, s_y_max);
#else
    ESP_LOGI(TAG, "touch panel at 0x%02x", CONFIG_ESP_VIM_TOUCH_ADDR);
#endif
    return ESP_OK;
}

bool esp_touch_available(void)
{
    return s_ready;
}

void esp_touch_set_handler(esp_touch_handler_t handler, void *ctx)
{
    s_handler = NULL;                   /* never a new handler with the old ctx */
    s_ctx = ctx;
    s_handler = handler;
}

i2c_master_bus_handle_t esp_touch_i2c_bus(int sda, int scl)
{
    return s_ready && sda == CONFIG_ESP_VIM_TOUCH_SDA && scl == CONFIG_ESP_VIM_TOUCH_SCL ? s_bus : NULL;
}
