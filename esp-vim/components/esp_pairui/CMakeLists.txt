# The Bluetooth keyboard pairing overlay (include/esp_pairui.h), for boards with
# a display and a touch panel. Requirements don't depend on the configuration:
# ESP-IDF works them out before it is known.
idf_component_register(SRCS "esp_pairui.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_ble esp_display esp_touch esp_timer freertos log)
