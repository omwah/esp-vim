/*
 * esp_display: see include/esp_display.h.
 *
 * Two tasks meet at a stream buffer. The writer (whoever writes the console,
 * in practice the Vim task) only copies bytes in. The display task, pinned to
 * the other core, feeds them to libvterm, then repaints the rows libvterm
 * reports damaged: a row's damaged cells are drawn into one line buffer and
 * sent to the panel as a single SPI transfer -- or, on an RGB panel, copied
 * into its frame buffer in PSRAM, which the LCD peripheral streams out by
 * itself. A monochrome ST7305 has a 1-bit frame buffer here, in the panel's own
 * layout, sent whole once a burst of damage is painted. libvterm is only ever
 * touched by the display task.
 */

#include "esp_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#if CONFIG_ESP_VIM_DISP_RGB
# include "esp_lcd_panel_rgb.h"
#elif CONFIG_ESP_VIM_DISP_ST7305
# include "driver/spi_master.h"
#else
# include "driver/spi_master.h"
# include "esp_lcd_ili9341.h"
#endif
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "vterm.h"

#include "esp_display_font.h"
#include "esp_kbd.h"
#include "esp_timer.h"
#include "esp_touch.h"

static const char *TAG = "esp_display";

/* The terminal's font, switched at run time (esp_display_set_font), and the
 * grid it gives, centred: 53 cells of 6 px leave 320 - 318 = 2 px. Set by the
 * display task (and init, before it starts). */
static const esp_display_font_t *s_font;
static int s_font_index;
static int s_cols, s_rows, s_x0, s_y0;
#define FONT_W      (s_font->width)
#define FONT_H      (s_font->height)
#define COLS        s_cols
#define ROWS        s_rows
#define X0          s_x0
#define Y0          s_y0
#define STREAM_SIZE (8 * 1024)
#define CHUNK       16              /* cells per SPI transfer: 2.3 KB of line buffer at 6x12 */
#define NVS_NS      "esp_display"

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

#if CONFIG_ESP_VIM_DISP_ST7305
# define DISP_MONO  1
#else
# define DISP_MONO  0
#endif

/* An SPI panel takes each pixel's high byte first; an RGB panel's frame buffer
 * is plain little-endian RGB565, and a monochrome one's line buffer only tells
 * ink (0) from paper. */
#if CONFIG_ESP_VIM_DISP_RGB || DISP_MONO
# define PIXEL(v)   ((uint16_t)(v))
#else
# define PIXEL(v)   ((uint16_t)((v) >> 8 | (v) << 8))
#endif

#define INK         0x0000          /* monochrome: black */
#define PAPER       0xFFFF          /* ... and white */

#if !DISP_MONO
static esp_lcd_panel_handle_t s_panel;
#endif
static SemaphoreHandle_t s_flushed;     /* the line buffer's transfer is done */
static SemaphoreHandle_t s_write_lock;  /* one writer at a time */
static StreamBufferHandle_t s_stream;
static uint16_t *s_line;                /* CHUNK cells, RGB565, DMA-capable */
static int s_line_px;                   /* its size in pixels: for the largest font */
static bool s_active;

static VTerm *s_vt;
static VTermScreen *s_screen;
static VTermPos s_cursor;
static bool s_cursor_visible = true;
static volatile int s_mouse;            /* the terminal's mouse mode: VTERM_PROP_MOUSE_* */
static SemaphoreHandle_t s_panel_lock;  /* one drawer at a time: terminal or overlay */
static volatile bool s_overlay;         /* the overlay has the panel */
static volatile bool s_repaint;         /* repaint the whole terminal (after the overlay) */
static int *s_dirty_lo, *s_dirty_hi;    /* damaged columns [lo, hi) per row */
static int s_max_rows;                  /* rows those have: the smallest font's */
static volatile int s_font_req = -1;    /* a font for the display task to switch to */
static SemaphoreHandle_t s_font_done;   /* ... and it has */
static const esp_display_sleep_t *volatile s_sleep_req;    /* esp_display_sleep() */
static SemaphoreHandle_t s_sleep_done;  /* the display task has drawn it, and parked */
static SemaphoreHandle_t s_resume;      /* esp_display_wake(): carry on */
static volatile bool s_parked;          /* the display task waits for it */

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
    return glyph_in(s_font, cp);
}

