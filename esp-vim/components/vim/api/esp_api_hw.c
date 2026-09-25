/*
 * esp_serial_*(), esp_i2c_scan(), esp_adc_read(), esp_sensors(): the chip's
 * peripherals, behind :EspSerial, :EspI2cScan, :EspAdc and :EspSensors.
 *
 * Every pin goes through the same check as :EspGpio (esp_api_gpio_usable):
 * nothing the flash, PSRAM or console depends on can be taken.
 */

#include "vim.h"
#include "esp_vim_api.h"
#include "esp_touch.h"

#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "sdkconfig.h"

/* -------------------------------------------------------------- serial -- */

static bool serial_port_ok(int port)
{
    if (port >= 1 && port < SOC_UART_HP_NUM)
        return true;
    semsg("esp_serial: UART%d cannot be used (UART0 is the console; this chip has UART1..UART%d)",
          port, SOC_UART_HP_NUM - 1);
    return false;
}

static bool pin_ok(int pin, const char *fn)
{
    if (esp_api_gpio_usable(pin))
        return true;
    semsg("%s(): GPIO %d does not exist or is in use by the system", fn, pin);
    return false;
}

/*
 * esp_serial_open({port}, {baud} [, {tx}, {rx}]): open UART{port} (not 0, the
 * console). Pins default to the board's (menuconfig "esp-vim board").
 */
void f_esp_serial_open(typval_T *argvars, typval_T *rettv)
{
    int port = (int)tv_get_number(&argvars[0]);
    int baud = (int)tv_get_number(&argvars[1]);
    int tx = argvars[2].v_type != VAR_UNKNOWN ? (int)tv_get_number(&argvars[2]) : CONFIG_ESP_VIM_SERIAL_TX;
    int rx = argvars[2].v_type != VAR_UNKNOWN && argvars[3].v_type != VAR_UNKNOWN
           ? (int)tv_get_number(&argvars[3]) : CONFIG_ESP_VIM_SERIAL_RX;
    if (!serial_port_ok(port) || !pin_ok(tx, "esp_serial_open") || !pin_ok(rx, "esp_serial_open"))
        return;
    if (baud < 300 || baud > 5000000) {
        semsg("esp_serial_open(): bad baud rate %d", baud);
        return;
    }
    /* A previous Vim session may have left it open: the driver outlives it. */
    if (uart_is_driver_installed(port))
        uart_driver_delete(port);
    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e = uart_driver_install(port, 4096, 0, 0, NULL, 0);
    if (e == ESP_OK)
        e = uart_param_config(port, &cfg);
    if (e == ESP_OK)
        e = uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) {
        uart_driver_delete(port);
        semsg("esp_serial_open(UART%d): %s", port, esp_err_to_name(e));
        return;
    }
    rettv->vval.v_number = TRUE;
}

/* esp_serial_read({port}): whatever has arrived, without waiting ("" if none). */
void f_esp_serial_read(typval_T *argvars, typval_T *rettv)
{
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = NULL;
    int port = (int)tv_get_number(&argvars[0]);
    if (!serial_port_ok(port))
        return;
    if (!uart_is_driver_installed(port)) {
        semsg("esp_serial_read(): UART%d is not open", port);
        return;
    }
    size_t avail = 0;
    uart_get_buffered_data_len(port, &avail);
    if (avail > 8192)
        avail = 8192;
    char_u *buf = alloc(avail + 1);
    if (buf == NULL)
        return;
    int n = avail ? uart_read_bytes(port, buf, avail, 0) : 0;
    buf[n > 0 ? n : 0] = NUL;
    rettv->vval.v_string = buf;
}

/* esp_serial_write({port}, {text}): number of bytes queued. */
void f_esp_serial_write(typval_T *argvars, typval_T *rettv)
{
    int port = (int)tv_get_number(&argvars[0]);
    char_u *s = tv_get_string_chk(&argvars[1]);
    if (s == NULL || !serial_port_ok(port))
        return;
    if (!uart_is_driver_installed(port)) {
        semsg("esp_serial_write(): UART%d is not open", port);
        return;
    }
    rettv->vval.v_number = uart_write_bytes(port, s, STRLEN(s));
}

