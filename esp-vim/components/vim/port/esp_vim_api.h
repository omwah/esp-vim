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

/* api/esp_api_gpio.c */
void f_esp_gpio_mode(typval_T *argvars, typval_T *rettv);
void f_esp_gpio_pins(typval_T *argvars, typval_T *rettv);
void f_esp_gpio_read(typval_T *argvars, typval_T *rettv);
void f_esp_gpio_write(typval_T *argvars, typval_T *rettv);

#endif /* ESP_VIM_API_H */
