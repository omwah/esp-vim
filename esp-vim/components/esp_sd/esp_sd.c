/*
 * esp_sd: see include/esp_sd.h.
 *
 * ESP-IDF does the work: an SPI or SDMMC host driver, sdmmc's card protocol,
 * and esp_vfs_fat's mount of the card's FatFs volume at /sd. What is ours is
 * the policy -- never format, say why a mount failed -- and the status.
 */

#include "esp_sd.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_ESP_VIM_SD

#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_sdmmc.h"               /* after ff.h, whose types it uses */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

static SemaphoreHandle_t s_lock;
static sdmmc_card_t *s_card;
static char s_err[96];
static bool s_bus_ready;

#if CONFIG_ESP_VIM_SD_SPI
#define BUS_NAME "SPI"
#else
#define BUS_NAME "SDMMC 4-bit"
#endif

static void why(char *err, size_t errlen, esp_err_t e)
{
    const char *what;
    switch (e) {
    case ESP_FAIL:
        what = "the card has no FAT or exFAT filesystem, or it is damaged "
               "(left as it is: nothing was formatted)";
        break;
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_INVALID_RESPONSE:
    case ESP_ERR_INVALID_CRC:
    case ESP_ERR_NOT_SUPPORTED:
        what = "no card in the slot, or it does not answer";
        break;
    case ESP_ERR_NO_MEM:
        what = "out of memory";
        break;
    default:
        what = esp_err_to_name(e);
        break;
    }
    snprintf(err, errlen, "%s", what);
}

/* The mount itself, with s_lock held. */
static esp_err_t mount(void)
{
    esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = false,    /* never: it is someone's card */
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
#if CONFIG_ESP_VIM_SD_SPI
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = CONFIG_ESP_VIM_SD_MHZ * 1000;
    if (!s_bus_ready) {
        spi_bus_config_t bus = {
            .mosi_io_num = CONFIG_ESP_VIM_SD_SPI_MOSI,
            .miso_io_num = CONFIG_ESP_VIM_SD_SPI_MISO,
            .sclk_io_num = CONFIG_ESP_VIM_SD_SPI_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        esp_err_t e = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
        if (e != ESP_OK)
            return e;
        s_bus_ready = true;
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = CONFIG_ESP_VIM_SD_SPI_CS;
    slot.host_id = host.slot;
    return esp_vfs_fat_sdspi_mount(ESP_SD_PATH, &host, &slot, &mc, &s_card);
#else
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_ESP_VIM_SD_MHZ * 1000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = CONFIG_ESP_VIM_SD_MMC_CLK;
    slot.cmd = CONFIG_ESP_VIM_SD_MMC_CMD;
    slot.d0 = CONFIG_ESP_VIM_SD_MMC_D0;
    slot.d1 = CONFIG_ESP_VIM_SD_MMC_D1;
    slot.d2 = CONFIG_ESP_VIM_SD_MMC_D2;
    slot.d3 = CONFIG_ESP_VIM_SD_MMC_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    return esp_vfs_fat_sdmmc_mount(ESP_SD_PATH, &host, &slot, &mc, &s_card);
#endif
}

int esp_sd_mount(char *err, size_t errlen)
{
    if (s_lock == NULL)
        return snprintf(err, errlen, "not set up"), -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = 0;
    if (s_card == NULL) {
        esp_err_t e = mount();
        if (e != ESP_OK) {
            s_card = NULL;
            why(s_err, sizeof s_err, e);
            snprintf(err, errlen, "%s", s_err);
            rc = -1;
        } else {
            s_err[0] = '\0';
        }
    }
    xSemaphoreGive(s_lock);
    return rc;
}

int esp_sd_unmount(char *err, size_t errlen)
{
    if (s_lock == NULL)
        return snprintf(err, errlen, "not set up"), -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = 0;
    if (s_card == NULL) {
        snprintf(err, errlen, "no card is mounted");
        rc = -1;
    } else {
        esp_err_t e = esp_vfs_fat_sdcard_unmount(ESP_SD_PATH, s_card);
        if (e != ESP_OK) {
            snprintf(err, errlen, "%s", esp_err_to_name(e));
            rc = -1;
        } else {
            s_card = NULL;
        }
    }
    xSemaphoreGive(s_lock);
    return rc;
}

void esp_sd_init(void)
{
    char err[96];
    s_lock = xSemaphoreCreateMutex();
    if (s_lock != NULL)
        esp_sd_mount(err, sizeof err);  /* fine if there is no card */
}

static const char *fs_name(BYTE t)
{
    switch (t) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "exFAT";
    default:       return "?";
    }
}

void esp_sd_info(esp_sd_info_t *out)
{
    memset(out, 0, sizeof *out);
    out->configured = true;
    out->bus = BUS_NAME;
    out->type = "";
    out->fs = "";
    if (s_lock == NULL)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out->last_error, sizeof out->last_error, "%s", s_err);
    sdmmc_card_t *c = s_card;
    if (c != NULL) {
        out->mounted = true;
        memcpy(out->name, c->cid.name, sizeof c->cid.name);
        out->name[sizeof out->name - 1] = '\0';
        out->type = c->is_mmc ? "MMC" : c->is_sdio ? "SDIO"
                  : (c->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC";
        out->card_bytes = (uint64_t)c->csd.capacity * c->csd.sector_size;
        out->khz = c->real_freq_khz;
        esp_vfs_fat_info(ESP_SD_PATH, &out->total_bytes, &out->free_bytes);
        BYTE pdrv = ff_diskio_get_pdrv_card(c);
        char drv[3] = { (char)('0' + pdrv), ':', '\0' };
        FATFS *fs = NULL;
        DWORD nclst;
        if (pdrv != 0xff && f_getfree(drv, &nclst, &fs) == FR_OK && fs != NULL)
            out->fs = fs_name(fs->fs_type);
    }
    xSemaphoreGive(s_lock);
}

#else   /* no SD slot in this build */

void esp_sd_init(void) {}

int esp_sd_mount(char *err, size_t errlen)
{
    return snprintf(err, errlen, "this board has no SD card slot"), -1;
}

int esp_sd_unmount(char *err, size_t errlen)
{
    return snprintf(err, errlen, "this board has no SD card slot"), -1;
}

void esp_sd_info(esp_sd_info_t *out)
{
    memset(out, 0, sizeof *out);
    out->type = out->fs = out->bus = "";
}

#endif
