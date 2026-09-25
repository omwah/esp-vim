# The network interface and HTTP(S) client (include/esp_net.h). Outside the vim
# component for the same reasons as esp_fs: it outlives Vim sessions, and the
# web server will use it from its own task.
idf_component_register(
    SRCS "esp_net.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_event esp_http_client esp_eth mbedtls esp_fs heap
)
