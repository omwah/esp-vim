/*
 * esp_display: see include/esp_display.h.
 *
 * Two tasks meet at a stream buffer. The writer (whoever writes the console,
 * in practice the Vim task) only copies bytes in. The display task, pinned to
 * the other core, feeds them to libvterm, then repaints the rows libvterm
 * reports damaged: a row's damaged cells are drawn into one line buffer and
 * sent to the panel as a single SPI transfer. libvterm is only ever touched by
 * the display task.
 */

#include "esp_display.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"
#include "vterm.h"

#include "esp_display_font.h"

static const char *TAG = "esp_display";

#define FONT        esp_display_font
#define COLS        (CONFIG_ESP_VIM_DISP_WIDTH / FONT_W)
#define ROWS        (CONFIG_ESP_VIM_DISP_HEIGHT / FONT_H)
#define FONT_W      6               /* checked against the font at init */
#define FONT_H      12
#define STREAM_SIZE (8 * 1024)
#define CHUNK       16              /* cells per SPI transfer: 2.3 KB of line buffer */
/* The grid, centred: 53 cells of 6 px leave 320 - 318 = 2 px. */
#define X0          ((CONFIG_ESP_VIM_DISP_WIDTH - COLS * FONT_W) / 2)
#define Y0          ((CONFIG_ESP_VIM_DISP_HEIGHT - ROWS * FONT_H) / 2)

/* Kconfig bools as 0/1. */
#ifdef CONFIG_ESP_VIM_DISP_INVERT
# define DISP_INVERT true
#else
# define DISP_INVERT false
#endif
#ifdef CONFIG_ESP_VIM_DISP_SWAP_XY
# define DISP_SWAP_XY true
#else
# define DISP_SWAP_XY false
#endif
#ifdef CONFIG_ESP_VIM_DISP_MIRROR_X
# define DISP_MIRROR_X true
#else
# define DISP_MIRROR_X false
#endif
#ifdef CONFIG_ESP_VIM_DISP_MIRROR_Y
# define DISP_MIRROR_Y true
#else
# define DISP_MIRROR_Y false
#endif
#ifdef CONFIG_ESP_VIM_DISP_BGR
# define DISP_ORDER LCD_RGB_ELEMENT_ORDER_BGR
#else
# define DISP_ORDER LCD_RGB_ELEMENT_ORDER_RGB
#endif

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flushed;     /* the line buffer's transfer is done */
static SemaphoreHandle_t s_write_lock;  /* one writer at a time */
static StreamBufferHandle_t s_stream;
static uint16_t *s_line;                /* CHUNK cells, RGB565, DMA-capable */
static bool s_active;

static VTerm *s_vt;
static VTermScreen *s_screen;
static VTermPos s_cursor;
static bool s_cursor_visible = true;
static int s_dirty_lo[ROWS], s_dirty_hi[ROWS];  /* damaged columns [lo, hi) per row */

/* -------------------------------------------------------------- damage -- */

static void mark(int row, int c0, int c1)
{
    if (row < 0 || row >= ROWS)
        return;
    if (c0 < s_dirty_lo[row])
        s_dirty_lo[row] = c0 < 0 ? 0 : c0;
    if (c1 > s_dirty_hi[row])
        s_dirty_hi[row] = c1 > COLS ? COLS : c1;
}

static int on_damage(VTermRect r, void *user)
{
    for (int row = r.start_row; row < r.end_row; row++)
        mark(row, r.start_col, r.end_col);
    return 1;
}

static int on_movecursor(VTermPos pos, VTermPos old, int visible, void *user)
{
    mark(old.row, old.col, old.col + 1);
    mark(pos.row, pos.col, pos.col + 1);
    s_cursor = pos;
    return 1;
}

static int on_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    if (prop == VTERM_PROP_CURSORVISIBLE) {
        s_cursor_visible = val->boolean;
        mark(s_cursor.row, s_cursor.col, s_cursor.col + 1);
    }
    return 1;
}

static const VTermScreenCallbacks s_callbacks = {
    .damage = on_damage,
    .movecursor = on_movecursor,
    .settermprop = on_settermprop,
};

/* ------------------------------------------------------------ painting -- */

static const uint8_t *glyph(uint32_t cp)
{
    int lo = 0, hi = FONT.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (FONT.codepoints[mid] == cp)
            return FONT.bitmaps + mid * FONT_H;
        if (FONT.codepoints[mid] < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return cp == '?' ? NULL : glyph('?');
}

/* RGB565, byte-swapped: the panel takes the high byte first. */
static uint16_t rgb565(VTermColor *c)
{
    vterm_screen_convert_color_to_rgb(s_screen, c);
    uint16_t v = ((c->red & 0xF8) << 8) | ((c->green & 0xFC) << 3) | (c->blue >> 3);
    return (uint16_t)(v >> 8 | v << 8);
}

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flushed, &woken);
    return woken == pdTRUE;
}

