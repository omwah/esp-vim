/*
 * esp_fs: validated file operations. See include/esp_fs.h for the contract.
 */

#include "esp_fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define PATH_MAX_LEN    512     /* FAT allows 255 per name; this bounds a whole path */
#define MAX_DEPTH       24      /* recursion limit for copy/delete */
#define COPY_CHUNK      4096

static const struct {
    const char *path;
    bool readonly;
} ROOTS[] = {
    { "/fat",   false },
    { "/sd",    false },
    { "/vimrt", true  },
};
#define NROOTS (sizeof ROOTS / sizeof ROOTS[0])

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

esp_err_t esp_fs_init(void)
{
    if (s_lock == NULL)
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static void lock(void)   { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGiveRecursive(s_lock); }

static int fail(esp_fs_err_t *err, const char *fmt, ...)
{
    if (err != NULL) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err->msg, sizeof err->msg, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static int fail_errno(esp_fs_err_t *err, const char *path)
{
    return fail(err, "%s: %s", path, strerror(errno));
}

static bool is_mounted(const char *root)
{
    DIR *d = opendir(root);
    if (d == NULL)
        return false;
    closedir(d);
    return true;
}

/* Index of the root containing normalised {p}, or -1. "/" is not in a root. */
static int root_of(const char *p)
{
    for (size_t i = 0; i < NROOTS; i++) {
        size_t n = strlen(ROOTS[i].path);
        if (strncmp(p, ROOTS[i].path, n) == 0 && (p[n] == '\0' || p[n] == '/'))
            return (int)i;
    }
    return -1;
}

/* Fold "//", "." and ".." in absolute {in}. ".." above "/" stays at "/". */
static int normalise(const char *in, char *out, size_t outlen)
{
    if (in == NULL || in[0] != '/')
        return -1;
    size_t len = 0;
    out[0] = '\0';
    const char *p = in;
    while (*p) {
        while (*p == '/')
            p++;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t n = (size_t)(p - s);
        if (n == 0 || (n == 1 && s[0] == '.'))
            continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (len > 0 && out[len - 1] != '/')
                len--;
            if (len > 0)
                len--;          /* drop the '/' too */
            out[len] = '\0';
            continue;
        }
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)s[i] < 0x20)
                return -1;      /* no control characters in names */
        if (len + 1 + n + 1 > outlen)
            return -1;
        out[len++] = '/';
        memcpy(out + len, s, n);
        len += n;
        out[len] = '\0';
    }
    if (len == 0) {
        if (outlen < 2)
            return -1;
        strcpy(out, "/");
    }
    return 0;
}

int esp_fs_check(const char *path, bool for_write, char *out, size_t outlen, esp_fs_err_t *err)
{
    if (path == NULL || path[0] != '/')
        return fail(err, "%s: not an absolute path", path ? path : "(null)");
    if (normalise(path, out, outlen) != 0)
        return fail(err, "%s: invalid or too long", path);
    if (strcmp(out, "/") == 0) {
        if (for_write)
            return fail(err, "/: cannot be changed");
        return 0;
    }
    int r = root_of(out);
    if (r < 0 || !is_mounted(ROOTS[r].path))
        return fail(err, "%s: outside the device's storage (/fat, /sd, /vimrt)", out);
    if (for_write) {
        if (ROOTS[r].readonly)
            return fail(err, "%s: %s is read-only", out, ROOTS[r].path);
        if (strcmp(out, ROOTS[r].path) == 0)
            return fail(err, "%s: a storage root cannot be changed", out);
    }
    return 0;
}

static void root_info(size_t i, esp_fs_root_t *r)
{
    r->path = ROOTS[i].path;
    r->readonly = ROOTS[i].readonly;
    r->total = r->free = 0;
    uint64_t total, freeb;
    if (esp_vfs_fat_info(ROOTS[i].path, &total, &freeb) == ESP_OK) {
        r->total = total;
        r->free = freeb;
    }
}

int esp_fs_roots(esp_fs_root_cb cb, void *ctx, esp_fs_err_t *err)
{
    lock();
    for (size_t i = 0; i < NROOTS; i++) {
        if (!is_mounted(ROOTS[i].path))
            continue;
        esp_fs_root_t r;
        root_info(i, &r);
        if (!cb(ctx, &r))
            break;
    }
    unlock();
    return 0;
}

int esp_fs_space(const char *path, esp_fs_root_t *root, esp_fs_err_t *err)
{
    char p[PATH_MAX_LEN];
    if (esp_fs_check(path, false, p, sizeof p, err) != 0)
        return -1;
    int r = root_of(p);
    if (r < 0)
        return fail(err, "%s: not on a storage root", p);
    lock();
    root_info((size_t)r, root);
    unlock();
    return 0;
}

