// ST7735 TFT driver — see tft.h. All SPI traffic to the panel lives here.
//
// Framebuffer design (2026-11):
//   s_fb     — what the UI draws into (all tft_* drawing functions write here,
//              never to SPI). Pixels are stored byte-swapped (SPI byte order).
//   s_shown  — what the panel currently displays.
//   tft_flush() compares the two row by row and sends ONLY the pixels that
//   differ, grouped into rectangles, through a DMA staging buffer. A full-screen
//   redraw that changes 3 characters therefore sends ~3 characters' worth of
//   pixels, and the panel never shows a half-drawn or "cleared" frame (no flicker).
//   RAM: 2 x 40 KB + 8 KB staging, internal DMA-capable memory.

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tft.h"

static const char *TAG = "tft";

// SPI clock. ST7735 datasheet min write cycle is 66 ns (~15 MHz) but most panels
// run faster; valid ESP32 values are 80 MHz / n (40, 26.7, 20, 16, 13.3, 10 ...).
// 40 MHz verified on this panel 2026-11-27 (no corruption); a full-screen switch
// takes ~7 ms instead of ~28 ms at the old 10 MHz. Drop to 26.7/20 MHz if a
// different panel or longer wiring shows noise.
#ifndef TFT_SPI_HZ
#define TFT_SPI_HZ   (40*1000*1000)
#endif

#define FB_PIXELS     (TFT_W * TFT_H)
#define STAGE_BYTES   8192           // DMA staging buffer (also SPI max transfer)

static uint16_t *s_fb     = NULL;    // drawing target (byte-swapped RGB565)
static uint16_t *s_shown  = NULL;    // panel contents (byte-swapped RGB565)
static uint8_t  *s_stage  = NULL;    // DMA staging for one flush chunk
static bool      s_force_full = true; // first flush sends everything
static uint32_t  s_last_flush_us = 0, s_last_flush_px = 0;

static inline uint16_t swap16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

// ── TFT hardware pins ────────────────────────────────────────
#define TFT_SCLK  GPIO_NUM_36
#define TFT_MOSI  GPIO_NUM_35
#define TFT_CS    GPIO_NUM_37
#define TFT_DC    GPIO_NUM_45
#define TFT_RST   GPIO_NUM_21

// ── TFT primitives ───────────────────────────────────────────
static spi_device_handle_t s_spi;

static void tft_xfer(const uint8_t *d, int n, int dc)
{
    gpio_set_level(TFT_DC, dc);
    spi_transaction_t t = { .length = (size_t)n * 8, .tx_buffer = d };
    spi_device_polling_transmit(s_spi, &t);
}
static void tft_cmd(uint8_t c)               { tft_xfer(&c, 1, 0); }
static void tft_byte(uint8_t b)              { tft_xfer(&b, 1, 1); }
static void tft_data(const uint8_t *b, int n){ tft_xfer(b, n, 1); }

// Set the panel's address window: 6 small transactions (was 11 byte-by-byte).
static void tft_win(uint8_t x0,uint8_t y0,uint8_t x1,uint8_t y1)
{
    const uint8_t ca[4] = {0, x0, 0, x1};
    const uint8_t ra[4] = {0, y0, 0, y1};
    tft_cmd(0x2A); tft_data(ca, 4);
    tft_cmd(0x2B); tft_data(ra, 4);
    tft_cmd(0x2C);
}

// Pixel data: interrupt/DMA transaction - the task sleeps instead of
// busy-waiting, so the CPU stays free for audio rendering on the same core.
static void tft_send_pixels(const uint8_t *buf, int nbytes)
{
    gpio_set_level(TFT_DC, 1);
    spi_transaction_t t = { .length = (size_t)nbytes * 8, .tx_buffer = buf };
    spi_device_transmit(s_spi, &t);
}