void f_esp_serial_close(typval_T *argvars, typval_T *rettv UNUSED)
{
    int port = (int)tv_get_number(&argvars[0]);
    if (serial_port_ok(port) && uart_is_driver_installed(port))
        uart_driver_delete(port);
}

/* ----------------------------------------------------------------- i2c -- */

typedef bool (*i2c_found_cb)(void *ctx, i2c_master_bus_handle_t bus, int addr);

/* Probe every 7-bit address on a bus built on {sda}/{scl} for the call. */
static bool i2c_scan(int sda, int scl, i2c_found_cb cb, void *ctx, const char *fn)
{
    /* The touch panel's bus (the board's own, on the ES3C28P) is shared, not
     * claimed a second time; its pins are otherwise reserved. */
    i2c_master_bus_handle_t shared = esp_touch_i2c_bus(sda, scl);
    if (shared != NULL) {
        for (int a = 0x08; a < 0x78; a++)
            if (i2c_master_probe(shared, a, 20) == ESP_OK && !cb(ctx, shared, a))
                break;
        return true;
    }
    if (!pin_ok(sda, fn) || !pin_ok(scl, fn))
        return false;
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,                         /* any free controller */
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t e = i2c_new_master_bus(&cfg, &bus);
    if (e != ESP_OK) {
        semsg("%s(): cannot use SDA %d / SCL %d: %s", fn, sda, scl, esp_err_to_name(e));
        return false;
    }
    for (int a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK && !cb(ctx, bus, a))
            break;
        if (a % 16 == 0) {
            ui_breakcheck();
            if (got_int)
                break;
        }
    }
    i2c_del_master_bus(bus);
    return true;
}

static void pins_arg(typval_T *argvars, int *sda, int *scl)
{
    *sda = argvars[0].v_type != VAR_UNKNOWN ? (int)tv_get_number(&argvars[0]) : CONFIG_ESP_VIM_I2C_SDA;
    *scl = argvars[0].v_type != VAR_UNKNOWN && argvars[1].v_type != VAR_UNKNOWN
         ? (int)tv_get_number(&argvars[1]) : CONFIG_ESP_VIM_I2C_SCL;
}

static bool add_addr(void *ctx, i2c_master_bus_handle_t bus UNUSED, int addr)
{
    return list_append_number((list_T *)ctx, addr) == OK;
}

/* esp_i2c_scan([{sda}, {scl}]) -> List of the addresses that answered. */
void f_esp_i2c_scan(typval_T *argvars, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    int sda, scl;
    pins_arg(argvars, &sda, &scl);
    i2c_scan(sda, scl, add_addr, rettv->vval.v_list, "esp_i2c_scan");
}

/*
 * Devices :EspSensors recognises: by address, and where the chip has an ID
 * register, by that too, so a different part at the same address is not
 * misnamed.
 */
static const struct {
    uint8_t addr, id_reg, id;           /* id_reg 0xff: identify by address alone */
    const char *name;
} KNOWN[] = {
    { 0x68, 0x00, 0x24, "BMI270 IMU (accelerometer, gyroscope)" },
    { 0x69, 0x00, 0x24, "BMI270 IMU (accelerometer, gyroscope)" },
    { 0x76, 0xd0, 0x60, "BME280 (temperature, humidity, pressure)" },
    { 0x77, 0xd0, 0x60, "BME280 (temperature, humidity, pressure)" },
    { 0x76, 0xd0, 0x58, "BMP280 (temperature, pressure)" },
    { 0x77, 0xd0, 0x58, "BMP280 (temperature, pressure)" },
    { 0x38, 0xa3, 0x64, "FT6336 touch controller" },
    { 0x5d, 0xff, 0x00, "GT911 touch controller" },
    { 0x14, 0xff, 0x00, "GT911 touch controller" },
    { 0x18, 0xfd, 0x83, "ES8311 audio codec" },
    { 0x43, 0xff, 0x00, "PI4IOE5V6408 IO expander" },
    { 0x44, 0xff, 0x00, "PI4IOE5V6408 IO expander" },
    { 0x6d, 0xff, 0x00, "M5Stack Tab5 keyboard" },
    { 0x40, 0xff, 0x00, "INA226 current / power monitor" },
    { 0x51, 0xff, 0x00, "RTC (PCF8563 / RX8130)" },
};

