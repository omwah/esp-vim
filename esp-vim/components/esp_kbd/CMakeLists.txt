# Keyboards as console input (include/esp_kbd.h). Outside the vim component:
# the queue outlives Vim sessions, and keyboard drivers feed it from their own
# tasks. esp_kbd_tab5_dev.c is the Tab5 keyboard accessory's driver, empty
# unless ESP_VIM_KBD_TAB5 (requirements can't depend on the configuration);
# esp_kbd_cardputer_dev.c the M5Stack Cardputer ADV's, unless ESP_VIM_KBD_CARDPUTER;
# esp_kbd_tdeck_dev.c the LilyGo T-Deck's, unless ESP_VIM_KBD_TDECK (it shares the
# touch panel's I2C bus, through esp_touch).
idf_component_register(SRCS "esp_kbd.c" "esp_kbd_hid.c" "esp_kbd_tab5.c" "esp_kbd_tab5_dev.c"
                            "esp_kbd_cardputer.c" "esp_kbd_cardputer_dev.c"
                            "esp_kbd_tdeck.c" "esp_kbd_tdeck_dev.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_timer heap esp_driver_i2c esp_driver_gpio esp_hw_support log esp_touch nvs_flash)