// -- Drawing into the framebuffer (no SPI here) ---------------
// Clip rectangle (transitions draw a screen "through" a window). Full screen by default.
static int s_cx0 = 0, s_cy0 = 0, s_cx1 = TFT_W - 1, s_cy1 = TFT_H - 1;
void tft_set_clip(int x0, int y0, int x1, int y1)
{
    s_cx0 = x0 < 0 ? 0 : x0; s_cy0 = y0 < 0 ? 0 : y0;
    s_cx1 = x1 > TFT_W - 1 ? TFT_W - 1 : x1; s_cy1 = y1 > TFT_H - 1 ? TFT_H - 1 : y1;
}
// Dark mode: the panel's own colour inversion (ST7735 INVON / INVOFF). UI task only.
void tft_set_invert(bool on) { tft_cmd(on ? 0x21 : 0x20); }

void tft_clip_reset(void) { s_cx0 = 0; s_cy0 = 0; s_cx1 = TFT_W - 1; s_cy1 = TFT_H - 1; }

static inline void fb_px(int x, int y, uint16_t sw)
{
    if (x >= s_cx0 && x <= s_cx1 && y >= s_cy0 && y <= s_cy1) s_fb[y * TFT_W + x] = sw;
}

void tft_fill(uint8_t x0,uint8_t y0,uint8_t x1,uint8_t y1, uint16_t c)
{
    int ix0 = x0, iy0 = y0, ix1 = x1, iy1 = y1;
    if (ix0 < s_cx0) ix0 = s_cx0;
    if (iy0 < s_cy0) iy0 = s_cy0;
    if (ix1 > s_cx1) ix1 = s_cx1;
    if (iy1 > s_cy1) iy1 = s_cy1;
    int w = ix1 - ix0 + 1;
    if (w <= 0 || iy1 < iy0 || ix0 >= TFT_W || iy0 >= TFT_H) return;
    uint16_t sw = swap16(c);
    for (int y = iy0; y <= iy1; y++) {
        uint16_t *row = s_fb + y * TFT_W + ix0;
        for (int i = 0; i < w; i++) row[i] = sw;
    }
}

void tft_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    uint16_t sw = swap16(c);
    int dx = abs(x1-x0), dy = abs(y1-y0);
    int sx = (x0<x1) ? 1 : -1, sy = (y0<y1) ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        fb_px(x0, y0, sw);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// -- Flush: send only what changed ----------------------------
// Rows whose contents differ are grouped into bands of consecutive rows; each
// band is sent as one rectangle spanning the union of the rows' changed x
// ranges. Band data is packed into the DMA staging buffer (<= STAGE_BYTES per
// transaction), then copied into s_shown.
static bool row_span(int y, int *a, int *b)
{
    const uint16_t *f = s_fb + y * TFT_W, *sh = s_shown + y * TFT_W;
    if (s_force_full) { *a = 0; *b = TFT_W - 1; return true; }
    if (memcmp(f, sh, TFT_W * 2) == 0) return false;
    int l = 0, r = TFT_W - 1;
    while (f[l] == sh[l]) l++;
    while (f[r] == sh[r]) r--;
    *a = l; *b = r;
    return true;
}

void tft_flush(void)
{
    int64_t t0 = esp_timer_get_time();
    uint32_t sent_px = 0;
    int y = 0;
    while (y < TFT_H) {
        int a, b;
        while (y < TFT_H && !row_span(y, &a, &b)) y++;
        if (y >= TFT_H) break;
        int y0 = y, y1 = y;
        for (int yy = y + 1; yy < TFT_H; yy++) {
            int aa, bb;
            if (!row_span(yy, &aa, &bb)) break;
            if (aa < a) a = aa;
            if (bb > b) b = bb;
            y1 = yy;
        }
        int w = b - a + 1;
        tft_win((uint8_t)a, (uint8_t)y0, (uint8_t)b, (uint8_t)y1);
        int rows_per_chunk = STAGE_BYTES / (w * 2);
        if (rows_per_chunk < 1) rows_per_chunk = 1;
        for (int ry = y0; ry <= y1; ry += rows_per_chunk) {
            int nr = y1 - ry + 1;
            if (nr > rows_per_chunk) nr = rows_per_chunk;
            for (int k = 0; k < nr; k++) {
                const uint16_t *src = s_fb + (ry + k) * TFT_W + a;
                memcpy(s_stage + k * w * 2, src, w * 2);
                memcpy(s_shown + (ry + k) * TFT_W + a, src, w * 2);
            }
            tft_send_pixels(s_stage, nr * w * 2);
        }
        sent_px += (uint32_t)w * (uint32_t)(y1 - y0 + 1);
        y = y1 + 1;
    }
    s_force_full = false;
    if (sent_px) {
        s_last_flush_us = (uint32_t)(esp_timer_get_time() - t0);
        s_last_flush_px = sent_px;
    }
}

