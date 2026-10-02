idf_component_register(SRCS "esp_vim_main.c"
                       REQUIRES vim esp_board fatfs vfs esp_driver_uart esp_driver_usb_serial_jtag nvs_flash esp_fs esp_net esp_ssh esp_web esp_display esp_ble esp_kbd esp_usbkbd esp_touch esp_pairui esp_power esp_time esp_sd esp_logbuf)
