#pragma once
// UI module — include exactly once from main.c

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "touch_telemetry.h"
#include "amy_engine.h"

// ── TFT hardware pins ────────────────────────────────────────
#define TFT_SCLK  GPIO_NUM_36
#define TFT_MOSI  GPIO_NUM_35
#define TFT_CS    GPIO_NUM_37
#define TFT_DC    GPIO_NUM_45
#define TFT_RST   GPIO_NUM_21
#define TFT_W     160
#define TFT_H     128

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

static void tft_line(int x0, int y0, int x1, int y1, uint16_t c)
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
static void tft_pad_circle(int cx, int cy, bool active)
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
typedef enum { UI_CARDS, UI_MAIN, UI_CALIB, UI_EFFECTS, UI_ADSR, UI_PRESETS } ui_section_t;

static ui_section_t s_sec      = UI_CARDS;
static int          s_card_cur = 0;   // 0=MAIN 1=PRST 2=CAL
static bool         s_dirty    = true;

// Ribbon state (set by on_touch / octave buttons)
static int  s_octave_shift = 0;
static bool s_pad_active[TOUCH_NOTE_PADS] = {false};

// MAIN card columns: ENV FLT LFO ECH GLD
static int      s_main_col     = 0;
static bool     s_main_editing = false;
static int      s_main_sub     = 0;   // sub-param within column
static uint16_t s_main_adsr[4];       // 0=A 1=D 2=S 3=R  (0..10000)
static uint16_t s_main_flt_cut;
static uint16_t s_main_flt_res;
static uint8_t  s_main_flt_type;      // 0=LPF 1=BPF 2=HPF
static uint16_t s_main_lfo_rate;
static uint16_t s_main_lfo_depth;
static uint16_t s_main_ech_amt;
static uint16_t s_main_ech_fb;
static uint16_t s_main_gld;

// sub-param counts per column
static const int s_main_sub_max[5] = {4, 3, 2, 2, 1};

static inline uint16_t u16clamp(int v) { return (uint16_t)(v < 0 ? 0 : v > 10000 ? 10000 : v); }

// ── Presets ──────────────────────────────────────────────────
#define PRESET_COUNT  6
#define PRESET_MAGIC  0xA5u
#define PRESET_NVS_NS "pst"

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  wave_id;
    uint8_t  flt_type;
    uint8_t  _pad;
    uint16_t flt_cut;
    uint16_t flt_res;
    uint16_t env_atk;
    uint16_t env_dec;
    uint16_t env_sus;
    uint16_t env_rel;
    uint16_t lfo_rt;
    uint16_t lfo_dp;
    uint16_t ech_amt;
    uint16_t ech_fb;
    uint16_t gld;
} preset_t;

static preset_t s_presets[PRESET_COUNT];
static bool     s_preset_valid[PRESET_COUNT];

static int  s_prst_cur    = 0;     // 0..5 currently highlighted slot
static bool s_prst_action = false; // in action sub-menu
static int  s_prst_act    = 0;     // 0=LOAD 1=SAVE 2=CLR

// Calibration section
static int      s_cal_pad     = 0;
static bool     s_cal_editing = false;
static uint16_t s_cal_thr     = 100;

// Effects section
// items 0-7: FltType CUT RES ECH EFB GLD LFRT LFDP
static int      s_fx_cur      = 0;
static int      s_fx_scroll   = 0;
static bool     s_fx_editing  = false;
static uint8_t  s_fx_flt_type = 0;       // 0=LPF 1=BPF 2=HPF
static uint16_t s_fx_vals[7];            // [0]=cut [1]=res [2]=ech [3]=efb [4]=gld [5]=lfr [6]=lfd

// ADSR section (0-10000 internal scale)
static int      s_adsr_cur     = 0;  // 0=A 1=D 2=S 3=R
static bool     s_adsr_editing = false;
static uint16_t s_adsr[4];           // loaded from engine on enter

// ── ADSR display conversion ───────────────────────────────────
static uint32_t ui_a_ms(uint16_t v)  { return (uint32_t)(2.0f  + (v/10000.0f)*1998.0f); }
static uint32_t ui_d_ms(uint16_t v)  { return (uint32_t)(5.0f  + (v/10000.0f)*995.0f);  }
static uint32_t ui_s_pct(uint16_t v) { return v / 100u; }
static uint32_t ui_r_ms(uint16_t v)  { return (uint32_t)(10.0f + (v/10000.0f)*4990.0f); }

// ── Preset helpers ────────────────────────────────────────────