/* RGB565, in the panel's byte order. */
static __attribute__((unused)) uint16_t rgb565(VTermColor *c)
{
    vterm_screen_convert_color_to_rgb(s_screen, c);
    uint16_t v = ((c->red & 0xF8) << 8) | ((c->green & 0xFC) << 3) | (c->blue >> 3);
    return PIXEL(v);
}

#if !CONFIG_ESP_VIM_DISP_RGB
static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flushed, &woken);
    return woken == pdTRUE;
}
#endif

#if DISP_MONO

/*
 * The ST7305's frame buffer: 1 bit per pixel, 1 white. Its own layout is
 * portrait, 300 wide and 400 tall, each byte a block 4 pixels across and 2 down
 * (bit 7 the top left, then down, then across); landscape turns it a quarter,
 * as Waveshare's driver does.
 */
#define NATIVE_W    300
#define NATIVE_H    400
#define FB_SIZE     (NATIVE_W * NATIVE_H / 8)

static esp_lcd_panel_io_handle_t s_io;
static uint8_t *s_fb;                   /* DMA-capable */
static bool s_fb_dirty;

static inline void fb_set(int x, int y, bool white)
{
#if CONFIG_ESP_VIM_DISP_MIRROR_X
    x = CONFIG_ESP_VIM_DISP_WIDTH - 1 - x;
#endif
#if CONFIG_ESP_VIM_DISP_MIRROR_Y
    y = CONFIG_ESP_VIM_DISP_HEIGHT - 1 - y;
#endif
    int px = x, py = y;
    if (CONFIG_ESP_VIM_DISP_WIDTH > CONFIG_ESP_VIM_DISP_HEIGHT) {
        px = NATIVE_W - 1 - y;
        py = x;
    }
    uint8_t *b = s_fb + (py >> 1) * (NATIVE_W / 4) + (px >> 2);
    uint8_t mask = 0x80 >> (((px & 3) << 1) | (py & 1));
    *b = white ? *b | mask : *b & ~mask;
}

/* Send the frame buffer, if anything was drawn into it. 15 KB: 12 ms at 10 MHz. */
static void flush(void)
{
    if (!s_fb_dirty)
        return;
    s_fb_dirty = false;
    esp_lcd_panel_io_tx_param(s_io, 0x2A, (uint8_t[]){ 0x12, 0x2A }, 2);  /* columns */
    esp_lcd_panel_io_tx_param(s_io, 0x2B, (uint8_t[]){ 0x00, 0xC7 }, 2);  /* rows */
    esp_lcd_panel_io_tx_color(s_io, 0x2C, s_fb, FB_SIZE);
    xSemaphoreTake(s_flushed, portMAX_DELAY);
}

/* Draw from the line buffer into the frame buffer; flush() sends it. */
static void draw(int x0, int y0, int x1, int y1)
{
    const uint16_t *p = s_line;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            fb_set(x, y, *p++ != INK);
    s_fb_dirty = true;
}

#else

static void flush(void)
{
}

/* Draw from the line buffer, and wait until it may be reused: an SPI
 * transfer runs on, an RGB panel's copy is done on return. */
static void draw(int x0, int y0, int x1, int y1)
{
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, s_line);
#if !CONFIG_ESP_VIM_DISP_RGB
    xSemaphoreTake(s_flushed, portMAX_DELAY);
#endif
}

#endif

