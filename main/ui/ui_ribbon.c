// Ribbon bar (y=0..21): pads, waveform, octave, looper dot — full + partial
// redraw. Also the ribbon-state setters of the public API.

#include <stdio.h>
#include "esp_timer.h"
#include "touch_telemetry.h"
#include "looper.h"
#include "ui.h"
#include "ui_internal.h"

// Ribbon state (written by the touch task via on_touch, read by the UI task)
// UI's copy of the octave shift — main.c owns the value and pushes it via ui_set_octave().
static volatile int  s_octave_shift = 0;

// Octave-limit flash: when an octave pad is pressed at ±2 (nothing can change),
// the label blinks red twice so the press doesn't feel ignored.
// Set from the touch task (timestamp only); drawn by the UI task.
#define OCT_FLASH_US 400000LL
static volatile int64_t s_oct_flash_start = 0;   // 0 = no flash running
static bool             s_rb_oct_red      = false;
static volatile bool s_pad_active[TOUCH_NOTE_PADS] = {false};

// Ribbon cache — what is currently on screen, so ui_ribbon_update() redraws only what changed.
static bool           s_rb_valid = false;
static bool           s_rb_pad[TOUCH_NOTE_PADS];
static uint8_t        s_rb_wave;
static int            s_rb_oct;
static looper_state_t s_rb_loop;
static int            s_rb_sep = -1;   // separator key (home flag + loop progress pixels)

// Draws a waveform pixel-art glyph inside box (x0,y0)-(x1,y1).
// Reusable for both the menu cell and the future ribbon bar.
void ui_draw_wave_glyph(uint8_t wave_id, int x0, int y0, int x1, int y1, uint16_t fg)
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

// ---- RIBBON BAR (y=0..21) -----------------------------------
// 8 pad circles + waveform glyph + octave label + looper dot + separator.
// Each element has its own draw helper so ui_ribbon_update() can repaint
// just the element that changed (no full clear → no flicker while playing).

// Pad circles: centers at x=6+i*10, y=10 — radius 3. Each circle paints its
// whole footprint (filled or ring + hollow), so no clear is needed.
static void ribbon_draw_pad(int i, bool active)
{
    tft_pad_circle(6 + i * 10, 10, active);
    s_rb_pad[i] = active;
}

// Waveform glyph (x=90..110, y=3..17) — orange
static void ribbon_draw_wave(uint8_t wave_id)
{
    tft_fill(90, 3, 110, 17, C_BLACK);
    ui_draw_wave_glyph(wave_id, 90, 3, 110, 17, C_ORANGE);
    s_rb_wave = wave_id;
}

// Octave label — yellow when shifted, dim gray at zero. Always 5 chars wide,
// and tft_text paints its background, so it overwrites the previous label.
static void ribbon_draw_oct(int shift, bool red)
{
    char oct_buf[8];
    if (shift == 0)
        snprintf(oct_buf, sizeof(oct_buf), "OCT 0");
    else
        snprintf(oct_buf, sizeof(oct_buf), "OCT%+d", shift);
    uint16_t fg = red ? C_RED : (shift ? C_BLUE : C_DKGRAY);
    tft_text(oct_buf, 116, 6, fg, C_BLACK, 1);
    s_rb_oct     = shift;
    s_rb_oct_red = red;
}

// Is the octave-limit flash currently in a red phase? (two 130 ms pulses)
static bool oct_flash_red(void)
{
    int64_t start = s_oct_flash_start;
    if (start == 0) return false;
    int64_t e = esp_timer_get_time() - start;
    if (e >= OCT_FLASH_US) { s_oct_flash_start = 0; return false; }
    return (e < 130000) || (e >= 200000 && e < 330000);
}

// Looper dot (5×5, top-right)
static void ribbon_draw_loop(looper_state_t ls)
{
    uint16_t lc = (ls == LOOPER_RECORDING) ? C_RED    :
                  (ls == LOOPER_PLAYING)   ? C_GREEN  :
                  (ls == LOOPER_OVERDUB)   ? C_BLUE   : C_BLACK;
    tft_fill(154, 5, 158, 9, lc);
    s_rb_loop = ls;
}

