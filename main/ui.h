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
#include "looper.h"
#include "mux_pots.h"

// ── TFT hardware pins ────────────────────────────────────────
#define TFT_SCLK  GPIO_NUM_36
#define TFT_MOSI  GPIO_NUM_35
#define TFT_CS    GPIO_NUM_37
#define TFT_DC    GPIO_NUM_45
#define TFT_RST   GPIO_NUM_21
#define TFT_W     160
#define TFT_H     128

// RGB565 palette — panel is BGR (MADCTL bit3=1), so R and B channels are physically swapped.
// All values below are corrected for BGR: what we call C_RED sends B=31 on the wire so
// the panel displays it as red.  Green (middle channel) is unaffected.
#define C_BLACK   0xFFFFu  // light mode: C_BLACK renders WHITE on screen
#define C_WHITE   0x0000u  // light mode: C_WHITE renders BLACK on screen
#define C_GREEN   0x07E0u  // G channel symmetric — unchanged
#define C_RED     0x001Fu  // BGR-corrected (was 0xF800, appeared blue)
#define C_YELLOW  0x07FFu  // BGR-corrected (was 0xFFE0, appeared cyan)
#define C_CYAN    0xFFE0u  // BGR-corrected (was 0x07FFu, appeared yellow)
#define C_ORANGE  0x029Fu  // warm amber — TE accent (R=255,G=160,B=0 as seen on panel)
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

// Bold text: draw twice with 1px x-offset for a heavier stroke.
static void tft_text_bold(const char *str, int x, int y, uint16_t fg, uint16_t bg, int sc)
{
    tft_text(str, x,   y, fg, bg, sc);
    tft_text(str, x+1, y, fg, bg, sc);
}

