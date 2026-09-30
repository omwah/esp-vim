/*
 * esp_sd(), esp_sd_mount(), esp_sd_eject(): the microSD card at /sd, behind
 * :EspSd (over components/esp_sd).
 */

#include "vim.h"
#include "esp_vim_api.h"
#include "esp_sd.h"

/* esp_sd() -> Dict: configured, mounted, name, type, card_bytes, fs,
 * total, free, bus, khz, error (why the last mount failed). */
void f_esp_sd(typval_T *argvars UNUSED, typval_T *rettv)
{
    esp_sd_info_t s;
    if (rettv_dict_alloc(rettv) == FAIL)
        return;
    esp_sd_info(&s);
    dict_T *d = rettv->vval.v_dict;
    dict_add_bool(d, "configured", s.configured);
    dict_add_bool(d, "mounted", s.mounted);
    dict_add_string(d, "name", (char_u *)s.name);
    dict_add_string(d, "type", (char_u *)s.type);
    dict_add_number(d, "card_bytes", (varnumber_T)s.card_bytes);
    dict_add_string(d, "fs", (char_u *)s.fs);
    dict_add_number(d, "total", (varnumber_T)s.total_bytes);
    dict_add_number(d, "free", (varnumber_T)s.free_bytes);
    dict_add_string(d, "bus", (char_u *)s.bus);
    dict_add_number(d, "khz", s.khz);
    dict_add_string(d, "error", (char_u *)s.last_error);
}

/* esp_sd_mount(): mount the card at /sd; TRUE, or an error saying why not. */
void f_esp_sd_mount(typval_T *argvars UNUSED, typval_T *rettv)
{
    char err[96];
    if (esp_sd_mount(err, sizeof err) == 0)
        rettv->vval.v_number = TRUE;
    else
        semsg("esp_sd_mount(): %s", err);
}

/* esp_sd_eject(): unmount /sd so the card can come out. Refused while a
 * buffer with unsaved changes is on the card. */
void f_esp_sd_eject(typval_T *argvars UNUSED, typval_T *rettv)
{
    buf_T *buf;
    char err[96];

    FOR_ALL_BUFFERS(buf)
	if (bufIsChanged(buf) && buf->b_ffname != NULL
		&& STRNCMP(buf->b_ffname, ESP_SD_PATH "/", sizeof(ESP_SD_PATH)) == 0)
	{
	    semsg("esp_sd_eject(): %s has unsaved changes; write it or :bwipe! it first",
		  buf->b_ffname);
	    return;
	}
    if (esp_sd_unmount(err, sizeof err) == 0)
        rettv->vval.v_number = TRUE;
    else
        semsg("esp_sd_eject(): %s", err);
}
