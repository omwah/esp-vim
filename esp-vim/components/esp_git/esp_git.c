/*
 * esp_git: see include/esp_git.h.
 */

#include "esp_git.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_ssh.h"
#include "libssh2.h"
#include "nvs.h"

#define KEYFILE     ESP_SSH_DIR "/id_ecdsa"
#define NVS_NS      "esp_git"

static bool s_ready;

esp_err_t esp_git_init(void)
{
    if (s_ready)
        return ESP_OK;
    if (git_libgit2_init() < 0)
        return ESP_FAIL;
    /*
     * Without mmap a pack "window" is a malloc'd copy of that much of the
     * pack, and the 32-bit default is 32 MB; the object cache may hold 256
     * MB. Sizes for a device (stage 6z measured a 620 KB pack's clone at 2 MB
     * of heap with these).
     */
    git_libgit2_opts(GIT_OPT_SET_MWINDOW_SIZE, (size_t)256 * 1024);
    git_libgit2_opts(GIT_OPT_SET_MWINDOW_MAPPED_LIMIT, (size_t)1024 * 1024);
    git_libgit2_opts(GIT_OPT_SET_CACHE_MAX_SIZE, (ssize_t)1024 * 1024);
    s_ready = true;
    return ESP_OK;
}

void esp_git_auth_begin(esp_git_auth_t *a, const char *url)
{
    memset(a, 0, sizeof *a);
    snprintf(a->url, sizeof a->url, "%s", url ? url : "");
}

/* ---- credentials ---- */

