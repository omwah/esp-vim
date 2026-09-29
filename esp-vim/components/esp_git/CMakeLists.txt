# Git on the device, over libgit2 (docs/PLAN.md, Phase 8): its set-up,
# credentials, host keys and gc. The esp_git_*() builtins are in the Vim
# component (api/esp_api_git.c).
idf_component_register(SRCS "esp_git.c" INCLUDE_DIRS "include"
    REQUIRES libgit2
    PRIV_REQUIRES esp_ssh nvs_flash skuodi__libssh2_esp)