/* Cells [c0, c1) of a row, at most CHUNK of them, in one transfer. */
static void paint_cells(int row, int c0, int c1)
{
    int w = (c1 - c0) * FONT_W;
    for (int col = c0; col < c1; col++) {
        VTermScreenCell cell;
        VTermPos pos = { .row = row, .col = col };
        if (!vterm_screen_get_cell(s_screen, pos, &cell))
            continue;
        bool inverse = cell.attrs.reverse
            ^ (s_cursor_visible && row == s_cursor.row && col == s_cursor.col);
#if DISP_MONO
        /* Black on white, and white on black where the cell is reversed, is
         * the cursor, or has a background colour of its own (a Visual
         * selection, a search match): colour itself can't be shown. */
        inverse ^= !VTERM_COLOR_IS_DEFAULT_BG(&cell.bg);
        uint16_t fg = inverse ? PAPER : INK, bg = inverse ? INK : PAPER;
#else
        uint16_t fg = rgb565(&cell.fg), bg = rgb565(&cell.bg);
        if (inverse) {
            uint16_t t = fg; fg = bg; bg = t;
        }
#endif
        uint32_t ch = cell.chars[0];
        /* The right half of a double-width character is blank; so is conceal. */
        const uint8_t *g = (ch == 0 || ch == (uint32_t)-1 || cell.attrs.conceal) ? NULL : glyph(ch);
        uint16_t *px = s_line + (col - c0) * FONT_W;
        for (int y = 0; y < FONT_H; y++) {
            unsigned bits = !g ? 0 : s_font->bytes_per_row == 2 ? (g[2 * y] << 8 | g[2 * y + 1])
                                                                : g[y] << 8;
#if DISP_MONO
            if (cell.attrs.bold)
                bits |= bits >> 1;      /* bold: a pixel heavier */
#endif
            if (cell.attrs.underline && y == FONT_H - 1)
                bits = 0xFFFF;
            for (int x = 0; x < FONT_W; x++)
                px[y * w + x] = (bits & (0x8000 >> x)) ? fg : bg;
        }
    }
    draw(X0 + c0 * FONT_W, Y0 + row * FONT_H, X0 + c1 * FONT_W, Y0 + (row + 1) * FONT_H);
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
static void go_to_sleep(const esp_display_sleep_t *req);

/* Take font i: the grid it gives, and libvterm's screen resized to it. The
 * whole terminal is repainted next time round. */
static void use_font(int i)
{
    s_font = esp_display_fonts[i];
    s_font_index = i;
    s_cols = CONFIG_ESP_VIM_DISP_WIDTH / FONT_W;
    s_rows = CONFIG_ESP_VIM_DISP_HEIGHT / FONT_H;
    s_x0 = (CONFIG_ESP_VIM_DISP_WIDTH - s_cols * FONT_W) / 2;
    s_y0 = (CONFIG_ESP_VIM_DISP_HEIGHT - s_rows * FONT_H) / 2;
    for (int row = 0; row < s_max_rows; row++) {
        s_dirty_lo[row] = s_cols;
        s_dirty_hi[row] = 0;
    }
    if (s_vt) {
        vterm_set_size(s_vt, s_rows, s_cols);
        s_repaint = true;
    }
}

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
        if (s_font_req >= 0) {
            use_font(s_font_req);
            s_font_req = -1;
            xSemaphoreGive(s_font_done);
        }
        if (s_sleep_req) {
            const esp_display_sleep_t *req = s_sleep_req;
            s_sleep_req = NULL;
            go_to_sleep(req);           /* returns on esp_display_wake() */
        }
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
        flush();
        xSemaphoreGive(s_panel_lock);
    }
}

/* ----------------------------------------------------------------- overlay -- */

#define BIG         esp_display_font_big
#if CONFIG_ESP_VIM_DISP_OVERLAY_16X32
# define OV_W       16                  /* checked against the font at init */
# define OV_H       32
#else
# define OV_W       12
# define OV_H       24
#endif
#define OV_COLS     (CONFIG_ESP_VIM_DISP_WIDTH / OV_W)
#define OV_ROWS     (CONFIG_ESP_VIM_DISP_HEIGHT / OV_H)
#define OV_X0       ((CONFIG_ESP_VIM_DISP_WIDTH - OV_COLS * OV_W) / 2)
#define OV_Y0       ((CONFIG_ESP_VIM_DISP_HEIGHT - OV_ROWS * OV_H) / 2)
#define OV_CHUNK    (s_line_px / (OV_W * OV_H))     /* cells per transfer */

static __attribute__((unused)) uint16_t swap565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return PIXEL(v);
}

/* Up to OV_CHUNK cells of text at (row, col), in one transfer. Panel lock held. */
static void overlay_cells(int row, int col, const char *text, int n, bool inverse)
{
#if DISP_MONO
    uint16_t fg = INK, bg = PAPER;
#else
    uint16_t fg = swap565(0xE5, 0xE5, 0xE5), bg = swap565(0x00, 0x00, 0x00);
#endif
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
    draw(OV_X0 + col * OV_W, OV_Y0 + row * OV_H, OV_X0 + (col + n) * OV_W, OV_Y0 + (row + 1) * OV_H);
}

