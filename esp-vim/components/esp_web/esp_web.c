/*
 * esp_web: the HTTPS file manager, settings and live status. See
 * include/esp_web.h for the security model and the threading contract.
 */

#include "esp_web.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_fs.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "esp_web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char app_js_start[]     asm("_binary_app_js_start");
extern const char app_js_end[]       asm("_binary_app_js_end");

#define NVS_WEB       "espweb"
#define NVS_SETTINGS  "espvim"
#define PBKDF2_ITERS  20000
#define SALT_LEN      16
#define HASH_LEN      32
#define MAX_SESSIONS  4
#define SESSION_IDLE_US (60LL * 60 * 1000000)     /* one hour */
#define CHUNK         4096

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static httpd_handle_t s_server;
static int s_port;
static char s_fingerprint[100];

typedef struct {
    char sid[65];               /* hex of 32 random bytes; "" = free slot */
    char csrf[33];
    int64_t last;
} session_t;
static session_t s_sessions[MAX_SESSIONS];

/* Failed-login backoff: after n failures, wait 2^n s (at most 64 s). */
static int s_fails;
static int64_t s_next_login_us;

static esp_web_status_t s_status;
static bool s_settings_changed;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

esp_err_t esp_web_init(void)
{
    if (s_lock == NULL)
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static void random_hex(char *out, size_t bytes)
{
    uint8_t b[32];
    esp_fill_random(b, bytes);
    for (size_t i = 0; i < bytes; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * bytes] = '\0';
}

static int rng(void *ctx, unsigned char *buf, size_t len)
{
    esp_fill_random(buf, len);
    return 0;
}

/* ------------------------------------------------------------------ NVS -- */

static esp_err_t nvs_get_alloc(const char *ns, const char *key, char **out, size_t *len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READONLY, &h);
    if (e != ESP_OK)
        return e;
    size_t n = 0;
    e = nvs_get_blob(h, key, NULL, &n);
    if (e == ESP_OK) {
        *out = malloc(n + 1);
        if (*out == NULL)
            e = ESP_ERR_NO_MEM;
        else if ((e = nvs_get_blob(h, key, *out, &n)) == ESP_OK) {
            (*out)[n] = '\0';
            *len = n;
        } else {
            free(*out);
            *out = NULL;
        }
    }
    nvs_close(h);
    return e;
}

static esp_err_t nvs_put(const char *ns, const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_blob(h, key, data, len);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* ----------------------------------------------------------- password -- */

static bool password_set(void)
{
    char *pw = NULL;
    size_t n = 0;
    bool set = nvs_get_alloc(NVS_WEB, "pw", &pw, &n) == ESP_OK && n == SALT_LEN + HASH_LEN;
    free(pw);
    return set;
}

static void pbkdf2(const char *pw, const uint8_t *salt, uint8_t *out)
{
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pw, strlen(pw),
                                  salt, SALT_LEN, PBKDF2_ITERS, HASH_LEN, out);
}

int esp_web_set_password(const char *password, char *err, size_t errlen)
{
    if (password == NULL || strlen(password) < ESP_WEB_MIN_PASSWORD) {
        snprintf(err, errlen, "the password must be at least %d characters", ESP_WEB_MIN_PASSWORD);
        return -1;
    }
    uint8_t rec[SALT_LEN + HASH_LEN];
    esp_fill_random(rec, SALT_LEN);
    pbkdf2(password, rec, rec + SALT_LEN);
    esp_err_t e = nvs_put(NVS_WEB, "pw", rec, sizeof rec);
    memset(rec, 0, sizeof rec);
    if (e != ESP_OK) {
        snprintf(err, errlen, "could not store the password: %s", esp_err_to_name(e));
        return -1;
    }
    lock();
    memset(s_sessions, 0, sizeof s_sessions);   /* everyone logs in again */
    unlock();
    return 0;
}

