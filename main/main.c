#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "amy_engine.h"
#include "touch_telemetry.h"

static const char *TAG = "Synthewi";

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
#define OUTPUT_GAIN_BOOST    2
#define AUDIO_BLOCK_SAMPLES  256
#define I2S_BCLK_GPIO   GPIO_NUM_38
#define I2S_LRCLK_GPIO  GPIO_NUM_39
#define I2S_DOUT_GPIO   GPIO_NUM_40

static i2s_chan_handle_t s_i2s_tx = NULL;

static void i2s_audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL));
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_LRCLK_GPIO,  .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));
    ESP_LOGI(TAG, "I2S ready");
}

static void audio_task(void *arg)
{
    (void)arg;
    static int16_t mono[AUDIO_BLOCK_SAMPLES];
    static int32_t out32[AUDIO_BLOCK_SAMPLES * 2];
    int32_t    peak       = 0;
    TickType_t last_log   = xTaskGetTickCount();

    for (;;) {
        amy_engine_render_mono_16(mono, AUDIO_BLOCK_SAMPLES);
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            int32_t s = (int32_t)mono[i] * OUTPUT_GAIN_BOOST;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            int32_t a = s < 0 ? -s : s;
            if (a > peak) peak = a;
            int32_t s32 = s << 16;
            out32[i*2] = s32; out32[i*2+1] = s32;
        }
        size_t written = 0;
        i2s_channel_write(s_i2s_tx, out32, sizeof(out32), &written, portMAX_DELAY);

        TickType_t now = xTaskGetTickCount();
        if ((now - last_log) >= pdMS_TO_TICKS(1000)) {
            ESP_LOGI(TAG, "audio peak=%ld", (long)peak);
            peak     = 0;
            last_log = now;
        }
    }
}

// ---------------------------------------------------------------------------
// Touch → AMY bridge
// ---------------------------------------------------------------------------
static const uint8_t s_pad_notes[8] = {60, 62, 64, 67, 69, 72, 74, 76};

static void on_touch_event(uint8_t pad, bool is_touching)
{
    if (pad >= 8) return;
    if (is_touching) {
        ESP_LOGI(TAG, "NOTE ON  pad=%d note=%d", pad, s_pad_notes[pad]);
        amy_engine_note_on(pad, s_pad_notes[pad]);
    } else {
        ESP_LOGI(TAG, "NOTE OFF pad=%d", pad);
        amy_engine_note_off(pad);
    }
}

static void on_pressure(uint8_t pad, float pressure_norm)
{
    amy_engine_update_pressure(pad, pressure_norm);
}

// ---------------------------------------------------------------------------
// TFT (ST7735 1.8" 128×160)
// ---------------------------------------------------------------------------
#define TFT_SCLK  GPIO_NUM_36
#define TFT_MOSI  GPIO_NUM_35
#define TFT_CS    GPIO_NUM_37
#define TFT_DC    GPIO_NUM_45
#define TFT_RST   GPIO_NUM_21
#define TFT_W     128
#define TFT_H     160

#define BLACK   0x0000
#define WHITE   0xFFFF
#define GREEN   0x07E0
#define RED     0xF800
#define YELLOW  0xFFE0
#define DKGRAY  0x2104
#define LTGRAY  0x8410
#define CYAN    0x07FF

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

static void tft_set_window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    tft_cmd(0x2A); tft_byte(0); tft_byte(x0); tft_byte(0); tft_byte(x1);
    tft_cmd(0x2B); tft_byte(0); tft_byte(y0); tft_byte(0); tft_byte(y1);
    tft_cmd(0x2C);
}

static void tft_send_n(int n)
{
    spi_transaction_t t = { .length = (size_t)n * 16, .tx_buffer = s_linebuf };
    gpio_set_level(TFT_DC, 1);
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_fill_rect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint16_t color)
{
    int w = x1 - x0 + 1;
    if (w <= 0) return;
    tft_set_window(x0, y0, x1, y1);
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int px = 0; px < w; px++) { s_linebuf[px*2] = hi; s_linebuf[px*2+1] = lo; }
    for (int row = y0; row <= y1; row++) tft_send_n(w);
}

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

