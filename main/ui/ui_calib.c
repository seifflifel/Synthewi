// CALIBRATION screen — 10-pad threshold overview + single-pad live edit.
// Restyled 2026-11 for the redesign (same layout and information as before):
// sliding amber frames, edit-view header, parameter-style value columns, no
// footer hints. Redrawn every UI tick while visible (live RAW / VEL readout).

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "touch_telemetry.h"
#include "ui_internal.h"

static inline int ri(float v) { return (int)lrintf(v); }

static void text_c(const char *s, int cx, int y, uint16_t fg, uint16_t bg)
{
    tft_text(s, cx - ((int)strlen(s) * 6 - 1) / 2, y, fg, bg, 1);
}

// Live readout is part of the full redraw now; kept for ui.c's partial path.
void ui_draw_calib_live(void) { ui_draw_calib(); }

// ── Single-pad edit ───────────────────────────────────────────
static void draw_edit(void)
{
    uint32_t raw  = touch_telemetry_get_raw((uint8_t)ui_cal_pad);
    uint32_t base = touch_telemetry_get_baseline((uint8_t)ui_cal_pad);
    uint16_t thr  = ui_cal_thr;
    bool     note_pad = (ui_cal_pad < TOUCH_NOTE_PADS);
    uint16_t tmax = note_pad ? ui_cal_thr_max : 0;
    uint32_t dv = (raw >= base) ? (raw - base) : (base - raw);
    bool active = (thr > 0 && dv > (uint32_t)thr);

    // Header: dot + pad name, live velocity on the right
    char buf[24];
    if (note_pad) snprintf(buf, sizeof(buf), "PAD %d", ui_cal_pad + 1);
    else          snprintf(buf, sizeof(buf), ui_cal_pad == TOUCH_NOTE_PADS ? "OCT-" : "OCT+");
    tft_disc(5, 27, 2, C_ORANGE);
    tft_text(buf, 10, 24, C_WHITE, C_BLACK, 1);
    if (note_pad) {
        int vel = 0;
        if (active) {
            float f = (tmax > thr) ? (float)(dv - thr) / (float)(tmax - thr) : 1.0f;
            f = f > 1.0f ? 1.0f : f < 0.0f ? 0.0f : f;
            vel = (int)((TOUCH_VEL_MIN + (1.0f - TOUCH_VEL_MIN) * f) * 100.0f + 0.5f);
        }
        if (vel) snprintf(buf, sizeof(buf), "VEL %3d%%", vel);
        else     snprintf(buf, sizeof(buf), "VEL  ---");
        tft_text(buf, 104, 24, vel ? C_GREEN : C_LTGRAY, C_BLACK, 1);
    }

    // Big bar in the icon frame: touch delta (green while above MIN), MIN marker
    // (orange) and MAX marker (blue, note pads). Fixed scale = base/8 as before.
    tft_rrect(1, 33, 158, 92, C_GRID);
    const int bx0 = 6, bx1 = 153, by0 = 50, by1 = 76, blen = bx1 - bx0;
    uint32_t scale = base / 8;
    if (scale < 2000) scale = 2000;
    if (note_pad && scale < (uint32_t)tmax * 5 / 4) scale = (uint32_t)tmax * 5 / 4;
    int raw_x = (int)(dv * blen / scale); if (raw_x > blen) raw_x = blen;
    tft_rect(bx0, by0, bx1, by1, C_GRID);
    if (raw_x > 0) tft_rect(bx0, by0, bx0 + raw_x, by1, active ? C_GREEN : C_LTGRAY);

    static float mn, mnt, mx, mxt;     // markers slide to new positions
    int tx = bx0 + (int)((uint32_t)thr * blen / scale);
    mn = ui_tween(mn, (float)(tx > bx1 ? bx1 : tx), &mnt);
    tft_rect(ri(mn), by0 - 5, ri(mn) + 1, by1 + 5, C_ORANGE);
    if (note_pad) {
        int m = bx0 + (int)((uint32_t)tmax * blen / scale);
        mx = ui_tween(mx, (float)(m > bx1 ? bx1 : m), &mxt);
        tft_rect(ri(mx), by0 - 5, ri(mx) + 1, by1 + 5, C_BLUE);
    }

    // Value columns: RAW | BASE | MIN (| MAX). The value being edited sits in a
    // filled frame of its marker colour; the frame slides MIN -> MAX.
    int n = note_pad ? 4 : 3;
    float cw = 160.0f / (float)n;
    int ed = 2 + ui_cal_stage;
    uint16_t edc = (ui_cal_stage == 0) ? C_ORANGE : C_BLUE;
    static float fr[4], ft[4];
    int t[4] = {ri(ed * cw) + 1, 95, ri((ed + 1) * cw) - 2, 123};
    for (int k = 0; k < 4; k++) fr[k] = ui_tween(fr[k], (float)t[k], &ft[k]);
    tft_rrect_fill(ri(fr[0]), ri(fr[1]), ri(fr[2]), ri(fr[3]), edc);

    const char *lbl[4] = {"RAW", "BASE", note_pad ? "MIN" : "THR", "MAX"};
    uint32_t val[4] = {raw, base, thr, tmax};
    uint16_t lc[4] = {C_LTGRAY, C_LTGRAY, C_ORANGE, C_BLUE};
    for (int i = 0; i < n; i++) {
        int cx = (ri(i * cw) + 1 + ri((i + 1) * cw) - 2) / 2;
        bool e = (i == ed);
        snprintf(buf, sizeof(buf), "%" PRIu32, val[i]);
        text_c(lbl[i], cx, 101, e ? C_BLACK : lc[i], e ? edc : C_BLACK);
        text_c(buf, cx, 112, e ? C_BLACK : C_DKGRAY, e ? edc : C_BLACK);
    }
}

