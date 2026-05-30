#pragma once
// UI module — include exactly once from main.c

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "touch_telemetry.h"
#include "amy_engine.h"

// ── TFT hardware pins ────────────────────────────────────────
#define TFT_SCLK  GPIO_NUM_36
#define TFT_MOSI  GPIO_NUM_35
#define TFT_CS    GPIO_NUM_37
#define TFT_DC    GPIO_NUM_45
#define TFT_RST   GPIO_NUM_21
#define TFT_W     128
#define TFT_H     160

// RGB565 palette
#define C_BLACK   0x0000u
#define C_WHITE   0xFFFFu
#define C_GREEN   0x07E0u
#define C_RED     0xF800u
#define C_YELLOW  0xFFE0u
#define C_CYAN    0x07FFu
#define C_DKGRAY  0x2104u
#define C_LTGRAY  0x8410u

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

static void tft_fill(uint8_t x0,uint8_t y0,uint8_t x1,uint8_t y1, uint16_t c)
{
    int w = x1 - x0 + 1;
    if (w <= 0) return;
    tft_win(x0, y0, x1, y1);
    uint8_t hi = c >> 8, lo = c & 0xFF;
    for (int i = 0; i < w; i++) { s_lb[i*2] = hi; s_lb[i*2+1] = lo; }
    for (int r = y0; r <= y1; r++) tft_pixels(w);
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
    tft_cmd(0x20); tft_cmd(0x36); tft_byte(0xC8);
    tft_cmd(0x3A); tft_byte(0x05);
    tft_cmd(0x2A); tft_byte(0); tft_byte(0); tft_byte(0); tft_byte(0x7F);
    tft_cmd(0x2B); tft_byte(0); tft_byte(0); tft_byte(0); tft_byte(0x9F);
    const uint8_t gp[]={0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,0x29,0x25,0x2b,0x39,0x00,0x01,0x03,0x10};
    tft_cmd(0xE0); tft_data(gp, 16);
    const uint8_t gn[]={0x03,0x1d,0x07,0x06,0x2e,0x2c,0x29,0x2d,0x2e,0x2e,0x37,0x3f,0x00,0x00,0x02,0x10};
    tft_cmd(0xE1); tft_data(gn, 16);
    tft_cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));
    tft_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(100));
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
static void tft_text(const char *str, int x, int y, uint16_t fg, uint16_t bg, int sc)
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

// ── UI state ─────────────────────────────────────────────────
typedef enum { UI_MENU, UI_WAVEFORM, UI_CALIB, UI_EFFECTS, UI_ADSR } ui_section_t;

static ui_section_t s_sec      = UI_MENU;
static int          s_menu_cur = 0;
static bool         s_dirty    = true;

// Waveform section
static int          s_wave_cur = 0;
static const uint8_t s_wave_ids[3]      = {AMY_ENGINE_WAVE_SQUARE, AMY_ENGINE_WAVE_SAW_DOWN, AMY_ENGINE_WAVE_TRIANGLE};
static const char * const s_wave_names[3] = {"SQUARE", "SAW", "TRIANGLE"};

// Calibration section
static int      s_cal_pad     = 0;
static bool     s_cal_editing = false;
static uint16_t s_cal_thr     = 100;

// Effects section
// items 0-9: FltType CUT RES RVB RDC ECH EFB GLD LFRT LFDP
static int      s_fx_cur      = 0;
static int      s_fx_scroll   = 0;
static bool     s_fx_editing  = false;
static uint8_t  s_fx_flt_type = 0;       // 0=LPF 1=BPF 2=HPF
static uint16_t s_fx_vals[9];            // [0]=cut [1]=res [2]=rvb [3]=rdc [4]=ech [5]=efb [6]=gld [7]=lfr [8]=lfd

// ADSR section (0-10000 internal scale)
static int      s_adsr_cur     = 0;  // 0=A 1=D 2=S 3=R
static bool     s_adsr_editing = false;
static uint16_t s_adsr[4];           // loaded from engine on enter

