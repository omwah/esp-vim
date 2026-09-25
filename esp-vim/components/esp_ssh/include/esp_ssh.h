/*
 * esp_ssh: SFTP and SCP over libssh2 (the skuodi/libssh2_esp component, mbedTLS
 * backend). Behind :EspFiles' remote panes, netrw's scp:// and sftp:// reads
 * and writes, and the esp_ssh_*() Vim builtins.
 *
 * URLs follow netrw's convention, so the same text works in :e and here:
 *
 *     scp://[user@]host[:port]/path        path relative to the login directory
 *     scp://[user@]host[:port]//abs/path   absolute path
 *     sftp://...                           the same
 *
 * The scheme picks the protocol for get/put (SCP or SFTP). Listing, mkdir,
 * remove and rename always use SFTP, which SCP cannot do.
 *
 * Trust: host keys are checked against /fat/.ssh/known_hosts (OpenSSH format).
 * An unknown host fails with ESP_SSH_E_HOSTKEY_UNKNOWN until esp_ssh_trust()
 * records its key: trust on first use, with the fingerprint shown to the user
 * by the caller. A changed key fails with ESP_SSH_E_HOSTKEY_CHANGED and is
 * never accepted automatically.
 *
 * Auth: public keys from /fat/.ssh/id_ecdsa, then /fat/.ssh/id_rsa (or the
 * file given), then the password if one is given. libssh2's mbedTLS backend
 * has no Ed25519, so device keys are ECDSA P-256 (esp_ssh_keygen()).
 *
 * One session is kept open and reused for the same user@host:port. All calls
 * are serialised by a mutex; this component knows nothing about Vim.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_SSH_DIR         "/fat/.ssh"
#define ESP_SSH_KNOWN_HOSTS ESP_SSH_DIR "/known_hosts"

/* Error codes, beyond -1 (other failures). The message is in the err buffer. */
#define ESP_SSH_E_HOSTKEY_UNKNOWN  (-2)
#define ESP_SSH_E_HOSTKEY_CHANGED  (-3)
#define ESP_SSH_E_AUTH             (-4)

typedef struct {
    const char *password;       /* NULL: keys only */
    const char *keyfile;        /* NULL: /fat/.ssh/id_ecdsa, then id_rsa */
} esp_ssh_auth_t;

typedef struct {
    char type[24];              /* "ecdsa-sha2-nistp256", "ssh-rsa", ... */
    char fingerprint[64];       /* "SHA256:<base64>", as ssh-keygen -l prints */
    int status;                 /* 0 known, ESP_SSH_E_HOSTKEY_UNKNOWN/_CHANGED */
} esp_ssh_hostkey_t;

typedef bool (*esp_ssh_progress_cb)(void *ctx, uint64_t bytes);

esp_err_t esp_ssh_init(void);

/* Connect far enough to see the host key; never authenticates. */
int esp_ssh_hostkey(const char *url, esp_ssh_hostkey_t *key, char *err, size_t errlen);

/* Record the host's current key in known_hosts (replacing none: a changed key
 * must be removed from known_hosts by hand first). */
int esp_ssh_trust(const char *url, char *err, size_t errlen);

/* Remote file -> local {dest} (validated by esp_fs; ".part" then rename). */
int esp_ssh_get(const char *url, const char *dest, const esp_ssh_auth_t *auth,
                esp_ssh_progress_cb progress, void *ctx, uint64_t *size, char *err, size_t errlen);

/* Local {src} (validated by esp_fs) -> remote file. */
int esp_ssh_put(const char *src, const char *url, const esp_ssh_auth_t *auth,
                esp_ssh_progress_cb progress, void *ctx, uint64_t *size, char *err, size_t errlen);

/* Entries of a remote directory ("." and ".." left out), as esp_fs lists them. */
int esp_ssh_list(const char *url, const esp_ssh_auth_t *auth,
                 esp_fs_entry_cb cb, void *ctx, char *err, size_t errlen);

int esp_ssh_mkdir(const char *url, const esp_ssh_auth_t *auth, char *err, size_t errlen);

/* A file, or a directory and everything in it. */
int esp_ssh_remove(const char *url, const esp_ssh_auth_t *auth,
                   esp_ssh_progress_cb progress, void *ctx, char *err, size_t errlen);

/* Rename within the same host; {newpath} is a remote path, as in a URL. */
int esp_ssh_rename(const char *url, const char *newpath, const esp_ssh_auth_t *auth,
                   char *err, size_t errlen);

/*
 * Generate an ECDSA P-256 key pair: {path} (PEM, private) and {path}.pub
 * (OpenSSH one-line public key, for a server's authorized_keys), comment
 * {comment}. The public line is also returned in {pub}.
 */
int esp_ssh_keygen(const char *path, const char *comment, char *pub, size_t publen,
                   char *err, size_t errlen);

/* Close the cached session, if any. */
void esp_ssh_disconnect(void);

#ifdef __cplusplus
}
#endif
