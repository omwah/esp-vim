# The date and time (include/esp_time.h): an RTC chip, NTP and the time zone.
# Its own component: the time outlives Vim sessions, and the web server's task
# sets it too.
idf_component_register(SRCS "esp_time.c" INCLUDE_DIRS "include"
    REQUIRES esp_netif
    PRIV_REQUIRES esp_driver_i2c nvs_flash log lwip esp_touch)