// ── ADSR display conversion ───────────────────────────────────
static uint32_t ui_a_ms(uint16_t v)  { return (uint32_t)(2.0f  + (v/10000.0f)*1998.0f); }
static uint32_t ui_d_ms(uint16_t v)  { return (uint32_t)(5.0f  + (v/10000.0f)*995.0f);  }
static uint32_t ui_s_pct(uint16_t v) { return v / 100u; }
static uint32_t ui_r_ms(uint16_t v)  { return (uint32_t)(10.0f + (v/10000.0f)*4990.0f); }

// ── Section draw functions ────────────────────────────────────

// ---- MENU (2×2 grid) ----------------------------------------
// Grid: two 64×80 cells. Top-left=0 WAVE, top-right=1 CALIB,
//                         bottom-left=2 FX, bottom-right=3 ADSR
static void ui_draw_menu(void)
{
    tft_fill(0, 0, TFT_W-1, TFT_H-1, C_BLACK);
    // Dividers
    tft_fill(63, 0, 64, TFT_H-1, C_DKGRAY);   // vertical
    tft_fill(0, 79, TFT_W-1, 80, C_DKGRAY);   // horizontal

    amy_engine_state_t st;
    amy_engine_get_state(&st);
    const char *wave_short =
        (st.wave_id == AMY_ENGINE_WAVE_SAW_DOWN) ? "SAW" :
        (st.wave_id == AMY_ENGINE_WAVE_TRIANGLE)  ? "TRI" : "SQR";

    const char *fx_flt =
        (st.filter_type == AMY_ENGINE_FILTER_BPF) ? "BPF" :
        (st.filter_type == AMY_ENGINE_FILTER_HPF) ? "HPF" : "LPF";
    const char *names[4]    = {"WAVE",  "CALIB", "FX",    "ADSR"};
    const char *statuses[4] = {wave_short, "PADS", fx_flt, "EDIT"};

    for (int i = 0; i < 4; i++) {
        int cx = (i % 2) * 64;
        int cy = (i / 2) * 80;
        bool sel = (i == s_menu_cur);
        uint16_t hbg = sel ? C_YELLOW : C_DKGRAY;
        uint16_t hfg = sel ? C_BLACK  : C_WHITE;
        // Header row (16px)
        tft_fill(cx, cy, cx+62, cy+17, hbg);
        tft_text(names[i], cx+2, cy+2, hfg, hbg, 2);
        // Status (lighter, below header)
        tft_text(statuses[i], cx+2, cy+22, C_LTGRAY, C_BLACK, 2);
    }
}

// ---- WAVEFORM -----------------------------------------------
static void ui_draw_waveform(void)
{
    tft_fill(0, 0, TFT_W-1, TFT_H-1, C_BLACK);
    tft_text("WAVEFORM", 2, 4, C_YELLOW, C_BLACK, 2);
    tft_fill(0, 22, TFT_W-1, 23, C_DKGRAY);

    for (int i = 0; i < 3; i++) {
        bool sel = (i == s_wave_cur);
        uint16_t fg = sel ? C_CYAN : C_LTGRAY;
        uint16_t bg = sel ? C_DKGRAY : C_BLACK;
        if (sel) tft_fill(0, 34 + i*24, TFT_W-1, 34 + i*24 + 19, C_DKGRAY);
        tft_text(s_wave_names[i], 8, 36 + i*24, fg, bg, 2);
    }

    tft_fill(0, 110, TFT_W-1, 111, C_DKGRAY);
    tft_text("BTN:SELECT", 2, 116, C_DKGRAY, C_BLACK, 2);
    tft_text("HOLD:BACK ", 2, 136, C_DKGRAY, C_BLACK, 2);
}

