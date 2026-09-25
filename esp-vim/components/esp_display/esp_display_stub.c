/* esp_display in a build without a display (CONFIG_ESP_VIM_DISPLAY off). */

#include "esp_display.h"

esp_err_t esp_display_init(void) { return ESP_ERR_NOT_SUPPORTED; }
bool esp_display_active(void) { return false; }
void esp_display_size(int *rows, int *cols) { *rows = *cols = 0; }
void esp_display_write(const void *buf, size_t len) { (void)buf; (void)len; }
void esp_display_touch(int ev, int x, int y, void *ctx) { (void)ev; (void)x; (void)y; (void)ctx; }
