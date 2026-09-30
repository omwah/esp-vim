# USB keyboards (include/esp_usbkbd.h), through the USB HID host class driver
# (idf_component.yml), which is only fetched when ESP_VIM_USB_KBD is set -- the
# component manager adds it to the requirements then.
idf_component_register(SRCS "esp_usbkbd.c" INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_kbd log freertos)
