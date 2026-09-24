/*
 * auto/osdef.h replacement for the ESP-IDF build.
 *
 * Upstream generates this file by running the C preprocessor over the system
 * headers with osdef.sh/osdef1.h.in, to declare any libc function the platform
 * failed to declare itself. That machinery assumes a host build and cannot run
 * here, but vim.h includes the header unconditionally -- so it must exist.
 *
 * Nearly empty by design: IDF's newlib declares what it actually provides. The
 * exception is the handful of things Vim calls that IDF neither declares nor
 * defines. Declaring them here keeps Vim's own sources compiling; the
 * DEFINITIONS live in port/esp_shims.c (Phase 3).
 */

#ifndef ESP_VIM_OSDEF_H
#define ESP_VIM_OSDEF_H

#include <sys/types.h>
#include <sys/stat.h>

/*
 * Not declared anywhere in ESP-IDF v5.5.5 (Phase 1 found this the hard way:
 * `lstat` is a compile error before it is ever a link error). Vim references
 * them even with HAVE_LSTAT et al undefined, via code paths the preprocessor
 * does not always cut out.
 */
#ifndef ESP_VIM_NO_COMPAT_DECLS
int     lstat(const char *path, struct stat *buf);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int     symlink(const char *target, const char *linkpath);
int     link(const char *oldpath, const char *newpath);
#endif

/*
 * ESP-IDF ships <sys/wait.h> but declares none of its contents -- there is no
 * process model. Vim references these from mch_get_cmd_output_direct() and
 * wait4pid(), which are unreachable here (no fork(), and USE_SYSTEM routes
 * :! through system()), but are still compiled.
 *
 * Declaring them keeps Vim's sources compiling and pushes the failure to the
 * linker, where it belongs: port/esp_shims.c supplies stubs in Phase 3 and the
 * undefined-symbol list is the authoritative worklist for what Vim really needs.
 */
#ifndef WNOHANG
# define WNOHANG        1
#endif
#ifndef WIFEXITED
# define WIFEXITED(s)   (1)
# define WEXITSTATUS(s) (((s) >> 8) & 0xff)
# define WIFSIGNALED(s) (0)
# define WTERMSIG(s)    (0)
#endif
pid_t waitpid(pid_t pid, int *status, int options);

#endif /* ESP_VIM_OSDEF_H */