// ── Overview: 10 bars (stored threshold), sliding selection frame ──
static void draw_overview(void)
{
    const int bar_w = 12, bar_gap = 2;
    const int x_start = (TFT_W - (TOUCH_TOTAL_PADS * (bar_w + bar_gap) - bar_gap)) / 2;
    const int y_top = 28, y_bot = 108, bar_h = y_bot - y_top;

    uint16_t max_thr = 1;
    for (int p = 0; p < TOUCH_TOTAL_PADS; p++) {
        uint16_t t = touch_telemetry_get_threshold((uint8_t)p);
        if (t > max_thr) max_thr = t;
    }
    for (int p = 0; p < TOUCH_TOTAL_PADS; p++) {
        int bx = x_start + p * (bar_w + bar_gap);
        bool sel = (p == ui_cal_pad);
        uint16_t thr = touch_telemetry_get_threshold((uint8_t)p);
        tft_line_dash(bx + bar_w / 2, y_top, bx + bar_w / 2, y_bot, 2, C_GRID);
        int fh = (int)((uint32_t)thr * bar_h / max_thr);
        if (fh > 2) tft_rrect_fill(bx + 1, y_bot - fh, bx + bar_w - 2, y_bot, sel ? C_ORANGE : C_LTGRAY);
        char l[2] = {(p < TOUCH_NOTE_PADS) ? (char)('1' + p) : (p == TOUCH_NOTE_PADS ? '-' : '+'), 0};
        tft_text(l, bx + (bar_w - 6) / 2, y_bot + 3, sel ? C_ORANGE : C_DKGRAY, C_BLACK, 1);
    }
    static float fr[4], ft[4];
    int sx = x_start + ui_cal_pad * (bar_w + bar_gap);
    int t[4] = {sx - 2, y_top - 3, sx + bar_w + 1, y_bot + 12};
    for (int k = 0; k < 4; k++) fr[k] = ui_tween(fr[k], (float)t[k], &ft[k]);
    tft_rrect(ri(fr[0]), ri(fr[1]), ri(fr[2]), ri(fr[3]), C_ORANGE);
    text_c("AUTO CAL: SOON", 80, 122, C_GRID, C_BLACK);
}

void ui_draw_calib(void)
{
    tft_rect(0, 22, TFT_W - 1, TFT_H - 1, C_BLACK);
    if (ui_cal_editing) draw_edit();
    else                draw_overview();
}