bool esp_display_overlay_begin(int *rows, int *cols)
{
    if (!s_active)
        return false;
    xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    s_overlay = true;
    clear_panel();
    flush();
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
    flush();
    xSemaphoreGive(s_panel_lock);
}

void esp_display_overlay_clear(void)
{
    if (!s_overlay)
        return;
    xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    clear_panel();
    flush();
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

/* ------------------------------------------------------------------- sleep -- */

/* Text in font f at pixel (x, y), up to as many cells as the line buffer holds
 * at a time. Panel lock held. */
static void text_at(const esp_display_font_t *f, int x, int y, const char *text, bool inverse)
{
#if DISP_MONO
    uint16_t fg = INK, bg = PAPER;
#else
    uint16_t fg = swap565(0xE5, 0xE5, 0xE5), bg = swap565(0x00, 0x00, 0x00);
#endif
    if (inverse) {
        uint16_t t = fg; fg = bg; bg = t;
    }
    const int chunk = s_line_px / (f->width * f->height);
    for (int len = strlen(text); len > 0; ) {
        int n = len < chunk ? len : chunk, w = n * f->width;
        for (int i = 0; i < n; i++) {
            const uint8_t *g = text[i] == ' ' ? NULL : glyph_in(f, (unsigned char)text[i]);
            uint16_t *px = s_line + i * f->width;
            for (int r = 0; r < f->height; r++) {
                unsigned bits = !g ? 0 : f->bytes_per_row == 2 ? (g[2 * r] << 8 | g[2 * r + 1])
                                                               : g[r] << 8;
                for (int c = 0; c < f->width; c++)
                    px[r * w + c] = (bits & (0x8000 >> c)) ? fg : bg;
            }
        }
        draw(x, y, x + w, y + f->height);
        x += w;
        text += n;
        len -= n;
    }
}

/* A picture made of text: the largest font it fits in, centred. */
static void text_screen(const char *const *lines, int n)
{
    int width = 0;
    for (int i = 0; i < n; i++)
        if ((int)strlen(lines[i]) > width)
            width = strlen(lines[i]);
    const esp_display_font_t *f = NULL;
    for (int i = 0; i < esp_display_font_count; i++) {
        const esp_display_font_t *c = esp_display_fonts[i];
        bool fits = CONFIG_ESP_VIM_DISP_WIDTH / c->width >= width
                    && CONFIG_ESP_VIM_DISP_HEIGHT / c->height >= n;
        if (fits && (f == NULL || c->height > f->height))
            f = c;
        if (f == NULL && i == esp_display_font_count - 1)
            f = c;                      /* too big for all of them: cut off */
    }
    int x0 = (CONFIG_ESP_VIM_DISP_WIDTH - width * f->width) / 2;
    int y0 = (CONFIG_ESP_VIM_DISP_HEIGHT - n * f->height) / 2;
    for (int i = 0; i < n; i++)
        if (y0 + (i + 1) * f->height <= CONFIG_ESP_VIM_DISP_HEIGHT)
            text_at(f, x0 < 0 ? 0 : x0, y0 < 0 ? 0 : y0 + i * f->height, lines[i], false);
}

#if DISP_MONO
/* A 1-bit picture (PBM's layout: rows padded to bytes, 1 black), centred. */
static void bitmap_screen(const uint8_t *bits, int w, int h)
{
    int x0 = (CONFIG_ESP_VIM_DISP_WIDTH - w) / 2, y0 = (CONFIG_ESP_VIM_DISP_HEIGHT - h) / 2;
    int stride = (w + 7) / 8;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int px = x0 + x, py = y0 + y;
            if (px >= 0 && py >= 0 && px < CONFIG_ESP_VIM_DISP_WIDTH && py < CONFIG_ESP_VIM_DISP_HEIGHT)
                fb_set(px, py, !(bits[y * stride + x / 8] & (0x80 >> (x % 8))));
        }
    s_fb_dirty = true;
}
#endif

/* The panel's pins through deep sleep: a reflective panel keeps its picture
 * only while its reset line stays high and nothing is clocked in; a backlight
 * stays off. Released at the next boot (esp_display_init). */
static const int s_hold_pins[] = {
#if DISP_MONO
    CONFIG_ESP_VIM_DISP_RST, CONFIG_ESP_VIM_DISP_CS, CONFIG_ESP_VIM_DISP_DC,
    CONFIG_ESP_VIM_DISP_SCLK, CONFIG_ESP_VIM_DISP_MOSI,
#endif
    CONFIG_ESP_VIM_DISP_BACKLIGHT,
};

