/*
 * esp_power: see include/esp_power.h.
 *
 * A task looks every 100 ms at the wake button and the idle clock, and every
 * 30 s at the battery. Sleep itself runs on whichever task asks for it -- that
 * task, or Vim's for :EspSleep -- one at a time. Everything else stops with
 * the chip meanwhile, Vim included (it's waiting for a key anyway).
 */

#include "esp_power.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
# include "driver/usb_serial_jtag.h"
#endif

#include "esp_ble.h"
#include "esp_display.h"
#include "esp_net.h"
#include "esp_touch.h"

static const char *TAG = "esp_power";

#define KEY         CONFIG_ESP_VIM_WAKE_KEY_GPIO
#define BAT         CONFIG_ESP_VIM_BAT_ADC_GPIO
#if BAT >= 0 || CONFIG_ESP_VIM_BAT_INA226
# define HAVE_BAT   1
#else
# define HAVE_BAT   0
#endif
#define NVS_NS      "esp_power"
#define POLL_MS     100
#define BAT_EVERY_US (30 * 1000000LL)
#define US_PER_MIN  (60 * 1000000LL)

static SemaphoreHandle_t s_sleep_lock;  /* one sleep at a time */
/* Sleep is always entered on the power task: its stack is in internal RAM,
 * which deep sleep with held pins requires (and a task whose stack is in PSRAM
 * -- Vim's, where the program runs from PSRAM -- can't provide). Others ask. */
static TaskHandle_t s_power_task;
static struct { volatile bool pending; bool deep; int wake_s; bool unsaved; } s_req;
static SemaphoreHandle_t s_req_done;
static int s_idle_min, s_deep_min;
static volatile int s_mv = -1;          /* the battery, last measured */
static int64_t s_mv_at;

static volatile bool s_waiting, s_unsaved;      /* Vim's, esp_power_vim_waiting() */
static volatile int64_t s_last_input;
static int64_t s_busy_since;

static unsigned s_sleeps, s_presses;
static volatile bool s_key_used;        /* the press that woke it: not one to sleep */
static RTC_DATA_ATTR unsigned s_deep_sleeps;    /* kept through deep sleep */
static const char *s_last_wake = "";

/* ------------------------------------------------------------- battery -- */

#if CONFIG_ESP_VIM_BAT_INA226

/* The battery's voltage, mV, from the INA226 on the board's I2C bus (the
 * Tab5's): its bus voltage register, 1.25 mV a step. */
static int battery_mv(void)
{
    i2c_master_bus_handle_t bus = esp_touch_i2c_bus(CONFIG_ESP_VIM_I2C_SDA, CONFIG_ESP_VIM_I2C_SCL);
    if (bus == NULL)
        return -1;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                               .device_address = CONFIG_ESP_VIM_BAT_INA226_ADDR,
                               .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK)
        return s_mv;
    uint8_t reg = 0x02, v[2];
    esp_err_t e = i2c_master_transmit_receive(dev, &reg, 1, v, 2, 50);
    i2c_master_bus_rm_device(dev);
    return e == ESP_OK ? (int)((v[0] << 8 | v[1]) * 125 / 100) : -1;
}

#elif BAT >= 0

/* The battery's voltage, mV, through its divider; the last one if the ADC is
 * busy (:EspAdc has it). */
static int battery_mv(void)
{
    adc_unit_t unit;
    adc_channel_t ch;
    if (adc_oneshot_io_to_channel(BAT, &unit, &ch) != ESP_OK)
        return -1;
    adc_oneshot_unit_handle_t adc;
    adc_oneshot_unit_init_cfg_t u = { .unit_id = unit };
    if (adc_oneshot_new_unit(&u, &adc) != ESP_OK)
        return s_mv;
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(adc, ch, &c);
    adc_cali_handle_t cali = NULL;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cc = { .unit_id = unit, .chan = ch,
                                           .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_cali_create_scheme_curve_fitting(&cc, &cali);
#endif
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(adc, ch, &raw) != ESP_OK)
            continue;
        if (cali == NULL || adc_cali_raw_to_voltage(cali, raw, &mv) != ESP_OK)
            mv = raw * 3100 / 4095;     /* uncalibrated: roughly, at 12 dB */
        sum += mv;
        n++;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (cali)
        adc_cali_delete_scheme_curve_fitting(cali);
#endif
    adc_oneshot_del_unit(adc);
    return n ? (int)((long long)sum / n * CONFIG_ESP_VIM_BAT_DIVIDER / 1000) : -1;
}

