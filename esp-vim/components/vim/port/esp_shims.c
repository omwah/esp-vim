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
#include "esp_vim_port.h"
#include <sys/select.h>
#include <termios.h>
#include <ctype.h>
#include <stdint.h>
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
/*  Zero-timeout select() for console fds                                   */
/*  (see esp_vim_port.h for why this exists)                                */
/* ======================================================================= */

#define ESP_POLL_MAX 4

static struct {
    int fd;
    bool (*pending)(void);
} s_polls[ESP_POLL_MAX];

void esp_vim_register_input_poll(int fd, bool (*input_pending)(void))
{
    for (int i = 0; i < ESP_POLL_MAX; i++) {
        if (s_polls[i].pending == NULL || s_polls[i].fd == fd) {
            s_polls[i].fd = fd;
            s_polls[i].pending = input_pending;
            return;
        }
    }
    ESP_LOGE(TAG, "too many console polls registered; fd %d ignored", fd);
}

static bool (*poll_for(int fd))(void)
{
    for (int i = 0; i < ESP_POLL_MAX; i++)
        if (s_polls[i].pending != NULL && s_polls[i].fd == fd)
            return s_polls[i].pending;
    return NULL;
}

extern int __real_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);

int __wrap_select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    if (tv == NULL || tv->tv_sec != 0 || tv->tv_usec != 0)
        return __real_select(nfds, r, w, e, tv);        /* a real wait */

    /* Only short-circuit when EVERY fd asked about has a registered poll. */
    for (int fd = 0; fd < nfds; fd++) {
        bool asked = (r && FD_ISSET(fd, r)) || (w && FD_ISSET(fd, w))
                  || (e && FD_ISSET(fd, e));
        if (asked && poll_for(fd) == NULL)
            return __real_select(nfds, r, w, e, tv);
    }

    int ready = 0;
    for (int fd = 0; fd < nfds; fd++) {
        if (r && FD_ISSET(fd, r)) {
            if (poll_for(fd)())
                ready++;
            else
                FD_CLR(fd, r);
        }
        if (w && FD_ISSET(fd, w))
            ready++;                    /* a console is always writable */
        if (e && FD_ISSET(fd, e))
            FD_CLR(fd, e);              /* and never in an error state */
    }
    return ready;
}

/* ======================================================================= */
/*  tcgetattr(): report the control characters a real tty would             */
/* ======================================================================= */

/*
 * ESP-IDF's UART tcgetattr() zeroes the whole struct termios and never fills
 * c_cc[] (esp_driver_uart/src/uart_vfs.c). Vim reads the backspace key from
 * c_cc[VERASE] -- exactly as a desktop Vim learns it from `stty` -- so it got 0,
 * kept builtin xterm's ^H, and the DEL (0x7f) that nearly every terminal sends
 * for Backspace did nothing. VINTR was 0 as well.
 *
 * When a console leaves c_cc[] entirely zero, report the POSIX/Linux line
 * discipline defaults instead. A console that sets any of its own values (the
 * Phase 10 display console will) is left untouched.
 */
extern int __real_tcgetattr(int fd, struct termios *t);

