# SFTP/SCP over libssh2 (include/esp_ssh.h). Outside the vim component: the
# SSH session is cached across calls and outlives Vim sessions, and this has
# nothing Vim-specific in it.
idf_component_register(
    SRCS "esp_ssh.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_fs mbedtls esp_hw_support heap freertos lwip
    PRIV_REQUIRES skuodi__libssh2_esp
)