#else

static int battery_mv(void)
{
    return -1;
}

#endif

static int measure(void)
{
    s_mv = battery_mv();
    s_mv_at = esp_timer_get_time();
    return s_mv;
}

/* A lithium battery's charge from its resting voltage, roughly: per cell, for
 * ESP_VIM_BAT_CELLS in series. */
static int percent(int mv)
{
    if (mv > 0)
        mv /= CONFIG_ESP_VIM_BAT_CELLS;
    static const int curve[][2] = {
        { 4200, 100 }, { 4100, 90 }, { 4000, 80 }, { 3900, 66 }, { 3800, 50 },
        { 3700, 32 }, { 3600, 18 }, { 3500, 9 }, { 3400, 4 }, { 3300, 0 },
    };
    if (mv < 0)
        return -1;
    if (mv >= curve[0][0])
        return 100;
    for (size_t i = 1; i < sizeof curve / sizeof curve[0]; i++)
        if (mv >= curve[i][0])
            return curve[i][1] + (mv - curve[i][0]) * (curve[i - 1][1] - curve[i][1])
                                 / (curve[i - 1][0] - curve[i][0]);
    return 0;
}

/* Where the power comes from. The USB port powers the board whenever it's
 * plugged in, but only a computer on it can be seen (by its USB traffic); a
 * charger can't, and counts as battery. */
static const char *source(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (usb_serial_jtag_is_connected())
        return "usb";
    if (s_mv > 0)
        return "battery";
#endif
    return "unknown";
}

static bool on_battery(void)
{
    return strcmp(source(), "battery") == 0;
}

static bool battery_low(void)
{
    return CONFIG_ESP_VIM_SLEEP_LOW_MV > 0 && on_battery() && s_mv > 0
           && s_mv < CONFIG_ESP_VIM_SLEEP_LOW_MV;
}

/* ---------------------------------------------------------- deep sleep -- */

/* The sleep screen: a hippo, and something to say. */
static const char *const s_hippo[] = {
    "      _                        _",
    "     ( `.    .------------.   .' )",
    "      `. `. /              \\ .' .'",
    "        `--|                |--'            z",
    "           |   '-'    '-'   |           z",
    "          /                  \\       Z",
    "         /                    \\",
    "        |    .------------.    |",
    "        |   /   ()    ()   \\   |",
    "        |   \\              /   |",
    "         \\   '------------'   /",
    "          '-.______________.-'",
};
#define HIPPO_LINES (sizeof s_hippo / sizeof s_hippo[0])

static const char *const s_quips[] = {
    "Out cold. Press KEY if it's important.",
    "Deep sleep. Like the river, only drier.",
    "Hippos sleep 16 hours a day. I'm catching up.",
    "Do not disturb. Unless it's watermelon.",
    "Gone wallowing. KEY brings me back.",
    "Not lazy. Energy-efficient.",
    "Your words will keep. So will I.",
    "Shh. Dreaming in monospace.",
};
static const char *const s_quips_low[] = {
    "Battery low. Feed me USB.",
    "Running on fumes. And pride.",
};

/* /fat/sleep.pbm, if there is one: a binary PBM (P4), at most the screen's
 * size. NULL otherwise; the caller frees it. */
static uint8_t *load_pbm(int *w, int *h)
{
    FILE *f = fopen("/fat/sleep.pbm", "rb");
    if (f == NULL)
        return NULL;
    int v[2], n = 0, c;
    if (fgetc(f) != 'P' || fgetc(f) != '4')
        goto bad;
    while (n < 2) {                     /* width and height, maybe with # comments */
        c = fgetc(f);
        if (c == '#')
            while ((c = fgetc(f)) != '\n' && c != EOF)
                ;
        if (c == EOF)
            goto bad;
        if (c >= '0' && c <= '9') {
            ungetc(c, f);
            if (fscanf(f, "%d", &v[n++]) != 1)
                goto bad;
        }
    }
    fgetc(f);                           /* one whitespace byte, then the bits */
    if (v[0] <= 0 || v[1] <= 0 || v[0] > 2048 || v[1] > 2048)
        goto bad;
    size_t size = (size_t)(v[0] + 7) / 8 * v[1];
    uint8_t *bits = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bits == NULL || fread(bits, 1, size, f) != size) {
        free(bits);
        goto bad;
    }
    fclose(f);
    *w = v[0];
    *h = v[1];
    return bits;
