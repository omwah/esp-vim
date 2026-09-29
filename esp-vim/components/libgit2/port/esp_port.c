/*
 * What libgit2 calls that ESP-IDF doesn't have (see esp_port.h, which maps
 * the names here). No users, processes or process groups; file times through
 * utime(), which FAT keeps to two seconds.
 */

#include <errno.h>
#include <pwd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <utime.h>

/* No password database: libgit2 then finds home in $HOME (/fat). */
int esp_git_getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t len,
                       struct passwd **result)
{
    (void)uid; (void)pwd; (void)buf; (void)len;
    *result = NULL;
    return ENOENT;
}

int esp_git_utimes(const char *path, const struct timeval tv[2])
{
    struct utimbuf t;
    if (tv == NULL)
        return utime(path, NULL);
    t.actime = tv[0].tv_sec;
    t.modtime = tv[1].tv_sec;
    return utime(path, &t);
}

/* Only mixed into a random seed (getentropy() supplies the real one). */
pid_t esp_git_getppid(void)       { return 0; }
pid_t esp_git_getpgid(pid_t pid)  { (void)pid; return 0; }
pid_t esp_git_getsid(pid_t pid)   { (void)pid; return 0; }
