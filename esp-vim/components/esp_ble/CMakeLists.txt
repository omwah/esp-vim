# Bluetooth LE (include/esp_ble.h). Outside the vim component for the same
# reasons as esp_net: the stack outlives Vim sessions, and Phase 11's keyboard
# support will use it from its own task.
idf_component_register(
    SRCS "esp_ble.c" "esp_ble_kbd.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bt log esp_hid esp_kbd nvs_flash esp_hw_support
)
