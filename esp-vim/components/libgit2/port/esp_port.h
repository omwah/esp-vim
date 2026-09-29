/*
 * Included first in every libgit2 source file (-include): what ESP-IDF lacks
 * or names differently.
 */
#pragma once

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xdiff_rename.h"

/*
 * Memory from PSRAM first. ESP-IDF gives every malloc() under 16 KB
 * (SPIRAM_MALLOC_ALWAYSINTERNAL) internal RAM, and a clone makes thousands
 * of small ones: on the ESP32-S3, with WiFi up, they took all of it and TLS
 * then failed for want of a socket buffer. libgit2's allocator and its
 * zlib both call these names.
 */
#include <stdlib.h>
#include "sdkconfig.h"
#if CONFIG_SPIRAM
#include "esp_heap_caps.h"
#define ESP_GIT_PSRAM   (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
static inline void *esp_git_malloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, ESP_GIT_PSRAM, MALLOC_CAP_DEFAULT);
}
static inline void *esp_git_calloc(size_t n, size_t size)
{
    return heap_caps_calloc_prefer(n, size, 2, ESP_GIT_PSRAM, MALLOC_CAP_DEFAULT);
}
static inline void *esp_git_realloc(void *p, size_t n)
{
    return heap_caps_realloc_prefer(p, n, 2, ESP_GIT_PSRAM, MALLOC_CAP_DEFAULT);
}
#define malloc  esp_git_malloc
#define calloc  esp_git_calloc
#define realloc esp_git_realloc
#endif

/* Vim's 'write' option is a global p_write too. */
#define p_write git2_p_write

/* FAT has no symbolic links: lstat is stat, and making or reading a link
 * fails (libgit2 then writes links as plain files, core.symlinks=false). */
static inline int esp_git_lstat(const char *path, struct stat *st)
{
    return stat(path, st);
}
#define lstat esp_git_lstat

static inline ssize_t esp_git_readlink(const char *path, char *buf, size_t len)
{
    (void)path; (void)buf; (void)len;
    errno = ENOSYS;
    return -1;
}
#define readlink esp_git_readlink

static inline int esp_git_symlink(const char *target, const char *path)
{
    (void)target; (void)path;
    errno = ENOSYS;
    return -1;
}
#define symlink esp_git_symlink

/* Defined in esp_port.c. */
#include <pwd.h>
#include <sys/time.h>
int esp_git_getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t len,
                       struct passwd **result);
#define getpwuid_r esp_git_getpwuid_r
int esp_git_utimes(const char *path, const struct timeval tv[2]);
#define utimes esp_git_utimes
pid_t esp_git_getppid(void);
#define getppid esp_git_getppid
pid_t esp_git_getpgid(pid_t pid);
#define getpgid esp_git_getpgid
pid_t esp_git_getsid(pid_t pid);
#define getsid esp_git_getsid

/* lwIP has getaddrinfo() but no gai_strerror(). */
static inline const char *esp_git_gai_strerror(int err)
{
    (void)err;
    return "host name lookup failed";
}
#define gai_strerror esp_git_gai_strerror