static void hold_pins(bool on)
{
    for (size_t i = 0; i < sizeof s_hold_pins / sizeof s_hold_pins[0]; i++) {
        if (s_hold_pins[i] < 0)
            continue;
        if (on)
            gpio_hold_en(s_hold_pins[i]);
        else
            gpio_hold_dis(s_hold_pins[i]);
    }
    if (on)
        gpio_deep_sleep_hold_en();
    else
        gpio_deep_sleep_hold_dis();
}

/* ESP-IDF isolates every GPIO in light sleep (ESP_SLEEP_GPIO_RESET_WORKAROUND:
 * no input or output, floating) unless told not to. The panel's reset line
 * would float low and the ST7305 lose its picture and set-up; a backlight
 * would flicker on. Keep these as they are through it. */
static void keep_pins_in_light_sleep(void)
{
    for (size_t i = 0; i < sizeof s_hold_pins / sizeof s_hold_pins[0]; i++)
        if (s_hold_pins[i] >= 0)
            gpio_sleep_sel_dis(s_hold_pins[i]);
}

static void backlight(bool on)
{
#if CONFIG_ESP_VIM_DISP_BACKLIGHT >= 0
    gpio_set_level(CONFIG_ESP_VIM_DISP_BACKLIGHT,
                   on ? CONFIG_ESP_VIM_DISP_BACKLIGHT_ON_LEVEL : !CONFIG_ESP_VIM_DISP_BACKLIGHT_ON_LEVEL);
#endif
}

/* On the display task: finish what Vim wrote, draw the sleep screen, rest the
 * panel, and park until esp_display_wake() (never, for deep sleep). */
static void go_to_sleep(const esp_display_sleep_t *req)
{
    const bool deep = req->deep;        /* the caller's, gone once it's told */
    xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    if (!s_overlay)
        paint_damage();
    if (deep) {
        clear_panel();
#if DISP_MONO
        if (req->bitmap)
            bitmap_screen(req->bitmap, req->width, req->height);
        else
#endif
        if (req->lines)
            text_screen(req->lines, req->nlines);
    } else if (req->badge && !s_overlay) {
        /* In the bottom right corner, over Vim's last cells. */
        int n = strlen(req->badge);
        text_at(s_font, X0 + (COLS - n) * FONT_W, Y0 + (ROWS - 1) * FONT_H, req->badge, true);
    }
    flush();
#if DISP_MONO
    esp_lcd_panel_io_tx_param(s_io, 0x39, NULL, 0);         /* low-power mode: keeps the picture */
#endif
    backlight(false);
    if (deep)
        hold_pins(true);
    xSemaphoreGive(s_panel_lock);
    s_parked = true;
    xSemaphoreGive(s_sleep_done);
    if (deep)
        vTaskSuspend(NULL);             /* the chip is about to stop */
    xSemaphoreTake(s_resume, portMAX_DELAY);
    s_parked = false;
#if DISP_MONO
    esp_lcd_panel_io_tx_param(s_io, 0x38, NULL, 0);         /* high-power mode */
#endif
    backlight(true);
    s_repaint = true;                   /* the badge goes */
}