// ---- CALIBRATION --------------------------------------------
static void ui_draw_calib(void)
{
    tft_fill(0, 0, TFT_W-1, TFT_H-1, C_BLACK);

    char hdr[24];
    snprintf(hdr, sizeof(hdr), "PAD %u %s",
             (uint8_t)(s_cal_pad + 1), s_cal_editing ? "EDIT" : "    ");
    tft_text(hdr, 2, 4, s_cal_editing ? C_CYAN : C_YELLOW, C_BLACK, 2);
    tft_fill(0, 22, TFT_W-1, 23, C_DKGRAY);

    uint16_t raw  = touch_telemetry_get_raw((uint8_t)s_cal_pad);
    uint16_t base = touch_telemetry_get_baseline((uint8_t)s_cal_pad);
    uint16_t thr  = s_cal_editing ? s_cal_thr : touch_telemetry_get_threshold((uint8_t)s_cal_pad);

    char buf[16];
    snprintf(buf, sizeof(buf), "RAW  %5u", (unsigned)raw);
    tft_text(buf, 4, 30, C_WHITE, C_BLACK, 2);
    snprintf(buf, sizeof(buf), "BASE %5u", (unsigned)base);
    tft_text(buf, 4, 50, C_LTGRAY, C_BLACK, 2);
    snprintf(buf, sizeof(buf), "THR  %5u", (unsigned)thr);
    tft_text(buf, 4, 70, s_cal_editing ? C_YELLOW : C_GREEN, C_BLACK, 2);

    // Bar: raw progress toward threshold (full bar = at threshold)
    uint32_t ceil_val = (uint32_t)thr * 4;
    int bar_w = (ceil_val > 0 && raw < ceil_val)
                ? (int)(((uint32_t)raw * (TFT_W-8)) / ceil_val)
                : (TFT_W-8);
    tft_fill(4, 94, 4+bar_w, 106, C_GREEN);
    if (4+bar_w < TFT_W-4)
        tft_fill(4+bar_w, 94, TFT_W-5, 106, C_DKGRAY);
    // Threshold marker at 25% of bar (= threshold itself)
    int mx = 4 + (TFT_W-8)/4;
    if (mx < TFT_W-2) tft_fill(mx, 88, mx+1, 112, C_RED);

    tft_fill(0, 116, TFT_W-1, 117, C_DKGRAY);
    if (s_cal_editing) {
        tft_text("BTN:SAVE  ", 2, 122, C_DKGRAY, C_BLACK, 2);
        tft_text("HOLD:MENU ", 2, 142, C_DKGRAY, C_BLACK, 2);
    } else {
        tft_text("ROT:PADS  ", 2, 122, C_DKGRAY, C_BLACK, 2);
        tft_text("BTN:EDIT  ", 2, 142, C_DKGRAY, C_BLACK, 2);
    }
}

// ---- EFFECTS ------------------------------------------------
// 10 items, 4 visible at a time, scrolls via s_fx_scroll.
// item 0: filter type (short press cycles LPF/BPF/HPF)
// items 1-9: numeric 0-10000, short press toggles editing
static void ui_draw_effects(void)
{
    static const char * const s_flt_names[3] = {"LPF", "BPF", "HPF"};

    tft_fill(0, 0, TFT_W-1, TFT_H-1, C_BLACK);
    tft_text("FX", 2, 4, C_YELLOW, C_BLACK, 2);
    tft_fill(0, 22, TFT_W-1, 23, C_DKGRAY);

    for (int vi = 0; vi < 4; vi++) {
        int item = s_fx_scroll + vi;
        if (item >= 10) break;

        bool is_cur = (item == s_fx_cur);
        bool editing = is_cur && s_fx_editing;
        char cur = editing ? '#' : (is_cur ? '>' : ' ');

        char row[16];
        switch (item) {
            case 0:
                snprintf(row, sizeof(row), "%cFLT %s", cur, s_flt_names[s_fx_flt_type]);
                break;
            case 1: {
                uint32_t hz = (uint32_t)(200.0f + (s_fx_vals[0] / 10000.0f) * 9800.0f);
                snprintf(row, sizeof(row), "%cCUT%5u", cur, (unsigned)hz);
                break;
            }
            case 2: {
                uint32_t pct = (uint32_t)((s_fx_vals[1] / 10000.0f) * 90.0f);
                snprintf(row, sizeof(row), "%cRES %3u%%", cur, (unsigned)pct);
                break;
            }
            case 3:
                snprintf(row, sizeof(row), "%cRVB %3u%%", cur, (unsigned)(s_fx_vals[2] / 100));
                break;
            case 4:
                snprintf(row, sizeof(row), "%cRDC %3u%%", cur, (unsigned)(s_fx_vals[3] / 100));
                break;
            case 5:
                snprintf(row, sizeof(row), "%cECH %3u%%", cur, (unsigned)(s_fx_vals[4] / 100));
                break;
            case 6:
                snprintf(row, sizeof(row), "%cEFB %3u%%", cur, (unsigned)(s_fx_vals[5] / 100));
                break;
            case 7: {
                uint32_t ms = ((uint32_t)s_fx_vals[6] * 500) / 10000;
                snprintf(row, sizeof(row), "%cGLD %3uMS", cur, (unsigned)ms);
                break;
            }
            case 8: {
                uint32_t x10 = 1 + ((uint32_t)s_fx_vals[7] * 99) / 10000; // 1-100 (tenths of Hz)
                snprintf(row, sizeof(row), "%cLFR%u.%uHZ", cur,
                         (unsigned)(x10 / 10), (unsigned)(x10 % 10));
                break;
            }
            case 9: {
                uint32_t hz = ((uint32_t)s_fx_vals[8] * 5000) / 10000;
                snprintf(row, sizeof(row), "%cLFD%4uHZ", cur, (unsigned)hz);
                break;
            }
            default: row[0] = '\0'; break;
        }

        uint16_t fg = editing ? C_CYAN : (is_cur ? C_WHITE : C_LTGRAY);
        uint16_t bg = is_cur ? C_DKGRAY : C_BLACK;
        int y = 28 + vi * 24;
        if (is_cur) tft_fill(0, y - 2, TFT_W-1, y + 17, C_DKGRAY);
        tft_text(row, 4, y, fg, bg, 2);
    }

    tft_fill(0, 126, TFT_W-1, 127, C_DKGRAY);
    if (s_fx_editing) {
        tft_text("ROT:ADJUST", 2, 132, C_DKGRAY, C_BLACK, 2);
        tft_text("BTN:DONE  ", 2, 148, C_DKGRAY, C_BLACK, 2);
    } else if (s_fx_cur == 0) {
        tft_text("BTN:CYCLE ", 2, 132, C_DKGRAY, C_BLACK, 2);
        tft_text("HLD:BACK  ", 2, 148, C_DKGRAY, C_BLACK, 2);
    } else {
        tft_text("BTN:EDIT  ", 2, 132, C_DKGRAY, C_BLACK, 2);
        tft_text("HLD:BACK  ", 2, 148, C_DKGRAY, C_BLACK, 2);
    }
}