static bool nvs_str(const char *key, char *buf, size_t len)
{
    nvs_handle_t h;
    buf[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t n = len;
    bool ok = nvs_get_str(h, key, buf, &n) == ESP_OK && buf[0] != '\0';
    nvs_close(h);
    return ok;
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int esp_git_credentials_cb(git_credential **out, const char *url, const char *user,
                           unsigned int allowed, void *payload)
{
    esp_git_auth_t *a = payload;
    (void)url;
    if (a->tries++ > 0) {
        /* The first answer was refused: say what to check, and stop. */
        if (allowed & GIT_CREDENTIAL_SSH_KEY)
            snprintf(a->err, sizeof a->err, "SSH authentication failed: is the device's key "
                     "(" KEYFILE ".pub, :EspSshKeygen) in the server's authorized_keys?");
        else
            snprintf(a->err, sizeof a->err, "HTTPS authentication failed: check the user and "
                     "token (:EspGitCredential)");
        return -1;
    }
    if (allowed & GIT_CREDENTIAL_USERNAME)
        return git_credential_username_new(out, user && *user ? user : "git");
    if (allowed & GIT_CREDENTIAL_SSH_KEY) {
        if (!exists(KEYFILE)) {
            snprintf(a->err, sizeof a->err, "no SSH key: make one with :EspSshKeygen");
            return -1;
        }
        /* The public half too: libssh2's mbedTLS backend can't derive it. */
        return git_credential_ssh_key_new(out, user && *user ? user : "git",
                                          exists(KEYFILE ".pub") ? KEYFILE ".pub" : NULL,
                                          KEYFILE, NULL);
    }
    if (allowed & GIT_CREDENTIAL_USERPASS_PLAINTEXT) {
        char nuser[64], token[160];
        nvs_str("user", nuser, sizeof nuser);
        if (!nvs_str("token", token, sizeof token)) {
            snprintf(a->err, sizeof a->err, "this server wants a user and token: "
                     ":EspGitCredential {user} (it asks for the token)");
            return -1;
        }
        int rc = git_credential_userpass_plaintext_new(out,
                     user && *user ? user : nuser[0] ? nuser : "git", token);
        memset(token, 0, sizeof token);
        return rc;
    }
    snprintf(a->err, sizeof a->err, "the server asks for a kind of login this device lacks");
    return -1;
}

/* ---- certificates ---- */

/* The port in an ssh:// URL; 22 otherwise (scp-like user@host:path too). */
static int url_port(const char *url)
{
    const char *p = strstr(url, "://");
    if (p == NULL)
        return 22;
    p += 3;
    const char *end = strchr(p, '/');
    const char *at = strchr(p, '@');
    if (at != NULL && (end == NULL || at < end))
        p = at + 1;
    if (*p == '[') {                                    /* [v6]:port */
        p = strchr(p, ']');
        if (p == NULL)
            return 22;
    }
    const char *colon = strchr(p, ':');
    if (colon == NULL || (end != NULL && colon > end))
        return 22;
    int port = atoi(colon + 1);
    return port > 0 ? port : 22;
}

static int libssh2_type(git_cert_ssh_raw_type_t t)
{
    switch (t) {
    case GIT_CERT_SSH_RAW_TYPE_RSA:          return LIBSSH2_HOSTKEY_TYPE_RSA;
    case GIT_CERT_SSH_RAW_TYPE_DSS:          return LIBSSH2_HOSTKEY_TYPE_DSS;
    case GIT_CERT_SSH_RAW_TYPE_KEY_ECDSA_256: return LIBSSH2_HOSTKEY_TYPE_ECDSA_256;
    case GIT_CERT_SSH_RAW_TYPE_KEY_ECDSA_384: return LIBSSH2_HOSTKEY_TYPE_ECDSA_384;
    case GIT_CERT_SSH_RAW_TYPE_KEY_ECDSA_521: return LIBSSH2_HOSTKEY_TYPE_ECDSA_521;
    case GIT_CERT_SSH_RAW_TYPE_KEY_ED25519:  return LIBSSH2_HOSTKEY_TYPE_ED25519;
    default:                                 return LIBSSH2_HOSTKEY_TYPE_UNKNOWN;
    }
}

int esp_git_certificate_cb(git_cert *cert, int valid, const char *host, void *payload)
{
    esp_git_auth_t *a = payload;
    (void)valid;
    if (cert->cert_type != GIT_CERT_HOSTKEY_LIBSSH2)
        return GIT_PASSTHROUGH;         /* TLS: libgit2's verdict (the bundle's) */
    const git_cert_hostkey *hk = (const git_cert_hostkey *)cert;
    if (!(hk->type & GIT_CERT_SSH_RAW)) {
        snprintf(a->err, sizeof a->err, "%s: the SSH host key is not available to check", host);
        return -1;
    }
    /* esp_ssh's messages: "unknown host key for ..." / "host key CHANGED for
     * ...", which the Vim side recognises (autoload/esp/git.vim). */
    return esp_ssh_check_hostkey(host, url_port(a->url), hk->hostkey, hk->hostkey_len,
                                 libssh2_type(hk->raw_type), a->err, sizeof a->err) == 0 ? 0 : -1;
}

/* ---- gc ---- */

static int fail(char *err, size_t n, const char *what)
{
    const git_error *e = git_error_last();
    snprintf(err, n, "%s%s%s", what, e ? ": " : "", e ? e->message : "");
    return -1;
}

/* {a}{b}{c} into {out}; false if it doesn't fit. */
static bool join(char *out, size_t n, const char *a, const char *b, const char *c)
{
    int r = snprintf(out, n, "%s%s%s", a, b, c);
    return r >= 0 && (size_t)r < n;
}

static bool is_hex(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return s[n] == '\0';
}

static size_t count_loose(const char *dir)
{
    DIR *d = opendir(dir);
    size_t n = 0;
    struct dirent *e;
    if (d == NULL)
        return 0;
    while ((e = readdir(d)) != NULL)
        if (strlen(e->d_name) == GIT_OID_SHA1_HEXSIZE - 2 && is_hex(e->d_name, GIT_OID_SHA1_HEXSIZE - 2))
            n++;
    closedir(d);
    return n;
}

typedef struct {
    git_odb *in;        /* the new pack */
    bool all;
} cover_t;

static int covered_cb(const git_oid *id, void *payload)
{
    cover_t *c = payload;
    if (!git_odb_exists(c->in, id)) {
        c->all = false;
        return 1;                       /* stop */
    }
    return 0;
}

static git_odb *pack_odb(const char *idx)
{
    git_odb *odb = NULL;
    git_odb_backend *be = NULL;
    if (git_odb_new(&odb) < 0)
        return NULL;
    if (git_odb_backend_one_pack(&be, idx) < 0 || git_odb_add_backend(odb, be, 1) < 0) {
        git_odb_free(odb);
        return NULL;
    }
    return odb;
}

int esp_git_gc(const char *path, size_t autolimit, esp_git_gc_t *out, char *err, size_t errlen)
{
    git_repository *repo = NULL;
    git_packbuilder *pb = NULL;
    git_revwalk *walk = NULL;
    git_odb *odb = NULL;
    char objdir[200], packdir[220], idx[300], name[GIT_OID_SHA1_HEXSIZE + 1] = "";
    int rc = -1;

    memset(out, 0, sizeof *out);
    if (git_repository_open_ext(&repo, path, 0, NULL) < 0)
        return fail(err, errlen, "not a git repository");
    snprintf(objdir, sizeof objdir, "%sobjects", git_repository_path(repo));
    snprintf(packdir, sizeof packdir, "%s/pack", objdir);

    if (autolimit > 0) {
        char d17[210];
        snprintf(d17, sizeof d17, "%s/17", objdir);
        if (count_loose(d17) * 256 < autolimit) {
            out->skipped = true;
            git_repository_free(repo);
            return 0;
        }
    }

    if (git_packbuilder_new(&pb, repo) < 0 || git_revwalk_new(&walk, repo) < 0) {
        rc = fail(err, errlen, "gc");
        goto out;
    }
    git_revwalk_push_glob(walk, "refs/*");      /* none yet: nothing to pack */
    git_revwalk_push_head(walk);
    if (git_packbuilder_insert_walk(pb, walk) < 0) {
        rc = fail(err, errlen, "gc: listing objects");
        goto out;
    }
    out->packed = git_packbuilder_object_count(pb);
    if (out->packed == 0) {
        rc = 0;
        goto out;
    }
    mkdir(packdir, 0777);
    if (git_packbuilder_write(pb, packdir, 0, NULL, NULL) < 0) {
        rc = fail(err, errlen, "gc: writing the pack");
        goto out;
    }
    snprintf(name, sizeof name, "%s", git_packbuilder_name(pb));
    /* Close the repository first: FAT won't delete a file that is open. */
    git_packbuilder_free(pb);
    pb = NULL;
    git_revwalk_free(walk);
    walk = NULL;
    git_repository_free(repo);
    repo = NULL;

    snprintf(idx, sizeof idx, "%s/pack-%s.idx", packdir, name);
    if ((odb = pack_odb(idx)) == NULL) {
        rc = fail(err, errlen, "gc: reading the new pack");
        goto out;
    }

    /* Loose objects the pack holds. */
    for (int i = 0; i < 256; i++) {
        char dir[220], file[280], hex[GIT_OID_SHA1_HEXSIZE + 1];
        snprintf(dir, sizeof dir, "%s/%02x", objdir, i);
        DIR *d = opendir(dir);
        struct dirent *e;
        if (d == NULL)
            continue;
        while ((e = readdir(d)) != NULL) {
            git_oid id;
            if (strlen(e->d_name) != GIT_OID_SHA1_HEXSIZE - 2 || !is_hex(e->d_name, GIT_OID_SHA1_HEXSIZE - 2))
                continue;
            snprintf(hex, sizeof hex, "%02x", i);
            if (!join(hex + 2, sizeof hex - 2, e->d_name, "", ""))
                continue;
            if (git_oid_fromstrp(&id, hex) == 0 && git_odb_exists(odb, &id)) {
                if (join(file, sizeof file, dir, "/", e->d_name) && unlink(file) == 0)
                    out->loose_removed++;
            }
        }
        closedir(d);
        rmdir(dir);                                 /* only if now empty */
    }

    /* Older packs whose every object is in the new one. */
    DIR *d = opendir(packdir);
    struct dirent *e;
    while (d != NULL && (e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        char other[320], base[300];
        if (len < 9 || strncmp(e->d_name, "pack-", 5) != 0 || strcmp(e->d_name + len - 4, ".idx") != 0
                || strstr(e->d_name, name) != NULL)
            continue;
        e->d_name[len - 4] = '\0';                  /* pack-<hash> */
        if (!join(base, sizeof base, packdir, "/", e->d_name))
            continue;
        if (!join(other, sizeof other, base, ".keep", "") || exists(other))
            continue;                               /* someone asked to keep it */
        join(other, sizeof other, base, ".idx", "");
        git_odb *old = pack_odb(other);
        cover_t c = { odb, true };
        if (old == NULL)
            continue;
        git_odb_foreach(old, covered_cb, &c);
        git_odb_free(old);
        if (!c.all)
            continue;
        unlink(other);
        join(other, sizeof other, base, ".pack", "");
        unlink(other);
        join(other, sizeof other, base, ".rev", "");
        unlink(other);
        out->packs_removed++;
    }
    if (d != NULL)
        closedir(d);
    rc = 0;
out:
    git_odb_free(odb);
    git_packbuilder_free(pb);
    git_revwalk_free(walk);
    git_repository_free(repo);
    return rc;
}