static bool password_ok(const char *password)
{
    char *rec = NULL;
    size_t n = 0;
    if (nvs_get_alloc(NVS_WEB, "pw", &rec, &n) != ESP_OK || n != SALT_LEN + HASH_LEN) {
        free(rec);
        return false;
    }
    uint8_t h[HASH_LEN];
    pbkdf2(password, (uint8_t *)rec, h);
    uint8_t diff = 0;
    for (int i = 0; i < HASH_LEN; i++)                  /* constant time */
        diff |= h[i] ^ (uint8_t)rec[SALT_LEN + i];
    free(rec);
    return diff == 0;
}

/* -------------------------------------------------------- certificate -- */

static void fingerprint_of(const char *pem, size_t len)
{
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    s_fingerprint[0] = '\0';
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, len + 1) == 0) {
        uint8_t d[32];
        mbedtls_sha256(crt.raw.p, crt.raw.len, d, 0);
        for (int i = 0; i < 32; i++)
            sprintf(s_fingerprint + 3 * i, "%02X%s", d[i], i < 31 ? ":" : "");
    }
    mbedtls_x509_crt_free(&crt);
}

/* The certificate and key from NVS, generating them the first time. */
static int load_cert(char **cert, size_t *clen, char **key, size_t *klen, char *err, size_t n)
{
    if (nvs_get_alloc(NVS_WEB, "cert", cert, clen) == ESP_OK
            && nvs_get_alloc(NVS_WEB, "key", key, klen) == ESP_OK) {
        fingerprint_of(*cert, *clen);
        return 0;
    }
    free(*cert);
    *cert = NULL;

    ESP_LOGI(TAG, "generating the web certificate (ECDSA P-256, self-signed)");
    int rc = -1;
    mbedtls_pk_context pk;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_crt_init(&crt);
    char *c = malloc(2048), *k = malloc(1024);
    uint8_t serial[16];
    esp_fill_random(serial, sizeof serial);
    serial[0] &= 0x7f;                                  /* positive */
    if (c == NULL || k == NULL
            || mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0
            || mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), rng, NULL) != 0) {
        snprintf(err, n, "certificate: key generation failed");
        goto out;
    }
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=esp-vim,O=esp-vim") != 0
            || mbedtls_x509write_crt_set_issuer_name(&crt, "CN=esp-vim,O=esp-vim") != 0
            || mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial) != 0
            /* The device clock may be anywhere; make the window wide. */
            || mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20491231235959") != 0
            || mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0
            || mbedtls_x509write_crt_pem(&crt, (unsigned char *)c, 2048, rng, NULL) != 0
            || mbedtls_pk_write_key_pem(&pk, (unsigned char *)k, 1024) != 0) {
        snprintf(err, n, "certificate: could not build it");
        goto out;
    }
    if (nvs_put(NVS_WEB, "cert", c, strlen(c)) != ESP_OK
            || nvs_put(NVS_WEB, "key", k, strlen(k)) != ESP_OK) {
        snprintf(err, n, "certificate: could not store it in NVS");
        goto out;
    }
    *cert = c;
    *clen = strlen(c);
    *key = k;
    *klen = strlen(k);
    c = k = NULL;
    fingerprint_of(*cert, *clen);
    rc = 0;
out:
    if (k)
        memset(k, 0, 1024);
    free(c);
    free(k);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    return rc;
}

/* ------------------------------------------------------------ helpers -- */

static void security_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                       "img-src 'self' data:; frame-ancestors 'none'");
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    security_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return e;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg);
    return send_json(req, status, j);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, "200 OK", j);
}

/* %xx and + decoding, in place. */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++, o++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o = (char)strtol(h, NULL, 16);
            s += 2;
        } else if (*s == '+') {
            *o = ' ';
        } else {
            *o = *s;
        }
    }
    *o = '\0';
}

/* Query parameter {key}, decoded, into {out}; false if absent. */
static bool query(httpd_req_t *req, const char *key, char *out, size_t n)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 1500)
        return false;
    char *q = malloc(qlen + 1);
    if (q == NULL)
        return false;
    bool ok = httpd_req_get_url_query_str(req, q, qlen + 1) == ESP_OK
           && httpd_query_key_value(q, key, out, n) == ESP_OK;
    free(q);
    if (ok)
        url_decode(out);
    return ok;
}

