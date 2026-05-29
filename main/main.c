#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "hw_test";

// ---------------------------------------------------------------------------
// TFT pins (ST7735 1.8" 128x160)
// ---------------------------------------------------------------------------
#define TFT_SCLK  GPIO_NUM_36
#define TFT_MOSI  GPIO_NUM_35
#define TFT_CS    GPIO_NUM_37
#define TFT_DC    GPIO_NUM_45
#define TFT_RST   GPIO_NUM_21
#define TFT_W     128
#define TFT_H     160

// ---------------------------------------------------------------------------
// Encoder pins (EC11)
// ---------------------------------------------------------------------------
#define ENC_CLK   GPIO_NUM_15
#define ENC_DT    GPIO_NUM_16
#define ENC_SW    GPIO_NUM_17

// ---------------------------------------------------------------------------
// RGB565 colors
// ---------------------------------------------------------------------------
#define BLACK     0x0000
#define WHITE     0xFFFF
#define GREEN     0x07E0
#define DKGRAY    0x2104
#define YELLOW    0xFFE0
#define BLUE      0x001F

// ---------------------------------------------------------------------------
// 5x8 bitmap font (column-major, bit 0 = top row), ASCII 32–90 (' ' to 'Z')
// ---------------------------------------------------------------------------
static const uint8_t font5x8[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x14,0x08,0x3E,0x08,0x14}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x08,0x14,0x22,0x41,0x00}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x00,0x41,0x22,0x14,0x08}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
};

// ---------------------------------------------------------------------------
// Menu items
// ---------------------------------------------------------------------------
static const char *menu_labels[] = { "WAVE", "FILTER", "LFO", "ENV", "GLIDE" };
#define MENU_COUNT  5
#define ITEM_H      32   // 5 items × 32px = 160px = full screen height
#define FONT_SCALE  2    // each pixel drawn 2×2
#define CHAR_W      (5 * FONT_SCALE + FONT_SCALE)  // 12px per char
#define CHAR_H      (8 * FONT_SCALE)               // 16px tall

// ---------------------------------------------------------------------------
// Encoder state (updated in ISR)
// ---------------------------------------------------------------------------
static volatile int32_t s_enc_steps = 0;  // +1 CW, -1 CCW per detent
static volatile int      s_btn_flag  = 0;  // set when button pressed

static void IRAM_ATTR enc_ab_isr(void *arg)
{
    // Gray-code quadrature decoder: 4 half-steps = 1 detent
    static uint8_t old_AB = 3;  // CLK=1, DT=1 at rest
    static int8_t  acc    = 0;
    static const int8_t table[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};

    uint8_t new_AB = (gpio_get_level(ENC_CLK) << 1) | gpio_get_level(ENC_DT);
    acc += table[(old_AB << 2) | new_AB];
    old_AB = new_AB;

    if      (acc >=  4) { s_enc_steps++; acc = 0; }
    else if (acc <= -4) { s_enc_steps--; acc = 0; }
}

static void IRAM_ATTR enc_sw_isr(void *arg)
{
    static int64_t last_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_us < 50000) return;  // 50ms debounce for button
    last_us = now;
    s_btn_flag = 1;
}

// ---------------------------------------------------------------------------
// SPI / TFT driver
// ---------------------------------------------------------------------------
static spi_device_handle_t s_spi;
static uint8_t s_linebuf[TFT_W * 2];

static void tft_write(const uint8_t *data, int len, int is_data)
{
    gpio_set_level(TFT_DC, is_data);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(s_spi, &t);
}
static void tft_cmd(uint8_t c)               { tft_write(&c, 1, 0); }
static void tft_byte(uint8_t b)              { tft_write(&b, 1, 1); }
static void tft_buf(const uint8_t *b, int n) { tft_write(b, n, 1); }

static void tft_init(void)
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
    tft_cmd(0x20);
    tft_cmd(0x36); tft_byte(0xC8);
    tft_cmd(0x3A); tft_byte(0x05);
    tft_cmd(0x2A); tft_byte(0); tft_byte(0); tft_byte(0); tft_byte(0x7F);
    tft_cmd(0x2B); tft_byte(0); tft_byte(0); tft_byte(0); tft_byte(0x9F);
    const uint8_t gp[] = {0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,0x29,0x25,0x2b,0x39,0x00,0x01,0x03,0x10};
    tft_cmd(0xE0); tft_buf(gp, 16);
    const uint8_t gn[] = {0x03,0x1d,0x07,0x06,0x2e,0x2c,0x29,0x2d,0x2e,0x2e,0x37,0x3f,0x00,0x00,0x02,0x10};
    tft_cmd(0xE1); tft_buf(gn, 16);
    tft_cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));
    tft_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(100));
}

