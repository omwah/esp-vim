/*
 * The POSIX surface Vim expects, supplied for ESP-IDF.
 *
 * Two jobs:
 *
 *  1. A userspace current working directory. ESP-IDF has none at all -- chdir()
 *     is newlib's ENOSYS stub and getcwd() always answers "/" (Phase 1). Vim
 *     needs a CWD for :cd, for mch_dirname(), and because mch_FullName() works
 *     by chdir'ing into a directory and asking where it landed.
 *
 *  2. Stubs for the 16 symbols Vim references but ESP-IDF does not provide
 *     (docs/PHASE2.md). Nearly all sit on code paths that cannot execute here --
 *     there is no fork(), and USE_SYSTEM routes :! through system() -- but they
 *     are compiled, so they must link.
 *
 * Every stub logs the first time it is called. If one of these ever appears in
 * the log, a path we believed unreachable is in fact reached, and that is worth
 * knowing immediately rather than debugging blind.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "vim-shim";

/*
 * Report the first call to an unreachable-by-design stub. Not rate-limited
 * beyond "once": a stub that fires repeatedly would otherwise drown the editor's
 * own output on the same UART.
 */
#define STUB_ONCE(name)                                                       \
    do {                                                                      \
        static bool _warned;                                                  \
        if (!_warned) {                                                       \
            _warned = true;                                                   \
            ESP_LOGW(TAG, "stub called: %s() -- a path believed unreachable "  \
                          "on this platform was taken", name);                \
        }                                                                     \
    } while (0)

/* ======================================================================= */
/*  Vim's heap: PSRAM first                                                 */
/*                                                                          */
/*  malloc/calloc/realloc are renamed to these by -D in the component's      */
/*  CMakeLists, for Vim's sources only. Prefer the 32 MB of PSRAM; fall back */
/*  to any heap rather than fail, since Vim treats NULL as out-of-memory.    */
/* ======================================================================= */

#define VIM_HEAP_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void *esp_vim_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, VIM_HEAP_CAPS);
    return p != NULL ? p : heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
}

void *esp_vim_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, VIM_HEAP_CAPS);
    return p != NULL ? p : heap_caps_calloc(n, size, MALLOC_CAP_DEFAULT);
}

void *esp_vim_realloc(void *ptr, size_t size)
{
    void *p = heap_caps_realloc(ptr, size, VIM_HEAP_CAPS);
    return p != NULL ? p : heap_caps_realloc(ptr, size, MALLOC_CAP_DEFAULT);
}

/* ======================================================================= */
/*  Userspace current working directory                                     */
/* ======================================================================= */

#ifndef ESP_VIM_CWD_INITIAL
# define ESP_VIM_CWD_INITIAL "/fat"
#endif

#define ESP_CWD_MAX 256

static char esp_cwd[ESP_CWD_MAX] = ESP_VIM_CWD_INITIAL;

/*
 * Collapse "." and ".." and any duplicate slashes, in place.
 *
 * This has to be done by string surgery rather than by asking the filesystem:
 * there is no realpath(), FAT has no symlinks for ".." to be ambiguous about,
 * and a path that does not exist yet must still normalise (Vim asks about files
 * before creating them).
 */
static void esp_normalise(char *path)
{
    char *segs[64];
    int   n = 0;
    char *p = path;

    while (*p == '/')
        p++;

    for (char *tok = strtok(p, "/"); tok != NULL; tok = strtok(NULL, "/")) {
        if (strcmp(tok, ".") == 0)
            continue;
        if (strcmp(tok, "..") == 0) {
            if (n > 0)
                n--;              /* ".." at the root stays at the root */
            continue;
        }
        if (n < (int)(sizeof(segs) / sizeof(segs[0])))
            segs[n++] = tok;
    }

    char out[ESP_CWD_MAX];
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t need = 1 + strlen(segs[i]);
        if (len + need + 1 >= sizeof(out))
            break;
        out[len++] = '/';
        strcpy(out + len, segs[i]);
        len += strlen(segs[i]);
    }
    if (len == 0) {
        strcpy(path, "/");
    } else {
        memcpy(path, out, len + 1);
    }
}

