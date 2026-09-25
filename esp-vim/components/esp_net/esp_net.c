/*
 * esp_net: interface bring-up and the HTTP(S) client. See include/esp_net.h.
 */

#include "esp_net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_fs.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#if CONFIG_ESP_VIM_NET_ETH
#include "esp_eth.h"
#endif
#if CONFIG_ESP_VIM_WIFI
#include "esp_wifi.h"
#endif

static const char *TAG = "esp_net";
static esp_netif_t *s_netif;
static const char *s_iface = "none";

/* ------------------------------------------------------------ interface -- */

#if CONFIG_ESP_VIM_NET_ETH
static esp_err_t start_ethernet(void)
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&cfg);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = -1;               /* find it on the MDIO bus */
    phy_config.reset_gpio_num = -1;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (mac == NULL || phy == NULL)
        return ESP_ERR_NO_MEM;

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    esp_err_t err = esp_eth_driver_install(&config, &eth);
    if (err != ESP_OK)
        return err;
    err = esp_netif_attach(s_netif, esp_eth_new_netif_glue(eth));
    if (err == ESP_OK)
        err = esp_eth_start(eth);           /* DHCP runs in the background */
    if (err == ESP_OK)
        s_iface = "ethernet";
    return err;
}
#endif

#if CONFIG_ESP_VIM_WIFI
/* Keep reconnecting unless the user disconnected on purpose. */
static volatile bool s_wifi_want;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        wifi_config_t c;
        if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK && c.sta.ssid[0]) {
            s_wifi_want = true;             /* a network was stored: rejoin it */
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED && s_wifi_want) {
        esp_wifi_connect();
    }
}

static esp_err_t start_wifi(void)
{
    s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err == ESP_OK)
        err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK)
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK)
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (err == ESP_OK)
        err = esp_wifi_start();
    if (err == ESP_OK)
        s_iface = "wifi";
    return err;
}

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "wep";
    case WIFI_AUTH_WPA_PSK:         return "wpa";
    case WIFI_AUTH_WPA2_PSK:        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa/wpa2";
    case WIFI_AUTH_WPA3_PSK:        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2/wpa3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "enterprise";
    default:                        return "other";
    }
}

int esp_net_wifi_scan(esp_net_ap_cb cb, void *ctx, char *err, size_t errlen)
{
    esp_err_t e = esp_wifi_scan_start(NULL, true);
    if (e != ESP_OK)
        return snprintf(err, errlen, "scan: %s", esp_err_to_name(e)), -1;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40)
        n = 40;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof *recs);
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        return snprintf(err, errlen, "out of memory"), -1;
    }
    esp_wifi_scan_get_ap_records(&n, recs);
    for (uint16_t i = 0; i < n; i++) {
        esp_net_ap_t ap = { .rssi = recs[i].rssi, .channel = recs[i].primary,
                            .auth = auth_name(recs[i].authmode) };
        snprintf(ap.ssid, sizeof ap.ssid, "%s", (const char *)recs[i].ssid);
        if (!cb(ctx, &ap))
            break;
    }
    free(recs);
    return 0;
}

int esp_net_wifi_connect(const char *ssid, const char *password, char *err, size_t errlen)
{
    if (ssid == NULL || !*ssid || strlen(ssid) > 32)
        return snprintf(err, errlen, "the network name must be 1 to 32 characters"), -1;
    if (password && *password && (strlen(password) < 8 || strlen(password) > 63))
        return snprintf(err, errlen, "a WPA password is 8 to 63 characters"), -1;
    wifi_config_t c = {0};
    memcpy(c.sta.ssid, ssid, strlen(ssid));
    if (password)
        memcpy(c.sta.password, password, strlen(password));
    c.sta.threshold.authmode = password && *password ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    s_wifi_want = false;
    esp_wifi_disconnect();
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &c);      /* stored: WIFI_STORAGE_FLASH */
    if (e == ESP_OK) {
        s_wifi_want = true;
        e = esp_wifi_connect();
    }
    return e == ESP_OK ? 0 : (snprintf(err, errlen, "connect: %s", esp_err_to_name(e)), -1);
}

int esp_net_wifi_disconnect(char *err, size_t errlen)
{
    s_wifi_want = false;
    wifi_config_t c = {0};
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &c);                     /* forget it */
    return 0;
}
#else
int esp_net_wifi_scan(esp_net_ap_cb cb, void *ctx, char *err, size_t errlen)
{
    return snprintf(err, errlen, "this build has no WiFi"), -1;
}
int esp_net_wifi_connect(const char *ssid, const char *password, char *err, size_t errlen)
{
    return snprintf(err, errlen, "this build has no WiFi"), -1;
}
int esp_net_wifi_disconnect(char *err, size_t errlen)
{
    return snprintf(err, errlen, "this build has no WiFi"), -1;
}
#endif

esp_err_t esp_net_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK)
        err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE)
        err = ESP_OK;                       /* someone made the loop already */
    if (err != ESP_OK)
        return err;
#if CONFIG_ESP_VIM_NET_ETH
    err = start_ethernet();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "ethernet: %s -- no network", esp_err_to_name(err));
#elif CONFIG_ESP_VIM_WIFI
    err = start_wifi();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "wifi: %s -- no network", esp_err_to_name(err));
#endif
    return err;
}

static void ip4(char *out, size_t n, const esp_ip4_addr_t *a)
{
    snprintf(out, n, IPSTR, IP2STR(a));
}

