# The board's own hardware, outside any one driver (include/esp_board.h): power
# switches, the shared I2C bus, the panel. On the Tab5 through Espressif's board
# support package (idf_component.yml), which is only fetched for that build --
# the component manager adds it to the requirements then. Requirements can't
# depend on the configuration otherwise (ESP-IDF works them out first).
idf_component_register(SRCS "esp_board.c" INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c esp_lcd
                       PRIV_REQUIRES esp_driver_gpio esp_hw_support log freertos)
