/*
 * esp_net_status(), esp_http_get(): Vim script access to components/esp_net.
 * Behind :EspNet, :EspGet, and netrw's http:// and https:// reads (patch 0009),
 * which is also how spell files are downloaded. And the radio co-processor
 * (the Tab5's C6): esp_c6(), esp_c6_update(), behind :EspC6.
 */

#include "vim.h"
#include "esp_vim_api.h"
#include "esp_net.h"
#include "esp_timer.h"

#define BODY_MAX (4 * 1024 * 1024)      /* esp_http_get() without a file */

/* esp_net_status() -> Dict: up, iface, ip, netmask, gw, dns, mac. */
void f_esp_net_status(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    esp_net_status_t st;
    esp_net_get_status(&st);
    dict_T *d = rettv->vval.v_dict;
    dict_add_bool(d, "up", st.up);
    dict_add_string(d, "iface", (char_u *)st.iface);
    dict_add_string(d, "ip", (char_u *)st.ip);
    dict_add_string(d, "netmask", (char_u *)st.netmask);
    dict_add_string(d, "gw", (char_u *)st.gw);
    dict_add_string(d, "dns", (char_u *)st.dns);
    dict_add_string(d, "mac", (char_u *)st.mac);
    dict_add_string(d, "ssid", (char_u *)st.ssid);
    dict_add_number(d, "rssi", st.rssi);
}

static bool add_ap(void *ctx, const esp_net_ap_t *ap)
{
    dict_T *d = dict_alloc();
    if (d == NULL)
        return false;
    dict_add_string(d, "ssid", (char_u *)ap->ssid);
    dict_add_number(d, "rssi", ap->rssi);
    dict_add_number(d, "channel", ap->channel);
    dict_add_string(d, "auth", (char_u *)ap->auth);
    return list_append_dict((list_T *)ctx, d) == OK;
}

/* esp_wifi_scan() -> List of Dicts: ssid, rssi, channel, auth. */
void f_esp_wifi_scan(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    char err[128];
    if (esp_net_wifi_scan(add_ap, rettv->vval.v_list, err, sizeof err) != 0)
        semsg("esp_wifi_scan(): %s", err);
}

/* esp_wifi_connect([{ssid} [, {password}]]): store the network and start
 * joining it; watch esp_net_status() for the result. Without {password}, the
 * saved network is joined again with its saved password: {ssid}, if given, must
 * be that network. An empty {password} is an open network. */
void f_esp_wifi_connect(typval_T *argvars, typval_T *rettv)
{
    char_u buf[NUMBUFLEN];
    char_u *ssid = NULL, *pw = NULL;
    if (argvars[0].v_type != VAR_UNKNOWN) {
        ssid = tv_get_string_buf_chk(&argvars[0], buf);
        if (ssid == NULL)
            return;
        if (argvars[1].v_type != VAR_UNKNOWN && (pw = tv_get_string_chk(&argvars[1])) == NULL)
            return;
    }
    char err[128];
    if (esp_net_wifi_connect((char *)ssid, (char *)pw, err, sizeof err) == 0)
        rettv->vval.v_number = TRUE;
    else
        semsg("esp_wifi_connect(): %s", err);
}

/* esp_wifi_disconnect(): leave the network until the next connect or restart.
 * It stays saved. */
void f_esp_wifi_disconnect(typval_T *argvars UNUSED, typval_T *rettv UNUSED)
{
    char err[128];
    if (esp_net_wifi_disconnect(err, sizeof err) != 0)
        semsg("esp_wifi_disconnect(): %s", err);
}

/* esp_wifi_forget(): leave the network and erase it and its password. */
void f_esp_wifi_forget(typval_T *argvars UNUSED, typval_T *rettv UNUSED)
{
    char err[128];
    if (esp_net_wifi_forget(err, sizeof err) != 0)
        semsg("esp_wifi_forget(): %s", err);
}

/* esp_wifi_saved(): the saved network's name, "" if none. Never the password. */
void f_esp_wifi_saved(typval_T *argvars UNUSED, typval_T *rettv)
{
    char ssid[33], err[128];
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = NULL;
    if (esp_net_wifi_saved(ssid, sizeof ssid, err, sizeof err) != 0)
        semsg("esp_wifi_saved(): %s", err);
    else
        rettv->vval.v_string = vim_strsave((char_u *)ssid);
}

static bool progress(void *ctx UNUSED, uint64_t bytes UNUSED)
{
    ui_breakcheck();
    return !got_int;
}

static bool body_sink(void *ctx, const char *data, size_t len)
{
    garray_T *ga = ctx;
    if ((size_t)ga->ga_len + len > BODY_MAX || ga_grow(ga, (int)len + 1) == FAIL)
        return false;
    mch_memmove((char *)ga->ga_data + ga->ga_len, data, len);
    ga->ga_len += (int)len;
    return true;
}

