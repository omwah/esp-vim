idf_component_register(SRCS "esp_vim_main.c"
                       REQUIRES vim fatfs vfs esp_driver_uart esp_driver_usb_serial_jtag nvs_flash esp_fs esp_net esp_ssh esp_web)