/*
 * Resolve a possibly-relative path against the userspace CWD.
 *
 * Returns a pointer into "buf", or the original path when it is already
 * absolute and needs no normalising -- the common case, since mch_FullName()
 * hands us absolute paths once the CWD works.
 */
static const char *esp_resolve(const char *path, char *buf, size_t buflen)
{
    if (path == NULL)
        return NULL;

    if (path[0] == '/') {
        /* Absolute, but may still contain "." or ".." for the VFS to choke on. */
        if (strstr(path, "/.") == NULL) {
            return path;
        }
        if (strlen(path) >= buflen)
            return path;            /* too long to normalise; let it fail honestly */
        strcpy(buf, path);
        esp_normalise(buf);
        return buf;
    }

    if (snprintf(buf, buflen, "%s/%s", esp_cwd, path) >= (int)buflen)
        return path;                /* overflow: fail honestly rather than truncate */
    esp_normalise(buf);
    return buf;
}

/*
 * Interposed with -Wl,--wrap rather than defined under the real names: ESP-IDF
 * already defines both (components/newlib/src/realpath.c:112-125, where chdir is
 * literally "errno = ENOSYS; return -1" and getcwd hardcodes "/"), so defining
 * ours would be a duplicate symbol.
 */
int __wrap_chdir(const char *path)
{
    char buf[ESP_CWD_MAX];
    const char *abs = esp_resolve(path, buf, sizeof(buf));
    struct stat st;

    if (abs == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (stat(abs, &st) != 0)
        return -1;                  /* errno set by stat() */
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    if (strlen(abs) >= sizeof(esp_cwd)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(esp_cwd, abs);
    return 0;
}

char *__wrap_getcwd(char *buf, size_t size)
{
    if (buf == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (strlen(esp_cwd) >= size) {
        errno = ERANGE;
        return NULL;
    }
    strcpy(buf, esp_cwd);
    return buf;
}

/* ======================================================================= */
/*  File operations, CWD-aware                                              */
/*                                                                          */
/*  Vim's mch_open/mch_fopen/mch_stat/... macros are redirected here by      */
/*  port/auto/config.h, so no Vim call site needed changing -- only the      */
/*  #ifndef guards added by patch 0005.                                      */
/* ======================================================================= */

int esp_open(const char *path, int flags, ...)
{
    char buf[ESP_CWD_MAX];
    mode_t mode = 0;

    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return open(esp_resolve(path, buf, sizeof(buf)), flags, mode);
}

/*
 * Returns void* because port/auto/config.h must declare this before <stdio.h>
 * has been seen; the mch_fopen macro casts it back to FILE*.
 */
void *esp_fopen(const char *path, const char *mode)
{
    char buf[ESP_CWD_MAX];
    return (void *)fopen(esp_resolve(path, buf, sizeof(buf)), mode);
}

int esp_stat(const char *path, struct stat *st)
{
    char buf[ESP_CWD_MAX];
    return stat(esp_resolve(path, buf, sizeof(buf)), st);
}

int esp_access(const char *path, int mode)
{
    char buf[ESP_CWD_MAX];
    return access(esp_resolve(path, buf, sizeof(buf)), mode);
}

int esp_unlink(const char *path)
{
    char buf[ESP_CWD_MAX];
    return unlink(esp_resolve(path, buf, sizeof(buf)));
}

int esp_rmdir(const char *path)
{
    char buf[ESP_CWD_MAX];
    return rmdir(esp_resolve(path, buf, sizeof(buf)));
}

/*
 * rename/mkdir/opendir have no mch_* macro to redirect, and defining them under
 * their own names would recurse. -Wl,--wrap solves both: __real_* is the
 * original, so these resolve the path and then delegate.
 */
extern int   __real_rename(const char *, const char *);
extern int   __real_mkdir(const char *, mode_t);
extern DIR  *__real_opendir(const char *);

int __wrap_rename(const char *from, const char *to)
{
    char fbuf[ESP_CWD_MAX], tbuf[ESP_CWD_MAX], fabs[ESP_CWD_MAX];
    const char *f = esp_resolve(from, fbuf, sizeof(fbuf));

    /* Copy the first result: the second resolve may reuse its buffer. */
    strncpy(fabs, f, sizeof(fabs) - 1);
    fabs[sizeof(fabs) - 1] = '\0';
    return __real_rename(fabs, esp_resolve(to, tbuf, sizeof(tbuf)));
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    char buf[ESP_CWD_MAX];
    return __real_mkdir(esp_resolve(path, buf, sizeof(buf)), mode);
}

DIR *__wrap_opendir(const char *path)
{
    char buf[ESP_CWD_MAX];
    return __real_opendir(esp_resolve(path, buf, sizeof(buf)));
}

/* ======================================================================= */
/*  Things ESP-IDF genuinely lacks                                          */
/* ======================================================================= */

/*
 * Declared by <time.h> but not implemented. Vim uses it for short waits, so
 * unlike the rest of this file it needs to actually work.
 */
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (rem != NULL) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }

    int64_t ms = (int64_t)req->tv_sec * 1000 + req->tv_nsec / 1000000;
    if (ms <= 0) {
        taskYIELD();
        return 0;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}

/*
 * Identity. There are no users, no groups and no hostname resolution, but Vim
 * asks -- for '~' expansion, for file-permission checks it then ignores, and
 * for the ":version" banner. Constant answers are correct here, not stubs.
 */
uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void)  { return 0; }
gid_t getegid(void) { return 0; }

static struct passwd esp_pw = {
    .pw_name  = (char *)"esp",
    .pw_uid   = 0,
    .pw_gid   = 0,
    .pw_dir   = (char *)"/fat",
    .pw_shell = (char *)"",
};

struct passwd *getpwuid(uid_t uid)
{
    (void)uid;
    return &esp_pw;
}

struct passwd *getpwnam(const char *name)
{
    if (name != NULL && strcmp(name, esp_pw.pw_name) == 0)
        return &esp_pw;
    return NULL;
}

struct group *getgrgid(gid_t gid)
{
    (void)gid;
    STUB_ONCE("getgrgid");
    return NULL;
}

int gethostname(char *name, size_t len)
{
    const char *h = "esp32p4";
    if (name == NULL || len == 0) {
        errno = EINVAL;
        return -1;
    }
    strncpy(name, h, len - 1);
    name[len - 1] = '\0';
    return 0;
}

mode_t umask(mode_t mask)
{
    (void)mask;
    return 0;           /* FATFS has no permission bits to mask */
}

/*
 * No process model. Every one of these is on a path that cannot be reached:
 * Vim's :! goes through system() because USE_SYSTEM is defined, and there is no
 * fork() for anything else to build on. They exist so the link succeeds, and
 * they shout if that reasoning turns out to be wrong.
 */
int execvp(const char *file, char *const argv[])
{
    (void)file; (void)argv;
    STUB_ONCE("execvp");
    errno = ENOSYS;
    return -1;
}

int pipe(int fds[2])
{
    (void)fds;
    STUB_ONCE("pipe");
    errno = ENOSYS;
    return -1;
}

int dup(int fd)
{
    (void)fd;
    STUB_ONCE("dup");
    errno = ENOSYS;
    return -1;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)pid; (void)options;
    STUB_ONCE("waitpid");
    if (status != NULL)
        *status = 0;
    errno = ECHILD;
    return -1;
}