/*
 * esp_http_get({url} [, {file}]) -> Dict, or {} after an error.
 * With {file}: downloads into it (made absolute against the working
 * directory) and returns status, size and path. Without: returns status,
 * size and body (up to 4 MB).
 */
void f_esp_http_get(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    char_u *url = tv_get_string_chk(&argvars[0]);
    if (url == NULL)
        return;
    url = vim_strsave(url);
    if (url == NULL)
        return;

    char err[256];
    esp_net_http_result_t res;
    dict_T *d = rettv->vval.v_dict;
    if (argvars[1].v_type != VAR_UNKNOWN) {
        char_u *file = tv_get_string_chk(&argvars[1]);
        char_u *full = file == NULL ? NULL : FullName_save(file, TRUE);
        if (full != NULL) {
            if (esp_net_http_download((char *)url, (char *)full, progress, NULL,
                                      &res, err, sizeof err) == 0) {
                dict_add_number(d, "status", res.status);
                dict_add_number(d, "size", (varnumber_T)res.size);
                dict_add_string(d, "path", full);
            } else {
                semsg("esp_http_get(): %s", err);
            }
            vim_free(full);
        }
    } else {
        garray_T ga;
        ga_init2(&ga, 1, 4096);
        if (esp_net_http_fetch((char *)url, body_sink, &ga, progress, NULL,
                               &res, err, sizeof err) == 0) {
            dict_add_number(d, "status", res.status);
            dict_add_number(d, "size", (varnumber_T)res.size);
            if (ga_grow(&ga, 1) == OK) {
                ((char *)ga.ga_data)[ga.ga_len] = NUL;
                dict_add_string(d, "body", (char_u *)ga.ga_data);
            }
        } else {
            semsg("esp_http_get(): %s", err);
        }
        ga_clear(&ga);
    }
    vim_free(url);
}

/* esp_c6() -> Dict: present (this build has a co-processor), link (it
 * answered), host (esp-hosted here), version (esp-hosted on the C6), chip,
 * error (why it didn't answer). Brings the link up if it isn't: that can take
 * some seconds. */
void f_esp_c6(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    esp_net_cp_info_t i;
    char err[96] = "";
    esp_net_cp_info(&i, err, sizeof err);
    dict_T *d = rettv->vval.v_dict;
    dict_add_bool(d, "present", i.present);
    dict_add_bool(d, "link", i.link);
    dict_add_string(d, "host", (char_u *)i.host);
    dict_add_string(d, "version", (char_u *)i.version);
    dict_add_string(d, "chip", (char_u *)i.chip);
    dict_add_string(d, "error", (char_u *)err);
}

typedef struct {
    int64_t last;
} cp_progress_t;

/* On the command line, four times a second at most; CTRL-C stops it. */
static bool cp_progress(void *ctx, uint64_t bytes, uint64_t total)
{
    cp_progress_t *p = ctx;
    int64_t now = esp_timer_get_time();
    if (now - p->last >= 250000 || bytes == total) {
        p->last = now;
        if (msg_silent == 0) {
            char s[80];
            vim_snprintf(s, sizeof s, "C6 firmware: %llu of %llu KB (%d%%)",
                         (unsigned long long)(bytes / 1024), (unsigned long long)(total / 1024),
                         total ? (int)(bytes * 100 / total) : 0);
            msg_start();
            msg_puts(s);
            msg_clr_eos();
            out_flush();
        }
        ui_breakcheck();
    }
    return !got_int;
}

/* esp_c6_update({file}) -> Dict: from, to (versions), project, bytes,
 * activated; or {} after an error. {file} is the C6's app image (the
 * co-processor build's network_adapter.bin), made absolute against the
 * working directory. Restart afterwards: the link doesn't survive the C6
 * restarting (:EspC6 update does both). */
void f_esp_c6_update(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    char_u *file = tv_get_string_chk(&argvars[0]);
    char_u *full = file == NULL ? NULL : FullName_save(file, TRUE);
    if (full == NULL)
        return;
    char err[160];
    esp_net_cp_update_t u;
    cp_progress_t p = {0};
    if (esp_net_cp_update((char *)full, cp_progress, &p, &u, err, sizeof err) == 0) {
        dict_T *d = rettv->vval.v_dict;
        dict_add_string(d, "from", (char_u *)u.from);
        dict_add_string(d, "to", (char_u *)u.to);
        dict_add_string(d, "project", (char_u *)u.project);
        dict_add_number(d, "bytes", (varnumber_T)u.bytes);
        dict_add_bool(d, "activated", u.activated);
    } else {
        semsg("esp_c6_update(): %s", err);
    }
    vim_free(full);
}
