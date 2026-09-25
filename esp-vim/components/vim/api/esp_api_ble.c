/*
 * esp_ble_scan() and the esp_bt_keyboard*() functions: Vim script access to
 * components/esp_ble. Behind :EspBleScan and :EspBtKeyboard.
 */

#include "vim.h"
#include "esp_vim_api.h"
#include "esp_ble.h"

static bool add_dev(void *ctx, const esp_ble_dev_t *dev)
{
    dict_T *d = dict_alloc();
    if (d == NULL)
        return false;
    dict_add_string(d, "addr", (char_u *)dev->addr);
    dict_add_string(d, "addr_type", (char_u *)dev->addr_type);
    dict_add_string(d, "name", (char_u *)dev->name);
    dict_add_number(d, "rssi", dev->rssi);
    dict_add_bool(d, "connectable", dev->connectable);
    dict_add_bool(d, "hid", dev->hid);
    dict_add_number(d, "appearance", dev->appearance);
    dict_add_string(d, "adv", (char_u *)dev->adv);
    return list_append_dict((list_T *)ctx, d) == OK;
}

/* esp_ble_scan([{seconds}]) -> List of Dicts: addr, addr_type, name, rssi,
 * connectable, hid. Scans for {seconds} (default 5, at most 30). */
void f_esp_ble_scan(typval_T *argvars, typval_T *rettv)
{
    if (rettv_list_alloc(rettv) == FAIL)
        return;
    varnumber_T secs = argvars[0].v_type == VAR_UNKNOWN ? 5 : tv_get_number(&argvars[0]);
    if (secs < 1 || secs > 30) {
        emsg("esp_ble_scan(): scan for 1 to 30 seconds");
        return;
    }
    char err[128];
    if (esp_ble_scan((unsigned)secs * 1000, add_dev, rettv->vval.v_list, err, sizeof err) != 0)
        semsg("esp_ble_scan(): %s", err);
}

/* esp_bt_keyboard() -> Dict: connected, addr, name, battery, paired,
 * last_report. */
void f_esp_bt_keyboard(typval_T *argvars UNUSED, typval_T *rettv)
{
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    esp_ble_kbd_status_t st;
    esp_ble_kbd_status(&st);
    dict_T *d = rettv->vval.v_dict;
    dict_add_bool(d, "connected", st.connected);
    dict_add_string(d, "addr", (char_u *)st.addr);
    dict_add_string(d, "name", (char_u *)st.name);
    dict_add_number(d, "battery", st.battery);
    dict_add_bool(d, "paired", st.paired);
    dict_add_string(d, "last_report", (char_u *)st.last_report);
    dict_add_string(d, "security", (char_u *)st.security);
}

/* esp_bt_keyboard_pair({addr} [, {addr_type}]): connect to the keyboard and
 * bond with it; {addr_type} is "public" (default) or "random", as
 * esp_ble_scan() reports it. */
void f_esp_bt_keyboard_pair(typval_T *argvars, typval_T *rettv)
{
    char_u buf[NUMBUFLEN];
    char_u *addr = tv_get_string_buf_chk(&argvars[0], buf);
    char_u *type = argvars[1].v_type == VAR_UNKNOWN ? (char_u *)"public"
                                                     : tv_get_string_chk(&argvars[1]);
    if (addr == NULL || type == NULL)
        return;
    char err[128];
    if (esp_ble_kbd_pair((char *)addr, STRCMP(type, "random") == 0, err, sizeof err) == 0)
        rettv->vval.v_number = TRUE;
    else
        semsg("esp_bt_keyboard_pair(): %s", err);
}

/* esp_bt_keyboard_forget(): disconnect and forget bonded keyboards. */
void f_esp_bt_keyboard_forget(typval_T *argvars UNUSED, typval_T *rettv)
{
    char err[128];
    if (esp_ble_kbd_forget(err, sizeof err) == 0)
        rettv->vval.v_number = TRUE;
    else
        semsg("esp_bt_keyboard_forget(): %s", err);
}
