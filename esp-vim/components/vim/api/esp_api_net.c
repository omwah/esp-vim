/*
 * esp_net_status(), esp_http_get(): Vim script access to components/esp_net.
 * Behind :EspNet, :EspGet, and netrw's http:// and https:// reads (patch 0009),
 * which is also how spell files are downloaded.
 */

#include "vim.h"
#include "esp_vim_api.h"
#include "esp_net.h"

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
