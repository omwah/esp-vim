/*
 * esp_net: the radio co-processor (see include/esp_net.h): which esp-hosted it
 * runs, and updating its firmware.
 *
 * The update is esp-hosted's own: the image goes to the C6 over the link in
 * chunks (esp_hosted_slave_ota_begin/write/end), the C6 writes it to its
 * other app slot and checks it, and activating it makes the C6 boot it
 * (firmware older than 2.6 has no activate call and boots it on its next
 * reset). The link doesn't survive the C6 restarting, so the caller restarts
 * this chip afterwards; esp-hosted resets the C6 again as it starts.
 */

#include "esp_net.h"

#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#if CONFIG_ESP_VIM_NET_WIFI_REMOTE

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_fs.h"
#include "esp_hosted.h"
#include "esp_log.h"

static const char *TAG = "esp_net_cp";

#define CHUNK       1500                /* esp-hosted's example's; one RPC each */

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return -1;
}

static void host_version(esp_net_cp_info_t *info)
{
    snprintf(info->host, sizeof info->host, "%d.%d.%d", ESP_HOSTED_VERSION_MAJOR_1,
             ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
}

/* The link, brought up if it isn't: esp-hosted resets the C6 and waits for
 * it. Then the C6's firmware version and chip. */
static int cp_query(esp_net_cp_info_t *info, esp_hosted_coprocessor_fwver_t *ver,
                    char *err, size_t errlen)
{
    esp_hosted_connect_to_slave();
    if (esp_hosted_get_coprocessor_fwversion(ver) != ESP_OK)
        return snprintf(err, errlen, "no answer from the co-processor"), -1;
    info->link = true;
    snprintf(info->version, sizeof info->version, "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
             ver->major1, ver->minor1, ver->patch1);
    uint32_t id = 0;
    if (esp_hosted_get_cp_info(&id, info->chip, sizeof info->chip) != ESP_OK)
        info->chip[0] = 0;
    return 0;
}

int esp_net_cp_info(esp_net_cp_info_t *info, char *err, size_t errlen)
{
    memset(info, 0, sizeof *info);
    info->present = true;
    host_version(info);
    esp_hosted_coprocessor_fwver_t ver;
    return cp_query(info, &ver, err, errlen);
}

/*
 * What {path} holds: an ESP32-C6 app image (the build's network_adapter.bin),
 * not a merged flash image or another chip's app. The app description is the
 * first segment's start.
 */
static int check_image(FILE *f, esp_net_cp_update_t *out, char *err, size_t errlen)
{
    esp_image_header_t h;
    esp_image_segment_header_t seg;
    esp_app_desc_t app;
    if (fread(&h, sizeof h, 1, f) != 1 || h.magic != ESP_IMAGE_HEADER_MAGIC)
        return snprintf(err, errlen, "not an ESP firmware image"), -1;
    if (h.chip_id != ESP_CHIP_ID_ESP32C6)
        return snprintf(err, errlen, "an image for another chip (id %u), not the ESP32-C6",
                        (unsigned)h.chip_id), -1;
    if (fread(&seg, sizeof seg, 1, f) != 1 || fread(&app, sizeof app, 1, f) != 1
            || app.magic_word != ESP_APP_DESC_MAGIC_WORD)
        return snprintf(err, errlen, "not an app image (a merged or bootloader image?): "
                        "use the build's network_adapter.bin"), -1;
    snprintf(out->to, sizeof out->to, "%.*s", (int)sizeof app.version, app.version);
    snprintf(out->project, sizeof out->project, "%.*s", (int)sizeof app.project_name,
             app.project_name);
    return 0;
}

int esp_net_cp_update(const char *path, esp_net_cp_progress_cb progress, void *ctx,
                      esp_net_cp_update_t *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    char full[256];
    esp_fs_err_t ferr;
    if (esp_fs_check(path, false, full, sizeof full, &ferr) != 0)
        return snprintf(err, errlen, "%s", ferr.msg), -1;
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
        return snprintf(err, errlen, "%s: no such file", full), -1;
    FILE *f = fopen(full, "rb");
    if (f == NULL)
        return snprintf(err, errlen, "%s: cannot open it", full), -1;
    uint8_t *buf = NULL;
    int rc = check_image(f, out, err, errlen);
    if (rc != 0)
        goto done;

    esp_net_cp_info_t info = {0};
    esp_hosted_coprocessor_fwver_t ver;
    if ((rc = cp_query(&info, &ver, err, errlen)) != 0)
        goto done;
    snprintf(out->from, sizeof out->from, "%s", info.version);
    if (info.chip[0] && strcmp(info.chip, "esp32c6") != 0) {
        rc = fail(err, errlen, "the co-processor is an %s, not an ESP32-C6", info.chip);
        goto done;
    }

    buf = malloc(CHUNK);
    if (buf == NULL) {
        rc = fail(err, errlen, "out of memory");
        goto done;
    }
    rewind(f);
    ESP_LOGI(TAG, "%s (%s %s, %ld bytes) -> the co-processor, which runs %s", full,
             out->project, out->to, (long)st.st_size, out->from);
    esp_err_t e = esp_hosted_slave_ota_begin();
    if (e != ESP_OK) {
        rc = fail(err, errlen, "the co-processor refused the update: %s", esp_err_to_name(e));
        goto done;
    }
    /* Stopping part way leaves the C6 on the firmware it runs: the slot being
     * written isn't the one it boots until the image is complete and checked. */
    size_t n;
    while ((n = fread(buf, 1, CHUNK, f)) > 0) {
        if ((e = esp_hosted_slave_ota_write(buf, n)) != ESP_OK) {
            rc = fail(err, errlen, "writing at %llu: %s", (unsigned long long)out->bytes,
                          esp_err_to_name(e));
            break;
        }
        out->bytes += n;
        if (progress && !progress(ctx, out->bytes, (uint64_t)st.st_size)) {
            rc = fail(err, errlen, "stopped at %llu of %ld bytes: the co-processor keeps %s",
                          (unsigned long long)out->bytes, (long)st.st_size, out->from);
            break;
        }
    }
    e = esp_hosted_slave_ota_end();
    if (rc != 0)
        goto done;
    if (ferror(f) || out->bytes != (uint64_t)st.st_size) {
        rc = fail(err, errlen, "%s: read error", full);
        goto done;
    }
    if (e != ESP_OK) {
        rc = fail(err, errlen, "the co-processor rejected the image: %s", esp_err_to_name(e));
        goto done;
    }
    /* activate() is 2.6's; before it, ota_end() already made the new slot the
     * one to boot, and the next reset of the C6 starts it. */
    if (ESP_HOSTED_VERSION_VAL(ver.major1, ver.minor1, ver.patch1) >= ESP_HOSTED_VERSION_VAL(2, 6, 0)) {
        e = esp_hosted_slave_ota_activate();
        if (e != ESP_OK) {
            rc = fail(err, errlen, "written, but activating it failed: %s", esp_err_to_name(e));
            goto done;
        }
        out->activated = true;
    }
    ESP_LOGI(TAG, "co-processor firmware %s written%s", out->to, out->activated ? ", activated" : "");
done:
    free(buf);
    fclose(f);
    return rc;
}

#else   /* no co-processor */

int esp_net_cp_info(esp_net_cp_info_t *info, char *err, size_t errlen)
{
    memset(info, 0, sizeof *info);
    return 0;
}

int esp_net_cp_update(const char *path, esp_net_cp_progress_cb progress, void *ctx,
                      esp_net_cp_update_t *out, char *err, size_t errlen)
{
    (void)path; (void)progress; (void)ctx;
    memset(out, 0, sizeof *out);
    return snprintf(err, errlen, "this build has no radio co-processor"), -1;
}

#endif
