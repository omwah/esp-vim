/*
 * esp_gpio_pins(), esp_gpio_mode(), esp_gpio_read(), esp_gpio_write(): the
 * pins behind :EspGpio.
 *
 * The one thing this must never do is touch a pin the system depends on:
 * reconfiguring a flash or PSRAM line hangs the chip, and the console pins
 * are the only way you are talking to it. ESP-IDF records the pins its drivers
 * claim (flash, PSRAM, UART) in esp_gpio_reserve; the console UART's pins are
 * added here because its default pins are never claimed through that API.
 */

#include "vim.h"
#include "esp_vim_api.h"

#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "soc/uart_pins.h"
#include "sdkconfig.h"

/* The console's pins: a :EspGpio on them would cut off the user. */
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) && defined(CONFIG_IDF_TARGET_ESP32S3)
# include "soc/usb_pins.h"
# define CONSOLE_TX USBPHY_DM_NUM
# define CONSOLE_RX USBPHY_DP_NUM
#elif defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
# define CONSOLE_TX CONFIG_ESP_CONSOLE_UART_TX_GPIO
# define CONSOLE_RX CONFIG_ESP_CONSOLE_UART_RX_GPIO
#else
# define CONSOLE_TX U0TXD_GPIO_NUM
# define CONSOLE_RX U0RXD_GPIO_NUM
#endif

/* Pins configured during this session, and which of them are outputs. A pin
 * nobody configured may not even be routed to the GPIO matrix yet, so a read
 * first makes it an input, and a write first makes it input+output (so it
 * both drives and reads back). */
static uint64_t s_configured_pins, s_output_pins;

static esp_err_t configure(int pin, gpio_mode_t mode)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(pin),
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        s_configured_pins |= BIT64(pin);
        if (mode == GPIO_MODE_INPUT)
            s_output_pins &= ~BIT64(pin);
        else
            s_output_pins |= BIT64(pin);
    }
    return err;
}

bool esp_api_gpio_usable(int pin)
{
    return GPIO_IS_VALID_GPIO(pin)
        && pin != CONSOLE_TX && pin != CONSOLE_RX
        && !esp_gpio_is_reserved(BIT64(pin));
}

/* The pin argument, or -1 (with an error given) if it may not be used. */
static int pin_arg(typval_T *tv, const char *fn)
{
    int error = FALSE;
    varnumber_T pin = tv_get_number_chk(tv, &error);
    if (error)
        return -1;
    if (pin < 0 || pin >= GPIO_NUM_MAX || !GPIO_IS_VALID_GPIO(pin)) {
        semsg("%s(): GPIO %lld does not exist on the %s", fn, (long long)pin, ESP_VIM_CHIP);
        return -1;
    }
    if (!esp_api_gpio_usable((int)pin)) {
        semsg("%s(): GPIO %lld is in use by the system (flash, PSRAM or console)",
              fn, (long long)pin);
        return -1;
    }
    return (int)pin;
}

/* esp_gpio_pins() -> List of the pin numbers :EspGpio may use. */
void f_esp_gpio_pins(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++)
        if (esp_api_gpio_usable(pin))
            list_append_number(rettv->vval.v_list, pin);
}

/*
 * esp_gpio_mode({pin}, {mode}): "in", "in_pullup", "in_pulldown", "out",
 * "od" (open drain, with pull-up), or "off" (back to the reset state).
 */
void f_esp_gpio_mode(typval_T *argvars, typval_T *rettv)
{
    int pin = pin_arg(&argvars[0], "esp_gpio_mode");
    if (pin < 0)
        return;
    const char *mode = (const char *)tv_get_string(&argvars[1]);

    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(pin),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    bool output = false;
    esp_err_t err;
    if (strcmp(mode, "off") == 0) {
        s_configured_pins &= ~BIT64(pin);
        s_output_pins &= ~BIT64(pin);
        err = gpio_reset_pin(pin);
    } else {
        if (strcmp(mode, "in") == 0) {
            cfg.mode = GPIO_MODE_INPUT;
        } else if (strcmp(mode, "in_pullup") == 0) {
            cfg.mode = GPIO_MODE_INPUT;
            cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        } else if (strcmp(mode, "in_pulldown") == 0) {
            cfg.mode = GPIO_MODE_INPUT;
            cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
        } else if (strcmp(mode, "out") == 0) {
            cfg.mode = GPIO_MODE_INPUT_OUTPUT;          /* readable back */
            output = true;
        } else if (strcmp(mode, "od") == 0) {
            cfg.mode = GPIO_MODE_INPUT_OUTPUT_OD;
            cfg.pull_up_en = GPIO_PULLUP_ENABLE;
            output = true;
        } else {
            semsg("esp_gpio_mode(): unknown mode \"%s\" "
                  "(in, in_pullup, in_pulldown, out, od, off)", mode);
            return;
        }
        err = gpio_config(&cfg);
        if (err == ESP_OK) {
            s_configured_pins |= BIT64(pin);
            if (output)
                s_output_pins |= BIT64(pin);
            else
                s_output_pins &= ~BIT64(pin);
        }
    }
    if (err != ESP_OK)
        semsg("esp_gpio_mode(%d, \"%s\"): %s", pin, mode, esp_err_to_name(err));
    rettv->vval.v_number = err == ESP_OK;
}

/* esp_gpio_read({pin}) -> 0 or 1; -1 (with an error) for a pin it may not read. */
void f_esp_gpio_read(typval_T *argvars, typval_T *rettv)
{
    rettv->vval.v_number = -1;
    int pin = pin_arg(&argvars[0], "esp_gpio_read");
    if (pin < 0)
        return;
    if (!(s_configured_pins & BIT64(pin))) {
        esp_err_t err = configure(pin, GPIO_MODE_INPUT);
        if (err != ESP_OK) {
            semsg("esp_gpio_read(%d): %s", pin, esp_err_to_name(err));
            return;
        }
    }
    rettv->vval.v_number = gpio_get_level(pin);
}

/* esp_gpio_write({pin}, {level}): drive the pin, making it an output first
 * (input+output, so it reads back) if it is not one already. */
void f_esp_gpio_write(typval_T *argvars, typval_T *rettv)
{
    int pin = pin_arg(&argvars[0], "esp_gpio_write");
    if (pin < 0)
        return;
    int level = tv_get_number(&argvars[1]) != 0;
    esp_err_t err = ESP_OK;
    if (!(s_output_pins & BIT64(pin)))
        err = configure(pin, GPIO_MODE_INPUT_OUTPUT);
    if (err == ESP_OK)
        err = gpio_set_level(pin, level);
    if (err != ESP_OK)
        semsg("esp_gpio_write(%d, %d): %s", pin, level, esp_err_to_name(err));
    rettv->vval.v_number = err == ESP_OK;
}
