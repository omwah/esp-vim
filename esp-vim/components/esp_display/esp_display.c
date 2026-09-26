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
#include <stdlib.h>
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
#include "esp_kbd.h"
#include "esp_timer.h"
#include "esp_touch.h"

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
static volatile int s_mouse;            /* the terminal's mouse mode: VTERM_PROP_MOUSE_* */
static SemaphoreHandle_t s_panel_lock;  /* one drawer at a time: terminal or overlay */
static volatile bool s_overlay;         /* the overlay has the panel */
static volatile bool s_repaint;         /* repaint the whole terminal (after the overlay) */
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
    if (prop == VTERM_PROP_MOUSE)
        s_mouse = val->number;
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

static const uint8_t *glyph_in(const esp_display_font_t *f, uint32_t cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->codepoints[mid] == cp)
            return f->bitmaps + mid * f->height * f->bytes_per_row;
        if (f->codepoints[mid] < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return cp == '?' ? NULL : glyph_in(f, '?');
}

static const uint8_t *glyph(uint32_t cp)
{
    return glyph_in(&FONT, cp);
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

static void clear_panel(void);

static void display_task(void *arg)
{
    static char buf[512];
    for (;;) {
        /* Wake at least every 100 ms: the overlay may have handed the panel back. */
        size_t n = xStreamBufferReceive(s_stream, buf, sizeof buf, pdMS_TO_TICKS(100));
        if (n)
            vterm_input_write(s_vt, buf, n);
        /* Take whatever else is already waiting, so a burst is painted once. */
        while ((n = xStreamBufferReceive(s_stream, buf, sizeof buf, 0)) > 0)
            vterm_input_write(s_vt, buf, n);
        if (s_overlay)
            continue;                   /* libvterm keeps the screen; painting waits */
        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        if (s_repaint) {
            s_repaint = false;
            clear_panel();
            for (int row = 0; row < ROWS; row++)
                mark(row, 0, COLS);
        }
        paint_damage();
        xSemaphoreGive(s_panel_lock);
    }
}

/* ----------------------------------------------------------------- overlay -- */

#define BIG         esp_display_font_big
#define OV_W        12                  /* checked against the font at init */
#define OV_H        24
#define OV_COLS     (CONFIG_ESP_VIM_DISP_WIDTH / OV_W)
#define OV_ROWS     (CONFIG_ESP_VIM_DISP_HEIGHT / OV_H)
#define OV_X0       ((CONFIG_ESP_VIM_DISP_WIDTH - OV_COLS * OV_W) / 2)
#define OV_Y0       ((CONFIG_ESP_VIM_DISP_HEIGHT - OV_ROWS * OV_H) / 2)
#define OV_CHUNK    (CHUNK * FONT_W * FONT_H / (OV_W * OV_H))    /* cells per transfer */

static uint16_t swap565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)(v >> 8 | v << 8);
}

/* Up to OV_CHUNK cells of text at (row, col), in one transfer. Panel lock held. */
static void overlay_cells(int row, int col, const char *text, int n, bool inverse)
{
    uint16_t fg = swap565(0xE5, 0xE5, 0xE5), bg = swap565(0x00, 0x00, 0x00);
    if (inverse) {
        uint16_t t = fg; fg = bg; bg = t;
    }
    int w = n * OV_W;
    for (int i = 0; i < n; i++) {
        const uint8_t *g = text[i] == ' ' ? NULL : glyph_in(&BIG, (unsigned char)text[i]);
        uint16_t *px = s_line + i * OV_W;
        for (int y = 0; y < OV_H; y++) {
            unsigned bits = g ? (g[2 * y] << 8 | g[2 * y + 1]) : 0;
            for (int x = 0; x < OV_W; x++)
                px[y * w + x] = (bits & (0x8000 >> x)) ? fg : bg;
        }
    }
    esp_lcd_panel_draw_bitmap(s_panel, OV_X0 + col * OV_W, OV_Y0 + row * OV_H,
                              OV_X0 + (col + n) * OV_W, OV_Y0 + (row + 1) * OV_H, s_line);
    xSemaphoreTake(s_flushed, portMAX_DELAY);
}

bool esp_display_overlay_begin(int *rows, int *cols)
{
    if (!s_active)
        return false;
    xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    s_overlay = true;
    clear_panel();
    xSemaphoreGive(s_panel_lock);
    *rows = OV_ROWS;
    *cols = OV_COLS;
    return true;
}

void esp_display_overlay_text(int row, int col, const char *text, bool inverse)
{
    if (!s_overlay || row < 0 || row >= OV_ROWS)
        return;
    int n = strlen(text);
    if (col + n > OV_COLS)
        n = OV_COLS - col;
    xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    for (int i = 0; i < n; i += OV_CHUNK)
        overlay_cells(row, col + i, text + i, n - i < OV_CHUNK ? n - i : OV_CHUNK, inverse);
    xSemaphoreGive(s_panel_lock);
}

