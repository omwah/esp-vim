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
#include <sys/stat.h>
#include <sys/select.h>
#include "esp_timer.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "esp_board.h"
#include "esp_fs.h"
#include "esp_sd.h"
#include "esp_net.h"
#include "esp_ssh.h"
#include "esp_web.h"
#include "esp_display.h"
#include "esp_ble.h"
#include "esp_kbd.h"
#include "esp_logbuf.h"
#include "esp_usbkbd.h"
#include "esp_touch.h"
#include "esp_pairui.h"
#include "esp_power.h"
#include "esp_time.h"
#include "esp_heap_caps.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_select.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_private/periph_ctrl.h"
#if CONFIG_ESP_VIM_LAUNCHER_APP             /* usb_console_take_back(): ESP32-S3 */
#include "soc/rtc_cntl_struct.h"
#endif
#include "esp_rom_sys.h"
#endif
#include "esp_vim_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Vim's renamed entry point. Declared here to avoid pulling in vim.h. */
int vim_main(int argc, char **argv);

#define ESP_VIM_STR_(x) #x
#define ESP_VIM_STR(x)  ESP_VIM_STR_(x)

static const char *TAG = "esp-vim";

/* Exposed so a debugger can find the Vim task's saved context (see docs). */
TaskHandle_t g_vim_task;

/*
 * Vim recurses in the regexp engine and in eval, and Xtensa's windowed ABI
 * (the S3 variant) costs more stack per frame than RISC-V. Kconfig-tunable
 * rather than a constant -- see docs/PLAN.md Phase 3.
 */
#define ESP_VIM_TASK_STACK (CONFIG_ESP_VIM_TASK_STACK_KB * 1024)

/*
 * Phase 1 finding: tcgetattr/tcsetattr fail with EINVAL until the UART driver
 * is installed and the VFS is routed through it. isatty(0) is true either way,
 * but Vim calls termios during startup, so the driver must come first.
 */
/*
 * The console: UART0, or on boards whose only USB port is the chip's own
 * (the Hosyond ES3C28P), the built-in USB Serial/JTAG port. Chosen by ESP-IDF's
 * console setting, so boot messages and Vim share it.
 */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

/* Non-blocking "has a key arrived?" (see esp_vim_port.h). */
static bool console_pending(void)
{
    return usb_serial_jtag_get_read_bytes_available() > 0;
}

static void console_flush_input(void)
{
    char buf[64];
    while (usb_serial_jtag_read_bytes(buf, sizeof buf, 0) > 0)
        ;
}

/* With nobody reading on the host, the driver drops output after a short
 * timeout rather than blocking, so the device boots with no terminal open. */
static void console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 4096, .rx_buffer_size = 4096 };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
}

#else

/* Non-blocking "has a key arrived?" (see esp_vim_port.h). */
static bool console_pending(void)
{
    size_t n = 0;
    return uart_get_buffered_data_len(UART_NUM_0, &n) == ESP_OK && n > 0;
}

static void console_flush_input(void)
{
    uart_flush_input(UART_NUM_0);
}

static void console_init(void)
{
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 4096, 4096, 0, NULL, 0));
    uart_vfs_dev_use_driver(UART_NUM_0);
    uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
    uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
}

#endif

/* NVS: :EspNvs, WiFi credentials, keys and settings. A full or newer-format
 * partition is erased, ESP-IDF's documented recovery -- unless it is a
 * launcher's (ESP_VIM_LAUNCHER_APP), whose settings are not ours to lose. */