// ---- ADSR ---------------------------------------------------
static void ui_draw_adsr(void)
{
    tft_fill(0, 0, TFT_W-1, TFT_H-1, C_BLACK);
    tft_text("ADSR", 2, 4, C_YELLOW, C_BLACK, 2);
    tft_fill(0, 22, TFT_W-1, 23, C_DKGRAY);

    // 4 param rows: A at y=30, D at y=54, S at y=78, R at y=102
    typedef uint32_t (*conv_fn)(uint16_t);
    const conv_fn conv[4]  = { ui_a_ms, ui_d_ms, ui_s_pct, ui_r_ms };
    const char * const units[4] = { "MS", "MS", "% ", "MS" };
    const char * const lbls[4]  = { "A", "D", "S", "R" };

    for (int i = 0; i < 4; i++) {
        bool is_cur  = (i == s_adsr_cur);
        bool editing = is_cur && s_adsr_editing;
        char cursor  = editing ? '#' : (is_cur ? '>' : ' ');
        uint32_t val = conv[i](s_adsr[i]);

        char row[16];
        if (i == 2) { // sustain → %
            snprintf(row, sizeof(row), "%c%s: %3" PRIu32 "%s",
                     cursor, lbls[i], val, units[i]);
        } else {
            snprintf(row, sizeof(row), "%c%s:%4" PRIu32 "%s",
                     cursor, lbls[i], val, units[i]);
        }

        uint16_t fg = editing ? C_CYAN : (is_cur ? C_WHITE : C_LTGRAY);
        uint16_t bg = is_cur ? C_DKGRAY : C_BLACK;
        if (is_cur) tft_fill(0, 28+i*24, TFT_W-1, 28+i*24+19, C_DKGRAY);
        tft_text(row, 4, 30+i*24, fg, bg, 2);
    }

    tft_fill(0, 126, TFT_W-1, 127, C_DKGRAY);
    if (s_adsr_editing) {
        tft_text("ROT:ADJUST", 2, 132, C_DKGRAY, C_BLACK, 2);
        tft_text("BTN:DONE  ", 2, 148, C_DKGRAY, C_BLACK, 2);
    } else {
        tft_text("ROT:MOVE  ", 2, 132, C_DKGRAY, C_BLACK, 2);
        tft_text("BTN:EDIT  ", 2, 148, C_DKGRAY, C_BLACK, 2);
    }
}

