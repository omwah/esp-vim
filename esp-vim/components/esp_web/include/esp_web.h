/*
 * esp_web: the password-protected web interface -- files, settings and a live
 * view of what is being edited, over HTTPS.
 *
 * Security model (docs/PLAN.md, Phase 6, "Web file manager"):
 *   - HTTPS only, with a self-signed ECDSA P-256 certificate generated on the
 *     device on first start and kept in NVS; its SHA-256 fingerprint is shown in
 *     Vim so the user can check what the browser is warning about.
 *   - Will not start without a password, and there is no default one. The
 *     password is stored as PBKDF2-HMAC-SHA256 with a per-device random salt.
 *   - Login gives a session cookie (HttpOnly, Secure, SameSite=Strict); every
 *     request that changes anything also needs the session's CSRF token.
 *   - Failed logins back off exponentially; sessions are capped and expire.
 *   - Every file operation goes through esp_fs, the one validated core.
 *
 * Threading: the HTTP server runs in its own task and never touches Vim. What
 * crosses between the two is exactly:
 *   - the status snapshot: written by the Vim task (esp_web_publish), read by
 *     the server;
 *   - settings: written by the server into NVS, with a "changed" flag the Vim
 *     task takes and applies itself (esp_web_settings_changed).
 * Both are guarded by one mutex.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_WEB_DEFAULT_PORT 443
#define ESP_WEB_MIN_PASSWORD 8

typedef struct {
    bool running;
    int port;
    bool password_set;
    int sessions;               /* logged-in browsers */
    char fingerprint[100];      /* "AB:CD:..." SHA-256 of the certificate */
} esp_web_info_t;

/* What Vim publishes about the current buffer. */
typedef struct {
    char file[256];
    char filetype[32];
    char mode[16];
    long line, col, lines, words, chars, bytes;
    bool modified;
    int buffers;
} esp_web_status_t;

/* Editor settings the web page may change: an allow-list, validated. */
typedef struct {
    int tabstop;                /* 1..16 */
    int shiftwidth;             /* 0..16 */
    bool expandtab, number, relativenumber, wrap;
    char colorscheme[24];       /* one of the shipped schemes, or "" */
    char background[8];         /* "light", "dark" or "" */
} esp_web_settings_t;

esp_err_t esp_web_init(void);

/* Start on {port}; fails (with a message) without a password. */
int esp_web_start(int port, char *err, size_t errlen);
void esp_web_stop(void);
void esp_web_get_info(esp_web_info_t *info);

/* Set the password (at least ESP_WEB_MIN_PASSWORD characters). Logs out
 * every existing session. */
int esp_web_set_password(const char *password, char *err, size_t errlen);

/* Vim task: publish the current status (copied). */
void esp_web_publish(const esp_web_status_t *status);

/* The stored settings (defaults where unset). */
void esp_web_settings_get(esp_web_settings_t *s);

/* Vim task: TRUE once after the web page changed the settings. */
bool esp_web_settings_changed(void);

#ifdef __cplusplus
}
#endif