bad:
    ESP_LOGW(TAG, "/fat/sleep.pbm: not a binary PBM (P4)");
    fclose(f);
    return NULL;
}

#if KEY >= 0
/* The button as a plain input, kept so through light sleep's GPIO isolation. */
static void key_as_gpio(void)
{
    if (rtc_gpio_is_valid_gpio(KEY))
        rtc_gpio_deinit(KEY);
    gpio_config_t g = { .pin_bit_mask = 1ULL << KEY, .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&g);
    gpio_sleep_sel_dis(KEY);
}
#endif

static void wait_key_released(void)
{
#if KEY >= 0
    while (gpio_get_level(KEY) == 0)
        vTaskDelay(pdMS_TO_TICKS(20));
#endif
}

/* Everything off; a boot follows the wake button, a timer, or a reset. */
static void deep_sleep(int wake_s)
{
    esp_ble_kbd_suspend();
    esp_net_suspend();
    int mv = measure();

    /* The screen stays on a reflective panel: say goodnight on it. */
    static char pad[3][96];
    const char *lines[HIPPO_LINES + 6];
    int n = 0, width = 0;
    for (size_t i = 0; i < HIPPO_LINES; i++) {
        lines[n++] = s_hippo[i];
        if ((int)strlen(s_hippo[i]) > width)
            width = strlen(s_hippo[i]);
    }
    const char *quip = battery_low()
        ? s_quips_low[esp_random() % (sizeof s_quips_low / sizeof s_quips_low[0])]
        : s_quips[esp_random() % (sizeof s_quips / sizeof s_quips[0])];
    char foot[64];
    snprintf(foot, sizeof foot, "%s", KEY >= 0 ? "KEY wakes me." : "RESET wakes me.");
    if (mv > 0)
        snprintf(foot + strlen(foot), sizeof foot - strlen(foot), "   Battery %d.%02d V",
                 mv / 1000, mv % 1000 / 10);
    /* When it went to sleep, if the clock has been set (esp_time). */
    char since[64] = "";
    time_t now = time(NULL);
    if (now >= 1735689600) {
        struct tm lt;
        localtime_r(&now, &lt);
        strftime(since, sizeof since, "Asleep since %H:%M, %a %d %b", &lt);
    }
    const char *centre[] = { quip, since, foot };
    lines[n++] = "";
    for (int i = 0; i < 3; i++) {       /* centred under the hippo */
        if (i == 1 && !since[0])
            continue;
        int len = strlen(centre[i]), left = width > len ? (width - len) / 2 : 0;
        if (left + len >= (int)sizeof pad[i])
            left = len = 0;
        memset(pad[i], ' ', left);
        memcpy(pad[i] + left, centre[i], len);
        pad[i][left + len] = '\0';
        lines[n++] = pad[i];
        if (i == 0)
            lines[n++] = "";
    }
    esp_display_sleep_t screen = { .deep = true, .lines = lines, .nlines = n };
    screen.bitmap = load_pbm(&screen.width, &screen.height);
    esp_display_sleep(&screen);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#if KEY >= 0
    if (rtc_gpio_is_valid_gpio(KEY)) {
        wait_key_released();
        esp_sleep_enable_ext0_wakeup(KEY, 0);
    }
#endif
    if (wake_s > 0)
        esp_sleep_enable_timer_wakeup((uint64_t)wake_s * 1000000);
    s_deep_sleeps++;
    ESP_LOGI(TAG, "deep sleep");
    esp_deep_sleep_start();
}

/* --------------------------------------------------------- reader mode -- */

/* Light sleep until the wake button (or {wake_s}), with the screen as it is.
 * On the way, a timer checks in: past deep_min, or with the battery low, and
 * nothing unsaved, it becomes a deep sleep. */
