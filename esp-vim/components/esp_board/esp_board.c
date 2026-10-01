/*
 * esp_board: see include/esp_board.h.
 */

#include "esp_board.h"

#include "sdkconfig.h"

#if CONFIG_ESP_VIM_BOARD_TAB5

#include <stdio.h>
#include "bsp/m5stack_tab5.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_io_expander.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "esp_board";

/*
 * The expanders' other pins, which the BSP leaves as they come out of reset
 * (M5Stack's own library sets them the same way):
 *   0x43 P0  antenna: low the internal one, high the external connector
 *   0x44 P7  CHG_EN, high: the battery charges
 *   0x44 P5  nCHG_QC_EN, high: no fast charging (500 mA)
 *   0x44 P4  PWROFF_PULSE: pulsed, it switches the Tab5 off (not used here)
 *   0x44 P6  USB_DET, an input: high while the USB-C port has power (a
 *            computer or a charger)
 */
#define IOE0_ANTENNA    IO_EXPANDER_PIN_NUM_0
#define IOE1_CHG_EN     IO_EXPANDER_PIN_NUM_7
#define IOE1_NQC_EN     IO_EXPANDER_PIN_NUM_5
#define IOE1_USB_DET    IO_EXPANDER_PIN_NUM_6

/* The Tab5's pins the board owns: the internal I2C bus, touch INT, the C6's
 * SDIO lines and reset, the SD slot, the audio codec's I2S and the camera's
 * clock. Reserved so :EspGpio can't take them. (The backlight, 22, is claimed
 * by its LEDC channel.) */
#define TAB5_PINS   (BIT64(31) | BIT64(32) | BIT64(23) \
                   | BIT64(8) | BIT64(9) | BIT64(10) | BIT64(11) | BIT64(12) | BIT64(13) | BIT64(15) \
                   | BIT64(39) | BIT64(40) | BIT64(41) | BIT64(42) | BIT64(43) | BIT64(44) \
                   | BIT64(26) | BIT64(27) | BIT64(28) | BIT64(29) | BIT64(30) | BIT64(36))

#define TAB5_C6_RESET   15              /* esp-hosted's CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE */

static bool s_i2c_ok, s_ioe_ok, s_c6_ok;
static const char *s_panel_name = "";
static esp_lcd_touch_handle_t s_touch;
static esp_io_expander_handle_t s_ioe1;

