# The microSD card at /sd (include/esp_sd.h). Outside the vim component: the
# mount outlives Vim sessions. Requirements can't depend on the configuration
# (ESP-IDF works them out first), so the SD drivers are always required.
idf_component_register(SRCS "esp_sd.c" INCLUDE_DIRS "include"
                       PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_sdspi esp_driver_spi freertos)
