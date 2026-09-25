/*
 * esp_web_*(), esp_settings(): Vim script access to components/esp_web, the
 * web interface. Behind :EspWebStart, :EspWebStop, :EspWebStatus and
 * :EspWebPasswd, and autoload/esp/web.vim's once-a-second tick that publishes
 * the editing status and applies settings changed in the browser.
 */

#include "vim.h"
#include "esp_vim_api.h"
#include "esp_web.h"

static void info_dict(dict_T *d)
{
    esp_web_info_t i;
    esp_web_get_info(&i);
    dict_add_bool(d, "running", i.running);
    dict_add_number(d, "port", i.port);
    dict_add_bool(d, "password_set", i.password_set);
    dict_add_number(d, "sessions", i.sessions);
    dict_add_string(d, "fingerprint", (char_u *)i.fingerprint);
}

/* esp_web_start([{port}]) -> Dict as esp_web_info(), or {} after an error. */
void f_esp_web_start(typval_T *argvars, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    int port = argvars[0].v_type == VAR_UNKNOWN ? ESP_WEB_DEFAULT_PORT
             : (int)tv_get_number(&argvars[0]);
    if (port <= 0 || port > 65535) {
        semsg("esp_web_start(): bad port %d", port);
        return;
    }
    char err[160];
    if (esp_web_start(port, err, sizeof err) != 0) {
        semsg("esp_web_start(): %s", err);
        return;
    }
    info_dict(rettv->vval.v_dict);
}

void f_esp_web_stop(typval_T *argvars UNUSED, typval_T *rettv UNUSED)
{
    esp_web_stop();
}

/* esp_web_info() -> Dict: running, port, password_set, sessions, fingerprint. */
void f_esp_web_info(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    info_dict(rettv->vval.v_dict);
}

/* esp_web_passwd({password}): set the web password; TRUE on success. */
void f_esp_web_passwd(typval_T *argvars, typval_T *rettv)
{
    char_u *pw = tv_get_string_chk(&argvars[0]);
    if (pw == NULL)
        return;
    char err[160];
    if (esp_web_set_password((char *)pw, err, sizeof err) == 0)
        rettv->vval.v_number = TRUE;
    else
        semsg("esp_web_passwd(): %s", err);
}

static long num_item(dict_T *d, char *key)
{
    return (long)dict_get_number(d, key);
}

/* esp_web_publish({status}): what is being edited, for the web page. Keys:
 * file, filetype, mode, line, col, lines, words, chars, bytes, modified,
 * buffers. */
void f_esp_web_publish(typval_T *argvars, typval_T *rettv UNUSED)
{
    if (argvars[0].v_type != VAR_DICT || argvars[0].vval.v_dict == NULL) {
        emsg("esp_web_publish(): expected a Dict");
        return;
    }
    dict_T *d = argvars[0].vval.v_dict;
    esp_web_status_t st;
    memset(&st, 0, sizeof st);
    char_u *s;
    if ((s = dict_get_string(d, "file", FALSE)) != NULL)
        vim_strncpy((char_u *)st.file, s, sizeof st.file - 1);
    if ((s = dict_get_string(d, "filetype", FALSE)) != NULL)
        vim_strncpy((char_u *)st.filetype, s, sizeof st.filetype - 1);
    if ((s = dict_get_string(d, "mode", FALSE)) != NULL)
        vim_strncpy((char_u *)st.mode, s, sizeof st.mode - 1);
    st.line = num_item(d, "line");
    st.col = num_item(d, "col");
    st.lines = num_item(d, "lines");
    st.words = num_item(d, "words");
    st.chars = num_item(d, "chars");
    st.bytes = num_item(d, "bytes");
    st.modified = num_item(d, "modified") != 0;
    st.buffers = (int)num_item(d, "buffers");
    esp_web_publish(&st);
}

/* esp_web_settings_changed() -> TRUE once after the browser saved settings. */
void f_esp_web_settings_changed(typval_T *argvars UNUSED, typval_T *rettv)
{
    rettv->vval.v_number = esp_web_settings_changed();
}

/* esp_settings() -> Dict of the stored editor settings: tabstop, shiftwidth,
 * expandtab, number, relativenumber, wrap, colorscheme, background. */
void f_esp_settings(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    esp_web_settings_t s;
    esp_web_settings_get(&s);
    dict_T *d = rettv->vval.v_dict;
    dict_add_number(d, "tabstop", s.tabstop);
    dict_add_number(d, "shiftwidth", s.shiftwidth);
    dict_add_bool(d, "expandtab", s.expandtab);
    dict_add_bool(d, "number", s.number);
    dict_add_bool(d, "relativenumber", s.relativenumber);
    dict_add_bool(d, "wrap", s.wrap);
    dict_add_string(d, "colorscheme", (char_u *)s.colorscheme);
    dict_add_string(d, "background", (char_u *)s.background);
}
