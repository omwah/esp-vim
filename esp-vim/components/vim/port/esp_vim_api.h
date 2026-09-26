/*
 * The esp_*() Vim script builtins: the C half of the :Esp* commands.
 *
 * Registered in Vim's builtin table by patches/vim/0008-evalfunc-esp-builtins,
 * implemented in components/vim/api/. Each returns plain data (a Number, a
 * String, a Dict or a List of Dicts); formatting it for people is the job of
 * runtime-image/autoload/esp.vim, so the commands can change without a reflash.
 *
 * Only included by evalfunc.c, after vim.h.
 */

#ifndef ESP_VIM_API_H
#define ESP_VIM_API_H

/* api/esp_api_ble.c (over components/esp_ble) */
void f_esp_ble_scan(typval_T *argvars, typval_T *rettv);
void f_esp_bt_keyboard(typval_T *argvars, typval_T *rettv);
void f_esp_bt_keyboard_forget(typval_T *argvars, typval_T *rettv);
void f_esp_bt_keyboard_pair(typval_T *argvars, typval_T *rettv);

/* api/esp_api_sys.c: the serial console's output */
void f_esp_console_output(typval_T *argvars, typval_T *rettv);
void f_esp_display(typval_T *argvars, typval_T *rettv);
void f_esp_display_font(typval_T *argvars, typval_T *rettv);

/* api/esp_api_fs.c (over components/esp_fs) */
void f_esp_fs_copy(typval_T *argvars, typval_T *rettv);
void f_esp_fs_delete(typval_T *argvars, typval_T *rettv);
void f_esp_fs_info(typval_T *argvars, typval_T *rettv);
void f_esp_fs_list(typval_T *argvars, typval_T *rettv);
void f_esp_fs_mkdir(typval_T *argvars, typval_T *rettv);
void f_esp_fs_move(typval_T *argvars, typval_T *rettv);
void f_esp_fs_roots(typval_T *argvars, typval_T *rettv);

/* api/esp_api_net.c (over components/esp_net) */
void f_esp_http_get(typval_T *argvars, typval_T *rettv);
void f_esp_net_status(typval_T *argvars, typval_T *rettv);
void f_esp_wifi_connect(typval_T *argvars, typval_T *rettv);
void f_esp_wifi_disconnect(typval_T *argvars, typval_T *rettv);
void f_esp_wifi_forget(typval_T *argvars, typval_T *rettv);
void f_esp_wifi_saved(typval_T *argvars, typval_T *rettv);
void f_esp_wifi_scan(typval_T *argvars, typval_T *rettv);

/* api/esp_api_ssh.c (over components/esp_ssh) */
void f_esp_ssh_get(typval_T *argvars, typval_T *rettv);
void f_esp_ssh_hostkey(typval_T *argvars, typval_T *rettv);
void f_esp_ssh_keygen(typval_T *argvars, typval_T *rettv);
void f_esp_ssh_list(typval_T *argvars, typval_T *rettv);
void f_esp_ssh_mkdir(typval_T *argvars, typval_T *rettv);
void f_esp_ssh_put(typval_T *argvars, typval_T *rettv);
void f_esp_ssh_remove(typval_T *argvars, typval_T *rettv);
void f_esp_ssh_rename(typval_T *argvars, typval_T *rettv);
void f_esp_ssh_trust(typval_T *argvars, typval_T *rettv);

/* api/esp_api_web.c (over components/esp_web) */
void f_esp_settings(typval_T *argvars, typval_T *rettv);
void f_esp_web_info(typval_T *argvars, typval_T *rettv);
void f_esp_web_passwd(typval_T *argvars, typval_T *rettv);
void f_esp_web_publish(typval_T *argvars, typval_T *rettv);
void f_esp_web_settings_changed(typval_T *argvars, typval_T *rettv);
void f_esp_web_start(typval_T *argvars, typval_T *rettv);
void f_esp_web_stop(typval_T *argvars, typval_T *rettv);

/* api/esp_api_sys.c */
void f_esp_heap(typval_T *argvars, typval_T *rettv);
void f_esp_info(typval_T *argvars, typval_T *rettv);
void f_esp_reboot(typval_T *argvars, typval_T *rettv);
void f_esp_tasks(typval_T *argvars, typval_T *rettv);

/* api/esp_api_nvs.c */
void f_esp_nvs_erase(typval_T *argvars, typval_T *rettv);
void f_esp_nvs_get(typval_T *argvars, typval_T *rettv);
void f_esp_nvs_list(typval_T *argvars, typval_T *rettv);
void f_esp_nvs_set(typval_T *argvars, typval_T *rettv);

/* api/esp_api_hw.c */
void f_esp_adc_read(typval_T *argvars, typval_T *rettv);
void f_esp_i2c_scan(typval_T *argvars, typval_T *rettv);
void f_esp_sensors(typval_T *argvars, typval_T *rettv);
void f_esp_serial_close(typval_T *argvars, typval_T *rettv);
void f_esp_serial_open(typval_T *argvars, typval_T *rettv);
void f_esp_serial_read(typval_T *argvars, typval_T *rettv);
void f_esp_serial_write(typval_T *argvars, typval_T *rettv);

/* api/esp_api_gpio.c */
bool esp_api_gpio_usable(int pin);      /* also used by esp_api_hw.c */
void f_esp_gpio_mode(typval_T *argvars, typval_T *rettv);
void f_esp_gpio_pins(typval_T *argvars, typval_T *rettv);
void f_esp_gpio_read(typval_T *argvars, typval_T *rettv);
void f_esp_gpio_write(typval_T *argvars, typval_T *rettv);

#endif /* ESP_VIM_API_H */
