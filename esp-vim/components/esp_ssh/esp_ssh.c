/*
 * esp_ssh: SFTP/SCP over libssh2. See include/esp_ssh.h for the contract.
 */

#include "esp_ssh.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libssh2.h"
#include "libssh2_sftp.h"

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"

#define CHUNK       16384
#define TIMEOUT_MS  20000
#define MAX_DEPTH   16

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

/* The one cached session. */
static struct {
    int sock;
    LIBSSH2_SESSION *session;
    LIBSSH2_SFTP *sftp;
    char user[64], host[128];
    int port;
} S = { .sock = -1 };

typedef struct {
    bool scp;
    char user[64], host[128];
    int port;
    char path[512];
} url_t;

esp_err_t esp_ssh_init(void)
{
    if (s_lock == NULL)
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
    return s_lock && libssh2_init(0) == 0 ? ESP_OK : ESP_FAIL;
}

static void lock(void)   { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGiveRecursive(s_lock); }

static int fail(char *err, size_t n, int code, const char *fmt, ...)
{
    if (err != NULL && n > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return code;
}

/* libssh2's own message for the last error on the session. */
static const char *ssh_msg(void)
{
    char *msg = NULL;
    if (S.session == NULL)
        return "not connected";
    libssh2_session_last_error(S.session, &msg, NULL, 0);
    return msg && *msg ? msg : "unknown error";
}

/* ------------------------------------------------------------------ url -- */

static int parse_url(const char *url, url_t *u, char *err, size_t n)
{
    memset(u, 0, sizeof *u);
    u->port = 22;
    const char *p;
    if (strncmp(url, "scp://", 6) == 0) {
        u->scp = true;
        p = url + 6;
    } else if (strncmp(url, "sftp://", 7) == 0) {
        p = url + 7;
    } else {
        return fail(err, n, -1, "%s: not an scp:// or sftp:// URL", url);
    }
    const char *slash = strchr(p, '/');
    size_t alen = slash ? (size_t)(slash - p) : strlen(p);
    char auth[200];
    if (alen == 0 || alen >= sizeof auth)
        return fail(err, n, -1, "%s: no host", url);
    memcpy(auth, p, alen);
    auth[alen] = '\0';
    char *hostpart = auth;
    char *at = strrchr(auth, '@');
    if (at != NULL) {
        *at = '\0';
        if (strlen(auth) >= sizeof u->user)
            return fail(err, n, -1, "%s: user name too long", url);
        memcpy(u->user, auth, strlen(auth) + 1);
        hostpart = at + 1;
    }
    char *colon = strpbrk(hostpart, ":#");     /* netrw allows host:port and host#port */
    if (colon != NULL) {
        *colon = '\0';
        u->port = atoi(colon + 1);
        if (u->port <= 0 || u->port > 65535)
            return fail(err, n, -1, "%s: bad port", url);
    }
    if (strlen(hostpart) >= sizeof u->host)
        return fail(err, n, -1, "%s: host name too long", url);
    memcpy(u->host, hostpart, strlen(hostpart) + 1);
    if (u->host[0] == '\0')
        return fail(err, n, -1, "%s: no host", url);
    if (u->user[0] == '\0')
        return fail(err, n, -1, "%s: no user name (write user@host)", url);
    /* netrw: "/path" is relative to the login directory, "//path" absolute. */
    snprintf(u->path, sizeof u->path, "%s", slash ? slash + 1 : "");
    return 0;
}

/* ------------------------------------------------------------- session -- */

void esp_ssh_disconnect(void)
{
    lock();
    if (S.sftp != NULL)
        libssh2_sftp_shutdown(S.sftp);
    if (S.session != NULL) {
        libssh2_session_disconnect(S.session, "bye");
        libssh2_session_free(S.session);
    }
    if (S.sock >= 0)
        close(S.sock);
    S.sftp = NULL;
    S.session = NULL;
    S.sock = -1;
    unlock();
}

static int tcp_connect(const url_t *u, char *err, size_t n)
{
    char port[8];
    snprintf(port, sizeof port, "%d", u->port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    int r = getaddrinfo(u->host, port, &hints, &res);
    if (r != 0 || res == NULL)
        return fail(err, n, -1, "%s: cannot resolve the host name", u->host);
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) {
        freeaddrinfo(res);
        return fail(err, n, -1, "socket: %s", strerror(errno));
    }
    struct timeval tv = { .tv_sec = TIMEOUT_MS / 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r != 0) {
        int e = errno;
        close(s);
        return fail(err, n, -1, "%s:%d: %s", u->host, u->port, strerror(e));
    }
    return s;
}

static const char *key_type_name(int type)
{
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return "ssh-rsa";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ecdsa-sha2-nistp256";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ecdsa-sha2-nistp384";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ecdsa-sha2-nistp521";
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return "ssh-ed25519";
    default:                             return "unknown";
    }
}

