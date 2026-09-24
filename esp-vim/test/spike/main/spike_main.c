/*
 * Phase 1 capability spike -- docs/PLAN.md.
 *
 * This is the GO/NO-GO gate for the whole port. It answers, on real emulated
 * ESP32-P4 silicon, the questions the Vim port's architecture assumes:
 *
 *   1. isatty(0) on the UART VFS fd  -- if false, Vim starts in filter mode and
 *      never renders. The single most important line here.
 *   2. select() on fd 0             -- does it block, and does it wake on input?
 *   3. tcgetattr/tcsetattr          -- do they succeed with VFS_SUPPORT_TERMIOS?
 *   4. Writable FAT at /fat         -- and what stat() actually fills in.
 *   5. Read-only FAT at /vimrt      -- $VIMRUNTIME must be readable at startup,
 *      because FEAT_NORMAL sources defaults.vim before the editor is usable.
 *   6. Odds and ends Vim's os_unix.c reaches for: system(), fork(), getpwuid(),
 *      signal().
 *
 * Output is deliberately greppable: every result is "  KEY: value", each section
 * is banner-delimited, and the run ends with SPIKE-DONE for esp-emu --exit-on.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <termios.h>
#include <signal.h>
#include <pwd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OK_FAIL(x) ((x) ? "yes" : "no")

static int g_pass = 0, g_fail = 0;

/* Record a gating result: something the port depends on. */
static void gate(const char *key, bool ok, const char *detail)
{
    printf("  %-28s %-5s %s\n", key, ok ? "PASS" : "FAIL", detail ? detail : "");
    if (ok) g_pass++; else g_fail++;
}

/* Record an observation: informative, not pass/fail. */
static void note(const char *key, const char *fmt, ...)
{
    va_list ap;
    printf("  %-28s ----  ", key);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void banner(const char *s)
{
    printf("\n=== %s\n", s);
}

/* ---------------------------------------------------------------- console -- */

/*
 * Question 1 and 3. Tested twice: once against the default console (a plain
 * non-driver UART) and once after installing the UART driver and routing the
 * VFS through it, because that is what decides whether isatty() and termios
 * have an implementation behind them at all.
 */
static void test_console_before(void)
{
    banner("console, BEFORE installing the UART driver");
    note("isatty(0)", "%s", OK_FAIL(isatty(0)));
    note("isatty(1)", "%s", OK_FAIL(isatty(1)));

    struct termios t;
    int r = tcgetattr(0, &t);
    note("tcgetattr(0)", "rc=%d errno=%d (%s)", r, r ? errno : 0,
         r ? strerror(errno) : "ok");
}

static void test_console_after(void)
{
    banner("console, AFTER uart_vfs_dev_use_driver()");

    /* This is the configuration the real port will run in. */
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 2048, 0, NULL, 0));
    uart_vfs_dev_use_driver(UART_NUM_0);
    uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
    uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);

    bool tty = isatty(0);
    gate("isatty(0)", tty,
         tty ? "Vim will render" : "*** Vim would start in filter mode ***");

    struct termios t;
    int rg = tcgetattr(0, &t);
    gate("tcgetattr(0)", rg == 0, rg ? strerror(errno) : "");

    if (rg == 0) {
        note("c_lflag", "0x%08lx (ICANON=%d ECHO=%d)",
             (unsigned long)t.c_lflag,
             (t.c_lflag & ICANON) ? 1 : 0, (t.c_lflag & ECHO) ? 1 : 0);

        /* Raw mode, as mch_settmode(TMODE_RAW) would want it. */
        struct termios raw = t;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_oflag &= ~OPOST;
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        int rs = tcsetattr(0, TCSANOW, &raw);
        gate("tcsetattr(0, raw)", rs == 0, rs ? strerror(errno) : "");

        if (rs == 0) {
            struct termios back;
            if (tcgetattr(0, &back) == 0) {
                bool stuck = (back.c_lflag & (ICANON | ECHO)) != 0;
                note("raw mode readback",
                     "c_lflag=0x%08lx %s", (unsigned long)back.c_lflag,
                     stuck ? "(flags did NOT take -- partial termios)"
                           : "(flags took)");
            }
            tcsetattr(0, TCSANOW, &t);  /* restore */
        }
    }

    /* fcntl O_NONBLOCK: the port needs non-blocking reads on the console. */
    int fl = fcntl(0, F_GETFL, 0);
    int rn = (fl == -1) ? -1 : fcntl(0, F_SETFL, fl | O_NONBLOCK);
    gate("fcntl(0, O_NONBLOCK)", rn != -1, rn == -1 ? strerror(errno) : "");
    if (fl != -1) fcntl(0, F_SETFL, fl);
}