/*
 * No signal delivery of any kind. CTRL-C reaches Vim as byte 0x03 in the input
 * stream, which the console layer turns into got_int -- see docs/PLAN.md Phase 3.
 * Registering a handler therefore has to succeed quietly rather than fail: Vim
 * checks the return value and complains, and the handler would never fire either
 * way.
 */
typedef void (*esp_sighandler_t)(int);

/*
 * The one signal that IS delivered: SIGALRM, raised by the setitimer() below.
 * Vim arms it around regexp matching and its handler (set_flag in os_unix.c)
 * only sets a volatile flag both regexp engines poll -- safe to call from the
 * esp_timer task.
 */
static volatile esp_sighandler_t s_alrm_handler;

esp_sighandler_t signal(int signum, esp_sighandler_t handler)
{
    if (signum == SIGALRM) {
        esp_sighandler_t prev = s_alrm_handler;
        s_alrm_handler = handler;
        return prev;
    }
    return NULL;        /* nothing else is delivered; NULL == SIG_DFL */
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *old)
{
    if (old != NULL) {
        memset(old, 0, sizeof(*old));
        if (signum == SIGALRM)
            old->sa_handler = s_alrm_handler;
    }
    if (act != NULL && signum == SIGALRM)
        s_alrm_handler = act->sa_handler;
    return 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    (void)how; (void)set;
    if (oldset != NULL)
        memset(oldset, 0, sizeof(*oldset));
    return 0;
}

