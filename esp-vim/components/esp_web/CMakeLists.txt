# The web interface (include/esp_web.h). Its own component: the HTTP server is
# its own task and must never reach into Vim; it talks to the file system
# through esp_fs and to Vim only through the status and settings exchanges.
idf_component_register(
    SRCS "esp_web.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "web/index.html" "web/app.js"
    REQUIRES esp_https_server esp_http_server mbedtls nvs_flash json esp_fs esp_timer esp_hw_support
)