static void preset_nvs_load(int slot)
{
    char key[4];
    snprintf(key, sizeof(key), "p%d", slot);
    nvs_handle_t h;
    if (nvs_open(PRESET_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        s_preset_valid[slot] = false;
        return;
    }
    size_t sz = sizeof(preset_t);
    esp_err_t err = nvs_get_blob(h, key, &s_presets[slot], &sz);
    nvs_close(h);
    s_preset_valid[slot] = (err == ESP_OK && sz == sizeof(preset_t)
                            && s_presets[slot].magic == PRESET_MAGIC);
}

static void preset_nvs_save(int slot)
{
    char key[4];
    snprintf(key, sizeof(key), "p%d", slot);
    nvs_handle_t h;
    if (nvs_open(PRESET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, &s_presets[slot], sizeof(preset_t));
    nvs_commit(h);
    nvs_close(h);
}

static void preset_nvs_clear(int slot)
{
    char key[4];
    snprintf(key, sizeof(key), "p%d", slot);
    nvs_handle_t h;
    if (nvs_open(PRESET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
    s_preset_valid[slot] = false;
    memset(&s_presets[slot], 0, sizeof(preset_t));
}

static void preset_capture(int slot)
{
    amy_engine_state_t st;
    amy_engine_get_state(&st);
    preset_t *p = &s_presets[slot];
    p->magic    = PRESET_MAGIC;
    p->wave_id  = st.wave_id;
    p->flt_type = st.filter_type;
    p->_pad     = 0;
    p->flt_cut  = st.filter_cutoff;
    p->flt_res  = st.filter_resonance;
    p->env_atk  = st.env_attack;
    p->env_dec  = st.env_decay;
    p->env_sus  = st.env_sustain;
    p->env_rel  = st.env_release;
    p->lfo_rt   = st.lfo_rate;
    p->lfo_dp   = st.lfo_depth;
    p->ech_amt  = st.echo_amount;
    p->ech_fb   = st.echo_feedback;
    p->gld      = st.glide;
    s_preset_valid[slot] = true;
}

static void preset_apply(int slot)
{
    if (!s_preset_valid[slot]) return;
    preset_t *p = &s_presets[slot];
    amy_engine_set_wave(p->wave_id);
    amy_engine_set_filter_type(p->flt_type);
    amy_engine_set_filter(p->flt_cut, p->flt_res);
    amy_engine_set_adsr(p->env_atk, p->env_dec, p->env_sus, p->env_rel);
    amy_engine_set_lfo(p->lfo_rt, p->lfo_dp);
    amy_engine_set_echo(p->ech_amt, p->ech_fb);
    amy_engine_set_glide(p->gld);
    amy_engine_save_state();
}

// ── Section draw functions ────────────────────────────────────

// Draws a waveform pixel-art glyph inside box (x0,y0)-(x1,y1).
// Reusable for both the menu cell and the future ribbon bar.
static void ui_draw_wave_glyph(uint8_t wave_id, int x0, int y0, int x1, int y1, uint16_t fg)
{
    int xm = (x0 + x1) / 2;
    int lw = (x1 - x0) / 4; // left/right flat width for square
    switch (wave_id) {
    case AMY_ENGINE_WAVE_SQUARE:
        tft_line(x0,    y1, x0+lw, y1, fg); // left base
        tft_line(x0+lw, y0, x0+lw, y1, fg); // rise
        tft_line(x0+lw, y0, x1-lw, y0, fg); // top
        tft_line(x1-lw, y0, x1-lw, y1, fg); // fall
        tft_line(x1-lw, y1, x1,    y1, fg); // right base
        break;
    case AMY_ENGINE_WAVE_SAW_DOWN:
        tft_line(x0,    y1, x1-lw, y0, fg); // rising diagonal
        tft_line(x1-lw, y0, x1-lw, y1, fg); // instant drop
        tft_line(x1-lw, y1, x1,    y1, fg); // right base
        break;
    case AMY_ENGINE_WAVE_TRIANGLE:
        tft_line(x0, y1, xm, y0, fg); // rising half
        tft_line(xm, y0, x1, y1, fg); // falling half
        break;
    default:
        tft_line(x0, (y0+y1)/2, x1, (y0+y1)/2, fg);
        break;
    }
}

// ---- RIBBON BAR (y=0..21, always drawn last) ----------------
// 8 pad circles + waveform glyph + octave label + separator.
static void ui_draw_ribbon(void)
{
    tft_fill(0, 0, TFT_W-1, 19, C_BLACK);

    // Pad circles: centers at x=6+i*10, y=10 — radius 3, spaced for 160px
    for (int i = 0; i < TOUCH_NOTE_PADS; i++)
        tft_pad_circle(6 + i * 10, 10, s_pad_active[i]);

    // Waveform glyph (x=90..110, y=3..17)
    amy_engine_state_t st;
    amy_engine_get_state(&st);
    ui_draw_wave_glyph(st.wave_id, 90, 3, 110, 17, C_CYAN);

    // Octave label (x=116, y=6, scale 1)
    char oct_buf[8];
    if (s_octave_shift == 0)
        snprintf(oct_buf, sizeof(oct_buf), "OCT 0");
    else
        snprintf(oct_buf, sizeof(oct_buf), "OCT%+d", s_octave_shift);
    tft_text(oct_buf, 116, 6, C_YELLOW, C_BLACK, 1);

    // Separator
    tft_fill(0, 20, TFT_W-1, 21, C_DKGRAY);
}

// ---- CARDS (top-level navigation) ---------------------------
// 3 horizontal cards: MAIN (x=0..41) | PRST (x=43..84) | CAL (x=86..127)
static void ui_draw_cards(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    // Vertical card dividers (3 cards × 53px + 2 gaps = 160px)
    tft_fill(53, 22, 53, TFT_H-1, C_DKGRAY);
    tft_fill(107, 22, 107, TFT_H-1, C_DKGRAY);

    static const char * const names[3] = {"MAIN", "PRST", "CAL"};
    static const char * const subs[3]  = {"PARAMS", "SAVE", "PADS"};
    static const int          xs[3]    = {0, 54, 108};
    static const int          ws[3]    = {53, 53, 52};

    for (int i = 0; i < 3; i++) {
        bool     sel = (i == s_card_cur);
        uint16_t hbg = sel ? C_YELLOW : C_DKGRAY;
        uint16_t hfg = sel ? C_BLACK  : C_WHITE;
        int cx = xs[i], cw = ws[i];
        int nlen = (int)strlen(names[i]);
        int tx   = cx + (cw - nlen * 6) / 2;

        // Header (y=22..35)
        tft_fill(cx, 22, cx + cw - 1, 35, hbg);
        tft_text(names[i], tx, 26, hfg, hbg, 1);

        // Sub-label (center of body)
        int slen = (int)strlen(subs[i]);
        int stx  = cx + (cw - slen * 6) / 2;
        tft_text(subs[i], stx, 60, sel ? C_WHITE : C_LTGRAY, C_BLACK, 1);
    }
}

// ---- PRESETS — 2×3 slot grid --------------------------------
// Columns: same as cards (x=0..52, 54..106, 108..159).
// Rows: y=22..73 (row 0), y=75..127 (row 1), divider y=74.
// Each cell: 10px header + 42/43px body.
static void ui_draw_presets(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    tft_fill(53,  22, 53,  TFT_H-1, C_DKGRAY); // vertical dividers
    tft_fill(107, 22, 107, TFT_H-1, C_DKGRAY);
    tft_fill(0,   74, TFT_W-1, 74,  C_DKGRAY); // horizontal divider

    static const int cxs[3] = {0, 54, 108};
    static const int cws[3] = {53, 53, 52};
    static const int rys[2] = {22, 75};
    static const char * const wn[7] = {"SI", "PU", "SA", "SU", "TR", "SQ", "KS"};
    static const char * const act_lbl[3] = {"LOAD", "SAVE", "CLR"};

    for (int i = 0; i < PRESET_COUNT; i++) {
        int row = i / 3, col = i % 3;
        int cx = cxs[col], cw = cws[col];
        int ry = rys[row];
        bool sel = (i == s_prst_cur);

        // Header strip (10px)
        uint16_t hbg = sel ? C_YELLOW : C_DKGRAY;
        uint16_t hfg = sel ? C_BLACK  : C_WHITE;
        tft_fill(cx, ry, cx + cw - 1, ry + 9, hbg);
        char hdr[4]; snprintf(hdr, sizeof(hdr), "P%d", i + 1);
        tft_text(hdr, cx + 2, ry + 1, hfg, hbg, 1);
        // Dot in header if slot has data
        if (s_preset_valid[i]) {
            int dx = cx + cw - 5;
            tft_fill((uint8_t)dx, (uint8_t)(ry + 3), (uint8_t)(dx + 2), (uint8_t)(ry + 5), hfg);
        }

        // Body
        int body_top = ry + 10;
        int body_bot = (row == 0) ? ry + 42 : TFT_H - 1;
        tft_fill(cx, body_top, cx + cw - 1, body_bot, C_BLACK);

        if (s_prst_action && sel) {
            // Action overlay: LOAD / SAVE / CLR
            for (int a = 0; a < 3; a++) {
                bool avail = (a == 1) || s_preset_valid[i]; // SAVE always available
                bool asel  = (a == s_prst_act);
                uint16_t bg = (asel && avail) ? C_DKGRAY : C_BLACK;
                uint16_t fg = !avail ? C_DKGRAY : (asel ? C_YELLOW : C_LTGRAY);
                int ay = body_top + 4 + a * 12;
                if (asel && avail)
                    tft_fill(cx, (uint8_t)ay, cx + cw - 1, (uint8_t)(ay + 8), C_DKGRAY);
                int llen = (int)strlen(act_lbl[a]);
                tft_text(act_lbl[a], cx + (cw - llen * 6) / 2, ay, fg, bg, 1);
            }
        } else if (s_preset_valid[i]) {
            preset_t *p = &s_presets[i];
            uint16_t fc = sel ? C_CYAN : C_LTGRAY;

            // Mini wave glyph (20×12, centered)
            int gx0 = cx + (cw - 20) / 2;
            ui_draw_wave_glyph(p->wave_id, gx0, body_top + 2, gx0 + 19, body_top + 13, fc);

            // Wave name (2 chars)
            const char *wname = (p->wave_id < 7) ? wn[p->wave_id] : "??";
            tft_text(wname, cx + 2, body_top + 16, fc, C_BLACK, 1);

            // Filter cutoff summary
            float fc_hz = fminf(13.0f * powf(2.0f, 0.0938f * ((p->flt_cut / 10000.0f) * 127.0f)), 12000.0f);
            char fstr[6];
            snprintf(fstr, sizeof(fstr), fc_hz >= 1000.0f ? "F%uK" : "F%u",
                     fc_hz >= 1000.0f ? (unsigned)(fc_hz / 1000.0f + 0.5f) : (unsigned)fc_hz);
            tft_text(fstr, cx + 2, body_top + 26, fc, C_BLACK, 1);

            // Attack summary
            uint32_t atk = (uint32_t)(2.0f + (p->env_atk / 10000.0f) * 1998.0f);
            char astr[6];
            snprintf(astr, sizeof(astr), atk >= 1000 ? "A%uK" : "A%u",
                     (unsigned)(atk >= 1000 ? atk / 1000 : atk));
            tft_text(astr, cx + 26, body_top + 26, fc, C_BLACK, 1);
        } else {
            // Empty slot
            tft_text("----", cx + (cw - 4 * 6) / 2, body_top + 16, C_DKGRAY, C_BLACK, 1);
        }
    }
}

// ---- MAIN card — 5 Spark-style columns ----------------------

// Filled vertical bar: x0..x1 wide, y_top..y_bot tall, filled from bottom by val (0..10000).
static void ui_vbar(int x0, int x1, int y_top, int y_bot, uint16_t val, uint16_t col)
{
    int h   = y_bot - y_top;
    int fh  = (int)((long)val * h / 10000);
    int fy  = y_bot - fh;
    tft_fill((uint8_t)x0, (uint8_t)y_top, (uint8_t)x1, (uint8_t)(fy > y_top ? fy-1 : y_top), C_DKGRAY);
    if (fh > 0)
        tft_fill((uint8_t)x0, (uint8_t)fy, (uint8_t)x1, (uint8_t)y_bot, col);
}

// ENV column: ADSR envelope shape + value text.
// col_x=0..30.  bar_top/bot = drawing area vertical bounds.
static void ui_draw_col_env(int cx, bool editing)
{
    int x0 = cx + 2, bot = 100, top = 36;
    int h = bot - top;

    int A_w = s_main_adsr[0] * 7 / 10000; if (A_w < 1) A_w = 1;
    int D_w = s_main_adsr[1] * 7 / 10000; if (D_w < 1) D_w = 1;
    int S_w = 4;
    int R_w = s_main_adsr[3] * 7 / 10000; if (R_w < 1) R_w = 1;
    int sust_y = bot - (int)((long)s_main_adsr[2] * h / 10000);
    if (sust_y < top + 2) sust_y = top + 2;

    // Each segment: CYAN if selected-and-editing, dim if editing-but-other, else green
    #define ECOL(sub) (editing ? (s_main_sub == (sub) ? C_CYAN : C_DKGRAY) : C_GREEN)
    int xa = x0 + A_w, xd = xa + D_w, xs = xd + S_w, xr = xs + R_w;
    tft_line(x0, bot, xa,  top,   ECOL(0));
    tft_line(xa, top, xd,  sust_y, ECOL(1));
    tft_line(xd, sust_y, xs, sust_y, ECOL(2));
    tft_line(xs, sust_y, xr, bot,  ECOL(3));
    #undef ECOL

    // Value label under shape
    static const char * const lbl[4] = {"A", "D", "S", "R"};
    int vi = editing ? s_main_sub : 0;
    uint32_t val;
    switch (vi) {
        case 0: val = ui_a_ms(s_main_adsr[0]); break;
        case 1: val = ui_d_ms(s_main_adsr[1]); break;
        case 2: val = ui_s_pct(s_main_adsr[2]); break;
        default: val = ui_r_ms(s_main_adsr[3]); break;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%s:%-4" PRIu32, lbl[vi], val);
    tft_text(buf, cx + 1, 104, editing ? C_CYAN : C_LTGRAY, C_BLACK, 1);
}

// Two-bar column (for FLT, LFO, ECH).
static void ui_draw_two_bars(int cx, int cw,
                              uint16_t v0, uint16_t v1,
                              const char *l0, const char *l1,
                              int sub0, int sub1, bool editing, int cur_sub)
{
    int bar_top = 44, bar_bot = 116;
    int bw = 8, gap = 3;
    int bx0 = cx + (cw - (bw + gap + bw)) / 2;
    int bx1 = bx0 + bw + gap;

    uint16_t c0 = editing ? (cur_sub == sub0 ? C_CYAN : C_DKGRAY) : C_GREEN;
    uint16_t c1 = editing ? (cur_sub == sub1 ? C_CYAN : C_DKGRAY) : C_GREEN;
    ui_vbar(bx0, bx0 + bw - 1, bar_top, bar_bot, v0, c0);
    ui_vbar(bx1, bx1 + bw - 1, bar_top, bar_bot, v1, c1);
    tft_text(l0, bx0 + 1, bar_bot + 2, c0, C_BLACK, 1);
    tft_text(l1, bx1 + 1, bar_bot + 2, c1, C_BLACK, 1);
}

// Single-bar column (for GLD).
static void ui_draw_one_bar(int cx, int cw, uint16_t val, const char *lbl, bool editing)
{
    int bar_top = 44, bar_bot = 116;
    int bw = 8;
    int bx = cx + (cw - bw) / 2;
    uint16_t col = editing ? C_CYAN : C_GREEN;
    ui_vbar(bx, bx + bw - 1, bar_top, bar_bot, val, col);
    tft_text(lbl, bx + 1, bar_bot + 2, col, C_BLACK, 1);
}

static void ui_draw_main(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    // Column positions: 5 cols each 31px + 1px dividers
    static const int cxs[5] = {0, 32, 64, 96, 128};
    static const int cws[5] = {31, 31, 31, 31, 32};
    static const char * const hdr[5] = {"ENV", "FLT", "LFO", "ECH", "GLD"};
    static const char * const flt_type_lbl[3] = {"LPF", "BPF", "HPF"};

    // Dividers
    tft_fill(31, 22, 31, TFT_H-1, C_DKGRAY);
    tft_fill(63, 22, 63, TFT_H-1, C_DKGRAY);
    tft_fill(95, 22, 95, TFT_H-1, C_DKGRAY);
    tft_fill(127, 22, 127, TFT_H-1, C_DKGRAY);

    for (int c = 0; c < 5; c++) {
        bool is_col = (c == s_main_col);
        bool editing = is_col && s_main_editing;
        uint16_t hbg = is_col ? C_YELLOW : C_DKGRAY;
        uint16_t hfg = is_col ? C_BLACK  : C_WHITE;
        int cx = cxs[c], cw = cws[c];

        // Column header y=22..31
        tft_fill(cx, 22, cx + cw - 1, 31, hbg);
        int tx = cx + (cw - 3 * 6) / 2;
        tft_text(hdr[c], tx, 23, hfg, hbg, 1);

        // Column body y=32..127
        switch (c) {
        case 0: // ENV
            ui_draw_col_env(cx, editing);
            break;
        case 1: { // FLT
            // Filter type label (sub=2 = TYPE)
            bool type_sel = editing && s_main_sub == 2;
            uint16_t tc = type_sel ? C_CYAN : (editing ? C_DKGRAY : C_LTGRAY);
            tft_text(flt_type_lbl[s_main_flt_type], cx + 2, 33, tc, C_BLACK, 1);
            ui_draw_two_bars(cx, cw, s_main_flt_cut, s_main_flt_res, "F", "Q", 0, 1, editing, s_main_sub);
            break;
        }
        case 2: // LFO
            ui_draw_two_bars(cx, cw, s_main_lfo_rate, s_main_lfo_depth, "R", "D", 0, 1, editing, s_main_sub);
            break;
        case 3: // ECH
            ui_draw_two_bars(cx, cw, s_main_ech_amt, s_main_ech_fb, "A", "F", 0, 1, editing, s_main_sub);
            break;
        case 4: // GLD
            ui_draw_one_bar(cx, cw, s_main_gld, "G", editing);
            break;
        }
    }
}

// ---- CALIBRATION --------------------------------------------
static void ui_draw_calib(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    // Pad label (in ribbon-adjacent area, y=24)
    char hdr[12];
    if (s_cal_pad < TOUCH_NOTE_PADS)
        snprintf(hdr, sizeof(hdr), "PAD %u", (uint8_t)(s_cal_pad + 1));
    else
        snprintf(hdr, sizeof(hdr), s_cal_pad == TOUCH_NOTE_PADS ? "OCT-" : "OCT+");
    tft_text(hdr, 4, 24, s_cal_editing ? C_CYAN : C_YELLOW, C_BLACK, 2);

    uint16_t raw  = touch_telemetry_get_raw((uint8_t)s_cal_pad);
    uint16_t base = touch_telemetry_get_baseline((uint8_t)s_cal_pad);
    uint16_t thr  = s_cal_editing ? s_cal_thr : touch_telemetry_get_threshold((uint8_t)s_cal_pad);

    char buf[16];
    snprintf(buf, sizeof(buf), "RAW  %5u", (unsigned)raw);
    tft_text(buf, 4, 44, C_WHITE, C_BLACK, 2);
    snprintf(buf, sizeof(buf), "BASE %5u", (unsigned)base);
    tft_text(buf, 4, 60, C_LTGRAY, C_BLACK, 2);
    snprintf(buf, sizeof(buf), "THR  %5u", (unsigned)thr);
    tft_text(buf, 4, 76, s_cal_editing ? C_YELLOW : C_GREEN, C_BLACK, 2);

    // Bar: raw progress toward threshold
    uint32_t ceil_val = (uint32_t)thr * 4;
    int bar_w = (ceil_val > 0 && raw < ceil_val)
                ? (int)(((uint32_t)raw * (TFT_W-8)) / ceil_val)
                : (TFT_W-8);
    tft_fill(4, 94, 4+bar_w, 104, C_GREEN);
    if (4+bar_w < TFT_W-4)
        tft_fill(4+bar_w, 94, TFT_W-5, 104, C_DKGRAY);
    int mx = 4 + (TFT_W-8)/4;
    if (mx < TFT_W-2) tft_fill(mx, 89, mx+1, 109, C_RED);

    tft_fill(0, 112, TFT_W-1, 113, C_DKGRAY);
    if (s_cal_editing)
        tft_text("BTN:SAVE  HOLD:BACK", 2, 116, C_DKGRAY, C_BLACK, 1);
    else
        tft_text("ROT:PADS  BTN:EDIT ", 2, 116, C_DKGRAY, C_BLACK, 1);
}

// ---- EFFECTS (legacy — now superseded by UI_MAIN columns) ---
static void ui_draw_effects(void)
{
    static const char * const s_flt_names[3] = {"LPF", "BPF", "HPF"};
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    for (int vi = 0; vi < 4; vi++) {
        int item = s_fx_scroll + vi;
        if (item >= 8) break;

        bool is_cur = (item == s_fx_cur);
        bool editing = is_cur && s_fx_editing;
        char cur = editing ? '#' : (is_cur ? '>' : ' ');

        char row[16];
        switch (item) {
            case 0:
                snprintf(row, sizeof(row), "%cFLT %s", cur, s_flt_names[s_fx_flt_type]);
                break;
            case 1: {
                float t = s_fx_vals[0] / 10000.0f;
                uint32_t hz = (uint32_t)fminf(13.0f * powf(2.0f, 0.0938f * (t * 127.0f)), 12000.0f);
                snprintf(row, sizeof(row), "%cCUT%5u", cur, (unsigned)hz);
                break;
            }
            case 2: {
                float q = 0.7f * powf(2.0f, 4.0f * (s_fx_vals[1] / 10000.0f));
                uint32_t qi = (uint32_t)(q * 10.0f);
                snprintf(row, sizeof(row), "%cQ %2u.%u ", cur, (unsigned)(qi/10), (unsigned)(qi%10));
                break;
            }
            case 3:
                snprintf(row, sizeof(row), "%cECH %3u%%", cur, (unsigned)(s_fx_vals[2] / 100));
                break;
            case 4:
                snprintf(row, sizeof(row), "%cEFB %3u%%", cur, (unsigned)(s_fx_vals[3] / 100));
                break;
            case 5: {
                uint32_t ms = ((uint32_t)s_fx_vals[4] * 500) / 10000;
                snprintf(row, sizeof(row), "%cGLD %3uMS", cur, (unsigned)ms);
                break;
            }
            case 6: {
                uint32_t x10 = 1 + ((uint32_t)s_fx_vals[5] * 99) / 10000;
                snprintf(row, sizeof(row), "%cLFR%u.%uHZ", cur,
                         (unsigned)(x10 / 10), (unsigned)(x10 % 10));
                break;
            }
            case 7: {
                uint32_t hz = ((uint32_t)s_fx_vals[6] * 5000) / 10000;
                snprintf(row, sizeof(row), "%cLFD%4uHZ", cur, (unsigned)hz);
                break;
            }
            default: row[0] = '\0'; break;
        }

        uint16_t fg = editing ? C_CYAN : (is_cur ? C_WHITE : C_LTGRAY);
        uint16_t bg = is_cur ? C_DKGRAY : C_BLACK;
        int y = 24 + vi * 22;
        if (is_cur) tft_fill(0, y - 2, TFT_W-1, y + 17, C_DKGRAY);
        tft_text(row, 4, y, fg, bg, 2);
    }

    tft_fill(0, 114, TFT_W-1, 115, C_DKGRAY);
    if (s_fx_editing)
        tft_text("ROT:ADJ BTN:DONE HLD:BACK", 2, 118, C_DKGRAY, C_BLACK, 1);
    else if (s_fx_cur == 0)
        tft_text("BTN:CYCLE        HLD:BACK", 2, 118, C_DKGRAY, C_BLACK, 1);
    else
        tft_text("BTN:EDIT         HLD:BACK", 2, 118, C_DKGRAY, C_BLACK, 1);
}

// ---- ADSR (legacy — now part of UI_MAIN ENV column) ---------
static void ui_draw_adsr(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

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
        if (i == 2)
            snprintf(row, sizeof(row), "%c%s: %3" PRIu32 "%s", cursor, lbls[i], val, units[i]);
        else
            snprintf(row, sizeof(row), "%c%s:%4" PRIu32 "%s", cursor, lbls[i], val, units[i]);

        uint16_t fg = editing ? C_CYAN : (is_cur ? C_WHITE : C_LTGRAY);
        uint16_t bg = is_cur ? C_DKGRAY : C_BLACK;
        if (is_cur) tft_fill(0, 22+i*22, TFT_W-1, 22+i*22+21, C_DKGRAY);
        tft_text(row, 4, 24+i*22, fg, bg, 2);
    }

    tft_fill(0, 112, TFT_W-1, 113, C_DKGRAY);
    if (s_adsr_editing)
        tft_text("ROT:ADJ  BTN:DONE  HLD:BACK", 2, 117, C_DKGRAY, C_BLACK, 1);
    else
        tft_text("ROT:MOVE BTN:EDIT  HLD:BACK", 2, 117, C_DKGRAY, C_BLACK, 1);
}

static void ui_redraw(void)
{
    switch (s_sec) {
        case UI_CARDS:   ui_draw_cards();   break;
        case UI_MAIN:    ui_draw_main();    break;
        case UI_CALIB:   ui_draw_calib();   break;
        case UI_EFFECTS: ui_draw_effects(); break;
        case UI_ADSR:    ui_draw_adsr();    break;
        case UI_PRESETS: ui_draw_presets(); break;
    }
    ui_draw_ribbon(); // always last — overwrites y=0..21
}

// ── Section enter helpers ─────────────────────────────────────
static void ui_enter(ui_section_t sec)
{
    s_sec = sec;
    s_dirty = true;

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
    if (sec == UI_MAIN) {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        s_main_adsr[0]   = st.env_attack;
        s_main_adsr[1]   = st.env_decay;
        s_main_adsr[2]   = st.env_sustain;
        s_main_adsr[3]   = st.env_release;
        s_main_flt_cut   = st.filter_cutoff;
        s_main_flt_res   = st.filter_resonance;
        s_main_flt_type  = st.filter_type;
        s_main_lfo_rate  = st.lfo_rate;
        s_main_lfo_depth = st.lfo_depth;
        s_main_ech_amt   = st.echo_amount;
        s_main_ech_fb    = st.echo_feedback;
        s_main_gld       = st.glide;
        s_main_col       = 0;
        s_main_editing   = false;
        s_main_sub       = 0;
    }
    if (sec == UI_CALIB) {
        s_cal_editing = false;
    }
    if (sec == UI_PRESETS) {
        for (int i = 0; i < PRESET_COUNT; i++)
            preset_nvs_load(i);
        s_prst_cur    = 0;
        s_prst_action = false;
        s_prst_act    = 0;
    }
    if (sec == UI_EFFECTS) {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        s_fx_flt_type = st.filter_type;
        s_fx_vals[0]  = st.filter_cutoff;
        s_fx_vals[1]  = st.filter_resonance;
        s_fx_vals[2]  = st.echo_amount;
        s_fx_vals[3]  = st.echo_feedback;
        s_fx_vals[4]  = st.glide;
        s_fx_vals[5]  = st.lfo_rate;
        s_fx_vals[6]  = st.lfo_depth;
        s_fx_cur      = 0;
        s_fx_scroll   = 0;
        s_fx_editing  = false;
    }
}

static void ui_exit_to_cards(void)
{
    if (s_sec == UI_CALIB && s_cal_editing) {
        touch_telemetry_set_threshold((uint8_t)s_cal_pad, s_cal_thr);
        s_cal_editing = false;
    }
    if (s_sec == UI_MAIN || s_sec == UI_EFFECTS || s_sec == UI_ADSR) {
        amy_engine_save_state();
    }
    ui_enter(UI_CARDS);
}

// ── Public API ────────────────────────────────────────────────

void ui_notify_lever(void) { s_dirty = true; }

void ui_set_octave(int shift) { s_octave_shift = shift; s_dirty = true; }

void ui_set_pad_active(uint8_t pad, bool active)
{
    if (pad >= TOUCH_NOTE_PADS || s_pad_active[pad] == active) return;
    s_pad_active[pad] = active;
    s_dirty = true; // main task handles SPI — touch task must not call tft directly
}

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

    case UI_CARDS:
        if (delta) {
            s_card_cur = (s_card_cur + delta % 3 + 3) % 3;
            s_dirty = true;
        }
        if (short_press) {
            switch (s_card_cur) {
                case 0: ui_enter(UI_MAIN);    break;
                case 1: ui_enter(UI_PRESETS); break;
                case 2: ui_enter(UI_CALIB);   break;
            }
        }
        break;

    case UI_MAIN:
        if (!s_main_editing) {
            if (delta) {
                s_main_col = (s_main_col + delta % 5 + 5) % 5;
                s_main_sub = 0;
                s_dirty = true;
            }
            if (short_press) { s_main_editing = true; s_main_sub = 0; s_dirty = true; }
            if (long_press)  { amy_engine_save_state(); ui_enter(UI_CARDS); }
        } else {
            if (delta) {
                switch (s_main_col) {
                case 0: // ENV
                    s_main_adsr[s_main_sub] = u16clamp((int)s_main_adsr[s_main_sub] + delta * 500);
                    amy_engine_set_adsr(s_main_adsr[0], s_main_adsr[1], s_main_adsr[2], s_main_adsr[3]);
                    break;
                case 1: // FLT
                    if (s_main_sub == 0) {
                        s_main_flt_cut = u16clamp((int)s_main_flt_cut + delta * 500);
                        amy_engine_set_filter(s_main_flt_cut, s_main_flt_res);
                    } else if (s_main_sub == 1) {
                        s_main_flt_res = u16clamp((int)s_main_flt_res + delta * 500);
                        amy_engine_set_filter(s_main_flt_cut, s_main_flt_res);
                    } else { // TYPE (sub=2): encoder cycles
                        if (delta > 0) s_main_flt_type = (s_main_flt_type + 1) % 3;
                        else           s_main_flt_type = (s_main_flt_type + 2) % 3;
                        amy_engine_set_filter_type(s_main_flt_type);
                    }
                    break;
                case 2: // LFO
                    if (s_main_sub == 0) s_main_lfo_rate  = u16clamp((int)s_main_lfo_rate  + delta * 500);
                    else                 s_main_lfo_depth = u16clamp((int)s_main_lfo_depth + delta * 500);
                    amy_engine_set_lfo(s_main_lfo_rate, s_main_lfo_depth);
                    break;
                case 3: // ECH
                    if (s_main_sub == 0) s_main_ech_amt = u16clamp((int)s_main_ech_amt + delta * 500);
                    else                 s_main_ech_fb  = u16clamp((int)s_main_ech_fb  + delta * 500);
                    amy_engine_set_echo(s_main_ech_amt, s_main_ech_fb);
                    break;
                case 4: // GLD
                    s_main_gld = u16clamp((int)s_main_gld + delta * 500);
                    amy_engine_set_glide(s_main_gld);
                    break;
                }
                s_dirty = true;
            }
            if (short_press) {
                s_main_sub = (s_main_sub + 1) % s_main_sub_max[s_main_col];
                s_dirty = true;
            }
            if (long_press) { amy_engine_save_state(); s_main_editing = false; s_dirty = true; }
        }
        break;

    case UI_CALIB:
        if (!s_cal_editing) {
            if (delta) {
                s_cal_pad = (s_cal_pad + delta % TOUCH_TOTAL_PADS + TOUCH_TOTAL_PADS) % TOUCH_TOTAL_PADS;
                s_dirty = true;
            }
            if (short_press) {
                s_cal_thr     = touch_telemetry_get_threshold((uint8_t)s_cal_pad);
                s_cal_editing = true;
                s_dirty       = true;
            }
            if (long_press) ui_exit_to_cards();
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
                ui_exit_to_cards();
            }
        }
        break;

    case UI_EFFECTS:
        if (!s_fx_editing) {
            if (delta) {
                s_fx_cur += delta;
                if (s_fx_cur < 0) s_fx_cur = 0;
                if (s_fx_cur > 7) s_fx_cur = 7;
                if (s_fx_cur < s_fx_scroll)       s_fx_scroll = s_fx_cur;
                if (s_fx_cur >= s_fx_scroll + 4)  s_fx_scroll = s_fx_cur - 3;
                s_dirty = true;
            }
            if (short_press) {
                if (s_fx_cur == 0) {
                    s_fx_flt_type = (s_fx_flt_type + 1) % 3;
                    // Per-type defaults recalculated for Spark exponential formula.
                    // LPF≈8kHz, BPF≈1.3kHz, HPF≈250Hz
                    static const uint16_t s_flt_cutoff_dflt[3] = {7778, 5579, 3581};
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
                        amy_engine_set_echo(s_fx_vals[2], s_fx_vals[3]);   break;
                    case 5:
                        amy_engine_set_glide(s_fx_vals[4]); break;
                    case 6: case 7:
                        amy_engine_set_lfo(s_fx_vals[5], s_fx_vals[6]); break;
                }
                s_dirty = true;
            }
            if (short_press) { s_fx_editing = false; s_dirty = true; }
        }
        if (long_press) ui_exit_to_cards();
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
        if (long_press) ui_exit_to_cards();
        break;

    case UI_PRESETS:
        if (!s_prst_action) {
            if (delta) {
                s_prst_cur = (s_prst_cur + delta % PRESET_COUNT + PRESET_COUNT) % PRESET_COUNT;
                s_dirty = true;
            }
            if (short_press) {
                s_prst_action = true;
                // Auto-select sensible default: LOAD for occupied, SAVE for empty
                s_prst_act = s_preset_valid[s_prst_cur] ? 0 : 1;
                s_dirty = true;
            }
            if (long_press) ui_exit_to_cards();
        } else {
            if (delta) {
                s_prst_act = (s_prst_act + delta % 3 + 3) % 3;
                s_dirty = true;
            }
            if (short_press) {
                switch (s_prst_act) {
                case 0: // LOAD — no-op on empty slot
                    if (s_preset_valid[s_prst_cur]) preset_apply(s_prst_cur);
                    break;
                case 1: // SAVE — always available
                    preset_capture(s_prst_cur);
                    preset_nvs_save(s_prst_cur);
                    break;
                case 2: // CLR — no-op on empty slot
                    if (s_preset_valid[s_prst_cur]) preset_nvs_clear(s_prst_cur);
                    break;
                }
                s_prst_action = false;
                s_dirty = true;
            }
            if (long_press) {
                s_prst_action = false; // cancel action, stay in presets
                s_dirty = true;
            }
        }
        break;
    }

    // Timed refresh only for CALIB — RAW/BASE update from hardware without user input
    static TickType_t last_refresh = 0;
    TickType_t now = xTaskGetTickCount();
    if (s_sec == UI_CALIB && !changed && (now - last_refresh) >= pdMS_TO_TICKS(50)) {
        s_dirty      = true;
        last_refresh = now;
    }
    if (s_dirty || changed) {
        last_refresh = now;
        s_dirty = false;
        ui_redraw();
    }
}
