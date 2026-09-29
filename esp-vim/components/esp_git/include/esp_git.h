/*
 * esp_git: the device's side of git over libgit2 -- what doesn't depend on
 * Vim. The esp_git_*() builtins (components/vim/api/esp_api_git.c) call
 * libgit2 directly, on the Vim task, and take from here:
 *
 *   - esp_git_init(): libgit2 set up once, with memory limits for a device
 *     (no mmap: a pack window is a malloc'd copy, 32 MB by default);
 *   - the remote callbacks for credentials and certificates:
 *       SSH:   the device key, /fat/.ssh/id_ecdsa (:EspSshKeygen); the
 *              server's host key checked against /fat/.ssh/known_hosts by
 *              esp_ssh's rules (trust on first use, a changed key refused);
 *       HTTPS: a user and token from NVS (namespace "esp_git", keys "user"
 *              and "token"), or the ones in the URL; certificates verified
 *              against ESP-IDF's bundle (libgit2 patch 0005);
 *   - esp_git_gc(): pack the loose objects that FAT stores so badly.
 *
 * Not thread-safe: libgit2 is built without threads, and everything here
 * runs on the Vim task.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "git2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Once; later calls do nothing. */
esp_err_t esp_git_init(void);

/*
 * State for one remote operation. Put it FIRST in the struct given as the
 * callbacks' payload: the callbacks below cast the payload to this.
 */
typedef struct {
    char url[256];
    int tries;              /* credential requests so far */
    char err[256];          /* why we refused, when we did: better than libgit2's */
} esp_git_auth_t;

void esp_git_auth_begin(esp_git_auth_t *a, const char *url);

int esp_git_credentials_cb(git_credential **out, const char *url, const char *user,
                           unsigned int allowed, void *payload);
int esp_git_certificate_cb(git_cert *cert, int valid, const char *host, void *payload);

typedef struct {
    size_t packed;          /* objects in the new pack */
    size_t loose_removed;   /* loose objects deleted (now in the pack) */
    size_t packs_removed;   /* older packs whose objects it all holds */
    bool skipped;           /* {auto}: too few loose objects to bother */
} esp_git_gc_t;

/*
 * Pack every object reachable from the refs and HEAD into one new pack, then
 * delete the loose objects it holds and the older packs it makes redundant.
 * Unreachable loose objects are left alone. {auto} > 0: only if about that
 * many loose objects exist (estimated from objects/17, as git's gc.auto).
 */
int esp_git_gc(const char *path, size_t autolimit, esp_git_gc_t *out, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