/* Read a small request body (JSON). NULL if too big or unreadable. */
static char *read_body(httpd_req_t *req, size_t max)
{
    if (req->content_len > max)
        return NULL;
    char *b = malloc(req->content_len + 1);
    if (b == NULL)
        return NULL;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, b + got, req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            free(b);
            return NULL;
        }
        got += (size_t)r;
    }
    b[got] = '\0';
    return b;
}

/* ------------------------------------------------------------ sessions -- */

/* The session for this request's cookie, refreshed; NULL if none. Caller
 * must hold the lock. */
static session_t *session_of(httpd_req_t *req)
{
    char sid[80];
    size_t n = sizeof sid;
    if (httpd_req_get_cookie_val(req, "sid", sid, &n) != ESP_OK || strlen(sid) != 64)
        return NULL;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session_t *s = &s_sessions[i];
        if (s->sid[0] && strcmp(s->sid, sid) == 0) {
            if (now - s->last > SESSION_IDLE_US) {
                memset(s, 0, sizeof *s);
                return NULL;
            }
            s->last = now;
            return s;
        }
    }
    return NULL;
}

/*
 * Gate for /api/ handlers: a valid session, and for anything that changes
 * state (not GET) the session's CSRF token in X-CSRF-Token. On failure the
 * error response has been sent and ESP_FAIL is returned.
 */
static esp_err_t require_session(httpd_req_t *req)
{
    lock();
    session_t *s = session_of(req);
    char csrf[33] = "";
    bool ok = s != NULL;
    if (ok)
        memcpy(csrf, s->csrf, sizeof csrf);
    unlock();
    if (!ok) {
        send_error(req, "401 Unauthorized", "not logged in");
        return ESP_FAIL;
    }
    if (req->method != HTTP_GET) {
        char tok[40];
        if (httpd_req_get_hdr_value_str(req, "X-CSRF-Token", tok, sizeof tok) != ESP_OK
                || strcmp(tok, csrf) != 0) {
            send_error(req, "403 Forbidden", "missing or wrong CSRF token");
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------ handlers -- */

static esp_err_t h_index(httpd_req_t *req)
{
    security_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_app_js(httpd_req_t *req)
{
    security_headers(req);
    httpd_resp_set_type(req, "text/javascript; charset=utf-8");
    return httpd_resp_send(req, app_js_start, app_js_end - app_js_start - 1);
}

static esp_err_t h_session(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    lock();
    session_t *s = session_of(req);
    cJSON_AddBoolToObject(j, "logged_in", s != NULL);
    if (s)
        cJSON_AddStringToObject(j, "csrf", s->csrf);
    unlock();
    char chip[16] = "ESP32-";                   /* esp32p4 -> ESP32-P4 */
    size_t n = strlen(chip);
    for (const char *t = CONFIG_IDF_TARGET + 5; *t && n < sizeof chip - 1; t++)
        chip[n++] = (char)toupper((unsigned char)*t);
    chip[n] = '\0';
    cJSON_AddStringToObject(j, "chip", chip);
    return send_json(req, "200 OK", j);
}

static esp_err_t h_login(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    lock();
    int64_t wait = s_next_login_us - now;
    unlock();
    if (wait > 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "too many failed logins; wait %d s", (int)(wait / 1000000) + 1);
        return send_error(req, "429 Too Many Requests", msg);
    }
    char *body = read_body(req, 512);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *pw = j ? cJSON_GetObjectItem(j, "password") : NULL;
    bool ok = cJSON_IsString(pw) && password_ok(pw->valuestring);
    cJSON_Delete(j);
    if (!ok) {
        lock();
        if (s_fails < 6)
            s_fails++;
        s_next_login_us = esp_timer_get_time() + ((int64_t)1 << s_fails) * 1000000;
        unlock();
        return send_error(req, "401 Unauthorized", "wrong password");
    }

    lock();
    s_fails = 0;
    s_next_login_us = 0;
    /* A free slot, else the one idle longest. */
    session_t *slot = &s_sessions[0];
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_sessions[i].sid[0]) {
            slot = &s_sessions[i];
            break;
        }
        if (s_sessions[i].last < slot->last)
            slot = &s_sessions[i];
    }
    random_hex(slot->sid, 32);
    random_hex(slot->csrf, 16);
    slot->last = esp_timer_get_time();
    char cookie[160], csrf[33];
    snprintf(cookie, sizeof cookie, "sid=%s; Path=/; HttpOnly; Secure; SameSite=Strict", slot->sid);
    memcpy(csrf, slot->csrf, sizeof csrf);
    unlock();

    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "csrf", csrf);
    return send_json(req, "200 OK", r);
}