static void reader_sleep(int wake_s, bool unsaved)
{
    esp_ble_kbd_suspend();
    esp_net_suspend();
    esp_display_sleep_t screen = { .badge = KEY >= 0 ? " Zzzz KEY " : " Zzzz " };
    esp_display_sleep(&screen);
    wait_key_released();

    int64_t start = esp_timer_get_time();
    s_last_wake = "";
    for (;;) {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#if KEY >= 0
        /* Two ways to hear it: the GPIO wake, and the RTC domain's (ext1),
         * which doesn't depend on the digital pad's sleep configuration. */
        gpio_wakeup_enable(KEY, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
        if (rtc_gpio_is_valid_gpio(KEY))
            esp_sleep_enable_ext1_wakeup_io(1ULL << KEY, ESP_EXT1_WAKEUP_ANY_LOW);
#endif
        int64_t asleep = esp_timer_get_time() - start, t = 0;
        if (wake_s > 0)
            t = wake_s * 1000000LL - asleep;
        else if (s_deep_min > 0 && !unsaved)
            t = s_deep_min * US_PER_MIN - asleep;
        if (HAVE_BAT && wake_s == 0 && (t <= 0 || t > 15 * US_PER_MIN))
            t = 15 * US_PER_MIN;        /* look at the battery now and then */
        if (t > 0)
            esp_sleep_enable_timer_wakeup(t < 1000000 ? 1000000 : t);
        esp_light_sleep_start();
        s_key_used = true;              /* before the watcher can see it held */

        uint32_t causes = esp_sleep_get_wakeup_causes();
        if (causes & (BIT(ESP_SLEEP_WAKEUP_GPIO) | BIT(ESP_SLEEP_WAKEUP_EXT1))) {
            s_last_wake = "key";
            break;
        }
        if (!(causes & BIT(ESP_SLEEP_WAKEUP_TIMER)))
            break;
        asleep = esp_timer_get_time() - start;
        if (wake_s > 0 && asleep >= wake_s * 1000000LL - 1000000) {
            s_last_wake = "timer";
            break;
        }
        measure();
        if (!unsaved && ((s_deep_min > 0 && asleep >= s_deep_min * US_PER_MIN) || battery_low()))
            deep_sleep(0);              /* doesn't return */
    }
#if KEY >= 0
    gpio_wakeup_disable(KEY);
    key_as_gpio();                      /* ext1 left it an RTC pin */
#endif
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_display_wake();
    esp_net_resume();
    esp_ble_kbd_resume();
    s_sleeps++;
    s_last_input = esp_timer_get_time();
    wait_key_released();                /* the press that woke it isn't one to sleep */
}

esp_err_t esp_power_sleep(bool deep, int wake_s, bool unsaved)
{
    if (s_sleep_lock == NULL)
        return ESP_ERR_INVALID_STATE;
    if (!deep && KEY < 0 && wake_s <= 0)
        return ESP_ERR_NOT_SUPPORTED;   /* nothing would wake it but a reset */
    if (xSemaphoreTake(s_sleep_lock, 0) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    if (xTaskGetCurrentTaskHandle() == s_power_task) {
        if (deep)
            deep_sleep(wake_s);
        reader_sleep(wake_s, unsaved);
    } else {
        xSemaphoreTake(s_req_done, 0);
        s_req.deep = deep;
        s_req.wake_s = wake_s;
        s_req.unsaved = unsaved;
        s_req.pending = true;
        xTaskNotifyGive(s_power_task);
        xSemaphoreTake(s_req_done, portMAX_DELAY);  /* woken (deep: never) */
    }
    xSemaphoreGive(s_sleep_lock);
    return ESP_OK;
}

/* ---------------------------------------------------------- the watcher -- */

static void power_task(void *arg)
{
#if KEY >= 0
    int down = 0;                       /* polls the button has been down */
    bool armed = true;                  /* released since the last action */
#endif
    bool want = false;                  /* pressed: sleep once Vim waits */
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));   /* or at once, for a request */
        if (s_req.pending) {            /* esp_power_sleep() from another task, lock held */
            s_req.pending = false;
            if (s_req.deep)
                deep_sleep(s_req.wake_s);
            reader_sleep(s_req.wake_s, s_req.unsaved);
            xSemaphoreGive(s_req_done);
#if KEY >= 0
            armed = false;              /* the wake press is let go first */
            down = 0;
#endif
            continue;
        }
        int64_t now = esp_timer_get_time();
#if KEY >= 0
        if (s_key_used) {               /* woke by it: wait for it to be let go */
            s_key_used = false;
            armed = false;
            want = false;
        }
#endif
        if (HAVE_BAT && now - s_mv_at >= BAT_EVERY_US)
            measure();
#if KEY >= 0
        if (gpio_get_level(KEY) == 0) {
            if (++down == 2 && armed) { /* 100 ms down: a press */
                want = true;
                armed = false;
                s_presses++;
            }
        } else {
            down = 0;
            armed = true;
        }
#endif
        if (!s_waiting)
            continue;
        bool idle = s_idle_min > 0 && on_battery()
                    && now - s_last_input >= s_idle_min * US_PER_MIN;
        if (battery_low() && !s_unsaved) {
            if (xSemaphoreTake(s_sleep_lock, 0) == pdTRUE)
                deep_sleep(0);
        } else if ((want || idle) && KEY >= 0) {
            want = false;
            if (xSemaphoreTake(s_sleep_lock, 0) == pdTRUE) {
                reader_sleep(0, s_unsaved);
                xSemaphoreGive(s_sleep_lock);
            }
#if KEY >= 0
            armed = false;              /* until the wake press is let go */
#endif
        }
    }
}