void esp_display_overlay_clear(void)
{
    if (!s_overlay)
        return;
    xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    clear_panel();
    xSemaphoreGive(s_panel_lock);
}

void esp_display_overlay_cell_at(int x, int y, int *row, int *col)
{
    *row = (y - OV_Y0) / OV_H;
    *col = (x - OV_X0) / OV_W;
}

void esp_display_overlay_end(void)
{
    if (!s_overlay)
        return;
    s_repaint = true;                   /* the display task repaints the terminal */
    s_overlay = false;
}

/* ---------------------------------------------------------- touch-as-mouse -- */

#define HOLD_US   (400 * 1000)          /* press this long before moving: a drag */
#define SLOP_PX   12                    /* movement smaller than this is a tap */
#define WHEEL_PX  (3 * FONT_H)          /* a wheel step per 3 rows: Vim scrolls 3 */

/* Runs on the touch task; only it touches this state. */
static enum { G_IDLE, G_PENDING, G_SCROLL, G_DRAG } s_gesture;
static int s_x0, s_y0, s_wheel_y, s_row0, s_col0, s_row, s_col;
static int64_t s_t0;

static void cell_at(int x, int y, int *row, int *col)
{
    int c = (x - X0) / FONT_W, r = (y - Y0) / FONT_H;
    *col = (c < 0 ? 0 : c >= COLS ? COLS - 1 : c) + 1;
    *row = (r < 0 ? 0 : r >= ROWS ? ROWS - 1 : r) + 1;
}

/* An xterm SGR mouse report: button 0 = left, 32 = left moved while held,
 * 64/65 = wheel up/down. */
static void mouse(int button, int row, int col, bool release)
{
    char b[24];
    int n = snprintf(b, sizeof b, "\033[<%d;%d;%d%c", button, col, row, release ? 'm' : 'M');
    esp_kbd_push(b, n);
}

void esp_display_touch(int ev, int x, int y, void *ctx)
{
    if (!s_active || s_mouse == VTERM_PROP_MOUSE_NONE) {
        s_gesture = G_IDLE;
        return;
    }
    int row, col;
    cell_at(x, y, &row, &col);
    int64_t now = esp_timer_get_time();
    switch (ev) {
    case ESP_TOUCH_DOWN:
        s_gesture = G_PENDING;
        s_x0 = x, s_y0 = y, s_t0 = now;
        s_row0 = s_row = row, s_col0 = s_col = col;
        break;
    case ESP_TOUCH_MOVE:
        if (s_gesture == G_PENDING) {
            int dx = abs(x - s_x0), dy = abs(y - s_y0);
            bool moved = dx > SLOP_PX || dy > SLOP_PX;
            if (now - s_t0 >= HOLD_US || (moved && dx >= dy)) {
                /* Held still first, or moving sideways: a drag, pressing
                 * where it started. */
                s_gesture = G_DRAG;
                mouse(0, s_row0, s_col0, false);
            } else if (moved) {                 /* up or down, straight away */
                s_gesture = G_SCROLL;
                s_wheel_y = s_y0;
            }
        }
        if (s_gesture == G_SCROLL) {
            /* The content follows the finger: moving up shows what's below. */
            for (; y - s_wheel_y <= -WHEEL_PX; s_wheel_y -= WHEEL_PX)
                mouse(65, s_row0, s_col0, false);
            for (; y - s_wheel_y >= WHEEL_PX; s_wheel_y += WHEEL_PX)
                mouse(64, s_row0, s_col0, false);
        } else if (s_gesture == G_DRAG && (row != s_row || col != s_col)) {
            mouse(32, row, col, false);
            s_row = row, s_col = col;
        }
        break;
    case ESP_TOUCH_UP:
        if (s_gesture == G_PENDING) {           /* a tap: click */
            mouse(0, s_row0, s_col0, false);
            mouse(0, s_row0, s_col0, true);
        } else if (s_gesture == G_DRAG) {
            mouse(0, s_row, s_col, true);
        }
        s_gesture = G_IDLE;
        break;
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
    if (BIG.width != OV_W || BIG.height != OV_H || BIG.bytes_per_row != 2) {
        ESP_LOGE(TAG, "overlay font is %dx%d, expected %dx%d", BIG.width, BIG.height, OV_W, OV_H);
        return ESP_ERR_INVALID_SIZE;
    }
    s_flushed = xSemaphoreCreateBinary();
    s_write_lock = xSemaphoreCreateMutex();
    s_panel_lock = xSemaphoreCreateMutex();
    s_stream = xStreamBufferCreateWithCaps(STREAM_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_line = heap_caps_malloc(CHUNK * FONT_W * FONT_H * sizeof(uint16_t),
                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_flushed || !s_write_lock || !s_panel_lock || !s_stream || !s_line)
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
