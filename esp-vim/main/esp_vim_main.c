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
#include <unistd.h>
#include <sys/select.h>
#include "esp_timer.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_vim_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Vim's renamed entry point. Declared here to avoid pulling in vim.h. */
int vim_main(int argc, char **argv);

static const char *TAG = "esp-vim";

/* Exposed so a debugger can find the Vim task's saved context (see docs). */
TaskHandle_t g_vim_task;

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
/* Non-blocking "has a key arrived?" for the UART console (see esp_vim_port.h). */
static bool uart_console_pending(void)
{
    size_t n = 0;
    return uart_get_buffered_data_len(UART_NUM_0, &n) == ESP_OK && n > 0;
}

static void console_init(void)
{
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 4096, 4096, 0, NULL, 0));
    uart_vfs_dev_use_driver(UART_NUM_0);
    uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
    uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
}

static void storage_init(void)
{
    /* NVS: :EspNvs, and later WiFi credentials, keys and settings. A full or
     * newer-format partition is erased, ESP-IDF's documented recovery. */
    esp_err_t n = nvs_flash_init();
    if (n == ESP_ERR_NVS_NO_FREE_PAGES || n == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs: %s; erasing the partition", esp_err_to_name(n));
        if (nvs_flash_erase() == ESP_OK)
            n = nvs_flash_init();
    }
    if (n != ESP_OK)
        ESP_LOGE(TAG, "nvs init failed: %s -- :EspNvs will not work", esp_err_to_name(n));

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
    /*
     * With $PATH unset, Vim's mch_can_exe() returns -1 ("cannot tell"), which Vim
     * script treats as TRUE -- so every plugin probing executable('shellcheck')
     * and the like concluded the tool was installed and tried to use it. A real
     * but empty directory makes executable() answer an honest 0.
     */
    setenv("PATH", "/fat/bin", 1);
    setenv("VIM", "/vimrt", 1);
    setenv("VIMRUNTIME", "/vimrt", 1);
    setenv("LINES", "24", 1);
    setenv("COLUMNS", "80", 1);
}

/*
 * Read back a file Vim wrote on a previous run and print it.
 *
 * This is the other half of the emulator round-trip gate (docs/PLAN.md Phase 5):
 * run one injects an edit and saves, run two -- against the --save-state image --
 * proves the bytes survived a reboot. Cheap enough to leave in.
 */
static void report_test_artifact(void)
{
    FILE *f = fopen("/fat/vimtest.txt", "r");
    if (f == NULL)
        return;

    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    printf("ESPVIM-ARTIFACT<<%s>>\n", buf);
}

/*
 * Learn the terminal's size, since a UART has no TIOCGWINSZ.
 *
 * Park the cursor at an impossible position and ask where it ended up (CSI 6n,
 * a cursor position report): a real terminal clamps to its last row and column
 * and answers "ESC [ rows ; cols R". Save/restore the cursor around it so the
 * screen is left as found. No answer within the timeout -- a scripted session,
 * a log capture, a dumb serial monitor -- keeps the 24x80 default.
 */
static void probe_terminal_size(void)
{
    static const char query[] = "\0337\033[999;999H\033[6n\0338";
    char buf[32];
    size_t n = 0;
    int rows = 0, cols = 0;

    write(STDOUT_FILENO, query, sizeof(query) - 1);

    int64_t deadline = esp_timer_get_time() + 300 * 1000;
    while (n < sizeof(buf) - 1) {
        int64_t left = deadline - esp_timer_get_time();
        if (left <= 0)
            break;
        fd_set r;
        FD_ZERO(&r);
        FD_SET(STDIN_FILENO, &r);
        struct timeval tv = { .tv_sec = 0, .tv_usec = (suseconds_t)left };
        if (select(STDIN_FILENO + 1, &r, NULL, NULL, &tv) <= 0)
            break;
        if (read(STDIN_FILENO, buf + n, 1) != 1)
            break;
        if (buf[n++] == 'R')
            break;
    }
    buf[n] = '\0';

    const char *csi = strstr(buf, "\033[");
    if (csi != NULL && sscanf(csi, "\033[%d;%dR", &rows, &cols) == 2
            && rows >= 5 && rows < 1000 && cols >= 20 && cols < 1000) {
        char v[8];
        snprintf(v, sizeof(v), "%d", rows);
        setenv("LINES", v, 1);
        snprintf(v, sizeof(v), "%d", cols);
        setenv("COLUMNS", v, 1);
        printf("ESPVIM-TERM %dx%d (probed)\n", rows, cols);
    } else {
        printf("ESPVIM-TERM %sx%s (default: no answer from terminal)\n",
               getenv("LINES"), getenv("COLUMNS"));
    }
}

