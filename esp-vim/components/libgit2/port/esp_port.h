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
