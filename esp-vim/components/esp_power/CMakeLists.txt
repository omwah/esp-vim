# The battery, and sleep (include/esp_power.h). Its own component: it outlives
# Vim sessions, and puts the radios and the display to sleep around the chip.
idf_component_register(SRCS "esp_power.c" INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_adc esp_driver_gpio esp_driver_usb_serial_jtag esp_hw_support esp_timer
                  nvs_flash log heap esp_display esp_net esp_ble esp_touch esp_driver_i2c)