/*
 * Vim's heap budget: half the free PSRAM, at most 16 MB -- 16 MB on the Tab5,
 * about 4 MB on an 8 MB ESP32-S3. Vim measured under 3 MB with large files open
 * (docs/PHASE4.md); the cap keeps it from starving the services of later phases.
 */
#define ESP_VIM_HEAP_MAX (16u * 1024 * 1024)

static esp_vim_session_t s_session;     /* survives session resets: not in Vim */
static TaskHandle_t s_supervisor;       /* app_main's task, which runs sessions */

/*
 * Strong override of the port's weak default: Vim has exited (after printing
 * ESPVIM-EXIT). Tell the supervisor, then end this task.
 *
 * Each session runs in its own FreeRTOS task, and a session ends by deleting
 * its task -- nothing unwinds Vim's call stack. The first version longjmp'd
 * back out of Vim instead; on the ESP32-S3 the next session then corrupted its
 * heap, while the RISC-V P4 was fine. Xtensa's windowed register ABI makes a
 * longjmp out of a deep call chain delicate; a fresh task with a fresh stack
 * sidesteps it on every architecture.
 */
void esp_vim_session_exit(int status)
{
    (void)status;
    xTaskNotifyGive(s_supervisor);
    vTaskDelete(NULL);
    for (;;)
        vTaskDelay(portMAX_DELAY);  /* not reached */
}

/* Between sessions: say what happened, wait for a key. */
static void between_sessions(void)
{
    static const char msg[] =
        "\r\n"
        "  All buffers closed.  Vim is forever.\r\n"
        "  Press any key to start a new session.\r\n";

    /* Discard keys typed while quitting, so a stray one cannot skip this --
     * BEFORE showing the prompt, or a key pressed in answer to it could be
     * flushed away and the wait would never end. */
    uart_flush_input(UART_NUM_0);
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);

    fd_set r;
    FD_ZERO(&r);
    FD_SET(STDIN_FILENO, &r);
    if (select(STDIN_FILENO + 1, &r, NULL, NULL, NULL) > 0) {
        char c;
        read(STDIN_FILENO, &c, 1);
    }
}

/* One Vim session: a fresh task with a fresh stack every time. */
static void vim_task(void *arg)
{
    unsigned session = (unsigned)(uintptr_t)arg;
    char *argv[] = { "vim", NULL };

    /* Power-on state for Vim, and an empty heap. Must run on THIS task:
     * what a session opens is owned by the task that begins it. */
    esp_vim_session_begin(&s_session);
    /* The poll registry lives in the port's .bss, just reset. */
    esp_vim_register_input_poll(0, uart_console_pending);

    probe_terminal_size();                         /* the window may have changed */
    if (session == 1)
        report_test_artifact();

    size_t used, peak, total;
    esp_vim_heap_stats(&used, &peak, &total);
    printf("ESPVIM-HEAP session=%u int_free=%u vim_heap_size=%u vim_heap_used=%u\n",
           session, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)total, (unsigned)used);
    printf("\nESPVIM-READY\n");                    /* stable marker for esp-emu --inject-on */

    vim_main(1, argv);

    /* Vim's main() never returns -- it exits via getout() -- but be safe. */
    printf("\nESPVIM-EXIT rc=0 ESPVIM-END\n");
    esp_vim_session_exit(0);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);

    /* Before anything else touches Vim: this snapshots Vim's .data while it
     * still holds its initial values. */
    size_t budget = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 2;
    if (budget > ESP_VIM_HEAP_MAX)
        budget = ESP_VIM_HEAP_MAX;
    ESP_ERROR_CHECK(esp_vim_session_init(&s_session, budget));

    console_init();
    storage_init();
    environment_init();

    /*
     * app_main's task becomes the session supervisor: start a Vim session,
     * wait for it to end, show the between-sessions prompt, repeat.
     *
     * Sessions are pinned to core 0, the core that installed the UART driver and
     * owns its ISR. Left unpinned, a Vim task on core 1 crashed inside ESP-IDF's
     * select() ("assert failed: spinlock_acquire (lock)"). Whether that is
     * ESP-IDF's cross-core select path or the emulator's multi-hart model is NOT
     * yet established -- re-test unpinned on real silicon (docs/PLAN.md Phase 9).
     */
    s_supervisor = xTaskGetCurrentTaskHandle();
    for (unsigned session = 1;; session++) {
        if (xTaskCreatePinnedToCore(vim_task, "vim", ESP_VIM_TASK_STACK,
                                    (void *)(uintptr_t)session, 5, &g_vim_task, 0) != pdPASS) {
            ESP_LOGE(TAG, "cannot create the Vim task");
            esp_restart();
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* until Vim exits */
        vTaskDelay(1);                             /* let the idle task reclaim its stack */
        between_sessions();
    }
}