/* Question 2: does select() on stdin block, and does it wake on input? */
static void test_select(void)
{
    banner("select() on fd 0");

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };

    int64_t t0 = esp_timer_get_time();
    int r = select(1, &rfds, NULL, NULL, &tv);
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

    note("select() rc", "%d (errno=%d)", r, r < 0 ? errno : 0);
    note("blocked for", "%lld ms of a 250 ms timeout", (long long)elapsed_ms);

    /* A timeout that actually elapses is the thing we need: it means select()
     * is really waiting on the fd, not returning immediately. */
    bool blocked = (r == 0 && elapsed_ms >= 200);
    bool had_input = (r > 0);
    gate("select() blocks on stdin", blocked || had_input,
         had_input ? "(input was already pending -- also fine)"
                   : (blocked ? "" : "returned immediately: busy-loop risk"));
}

/* ------------------------------------------------------------- filesystem -- */

static void dump_stat(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        note("stat", "%s: %s", path, strerror(errno));
        return;
    }
    note("stat fields", "size=%ld ino=%lu dev=%lu mode=%o mtime=%ld",
         (long)st.st_size, (unsigned long)st.st_ino,
         (unsigned long)st.st_dev, st.st_mode, (long)st.st_mtime);
}

/* Question 4. */
static void test_rw_fs(void)
{
    banner("writable FATFS at /fat (partition 'storage')");

    static wl_handle_t wl = WL_INVALID_HANDLE;
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 8,
        .format_if_mount_failed = true,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl("/fat", "storage", &cfg, &wl);
    gate("mount /fat rw", e == ESP_OK, esp_err_to_name(e));
    if (e != ESP_OK) return;

    int rmk = mkdir("/fat/d", 0755);
    gate("mkdir /fat/d", rmk == 0 || errno == EEXIST, (rmk == 0) ? "" : strerror(errno));

    FILE *f = fopen("/fat/d/t.txt", "w");
    gate("fopen write", f != NULL, f ? "" : strerror(errno));
    if (f) {
        fputs("hello from the spike\n", f);
        fclose(f);
    }

    char buf[64] = {0};
    f = fopen("/fat/d/t.txt", "r");
    gate("fopen read", f != NULL, f ? "" : strerror(errno));
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        gate("content round trip", n > 0 && strncmp(buf, "hello", 5) == 0, buf);
    }

    /* Vim's same-file detection leans on st_ino/st_dev; FAT is expected to
     * report 0 for both. Record it rather than assume it. */
    dump_stat("/fat/d/t.txt");

    DIR *d = opendir("/fat/d");
    gate("opendir", d != NULL, d ? "" : strerror(errno));
    if (d) {
        struct dirent *de;
        int n = 0;
        while ((de = readdir(d)) != NULL) n++;
        closedir(d);
        note("readdir entries", "%d", n);
    }

    int rrn = rename("/fat/d/t.txt", "/fat/d/t2.txt");
    gate("rename", rrn == 0, rrn ? strerror(errno) : "");

    /* ESP-IDF implements no working directory: chdir() is newlib's ENOSYS stub
     * and getcwd() always answers "/". Vim needs a CWD for :cd and for resolving
     * relative paths, so the port must supply a userspace one. Recorded here so
     * the gap is measured, not assumed. */
    char cwd[128] = {0};
    int rc = chdir("/fat/d");
    gate("chdir", rc == 0, rc ? strerror(errno) : "");
    char *gw = getcwd(cwd, sizeof(cwd));
    note("getcwd", "%s", gw ? cwd : strerror(errno));
    note("  -> port impact", "%s",
         (rc != 0 || (gw && strcmp(cwd, "/fat/d") != 0))
         ? "needs a userspace CWD in port/esp_shims.c"
         : "native CWD works");

    int rul = unlink("/fat/d/t2.txt");
    gate("unlink", rul == 0, rul ? strerror(errno) : "");
    chdir("/");
}

/* Question 5: $VIMRUNTIME must be readable, or FEAT_NORMAL cannot start. */
static void test_ro_fs(void)
{
    banner("read-only FATFS at /vimrt (partition 'vimrt')");

    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "vimrt");
    gate("partition 'vimrt' found", p != NULL, p ? "" : "check partitions.csv");
    if (!p) return;
    note("vimrt size", "%lu bytes at 0x%lx",
         (unsigned long)p->size, (unsigned long)p->address);

    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t e = esp_vfs_fat_rawflash_mount("/vimrt", "vimrt", &cfg);
    gate("mount /vimrt ro", e == ESP_OK, esp_err_to_name(e));
    if (e != ESP_OK) return;

    char buf[96] = {0};
    FILE *f = fopen("/vimrt/hello.txt", "r");
    gate("read /vimrt/hello.txt", f != NULL, f ? "" : strerror(errno));
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[strcspn(buf, "\n")] = 0;
        gate("ro content", n > 0, buf);
    }

    dump_stat("/vimrt/hello.txt");

    DIR *d = opendir("/vimrt/syntax");
    gate("opendir /vimrt/syntax", d != NULL, d ? "" : strerror(errno));
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) note("  entry", "%s", de->d_name);
        closedir(d);
    }

    /* Confirm it really is read-only -- if writes succeed we have the wrong
     * mount and Vim could corrupt its own runtime. */
    f = fopen("/vimrt/should-fail.txt", "w");
    gate("ro partition rejects write", f == NULL,
         f ? "*** WRITE SUCCEEDED -- not read-only ***" : strerror(errno));
    if (f) fclose(f);
}

