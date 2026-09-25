/* Shared between esp_ble.c (the stack and scanning) and esp_ble_kbd.c. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern SemaphoreHandle_t esp_ble__lock;     /* one scan, pairing or reconnect at a time */

bool esp_ble__init_locks(void);
int esp_ble__start(char *err, size_t errlen);   /* with esp_ble__lock held */
uint8_t esp_ble__own_addr_type(void);

/* esp_ble_kbd.c */
void esp_ble__config_security(void);
void esp_ble__cancel_connect(void);
