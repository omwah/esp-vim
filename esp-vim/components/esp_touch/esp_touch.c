/*
 * esp_touch: see include/esp_touch.h.
 *
 * The FT6336G holds its INT line low while a finger is down (its default
 * "polling" interrupt mode); a GT911 pulses it with each new report. Either
 * way the touch task sleeps until INT falls, then reads the first touch point
 * every 16 ms until the panel reports no touches, handing DOWN, MOVE... and UP
 * to the handler. Only the first point is used.
 *
 * A GT911's INT mode is in its own configuration, and on some boards (the
 * LilyGo T-Deck Plus) it differs from one unit to the next: rising or falling
 * pulses, or a level held while touched. ESP_VIM_TOUCH_ANY_EDGE wakes the task
 * on either edge, which catches a touch in every mode; an edge that brings no
 * new report is simply looked past. The configuration itself is never written.
 * Without a reset line the GT911 chose its address (0x5D or 0x14) at power-on,
 * from wherever INT happened to be: the other one is tried as well.
 *
 * Points are turned to the screen by a swap and two mirrors: the Kconfig's to
 * begin with. A GT911's configuration can reverse its axes itself (Module_Switch1,
 * 0x804D: bit 7 Driver_Reversal, bit 6 Sensor_Reversal, bit 3 X2Y), and panels
 * of one board differ there -- the T-Deck Plus's left-right from batch to batch.
 * With ESP_VIM_TOUCH_GT911_SWITCH_REF, the value the Kconfig's turn is right
 * for, a panel whose bits differ from it gets those axes turned back. Over all
 * that, :EspTouch sets a turn of its own (esp_touch_set_turn), kept in NVS.
 *
 * The board's own controller (ESP_VIM_TOUCH_BOARD, the Tab5's) has no interrupt
 * here: it is read every POLL_MS, and its points come in the panel's native
 * orientation, turned the way the display turns the picture.
 */

#include "esp_touch.h"

#include "driver/gpio.h"
#include "esp_board.h"
#include "esp_board_rotate.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "esp_touch";

#define REG_TD_STATUS 0x02              /* FT6336: touches (low nibble), then point 1 */
#define GT_STATUS     0x814E            /* GT911: ready (bit 7), touches; then points */
#define GT_X_MAX      0x8048            /* GT911 configuration: the reported range */
#define GT_CONFIG     0x8047            /* GT911 configuration: its version first */
#define GT_SWITCH1    0x804D            /* ... Module_Switch1 */
#define POLL_MS       16
#define NVS_NS        "esp_touch"

#if !CONFIG_ESP_VIM_TOUCH_BOARD
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_irq;
#endif
#if CONFIG_ESP_VIM_TOUCH_GT911
static uint16_t s_addr = CONFIG_ESP_VIM_TOUCH_ADDR;    /* where it answered */
static int s_cfg_version = -1, s_switch1 = -1;          /* from its configuration */
#endif
static volatile esp_touch_handler_t s_handler;
static void *volatile s_ctx;
static bool s_ready;

/* How points are turned to the screen: the board's (the Kconfig's, after the
 * panel's own reversal bits), and the one in use (NVS's, once :EspTouch set
 * one). One byte, so the touch task reads it whole. */
#define TURN_SWAP 0x01
#define TURN_MX   0x02
#define TURN_MY   0x04
#if CONFIG_ESP_VIM_TOUCH_SWAP_XY
# define KCONFIG_SWAP TURN_SWAP
#else
# define KCONFIG_SWAP 0
#endif
#if CONFIG_ESP_VIM_TOUCH_MIRROR_X
# define KCONFIG_MX TURN_MX
#else
# define KCONFIG_MX 0
#endif
#if CONFIG_ESP_VIM_TOUCH_MIRROR_Y
# define KCONFIG_MY TURN_MY
#else
# define KCONFIG_MY 0
#endif
static uint8_t s_turn_board = KCONFIG_SWAP | KCONFIG_MX | KCONFIG_MY;
static volatile uint8_t s_turn;
static bool s_turn_saved;

#if !CONFIG_ESP_VIM_TOUCH_BOARD
static void on_int(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_irq, &woken);
    if (woken)
        portYIELD_FROM_ISR();
}
#endif

#ifndef CONFIG_ESP_VIM_DISP_ROTATION
# define CONFIG_ESP_VIM_DISP_ROTATION 0         /* no display to follow */
#endif

#if CONFIG_ESP_VIM_TOUCH_BOARD

/* The point turned into the picture's coordinates here, so the swap and
 * mirror settings (which read_point applies) are off for this controller. */
