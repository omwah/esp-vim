/*
 * esp_sd: the board's microSD card, mounted at /sd.
 *
 * FAT12/16/32 or exFAT (ESP-IDF's FatFs with exFAT turned on,
 * patches/esp-idf/0002). A card is never formatted: one that won't mount is
 * reported, with why, and left as it is. Mounted at boot when a card is in;
 * esp_sd_unmount() before taking it out, esp_sd_mount() after putting one in
 * (the slots on these boards have no card-detect line).
 *
 * Its own component: the mount outlives Vim sessions. Any task.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_SD_PATH "/sd"

typedef struct {
    bool configured;            /* this build has an SD slot */
    bool mounted;
    char name[8];               /* the card's product name */
    const char *type;           /* "SDSC", "SDHC/SDXC", "MMC", "SDIO" */
    uint64_t card_bytes;        /* the card's capacity */
    const char *fs;             /* "FAT12", "FAT16", "FAT32", "exFAT" */
    uint64_t total_bytes, free_bytes;   /* of the filesystem */
    const char *bus;            /* "SPI", "SDMMC 4-bit" */
    int khz;                    /* the clock the card runs at */
    char last_error[96];        /* why the last mount failed, "" if it didn't */
} esp_sd_info_t;

/* At boot: set the slot up and mount a card if one is in. Never fails the
 * boot: a missing or unreadable card is recorded in last_error. */
void esp_sd_init(void);

/* Mount the card at /sd. 0, or -1 with why in {err}. */
int esp_sd_mount(char *err, size_t errlen);

/* Unmount it (before the card comes out). 0, or -1 with why. */
int esp_sd_unmount(char *err, size_t errlen);

void esp_sd_info(esp_sd_info_t *out);

#ifdef __cplusplus
}
#endif