static esp_err_t h_logout(httpd_req_t *req)
{
    if (require_session(req) != ESP_OK)
        return ESP_OK;
    lock();
    session_t *s = session_of(req);
    if (s)
        memset(s, 0, sizeof *s);
    unlock();
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=0");
    return send_ok(req);
}

static bool add_entry(void *ctx, const esp_fs_entry_t *e)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddBoolToObject(o, "dir", e->dir);
    cJSON_AddNumberToObject(o, "size", (double)e->size);
    cJSON_AddNumberToObject(o, "mtime", (double)e->mtime);
    cJSON_AddItemToArray((cJSON *)ctx, o);
    return true;
}

static esp_err_t h_list(httpd_req_t *req)
{
    if (require_session(req) != ESP_OK)
        return ESP_OK;
    char path[512];
    if (!query(req, "path", path, sizeof path))
        strcpy(path, "/");
    cJSON *arr = cJSON_CreateArray();
    esp_fs_err_t err;
    if (esp_fs_list(path, add_entry, arr, &err) != 0) {
        cJSON_Delete(arr);
        return send_error(req, "400 Bad Request", err.msg);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "path", path);
    cJSON_AddItemToObject(j, "entries", arr);
    if (strcmp(path, "/") != 0) {
        esp_fs_root_t r;
        if (esp_fs_space(path, &r, &err) == 0) {
            cJSON_AddNumberToObject(j, "free", (double)r.free);
            cJSON_AddNumberToObject(j, "total", (double)r.total);
            cJSON_AddBoolToObject(j, "readonly", r.readonly);
        }
    }
    return send_json(req, "200 OK", j);
}

static esp_err_t h_download(httpd_req_t *req)
{
    if (require_session(req) != ESP_OK)
        return ESP_OK;
    char path[512];
    if (!query(req, "path", path, sizeof path))
        return send_error(req, "400 Bad Request", "no path");
    esp_fs_err_t err;
    uint64_t size = 0;
    int fd = esp_fs_open_read(path, &size, &err);
    if (fd < 0)
        return send_error(req, "404 Not Found", err.msg);
    const char *base = strrchr(path, '/');
    char disp[300];
    snprintf(disp, sizeof disp, "attachment; filename=\"%.255s\"", base ? base + 1 : path);
    for (char *c = disp + 22; *c; c++)          /* no quotes or controls in the name */
        if (*c == '"' || (unsigned char)*c < 0x20)
            *c = '_';
    disp[strlen(disp) - 1] = '"';
    security_headers(req);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    char *buf = malloc(CHUNK);
    esp_err_t e = buf ? ESP_OK : ESP_ERR_NO_MEM;
    ssize_t r;
    while (e == ESP_OK && buf && (r = read(fd, buf, CHUNK)) > 0)
        e = httpd_resp_send_chunk(req, buf, r);
    free(buf);
    close(fd);
    if (e == ESP_OK)
        e = httpd_resp_send_chunk(req, NULL, 0);
    return e;
}