/* One expander pin as a push-pull output at this level. */
static esp_err_t ioe_out(esp_io_expander_handle_t ioe, uint32_t pin, int level)
{
    if (ioe == NULL)
        return ESP_ERR_INVALID_STATE;
    esp_err_t e = esp_io_expander_set_level(ioe, pin, level);
    if (e == ESP_OK)
        e = esp_io_expander_set_dir(ioe, pin, IO_EXPANDER_OUTPUT);
    if (e == ESP_OK)
        e = esp_io_expander_set_output_mode(ioe, pin, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    return e;
}

esp_err_t esp_board_init(void)
{
    esp_err_t e = bsp_i2c_init();
    esp_gpio_reserve(TAB5_PINS);        /* after: the I2C driver won't take reserved pins */
    s_i2c_ok = e == ESP_OK;
    if (!s_i2c_ok) {
        printf("ESPVIM-BOARD tab5: internal I2C bus: %s\n", esp_err_to_name(e));
        return e;
    }
    /* With ESP_BSP_ERROR_CHECK off, a missing expander is NULL here, not a
     * restart; the BSP's feature switches must then not be asked to use it. */
    esp_io_expander_handle_t ioe0 = bsp_io_expander_init(), ioe1 = bsp_io_expander1_init();
    /* The C6 first: esp-hosted resets it and waits for it once the network
     * starts. Then the USB-A port, the touch controller (which the panel's
     * revision is read from), and the speaker off: nothing plays sound. */
    esp_err_t wifi = ESP_ERR_NOT_FOUND, usb = ESP_ERR_NOT_FOUND, touch = ESP_ERR_NOT_FOUND;
    esp_err_t other = ESP_ERR_NOT_FOUND;
    if (ioe1) {
        /* The C6 held in reset from its power-on until esp-hosted connects
         * (its reset pulse ends by letting go). A C6 still running from
         * before a restart of ours otherwise keeps its old link, and
         * esp-hosted 1.x then retries a dead one without end, at high
         * priority. GPIO15 low is reset, high runs. (esp-hosted 1.4 drives it
         * that way whatever its Kconfig says: its "#ifdef H_RESET_ACTIVE_HIGH"
         * is always true. 2.x reads the setting, and "active low", as
         * M5Stack's config has it, leaves the C6 held in reset.) */
        gpio_set_direction(TAB5_C6_RESET, GPIO_MODE_OUTPUT);
        gpio_set_level(TAB5_C6_RESET, 0);
        wifi = bsp_feature_enable(BSP_FEATURE_WIFI, true);
        usb = bsp_feature_enable(BSP_FEATURE_USB, true);
        other = ioe_out(ioe1, IOE1_NQC_EN, 1);
        if (other == ESP_OK)
            other = ioe_out(ioe1, IOE1_CHG_EN, 1);
        /* The driver's reset leaves every pin a high-impedance output, and
         * one doesn't read its pin: USB_DET an input, pulled down. */
        esp_io_expander_set_dir(ioe1, IOE1_USB_DET, IO_EXPANDER_INPUT);
        esp_io_expander_set_pullupdown(ioe1, IOE1_USB_DET, IO_EXPANDER_PULL_DOWN);
    }
    if (ioe0) {
        touch = bsp_feature_enable(BSP_FEATURE_TOUCH, true);
        bsp_feature_enable(BSP_FEATURE_SPEAKER, false);
        bsp_feature_enable(BSP_FEATURE_CAMERA, false);
        esp_err_t ant = ioe_out(ioe0, IOE0_ANTENNA, 0);
        if (other == ESP_OK)
            other = ant;
    }
    s_ioe_ok = ioe0 && ioe1;
    s_ioe1 = ioe1;
    s_c6_ok = wifi == ESP_OK;
    printf("ESPVIM-BOARD tab5: expander 0x43 %s, 0x44 %s; C6 power %s, USB-A 5V %s, touch %s,"
           " charging %s\n",
           ioe0 ? "ok" : "missing", ioe1 ? "ok" : "missing", esp_err_to_name(wifi),
           esp_err_to_name(usb), esp_err_to_name(touch), esp_err_to_name(other));
    return ioe0 && ioe1 && wifi == ESP_OK ? ESP_OK : ESP_FAIL;
}

const char *esp_board_name(void)
{
    return "M5Stack Tab5";
}

i2c_master_bus_handle_t esp_board_i2c_bus(int sda, int scl)
{
    return s_i2c_ok && sda == BSP_I2C_SDA && scl == BSP_I2C_SCL ? bsp_i2c_get_handle() : NULL;
}

/*
 * Which panel: the touch controller tells. An ST712x answers at 0x55 with its
 * firmware version at register 0 -- 1 on the ST7121, 3 on the ST7123 (the
 * BSP's rule, and M5Stack's). No answer there and a GT911 at 0x14: the first
 * revision, ILI9881C. The ST71xx can take some tens of milliseconds to answer
 * after its reset, so look for up to 600 ms.
 */
enum { PANEL_UNKNOWN, PANEL_ILI9881C, PANEL_ST7123, PANEL_ST7121 };

static int detect_panel(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    for (int i = 0; i < 60; i++) {
        if (i2c_master_probe(bus, 0x55, 50) == ESP_OK) {
            i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                       .device_address = 0x55, .scl_speed_hz = 100000 };
            i2c_master_dev_handle_t dev;
            uint8_t reg[2] = { 0, 0 }, fw = 0;
            if (i2c_master_bus_add_device(bus, &dc, &dev) == ESP_OK) {
                esp_err_t e = i2c_master_transmit_receive(dev, reg, 2, &fw, 1, 50);
                i2c_master_bus_rm_device(dev);
                if (e == ESP_OK && fw == 1)
                    return PANEL_ST7121;
                if (e == ESP_OK && fw == 3)
                    return PANEL_ST7123;
                if (e == ESP_OK)
                    ESP_LOGW(TAG, "ST712x touch firmware %u: not one the BSP knows", fw);
            }
        } else if (i2c_master_probe(bus, 0x14, 50) == ESP_OK) {
            return PANEL_ILI9881C;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return PANEL_UNKNOWN;
}

esp_err_t esp_board_panel_new(esp_lcd_panel_handle_t *panel, int *width, int *height)
{
    if (!s_ioe_ok)                      /* the panel's power and reset are on the expanders */
        return ESP_ERR_INVALID_STATE;
    /* M5Stack's order, and the BSP's: the panel out of reset first -- on the
     * ST7123/ST7121 the touch controller is part of the display chip and stays
     * silent while it's held -- then a touch reset pulse, with TP INT high
     * (the GT911's address select: 0x14), and a moment before asking. */
    bsp_feature_enable(BSP_FEATURE_LCD, true);
    gpio_set_direction(BSP_LCD_TOUCH_INT, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_LCD_TOUCH_INT, 1);
    bsp_feature_enable(BSP_FEATURE_TOUCH, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_direction(BSP_LCD_TOUCH_INT, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(100));
    int kind = detect_panel();
    static const char *const names[] = { "", "ILI9881C", "ST7123", "ST7121" };
    if (kind == PANEL_UNKNOWN) {
        printf("ESPVIM-BOARD tab5: no touch controller at 0x55 or 0x14: panel unknown\n");
        return ESP_ERR_NOT_FOUND;
    }
    /* The BSP's lane rates: 1 Gbit/s, and 965 Mbit/s for the ST7121 (its LVGL
     * start-up path chooses that; the one we call leaves it to us). */
    bsp_display_config_t cfg = {
        .dsi_bus = {
            .phy_clk_src = 0,
            .lane_bit_rate_mbps = kind == PANEL_ST7121 ? 965 : BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };
    bsp_lcd_handles_t h = {0};
    esp_err_t e = bsp_display_new_with_handles(&cfg, &h);
    if (e == ESP_OK)
        e = esp_lcd_panel_disp_on_off(h.panel, true);
    printf("ESPVIM-BOARD tab5: display %s, %dx%d MIPI-DSI: %s\n", names[kind],
           BSP_LCD_H_RES, BSP_LCD_V_RES, esp_err_to_name(e));
    if (e != ESP_OK)
        return e;
    s_panel_name = names[kind];
    *panel = h.panel;
    *width = BSP_LCD_H_RES;
    *height = BSP_LCD_V_RES;
    return ESP_OK;
}

const char *esp_board_panel_name(void)
{
    return s_panel_name;
}

esp_err_t esp_board_backlight(int percent)
{
    return bsp_display_brightness_set(percent);
}

esp_err_t esp_board_touch_init(void)
{
    /* Only for a panel found: the BSP asserts on a board it can't tell. */
    if (!*s_panel_name)
        return ESP_ERR_NOT_FOUND;
    esp_err_t e = bsp_touch_new(NULL, &s_touch);
    printf("ESPVIM-BOARD tab5: touch: %s\n", esp_err_to_name(e));
    return e;
}

int esp_board_touch_read(int *x, int *y)
{
    if (s_touch == NULL || esp_lcd_touch_read_data(s_touch) != ESP_OK)
        return -1;
    esp_lcd_touch_point_data_t p;
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(s_touch, &p, &n, 1) != ESP_OK || n == 0)
        return 0;
    *x = p.x;
    *y = p.y;
    return 1;
}

esp_err_t esp_board_usb_power(bool on)
{
    if (!s_ioe_ok)
        return ESP_ERR_INVALID_STATE;
    return bsp_feature_enable(BSP_FEATURE_USB, on);
}

bool esp_board_coprocessor_powered(void)
{
    return s_c6_ok;
}

int esp_board_usb_c_powered(void)
{
    uint32_t level;
    if (s_ioe1 == NULL || esp_io_expander_get_level(s_ioe1, IOE1_USB_DET, &level) != ESP_OK)
        return -1;
    return level != 0;
}

#else   /* no board layer */

esp_err_t esp_board_init(void) { return ESP_OK; }
const char *esp_board_name(void) { return NULL; }
i2c_master_bus_handle_t esp_board_i2c_bus(int sda, int scl) { (void)sda; (void)scl; return NULL; }
esp_err_t esp_board_panel_new(esp_lcd_panel_handle_t *panel, int *width, int *height)
{
    (void)panel; (void)width; (void)height;
    return ESP_ERR_NOT_SUPPORTED;
}
const char *esp_board_panel_name(void) { return ""; }
esp_err_t esp_board_backlight(int percent) { (void)percent; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_board_touch_init(void) { return ESP_ERR_NOT_SUPPORTED; }
int esp_board_touch_read(int *x, int *y) { (void)x; (void)y; return -1; }
esp_err_t esp_board_usb_power(bool on) { (void)on; return ESP_ERR_NOT_SUPPORTED; }
bool esp_board_coprocessor_powered(void) { return true; }
int esp_board_usb_c_powered(void) { return -1; }

#endif
