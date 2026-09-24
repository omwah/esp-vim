/*
 * Entry point for Vim on ESP32-P4.
 *
 * Vim's own main() is renamed to vim_main() by -Dmain=vim_main, so this file
 * owns startup: bring up the console and filesystems the way Phase 1 proved
 * they must be brought up, then hand control to Vim and never return.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Vim's renamed entry point. Declared here to avoid pulling in vim.h. */
int vim_main(int argc, char **argv);

static const char *TAG = "esp-vim";

/*
 * Vim recurses in the regexp engine and in eval, and Xtensa's windowed ABI
 * (the S3 variant) costs more stack per frame than RISC-V. Kconfig-tunable
 * rather than a constant -- see docs/PLAN.md Phase 3.
 */
#ifndef ESP_VIM_TASK_STACK
# define ESP_VIM_TASK_STACK (64 * 1024)
#endif

/*
 * Phase 1 finding: tcgetattr/tcsetattr fail with EINVAL until the UART driver
 * is installed and the VFS is routed through it. isatty(0) is true either way,
 * but Vim calls termios during startup, so the driver must come first.
 */
static void console_init(void)
{
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 4096, 4096, 0, NULL, 0));
    uart_vfs_dev_use_driver(UART_NUM_0);
    uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
    uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
}

static void storage_init(void)
{
    static wl_handle_t wl = WL_INVALID_HANDLE;
    esp_vfs_fat_mount_config_t rw = {
        .max_files = 12,
        .format_if_mount_failed = true,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl("/fat", "storage", &rw, &wl);
    if (e != ESP_OK)
        ESP_LOGE(TAG, "/fat mount failed: %s", esp_err_to_name(e));

    esp_vfs_fat_mount_config_t ro = { .max_files = 12 };
    e = esp_vfs_fat_rawflash_mount("/vimrt", "vimrt", &ro);
    if (e != ESP_OK)
        ESP_LOGE(TAG, "/vimrt mount failed: %s -- $VIMRUNTIME will be missing",
                 esp_err_to_name(e));
}

/*
 * Vim reads all of these. There is no shell and no passwd database to get them
 * from, and with no TIOCGWINSZ and no SIGWINCH, LINES/COLUMNS are how
 * mch_get_shellsize() learns the geometry at all.
 */
static void environment_init(void)
{
    setenv("HOME", "/fat", 1);
    setenv("TERM", "xterm", 1);      /* resolves to builtin_xterm, no termcap file */
    setenv("SHELL", "", 1);          /* no shell exists; keep Vim from guessing */
    setenv("VIM", "/vimrt", 1);
    setenv("VIMRUNTIME", "/vimrt", 1);
    setenv("LINES", "24", 1);
    setenv("COLUMNS", "80", 1);
}

static void vim_task(void *arg)
{
    (void)arg;
    char *argv[] = { "vim", NULL };

    printf("\nESPVIM-READY\n");     /* stable marker for esp-emu --inject-on */
    int rc = vim_main(1, argv);
    printf("\nESPVIM-EXIT rc=%d\n", rc);

    /* mch_exit() must not call exit(): on IDF that restarts the chip. */
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);

    console_init();
    storage_init();
    environment_init();

    xTaskCreate(vim_task, "vim", ESP_VIM_TASK_STACK / sizeof(StackType_t) * sizeof(StackType_t),
                NULL, 5, NULL);
}