#if CONFIG_ESP_VIM_LAUNCHER_APP
# define NVS_ERASABLE false
#else
# define NVS_ERASABLE true
#endif
static void nvs_init(void)
{
    esp_err_t n = nvs_flash_init();
    if (NVS_ERASABLE && (n == ESP_ERR_NVS_NO_FREE_PAGES || n == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_LOGW(TAG, "nvs: %s; erasing the partition", esp_err_to_name(n));
        if (nvs_flash_erase() == ESP_OK)
            n = nvs_flash_init();
    }
    if (n != ESP_OK)
        ESP_LOGE(TAG, "nvs init failed: %s -- :EspNvs will not work", esp_err_to_name(n));
}

static void storage_init(void)
{

    ESP_ERROR_CHECK(esp_fs_init());     /* before any task can use it */

#if CONFIG_ESP_VIM_FLASH_FS
    static wl_handle_t wl = WL_INVALID_HANDLE;
    esp_vfs_fat_mount_config_t rw = {
        .max_files = 12,
        .format_if_mount_failed = true,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl("/fat", "storage", &rw, &wl);
    if (e != ESP_OK)
        ESP_LOGE(TAG, "/fat mount failed: %s", esp_err_to_name(e));
#endif

    esp_sd_init();                      /* /sd, when the board has a slot and a card is in */

#if CONFIG_ESP_VIM_FLASH_RUNTIME
    esp_vfs_fat_mount_config_t ro = { .max_files = 12 };
    esp_err_t r = esp_vfs_fat_rawflash_mount("/vimrt", "vimrt", &ro);
    if (r != ESP_OK)
        ESP_LOGE(TAG, "/vimrt mount failed: %s -- $VIMRUNTIME will be missing",
                 esp_err_to_name(r));
#endif
}

/*
 * Vim reads all of these. There is no shell and no passwd database to get them
 * from, and with no TIOCGWINSZ and no SIGWINCH, LINES/COLUMNS are how
 * mch_get_shellsize() learns the geometry at all.
 */
static void environment_init(void)
{
    const char *home = CONFIG_ESP_VIM_HOME_DIR;
    char path[96];

    setenv("HOME", home, 1);
    setenv("TERM", "xterm", 1);      /* resolves to builtin_xterm, no termcap file */
    setenv("SHELL", "", 1);          /* no shell exists; keep Vim from guessing */
    /*
     * With $PATH unset, Vim's mch_can_exe() returns -1 ("cannot tell"), which Vim
     * script treats as TRUE -- so every plugin probing executable('shellcheck')
     * and the like concluded the tool was installed and tried to use it. A real
     * but empty directory makes executable() answer an honest 0.
     */
    snprintf(path, sizeof path, "%s/bin", home);
    setenv("PATH", path, 1);
    /*
     * Temporary files: Vim tries $TMPDIR, then /tmp, "." and $HOME, and on this
     * filesystem layout /tmp does not exist and "." may be the read-only /vimrt.
     * netrw downloads into a temp file, so this matters. Emptied at every boot:
     * a session that ends by restarting never runs Vim's own cleanup.
     */
    esp_fs_err_t ferr;
    snprintf(path, sizeof path, "%s/.tmp", home);
    esp_fs_delete(path, NULL, NULL, &ferr);            /* fine if absent */
    mkdir(path, 0777);
    setenv("TMPDIR", path, 1);
    setenv("VIM", CONFIG_ESP_VIM_RUNTIME_DIR, 1);
    setenv("VIMRUNTIME", CONFIG_ESP_VIM_RUNTIME_DIR, 1);
#if !CONFIG_ESP_VIM_FLASH_RUNTIME
    /*
     * The runtime is on the SD card. Without it -- no card, or a card nobody
     * copied it to -- Vim would start in Vi-compatible mode with nothing to
     * say why. $VIMINIT stands in for the missing vimrc: the settings this
     * port can't do without (runtime-image/vimrc has the reasons), and a word
     * on what is wrong.
     */
    struct stat st;
    if (stat(CONFIG_ESP_VIM_RUNTIME_DIR "/vimrc", &st) != 0)
        setenv("VIMINIT",
               "set nocompatible noswapfile regexpengine=1 history=50 undolevels=100"
               " backspace=indent,eol,start ruler showmode"
               "|autocmd VimEnter * echomsg esp_sd().mounted"
               " ? 'No Vim runtime on the card: run pixi run sd-runtime'"
               " : 'No SD card: nothing can be saved'", 1);
#endif
    setenv("LINES", ESP_VIM_STR(CONFIG_ESP_VIM_TERM_ROWS), 1);
    setenv("COLUMNS", ESP_VIM_STR(CONFIG_ESP_VIM_TERM_COLS), 1);
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
 * a log capture, a dumb serial monitor -- keeps the default (24x80, or the
 * board's ESP_VIM_TERM_ROWS x _COLS).
 */
static void probe_terminal_size(void)
{
    if (esp_display_active()) {
        /* The screen is the display: Vim takes its size, whatever the
         * terminal on the serial console (which mirrors it) might be. */
        int rows, cols;
        char v[8];
        esp_display_size(&rows, &cols);
        snprintf(v, sizeof(v), "%d", rows);
        setenv("LINES", v, 1);
        snprintf(v, sizeof(v), "%d", cols);
        setenv("COLUMNS", v, 1);
        printf("ESPVIM-TERM %dx%d (display)\n", rows, cols);
        return;
    }

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
    esp_logbuf_capture(false);          /* the console is the log's again */
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
    char c;
    console_flush_input();
    while (esp_kbd_read(&c, 1) > 0)     /* and the keyboards' queue */
        ;
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);

    /* A key from the console or from a keyboard (components/esp_kbd, which
     * select() can't see): look at both, in short slices. */
    for (;;) {
        if (esp_kbd_read(&c, 1) > 0) {
            while (esp_kbd_read(&c, 1) > 0)
                ;                       /* the rest of an arrow's or F-key's sequence */
            return;
        }
        fd_set r;
        FD_ZERO(&r);
        FD_SET(STDIN_FILENO, &r);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50 * 1000 };
        if (select(STDIN_FILENO + 1, &r, NULL, NULL, &tv) > 0) {
            read(STDIN_FILENO, &c, 1);
            return;
        }
    }
}

/* One Vim session: a fresh task with a fresh stack every time. */
static void vim_task(void *arg)
{
    unsigned session = (unsigned)(uintptr_t)arg;
    char *argv[] = { "vim", NULL };

#if !CONFIG_SPIRAM
    /* Vim's heap is internal RAM, shared with everything else: its budget is
     * what the drivers and tasks have left, less a reserve for them. */
    size_t avail = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t reserve = CONFIG_ESP_VIM_HEAP_RESERVE_KB * 1024;
    s_session.heap_budget = avail > reserve + 1024 ? avail - reserve : 1024;
#endif

    /* Power-on state for Vim, and an empty heap. Must run on THIS task:
     * what a session opens is owned by the task that begins it. */
    esp_vim_session_begin(&s_session);
    /* The poll registry and the output mirror live in the port's .bss, just
     * reset. */
    esp_vim_register_input_poll(0, console_pending);
    esp_vim_set_extra_input(esp_kbd_pending, esp_kbd_read);   /* keyboards */
    if (esp_display_active())
        esp_vim_set_output_mirror(esp_display_write);
    esp_vim_console_output_load();          /* :EspConsole on/off, after the mirror */
    esp_logbuf_capture(true);               /* Vim has the console: log lines to :EspLog */

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

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && CONFIG_ESP_VIM_BOARD_TAB5
/*
 * The Tab5's RESET button resets the P4 (it reads as "watchdog") without its
 * USB pins ever showing a disconnect: the host keeps the old device, whose
 * state on our side is gone, and finds out only when something next uses the
 * port -- which then fails, disappears, and comes back some seconds later.
 * Unused, the console looked missing for 30 s and more (docs/PHASE9.md). So
 * after any reset but a power-on, which the host sees as a plug-in anyway, the
 * D+ pull-up is held off for 100 ms: a clean unplug, then the register as it
 * was, and the host enumerates the console again at once. What is printed
 * before then is lost, as it is with no terminal open.
 *
 * The register is put back whole: the override's values are written into the
 * same dp_pullup/dm_* bits the PHY uses without it, so only switching the
 * override off again would leave D+ without its pull-up, the device unplugged
 * for good (the first version did).
 */
static void usb_console_reconnect(void)
{
    if (esp_reset_reason() == ESP_RST_POWERON)
        return;
    uint32_t saved = USB_SERIAL_JTAG.conf0.val;
    usb_serial_jtag_pull_override_vals_t off = { 0 };   /* no pull-ups or -downs */
    usb_serial_jtag_ll_phy_enable_pull_override(&off);
    esp_rom_delay_us(100 * 1000);
    USB_SERIAL_JTAG.conf0.val = saved;
}
#endif

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && CONFIG_ESP_VIM_LAUNCHER_APP
/*
 * A launcher (bmorcelli/Launcher) starts an app with USB disconnected -- its
 * Arduino apps connect it again themselves, as Arduino's USB serial does --
 * and ESP-IDF's console, which expects the reset state, never did: the console
 * was gone, and the host saw no device at all until a power cycle. So put the
 * USB Serial/JTAG back as a power-on leaves it: its clock on, the chip's own
 * PHY routed to it, the pads on, and no overrides -- neither the peripheral's
 * nor the RTC controller's, which a software reset doesn't clear (hence "until
 * a power cycle"). The host then sees a plug-in.
 */
static void usb_console_take_back(void)
{
    /* Only when it was left disconnected: after our own restart (a new Vim
     * session) it is still connected, and a reset would make the host find
     * it all over again for nothing. */
    if (!USB_SERIAL_JTAG.conf0.pad_pull_override && USB_SERIAL_JTAG.conf0.dp_pullup
            && USB_SERIAL_JTAG.conf0.usb_pad_enable && !RTCCNTL.usb_conf.usb_pad_pull_override
            && !RTCCNTL.usb_conf.usb_pad_enable_override && !RTCCNTL.usb_conf.sw_usb_phy_sel)
        return;
    RTCCNTL.usb_conf.usb_pad_pull_override = 0;
    RTCCNTL.usb_conf.usb_pad_enable_override = 0;
    RTCCNTL.usb_conf.usb_tx_en_override = 0;
    RTCCNTL.usb_conf.usb_vref_override = 0;
    /* A software reset leaves the peripheral as the launcher's USB code left
     * it -- on purpose, so USB stays connected across one -- and clearing the
     * PHY overrides alone gave a device the host found but that never sent a
     * byte. Reset it: everything as at power-on, D+ pulled up included. */
    PERIPH_RCC_ATOMIC() {
        usb_serial_jtag_ll_enable_bus_clock(true);
        usb_serial_jtag_ll_reset_register();
    }
    usb_serial_jtag_ll_phy_enable_external(false);
    /* The PHY uses the dp_pullup/dm_* bits with the override off too (see
     * usb_console_reconnect), and the launcher leaves them all clear: set
     * them as a reset does -- D+ pulled up -- before letting go of it. */
    const usb_serial_jtag_pull_override_vals_t reset_pulls = { .dp_pu = true };
    usb_serial_jtag_ll_phy_enable_pull_override(&reset_pulls);
    usb_serial_jtag_ll_phy_disable_pull_override();
    usb_serial_jtag_ll_phy_enable_pad(true);
}

#endif



void app_main(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && CONFIG_ESP_VIM_BOARD_TAB5
    usb_console_reconnect();
#endif
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && CONFIG_ESP_VIM_LAUNCHER_APP
    usb_console_take_back();
#endif
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_logbuf_init();                  /* the log kept off Vim's screen (:EspLog) */

    /* Before anything else touches Vim: this snapshots Vim's .data while it
     * still holds its initial values. */
#if CONFIG_SPIRAM
    size_t budget = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 2;
    if (budget > ESP_VIM_HEAP_MAX)
        budget = ESP_VIM_HEAP_MAX;
#else
    size_t budget = 0;                  /* set as each session starts (vim_task) */
#endif
    ESP_ERROR_CHECK(esp_vim_session_init(&s_session, budget));

    /*
     * Order matters. NVS before the network (WiFi keeps its calibration and
     * credentials there); the network before the console UART driver, which
     * on the ESP32-S3 otherwise left WiFi's interrupt unhandled ("Unhandled
     * interrupt 12", in a loop) in the emulator. The network returns at once;
     * DHCP carries on in the background.
     */
    esp_board_init();                   /* the board's power switches (the Tab5's C6) first */
    nvs_init();
    esp_net_init();
    if (esp_time_init() != ESP_OK)      /* the RTC chip, NTP, the time zone */
        ESP_LOGE(TAG, "time: not available");
    console_init();
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && CONFIG_ESP_VIM_LAUNCHER_APP
    /* The host is still finding the console usb_console_take_back() just
     * reconnected: give it a moment, or what Vim says first is lost. */
    for (int i = 0; i < 10 && !usb_serial_jtag_is_connected(); i++)
        vTaskDelay(pdMS_TO_TICKS(100));
#endif
    esp_kbd_init();                     /* keyboards and touch feed console input */
    esp_err_t kb = esp_kbd_tab5_start();    /* the Tab5's keyboard accessory, when attached */
    if (kb != ESP_OK && kb != ESP_ERR_NOT_SUPPORTED)
        ESP_LOGE(TAG, "Tab5 keyboard: %s", esp_err_to_name(kb));
    kb = esp_kbd_cardputer_start();     /* the Cardputer's own keys */
    if (kb != ESP_OK && kb != ESP_ERR_NOT_SUPPORTED)
        ESP_LOGE(TAG, "Cardputer keyboard: %s", esp_err_to_name(kb));
    /* USB keyboards, on a USB host port -- on a board that switches the
     * port's power, once it's on (under esp-emu, with no expanders and no USB
     * controller, it isn't, and the host driver would assert). */
    if (esp_board_name() == NULL || esp_board_usb_power(true) == ESP_OK) {
        kb = esp_usbkbd_start();
        if (kb != ESP_OK && kb != ESP_ERR_NOT_SUPPORTED)
            ESP_LOGE(TAG, "USB keyboards: %s", esp_err_to_name(kb));
    }
#if CONFIG_ESP_VIM_DISPLAY
    if (esp_display_init() != ESP_OK)   /* each session hooks it up (vim_task) */
        ESP_LOGE(TAG, "display: not available -- the console is serial only");
#endif
#if CONFIG_ESP_VIM_TOUCH
    if (esp_touch_init() == ESP_OK)
        esp_touch_set_handler((esp_touch_handler_t)esp_display_touch, NULL);
    else
        ESP_LOGE(TAG, "touch: not available");
#endif
    kb = esp_kbd_tdeck_start();         /* the T-Deck's own keys: on the touch panel's bus */
    if (kb != ESP_OK && kb != ESP_ERR_NOT_SUPPORTED)
        ESP_LOGE(TAG, "T-Deck keyboard: %s", esp_err_to_name(kb));
    storage_init();
    environment_init();
#if CONFIG_ESP_VIM_SSH
    esp_ssh_init();
#endif
#if CONFIG_ESP_VIM_WEB
    esp_web_init();
#endif
    esp_ble_kbd_boot();     /* a bonded Bluetooth keyboard reconnects by itself */
    esp_pairui_start();     /* no keyboard: offer to pair one by touch */
    if (esp_power_init() != ESP_OK)     /* the battery, and sleep */
        ESP_LOGE(TAG, "power: not available");

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
    /*
     * The Vim task's stack is reserved at link time, not taken from the heap
     * at the first session: with WiFi up, the ESP32-S3's internal RAM no longer
     * has 64 KB in one piece, and "cannot create the Vim task" rebooted the
     * device in a loop. Every session reuses the same stack and TCB, so the
     * previous task must be fully deleted first.
     */
#if CONFIG_ESP_VIM_STACK_IN_PSRAM
    EXT_RAM_BSS_ATTR
#endif
    static StackType_t vim_stack[ESP_VIM_TASK_STACK];
    static StaticTask_t vim_tcb;
    for (unsigned session = 1;; session++) {
        g_vim_task = xTaskCreateStaticPinnedToCore(vim_task, "vim", ESP_VIM_TASK_STACK,
                                                   (void *)(uintptr_t)session, 5,
                                                   vim_stack, &vim_tcb, 0);
        if (g_vim_task == NULL) {
            ESP_LOGE(TAG, "cannot create the Vim task");
            esp_restart();
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* until Vim exits */
        while (eTaskGetState(g_vim_task) != eDeleted)
            vTaskDelay(1);                         /* its stack is about to be reused */
        between_sessions();
#if CONFIG_ESP_VIM_SESSION_REBOOT
        /* No copy of Vim's .data to start afresh from (esp_vim_session_init):
         * a restart is the clean slate. Under a launcher, a software reset
         * comes back to this app, not to the launcher. */
        esp_restart();
#endif
    }
}