/* ---------------------------------------------------------------- API -- */

void esp_power_input(void)
{
    s_last_input = esp_timer_get_time();
}

void esp_power_vim_waiting(bool waiting, bool unsaved)
{
    int64_t now = esp_timer_get_time();
    if (waiting) {
        /* Back from a long command: that was activity too, or the board
         * would sleep the moment it finished. Vim's own timeouts (CursorHold)
         * come and go without counting. */
        if (!s_waiting && s_busy_since && now - s_busy_since > 2000000)
            s_last_input = now;
        s_unsaved = unsaved;
    } else if (s_waiting) {
        s_busy_since = now;
    }
    s_waiting = waiting;
}

void esp_power_status(esp_power_status_t *st)
{
    if (HAVE_BAT && esp_timer_get_time() - s_mv_at > 2000000)
        measure();                      /* asked for: a fresh reading */
    memset(st, 0, sizeof *st);
    st->battery = HAVE_BAT;
    st->source = source();
    st->battery_mv = s_mv;
    st->battery_pct = percent(s_mv);
    st->reader = KEY >= 0;
    st->idle_min = s_idle_min;
    st->deep_min = s_deep_min;
    st->idle_s = (int)((esp_timer_get_time() - s_last_input) / 1000000);
    st->sleeps = s_sleeps;
    st->presses = s_presses;
    st->deep_sleeps = s_deep_sleeps;
    st->last_wake = s_last_wake;
}

void esp_power_set(int idle_min, int deep_min)
{
    nvs_handle_t h;
    bool ok = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (idle_min >= 0) {
        s_idle_min = idle_min;
        if (ok)
            nvs_set_u16(h, "idle", idle_min);
    }
    if (deep_min >= 0) {
        s_deep_min = deep_min;
        if (ok)
            nvs_set_u16(h, "deep", deep_min);
    }
    if (ok) {
        nvs_commit(h);
        nvs_close(h);
    }
    s_last_input = esp_timer_get_time();    /* the new idle time counts from now */
}

esp_err_t esp_power_init(void)
{
    s_idle_min = CONFIG_ESP_VIM_SLEEP_IDLE_MIN;
    s_deep_min = CONFIG_ESP_VIM_SLEEP_DEEP_MIN;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint16_t v;
        if (nvs_get_u16(h, "idle", &v) == ESP_OK)
            s_idle_min = v;
        if (nvs_get_u16(h, "deep", &v) == ESP_OK)
            s_deep_min = v;
        nvs_close(h);
    }
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0)
        s_last_wake = "key";
    else if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER)
        s_last_wake = "timer";
#if KEY >= 0
    key_as_gpio();                      /* after a deep sleep's wake: a GPIO again */
#endif
    s_sleep_lock = xSemaphoreCreateMutex();
    if (s_sleep_lock == NULL)
        return ESP_ERR_NO_MEM;
    s_last_input = esp_timer_get_time();
    measure();
    s_req_done = xSemaphoreCreateBinary();
    if (s_req_done == NULL)
        return ESP_ERR_NO_MEM;
    /* An internal stack: sleep touches flash (NVS for the radios, the PBM),
     * and deep sleep with held pins must start from one. Every board has the
     * task, even with no button or battery to watch: :EspSleep runs here. */
    if (xTaskCreatePinnedToCore(power_task, "power", 6144, NULL, 2, &s_power_task, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