// ---------------------------------------------------------------------------
// 5×8 bitmap font (column-major, bit 0 = top row), ASCII 32–90
// ---------------------------------------------------------------------------
static const uint8_t font5x8[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '\''
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

#define FONT_SCALE 2
#define CHAR_W     (5 * FONT_SCALE + FONT_SCALE)  // 12px
#define CHAR_H     (8 * FONT_SCALE)               // 16px

static void draw_text(const char *str, int x, int y, uint16_t fg, uint16_t bg)
{
    // Clip to screen — prevents linebuf overflow and invalid window coords
    int max_chars = (TFT_W - x) / CHAR_W;
    int len = strlen(str);
    if (len > max_chars) len = max_chars;
    if (len <= 0) return;
    int w = len * CHAR_W;

    tft_set_window(x, y, x + w - 1, y + CHAR_H - 1);
    for (int row = 0; row < CHAR_H; row++) {
        int bit = row / FONT_SCALE;
        // Fill background first
        uint8_t bhi = bg >> 8, blo = bg & 0xFF;
        for (int px = 0; px < w; px++) { s_linebuf[px*2] = bhi; s_linebuf[px*2+1] = blo; }
        // Draw glyphs
        for (int ci = 0; ci < len; ci++) {
            char c = str[ci];
            const uint8_t *g = (c >= 32 && c <= 90) ? font5x8[c - 32] : font5x8[0];
            for (int col = 0; col < 5; col++) {
                uint16_t color = ((g[col] >> bit) & 1) ? fg : bg;
                int px = ci * CHAR_W + col * FONT_SCALE;
                for (int s = 0; s < FONT_SCALE && (px + s) < w; s++) {
                    s_linebuf[(px+s)*2]   = color >> 8;
                    s_linebuf[(px+s)*2+1] = color & 0xFF;
                }
            }
        }
        tft_send_n(w);
    }
}

static void draw_text_num(int x, int y, uint16_t fg, uint16_t bg, const char *label, uint32_t val)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%s%5" PRIu32, label, val);
    draw_text(buf, x, y, fg, bg);
}

// ---------------------------------------------------------------------------
// Rotary encoder
// ---------------------------------------------------------------------------
#define ENC_CLK  GPIO_NUM_15
#define ENC_DT   GPIO_NUM_16
#define ENC_SW   GPIO_NUM_17

static volatile int32_t s_enc_steps = 0;
static volatile int     s_btn_flag  = 0;
static volatile int64_t s_btn_press_time = 0;  // µs timestamp of last press