static esp_err_t h_upload(httpd_req_t *req)
{
    if (require_session(req) != ESP_OK)
        return ESP_OK;
    char path[512], ow[4] = "0";
    if (!query(req, "path", path, sizeof path))
        return send_error(req, "400 Bad Request", "no path");
    query(req, "overwrite", ow, sizeof ow);
    esp_fs_err_t err;
    int fd = esp_fs_create_part(path, ow[0] == '1', &err);
    if (fd < 0)
        return send_error(req, "409 Conflict", err.msg);
    char *buf = malloc(CHUNK);
    size_t left = req->content_len;
    bool ok = buf != NULL;
    while (ok && left > 0) {
        int r = httpd_req_recv(req, buf, left < CHUNK ? left : CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (r <= 0 || write(fd, buf, r) != r) {
            ok = false;
            break;
        }
        left -= (size_t)r;
    }
    free(buf);
    if (close(fd) != 0)
        ok = false;
    if (esp_fs_finish_part(path, ok, &err) != 0 || !ok)
        return send_error(req, "500 Internal Server Error", ok ? err.msg : "upload failed (disk full?)");
    return send_ok(req);
}

static esp_err_t h_mkdir(httpd_req_t *req)
{
    if (require_session(req) != ESP_OK)
        return ESP_OK;
    char path[512];
    esp_fs_err_t err;
    if (!query(req, "path", path, sizeof path))
        return send_error(req, "400 Bad Request", "no path");
    if (esp_fs_mkdir(path, &err) != 0)
        return send_error(req, "400 Bad Request", err.msg);
    return send_ok(req);
}

static esp_err_t h_delete(httpd_req_t *req)
{
    if (require_session(req) != ESP_OK)
        return ESP_OK;
    char path[512];
    esp_fs_err_t err;
    if (!query(req, "path", path, sizeof path))
        return send_error(req, "400 Bad Request", "no path");
    if (esp_fs_delete(path, NULL, NULL, &err) != 0)
        return send_error(req, "400 Bad Request", err.msg);
    return send_ok(req);
}

static esp_err_t h_move(httpd_req_t *req)
{
    if (require_session(req) != ESP_OK)
        return ESP_OK;
    char from[512], to[512], ow[4] = "0";
    esp_fs_err_t err;
    if (!query(req, "from", from, sizeof from) || !query(req, "to", to, sizeof to))
        return send_error(req, "400 Bad Request", "need from and to");
    query(req, "overwrite", ow, sizeof ow);
    if (esp_fs_move(from, to, ow[0] == '1' ? ESP_FS_OVERWRITE : 0, NULL, NULL, &err) != 0)
        return send_error(req, "400 Bad Request", err.msg);
    return send_ok(req);
}

/* ------------------------------------------------------------ settings -- */

static const char *const COLORS[] = { "default", "desert", "habamax", "slate", "lunaperche", "retrobox" };

static bool colorscheme_ok(const char *c)
{
    if (c[0] == '\0')
        return true;
    for (size_t i = 0; i < sizeof COLORS / sizeof COLORS[0]; i++)
        if (strcmp(c, COLORS[i]) == 0)
            return true;
    return false;
}

void esp_web_settings_get(esp_web_settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->tabstop = 8;
    s->shiftwidth = 8;
    s->wrap = true;
    char *blob = NULL;
    size_t n = 0;
    if (nvs_get_alloc(NVS_SETTINGS, "settings", &blob, &n) == ESP_OK && n == sizeof *s)
        memcpy(s, blob, sizeof *s);
    free(blob);
    s->colorscheme[sizeof s->colorscheme - 1] = '\0';
    s->background[sizeof s->background - 1] = '\0';
}

static cJSON *settings_json(const esp_web_settings_t *s)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "tabstop", s->tabstop);
    cJSON_AddNumberToObject(j, "shiftwidth", s->shiftwidth);
    cJSON_AddBoolToObject(j, "expandtab", s->expandtab);
    cJSON_AddBoolToObject(j, "number", s->number);
    cJSON_AddBoolToObject(j, "relativenumber", s->relativenumber);
    cJSON_AddBoolToObject(j, "wrap", s->wrap);
    cJSON_AddStringToObject(j, "colorscheme", s->colorscheme);
    cJSON_AddStringToObject(j, "background", s->background);
    cJSON *c = cJSON_AddArrayToObject(j, "colorschemes");
    for (size_t i = 0; i < sizeof COLORS / sizeof COLORS[0]; i++)
        cJSON_AddItemToArray(c, cJSON_CreateString(COLORS[i]));
    return j;
}

