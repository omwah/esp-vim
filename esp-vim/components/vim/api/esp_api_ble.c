/*
 * esp_ble_scan(): Vim script access to components/esp_ble. Behind :EspBleScan.
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
    return list_append_dict((list_T *)ctx, d) == OK;
}

/* esp_ble_scan([{seconds}]) -> List of Dicts: addr, addr_type, name, rssi,
 * connectable. Scans for {seconds} (default 5, at most 30). */
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