static void ui_redraw(void)
{
    switch (s_sec) {
        case UI_MENU:     ui_draw_menu();     break;
        case UI_WAVEFORM: ui_draw_waveform(); break;
        case UI_CALIB:    ui_draw_calib();    break;
        case UI_EFFECTS:  ui_draw_effects();  break;
        case UI_ADSR:     ui_draw_adsr();     break;
    }
}

// ── Section enter helpers ─────────────────────────────────────
static void ui_enter(ui_section_t sec)
{
    s_sec = sec;
    s_dirty = true;

    if (sec == UI_WAVEFORM) {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        s_wave_cur = 0;
        for (int i = 0; i < 3; i++)
            if (s_wave_ids[i] == st.wave_id) { s_wave_cur = i; break; }
    }
    if (sec == UI_ADSR) {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        s_adsr[0] = st.env_attack;
        s_adsr[1] = st.env_decay;
        s_adsr[2] = st.env_sustain;
        s_adsr[3] = st.env_release;
        s_adsr_cur     = 0;
        s_adsr_editing = false;
    }
    if (sec == UI_CALIB) {
        s_cal_editing = false;
    }
    if (sec == UI_EFFECTS) {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        s_fx_flt_type = st.filter_type;
        s_fx_vals[0]  = st.filter_cutoff;
        s_fx_vals[1]  = st.filter_resonance;
        s_fx_vals[2]  = st.reverb_amount;
        s_fx_vals[3]  = st.reverb_decay;
        s_fx_vals[4]  = st.echo_amount;
        s_fx_vals[5]  = st.echo_feedback;
        s_fx_vals[6]  = st.glide;
        s_fx_vals[7]  = st.lfo_rate;
        s_fx_vals[8]  = st.lfo_depth;
        s_fx_cur      = 0;
        s_fx_scroll   = 0;
        s_fx_editing  = false;
    }
}

static void ui_exit_to_menu(void)
{
    if (s_sec == UI_CALIB && s_cal_editing) {
        touch_telemetry_set_threshold((uint8_t)s_cal_pad, s_cal_thr);
        s_cal_editing = false;
    }
    if (s_sec == UI_EFFECTS || s_sec == UI_ADSR) {
        amy_engine_save_state();
    }
    ui_enter(UI_MENU);
}

// ── Public API ────────────────────────────────────────────────

void ui_init(void)
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
    tft_init();
    ui_redraw();
}