/* Cells [c0, c1) of a row, at most CHUNK of them, in one transfer. */
static void paint_cells(int row, int c0, int c1)
{
    int w = (c1 - c0) * FONT_W;
    for (int col = c0; col < c1; col++) {
        VTermScreenCell cell;
        VTermPos pos = { .row = row, .col = col };
        if (!vterm_screen_get_cell(s_screen, pos, &cell))
            continue;
        uint16_t fg = rgb565(&cell.fg), bg = rgb565(&cell.bg);
        bool inverse = cell.attrs.reverse
            ^ (s_cursor_visible && row == s_cursor.row && col == s_cursor.col);
        if (inverse) {
            uint16_t t = fg; fg = bg; bg = t;
        }
        uint32_t ch = cell.chars[0];
        /* The right half of a double-width character is blank; so is conceal. */
        const uint8_t *g = (ch == 0 || ch == (uint32_t)-1 || cell.attrs.conceal) ? NULL : glyph(ch);
        uint16_t *px = s_line + (col - c0) * FONT_W;
        for (int y = 0; y < FONT_H; y++) {
            uint8_t bits = g ? g[y] : 0;
            if (cell.attrs.underline && y == FONT_H - 1)
                bits = 0xFF;
            for (int x = 0; x < FONT_W; x++)
                px[y * w + x] = (bits & (0x80 >> x)) ? fg : bg;
        }
    }
    esp_lcd_panel_draw_bitmap(s_panel, X0 + c0 * FONT_W, Y0 + row * FONT_H,
                              X0 + c1 * FONT_W, Y0 + (row + 1) * FONT_H, s_line);
    xSemaphoreTake(s_flushed, portMAX_DELAY);   /* the buffer is reused next */
}

static void paint_row(int row, int c0, int c1)
{
    for (int c = c0; c < c1; c += CHUNK)
        paint_cells(row, c, c + CHUNK < c1 ? c + CHUNK : c1);
}

static void paint_damage(void)
{
    vterm_screen_flush_damage(s_screen);
    for (int row = 0; row < ROWS; row++) {
        if (s_dirty_lo[row] < s_dirty_hi[row])
            paint_row(row, s_dirty_lo[row], s_dirty_hi[row]);
        s_dirty_lo[row] = COLS;
        s_dirty_hi[row] = 0;
    }
}

static void display_task(void *arg)
{
    static char buf[512];
    for (;;) {
        size_t n = xStreamBufferReceive(s_stream, buf, sizeof buf, portMAX_DELAY);
        vterm_input_write(s_vt, buf, n);
        /* Take whatever else is already waiting, so a burst is painted once. */
        while ((n = xStreamBufferReceive(s_stream, buf, sizeof buf, 0)) > 0)
            vterm_input_write(s_vt, buf, n);
        paint_damage();
    }
}

/* ---------------------------------------------------------------- setup -- */

static void *vt_malloc(size_t size, void *data)
{
    return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void vt_free(void *ptr, void *data)
{
    heap_caps_free(ptr);
}

static VTermAllocatorFunctions s_alloc = { .malloc = vt_malloc, .free = vt_free };

static esp_err_t panel_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_ESP_VIM_DISP_SCLK,
        .mosi_io_num = CONFIG_ESP_VIM_DISP_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK * FONT_W * FONT_H * sizeof(uint16_t),
    };
    esp_err_t e = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK)
        return e;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = CONFIG_ESP_VIM_DISP_CS,
        .dc_gpio_num = CONFIG_ESP_VIM_DISP_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_ESP_VIM_DISP_SPI_MHZ * 1000 * 1000,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = on_color_done,
    };
    e = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_cfg, &io);
    if (e != ESP_OK)
        return e;
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = CONFIG_ESP_VIM_DISP_RST,
        .rgb_ele_order = DISP_ORDER,
        .bits_per_pixel = 16,
    };
    e = esp_lcd_new_panel_ili9341(io, &dev, &s_panel);
    if (e == ESP_OK)
        e = esp_lcd_panel_reset(s_panel);   /* a software reset when there's no pin */
    if (e == ESP_OK)
        e = esp_lcd_panel_init(s_panel);
    if (e == ESP_OK)
        e = esp_lcd_panel_invert_color(s_panel, DISP_INVERT);
    if (e == ESP_OK)
        e = esp_lcd_panel_swap_xy(s_panel, DISP_SWAP_XY);
    if (e == ESP_OK)
        e = esp_lcd_panel_mirror(s_panel, DISP_MIRROR_X, DISP_MIRROR_Y);
    if (e == ESP_OK)
        e = esp_lcd_panel_disp_on_off(s_panel, true);
    return e;
}