int sigpending(sigset_t *set)
{
    if (set != NULL)
        memset(set, 0, sizeof(*set));
    return 0;           /* nothing is ever pending */
}

/*
 * ITIMER_REAL, implemented with a one-shot esp_timer that "delivers" SIGALRM by
 * calling the handler registered above.
 *
 * Vim arms this around every regexp match (start_timeout/stop_timeout in
 * os_unix.c) so a pathological pattern is abandoned after 'redrawtime' or a
 * search() timeout. Without it the backtracking engine -- which this port uses
 * by default, see the system vimrc -- could hang the editor on an exponential
 * pattern. It was first a no-op; before that, an ENOSYS stub that Vim reported
 * as "E1286: Could not set timeout" on every search.
 */
static esp_timer_handle_t s_itimer;

static void itimer_fire(void *arg)
{
    (void)arg;
    esp_sighandler_t h = s_alrm_handler;
    if (h != NULL && h != SIG_IGN && h != SIG_DFL)
        h(SIGALRM);
}

int setitimer(int which, const struct itimerval *new_value,
              struct itimerval *old_value)
{
    if (which != ITIMER_REAL) {
        errno = EINVAL;
        return -1;
    }
    if (old_value != NULL)
        memset(old_value, 0, sizeof(*old_value));   /* remaining time not tracked */

    if (s_itimer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = itimer_fire,
            .name = "vim-itimer",
        };
        if (esp_timer_create(&args, &s_itimer) != ESP_OK) {
            errno = ENOMEM;
            return -1;
        }
    }

    esp_timer_stop(s_itimer);       /* fails harmlessly if not running */
    if (new_value == NULL)
        return 0;

    uint64_t us = (uint64_t)new_value->it_value.tv_sec * 1000000u
                + (uint64_t)new_value->it_value.tv_usec;
    if (us == 0)
        return 0;                   /* an all-zero it_value disarms */
    return esp_timer_start_once(s_itimer, us) == ESP_OK ? 0 : -1;
}

/*
 * Vim exits by calling exit() from mch_exit(), and on ESP-IDF exit() reaches
 * _exit(), which is literally "abort()" (components/newlib/src/syscalls.c:121).
 * That panics the chip and reboots it -- so an ordinary ":q" became a crash loop
 * until this wrap existed.
 *
 * Parking the task instead leaves the panic path free for real faults, and keeps
 * the exit code visible for the emulator harness to assert on.
 */
void __wrap_exit(int status)
{
    /* ESPVIM-END terminates the line so a harness using esp-emu --exit-on can
     * trigger on it: --exit-on stops the emulator the moment its string appears,
     * so triggering on "ESPVIM-EXIT" itself truncated the " rc=N" after it. */
    /* Heap telemetry rides on the exit line so every gate run records it.
     * int_min is the internal-RAM low-water mark: the number that went to zero
     * before Vim's heap was moved to PSRAM (docs/PHASE4.md). */
    printf("\nESPVIM-EXIT rc=%d int_free=%u int_min=%u psram_free=%u ESPVIM-END\n",
           status,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    fflush(stdout);
    vTaskDelete(NULL);          /* does not return */
    for (;;)
        vTaskDelay(portMAX_DELAY);
}

void __wrap__exit(int status)
{
    __wrap_exit(status);
}

/*
 * Vim's :! with USE_SYSTEM. There is no shell to run, and saying so plainly is
 * better than a mysterious failure -- mch_call_shell() turns a -1 into E371.
 */
int __wrap_system(const char *command)
{
    if (command == NULL)
        return 0;       /* "is a shell available?" -- no */
    ESP_LOGW(TAG, "no shell on this platform; refused: %s", command);
    errno = ENOSYS;
    return -1;
}