// Separator (y=20..21). Off the HOME screen it doubles as a thin loop progress
// line: recorded part in the loop colour, playhead dark. Returns a key that
// changes whenever the drawing would.
static int ribbon_sep_key(void)
{
    if (ui_on_home) return 0;
    looper_progress_t lp;
    looper_get_progress(&lp);
    if (lp.state == LOOPER_IDLE || lp.max_length == 0) return 0;
    uint32_t len = (lp.state == LOOPER_RECORDING) ? lp.position : lp.length;
    int lf = (int)((uint64_t)len * (TFT_W - 1) / lp.max_length);
    int hf = (int)((uint64_t)lp.position * (TFT_W - 1) / lp.max_length);
    return 1 + (int)lp.state * 100000 + lf * 256 + hf;
}
static void ribbon_draw_sep(void)
{
    s_rb_sep = ribbon_sep_key();
    tft_fill(0, 20, TFT_W-1, 21, C_DKGRAY);
    if (s_rb_sep == 0) return;
    int st = (s_rb_sep - 1) / 100000, rest = (s_rb_sep - 1) % 100000;
    int lf = rest / 256, hf = rest % 256;
    uint16_t col = st == LOOPER_RECORDING ? C_RED : st == LOOPER_PLAYING ? C_GREEN : C_BLUE;
    tft_rect(0, 20, lf, 21, col);
    tft_rect(hf, 20, hf + 1, 21, C_WHITE);
}

// Full ribbon repaint — used on section changes (called last by ui_redraw).
void ui_draw_ribbon(void)
{
    tft_fill(0, 0, TFT_W-1, 19, C_BLACK);
    for (int i = 0; i < TOUCH_NOTE_PADS; i++)
        ribbon_draw_pad(i, s_pad_active[i]);
    amy_engine_state_t st;
    amy_engine_get_state(&st);
    ribbon_draw_wave(st.wave_id);
    ribbon_draw_oct(s_octave_shift, oct_flash_red());
    ribbon_draw_loop(looper_get_state());
    ribbon_draw_sep();
    s_rb_valid = true;
}

// Partial ribbon refresh — compares live state against what is on screen and
// repaints only the elements that differ. Cheap enough to run every UI tick.
// Polling (instead of flags) also catches changes nobody signals, e.g. the
// looper auto-switching REC → PLAY when its buffer fills.
void ui_ribbon_update(void)
{
    if (!s_rb_valid) { ui_draw_ribbon(); return; }

    for (int i = 0; i < TOUCH_NOTE_PADS; i++) {
        bool a = s_pad_active[i]; // read once — touch task may change it concurrently
        if (a != s_rb_pad[i]) ribbon_draw_pad(i, a);
    }

    amy_engine_state_t st;
    amy_engine_get_state(&st);
    if (st.wave_id != s_rb_wave) ribbon_draw_wave(st.wave_id);

    int  oct = s_octave_shift;
    bool red = oct_flash_red();
    if (oct != s_rb_oct || red != s_rb_oct_red) ribbon_draw_oct(oct, red);

    looper_state_t ls = looper_get_state();
    if (ls != s_rb_loop) ribbon_draw_loop(ls);

    if (ribbon_sep_key() != s_rb_sep) ribbon_draw_sep();
}

// ── Public API ────────────────────────────────────────────────

// Ribbon-only changes: no s_dirty here. ui_ribbon_update() (every UI tick)
// notices the new state and repaints just that element. The main task owns
// SPI — the touch task must never call tft_* directly.
void ui_notify_lever(void) { }

void ui_set_octave(int shift) { s_octave_shift = shift; }

void ui_octave_limit_hit(void) { s_oct_flash_start = esp_timer_get_time(); }

void ui_set_pad_active(uint8_t pad, bool active)
{
    if (pad >= TOUCH_NOTE_PADS) return;
    s_pad_active[pad] = active;
}