// Send pixels for a single row into the current window (DC already high)
static void tft_send_line(void)
{
    spi_transaction_t t = { .length = TFT_W * 16, .tx_buffer = s_linebuf };
    gpio_set_level(TFT_DC, 1);
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_set_window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    tft_cmd(0x2A); tft_byte(0); tft_byte(x0); tft_byte(0); tft_byte(x1);
    tft_cmd(0x2B); tft_byte(0); tft_byte(y0); tft_byte(0); tft_byte(y1);
    tft_cmd(0x2C);
}

// ---------------------------------------------------------------------------
// Draw a menu item — single set_window + one line per pixel row (fast)
// ---------------------------------------------------------------------------
static void draw_item(int idx, bool selected)
{
    const char *label = menu_labels[idx];
    int   label_len   = strlen(label);
    int   text_width  = label_len * CHAR_W;
    int   text_x      = (TFT_W - text_width) / 2;   // centered
    int   y_top       = idx * ITEM_H;
    int   text_y_off  = (ITEM_H - CHAR_H) / 2;       // vertical centering

    uint16_t bg = selected ? GREEN  : DKGRAY;
    uint16_t fg = selected ? BLACK  : WHITE;

    // Cursor indicator on the left for selected item
    uint16_t cursor_color = selected ? YELLOW : bg;

    tft_set_window(0, y_top, TFT_W - 1, y_top + ITEM_H - 1);

    for (int row = 0; row < ITEM_H; row++) {
        // Background
        for (int px = 0; px < TFT_W; px++) {
            s_linebuf[px * 2]     = bg >> 8;
            s_linebuf[px * 2 + 1] = bg & 0xFF;
        }

        // Left cursor bar (4px wide)
        if (selected) {
            for (int px = 0; px < 4; px++) {
                s_linebuf[px * 2]     = cursor_color >> 8;
                s_linebuf[px * 2 + 1] = cursor_color & 0xFF;
            }
        }

        // Text pixels (only within the text row band)
        int font_row = row - text_y_off;
        if (font_row >= 0 && font_row < CHAR_H) {
            int bit = font_row / FONT_SCALE;   // 0-7 font bit row
            for (int ci = 0; ci < label_len; ci++) {
                char c = label[ci];
                if (c < 32 || c > 90) continue;
                const uint8_t *glyph = font5x8[c - 32];
                for (int col = 0; col < 5; col++) {
                    uint16_t color = ((glyph[col] >> bit) & 1) ? fg : bg;
                    int px = text_x + ci * CHAR_W + col * FONT_SCALE;
                    for (int s = 0; s < FONT_SCALE && (px + s) < TFT_W; s++) {
                        s_linebuf[(px + s) * 2]     = color >> 8;
                        s_linebuf[(px + s) * 2 + 1] = color & 0xFF;
                    }
                }
            }
        }

        tft_send_line();
    }
}

static void draw_menu(int selected)
{
    for (int i = 0; i < MENU_COUNT; i++)
        draw_item(i, i == selected);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    // --- SPI bus ---
    spi_bus_config_t bus = {
        .mosi_io_num   = TFT_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = TFT_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_W * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = TFT_CS,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    // --- DC + RST as plain output GPIOs ---
    gpio_config_t out_io = {
        .pin_bit_mask = (1ULL << TFT_DC) | (1ULL << TFT_RST),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_io);

    // CLK + DT: any edge for quadrature decoding
    gpio_config_t enc_ab = {
        .pin_bit_mask = (1ULL << ENC_CLK) | (1ULL << ENC_DT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&enc_ab);

    // SW: falling edge only
    gpio_config_t enc_sw = {
        .pin_bit_mask = (1ULL << ENC_SW),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&enc_sw);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(ENC_CLK, enc_ab_isr, NULL);
    gpio_isr_handler_add(ENC_DT,  enc_ab_isr, NULL);
    gpio_isr_handler_add(ENC_SW,  enc_sw_isr, NULL);

    // --- Init TFT ---
    tft_init();
    ESP_LOGI(TAG, "TFT ready");

    // --- Draw initial menu ---
    int selected = 0;
    draw_menu(selected);
    ESP_LOGI(TAG, "menu drawn — rotate encoder to navigate, press to select");

    // --- Main loop: react to encoder ---
    int32_t last_steps = 0;
    for (;;) {
        int32_t steps = s_enc_steps;
        if (steps != last_steps) {
            int delta = steps - last_steps;
            last_steps = steps;

            int prev = selected;
            selected = (selected + delta % MENU_COUNT + MENU_COUNT) % MENU_COUNT;

            if (selected != prev) {
                // Only redraw the two changed items — fast
                draw_item(prev,     false);
                draw_item(selected, true);
                ESP_LOGI(TAG, "selected: %s", menu_labels[selected]);
            }
        }

        if (s_btn_flag) {
            s_btn_flag = 0;
            ESP_LOGI(TAG, "button pressed — selected: %s", menu_labels[selected]);
            // Flash the selected item briefly as visual feedback
            draw_item(selected, false);
            vTaskDelay(pdMS_TO_TICKS(80));
            draw_item(selected, true);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
