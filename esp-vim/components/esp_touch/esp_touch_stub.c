/* esp_touch in a build without a touch panel (CONFIG_ESP_VIM_TOUCH off). */

#include "esp_touch.h"

esp_err_t esp_touch_init(void) { return ESP_ERR_NOT_SUPPORTED; }
bool esp_touch_available(void) { return false; }
void esp_touch_set_handler(esp_touch_handler_t handler, void *ctx) { (void)handler; (void)ctx; }
i2c_master_bus_handle_t esp_touch_i2c_bus(int sda, int scl) { (void)sda; (void)scl; return NULL; }