int esp_fs_list(const char *dir, esp_fs_entry_cb cb, void *ctx, esp_fs_err_t *err)
{
    char p[PATH_MAX_LEN];
    if (esp_fs_check(dir, false, p, sizeof p, err) != 0)
        return -1;

    lock();
    int rc = 0;
    if (strcmp(p, "/") == 0) {
        /* The virtual top: one directory per mounted root. */
        for (size_t i = 0; i < NROOTS; i++) {
            if (!is_mounted(ROOTS[i].path))
                continue;
            esp_fs_entry_t e = { .name = ROOTS[i].path + 1, .dir = true };
            if (!cb(ctx, &e))
                break;
        }
        unlock();
        return 0;
    }

    DIR *d = opendir(p);
    if (d == NULL) {
        rc = fail_errno(err, p);
    } else {
        size_t base = strlen(p);
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            if (base + 1 + strlen(de->d_name) + 1 > sizeof p)
                continue;
            p[base] = '/';
            strcpy(p + base + 1, de->d_name);
            struct stat st;
            esp_fs_entry_t e = { .name = de->d_name, .dir = de->d_type == DT_DIR };
            if (stat(p, &st) == 0) {
                e.dir = S_ISDIR(st.st_mode);
                e.size = e.dir ? 0 : (uint64_t)st.st_size;
                e.mtime = st.st_mtime;
            }
            p[base] = '\0';
            if (!cb(ctx, &e))
                break;
        }
        closedir(d);
    }
    unlock();
    return rc;
}

/* ------------------------------------------------------------ recursion -- */

typedef struct {
    esp_fs_progress_cb progress;
    void *ctx;
    uint64_t done;
    esp_fs_err_t *err;
    uint8_t *buf;
} op_t;

static bool keep_going(op_t *op)
{
    if (op->progress != NULL && !op->progress(op->ctx, op->done)) {
        fail(op->err, "interrupted");
        return false;
    }
    return true;
}

static int delete_tree(op_t *op, char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return fail_errno(op->err, path);
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) != 0)
            return fail_errno(op->err, path);
        return keep_going(op) ? 0 : -1;
    }
    if (depth >= MAX_DEPTH)
        return fail(op->err, "%s: nested too deeply", path);

    DIR *d = opendir(path);
    if (d == NULL)
        return fail_errno(op->err, path);
    size_t base = strlen(path);
    int rc = 0;
    struct dirent *de;
    while (rc == 0 && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (base + 1 + strlen(de->d_name) + 1 > PATH_MAX_LEN) {
            rc = fail(op->err, "%s/%s: path too long", path, de->d_name);
            break;
        }
        path[base] = '/';
        strcpy(path + base + 1, de->d_name);
        rc = delete_tree(op, path, depth + 1);
        path[base] = '\0';
    }
    closedir(d);
    if (rc == 0 && rmdir(path) != 0)
        rc = fail_errno(op->err, path);
    return rc;
}

static int copy_file(op_t *op, const char *src, const char *dst, bool overwrite)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return fail_errno(op->err, src);
    int flags = O_WRONLY | O_CREAT | O_TRUNC | (overwrite ? 0 : O_EXCL);
    int out = open(dst, flags, 0666);
    if (out < 0) {
        int e = errno;
        close(in);
        errno = e;
        if (e == EEXIST)
            return fail(op->err, "%s: already exists", dst);
        return fail_errno(op->err, dst);
    }
    int rc = 0;
    for (;;) {
        ssize_t n = read(in, op->buf, COPY_CHUNK);
        if (n < 0) {
            rc = fail_errno(op->err, src);
            break;
        }
        if (n == 0)
            break;
        for (ssize_t off = 0; off < n; ) {
            ssize_t w = write(out, op->buf + off, (size_t)(n - off));
            if (w <= 0) {
                rc = fail(op->err, "%s: %s", dst, w < 0 ? strerror(errno) : "disk full");
                break;
            }
            off += w;
        }
        if (rc != 0)
            break;
        op->done += (uint64_t)n;
        if (!keep_going(op)) {
            rc = -1;
            break;
        }
    }
    close(in);
    if (close(out) != 0 && rc == 0)
        rc = fail_errno(op->err, dst);
    if (rc != 0)
        unlink(dst);            /* never leave a truncated copy behind */
    return rc;
}

static int copy_tree(op_t *op, char *src, char *dst, bool overwrite, int depth)
{
    struct stat st;
    if (stat(src, &st) != 0)
        return fail_errno(op->err, src);
    if (!S_ISDIR(st.st_mode)) {
        struct stat dt;
        if (stat(dst, &dt) == 0 && S_ISDIR(dt.st_mode))
            return fail(op->err, "%s: is a directory", dst);
        return copy_file(op, src, dst, overwrite);
    }
    if (depth >= MAX_DEPTH)
        return fail(op->err, "%s: nested too deeply", src);

    struct stat dt;
    if (stat(dst, &dt) == 0) {
        if (!S_ISDIR(dt.st_mode))
            return fail(op->err, "%s: exists and is not a directory", dst);
        if (!overwrite)
            return fail(op->err, "%s: already exists", dst);
    } else if (mkdir(dst, 0777) != 0) {
        return fail_errno(op->err, dst);
    }

    DIR *d = opendir(src);
    if (d == NULL)
        return fail_errno(op->err, src);
    size_t sb = strlen(src), db = strlen(dst);
    int rc = 0;
    struct dirent *de;
    while (rc == 0 && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        size_t n = strlen(de->d_name);
        if (sb + 1 + n + 1 > PATH_MAX_LEN || db + 1 + n + 1 > PATH_MAX_LEN) {
            rc = fail(op->err, "%s/%s: path too long", src, de->d_name);
            break;
        }
        src[sb] = '/';
        strcpy(src + sb + 1, de->d_name);
        dst[db] = '/';
        strcpy(dst + db + 1, de->d_name);
        rc = copy_tree(op, src, dst, overwrite, depth + 1);
        src[sb] = '\0';
        dst[db] = '\0';
    }
    closedir(d);
    return rc;
}