void tft_get_flush_stats(uint32_t *us, uint32_t *pixels)
{
    if (us)     *us     = s_last_flush_us;
    if (pixels) *pixels = s_last_flush_px;
}

// Filled (active=green) or outline ring (inactive=gray) pad circle, radius 3.
void tft_pad_circle(int cx, int cy, bool active)
{
    static const int8_t hw3[7] = {0, 2, 2, 3, 2, 2, 0}; // r=3 half-widths
    static const int8_t hw2[5] = {0, 1, 2, 1, 0};        // r=2 half-widths (inner hollow)
    uint16_t col = active ? C_GREEN : C_LTGRAY;
    for (int i = 0; i < 7; i++) {
        int y = cy - 3 + i;
        if (y < 0 || y >= TFT_H) continue;
        int x0 = cx - hw3[i], x1 = cx + hw3[i];
        if (x0 < 0) x0 = 0;
        if (x1 >= TFT_W) x1 = TFT_W-1;
        tft_fill((uint8_t)x0, (uint8_t)y, (uint8_t)x1, (uint8_t)y, col);
    }
    if (!active) {
        for (int i = 0; i < 5; i++) {
            int y = cy - 2 + i;
            if (y < 0 || y >= TFT_H) continue;
            int x0 = cx - hw2[i], x1 = cx + hw2[i];
            if (x0 < 0) x0 = 0;
            if (x1 >= TFT_W) x1 = TFT_W-1;
            tft_fill((uint8_t)x0, (uint8_t)y, (uint8_t)x1, (uint8_t)y, C_BLACK);
        }
    }
}

static void tft_panel_init(void)
{
    gpio_set_level(TFT_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TFT_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));
    tft_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));
    tft_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(255));
    tft_cmd(0xB1); tft_byte(0x01); tft_byte(0x2C); tft_byte(0x2D);
    tft_cmd(0xB2); tft_byte(0x01); tft_byte(0x2C); tft_byte(0x2D);
    tft_cmd(0xB3); tft_byte(0x01); tft_byte(0x2C); tft_byte(0x2D);
                   tft_byte(0x01); tft_byte(0x2C); tft_byte(0x2D);
    tft_cmd(0xB4); tft_byte(0x07);
    tft_cmd(0xC0); tft_byte(0xA2); tft_byte(0x02); tft_byte(0x84);
    tft_cmd(0xC1); tft_byte(0xC5);
    tft_cmd(0xC2); tft_byte(0x0A); tft_byte(0x00);
    tft_cmd(0xC3); tft_byte(0x8A); tft_byte(0x2A);
    tft_cmd(0xC4); tft_byte(0x8A); tft_byte(0xEE);
    tft_cmd(0xC5); tft_byte(0x0E);
    tft_cmd(0x20); tft_cmd(0x36); tft_byte(0x68); // landscape: MX=1 MV=1 RGB=1
    tft_cmd(0x3A); tft_byte(0x05);
    tft_cmd(0x2A); tft_byte(0); tft_byte(0); tft_byte(0); tft_byte(0x9F); // cols 0..159
    tft_cmd(0x2B); tft_byte(0); tft_byte(0); tft_byte(0); tft_byte(0x7F); // rows 0..127
    const uint8_t gp[]={0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,0x29,0x25,0x2b,0x39,0x00,0x01,0x03,0x10};
    tft_cmd(0xE0); tft_data(gp, 16);
    const uint8_t gn[]={0x03,0x1d,0x07,0x06,0x2e,0x2c,0x29,0x2d,0x2e,0x2e,0x37,0x3f,0x00,0x00,0x02,0x10};
    tft_cmd(0xE1); tft_data(gn, 16);
    tft_cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));
    tft_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(100));
}