static bool add_sensor(void *ctx, i2c_master_bus_handle_t bus, int addr)
{
    const char *name = NULL;
    for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0] && name == NULL; i++) {
        if (KNOWN[i].addr != addr)
            continue;
        if (KNOWN[i].id_reg == 0xff) {
            name = KNOWN[i].name;
            continue;
        }
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                   .device_address = addr, .scl_speed_hz = 100000 };
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(bus, &dc, &dev) == ESP_OK) {
            uint8_t reg = KNOWN[i].id_reg, id = 0;
            if (i2c_master_transmit_receive(dev, &reg, 1, &id, 1, 50) == ESP_OK && id == KNOWN[i].id)
                name = KNOWN[i].name;
            i2c_master_bus_rm_device(dev);
        }
    }
    dict_T *d = dict_alloc();
    if (d == NULL)
        return false;
    dict_add_number(d, "addr", addr);
    dict_add_string(d, "name", (char_u *)(name ? name : "unknown"));
    return list_append_dict((list_T *)ctx, d) == OK;
}

/* esp_sensors([{sda}, {scl}]) -> List of Dicts: addr, name. */
void f_esp_sensors(typval_T *argvars, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    int sda, scl;
    pins_arg(argvars, &sda, &scl);
    i2c_scan(sda, scl, add_sensor, rettv->vval.v_list, "esp_sensors");
}

/* ----------------------------------------------------------------- adc -- */

/* esp_adc_read({pin}) -> Dict: raw, mv (calibrated; -1 if no calibration),
 * unit, channel. */
void f_esp_adc_read(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    int pin = (int)tv_get_number(&argvars[0]);
    adc_unit_t unit;
    adc_channel_t ch;
    if (adc_oneshot_io_to_channel(pin, &unit, &ch) != ESP_OK) {
        semsg("esp_adc_read(): GPIO %d is not an ADC pin on the %s", pin, ESP_VIM_CHIP);
        return;
    }
    if (!pin_ok(pin, "esp_adc_read"))
        return;
    adc_oneshot_unit_handle_t h;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    esp_err_t e = adc_oneshot_new_unit(&ucfg, &h);
    if (e != ESP_OK) {
        semsg("esp_adc_read(): ADC%d: %s", unit + 1, esp_err_to_name(e));
        return;
    }
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    int raw = 0, mv = -1;
    e = adc_oneshot_config_channel(h, ch, &ccfg);
    if (e == ESP_OK)
        e = adc_oneshot_read(h, ch, &raw);
    if (e == ESP_OK) {
        adc_cali_handle_t cali;
        adc_cali_curve_fitting_config_t cc = { .unit_id = unit, .chan = ch,
                                               .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
        if (adc_cali_create_scheme_curve_fitting(&cc, &cali) == ESP_OK) {
            if (adc_cali_raw_to_voltage(cali, raw, &mv) != ESP_OK)
                mv = -1;
            adc_cali_delete_scheme_curve_fitting(cali);
        }
    }
    adc_oneshot_del_unit(h);
    if (e != ESP_OK) {
        semsg("esp_adc_read(GPIO %d): %s", pin, esp_err_to_name(e));
        return;
    }
    dict_T *d = rettv->vval.v_dict;
    dict_add_number(d, "raw", raw);
    dict_add_number(d, "mv", mv);
    dict_add_number(d, "unit", unit + 1);
    dict_add_number(d, "channel", ch);
}