// Outline rounded rectangle (corner radius ≈ 2 px).
static void tft_rrect(int x0, int y0, int x1, int y1, uint16_t c)
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
static void tft_rrect_fill(int x0, int y0, int x1, int y1, uint16_t c)
{
    for (int y = y0; y <= y1; y++) {
        int ins = (y == y0 || y == y1) ? 2 : (y == y0+1 || y == y1-1) ? 1 : 0;
        tft_fill(x0+ins, y, x1-ins, y, c);
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

// MAIN 6-card overview + per-card edit
static int      s_main_card    = 0;    // 0-5: selected card in overview
static bool     s_main_in_card = false; // false=overview, true=card edit
static int      s_main_param   = 0;    // param index within card edit
// Param counts per card: ENV FLT EG1 LFO ECH GLD
static const int s_card_param_max[6] = {4, 3, 2, 2, 3, 1};
// Mirror of engine state for display (loaded on enter, pot-synced in overview)
static uint16_t s_main_adsr[4];        // 0=A 1=D 2=S 3=R
static uint16_t s_main_flt_cut;
static uint16_t s_main_flt_res;
static uint8_t  s_main_flt_type;       // 0=LPF 1=BPF 2=HPF
static uint16_t s_main_lfo_rate;
static uint16_t s_main_lfo_depth;
static uint16_t s_main_ech_amt;
static uint16_t s_main_ech_fb;
static uint16_t s_main_ech_dly;
static uint16_t s_main_eg1_depth;
static uint16_t s_main_eg1_decay;
static uint16_t s_main_gld;
// Echo delay snap values (0-10000 → 1/16, 1/8, 1/6, 1/4, 1/3, 1/2 second)
static const uint16_t s_echo_delay_snaps[6] = {277, 1667, 2593, 4444, 6296, 10000};

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
    uint16_t eg1_depth;
    uint16_t eg1_decay;
    uint16_t ech_dly;
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
static uint32_t ui_a_ms(uint16_t v)      { return (uint32_t)(2.0f   + (v/10000.0f)*1998.0f); }
static uint32_t ui_d_ms(uint16_t v)      { return (uint32_t)(5.0f   + (v/10000.0f)*995.0f);  }
static uint32_t ui_s_pct(uint16_t v)     { return v / 100u; }
static uint32_t ui_r_ms(uint16_t v)      { return (uint32_t)(10.0f  + (v/10000.0f)*4990.0f); }
static float    ui_filter_hz(uint16_t v) { return fminf(200.0f * powf(2.0f, 5.9f * (v/10000.0f)), 12000.0f); }
static float    ui_filter_q(uint16_t v)  { return 0.7f  * powf(2.0f, 4.0f * (v/10000.0f)); }
static float    ui_lfo_hz(uint16_t v)    { return fmaxf(0.6f*powf(2.0f,0.04f*((v/10000.0f)*127.0f))-0.1f,0.001f); }
static uint32_t ui_eg1_decay_ms(uint16_t v) { return (uint32_t)(5.0f + (v/10000.0f)*1995.0f); }
static uint32_t ui_glide_ms(uint16_t v)  { return (uint32_t)((v/10000.0f)*500.0f); }

// Returns fraction string for echo delay (0-10000 scale, snapped to nearest 1/x of a second).
static const char* ui_echo_frac(uint16_t v) {
    if (v <  978) return "1/16";
    if (v < 2133) return "1/8";
    if (v < 3511) return "1/6";
    if (v < 5378) return "1/4";
    if (v < 8156) return "1/3";
    return "1/2";
}

// Nearest echo delay snap index (for encoder step-through).
static int echo_snap_idx(uint16_t v) {
    int best = 0, best_d = abs((int)v - (int)s_echo_delay_snaps[0]);
    for (int i = 1; i < 6; i++) {
        int d = abs((int)v - (int)s_echo_delay_snaps[i]);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

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
    p->ech_amt   = st.echo_amount;
    p->ech_fb    = st.echo_feedback;
    p->gld       = st.glide;
    p->eg1_depth = st.filter_env_depth;
    p->eg1_decay = st.filter_env_decay;
    p->ech_dly   = st.echo_delay;
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
    amy_engine_set_echo_delay(p->ech_dly);
    amy_engine_set_filter_env(p->eg1_depth, p->eg1_decay);
    amy_engine_set_glide(p->gld);
    amy_engine_save_state();
    mux_pots_on_preset_load();  // Lock all pots to prevent jumps when pot is at different position
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
// 8 pad circles + waveform glyph + octave label + looper dot + separator.
static void ui_draw_ribbon(void)
{
    tft_fill(0, 0, TFT_W-1, 19, C_BLACK);

    // Pad circles: centers at x=6+i*10, y=10 — radius 3, spaced for 160px
    for (int i = 0; i < TOUCH_NOTE_PADS; i++)
        tft_pad_circle(6 + i * 10, 10, s_pad_active[i]);

    // Waveform glyph (x=90..110, y=3..17) — orange; KS easter egg shows "P" for pluck
    amy_engine_state_t st;
    amy_engine_get_state(&st);
    if (st.wave_id == AMY_ENGINE_WAVE_KS)
        tft_text_bold("P", 95, 4, C_ORANGE, C_BLACK, 2);
    else
        ui_draw_wave_glyph(st.wave_id, 90, 3, 110, 17, C_ORANGE);

    // Octave label — yellow when shifted, dim gray at zero
    char oct_buf[8];
    if (s_octave_shift == 0)
        snprintf(oct_buf, sizeof(oct_buf), "OCT 0");
    else
        snprintf(oct_buf, sizeof(oct_buf), "OCT%+d", s_octave_shift);
    tft_text(oct_buf, 116, 6, s_octave_shift ? C_YELLOW : C_DKGRAY, C_BLACK, 1);

    // Looper dot (5×5, top-right) — colors now correct with BGR fix
    looper_state_t ls = looper_get_state();
    uint16_t lc = (ls == LOOPER_RECORDING) ? C_RED    :
                  (ls == LOOPER_PLAYING)   ? C_GREEN  :
                  (ls == LOOPER_OVERDUB)   ? C_YELLOW : C_BLACK;
    tft_fill(154, 5, 158, 9, lc);

    // Separator
    tft_fill(0, 20, TFT_W-1, 21, C_DKGRAY);
}

// ---- CARDS (top-level navigation) ---------------------------
// 3 rounded cards with gap borders, TE-style amber accent for selected.
static void ui_draw_cards(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    static const char * const names[3] = {"MAIN", "PRST", "CAL"};
    // Card x ranges with 1px gaps between them
    static const int cxs[3] = {1,  54, 108};
    static const int cxe[3] = {52, 105, 158};

    for (int i = 0; i < 3; i++) {
        bool sel = (i == s_card_cur);
        int x0 = cxs[i], x1 = cxe[i];
        int cw = x1 - x0 + 1;

        // Rounded card outline — orange if selected, dark gray if not
        tft_rrect(x0, 23, x1, 126, sel ? C_ORANGE : C_DKGRAY);

        // Header/body divider line at y=41
        tft_fill(x0+2, 41, x1-2, 41, sel ? C_ORANGE : C_DKGRAY);

        // Header text: scale-2 bold orange for selected, scale-1 dim for unselected
        int nchars = (int)strlen(names[i]);
        if (sel) {
            int tx = x0 + (cw - nchars * 12) / 2 - 1; // -1 compensates bold +1 offset
            tft_text_bold(names[i], tx, 25, C_ORANGE, C_BLACK, 2);
        } else {
            int tx = x0 + (cw - nchars * 6) / 2;
            tft_text(names[i], tx, 29, C_DKGRAY, C_BLACK, 1);
        }
    }

    // ── MAIN body: mini ADSR envelope preview (card 0, x=1..52) ──
    {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        int ex0=4, ex1=50, ey_bot=116, ey_top=54, eh=ey_bot-ey_top;
        int Aw=(int)((long)st.env_attack  * (ex1-ex0) / 30000); if(Aw<1)Aw=1;
        int Dw=(int)((long)st.env_decay   * (ex1-ex0) / 30000); if(Dw<1)Dw=1;
        int Sw=5;
        int Rw=(int)((long)st.env_release * (ex1-ex0) / 30000); if(Rw<1)Rw=1;
        int tot=Aw+Dw+Sw+Rw, avail=ex1-ex0-2;
        if (tot > avail) { Aw=Aw*avail/tot; Dw=Dw*avail/tot; Rw=Rw*avail/tot; Sw=3; }
        int sy=ey_bot-(int)((long)st.env_sustain*eh/10000);
        if (sy < ey_top+2) sy = ey_top+2;
        int xe=ex0+(avail-(Aw+Dw+Sw+Rw))/2;
        int xa=xe+Aw, xd=xa+Dw, xs=xd+Sw, xr=xs+Rw;
        uint16_t ec = (s_card_cur == 0) ? C_ORANGE : C_DKGRAY;
        tft_line(xe, ey_bot, xa, ey_top, ec);
        tft_line(xa, ey_top, xd, sy, ec);
        tft_line(xd, sy, xs, sy, ec);
        tft_line(xs, sy, xr, ey_bot, ec);
    }

    // ── PRST body: 6 small preset squares 3×2 (card 1, x=54..105) ──
    {
        // squares 13×13, col gap 4, row gap 15
        // col x: 57, 74, 91 — row y: 56, 84
        for (int j = 0; j < PRESET_COUNT; j++) {
            int col = j % 3, row = j / 3;
            int sx = 57 + col * 17;  // 13px square + 4px gap
            int sy = 56 + row * 29;  // 13px square + 16px gap (centers in 85px body)
            bool occ = s_preset_valid[j];
            if (occ)
                tft_rrect_fill(sx, sy, sx+12, sy+12, (s_card_cur==1) ? C_ORANGE : C_LTGRAY);
            else
                tft_rrect(sx, sy, sx+12, sy+12, C_DKGRAY);
            char n[2] = {(char)('1'+j), 0};
            uint16_t nc = occ ? C_BLACK  : C_DKGRAY;
            uint16_t nb = occ ? ((s_card_cur==1) ? C_ORANGE : C_LTGRAY) : C_BLACK;
            tft_text(n, sx+4, sy+3, nc, nb, 1);
        }
    }

    // ── CAL body: decorative bar + threshold marker (card 2, x=108..158) ──
    {
        uint16_t mc = (s_card_cur == 2) ? C_ORANGE : C_DKGRAY;
        // 8 tiny vertical bars representing pads
        for (int p = 0; p < 8; p++) {
            int bx = 111 + p * 6;
            int by_bot = 114, by_top = 54;
            int bh = by_bot - by_top;
            // static height pattern: alternating heights, decorative only
            int fh = bh * (3 + (p % 3)) / 6;
            tft_fill(bx, by_bot-fh, bx+3, by_bot, (p % 3 == 0) ? mc : C_DKGRAY);
        }
        // Threshold line across all bars
        tft_fill(110, 54+28, 157, 54+29, mc);
    }
}

// ---- PRESETS — 2×3 slot grid --------------------------------
// Columns: x=0..52, 54..106, 108..159.  Rows: y=22..73 / y=75..127.
static void ui_draw_presets(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    tft_fill(53,  22, 53,  TFT_H-1, C_DKGRAY);
    tft_fill(107, 22, 107, TFT_H-1, C_DKGRAY);
    tft_fill(0,   74, TFT_W-1, 74,  C_DKGRAY);

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

        // Header strip (10px) — orange for selected, dark gray for others
        uint16_t hbg = sel ? C_ORANGE : C_DKGRAY;
        uint16_t hfg = sel ? C_BLACK  : C_LTGRAY;
        tft_fill(cx, ry, cx + cw - 1, ry + 9, hbg);
        char hdr[4]; snprintf(hdr, sizeof(hdr), "P%d", i + 1);
        tft_text(hdr, cx + 2, ry + 1, hfg, hbg, 1);
        if (s_preset_valid[i]) {
            int dx = cx + cw - 5;
            tft_fill((uint8_t)dx, (uint8_t)(ry+3), (uint8_t)(dx+2), (uint8_t)(ry+5), hfg);
        }

        // Body
        int body_top = ry + 10;
        int body_bot = (row == 0) ? ry + 42 : TFT_H - 1;
        tft_fill(cx, body_top, cx + cw - 1, body_bot, C_BLACK);

        if (s_prst_action && sel) {
            // Action overlay: LOAD / SAVE / CLR
            for (int a = 0; a < 3; a++) {
                bool avail = (a == 1) || s_preset_valid[i];
                bool asel  = (a == s_prst_act);
                uint16_t fg = !avail ? C_DKGRAY : (asel ? C_ORANGE : C_LTGRAY);
                uint16_t bg = (asel && avail) ? C_DKGRAY : C_BLACK;
                int ay = body_top + 4 + a * 12;
                if (asel && avail)
                    tft_fill(cx, (uint8_t)ay, cx+cw-1, (uint8_t)(ay+8), C_DKGRAY);
                int llen = (int)strlen(act_lbl[a]);
                tft_text(act_lbl[a], cx + (cw - llen*6)/2, ay, fg, bg, 1);
            }
        } else if (s_preset_valid[i]) {
            preset_t *p = &s_presets[i];
            uint16_t fc = sel ? C_ORANGE : C_LTGRAY;

            // Mini wave glyph (20×12, centered)
            int gx0 = cx + (cw - 20) / 2;
            ui_draw_wave_glyph(p->wave_id, gx0, body_top+2, gx0+19, body_top+13, fc);

            // Wave name (2 chars)
            const char *wname = (p->wave_id < 7) ? wn[p->wave_id] : "??";
            tft_text(wname, cx+2, body_top+16, fc, C_BLACK, 1);

            // Filter cutoff summary
            float fc_hz = ui_filter_hz(p->flt_cut);
            char fstr[6];
            snprintf(fstr, sizeof(fstr), fc_hz >= 1000.0f ? "F%uK" : "F%u",
                     fc_hz >= 1000.0f ? (unsigned)(fc_hz/1000.0f+0.5f) : (unsigned)fc_hz);
            tft_text(fstr, cx+2, body_top+26, fc, C_BLACK, 1);

            // Attack summary
            uint32_t atk = (uint32_t)(2.0f + (p->env_atk/10000.0f)*1998.0f);
            char astr[6];
            snprintf(astr, sizeof(astr), atk >= 1000 ? "A%uK" : "A%u",
                     (unsigned)(atk >= 1000 ? atk/1000 : atk));
            tft_text(astr, cx+26, body_top+26, fc, C_BLACK, 1);
        } else {
            tft_text("----", cx+(cw-4*6)/2, body_top+16, C_DKGRAY, C_BLACK, 1);
        }
    }
}

// ---- MAIN card — 6-card overview + per-card edit ----------------

// Thin vertical bar: 1px track edges, fill from bottom.
static void ui_vbar(int x0, int x1, int y_top, int y_bot, uint16_t val, uint16_t col)
{
    int fh = (int)((long)val * (y_bot - y_top) / 10000);
    tft_fill((uint8_t)x0, (uint8_t)y_top, (uint8_t)x1, (uint8_t)y_bot, C_BLACK);
    tft_fill((uint8_t)x0, (uint8_t)y_top, (uint8_t)x0, (uint8_t)y_bot, C_DKGRAY);
    tft_fill((uint8_t)x1, (uint8_t)y_top, (uint8_t)x1, (uint8_t)y_bot, C_DKGRAY);
    if (fh > 0) tft_fill((uint8_t)x0, (uint8_t)(y_bot-fh), (uint8_t)x1, (uint8_t)y_bot, col);
}

// ---- 6-card overview ----------------------------------------
static void ui_draw_main_overview(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    // 1px dividers between cols and rows
    tft_fill(52,  22, 52,  TFT_H-1, C_DKGRAY);
    tft_fill(105, 22, 105, TFT_H-1, C_DKGRAY);
    tft_fill(0,   74, TFT_W-1, 74,  C_DKGRAY);

    static const int    cxs[3]  = {0, 53, 106};
    static const int    cws[3]  = {52, 52, 53};
    static const int    rys[2]  = {22, 75};
    static const int    rhs[2]  = {52, 53};
    static const char * const nm[6] = {"ENV","FLT","EG1","LFO","ECH","GLD"};
    static const char * const flt_nm[3] = {"LPF","BPF","HPF"};

    amy_engine_state_t st;
    amy_engine_get_state(&st);

    for (int ci = 0; ci < 6; ci++) {
        int col = ci % 3, row = ci / 3;
        int cx = cxs[col], cw = cws[col];
        int ry = rys[row], rh = rhs[row];
        bool sel = (ci == s_main_card);
        uint16_t oc = sel ? C_ORANGE : C_DKGRAY;

        tft_rrect(cx, ry, cx+cw-1, ry+rh-1, oc);
        // Header (10px): filled orange when selected
        tft_fill(cx+1, ry+1, cx+cw-2, ry+9, sel ? C_ORANGE : C_BLACK);
        int tx = cx + (cw - 3*6) / 2;
        tft_text(nm[ci], tx, ry+1, sel ? C_BLACK : C_DKGRAY, sel ? C_ORANGE : C_BLACK, 1);
        tft_fill(cx+1, ry+10, cx+cw-2, ry+10, oc);

        int bx = cx + 3;
        int by = ry + 13;  // body start y

        switch (ci) {
        case 0: { // ENV — mini ADSR shape
            int ex0=cx+3, ex1=cx+cw-4;
            int ey_t=ry+15, ey_b=ry+rh-5, eh=ey_b-ey_t;
            int Aw=(int)((long)st.env_attack *(ex1-ex0)/30000); if(Aw<1)Aw=1;
            int Dw=(int)((long)st.env_decay  *(ex1-ex0)/30000); if(Dw<1)Dw=1;
            int Sw=3;
            int Rw=(int)((long)st.env_release*(ex1-ex0)/30000); if(Rw<1)Rw=1;
            int tot=Aw+Dw+Sw+Rw, av=ex1-ex0-2;
            if(tot>av){Aw=Aw*av/tot;Dw=Dw*av/tot;Rw=Rw*av/tot;Sw=2;}
            int sy=ey_b-(int)((long)st.env_sustain*eh/10000);
            if(sy<ey_t+1)sy=ey_t+1;
            int xe=ex0+(av-(Aw+Dw+Sw+Rw))/2;
            int xa=xe+Aw,xd=xa+Dw,xs=xd+Sw,xr=xs+Rw;
            tft_line(xe,ey_b,xa,ey_t,oc); tft_line(xa,ey_t,xd,sy,oc);
            tft_line(xd,sy,xs,sy,oc);     tft_line(xs,sy,xr,ey_b,oc);
            break;
        }
        case 1: { // FLT — type + F + Q
            tft_text(flt_nm[st.filter_type], bx, by, oc, C_BLACK, 1);
            float fhz = ui_filter_hz(st.filter_cutoff);
            char buf[16];
            if(fhz>=1000.0f) snprintf(buf,sizeof(buf),"F%3uK",(unsigned)(fhz/1000.0f+0.5f));
            else              snprintf(buf,sizeof(buf),"F%4u",(unsigned)fhz);
            tft_text(buf, bx, by+9,  oc, C_BLACK, 1);
            float fq=ui_filter_q(st.filter_resonance);
            uint32_t qi=(uint32_t)(fq*10.0f);
            snprintf(buf,sizeof(buf),"Q%u.%u",(unsigned)(qi/10),(unsigned)(qi%10));
            tft_text(buf, bx, by+18, oc, C_BLACK, 1);
            break;
        }
        case 2: { // EG1 — depth + decay
            char buf[16];
            snprintf(buf,sizeof(buf),"F%4u",(unsigned)((st.filter_env_depth/10000.0f)*8000.0f));
            tft_text(buf, bx, by,   oc, C_BLACK, 1);
            snprintf(buf,sizeof(buf),"D%4u",(unsigned)ui_eg1_decay_ms(st.filter_env_decay));
            tft_text(buf, bx, by+9, oc, C_BLACK, 1);
            break;
        }
        case 3: { // LFO — rate + depth
            float rh2=ui_lfo_hz(st.lfo_rate);
            uint32_t rh10=(uint32_t)(rh2*10.0f);
            char buf[16];
            snprintf(buf,sizeof(buf),"R%u.%uHZ",(unsigned)(rh10/10),(unsigned)(rh10%10));
            tft_text(buf, bx, by,   oc, C_BLACK, 1);
            snprintf(buf,sizeof(buf),"D%4u",(unsigned)((st.lfo_depth/10000.0f)*5000.0f));
            tft_text(buf, bx, by+9, oc, C_BLACK, 1);
            break;
        }
        case 4: { // ECHO — amount% + delay fraction
            char buf[16];
            snprintf(buf,sizeof(buf),"%3u%%",(unsigned)(st.echo_amount/100));
            tft_text(buf, bx, by,   oc, C_BLACK, 1);
            tft_text(ui_echo_frac(st.echo_delay), bx, by+9, oc, C_BLACK, 1);
            break;
        }
        case 5: { // GLD — ms value
            uint32_t ms=ui_glide_ms(st.glide);
            char buf[16];
            if(ms==0) snprintf(buf,sizeof(buf),"OFF");
            else      snprintf(buf,sizeof(buf),"%3ums",(unsigned)ms);
            int tw=(int)strlen(buf)*6;
            tft_text(buf, cx+(cw-tw)/2, by+9, oc, C_BLACK, 1);
            break;
        }
        }
    }
}

// ---- Single-card edit ----------------------------------------
static void ui_draw_main_card_edit(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    static const char * const nm[6]       = {"ENV","FLT","EG1","LFO","ECH","GLD"};
    static const char * const flt_nm[3]   = {"LPF","BPF","HPF"};

    tft_text_bold(nm[s_main_card], 4, 24, C_ORANGE, C_BLACK, 2);
    tft_fill(0, 42, TFT_W-1, 43, C_LTGRAY);

    int p  = s_main_param;
    // ENV needs 4 param lines so visual is shorter; others get more visual space.
    int vt = 46;
    int vb = (s_main_card == 0) ? 80 : 88;
    int py = (s_main_card == 0) ? 83 : 91;

    // pc(i): yellow if current param, orange otherwise
    #define PC(i) ((p==(i)) ? C_YELLOW : C_ORANGE)

    switch (s_main_card) {
    case 0: { // ENV — ADSR envelope shape + 4 param lines
        int ex0=8, ex1=TFT_W-9, eh=vb-vt;
        int Aw=(int)((long)s_main_adsr[0]*(ex1-ex0)/30000); if(Aw<1)Aw=1;
        int Dw=(int)((long)s_main_adsr[1]*(ex1-ex0)/30000); if(Dw<1)Dw=1;
        int Sw=5;
        int Rw=(int)((long)s_main_adsr[3]*(ex1-ex0)/30000); if(Rw<1)Rw=1;
        int tot=Aw+Dw+Sw+Rw, av=ex1-ex0-2;
        if(tot>av){Aw=Aw*av/tot;Dw=Dw*av/tot;Rw=Rw*av/tot;Sw=4;}
        int sy=vb-(int)((long)s_main_adsr[2]*eh/10000);
        if(sy<vt+2)sy=vt+2;
        int xa=ex0+Aw,xd=xa+Dw,xs=xd+Sw,xr=xs+Rw;
        tft_line(ex0,vb,xa,vt,PC(0)); tft_line(xa,vt,xd,sy,PC(1));
        tft_line(xd,sy,xs,sy,PC(2)); tft_line(xs,sy,xr,vb,PC(3));
        char buf[16];
        snprintf(buf,sizeof(buf),"A  %4ums",(unsigned)ui_a_ms(s_main_adsr[0]));
        tft_text(buf,4,py,   PC(0),C_BLACK,1);
        snprintf(buf,sizeof(buf),"D  %4ums",(unsigned)ui_d_ms(s_main_adsr[1]));
        tft_text(buf,4,py+8, PC(1),C_BLACK,1);
        snprintf(buf,sizeof(buf),"S    %3u%%",(unsigned)ui_s_pct(s_main_adsr[2]));
        tft_text(buf,4,py+16,PC(2),C_BLACK,1);
        snprintf(buf,sizeof(buf),"R  %4ums",(unsigned)ui_r_ms(s_main_adsr[3]));
        tft_text(buf,4,py+24,PC(3),C_BLACK,1);
        break;
    }
    case 1: { // FLT — type label + two bars + 3 param lines
        tft_text(flt_nm[s_main_flt_type], 4, vt, PC(2), C_BLACK, 1);
        int bw=5,gap=12;
        int bx0=(TFT_W-(bw+gap+bw))/2, bx1=bx0+bw+gap;
        ui_vbar(bx0,bx0+bw-1,vt+12,vb,s_main_flt_cut,PC(0));
        ui_vbar(bx1,bx1+bw-1,vt+12,vb,s_main_flt_res,PC(1));
        tft_text("F",bx0+1,vb+2,PC(0),C_BLACK,1);
        tft_text("Q",bx1+1,vb+2,PC(1),C_BLACK,1);
        char buf[16];
        float fhz=ui_filter_hz(s_main_flt_cut);
        if(fhz>=1000.0f) snprintf(buf,sizeof(buf),"F  %3uKHZ",(unsigned)(fhz/1000.0f+0.5f));
        else              snprintf(buf,sizeof(buf),"F  %4uHZ", (unsigned)fhz);
        tft_text(buf,4,py,   PC(0),C_BLACK,1);
        float fq=ui_filter_q(s_main_flt_res);
        uint32_t qi=(uint32_t)(fq*10.0f);
        snprintf(buf,sizeof(buf),"Q  %u.%u",(unsigned)(qi/10),(unsigned)(qi%10));
        tft_text(buf,4,py+8, PC(1),C_BLACK,1);
        snprintf(buf,sizeof(buf),"TYPE  %s",flt_nm[s_main_flt_type]);
        tft_text(buf,4,py+16,PC(2),C_BLACK,1);
        break;
    }
    case 2: { // EG1 — two bars + 2 param lines
        int bw=5,gap=14;
        int bx0=(TFT_W-(bw+gap+bw))/2, bx1=bx0+bw+gap;
        ui_vbar(bx0,bx0+bw-1,vt,vb,s_main_eg1_depth,PC(0));
        ui_vbar(bx1,bx1+bw-1,vt,vb,s_main_eg1_decay, PC(1));
        tft_text("F",bx0+1,vb+2,PC(0),C_BLACK,1);
        tft_text("D",bx1+1,vb+2,PC(1),C_BLACK,1);
        char buf[16];
        snprintf(buf,sizeof(buf),"F  %4uHZ",(unsigned)((s_main_eg1_depth/10000.0f)*8000.0f));
        tft_text(buf,4,py,   PC(0),C_BLACK,1);
        snprintf(buf,sizeof(buf),"D  %4ums",(unsigned)ui_eg1_decay_ms(s_main_eg1_decay));
        tft_text(buf,4,py+8, PC(1),C_BLACK,1);
        break;
    }
    case 3: { // LFO — two bars + 2 param lines
        int bw=5,gap=14;
        int bx0=(TFT_W-(bw+gap+bw))/2, bx1=bx0+bw+gap;
        ui_vbar(bx0,bx0+bw-1,vt,vb,s_main_lfo_rate, PC(0));
        ui_vbar(bx1,bx1+bw-1,vt,vb,s_main_lfo_depth,PC(1));
        tft_text("R",bx0+1,vb+2,PC(0),C_BLACK,1);
        tft_text("D",bx1+1,vb+2,PC(1),C_BLACK,1);
        char buf[16];
        float rh=ui_lfo_hz(s_main_lfo_rate);
        uint8_t rh_i=(uint8_t)(rh>99.0f?99.0f:rh);
        uint8_t rh_f=(uint8_t)((unsigned)(rh*10.0f)%10);
        snprintf(buf,sizeof(buf),"R  %u.%uHZ",rh_i,rh_f);
        tft_text(buf,4,py,   PC(0),C_BLACK,1);
        snprintf(buf,sizeof(buf),"D  %4uHZ",(unsigned)((s_main_lfo_depth/10000.0f)*5000.0f));
        tft_text(buf,4,py+8, PC(1),C_BLACK,1);
        break;
    }
    case 4: { // ECHO — two bars + fraction text + 3 param lines
        int bw=5,gap=12;
        int bx0=(TFT_W/2-(bw+gap/2))-4, bx1=bx0+bw+gap;
        ui_vbar(bx0,bx0+bw-1,vt,vb,s_main_ech_amt,PC(0));
        ui_vbar(bx1,bx1+bw-1,vt,vb,s_main_ech_fb, PC(1));
        tft_text("A",bx0+1,vb+2,PC(0),C_BLACK,1);
        tft_text("F",bx1+1,vb+2,PC(1),C_BLACK,1);
        tft_text(ui_echo_frac(s_main_ech_dly), bx1+14, vt+16, PC(2), C_BLACK, 1);
        char buf[16];
        snprintf(buf,sizeof(buf),"AMT  %3u%%",(unsigned)(s_main_ech_amt/100));
        tft_text(buf,4,py,   PC(0),C_BLACK,1);
        snprintf(buf,sizeof(buf),"FB   %3u%%",(unsigned)(s_main_ech_fb/100));
        tft_text(buf,4,py+8, PC(1),C_BLACK,1);
        snprintf(buf,sizeof(buf),"DLY  %s",ui_echo_frac(s_main_ech_dly));
        tft_text(buf,4,py+16,PC(2),C_BLACK,1);
        break;
    }
    case 5: { // GLD — single bar + 1 param line
        int bx=TFT_W/2-3;
        ui_vbar(bx,bx+5,vt,vb,s_main_gld,PC(0));
        tft_text("G",bx+1,vb+2,PC(0),C_BLACK,1);
        uint16_t ms=(uint16_t)ui_glide_ms(s_main_gld);
        char buf[16];
        if(ms==0) snprintf(buf,sizeof(buf),"GLD  OFF");
        else      snprintf(buf,sizeof(buf),"GLD  %3ums",ms);
        tft_text(buf,4,py,PC(0),C_BLACK,1);
        break;
    }
    }
    #undef PC

    tft_fill(0, 119, TFT_W-1, 120, C_DKGRAY);
    tft_text("ROT:ADJ  BTN:NEXT  HLD:BACK", 2, 122, C_DKGRAY, C_BLACK, 1);
}

static void ui_draw_main(void)
{
    if (s_main_in_card) ui_draw_main_card_edit();
    else                ui_draw_main_overview();
}

// ---- CALIBRATION --------------------------------------------
// Overview: 10 live bar columns (50 ms refresh), encoder selects pad, short press → edit.
// Edit:     full-screen single-pad with RAW/BASE/THR and horizontal bar (preserved).
static void ui_draw_calib(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    if (s_cal_editing) {
        // ── Single-pad edit ─────────────────────────────────────
        char phdr[12];
        if (s_cal_pad < TOUCH_NOTE_PADS)
            snprintf(phdr, sizeof(phdr), "PAD %u", (uint8_t)(s_cal_pad+1));
        else
            snprintf(phdr, sizeof(phdr), s_cal_pad == TOUCH_NOTE_PADS ? "OCT-" : "OCT+");
        tft_text_bold(phdr, 4, 24, C_ORANGE, C_BLACK, 2);
        tft_fill(0, 42, TFT_W-1, 43, C_LTGRAY);

        uint16_t raw  = touch_telemetry_get_raw((uint8_t)s_cal_pad);
        uint16_t base = touch_telemetry_get_baseline((uint8_t)s_cal_pad);
        uint16_t thr  = s_cal_thr;

        // Large horizontal bar (x=4..155, y=54..88, 34px tall)
        int bx0=4, bx1=TFT_W-5, by0=54, by1=88, blen=bx1-bx0;
        // Use delta from baseline (not absolute raw) — absolute values are ~60000+
        uint32_t dv = (raw >= base) ? (uint32_t)(raw - base) : (uint32_t)(base - raw);
        // Fixed scale = base/8 so threshold marker position reflects actual thr value
        uint32_t scale = (uint32_t)base / 8;
        if (scale < 2000) scale = 2000;
        int raw_x = (int)(dv  * blen / scale); if (raw_x > blen) raw_x = blen;
        int thr_x = (int)((uint32_t)thr * blen / scale); if (thr_x > blen) thr_x = blen;
        // Bar track (light gray)
        tft_fill(bx0, by0, bx1, by1, C_LTGRAY);
        // Delta fill — green when touching, dark gray otherwise
        bool active_touch = (thr > 0 && dv > (uint32_t)thr);
        if (raw_x > 0)
            tft_fill(bx0, by0, bx0+raw_x, by1, active_touch ? C_GREEN : C_DKGRAY);
        // Threshold marker: orange 2px vertical line at thr position (moves with encoder)
        int tx = bx0 + thr_x;
        tft_fill(tx, by0-4, tx+1, by1+4, C_ORANGE);

        // Values (scale-1 for clean compact layout)
        char buf[20];
        snprintf(buf, sizeof(buf), "RAW  %5u", (unsigned)raw);
        tft_text(buf, 4, 96, C_WHITE, C_BLACK, 1);
        snprintf(buf, sizeof(buf), "BASE %5u", (unsigned)base);
        tft_text(buf, 4, 106, C_LTGRAY, C_BLACK, 1);
        snprintf(buf, sizeof(buf), "THR  %5u", (unsigned)thr);
        tft_text(buf, 86, 96, C_ORANGE, C_BLACK, 1);

        tft_fill(0, 117, TFT_W-1, 118, C_LTGRAY);
        tft_text("ROT:THR  BTN:SAVE  HLD:BACK", 2, 121, C_LTGRAY, C_BLACK, 1);
        return;
    }

    // ── Overview: 10 static bars showing stored threshold level ──
    // 10 cols × (12px bar + 2px gap) − 1 gap = 138px → x_start = 11
    const int bar_w = 12, bar_gap = 2;
    const int x_start = (TFT_W - (TOUCH_TOTAL_PADS*(bar_w+bar_gap) - bar_gap)) / 2;
    const int y_top = 28, y_bot = 108, bar_h = y_bot - y_top;

    // Normalize bars to the largest threshold set across all pads
    uint16_t max_thr = 1;
    for (int p = 0; p < TOUCH_TOTAL_PADS; p++) {
        uint16_t t = touch_telemetry_get_threshold((uint8_t)p);
        if (t > max_thr) max_thr = t;
    }

    for (int p = 0; p < TOUCH_TOTAL_PADS; p++) {
        int bx = x_start + p*(bar_w+bar_gap);
        bool sel = (p == s_cal_pad);
        uint16_t thr = touch_telemetry_get_threshold((uint8_t)p);

        // Track line on black background
        tft_fill(bx, y_top, bx+bar_w-1, y_bot, C_BLACK);
        tft_fill(bx+bar_w/2, y_top, bx+bar_w/2, y_bot, C_DKGRAY);

        // Static bar: height = threshold / max_threshold
        int fh = (int)((uint32_t)thr * bar_h / max_thr);
        if (fh > 0)
            tft_fill(bx, y_bot-fh, bx+bar_w-1, y_bot, sel ? C_ORANGE : C_LTGRAY);

        // Selected: orange rounded outline
        if (sel)
            tft_rrect(bx-1, y_top-1, bx+bar_w, y_bot+1, C_ORANGE);

        // Label below bar (1..8 for note pads, - / + for OCT)
        char lbl[2]; lbl[1] = 0;
        lbl[0] = (p < TOUCH_NOTE_PADS) ? (char)('1'+p)
                                        : (p == TOUCH_NOTE_PADS ? '-' : '+');
        tft_text(lbl, bx+(bar_w-6)/2, y_bot+2, sel ? C_ORANGE : C_DKGRAY, C_BLACK, 1);
    }

    tft_fill(0, 119, TFT_W-1, 120, C_DKGRAY);
    tft_text("ROT:SEL  BTN:EDIT  HLD:BACK", 2, 122, C_DKGRAY, C_BLACK, 1);
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
                uint32_t hz = (uint32_t)ui_filter_hz(s_fx_vals[0]);
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
        s_main_ech_dly   = st.echo_delay;
        s_main_eg1_depth = st.filter_env_depth;
        s_main_eg1_decay = st.filter_env_decay;
        s_main_gld       = st.glide;
        s_main_card      = 0;
        s_main_in_card   = false;
        s_main_param     = 0;
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
    // Pre-load preset validity so the PRST squares in the cards view are correct on first draw
    for (int i = 0; i < PRESET_COUNT; i++)
        preset_nvs_load(i);
    // Factory preset: slot 6 (index 5) = KS pluck easter egg; written once to NVS if empty
    if (!s_preset_valid[5]) {
        preset_t *p = &s_presets[5];
        p->magic     = PRESET_MAGIC;
        p->wave_id   = AMY_ENGINE_WAVE_KS;
        p->flt_type  = AMY_ENGINE_FILTER_LPF;
        p->_pad      = 0;
        p->flt_cut   = 8000;
        p->flt_res   = 0;
        p->env_atk   = 100;
        p->env_dec   = 100;
        p->env_sus   = 0;
        p->env_rel   = 6000;
        p->lfo_rt    = 2000;
        p->lfo_dp    = 0;
        p->ech_amt   = 2500;
        p->ech_fb    = 2500;
        p->gld       = 0;
        p->eg1_depth = 0;
        p->eg1_decay = 1000;
        p->ech_dly   = 4444;
        s_preset_valid[5] = true;
        preset_nvs_save(5);
    }
    ui_redraw();
}

// Called from main loop every ~20 ms.
// delta: encoder steps since last call (signed).
// short_press / long_press / vlong_press: button event flags (at most one true per call).
// vlong_press (3.5 s hold): cycles looper state when in UI_CARDS.
void ui_tick(int delta, bool short_press, bool long_press, bool vlong_press)
{
    bool changed = (delta != 0) || short_press || long_press || vlong_press;

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
        if (long_press) {
            looper_cycle(); // IDLE→REC→PLAY→OD→PLAY→...
            s_dirty = true;
        }
        if (vlong_press) {
            looper_clear(); // 3.5 s hold — stop and clear loop
            s_dirty = true;
        }
        break;

    case UI_MAIN:
        // Pot display sync — 10 Hz, overview only; triggers redraw on pot-controlled changes
        if (!s_main_in_card) {
            static int64_t s_pot_sync_us = 0;
            int64_t t_now = esp_timer_get_time();
            if (t_now - s_pot_sync_us >= 100000LL) {
                s_pot_sync_us = t_now;
                amy_engine_state_t st;
                amy_engine_get_state(&st);
                bool ch = s_main_adsr[0] != st.env_attack       ||
                          s_main_adsr[1] != st.env_decay         ||
                          s_main_adsr[2] != st.env_sustain       ||
                          s_main_adsr[3] != st.env_release       ||
                          s_main_flt_cut  != st.filter_cutoff    ||
                          s_main_flt_res  != st.filter_resonance ||
                          s_main_ech_amt  != st.echo_amount      ||
                          s_main_ech_fb   != st.echo_feedback    ||
                          s_main_gld      != st.glide;
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
                s_main_ech_dly   = st.echo_delay;
                s_main_eg1_depth = st.filter_env_depth;
                s_main_eg1_decay = st.filter_env_decay;
                s_main_gld       = st.glide;
                if (ch) s_dirty = true;
            }
        }

        if (!s_main_in_card) {
            // Overview: encoder scrolls cards, short press enters card, long press → CARDS
            if (delta) {
                s_main_card = (s_main_card + delta % 6 + 6) % 6;
                s_dirty = true;
            }
            if (short_press) { s_main_in_card = true; s_main_param = 0; s_dirty = true; }
            if (long_press)  { amy_engine_save_state(); ui_enter(UI_CARDS); }
        } else {
            // Card edit: encoder adjusts current param, short press advances (last → save+overview)
            int p = s_main_param;
            if (delta) {
                switch (s_main_card) {
                case 0: // ENV
                    s_main_adsr[p] = u16clamp((int)s_main_adsr[p] + delta * 500);
                    amy_engine_set_adsr(s_main_adsr[0],s_main_adsr[1],s_main_adsr[2],s_main_adsr[3]);
                    break;
                case 1: // FLT
                    if (p == 0)      { s_main_flt_cut = u16clamp((int)s_main_flt_cut + delta*500); amy_engine_set_filter(s_main_flt_cut,s_main_flt_res); }
                    else if (p == 1) { s_main_flt_res = u16clamp((int)s_main_flt_res + delta*500); amy_engine_set_filter(s_main_flt_cut,s_main_flt_res); }
                    else             { s_main_flt_type = (uint8_t)((s_main_flt_type + (delta>0?1:2)) % 3); amy_engine_set_filter_type(s_main_flt_type); }
                    break;
                case 2: // EG1
                    if (p == 0) s_main_eg1_depth = u16clamp((int)s_main_eg1_depth + delta*500);
                    else        s_main_eg1_decay  = u16clamp((int)s_main_eg1_decay  + delta*500);
                    amy_engine_set_filter_env(s_main_eg1_depth, s_main_eg1_decay);
                    break;
                case 3: // LFO
                    if (p == 0) s_main_lfo_rate  = u16clamp((int)s_main_lfo_rate  + delta*500);
                    else        s_main_lfo_depth = u16clamp((int)s_main_lfo_depth + delta*500);
                    amy_engine_set_lfo(s_main_lfo_rate, s_main_lfo_depth);
                    break;
                case 4: // ECHO
                    if (p == 0)      { s_main_ech_amt = u16clamp((int)s_main_ech_amt + delta*500); amy_engine_set_echo(s_main_ech_amt,s_main_ech_fb); }
                    else if (p == 1) { s_main_ech_fb  = u16clamp((int)s_main_ech_fb  + delta*500); amy_engine_set_echo(s_main_ech_amt,s_main_ech_fb); }
                    else { // DLY: snap through fractions
                        int idx = echo_snap_idx(s_main_ech_dly) + (delta > 0 ? 1 : -1);
                        if (idx < 0) idx = 0;
                        if (idx > 5) idx = 5;
                        s_main_ech_dly = s_echo_delay_snaps[idx];
                        amy_engine_set_echo_delay(s_main_ech_dly);
                    }
                    break;
                case 5: // GLD
                    s_main_gld = u16clamp((int)s_main_gld + delta*500);
                    amy_engine_set_glide(s_main_gld);
                    break;
                }
                s_dirty = true;
            }
            if (short_press) {
                if (s_main_param + 1 >= s_card_param_max[s_main_card]) {
                    amy_engine_save_state();
                    s_main_in_card = false;
                    s_main_param   = 0;
                } else {
                    s_main_param++;
                }
                s_dirty = true;
            }
            if (long_press) {
                amy_engine_save_state();
                s_main_in_card = false;
                s_main_param   = 0;
                s_dirty        = true;
            }
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
    if (s_sec == UI_CALIB && s_cal_editing && !changed && (now - last_refresh) >= pdMS_TO_TICKS(50)) {
        s_dirty      = true;
        last_refresh = now;
    }
    if (s_dirty || changed) {
        last_refresh = now;
        s_dirty = false;
        ui_redraw();
    }
}
