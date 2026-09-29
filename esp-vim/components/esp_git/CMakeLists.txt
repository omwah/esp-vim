# Git on the device. For now only the libgit2 feasibility self-test (stage 6z,
# docs/PLAN.md). Nothing refers to libgit2 unless it is enabled, so it is only
# linked then. (Requirements can't depend on Kconfig; sources can.)
set(srcs)
if(CONFIG_ESP_VIM_GIT_SELFTEST)
    list(APPEND srcs "esp_git_selftest.c")
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "include"
    PRIV_REQUIRES libgit2 esp_fs freertos heap esp_timer)
if(srcs)
    # libgit2's *_OPTIONS_INIT macros fill only the leading fields.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
endif()