static int read_raw(int *rx, int *ry)
{
    int nx, ny;
    int r = esp_board_touch_read(&nx, &ny);
    if (r <= 0)
        return 0;
    esp_board_rot_to_logical(CONFIG_ESP_VIM_DISP_ROTATION, CONFIG_ESP_VIM_TOUCH_WIDTH,
                             CONFIG_ESP_VIM_TOUCH_HEIGHT, nx, ny, rx, ry);
    return 1;
}

#elif CONFIG_ESP_VIM_TOUCH_GT911

static int s_x_max, s_y_max;            /* the range it reports in; 0: the screen's */

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

/* The board's turn, for a panel whose own axis bits differ from the ones the
 * Kconfig's turn was made for: each axis it reverses differently is turned
 * back. Which of the panel's axes each bit reverses is the datasheet's naming
 * (driver lines along its X, sensor lines along its Y), so far unchecked. */
static void follow_reversal(void)
{
#if CONFIG_ESP_VIM_TOUCH_GT911_SWITCH_REF >= 0
    if (s_switch1 < 0)
        return;
    uint8_t d = (s_switch1 ^ CONFIG_ESP_VIM_TOUCH_GT911_SWITCH_REF) & 0xC8;
    uint8_t t = s_turn_board;
    bool swap = t & TURN_SWAP;
    if (d & 0x80)                       /* Driver_Reversal: its X */
        t ^= swap ? TURN_MY : TURN_MX;
    if (d & 0x40)                       /* Sensor_Reversal: its Y */
        t ^= swap ? TURN_MX : TURN_MY;
    if (d & 0x08)                       /* X2Y: it swaps, or stopped */
        t ^= TURN_SWAP;
    if (d)
        ESP_LOGI(TAG, "Module_Switch1 0x%02x, not 0x%02x: turned to follow it",
                 s_switch1, CONFIG_ESP_VIM_TOUCH_GT911_SWITCH_REF);
    s_turn_board = t;
#endif
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
    /* Scaled from the range its configuration reports in to the screen's, in
     * the panel's own axes: swapped, its X is the screen's Y (a portrait panel
     * turned to landscape). */
    bool swap = s_turn & TURN_SWAP;
    int w = swap ? CONFIG_ESP_VIM_TOUCH_HEIGHT : CONFIG_ESP_VIM_TOUCH_WIDTH;
    int h = swap ? CONFIG_ESP_VIM_TOUCH_WIDTH : CONFIG_ESP_VIM_TOUCH_HEIGHT;
    *rx = (p[1] | p[2] << 8) * w / (s_x_max ? s_x_max : w);
    *ry = (p[3] | p[4] << 8) * h / (s_y_max ? s_y_max : h);
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
    uint8_t t = s_turn;
    int sx = t & TURN_SWAP ? ry : rx, sy = t & TURN_SWAP ? rx : ry;
    if (t & TURN_MX)
        sx = CONFIG_ESP_VIM_TOUCH_WIDTH - 1 - sx;
    if (t & TURN_MY)
        sy = CONFIG_ESP_VIM_TOUCH_HEIGHT - 1 - sy;
    *x = sx < 0 ? 0 : sx >= CONFIG_ESP_VIM_TOUCH_WIDTH ? CONFIG_ESP_VIM_TOUCH_WIDTH - 1 : sx;
    *y = sy < 0 ? 0 : sy >= CONFIG_ESP_VIM_TOUCH_HEIGHT ? CONFIG_ESP_VIM_TOUCH_HEIGHT - 1 : sy;
    return true;
}

/* The turn in use: :EspTouch's, kept in NVS, or the board's. */
static void load_turn(void)
{
    s_turn = s_turn_board;
    s_turn_saved = false;
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "turn", &v) == ESP_OK) {
            s_turn = v & (TURN_SWAP | TURN_MX | TURN_MY);
            s_turn_saved = true;
        }
        nvs_close(h);
    }
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
#if CONFIG_ESP_VIM_TOUCH_BOARD
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));     /* no interrupt: look */
#else
        /* Wait for a touch; also look now and then, in case an edge was missed. */
        xSemaphoreTake(s_irq, pdMS_TO_TICKS(1000));
#endif
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
#if !CONFIG_ESP_VIM_TOUCH_BOARD
        xSemaphoreTake(s_irq, 0);       /* edges from this touch are done with */
#endif
    }
}

#if CONFIG_ESP_VIM_TOUCH_BOARD