/* ------------------------------------------------------------------ misc -- */

/*
 * Question 6: the odds and ends os_unix.c reaches for.
 *
 * Declared weak so their ABSENCE is a runtime observation rather than a link
 * error -- which is the interesting result. Vim calls all of these; anything
 * reported "absent" here needs a shim in port/esp_shims.c.
 */
/* Declared by ESP-IDF's headers: probe with __typeof__ so signatures match. */
extern __typeof__(getuid)   getuid   __attribute__((weak));
extern __typeof__(geteuid)  geteuid  __attribute__((weak));
extern __typeof__(getpwuid) getpwuid __attribute__((weak));
extern __typeof__(getpwnam) getpwnam __attribute__((weak));
extern __typeof__(signal)   signal   __attribute__((weak));
extern __typeof__(fork)     fork     __attribute__((weak));
extern __typeof__(system)   system   __attribute__((weak));
extern __typeof__(chmod)    chmod    __attribute__((weak));
extern __typeof__(umask)    umask    __attribute__((weak));

/*
 * NOT declared by ESP-IDF's headers at all -- 'lstat' is a compile error before
 * it is ever a link error. That is itself a finding: the port must provide both
 * declaration and definition. Explicit prototypes so we can still probe them.
 */
extern int     lstat(const char *, struct stat *) __attribute__((weak));
extern ssize_t readlink(const char *, char *, size_t) __attribute__((weak));
extern int     symlink(const char *, const char *) __attribute__((weak));
extern int     link(const char *, const char *) __attribute__((weak));

static void sym(const char *name, void *addr, const char *why)
{
    printf("  %-28s %-7s %s\n", name, addr ? "present" : "ABSENT", why);
}

static void test_posix_odds(void)
{
    banner("POSIX surface Vim's os_unix.c expects (weak-linked probe)");

    sym("system()",   (void *)system,   "Vim routes :! here via USE_SYSTEM");
    sym("fork()",     (void *)fork,     "must be absent; :! and pty depend on it");
    sym("getuid()",   (void *)getuid,   "mch_get_uid");
    sym("geteuid()",  (void *)geteuid,  "file-permission checks");
    sym("getpwuid()", (void *)getpwuid, "'~' expansion, mch_get_user_name");
    sym("getpwnam()", (void *)getpwnam, "'~user' expansion");
    sym("signal()",   (void *)signal,   "SIGINT/SIGWINCH handling");
    sym("lstat()",    (void *)lstat,    "symlink-aware stat");
    sym("readlink()", (void *)readlink, "resolving symlinks");
    sym("symlink()",  (void *)symlink,  "creating symlinks");
    sym("link()",     (void *)link,     "backup-by-rename path");
    sym("chmod()",    (void *)chmod,    "preserving file modes");
    sym("umask()",    (void *)umask,    "new-file modes");

    if (&system) {
        int rs = system("echo hi");
        note("system() returns", "%d (a stub is fine; Vim reports E371)", rs);
    }
    if (&getuid) note("getuid()", "%d", (int)getuid());
    if (&getpwuid) {
        struct passwd *pw = getpwuid(0);
        note("getpwuid(0)", "%s", pw && pw->pw_name ? pw->pw_name : "NULL");
    }

    char *home = getenv("HOME");
    note("getenv(HOME)", "%s", home ? home : "unset -- app_main must set it");
    note("sizeof(void*)", "%d", (int)sizeof(void *));
}

static void report_chip(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    banner("target");
    note("chip", "model=%d cores=%d revision=%d", ci.model, ci.cores, ci.revision);
    note("IDF", "%s", esp_get_idf_version());
    note("free heap", "%lu bytes", (unsigned long)esp_get_free_heap_size());
}

void app_main(void)
{
    /* Quiet the log so the report is the only thing on the wire. */
    esp_log_level_set("*", ESP_LOG_WARN);

    printf("\n\nSPIKE-BEGIN\n");
    printf("Phase 1 capability spike -- docs/PLAN.md\n");

    report_chip();
    test_console_before();
    test_console_after();
    test_select();
    test_rw_fs();
    test_ro_fs();
    test_posix_odds();

    printf("\n=== summary\n");
    printf("  gates passed: %d\n", g_pass);
    printf("  gates failed: %d\n", g_fail);
    printf("\nSPIKE-DONE\n");

    /* Do not return: app_main returning would restart or idle confusingly. */
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