void tft_init(void)
{
    s_fb    = heap_caps_malloc(FB_PIXELS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_shown = heap_caps_malloc(FB_PIXELS * 2, MALLOC_CAP_INTERNAL);
    s_stage = heap_caps_malloc(STAGE_BYTES,   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK((s_fb && s_shown && s_stage) ? ESP_OK : ESP_ERR_NO_MEM);
    memset(s_fb, 0, FB_PIXELS * 2);
    memset(s_shown, 0, FB_PIXELS * 2);
    ESP_LOGI(TAG, "framebuffer ready (2 x %u KB + %u KB staging), SPI %u MHz",
             (unsigned)(FB_PIXELS * 2 / 1024), (unsigned)(STAGE_BYTES / 1024),
             (unsigned)(TFT_SPI_HZ / 1000000));

    spi_bus_config_t bus = {
        .mosi_io_num = TFT_MOSI, .miso_io_num = -1, .sclk_io_num = TFT_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = STAGE_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = TFT_SPI_HZ, .mode = 0,
        .spics_io_num = TFT_CS, .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));
    gpio_config_t pins = {
        .pin_bit_mask = (1ULL<<TFT_DC)|(1ULL<<TFT_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pins);
    tft_panel_init();
}

// ── 5×8 font (ASCII 32–90) ───────────────────────────────────
static const uint8_t FONT[59][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};

// draw_text with configurable scale (1=tiny, 2=normal)
void tft_text(const char *str, int x, int y, uint16_t fg, uint16_t bg, int sc)
{
    int cw = 5*sc + sc, ch = 8*sc;
    int ml = (TFT_W - x) / cw;
    int n  = (int)strlen(str);
    if (n > ml) n = ml;
    if (n <= 0) return;
    int w = n * cw;
    if (x < 0 || y < 0) return;
    uint16_t sfg = swap16(fg), sbg = swap16(bg);
    if (s_cx0 > 0 || s_cy0 > 0 || s_cx1 < TFT_W - 1 || s_cy1 < TFT_H - 1) {   // clipped: per pixel
        for (int row = 0; row < ch; row++) {
            int bit = row / sc;
            for (int ci = 0; ci < n; ci++) {
                char c = str[ci];
                const uint8_t *g = (c>=32&&c<=90) ? FONT[c-32] : FONT[0];
                for (int px = 0; px < cw; px++) {
                    int col = px / sc;
                    bool on = col < 5 && ((g[col] >> bit) & 1);
                    fb_px(x + ci * cw + px, y + row, on ? sfg : sbg);
                }
            }
        }
        return;
    }
    for (int row = 0; row < ch; row++) {
        int yy = y + row;
        if (yy >= TFT_H) break;
        int bit = row / sc;
        uint16_t *line = s_fb + yy * TFT_W + x;
        for (int px = 0; px < w; px++) line[px] = sbg;   // character cells incl. spacing
        for (int ci = 0; ci < n; ci++) {
            char c = str[ci];
            const uint8_t *g = (c>=32&&c<=90) ? FONT[c-32] : FONT[0];
            for (int col = 0; col < 5; col++) {
                uint16_t color = ((g[col]>>bit)&1) ? sfg : sbg;
                int px = ci*cw + col*sc;
                for (int s = 0; s < sc && (px+s)<w; s++) line[px+s] = color;
            }
        }
    }
}

// Bold text: draw twice with 1px x-offset for a heavier stroke.
void tft_text_bold(const char *str, int x, int y, uint16_t fg, uint16_t bg, int sc)
{
    tft_text(str, x,   y, fg, bg, sc);
    tft_text(str, x+1, y, fg, bg, sc);
}

// Outline rounded rectangle (corner radius ≈ 2 px).
void tft_rrect(int x0, int y0, int x1, int y1, uint16_t c)
{
    tft_fill(x0+2, y0,   x1-2, y0,   c);
    tft_fill(x0+2, y1,   x1-2, y1,   c);
    tft_fill(x0,   y0+2, x0,   y1-2, c);
    tft_fill(x1,   y0+2, x1,   y1-2, c);
    tft_fill(x0+1, y0+1, x0+1, y0+1, c);
    tft_fill(x1-1, y0+1, x1-1, y0+1, c);
    tft_fill(x0+1, y1-1, x0+1, y1-1, c);
    tft_fill(x1-1, y1-1, x1-1, y1-1, c);
}

// Filled rounded rectangle (corner radius ≈ 2 px).
void tft_rrect_fill(int x0, int y0, int x1, int y1, uint16_t c)
{
    for (int y = y0; y <= y1; y++) {
        int ins = (y == y0 || y == y1) ? 2 : (y == y0+1 || y == y1-1) ? 1 : 0;
        tft_fill(x0+ins, y, x1-ins, y, c);
    }
}

// ── Clipped primitives (redesign 2026-11) ────────────────────
void tft_pixel(int x, int y, uint16_t c) { fb_px(x, y, swap16(c)); }

void tft_rect(int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > TFT_W - 1) x1 = TFT_W - 1;
    if (y1 > TFT_H - 1) y1 = TFT_H - 1;
    if (x1 < x0 || y1 < y0) return;
    tft_fill((uint8_t)x0, (uint8_t)y0, (uint8_t)x1, (uint8_t)y1, c);
}

void tft_line_dash(int x0, int y0, int x1, int y1, int every, uint16_t c)
{
    uint16_t sw = swap16(c);
    int dx = abs(x1-x0), dy = abs(y1-y0);
    int sx = (x0<x1) ? 1 : -1, sy = (y0<y1) ? 1 : -1;
    int err = dx - dy, n = 0;
    if (every < 1) every = 1;
    for (;;) {
        if (n++ % every == 0) fb_px(x0, y0, sw);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void tft_line_thick(int x0, int y0, int x1, int y1, uint16_t c)
{
    tft_line(x0, y0, x1, y1, c);
    tft_line(x0, y0 + 1, x1, y1 + 1, c);
    tft_line(x0 + 1, y0, x1 + 1, y1, c);
}

void tft_disc(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int hw = (int)(sqrtf((float)(r * r - dy * dy)) + 0.35f);
        tft_rect(cx - hw, cy + dy, cx + hw, cy + dy, c);
    }
}

void tft_ring(int cx, int cy, int r, uint16_t c)
{
    uint16_t sw = swap16(c);
    int x = r, y = 0, e = 1 - r;
    while (x >= y) {
        fb_px(cx + x, cy + y, sw); fb_px(cx + y, cy + x, sw);
        fb_px(cx - y, cy + x, sw); fb_px(cx - x, cy + y, sw);
        fb_px(cx - x, cy - y, sw); fb_px(cx - y, cy - x, sw);
        fb_px(cx + y, cy - x, sw); fb_px(cx + x, cy - y, sw);
        y++;
        if (e < 0) e += 2 * y + 1; else { x--; e += 2 * (y - x) + 1; }
    }
}

void tft_arc(int cx, int cy, int r, float a0, float a1, uint16_t c)
{
    uint16_t sw = swap16(c);
    int n = r * 3; if (n < 6) n = 6;
    for (int i = 0; i <= n; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)n;
        fb_px(cx + (int)lrintf(cosf(a) * r), cy + (int)lrintf(sinf(a) * r), sw);
    }
}
