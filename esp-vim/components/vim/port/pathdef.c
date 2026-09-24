/*
 * auto/pathdef.c replacement for the ESP-IDF build.
 *
 * Upstream generates this at build time from the Makefile's variables. Here the
 * values are fixed by the firmware image, so it is written by hand.
 *
 * The two that matter are the runtime directories: they must name the read-only
 * partition the curated $VIMRUNTIME is flashed to (docs/PLAN.md Phase 4), or
 * FEAT_NORMAL will fail during startup when it tries to source defaults.vim.
 */

#include "vim.h"

char_u *default_vim_dir        = (char_u *)"/vimrt";
char_u *default_vimruntime_dir = (char_u *)"/vimrt";
#include "sdkconfig.h"
#include "esp_idf_version.h"

#if CONFIG_IDF_TARGET_ARCH_XTENSA
# define ESP_VIM_CC "xtensa-" CONFIG_IDF_TARGET "-elf-gcc"
#else
# define ESP_VIM_CC "riscv32-esp-elf-gcc"
#endif
#define ESP_VIM_STR_(x) #x
#define ESP_VIM_STR(x) ESP_VIM_STR_(x)

char_u *all_cflags             = (char_u *)ESP_VIM_CC " " __VERSION__ " (ESP-IDF v"
        ESP_VIM_STR(ESP_IDF_VERSION_MAJOR) "." ESP_VIM_STR(ESP_IDF_VERSION_MINOR) "."
        ESP_VIM_STR(ESP_IDF_VERSION_PATCH) ")";
char_u *all_lflags             = (char_u *)"esp-idf component link";
char_u *compiled_user          = (char_u *)"esp";
char_u *compiled_sys           = (char_u *)ESP_VIM_CHIP;   /* from the component CMakeLists */