/* Is {inner} {outer} itself or somewhere below it? (Both normalised.) */
static bool within(const char *inner, const char *outer)
{
    size_t n = strlen(outer);
    return strncmp(inner, outer, n) == 0 && (inner[n] == '\0' || inner[n] == '/');
}

/* Paths and scratch for one operation, all on the heap: this may run on a
 * task with a small stack (the web server). */
typedef struct {
    char src[PATH_MAX_LEN];
    char dst[PATH_MAX_LEN];
    uint8_t buf[COPY_CHUNK];
} scratch_t;

static scratch_t *scratch_alloc(void)
{
    scratch_t *s = heap_caps_malloc(sizeof *s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s ? s : malloc(sizeof *s);
}

static int copy_or_move(bool move, const char *src, const char *dst, unsigned flags,
                        esp_fs_progress_cb progress, void *ctx, esp_fs_err_t *err)
{
    scratch_t *s = scratch_alloc();
    if (s == NULL)
        return fail(err, "out of memory");
    int rc = -1;
    if (esp_fs_check(src, move, s->src, sizeof s->src, err) != 0
            || esp_fs_check(dst, true, s->dst, sizeof s->dst, err) != 0)
        goto out;
    if (strcmp(s->src, "/") == 0 || root_of(s->src) < 0 || strcmp(s->src, ROOTS[root_of(s->src)].path) == 0) {
        fail(err, "%s: a storage root cannot be %s", s->src, move ? "moved" : "copied");
        goto out;
    }
    if (within(s->dst, s->src)) {
        fail(err, "%s: cannot %s a directory into itself", s->dst, move ? "move" : "copy");
        goto out;
    }

    op_t op = { .progress = progress, .ctx = ctx, .err = err, .buf = s->buf };
    bool overwrite = flags & ESP_FS_OVERWRITE;
    lock();
    struct stat st;
    if (move && root_of(s->src) == root_of(s->dst)) {
        /* Same filesystem: a rename. FAT refuses to rename onto an existing
         * name, so an allowed overwrite removes the target first -- files
         * only; replacing a whole directory by rename is not offered. */
        if (stat(s->dst, &st) == 0) {
            if (!overwrite || S_ISDIR(st.st_mode)) {
                fail(err, "%s: already exists", s->dst);
                unlock();
                goto out;
            }
            if (unlink(s->dst) != 0) {
                fail_errno(err, s->dst);
                unlock();
                goto out;
            }
        }
        rc = rename(s->src, s->dst) == 0 ? 0 : fail_errno(err, s->src);
    } else {
        rc = copy_tree(&op, s->src, s->dst, overwrite, 0);
        if (rc == 0 && move)
            rc = delete_tree(&op, s->src, 0);
    }
    unlock();
out:
    free(s);
    return rc;
}

int esp_fs_copy(const char *src, const char *dst, unsigned flags,
                esp_fs_progress_cb progress, void *ctx, esp_fs_err_t *err)
{
    return copy_or_move(false, src, dst, flags, progress, ctx, err);
}

int esp_fs_move(const char *src, const char *dst, unsigned flags,
                esp_fs_progress_cb progress, void *ctx, esp_fs_err_t *err)
{
    return copy_or_move(true, src, dst, flags, progress, ctx, err);
}

int esp_fs_delete(const char *path, esp_fs_progress_cb progress, void *ctx, esp_fs_err_t *err)
{
    scratch_t *s = scratch_alloc();
    if (s == NULL)
        return fail(err, "out of memory");
    int rc = -1;
    if (esp_fs_check(path, true, s->src, sizeof s->src, err) == 0) {
        op_t op = { .progress = progress, .ctx = ctx, .err = err, .buf = s->buf };
        lock();
        rc = delete_tree(&op, s->src, 0);
        unlock();
    }
    free(s);
    return rc;
}

int esp_fs_mkdir(const char *path, esp_fs_err_t *err)
{
    char p[PATH_MAX_LEN];
    if (esp_fs_check(path, true, p, sizeof p, err) != 0)
        return -1;
    lock();
    int rc = mkdir(p, 0777) == 0 ? 0
           : errno == EEXIST ? fail(err, "%s: already exists", p)
           : fail_errno(err, p);
    unlock();
    return rc;
}