static void IRAM_ATTR enc_ab_isr(void *arg)
{
    static uint8_t old_AB = 3;
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
    if (now - last_us < 50000) return;
    last_us = now;
    s_btn_press_time = now;
    s_btn_flag = 1;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
#define PAD_COUNT  8
#define LONG_PRESS_US  800000  // 800 ms

typedef enum { MODE_BROWSE, MODE_EDIT } ui_mode_t;

static ui_mode_t s_mode     = MODE_BROWSE;
static int       s_cur_pad  = 0;
static uint16_t  s_edit_thr = 0;   // working copy while editing

// Draw the browse screen for a given pad
static void ui_draw_browse(int pad)
{
    tft_fill_rect(0, 0, TFT_W-1, TFT_H-1, BLACK);

    // Header: "< PAD X >"
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "  PAD %d  ", pad + 1);
    draw_text(hdr, 0, 4, YELLOW, BLACK);

    // Divider
    tft_fill_rect(0, 22, TFT_W-1, 23, DKGRAY);

    uint16_t raw   = touch_telemetry_get_raw((uint8_t)pad);
    uint16_t base  = touch_telemetry_get_baseline((uint8_t)pad);
    uint16_t thr   = touch_telemetry_get_threshold((uint8_t)pad);

    draw_text_num(4, 32, WHITE,  BLACK, "RAW  ", raw);
    draw_text_num(4, 52, LTGRAY, BLACK, "BASE ", base);
    draw_text_num(4, 72, GREEN,  BLACK, "THR  ", thr);

    // Visual bar: how close raw is to threshold (green fills toward red mark)
    uint8_t bar_w = (thr > 0 && raw < thr * 4)
                    ? (uint8_t)(((uint32_t)raw * 100) / (thr * 4))
                    : 100;
    bar_w = (bar_w * (TFT_W - 8)) / 100;
    tft_fill_rect(4, 96, 4 + bar_w, 108, GREEN);
    if (4 + bar_w < TFT_W - 4)
        tft_fill_rect(4 + bar_w, 96, TFT_W - 5, 108, DKGRAY);
    // threshold marker
    uint8_t thr_x = (uint8_t)(((uint32_t)thr * (TFT_W - 8)) / (thr * 4)) + 4;
    if (thr_x < TFT_W - 2) tft_fill_rect(thr_x, 90, thr_x+1, 114, RED);

    draw_text("ROT:NEXT  ", 0, 130, DKGRAY, BLACK);
    draw_text("BTN:EDIT  ", 0, 148, DKGRAY, BLACK);
}