void esp_net_get_status(esp_net_status_t *st)
{
    memset(st, 0, sizeof *st);
    snprintf(st->iface, sizeof st->iface, "%s", s_iface);
    if (s_netif == NULL)
        return;
    uint8_t mac[6];
    if (esp_netif_get_mac(s_netif, mac) == ESP_OK)
        snprintf(st->mac, sizeof st->mac, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        st->up = esp_netif_is_netif_up(s_netif);
        ip4(st->ip, sizeof st->ip, &ip.ip);
        ip4(st->netmask, sizeof st->netmask, &ip.netmask);
        ip4(st->gw, sizeof st->gw, &ip.gw);
    }
#if CONFIG_ESP_VIM_WIFI
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK)
        snprintf(st->ssid, sizeof st->ssid, "%s", (const char *)c.sta.ssid);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        st->rssi = ap.rssi;
    else
        st->up = false;                     /* an address kept after a drop */
#endif
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK
            && dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0)
        ip4(st->dns, sizeof st->dns, &dns.ip.u_addr.ip4);
}

/* ----------------------------------------------------------------- http -- */

#define CHUNK       4096
#define MAX_REDIRECTS 5

static int fail(char *err, size_t n, const char *fmt, ...)
{
    if (err != NULL && n > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

int esp_net_http_fetch(const char *url, esp_net_sink_cb sink, void *sink_ctx,
                       esp_net_progress_cb progress, void *progress_ctx,
                       esp_net_http_result_t *result, char *err, size_t errlen)
{
    esp_net_http_result_t res = {0};
    if (url == NULL || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0))
        return fail(err, errlen, "%s: only http:// and https:// are supported", url ? url : "(null)");

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .user_agent = "esp-vim",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL)
        return fail(err, errlen, "%s: cannot start the HTTP client", url);

    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL)
        buf = malloc(CHUNK);
    int rc = -1;
    if (buf == NULL) {
        fail(err, errlen, "out of memory");
        goto out;
    }

    for (int hops = 0;; hops++) {
        esp_err_t e = esp_http_client_open(c, 0);
        if (e != ESP_OK) {
            fail(err, errlen, "%s: %s", url, esp_err_to_name(e));
            goto out;
        }
        if (esp_http_client_fetch_headers(c) < 0) {
            fail(err, errlen, "%s: no response", url);
            goto out;
        }
        res.status = esp_http_client_get_status_code(c);
        if (!is_redirect(res.status))
            break;
        if (hops == MAX_REDIRECTS) {
            fail(err, errlen, "%s: too many redirects", url);
            goto out;
        }
        esp_http_client_flush_response(c, NULL);
        if (esp_http_client_set_redirection(c) != ESP_OK) {
            fail(err, errlen, "%s: HTTP %d without a usable Location", url, res.status);
            goto out;
        }
        esp_http_client_close(c);
    }
    if (res.status != 200) {
        fail(err, errlen, "%s: HTTP %d", url, res.status);
        goto out;
    }

    for (;;) {
        int n = esp_http_client_read(c, buf, CHUNK);
        if (n < 0) {
            fail(err, errlen, "%s: connection lost after %llu bytes", url,
                 (unsigned long long)res.size);
            goto out;
        }
        if (n == 0) {
            /* End of body. Only a response that announced its length can be
             * known to be short; chunked and close-delimited bodies end here. */
            int64_t want = esp_http_client_get_content_length(c);
            if (want > 0 && res.size < (uint64_t)want) {
                fail(err, errlen, "%s: incomplete response (%llu of %lld bytes)", url,
                     (unsigned long long)res.size, (long long)want);
                goto out;
            }
            break;
        }
        if (!sink(sink_ctx, buf, (size_t)n)) {
            fail(err, errlen, "%s: could not store the data", url);
            goto out;
        }
        res.size += (uint64_t)n;
        if (progress != NULL && !progress(progress_ctx, res.size)) {
            fail(err, errlen, "interrupted");
            goto out;
        }
    }
    rc = 0;
out:
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (result != NULL)
        *result = res;
    return rc;
}

typedef struct {
    int fd;
    int err;
} file_sink_t;

static bool write_sink(void *ctx, const char *data, size_t len)
{
    file_sink_t *f = ctx;
    while (len > 0) {
        ssize_t w = write(f->fd, data, len);
        if (w <= 0) {
            f->err = w < 0 ? errno : ENOSPC;
            return false;
        }
        data += w;
        len -= (size_t)w;
    }
    return true;
}

int esp_net_http_download(const char *url, const char *dest,
                          esp_net_progress_cb progress, void *progress_ctx,
                          esp_net_http_result_t *result, char *err, size_t errlen)
{
    char path[512], part[520];
    esp_fs_err_t ferr;
    if (esp_fs_check(dest, true, path, sizeof path, &ferr) != 0)
        return fail(err, errlen, "%s", ferr.msg);
    snprintf(part, sizeof part, "%s.part", path);

    file_sink_t f = { .fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666) };
    if (f.fd < 0)
        return fail(err, errlen, "%s: %s", part, strerror(errno));
    int rc = esp_net_http_fetch(url, write_sink, &f, progress, progress_ctx, result, err, errlen);
    if (rc != 0 && f.err != 0)
        fail(err, errlen, "%s: %s", path, strerror(f.err));
    if (close(f.fd) != 0 && rc == 0)
        rc = fail(err, errlen, "%s: %s", part, strerror(errno));
    if (rc == 0) {
        unlink(path);                   /* FAT will not rename onto a name */
        if (rename(part, path) != 0)
            rc = fail(err, errlen, "%s: %s", path, strerror(errno));
    }
    if (rc != 0)
        unlink(part);
    return rc;
}