int __wrap_tcgetattr(int fd, struct termios *t)
{
    int rc = __real_tcgetattr(fd, t);
    if (rc != 0 || t == NULL)
        return rc;

    for (int i = 0; i < NCCS; i++)
        if (t->c_cc[i] != 0)
            return rc;                  /* the driver reported its own */

    t->c_cc[VINTR]  = 0x03;             /* ^C */
    t->c_cc[VQUIT]  = 0x1c;             /* ^\ */
    t->c_cc[VERASE] = 0x7f;             /* ^? -- what Backspace sends */
    t->c_cc[VKILL]  = 0x15;             /* ^U */
    t->c_cc[VEOF]   = 0x04;             /* ^D */
    t->c_cc[VSTART] = 0x11;             /* ^Q */
    t->c_cc[VSTOP]  = 0x13;             /* ^S */
    t->c_cc[VSUSP]  = 0x1a;             /* ^Z */
    t->c_cc[VMIN]   = 1;
    t->c_cc[VTIME]  = 0;
    return rc;
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
/*  Interposed at LINK time with -Wl,--wrap (components/vim/CMakeLists.txt) */
/*  so that every caller resolves relative paths, however the call is       */
/*  spelled. An earlier version redirected Vim's mch_* macros instead, but  */
/*  os_unix.c -- which implements many mch_* functions -- calls stat() and  */
/*  open() directly ("Keep the #ifdef outside of stat(), it may be a       */
/*  macro"), so mch_getperm() and mch_isdir() still saw unresolved paths:   */
/*  isdirectory('.') was 0 and relative glob(), and with it :e <Tab>        */
/*  completion, found nothing. Absolute paths -- everything ESP-IDF itself  */
/*  uses -- pass through unchanged.                                         */
/* ======================================================================= */

/*
 * File identity. FATFS reports st_dev = st_ino = 0 for EVERY file (Phase 1),
 * and Vim decides "is this the same file?" by comparing exactly those two
 * fields (fullpathcmp() in filepath.c, buffer identity in buffer.c). So every
 * existing file looked like every other: :help, finding the tag file "the same"
 * as the current buffer, searched the wrong buffer (E434), and :e between
 * existing files was unreliable.
 *
 * We synthesize an identity from the file's path instead: a 32-bit FNV-1a hash
 * of the normalised, case-folded absolute path (FAT is case-insensitive, so
 * A.TXT and a.txt ARE one file), split across st_dev and st_ino because both
 * are only 16 bits on this target. Every Vim use of st_dev pairs it with
 * st_ino, except the (dev_T)-1 "stat failed" sentinel, which is avoided.
 *
 * fstat() must agree with stat(): Vim re-checks the inode of the fd it is
 * writing against the one stat() gave before (bufwrite.c) and fails with E949
 * "File changed while writing" on a mismatch. So fds opened through the
 * wrappers remember their identity until closed.
 */
static uint32_t path_identity(const char *abs)
{
    char norm[ESP_CWD_MAX];
    strncpy(norm, abs, sizeof(norm) - 1);
    norm[sizeof(norm) - 1] = '\0';
    esp_normalise(norm);

    uint32_t h = 2166136261u;                       /* FNV-1a */
    for (const char *c = norm; *c; c++) {
        h ^= (uint8_t)tolower((unsigned char)*c);
        h *= 16777619u;
    }
    return h;
}

static void apply_identity(struct stat *st, uint32_t id)
{
    uint16_t dev = (uint16_t)(id >> 16), ino = (uint16_t)id;
    if (dev == 0xFFFF)
        dev = 0xFFFE;           /* (dev_T)-1 means "stat failed" to Vim */
    if (dev == 0 && ino == 0)
        ino = 1;                /* never look like FATFS's all-zero default */
    st->st_dev = dev;
    st->st_ino = ino;
}

#define ESP_FD_TRACK 64
static uint32_t s_fd_id[ESP_FD_TRACK];
static bool     s_fd_known[ESP_FD_TRACK];

static void fd_remember(int fd, const char *abs)
{
    if (fd >= 0 && fd < ESP_FD_TRACK) {
        s_fd_id[fd] = path_identity(abs);
        s_fd_known[fd] = true;
    }
}

static void fd_forget(int fd)
{
    if (fd >= 0 && fd < ESP_FD_TRACK)
        s_fd_known[fd] = false;
}

extern int   __real_open(const char *, int, ...);
extern FILE *__real_fopen(const char *, const char *);
extern int   __real_close(int);
extern int   __real_fclose(FILE *);
extern int   __real_stat(const char *, struct stat *);
extern int   __real_fstat(int, struct stat *);
extern int   __real_access(const char *, int);
extern int   __real_unlink(const char *);
extern int   __real_remove(const char *);
extern int   __real_rmdir(const char *);
extern int   __real_chmod(const char *, mode_t);

#define RESOLVE(path, buf) esp_resolve((path), (buf), sizeof(buf))

int __wrap_open(const char *path, int flags, ...)
{
    char buf[ESP_CWD_MAX];
    mode_t mode = 0;

    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    const char *abs = RESOLVE(path, buf);
    int fd = __real_open(abs, flags, mode);
    if (fd >= 0 && abs != NULL)
        fd_remember(fd, abs);
    return fd;
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    char buf[ESP_CWD_MAX];
    const char *abs = RESOLVE(path, buf);
    FILE *fp = __real_fopen(abs, mode);
    if (fp != NULL && abs != NULL)
        fd_remember(fileno(fp), abs);
    return fp;
}

int __wrap_close(int fd)
{
    fd_forget(fd);
    return __real_close(fd);
}

int __wrap_fclose(FILE *fp)
{
    if (fp != NULL)
        fd_forget(fileno(fp));
    return __real_fclose(fp);
}

int __wrap_stat(const char *path, struct stat *st)
{
    char buf[ESP_CWD_MAX];
    const char *abs = RESOLVE(path, buf);
    int rc = __real_stat(abs, st);
    if (rc == 0 && st != NULL && abs != NULL)
        apply_identity(st, path_identity(abs));
    return rc;
}

int __wrap_fstat(int fd, struct stat *st)
{
    int rc = __real_fstat(fd, st);
    if (rc == 0 && st != NULL && fd >= 0 && fd < ESP_FD_TRACK && s_fd_known[fd])
        apply_identity(st, s_fd_id[fd]);
    return rc;
}

int __wrap_access(const char *path, int mode)
{
    char buf[ESP_CWD_MAX];
    return __real_access(RESOLVE(path, buf), mode);
}

int __wrap_unlink(const char *path)
{
    char buf[ESP_CWD_MAX];
    return __real_unlink(RESOLVE(path, buf));
}

int __wrap_remove(const char *path)
{
    char buf[ESP_CWD_MAX];
    return __real_remove(RESOLVE(path, buf));
}

int __wrap_rmdir(const char *path)
{
    char buf[ESP_CWD_MAX];
    return __real_rmdir(RESOLVE(path, buf));
}

int __wrap_chmod(const char *path, mode_t mode)
{
    char buf[ESP_CWD_MAX];
    return __real_chmod(RESOLVE(path, buf), mode);
}

/* Same mechanism for the rest of the path-taking calls. */
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