// Called from main loop every ~20 ms.
// delta: encoder steps since last call (signed).
// short_press / long_press: button event flags (at most one true per call).
void ui_tick(int delta, bool short_press, bool long_press)
{
    bool changed = (delta != 0) || short_press || long_press;

    switch (s_sec) {

    case UI_MENU:
        if (delta) {
            s_menu_cur = (s_menu_cur + delta % 4 + 4) % 4;
            s_dirty = true;
        }
        if (short_press) {
            switch (s_menu_cur) {
                case 0: ui_enter(UI_WAVEFORM); break;
                case 1: ui_enter(UI_CALIB);    break;
                case 2: ui_enter(UI_EFFECTS);  break;
                case 3: ui_enter(UI_ADSR);     break;
            }
        }
        break;

    case UI_WAVEFORM:
        if (delta) {
            s_wave_cur += delta;
            if (s_wave_cur < 0) s_wave_cur = 0;
            if (s_wave_cur > 2) s_wave_cur = 2;
            s_dirty = true;
        }
        if (short_press) {
            amy_engine_set_wave(s_wave_ids[s_wave_cur]);
            amy_engine_save_state();
            ui_exit_to_menu();
        }
        if (long_press) ui_exit_to_menu();
        break;

    case UI_CALIB:
        if (!s_cal_editing) {
            if (delta) {
                s_cal_pad = (s_cal_pad + delta % 8 + 8) % 8;
                s_dirty = true;
            }
            if (short_press) {
                s_cal_thr     = touch_telemetry_get_threshold((uint8_t)s_cal_pad);
                s_cal_editing = true;
                s_dirty       = true;
            }
            if (long_press) ui_exit_to_menu();
        } else {
            if (delta) {
                int t = (int)s_cal_thr + delta * 50;
                if (t < 10) t = 10;
                s_cal_thr = (uint16_t)t;
                s_dirty   = true;
            }
            if (short_press) {
                touch_telemetry_set_threshold((uint8_t)s_cal_pad, s_cal_thr);
                s_cal_editing = false;
                s_dirty       = true;
            }
            if (long_press) {
                touch_telemetry_set_threshold((uint8_t)s_cal_pad, s_cal_thr);
                ui_exit_to_menu();
            }
        }
        break;

    case UI_EFFECTS:
        if (!s_fx_editing) {
            if (delta) {
                s_fx_cur += delta;
                if (s_fx_cur < 0) s_fx_cur = 0;
                if (s_fx_cur > 9) s_fx_cur = 9;
                if (s_fx_cur < s_fx_scroll)       s_fx_scroll = s_fx_cur;
                if (s_fx_cur >= s_fx_scroll + 4)  s_fx_scroll = s_fx_cur - 3;
                s_dirty = true;
            }
            if (short_press) {
                if (s_fx_cur == 0) {
                    s_fx_flt_type = (s_fx_flt_type + 1) % 3;
                    // Per-type cutoff defaults tuned for C4-E5 playing range (262-659 Hz).
                    static const uint16_t s_flt_cutoff_dflt[3] = {7959, 1122, 51};
                    s_fx_vals[0] = s_flt_cutoff_dflt[s_fx_flt_type];
                    amy_engine_set_filter_type(s_fx_flt_type);
                    amy_engine_set_filter(s_fx_vals[0], s_fx_vals[1]);
                    s_dirty = true;
                } else {
                    s_fx_editing = true;
                    s_dirty = true;
                }
            }
        } else {
            if (delta) {
                int v = (int)s_fx_vals[s_fx_cur - 1] + delta * 500;
                if (v < 0)     v = 0;
                if (v > 10000) v = 10000;
                s_fx_vals[s_fx_cur - 1] = (uint16_t)v;
                switch (s_fx_cur) {
                    case 1: case 2:
                        amy_engine_set_filter(s_fx_vals[0], s_fx_vals[1]); break;
                    case 3: case 4:
                        amy_engine_set_reverb(s_fx_vals[2], s_fx_vals[3]); break;
                    case 5: case 6:
                        amy_engine_set_echo(s_fx_vals[4], s_fx_vals[5]);   break;
                    case 7:
                        amy_engine_set_glide(s_fx_vals[6]); break;
                    case 8: case 9:
                        amy_engine_set_lfo(s_fx_vals[7], s_fx_vals[8]); break;
                }
                s_dirty = true;
            }
            if (short_press) { s_fx_editing = false; s_dirty = true; }
        }
        if (long_press) ui_exit_to_menu();
        break;

    case UI_ADSR:
        if (!s_adsr_editing) {
            if (delta) {
                s_adsr_cur += delta;
                if (s_adsr_cur < 0) s_adsr_cur = 0;
                if (s_adsr_cur > 3) s_adsr_cur = 3;
                s_dirty = true;
            }
            if (short_press) { s_adsr_editing = true; s_dirty = true; }
        } else {
            if (delta) {
                int v = (int)s_adsr[s_adsr_cur] + delta * 500;
                if (v < 0)     v = 0;
                if (v > 10000) v = 10000;
                s_adsr[s_adsr_cur] = (uint16_t)v;
                amy_engine_set_adsr(s_adsr[0], s_adsr[1], s_adsr[2], s_adsr[3]);
                s_dirty = true;
            }
            if (short_press) { s_adsr_editing = false; s_dirty = true; }
        }
        if (long_press) ui_exit_to_menu();
        break;
    }

    // Timed refresh for live data (calibration raw values change without user input)
    static TickType_t last_refresh = 0;
    TickType_t now = xTaskGetTickCount();
    uint32_t interval = (s_sec == UI_CALIB) ? 120 : 500;
    if (!changed && (now - last_refresh) >= pdMS_TO_TICKS(interval)) {
        s_dirty      = true;
        last_refresh = now;
    }
    if (s_dirty || changed) {
        last_refresh = now;
        s_dirty = false;
        ui_redraw();
    }
}