esp_err_t esp_display_sleep(const esp_display_sleep_t *req)
{
    if (!s_active)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_sleep_done, 0);
    s_sleep_req = req;
    if (s_parked)
        xSemaphoreGive(s_resume);       /* asleep already (light): wake to take it */
    return xSemaphoreTake(s_sleep_done, pdMS_TO_TICKS(3000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void esp_display_wake(void)
{
    if (s_active)
        xSemaphoreGive(s_resume);
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

#if CONFIG_ESP_VIM_DISP_RGB

static esp_err_t panel_init(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = CONFIG_ESP_VIM_DISP_PCLK_HZ,
            .h_res = CONFIG_ESP_VIM_DISP_WIDTH,
            .v_res = CONFIG_ESP_VIM_DISP_HEIGHT,
            .hsync_pulse_width = CONFIG_ESP_VIM_DISP_HSYNC_PULSE,
            .hsync_back_porch = CONFIG_ESP_VIM_DISP_HSYNC_BACK,
            .hsync_front_porch = CONFIG_ESP_VIM_DISP_HSYNC_FRONT,
            .vsync_pulse_width = CONFIG_ESP_VIM_DISP_VSYNC_PULSE,
            .vsync_back_porch = CONFIG_ESP_VIM_DISP_VSYNC_BACK,
            .vsync_front_porch = CONFIG_ESP_VIM_DISP_VSYNC_FRONT,
#if CONFIG_ESP_VIM_DISP_PCLK_ACTIVE_NEG
            .flags.pclk_active_neg = 1,
#endif
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = CONFIG_ESP_VIM_DISP_BOUNCE_LINES * CONFIG_ESP_VIM_DISP_WIDTH,
        .dma_burst_size = 64,
        .hsync_gpio_num = CONFIG_ESP_VIM_DISP_HSYNC,
        .vsync_gpio_num = CONFIG_ESP_VIM_DISP_VSYNC,
        .de_gpio_num = CONFIG_ESP_VIM_DISP_DE,
        .pclk_gpio_num = CONFIG_ESP_VIM_DISP_PCLK,
        .disp_gpio_num = -1,
        .flags.fb_in_psram = 1,
    };
    /* "D0,D1,...,D15" */
    const char *p = CONFIG_ESP_VIM_DISP_DATA;
    for (int i = 0; i < 16; i++) {
        char *end;
        cfg.data_gpio_nums[i] = (int)strtol(p, &end, 10);
        if (end == p)
            return ESP_ERR_INVALID_ARG;
        p = *end == ',' ? end + 1 : end;
    }
    esp_err_t e = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (e == ESP_OK)
        e = esp_lcd_panel_reset(s_panel);
    if (e == ESP_OK)
        e = esp_lcd_panel_init(s_panel);
    return e;
}

/* The panel's interrupt runs on the core that creates it, and with a bounce
 * buffer it copies every frame out of PSRAM: create it on core 1, beside the
 * display task, and leave core 0 to Vim. */
struct panel_init_job { TaskHandle_t caller; esp_err_t result; };

static void panel_init_task(void *arg)
{
    struct panel_init_job *job = arg;
    job->result = panel_init();
    xTaskNotifyGive(job->caller);
    vTaskDelete(NULL);
}

static esp_err_t panel_init_core1(void)
{
    struct panel_init_job job = { .caller = xTaskGetCurrentTaskHandle() };
    if (xTaskCreatePinnedToCore(panel_init_task, "panel_init", 3072, &job, 5, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return job.result;
}

#elif DISP_MONO

/* The ST7305's set-up, as Waveshare's driver sends it: {command, data length,
 * data...}. Voltages and timing, then 1 bit per pixel, inverted (so 1 is
 * white), high-power mode (a steady picture while it changes), and on. */
static const uint8_t st7305_init[] = {
    0xD6, 2, 0x17, 0x02,                /* NVM load */
    0xD1, 1, 0x01,                      /* booster on */
    0xC0, 2, 0x11, 0x04,                /* gate voltage */
    0xC1, 4, 0x41, 0x41, 0x41, 0x41,    /* source voltages */
    0xC2, 4, 0x19, 0x19, 0x19, 0x19,
    0xC4, 4, 0x41, 0x41, 0x41, 0x41,
    0xC5, 4, 0x19, 0x19, 0x19, 0x19,
    0xD8, 2, 0xA6, 0xE9,
    0xB2, 1, 0x05,                      /* frame rate */
    0xB3, 10, 0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45,
    0xB4, 8, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45,
    0x62, 3, 0x32, 0x03, 0x1F,
    0xB7, 1, 0x13,
    0xB0, 1, 0x64,
};
static const uint8_t st7305_on[] = {
    0xC9, 1, 0x00,
    0x36, 1, 0x48,                      /* memory access order */
    0x3A, 1, 0x11,                      /* 1 bit per pixel */
    0xB9, 1, 0x20,
    0xB8, 1, 0x29,
    0x21, 0,                            /* inversion on */
    0x35, 1, 0x00,                      /* tearing effect line */
    0xD0, 1, 0xFF,
    0x38, 0,                            /* high-power mode */
    0x29, 0,                            /* display on */
};

static esp_err_t send_all(const uint8_t *seq, size_t len)
{
    esp_err_t e = ESP_OK;
    for (size_t i = 0; e == ESP_OK && i < len; i += 2 + seq[i + 1])
        e = esp_lcd_panel_io_tx_param(s_io, seq[i], seq[i + 1] ? seq + i + 2 : NULL, seq[i + 1]);
    return e;
}

static esp_err_t panel_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_ESP_VIM_DISP_SCLK,
        .mosi_io_num = CONFIG_ESP_VIM_DISP_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FB_SIZE,
    };
    esp_err_t e = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK)
        return e;
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
    e = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_cfg, &s_io);
    if (e != ESP_OK)
        return e;
    if (CONFIG_ESP_VIM_DISP_RST >= 0) {
        gpio_config_t g = { .pin_bit_mask = 1ULL << CONFIG_ESP_VIM_DISP_RST,
                            .mode = GPIO_MODE_OUTPUT };
        gpio_config(&g);
        gpio_set_level(CONFIG_ESP_VIM_DISP_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(CONFIG_ESP_VIM_DISP_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(CONFIG_ESP_VIM_DISP_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    e = send_all(st7305_init, sizeof st7305_init);
    if (e == ESP_OK)
        e = esp_lcd_panel_io_tx_param(s_io, 0x11, NULL, 0);    /* sleep out */
    vTaskDelay(pdMS_TO_TICKS(200));
    if (e == ESP_OK)
        e = send_all(st7305_on, sizeof st7305_on);
    return e;
}

#else

static esp_err_t panel_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_ESP_VIM_DISP_SCLK,
        .mosi_io_num = CONFIG_ESP_VIM_DISP_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = s_line_px * sizeof(uint16_t),
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

#endif

/* Black out the whole panel, margins included. Its memory survives a software
 * reset, so a margin the grid never paints would otherwise keep whatever the
 * previous firmware left there. */
static void clear_panel(void)
{
    const int lines = s_line_px / CONFIG_ESP_VIM_DISP_WIDTH;
    memset(s_line, DISP_MONO ? 0xFF : 0, s_line_px * sizeof(uint16_t));    /* black, or paper */
    for (int y = 0; y < CONFIG_ESP_VIM_DISP_HEIGHT; y += lines) {
        int y1 = y + lines < CONFIG_ESP_VIM_DISP_HEIGHT ? y + lines : CONFIG_ESP_VIM_DISP_HEIGHT;
        draw(0, y, CONFIG_ESP_VIM_DISP_WIDTH, y1);
    }
}

static void backlight_on(void)
{
#if CONFIG_ESP_VIM_DISP_BACKLIGHT >= 0          /* -1: none (a reflective panel) */
    gpio_config_t g = { .pin_bit_mask = 1ULL << CONFIG_ESP_VIM_DISP_BACKLIGHT,
                        .mode = GPIO_MODE_OUTPUT };
    gpio_config(&g);
    gpio_set_level(CONFIG_ESP_VIM_DISP_BACKLIGHT, CONFIG_ESP_VIM_DISP_BACKLIGHT_ON_LEVEL);
#endif
}

/* The font chosen with esp_display_set_font() last time, else the first. */
static int saved_font(void)
{
    nvs_handle_t h;
    char name[32];
    size_t len = sizeof name;
    int found = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "font", name, &len) == ESP_OK)
            for (int i = 0; i < esp_display_font_count; i++)
                if (strcmp(esp_display_fonts[i]->name, name) == 0)
                    found = i;
        nvs_close(h);
    }
    return found;
}

esp_err_t esp_display_init(void)
{
    /* The line buffer holds CHUNK cells of the largest font, and at least a
     * panel line and an overlay cell (clear_panel, overlay_cells); the damage
     * lists as many rows as the smallest gives. */
    int max_px = 0, min_h = CONFIG_ESP_VIM_DISP_HEIGHT;
    for (int i = 0; i < esp_display_font_count; i++) {
        const esp_display_font_t *f = esp_display_fonts[i];
        if (f->width * f->height > max_px)
            max_px = f->width * f->height;
        if (f->height < min_h)
            min_h = f->height;
    }
    s_line_px = CHUNK * max_px;
    if (s_line_px < CONFIG_ESP_VIM_DISP_WIDTH)
        s_line_px = CONFIG_ESP_VIM_DISP_WIDTH;
    if (s_line_px < OV_W * OV_H)
        s_line_px = OV_W * OV_H;
    s_max_rows = CONFIG_ESP_VIM_DISP_HEIGHT / min_h;
    s_dirty_lo = calloc(s_max_rows, sizeof(int));
    s_dirty_hi = calloc(s_max_rows, sizeof(int));
    if (!s_dirty_lo || !s_dirty_hi)
        return ESP_ERR_NO_MEM;
    use_font(saved_font());
    if (BIG.width != OV_W || BIG.height != OV_H || BIG.bytes_per_row != 2) {
        ESP_LOGE(TAG, "overlay font is %dx%d, expected %dx%d", BIG.width, BIG.height, OV_W, OV_H);
        return ESP_ERR_INVALID_SIZE;
    }
    s_flushed = xSemaphoreCreateBinary();
    s_write_lock = xSemaphoreCreateMutex();
    s_panel_lock = xSemaphoreCreateMutex();
    s_font_done = xSemaphoreCreateBinary();
    s_sleep_done = xSemaphoreCreateBinary();
    s_resume = xSemaphoreCreateBinary();
    s_stream = xStreamBufferCreateWithCaps(STREAM_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* SPI sends it by DMA. An RGB panel copies it into the frame buffer, and
     * from internal RAM that copy doesn't also read PSRAM, which the LCD is
     * streaming the frame buffer out of -- unless the LCD streams from bounce
     * buffers, when PSRAM will do and the internal RAM is better spent. */
    s_line = heap_caps_malloc(s_line_px * sizeof(uint16_t),
#if (CONFIG_ESP_VIM_DISP_RGB && CONFIG_ESP_VIM_DISP_BOUNCE_LINES > 0) || DISP_MONO
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#endif
    if (!s_flushed || !s_write_lock || !s_panel_lock || !s_font_done || !s_sleep_done
            || !s_resume || !s_stream || !s_line)
        return ESP_ERR_NO_MEM;
#if DISP_MONO
    s_fb = heap_caps_malloc(FB_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_fb)
        return ESP_ERR_NO_MEM;
    memset(s_fb, 0xFF, FB_SIZE);        /* white */
#endif

    hold_pins(false);                   /* held through a deep sleep: let go */
#if CONFIG_ESP_VIM_DISP_RGB
    esp_err_t e = panel_init_core1();
#else
    esp_err_t e = panel_init();
#endif
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "panel: %s", esp_err_to_name(e));
        return e;
    }

    clear_panel();
    keep_pins_in_light_sleep();

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
    flush();
    backlight_on();

    /* Its stack in PSRAM: internal RAM is scarce on the S3, and this task never
     * touches flash, which is what a PSRAM stack must not do. */
    if (xTaskCreatePinnedToCoreWithCaps(display_task, "display", 6 * 1024, NULL, 5, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    s_active = true;
    ESP_LOGI(TAG, "%dx%d cells on a %dx%d panel, in %s", COLS, ROWS,
             CONFIG_ESP_VIM_DISP_WIDTH, CONFIG_ESP_VIM_DISP_HEIGHT, s_font->name);
    return ESP_OK;
}

bool esp_display_active(void)
{
    return s_active;
}

bool esp_display_mono(void)
{
    return s_active && DISP_MONO;
}

void esp_display_size(int *rows, int *cols)
{
    *rows = ROWS;
    *cols = COLS;
}

bool esp_display_font_info(int i, esp_display_font_info_t *info)
{
    if (!s_active || i < 0 || i >= esp_display_font_count)
        return false;
    const esp_display_font_t *f = esp_display_fonts[i];
    info->name = f->name;
    info->width = f->width;
    info->height = f->height;
    info->rows = CONFIG_ESP_VIM_DISP_HEIGHT / f->height;
    info->cols = CONFIG_ESP_VIM_DISP_WIDTH / f->width;
    return true;
}

int esp_display_font(void)
{
    return s_active ? s_font_index : -1;
}

esp_err_t esp_display_set_font(int i)
{
    if (!s_active)
        return ESP_ERR_INVALID_STATE;
    if (i < 0 || i >= esp_display_font_count)
        return ESP_ERR_INVALID_ARG;
    if (i != s_font_index) {
        /* libvterm is the display task's: it switches, within 100 ms. */
        xSemaphoreTake(s_font_done, 0);
        s_font_req = i;
        if (xSemaphoreTake(s_font_done, pdMS_TO_TICKS(2000)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "font", esp_display_fonts[i]->name);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
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
