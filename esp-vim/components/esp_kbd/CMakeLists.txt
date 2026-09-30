# Keyboards as console input (include/esp_kbd.h). Outside the vim component:
# the queue outlives Vim sessions, and keyboard drivers feed it from their own
# tasks. esp_kbd_tab5_dev.c is the Tab5 keyboard accessory's driver, empty
# unless ESP_VIM_KBD_TAB5 (requirements can't depend on the configuration).
idf_component_register(SRCS "esp_kbd.c" "esp_kbd_hid.c" "esp_kbd_tab5.c" "esp_kbd_tab5_dev.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_timer heap esp_driver_i2c esp_driver_gpio esp_hw_support log)