esp_err_t esp_touch_init(void)
{
    esp_err_t e = esp_board_touch_init();
    if (e != ESP_OK)
        return e;
    load_turn();
    if (xTaskCreatePinnedToCoreWithCaps(touch_task, "touch", 3072, NULL, 4, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    s_ready = true;
    ESP_LOGI(TAG, "the board's touch controller, read every %d ms", POLL_MS);
    return ESP_OK;
}

#else

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
# if CONFIG_ESP_VIM_TOUCH_RST < 0
    if (e != ESP_OK) {
        /* No reset line to choose its address with: it may be at the other. */
        uint16_t other = s_addr == 0x5D ? 0x14 : 0x5D;
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        dev.device_address = other;
        e = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
        if (e == ESP_OK)
            e = gt_read(GT_X_MAX, m, sizeof m);
        if (e == ESP_OK)
            s_addr = other;
    }
# endif
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no GT911 at 0x%02x%s", CONFIG_ESP_VIM_TOUCH_ADDR,
                 CONFIG_ESP_VIM_TOUCH_RST < 0 ? " or the other address" : "");
        return e;
    }
    s_x_max = m[0] | m[1] << 8;
    s_y_max = m[2] | m[3] << 8;
    uint8_t c;
    if (gt_read(GT_CONFIG, &c, 1) == ESP_OK)
        s_cfg_version = c;
    if (gt_read(GT_SWITCH1, &c, 1) == ESP_OK)
        s_switch1 = c;
    follow_reversal();
    gt_write(GT_STATUS, 0);
#endif

    s_irq = xSemaphoreCreateBinary();
    if (s_irq == NULL)
        return ESP_ERR_NO_MEM;
    gpio_config_t irq = {
        .pin_bit_mask = BIT64(CONFIG_ESP_VIM_TOUCH_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
#if CONFIG_ESP_VIM_TOUCH_ANY_EDGE
        .intr_type = GPIO_INTR_ANYEDGE,
#else
        .intr_type = GPIO_INTR_NEGEDGE,
#endif
    };
    gpio_config(&irq);
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        return e;
    gpio_isr_handler_add(CONFIG_ESP_VIM_TOUCH_INT, on_int, NULL);

    /* Its stack in PSRAM: it does I2C, never flash. */
    load_turn();
    if (xTaskCreatePinnedToCoreWithCaps(touch_task, "touch", 3072, NULL, 4, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    s_ready = true;
#if CONFIG_ESP_VIM_TOUCH_GT911
    ESP_LOGI(TAG, "GT911 at 0x%02x, %dx%d, config version 0x%02x, Module_Switch1 0x%02x",
             s_addr, s_x_max, s_y_max, s_cfg_version & 0xFF, s_switch1 & 0xFF);
#else
    ESP_LOGI(TAG, "touch panel at 0x%02x", CONFIG_ESP_VIM_TOUCH_ADDR);
#endif
    return ESP_OK;
}

#endif

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
#if !CONFIG_ESP_VIM_TOUCH_BOARD
    /* Also when no panel answered: the bus is set up, and its pins taken. */
    if (s_bus != NULL && sda == CONFIG_ESP_VIM_TOUCH_SDA && scl == CONFIG_ESP_VIM_TOUCH_SCL)
        return s_bus;
#endif
    return esp_board_i2c_bus(sda, scl);         /* the board's (the Tab5's), if these are its pins */
}

void esp_touch_get_info(esp_touch_info_t *info)
{
    *info = (esp_touch_info_t){
        .available = s_ready,
        .addr = -1,
        .config_version = -1,
        .switch1 = -1,
        .swap = s_turn & TURN_SWAP,
        .mirror_x = s_turn & TURN_MX,
        .mirror_y = s_turn & TURN_MY,
        .saved = s_turn_saved,
    };
#if CONFIG_ESP_VIM_TOUCH_BOARD
    info->chip = "board";
#elif CONFIG_ESP_VIM_TOUCH_GT911
    info->chip = "GT911";
    info->addr = s_addr;
    info->config_version = s_cfg_version;
    info->switch1 = s_switch1;
#else
    info->chip = "FT6336";
    info->addr = CONFIG_ESP_VIM_TOUCH_ADDR;
#endif
}

esp_err_t esp_touch_set_turn(bool swap, bool mirror_x, bool mirror_y)
{
    if (!s_ready)
        return ESP_ERR_INVALID_STATE;
    uint8_t t = (swap ? TURN_SWAP : 0) | (mirror_x ? TURN_MX : 0) | (mirror_y ? TURN_MY : 0);
    s_turn = t;
    s_turn_saved = true;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_u8(h, "turn", t);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t esp_touch_reset_turn(void)
{
    if (!s_ready)
        return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "turn");
        nvs_commit(h);
        nvs_close(h);
    }
    s_turn = s_turn_board;
    s_turn_saved = false;
    return ESP_OK;
}