static esp_err_t h_settings(httpd_req_t *req)
{
    if (require_session(req) != ESP_OK)
        return ESP_OK;
    esp_web_settings_t s;
    esp_web_settings_get(&s);
    if (req->method == HTTP_GET)
        return send_json(req, "200 OK", settings_json(&s));

    char *body = read_body(req, 1024);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (j == NULL)
        return send_error(req, "400 Bad Request", "expected a JSON object");
    const cJSON *v;
    const char *bad = NULL;
    if ((v = cJSON_GetObjectItem(j, "tabstop")) && !(cJSON_IsNumber(v) && v->valueint >= 1 && v->valueint <= 16))
        bad = "tabstop must be 1..16";
    else if (v)
        s.tabstop = v->valueint;
    if ((v = cJSON_GetObjectItem(j, "shiftwidth")) && !(cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 16))
        bad = "shiftwidth must be 0..16";
    else if (v)
        s.shiftwidth = v->valueint;
#define BOOLOPT(name) \
    if ((v = cJSON_GetObjectItem(j, #name)) && !cJSON_IsBool(v)) bad = #name " must be true or false"; \
    else if (v) s.name = cJSON_IsTrue(v);
    BOOLOPT(expandtab)
    BOOLOPT(number)
    BOOLOPT(relativenumber)
    BOOLOPT(wrap)
#undef BOOLOPT
    if ((v = cJSON_GetObjectItem(j, "colorscheme"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof s.colorscheme || !colorscheme_ok(v->valuestring))
            bad = "unknown colorscheme";
        else
            strcpy(s.colorscheme, v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(j, "background"))) {
        if (!cJSON_IsString(v) || (strcmp(v->valuestring, "") && strcmp(v->valuestring, "light")
                                   && strcmp(v->valuestring, "dark")))
            bad = "background must be light or dark";
        else
            strcpy(s.background, v->valuestring);
    }
    cJSON_Delete(j);
    if (bad)
        return send_error(req, "400 Bad Request", bad);
    if (nvs_put(NVS_SETTINGS, "settings", &s, sizeof s) != ESP_OK)
        return send_error(req, "500 Internal Server Error", "could not store the settings");
    lock();
    s_settings_changed = true;          /* the Vim task applies them */
    unlock();
    return send_json(req, "200 OK", settings_json(&s));
}

bool esp_web_settings_changed(void)
{
    if (s_lock == NULL)
        return false;
    lock();
    bool c = s_settings_changed;
    s_settings_changed = false;
    unlock();
    return c;
}

/* -------------------------------------------------------------- status -- */

void esp_web_publish(const esp_web_status_t *st)
{
    if (s_lock == NULL)
        return;
    lock();
    s_status = *st;
    unlock();
}

static esp_err_t h_status(httpd_req_t *req)
{
    if (require_session(req) != ESP_OK)
        return ESP_OK;
    lock();
    esp_web_status_t st = s_status;
    unlock();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "file", st.file);
    cJSON_AddStringToObject(j, "filetype", st.filetype);
    cJSON_AddStringToObject(j, "mode", st.mode);
    cJSON_AddNumberToObject(j, "line", st.line);
    cJSON_AddNumberToObject(j, "col", st.col);
    cJSON_AddNumberToObject(j, "lines", st.lines);
    cJSON_AddNumberToObject(j, "words", st.words);
    cJSON_AddNumberToObject(j, "chars", st.chars);
    cJSON_AddNumberToObject(j, "bytes", st.bytes);
    cJSON_AddBoolToObject(j, "modified", st.modified);
    cJSON_AddNumberToObject(j, "buffers", st.buffers);
    return send_json(req, "200 OK", j);
}

/* -------------------------------------------------------- start / stop -- */

static const httpd_uri_t ROUTES[] = {
    { .uri = "/",             .method = HTTP_GET,  .handler = h_index },
    { .uri = "/app.js",       .method = HTTP_GET,  .handler = h_app_js },
    { .uri = "/api/session",  .method = HTTP_GET,  .handler = h_session },
    { .uri = "/api/login",    .method = HTTP_POST, .handler = h_login },
    { .uri = "/api/logout",   .method = HTTP_POST, .handler = h_logout },
    { .uri = "/api/list",     .method = HTTP_GET,  .handler = h_list },
    { .uri = "/api/file",     .method = HTTP_GET,  .handler = h_download },
    { .uri = "/api/file",     .method = HTTP_PUT,  .handler = h_upload },
    { .uri = "/api/mkdir",    .method = HTTP_POST, .handler = h_mkdir },
    { .uri = "/api/delete",   .method = HTTP_POST, .handler = h_delete },
    { .uri = "/api/move",     .method = HTTP_POST, .handler = h_move },
    { .uri = "/api/settings", .method = HTTP_GET,  .handler = h_settings },
    { .uri = "/api/settings", .method = HTTP_POST, .handler = h_settings },
    { .uri = "/api/status",   .method = HTTP_GET,  .handler = h_status },
};

int esp_web_start(int port, char *err, size_t errlen)
{
    if (s_server != NULL) {
        snprintf(err, errlen, "already running on port %d", s_port);
        return -1;
    }
    if (!password_set()) {
        snprintf(err, errlen, "no password is set (use :EspWebPasswd first)");
        return -1;
    }
    char *cert = NULL, *key = NULL;
    size_t clen = 0, klen = 0;
    if (load_cert(&cert, &clen, &key, &klen, err, errlen) != 0)
        return -1;

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.servercert = (const uint8_t *)cert;
    conf.servercert_len = clen + 1;
    conf.prvtkey_pem = (const uint8_t *)key;
    conf.prvtkey_len = klen + 1;
    conf.port_secure = port;
    conf.httpd.max_open_sockets = 4;
    conf.httpd.max_uri_handlers = sizeof ROUTES / sizeof ROUTES[0];
    conf.httpd.stack_size = 10240;
    conf.httpd.lru_purge_enable = true;
    esp_err_t e = httpd_ssl_start(&s_server, &conf);
    /* The server keeps its own copies of the certificate and key. */
    memset(key, 0, klen);
    free(key);
    free(cert);
    if (e != ESP_OK) {
        s_server = NULL;
        snprintf(err, errlen, "could not start the server: %s", esp_err_to_name(e));
        return -1;
    }
    for (size_t i = 0; i < sizeof ROUTES / sizeof ROUTES[0]; i++)
        httpd_register_uri_handler(s_server, &ROUTES[i]);
    s_port = port;
    return 0;
}

void esp_web_stop(void)
{
    if (s_server != NULL) {
        httpd_ssl_stop(s_server);
        s_server = NULL;
    }
    lock();
    memset(s_sessions, 0, sizeof s_sessions);
    unlock();
}

void esp_web_get_info(esp_web_info_t *info)
{
    memset(info, 0, sizeof *info);
    info->running = s_server != NULL;
    info->port = s_port;
    info->password_set = password_set();
    if (s_fingerprint[0] == '\0') {
        char *c = NULL;
        size_t n = 0;
        if (nvs_get_alloc(NVS_WEB, "cert", &c, &n) == ESP_OK)
            fingerprint_of(c, n);
        free(c);
    }
    snprintf(info->fingerprint, sizeof info->fingerprint, "%s", s_fingerprint);
    lock();
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (s_sessions[i].sid[0] && now - s_sessions[i].last <= SESSION_IDLE_US)
            info->sessions++;
    unlock();
}