/* Black out the whole panel, margins included. Its memory survives a software
 * reset, so a margin the grid never paints would otherwise keep whatever the
 * previous firmware left there. */
static void clear_panel(void)
{
    const int lines = CHUNK * FONT_W * FONT_H / CONFIG_ESP_VIM_DISP_WIDTH;
    memset(s_line, 0, CHUNK * FONT_W * FONT_H * sizeof(uint16_t));
    for (int y = 0; y < CONFIG_ESP_VIM_DISP_HEIGHT; y += lines) {
        int y1 = y + lines < CONFIG_ESP_VIM_DISP_HEIGHT ? y + lines : CONFIG_ESP_VIM_DISP_HEIGHT;
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, CONFIG_ESP_VIM_DISP_WIDTH, y1, s_line);
        xSemaphoreTake(s_flushed, portMAX_DELAY);
    }
}

static void backlight_on(void)
{
    if (CONFIG_ESP_VIM_DISP_BACKLIGHT < 0)
        return;
    gpio_config_t g = { .pin_bit_mask = 1ULL << CONFIG_ESP_VIM_DISP_BACKLIGHT,
                        .mode = GPIO_MODE_OUTPUT };
    gpio_config(&g);
    gpio_set_level(CONFIG_ESP_VIM_DISP_BACKLIGHT, CONFIG_ESP_VIM_DISP_BACKLIGHT_ON_LEVEL);
}

esp_err_t esp_display_init(void)
{
    if (FONT.width != FONT_W || FONT.height != FONT_H) {
        ESP_LOGE(TAG, "font is %dx%d, expected %dx%d", FONT.width, FONT.height, FONT_W, FONT_H);
        return ESP_ERR_INVALID_SIZE;
    }
    s_flushed = xSemaphoreCreateBinary();
    s_write_lock = xSemaphoreCreateMutex();
    s_stream = xStreamBufferCreateWithCaps(STREAM_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_line = heap_caps_malloc(CHUNK * FONT_W * FONT_H * sizeof(uint16_t),
                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_flushed || !s_write_lock || !s_stream || !s_line)
        return ESP_ERR_NO_MEM;

    esp_err_t e = panel_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "panel: %s", esp_err_to_name(e));
        return e;
    }

    clear_panel();

    s_vt = vterm_new_with_allocator(ROWS, COLS, &s_alloc, NULL);
    if (s_vt == NULL)
        return ESP_ERR_NO_MEM;
    vterm_set_utf8(s_vt, 1);
    s_screen = vterm_obtain_screen(s_vt);
    vterm_screen_set_callbacks(s_screen, &s_callbacks, NULL);
    vterm_screen_enable_altscreen(s_screen, 1);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, 0xE5, 0xE5, 0xE5);     /* xterm's default foreground */
    vterm_color_rgb(&bg, 0x00, 0x00, 0x00);
    vterm_state_set_default_colors(vterm_obtain_state(s_vt), &fg, &bg);
    vterm_screen_reset(s_screen, 1);
    for (int row = 0; row < ROWS; row++)
        mark(row, 0, COLS);             /* paint the whole (blank) screen once */
    paint_damage();
    backlight_on();

    /* Its stack in PSRAM: internal RAM is scarce on the S3, and this task never
     * touches flash, which is what a PSRAM stack must not do. */
    if (xTaskCreatePinnedToCoreWithCaps(display_task, "display", 6 * 1024, NULL, 5, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    s_active = true;
    ESP_LOGI(TAG, "%dx%d cells on a %dx%d panel", COLS, ROWS,
             CONFIG_ESP_VIM_DISP_WIDTH, CONFIG_ESP_VIM_DISP_HEIGHT);
    return ESP_OK;
}

bool esp_display_active(void)
{
    return s_active;
}

void esp_display_size(int *rows, int *cols)
{
    *rows = ROWS;
    *cols = COLS;
}

void esp_display_write(const void *buf, size_t len)
{
    if (!s_active || len == 0)
        return;
    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    const char *p = buf;
    while (len > 0) {                   /* send() may take part of it when full */
        size_t n = xStreamBufferSend(s_stream, p, len, portMAX_DELAY);
        p += n;
        len -= n;
    }
    xSemaphoreGive(s_write_lock);
}