static int knownhost_keybits(int type)
{
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default:                             return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

/* Check (or, with {add}, record) the connected server's key. */
static int check_hostkey(const url_t *u, bool add, esp_ssh_hostkey_t *out, char *err, size_t n)
{
    size_t len;
    int type;
    const char *key = libssh2_session_hostkey(S.session, &len, &type);
    if (key == NULL)
        return fail(err, n, -1, "%s: no host key", u->host);

    esp_ssh_hostkey_t hk = {0};
    snprintf(hk.type, sizeof hk.type, "%s", key_type_name(type));
    const unsigned char *sha = (const unsigned char *)
        libssh2_hostkey_hash(S.session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (sha != NULL) {
        unsigned char b64[48];
        size_t olen = 0;
        mbedtls_base64_encode(b64, sizeof b64, &olen, sha, 32);
        while (olen > 0 && b64[olen - 1] == '=')
            olen--;                             /* ssh-keygen prints no padding */
        b64[olen] = '\0';
        snprintf(hk.fingerprint, sizeof hk.fingerprint, "SHA256:%s", b64);
    }

    LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(S.session);
    if (kh == NULL)
        return fail(err, n, -1, "known_hosts: out of memory");
    libssh2_knownhost_readfile(kh, ESP_SSH_KNOWN_HOSTS, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownhost_keybits(type);
    struct libssh2_knownhost *found = NULL;
    int c = libssh2_knownhost_checkp(kh, u->host, u->port, key, len, typemask, &found);
    int rc = 0;
    if (c == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
        hk.status = 0;
    } else if (c == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        hk.status = ESP_SSH_E_HOSTKEY_CHANGED;
        rc = fail(err, n, ESP_SSH_E_HOSTKEY_CHANGED,
                  "esp_ssh: host key CHANGED for %s (%s %s); remove its line from %s "
                  "only if you know why it changed", u->host, hk.type, hk.fingerprint,
                  ESP_SSH_KNOWN_HOSTS);
    } else if (add) {
        /* known_hosts writes "[host]:port" for non-22 ports, like OpenSSH. */
        char name[160];
        if (u->port == 22)
            snprintf(name, sizeof name, "%s", u->host);
        else
            snprintf(name, sizeof name, "[%s]:%d", u->host, u->port);
        mkdir(ESP_SSH_DIR, 0700);
        if (libssh2_knownhost_addc(kh, name, NULL, key, len, NULL, 0, typemask, NULL) != 0
                || libssh2_knownhost_writefile(kh, ESP_SSH_KNOWN_HOSTS,
                                               LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0)
            rc = fail(err, n, -1, "%s: could not write %s", u->host, ESP_SSH_KNOWN_HOSTS);
    } else {
        hk.status = ESP_SSH_E_HOSTKEY_UNKNOWN;
        rc = fail(err, n, ESP_SSH_E_HOSTKEY_UNKNOWN,
                  "esp_ssh: unknown host key for %s (%s %s)", u->host, hk.type, hk.fingerprint);
    }
    libssh2_knownhost_free(kh);
    if (out != NULL)
        *out = hk;
    return rc;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int authenticate(const url_t *u, const esp_ssh_auth_t *auth, char *err, size_t n)
{
    const char *keys[] = { auth && auth->keyfile ? auth->keyfile : NULL,
                           ESP_SSH_DIR "/id_ecdsa", ESP_SSH_DIR "/id_rsa" };
    char why[128] = "no key file";
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (keys[i] == NULL || !file_exists(keys[i]))
            continue;
        /* Pass the public half when it is there: libssh2's mbedTLS backend
         * cannot always derive it from an EC private key. */
        char pubfile[520];
        snprintf(pubfile, sizeof pubfile, "%s.pub", keys[i]);
        if (libssh2_userauth_publickey_fromfile(S.session, u->user,
                                                file_exists(pubfile) ? pubfile : NULL,
                                                keys[i], NULL) == 0)
            return 0;
        snprintf(why, sizeof why, "%s: %s", keys[i], ssh_msg());
        if (auth && auth->keyfile)
            break;                              /* an explicit key only */
    }
    if (auth && auth->password && *auth->password
            && libssh2_userauth_password(S.session, u->user, auth->password) == 0)
        return 0;
    return fail(err, n, ESP_SSH_E_AUTH, "esp_ssh: authentication failed for %s@%s (%s)",
                u->user, u->host, why);
}

/*
 * A connected session for {u}: the cached one if it is for the same
 * user@host:port, otherwise a new one. {stage}: 0 = through the host-key check
 * only (no auth), 1 = authenticated.
 */
static int open_session(const url_t *u, const esp_ssh_auth_t *auth, int stage,
                        bool trust, esp_ssh_hostkey_t *hk, char *err, size_t n)
{
    if (S.session != NULL && stage == 1 && strcmp(S.user, u->user) == 0
            && strcmp(S.host, u->host) == 0 && S.port == u->port)
        return 0;
    esp_ssh_disconnect();

    S.sock = tcp_connect(u, err, n);
    if (S.sock < 0)
        return -1;
    S.session = libssh2_session_init();
    if (S.session == NULL) {
        esp_ssh_disconnect();
        return fail(err, n, -1, "libssh2: out of memory");
    }
    libssh2_session_set_blocking(S.session, 1);
    libssh2_session_set_timeout(S.session, TIMEOUT_MS);
    if (libssh2_session_handshake(S.session, S.sock) != 0) {
        int rc = fail(err, n, -1, "%s:%d: SSH handshake failed: %s", u->host, u->port, ssh_msg());
        esp_ssh_disconnect();
        return rc;
    }
    int rc = check_hostkey(u, trust, hk, err, n);
    if (rc != 0 || stage == 0) {
        esp_ssh_disconnect();
        return rc;
    }
    rc = authenticate(u, auth, err, n);
    if (rc != 0) {
        esp_ssh_disconnect();
        return rc;
    }
    snprintf(S.user, sizeof S.user, "%s", u->user);
    snprintf(S.host, sizeof S.host, "%s", u->host);
    S.port = u->port;
    return 0;
}

static int need_sftp(char *err, size_t n)
{
    if (S.sftp == NULL)
        S.sftp = libssh2_sftp_init(S.session);
    if (S.sftp == NULL)
        return fail(err, n, -1, "SFTP unavailable: %s", ssh_msg());
    return 0;
}

/* Connect and authenticate for {url}, parsing it into {u}. */
static int begin(const char *url, url_t *u, const esp_ssh_auth_t *auth, bool sftp,
                 char *err, size_t n)
{
    int rc = parse_url(url, u, err, n);
    if (rc == 0)
        rc = open_session(u, auth, 1, false, NULL, err, n);
    if (rc == 0 && sftp)
        rc = need_sftp(err, n);
    return rc;
}

static void *chunk_alloc(void)
{
    void *b = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return b ? b : malloc(CHUNK);
}

/* After a failure mid-operation the session may be unusable: drop it, so the
 * next call reconnects rather than failing the same way. */
static int drop(int rc)
{
    esp_ssh_disconnect();
    return rc;
}

/* ------------------------------------------------------------- host key -- */

int esp_ssh_hostkey(const char *url, esp_ssh_hostkey_t *key, char *err, size_t errlen)
{
    url_t u;
    lock();
    int rc = parse_url(url, &u, err, errlen);
    if (rc == 0) {
        rc = open_session(&u, NULL, 0, false, key, err, errlen);
        if (rc == ESP_SSH_E_HOSTKEY_UNKNOWN || rc == ESP_SSH_E_HOSTKEY_CHANGED)
            rc = 0;                             /* reported in key->status */
    }
    unlock();
    return rc;
}

int esp_ssh_trust(const char *url, char *err, size_t errlen)
{
    url_t u;
    lock();
    int rc = parse_url(url, &u, err, errlen);
    if (rc == 0)
        rc = open_session(&u, NULL, 0, true, NULL, err, errlen);
    unlock();
    return rc;
}

/* ------------------------------------------------------------ transfers -- */

int esp_ssh_get(const char *url, const char *dest, const esp_ssh_auth_t *auth,
                esp_ssh_progress_cb progress, void *ctx, uint64_t *size, char *err, size_t errlen)
{
    char path[512], part[520];
    esp_fs_err_t ferr;
    if (esp_fs_check(dest, true, path, sizeof path, &ferr) != 0)
        return fail(err, errlen, -1, "%s", ferr.msg);
    snprintf(part, sizeof part, "%s.part", path);

    url_t u;
    lock();
    int rc = begin(url, &u, auth, false, err, errlen);
    if (rc != 0) {
        unlock();
        return rc;
    }
    uint64_t done = 0, total = 0;
    LIBSSH2_CHANNEL *ch = NULL;
    LIBSSH2_SFTP_HANDLE *fh = NULL;
    if (u.scp) {
        libssh2_struct_stat st;
        ch = libssh2_scp_recv2(S.session, u.path, &st);
        if (ch == NULL) {
            rc = fail(err, errlen, -1, "%s: %s", url, ssh_msg());
            unlock();
            return drop(rc);
        }
        total = (uint64_t)st.st_size;
    } else {
        if ((rc = need_sftp(err, errlen)) != 0) {
            unlock();
            return rc;
        }
        fh = libssh2_sftp_open(S.sftp, u.path, LIBSSH2_FXF_READ, 0);
        if (fh == NULL) {
            rc = fail(err, errlen, -1, "%s: cannot open (%s)", url, ssh_msg());
            unlock();
            return rc;
        }
    }

    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    char *buf = chunk_alloc();
    if (fd < 0 || buf == NULL) {
        rc = fail(err, errlen, -1, "%s: %s", part, fd < 0 ? strerror(errno) : "out of memory");
    } else {
        for (;;) {
            size_t want = CHUNK;
            if (u.scp) {
                if (done >= total)
                    break;
                if (total - done < want)
                    want = (size_t)(total - done);
            }
            ssize_t r = u.scp ? libssh2_channel_read(ch, buf, want)
                              : libssh2_sftp_read(fh, buf, want);
            if (r < 0) {
                rc = fail(err, errlen, -1, "%s: read failed after %llu bytes (%s)", url,
                          (unsigned long long)done, ssh_msg());
                break;
            }
            if (r == 0)
                break;
            if (write(fd, buf, (size_t)r) != r) {
                rc = fail(err, errlen, -1, "%s: %s", path, strerror(errno));
                break;
            }
            done += (uint64_t)r;
            if (progress && !progress(ctx, done)) {
                rc = fail(err, errlen, -1, "interrupted");
                break;
            }
        }
    }
    free(buf);
    if (fd >= 0 && close(fd) != 0 && rc == 0)
        rc = fail(err, errlen, -1, "%s: %s", part, strerror(errno));
    if (fh)
        libssh2_sftp_close(fh);
    if (ch)
        libssh2_channel_free(ch);
    if (rc == 0) {
        unlink(path);
        if (rename(part, path) != 0)
            rc = fail(err, errlen, -1, "%s: %s", path, strerror(errno));
    }
    if (rc != 0)
        unlink(part);
    if (size)
        *size = done;
    unlock();
    return rc != 0 && ch != NULL ? drop(rc) : rc;   /* an aborted SCP channel is not reusable */
}

int esp_ssh_put(const char *src, const char *url, const esp_ssh_auth_t *auth,
                esp_ssh_progress_cb progress, void *ctx, uint64_t *size, char *err, size_t errlen)
{
    char path[512];
    esp_fs_err_t ferr;
    if (esp_fs_check(src, false, path, sizeof path, &ferr) != 0)
        return fail(err, errlen, -1, "%s", ferr.msg);
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode))
        return fail(err, errlen, -1, "%s: %s", path, S_ISDIR(st.st_mode) ? "is a directory" : strerror(errno));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fail(err, errlen, -1, "%s: %s", path, strerror(errno));

    url_t u;
    lock();
    int rc = begin(url, &u, auth, false, err, errlen);
    LIBSSH2_CHANNEL *ch = NULL;
    LIBSSH2_SFTP_HANDLE *fh = NULL;
    if (rc == 0) {
        if (u.scp) {
            ch = libssh2_scp_send64(S.session, u.path, 0644, (libssh2_int64_t)st.st_size, 0, 0);
            if (ch == NULL)
                rc = fail(err, errlen, -1, "%s: %s", url, ssh_msg());
        } else if ((rc = need_sftp(err, errlen)) == 0) {
            fh = libssh2_sftp_open(S.sftp, u.path,
                                   LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                                   LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR
                                   | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
            if (fh == NULL)
                rc = fail(err, errlen, -1, "%s: cannot create (%s)", url, ssh_msg());
        }
    }
    uint64_t done = 0;
    char *buf = rc == 0 ? chunk_alloc() : NULL;
    if (rc == 0 && buf == NULL)
        rc = fail(err, errlen, -1, "out of memory");
    while (rc == 0) {
        ssize_t r = read(fd, buf, CHUNK);
        if (r < 0) {
            rc = fail(err, errlen, -1, "%s: %s", path, strerror(errno));
            break;
        }
        if (r == 0)
            break;
        for (ssize_t off = 0; off < r && rc == 0; ) {
            ssize_t w = u.scp ? libssh2_channel_write(ch, buf + off, (size_t)(r - off))
                              : libssh2_sftp_write(fh, buf + off, (size_t)(r - off));
            if (w < 0)
                rc = fail(err, errlen, -1, "%s: write failed after %llu bytes (%s)", url,
                          (unsigned long long)done, ssh_msg());
            else
                off += w;
        }
        done += (uint64_t)r;
        if (rc == 0 && progress && !progress(ctx, done))
            rc = fail(err, errlen, -1, "interrupted");
    }
    free(buf);
    close(fd);
    if (fh)
        libssh2_sftp_close(fh);
    if (ch) {
        if (rc == 0) {
            libssh2_channel_send_eof(ch);
            libssh2_channel_wait_eof(ch);
            libssh2_channel_wait_closed(ch);
        }
        libssh2_channel_free(ch);
    }
    if (size)
        *size = done;
    unlock();
    return rc != 0 && ch != NULL ? drop(rc) : rc;
}

/* ------------------------------------------------------ directory work -- */

int esp_ssh_list(const char *url, const esp_ssh_auth_t *auth,
                 esp_fs_entry_cb cb, void *ctx, char *err, size_t errlen)
{
    url_t u;
    lock();
    int rc = begin(url, &u, auth, true, err, errlen);
    if (rc != 0) {
        unlock();
        return rc;
    }
    LIBSSH2_SFTP_HANDLE *dh = libssh2_sftp_opendir(S.sftp, u.path[0] ? u.path : ".");
    if (dh == NULL) {
        rc = fail(err, errlen, -1, "%s: cannot list (%s)", url, ssh_msg());
        unlock();
        return rc;
    }
    char name[512];
    LIBSSH2_SFTP_ATTRIBUTES a;
    for (;;) {
        int r = libssh2_sftp_readdir(dh, name, sizeof name, &a);
        if (r < 0) {
            rc = fail(err, errlen, -1, "%s: %s", url, ssh_msg());
            break;
        }
        if (r == 0)
            break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        esp_fs_entry_t e = {
            .name = name,
            .dir = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(a.permissions),
            .size = (a.flags & LIBSSH2_SFTP_ATTR_SIZE) ? a.filesize : 0,
            .mtime = (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? (time_t)a.mtime : 0,
        };
        if (e.dir)
            e.size = 0;
        if (!cb(ctx, &e))
            break;
    }
    libssh2_sftp_closedir(dh);
    unlock();
    return rc;
}

int esp_ssh_mkdir(const char *url, const esp_ssh_auth_t *auth, char *err, size_t errlen)
{
    url_t u;
    lock();
    int rc = begin(url, &u, auth, true, err, errlen);
    if (rc == 0 && libssh2_sftp_mkdir(S.sftp, u.path,
                                      LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP
                                      | LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH) != 0)
        rc = fail(err, errlen, -1, "%s: cannot create (%s)", url, ssh_msg());
    unlock();
    return rc;
}

static int remove_tree(char *path, int depth, esp_ssh_progress_cb progress, void *ctx,
                       char *err, size_t n)
{
    LIBSSH2_SFTP_ATTRIBUTES a;
    if (libssh2_sftp_lstat(S.sftp, path, &a) != 0)
        return fail(err, n, -1, "%s: %s", path, ssh_msg());
    bool dir = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(a.permissions);
    if (!dir) {
        if (libssh2_sftp_unlink(S.sftp, path) != 0)
            return fail(err, n, -1, "%s: cannot delete (%s)", path, ssh_msg());
        return progress && !progress(ctx, 0) ? fail(err, n, -1, "interrupted") : 0;
    }
    if (depth >= MAX_DEPTH)
        return fail(err, n, -1, "%s: nested too deeply", path);
    LIBSSH2_SFTP_HANDLE *dh = libssh2_sftp_opendir(S.sftp, path);
    if (dh == NULL)
        return fail(err, n, -1, "%s: cannot list (%s)", path, ssh_msg());
    /* Collect names first: removing while reading a directory is unsafe. */
    size_t cap = 16, count = 0;
    char **names = malloc(cap * sizeof *names);
    char name[512];
    int rc = names ? 0 : fail(err, n, -1, "out of memory");
    while (rc == 0) {
        int r = libssh2_sftp_readdir(dh, name, sizeof name, &a);
        if (r <= 0)
            break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (count == cap) {
            char **grown = realloc(names, 2 * cap * sizeof *names);
            if (grown == NULL) {
                rc = fail(err, n, -1, "out of memory");
                break;
            }
            names = grown;
            cap *= 2;
        }
        names[count++] = strdup(name);
    }
    libssh2_sftp_closedir(dh);
    size_t base = strlen(path);
    for (size_t i = 0; i < count; i++) {
        if (rc == 0) {
            if (base + 1 + strlen(names[i]) + 1 > 512)
                rc = fail(err, n, -1, "%s/%s: path too long", path, names[i]);
            else {
                path[base] = '/';
                strcpy(path + base + 1, names[i]);
                rc = remove_tree(path, depth + 1, progress, ctx, err, n);
                path[base] = '\0';
            }
        }
        free(names[i]);
    }
    free(names);
    if (rc == 0 && libssh2_sftp_rmdir(S.sftp, path) != 0)
        rc = fail(err, n, -1, "%s: cannot remove (%s)", path, ssh_msg());
    return rc;
}

int esp_ssh_remove(const char *url, const esp_ssh_auth_t *auth,
                   esp_ssh_progress_cb progress, void *ctx, char *err, size_t errlen)
{
    url_t u;
    lock();
    int rc = begin(url, &u, auth, true, err, errlen);
    if (rc == 0 && (u.path[0] == '\0' || strcmp(u.path, "/") == 0 || strcmp(u.path, ".") == 0))
        rc = fail(err, errlen, -1, "%s: refusing to delete a home or root directory", url);
    if (rc == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s", u.path);
        size_t l = strlen(path);
        while (l > 1 && path[l - 1] == '/')
            path[--l] = '\0';
        rc = remove_tree(path, 0, progress, ctx, err, errlen);
    }
    unlock();
    return rc;
}

int esp_ssh_rename(const char *url, const char *newpath, const esp_ssh_auth_t *auth,
                   char *err, size_t errlen)
{
    url_t u;
    lock();
    int rc = begin(url, &u, auth, true, err, errlen);
    if (rc == 0 && libssh2_sftp_rename_ex(S.sftp, u.path, strlen(u.path), newpath, strlen(newpath),
                                          LIBSSH2_SFTP_RENAME_OVERWRITE | LIBSSH2_SFTP_RENAME_ATOMIC
                                          | LIBSSH2_SFTP_RENAME_NATIVE) != 0)
        rc = fail(err, errlen, -1, "%s: cannot rename to %s (%s)", url, newpath, ssh_msg());
    unlock();
    return rc;
}

/* --------------------------------------------------------------- keygen -- */

static int rng(void *ctx, unsigned char *buf, size_t len)
{
    esp_fill_random(buf, len);
    return 0;
}

/* Append an SSH wire-format string (uint32 length, bytes). */
static size_t put_string(unsigned char *out, const void *data, size_t len)
{
    out[0] = (unsigned char)(len >> 24);
    out[1] = (unsigned char)(len >> 16);
    out[2] = (unsigned char)(len >> 8);
    out[3] = (unsigned char)len;
    memcpy(out + 4, data, len);
    return 4 + len;
}

int esp_ssh_keygen(const char *path, const char *comment, char *pub, size_t publen,
                   char *err, size_t errlen)
{
    char priv[512], pubpath[520];
    esp_fs_err_t ferr;
    if (esp_fs_check(path, true, priv, sizeof priv, &ferr) != 0)
        return fail(err, errlen, -1, "%s", ferr.msg);
    snprintf(pubpath, sizeof pubpath, "%s.pub", priv);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = -1;
    unsigned char *pem = malloc(1024);
    if (pem == NULL) {
        fail(err, errlen, -1, "out of memory");
        goto out;
    }
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0
            || mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), rng, NULL) != 0) {
        fail(err, errlen, -1, "key generation failed");
        goto out;
    }
    if (mbedtls_pk_write_key_pem(&pk, pem, 1024) != 0) {
        fail(err, errlen, -1, "could not encode the private key");
        goto out;
    }

    /* Public key blob: string "ecdsa-sha2-nistp256", string "nistp256", string Q. */
    unsigned char q[65], blob[4 + 19 + 4 + 8 + 4 + 65];
    size_t qlen = 0;
    mbedtls_ecp_keypair *kp = mbedtls_pk_ec(pk);
    if (mbedtls_ecp_point_write_binary(&kp->MBEDTLS_PRIVATE(grp), &kp->MBEDTLS_PRIVATE(Q),
                                       MBEDTLS_ECP_PF_UNCOMPRESSED, &qlen, q, sizeof q) != 0) {
        fail(err, errlen, -1, "could not encode the public key");
        goto out;
    }
    size_t bl = put_string(blob, "ecdsa-sha2-nistp256", 19);
    bl += put_string(blob + bl, "nistp256", 8);
    bl += put_string(blob + bl, q, qlen);
    unsigned char b64[200];
    size_t b64len = 0;
    mbedtls_base64_encode(b64, sizeof b64, &b64len, blob, bl);
    b64[b64len] = '\0';
    snprintf(pub, publen, "ecdsa-sha2-nistp256 %s %s", b64, comment ? comment : "esp-vim");

    mkdir(ESP_SSH_DIR, 0700);
    int fd = open(priv, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t plen = strlen((char *)pem);
    if (fd < 0 || write(fd, pem, plen) != (ssize_t)plen) {
        if (fd >= 0)
            close(fd);
        fail(err, errlen, -1, "%s: %s", priv, strerror(errno));
        goto out;
    }
    close(fd);
    fd = open(pubpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t ulen = strlen(pub);
    if (fd < 0 || write(fd, pub, ulen) != (ssize_t)ulen || write(fd, "\n", 1) != 1) {
        if (fd >= 0)
            close(fd);
        fail(err, errlen, -1, "%s: %s", pubpath, strerror(errno));
        goto out;
    }
    close(fd);
    rc = 0;
out:
    if (pem) {
        memset(pem, 0, 1024);                   /* private key material */
        free(pem);
    }
    mbedtls_pk_free(&pk);
    return rc;
}