// Draw the edit screen
static void ui_draw_edit(int pad, uint16_t thr, bool full_redraw)
{
    if (full_redraw) {
        tft_fill_rect(0, 0, TFT_W-1, TFT_H-1, BLACK);
        char hdr[24];
        snprintf(hdr, sizeof(hdr), "PAD %d EDIT", pad + 1);
        draw_text(hdr, 0, 4, CYAN, BLACK);
        tft_fill_rect(0, 22, TFT_W-1, 23, DKGRAY);
        draw_text("HOLD=SAVE ", 0, 148, DKGRAY, BLACK);
    }

    uint16_t raw  = touch_telemetry_get_raw((uint8_t)pad);
    uint16_t base = touch_telemetry_get_baseline((uint8_t)pad);
    draw_text_num(4, 32, WHITE,  BLACK, "RAW  ", raw);
    draw_text_num(4, 52, LTGRAY, BLACK, "BASE ", base);
    draw_text_num(4, 72, YELLOW, BLACK, "THR  ", thr);

    // Bar
    uint32_t scale = (thr > 0) ? thr * 4 : 400;
    uint8_t bar_w  = (raw < scale) ? (uint8_t)(((uint32_t)raw * (TFT_W-8)) / scale) : (TFT_W-8);
    tft_fill_rect(4, 96, 4 + bar_w, 108, CYAN);
    if (4 + bar_w < TFT_W - 4)
        tft_fill_rect(4 + bar_w, 96, TFT_W - 5, 108, DKGRAY);
    uint8_t thr_x = (uint8_t)(((TFT_W-8)) / 4) + 4;
    if (thr_x < TFT_W - 2) tft_fill_rect(thr_x, 90, thr_x+1, 114, RED);

    draw_text("ROT:ADJ THR", 0, 130, DKGRAY, BLACK);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    // AMY
    amy_engine_init();
    ESP_LOGI(TAG, "AMY ready");

    // I2S
    i2s_audio_init();
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 10, NULL, 0);

    // Touch
    touch_telemetry_set_event_cb(on_touch_event);
    touch_telemetry_set_pressure_cb(on_pressure);
    err = touch_telemetry_start();
    if (err != ESP_OK) ESP_LOGW(TAG, "touch init failed (0x%x)", err);

    // SPI bus for TFT
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
    gpio_config_t out_io = {
        .pin_bit_mask = (1ULL<<TFT_DC)|(1ULL<<TFT_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_io);
    tft_init();

    // Encoder
    gpio_config_t enc_ab = {
        .pin_bit_mask = (1ULL<<ENC_CLK)|(1ULL<<ENC_DT),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&enc_ab);
    gpio_config_t enc_sw_cfg = {
        .pin_bit_mask = (1ULL<<ENC_SW), .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&enc_sw_cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(ENC_CLK, enc_ab_isr, NULL);
    gpio_isr_handler_add(ENC_DT,  enc_ab_isr, NULL);
    gpio_isr_handler_add(ENC_SW,  enc_sw_isr, NULL);

    // Initial screen
    ui_draw_browse(s_cur_pad);
    ESP_LOGI(TAG, "running — AMY synthesis active");

    int32_t    last_steps    = 0;
    TickType_t last_log_tick = xTaskGetTickCount();
    TickType_t last_refresh  = xTaskGetTickCount();

    for (;;) {
        TickType_t now_tick = xTaskGetTickCount();

        // --- Encoder rotation ---
        int32_t steps = s_enc_steps;
        if (steps != last_steps) {
            int delta = steps - last_steps;
            last_steps = steps;

            if (s_mode == MODE_BROWSE) {
                s_cur_pad = (s_cur_pad + delta % PAD_COUNT + PAD_COUNT) % PAD_COUNT;
                ui_draw_browse(s_cur_pad);

            } else {
                // Adjust threshold: 1 step = +/-5, faster if held
                int step = (delta > 0) ? 5 : -5;
                int new_thr = (int)s_edit_thr + step * delta;
                if (new_thr < 10)  new_thr = 10;
                if (new_thr > 5000) new_thr = 5000;
                s_edit_thr = (uint16_t)new_thr;
                ui_draw_edit(s_cur_pad, s_edit_thr, false);
            }
        }

        // --- Button ---
        if (s_btn_flag) {
            s_btn_flag = 0;
            int64_t press_time = s_btn_press_time;

            if (s_mode == MODE_BROWSE) {
                // Enter edit
                s_mode     = MODE_EDIT;
                s_edit_thr = touch_telemetry_get_threshold((uint8_t)s_cur_pad);
                ui_draw_edit(s_cur_pad, s_edit_thr, true);

            } else {
                // Check for long press by waiting up to 800ms for button release
                // We poll the GPIO directly here since ISR only fires on press
                int held_ms = 0;
                while (gpio_get_level(ENC_SW) == 0 && held_ms < 900) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    held_ms += 20;
                }
                if (held_ms >= 800) {
                    // Long press: save and exit edit
                    touch_telemetry_set_threshold((uint8_t)s_cur_pad, s_edit_thr);
                    s_mode = MODE_BROWSE;
                    ui_draw_browse(s_cur_pad);
                    ESP_LOGI(TAG, "pad %d threshold saved: %u", s_cur_pad + 1, s_edit_thr);
                }
                // Short press in edit mode: do nothing (just ignore)
                (void)press_time;
            }
        }

        // --- Live refresh in edit mode (raw changes in real-time) ---
        if (s_mode == MODE_EDIT && (now_tick - last_refresh) >= pdMS_TO_TICKS(100)) {
            ui_draw_edit(s_cur_pad, s_edit_thr, false);
            last_refresh = now_tick;
        }
        if (s_mode == MODE_BROWSE && (now_tick - last_refresh) >= pdMS_TO_TICKS(250)) {
            ui_draw_browse(s_cur_pad);
            last_refresh = now_tick;
        }

        // --- Throttled monitor: all pads every 1 s ---
        if ((now_tick - last_log_tick) >= pdMS_TO_TICKS(1000)) {
            last_log_tick = now_tick;
            ESP_LOGI(TAG, "touch raw/base/thr:");
            for (int i = 0; i < PAD_COUNT; i++) {
                ESP_LOGI(TAG, "  p%d raw=%-5u base=%-5u thr=%-5u",
                         i+1,
                         touch_telemetry_get_raw((uint8_t)i),
                         touch_telemetry_get_baseline((uint8_t)i),
                         touch_telemetry_get_threshold((uint8_t)i));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
