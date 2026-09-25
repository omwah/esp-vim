idf_component_register(SRCS "esp_vim_main.c"
                       REQUIRES vim fatfs vfs esp_driver_uart nvs_flash esp_fs esp_net)
