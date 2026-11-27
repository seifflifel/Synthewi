// ST7735 TFT driver — see tft.h. All SPI traffic to the panel lives here.

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "tft.h"

// ── TFT hardware pins ────────────────────────────────────────
#define TFT_SCLK  GPIO_NUM_36
#define TFT_MOSI  GPIO_NUM_35
#define TFT_CS    GPIO_NUM_37
#define TFT_DC    GPIO_NUM_45
#define TFT_RST   GPIO_NUM_21

// ── TFT primitives ───────────────────────────────────────────
static spi_device_handle_t s_spi;
static uint8_t             s_lb[TFT_W * 2]; // line buffer

static void tft_xfer(const uint8_t *d, int n, int dc)
{
    gpio_set_level(TFT_DC, dc);
    spi_transaction_t t = { .length = (size_t)n * 8, .tx_buffer = d };
    spi_device_polling_transmit(s_spi, &t);
}
static void tft_cmd(uint8_t c)               { tft_xfer(&c, 1, 0); }
static void tft_byte(uint8_t b)              { tft_xfer(&b, 1, 1); }
static void tft_data(const uint8_t *b, int n){ tft_xfer(b, n, 1); }

static void tft_win(uint8_t x0,uint8_t y0,uint8_t x1,uint8_t y1)
{
    tft_cmd(0x2A); tft_byte(0); tft_byte(x0); tft_byte(0); tft_byte(x1);
    tft_cmd(0x2B); tft_byte(0); tft_byte(y0); tft_byte(0); tft_byte(y1);
    tft_cmd(0x2C);
}

static void tft_pixels(int n)
{
    spi_transaction_t t = { .length = (size_t)n * 16, .tx_buffer = s_lb };
    gpio_set_level(TFT_DC, 1);
    spi_device_polling_transmit(s_spi, &t);
}

void tft_fill(uint8_t x0,uint8_t y0,uint8_t x1,uint8_t y1, uint16_t c)
{
    int w = x1 - x0 + 1;
    if (w <= 0) return;
    tft_win(x0, y0, x1, y1);
    uint8_t hi = c >> 8, lo = c & 0xFF;
    for (int i = 0; i < w; i++) { s_lb[i*2] = hi; s_lb[i*2+1] = lo; }
    for (int r = y0; r <= y1; r++) tft_pixels(w);
}

void tft_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1-x0), dy = abs(y1-y0);
    int sx = (x0<x1) ? 1 : -1, sy = (y0<y1) ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        tft_fill((uint8_t)x0, (uint8_t)y0, (uint8_t)x0, (uint8_t)y0, c);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
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
    spi_bus_config_t bus = {
        .mosi_io_num = TFT_MOSI, .miso_io_num = -1, .sclk_io_num = TFT_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = TFT_W * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 10*1000*1000, .mode = 0,
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
    tft_win(x, y, x+w-1, y+ch-1);
    for (int row = 0; row < ch; row++) {
        int bit = row / sc;
        uint8_t bhi = bg>>8, blo = bg&0xFF;
        for (int px = 0; px < w; px++) { s_lb[px*2]=bhi; s_lb[px*2+1]=blo; }
        for (int ci = 0; ci < n; ci++) {
            char c = str[ci];
            const uint8_t *g = (c>=32&&c<=90) ? FONT[c-32] : FONT[0];
            for (int col = 0; col < 5; col++) {
                uint16_t color = ((g[col]>>bit)&1) ? fg : bg;
                int px = ci*cw + col*sc;
                for (int s = 0; s < sc && (px+s)<w; s++) {
                    s_lb[(px+s)*2]=color>>8; s_lb[(px+s)*2+1]=color&0xFF;
                }
            }
        }
        tft_pixels(w);
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
