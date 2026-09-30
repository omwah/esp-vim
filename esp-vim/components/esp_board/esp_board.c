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
 */
#define IOE0_ANTENNA    IO_EXPANDER_PIN_NUM_0
#define IOE1_CHG_EN     IO_EXPANDER_PIN_NUM_7
#define IOE1_NQC_EN     IO_EXPANDER_PIN_NUM_5

/* The Tab5's pins the board owns: the internal I2C bus, touch INT, the C6's
 * SDIO lines and reset, the SD slot, the audio codec's I2S and the camera's
 * clock. Reserved so :EspGpio can't take them. (The backlight, 22, is claimed
 * by its LEDC channel.) */
#define TAB5_PINS   (BIT64(31) | BIT64(32) | BIT64(23) \
                   | BIT64(8) | BIT64(9) | BIT64(10) | BIT64(11) | BIT64(12) | BIT64(13) | BIT64(15) \
                   | BIT64(39) | BIT64(40) | BIT64(41) | BIT64(42) | BIT64(43) | BIT64(44) \
                   | BIT64(26) | BIT64(27) | BIT64(28) | BIT64(29) | BIT64(30) | BIT64(36))

static bool s_i2c_ok;
static const char *s_panel_name = "";
static esp_lcd_touch_handle_t s_touch;

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
    esp_gpio_reserve(TAB5_PINS);
    esp_err_t e = bsp_i2c_init();
    s_i2c_ok = e == ESP_OK;
    if (!s_i2c_ok) {
        printf("ESPVIM-BOARD tab5: internal I2C bus: %s\n", esp_err_to_name(e));
        return e;
    }
    esp_io_expander_handle_t ioe0 = bsp_io_expander_init(), ioe1 = bsp_io_expander1_init();
    /* The C6 first: esp-hosted resets it and waits for it once the network
     * starts. Then the USB-A port, the touch controller (which the panel's
     * revision is read from), and the speaker off: nothing plays sound. */
    esp_err_t wifi = bsp_feature_enable(BSP_FEATURE_WIFI, true);
    esp_err_t usb = bsp_feature_enable(BSP_FEATURE_USB, true);
    esp_err_t touch = bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    bsp_feature_enable(BSP_FEATURE_SPEAKER, false);
    bsp_feature_enable(BSP_FEATURE_CAMERA, false);
    esp_err_t other = ioe_out(ioe0, IOE0_ANTENNA, 0);
    if (other == ESP_OK)
        other = ioe_out(ioe1, IOE1_NQC_EN, 1);
    if (other == ESP_OK)
        other = ioe_out(ioe1, IOE1_CHG_EN, 1);
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
    if (!s_i2c_ok)
        return ESP_ERR_INVALID_STATE;
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
    if (!s_i2c_ok)
        return ESP_ERR_INVALID_STATE;
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
    return bsp_feature_enable(BSP_FEATURE_USB, on);
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

#endif
