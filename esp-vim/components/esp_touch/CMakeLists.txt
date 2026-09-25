# A capacitive touch panel (include/esp_touch.h). Outside the vim component: it
# outlives Vim sessions, and runs in its own task.
#
# Requirements are the same either way: ESP-IDF works them out before the
# configuration is known, so only the sources may depend on it.
if(CONFIG_ESP_VIM_TOUCH)
    set(srcs "esp_touch.c")
else()
    set(srcs "esp_touch_stub.c")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES esp_driver_gpio esp_hw_support log freertos)
