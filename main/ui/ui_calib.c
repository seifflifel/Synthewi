// CALIBRATION screen — 10-pad threshold overview + single-pad live edit.

#include <stdio.h>
#include <inttypes.h>
#include "touch_telemetry.h"
#include "ui_internal.h"

// CAL edit live refresh: redraw only bar + numbers, not the whole screen.
static int  s_cal_last_tx = -1;   // x of MIN threshold marker currently drawn (-1 = none)
static int  s_cal_last_mx = -1;   // x of MAX (velocity) marker currently drawn (-1 = none)

// ---- CALIBRATION --------------------------------------------
// Overview: 10 live bar columns (50 ms refresh), encoder selects pad, short press → edit.
// Edit:     full-screen single-pad with RAW/BASE/THR and horizontal bar (preserved).
// Live part of the single-pad edit screen: delta bar, threshold marker, numbers.
// Called on its own every 50 ms (and on encoder turns), so it must never clear
// a region it is about to repaint — every pixel is painted exactly once per call.
void ui_draw_calib_live(void)
{
    uint32_t raw  = touch_telemetry_get_raw((uint8_t)ui_cal_pad);
    uint32_t base = touch_telemetry_get_baseline((uint8_t)ui_cal_pad);
    uint16_t thr  = ui_cal_thr;
    bool     note_pad = (ui_cal_pad < TOUCH_NOTE_PADS);
    uint16_t tmax = note_pad ? ui_cal_thr_max : 0;

    // Large horizontal bar (x=4..155, y=54..88, 34px tall)
    int bx0=4, bx1=TFT_W-5, by0=54, by1=88, blen=bx1-bx0;
    // Use delta from baseline (not absolute raw) — absolute values are ~60000+
    uint32_t dv = (raw >= base) ? (raw - base) : (base - raw);
    // Fixed scale = base/8 so threshold marker position reflects actual thr value
    uint32_t scale = base / 8;
    if (scale < 2000) scale = 2000;
    if (note_pad && scale < (uint32_t)tmax * 5 / 4) scale = (uint32_t)tmax * 5 / 4;  // keep MAX on the bar
    int raw_x = (int)(dv  * blen / scale); if (raw_x > blen) raw_x = blen;
    int thr_x = (int)((uint32_t)thr * blen / scale); if (thr_x > blen) thr_x = blen;
    int tx = bx0 + thr_x;
    int mx = -1;
    if (note_pad) {
        int max_x = (int)((uint32_t)tmax * blen / scale); if (max_x > blen) max_x = blen;
        mx = bx0 + max_x;
    }

    // Marker moved → erase the old one first (its tails stick out above/below
    // the track, and at x=bx1+1 it sits outside the track entirely).
    if (s_cal_last_tx >= 0 && s_cal_last_tx != tx)
        tft_fill(s_cal_last_tx, by0-4, s_cal_last_tx+1, by1+4, C_BLACK);
    if (s_cal_last_mx >= 0 && s_cal_last_mx != mx)
        tft_fill(s_cal_last_mx, by0-4, s_cal_last_mx+1, by1+4, C_BLACK);

    // Bar as two non-overlapping segments: delta fill, then remaining track.
    bool active_touch = (thr > 0 && dv > (uint32_t)thr);
    int split = bx0;
    if (raw_x > 0) {
        tft_fill(bx0, by0, bx0+raw_x, by1, active_touch ? C_GREEN : C_DKGRAY);
        split = bx0 + raw_x + 1;
    }
    if (split <= bx1)
        tft_fill(split, by0, bx1, by1, C_LTGRAY);

    // MIN marker: orange 2px vertical line (trigger point, 10 % velocity)
    tft_fill(tx, by0-4, tx+1, by1+4, C_ORANGE);
    s_cal_last_tx = tx;
    // MAX marker: blue 2px vertical line (100 % velocity), note pads only
    if (note_pad) tft_fill(mx, by0-4, mx+1, by1+4, C_BLUE);
    s_cal_last_mx = mx;

    // Values — fixed-width fields; tft_text paints its background over old digits.
    // The value being edited is shown inverted.
    char buf[20];
    snprintf(buf, sizeof(buf), "RAW  %-7" PRIu32, raw);
    tft_text(buf, 4, 96, C_WHITE, C_BLACK, 1);
    snprintf(buf, sizeof(buf), "BASE %-7" PRIu32, base);
    tft_text(buf, 4, 106, C_LTGRAY, C_BLACK, 1);
    bool ed_min = (ui_cal_stage == 0), ed_max = (ui_cal_stage == 1);
    snprintf(buf, sizeof(buf), note_pad ? "MIN  %5u" : "THR  %5u", (unsigned)thr);
    tft_text(buf, 86, 96, ed_min ? C_BLACK : C_ORANGE, ed_min ? C_ORANGE : C_BLACK, 1);
    if (note_pad) {
        snprintf(buf, sizeof(buf), "MAX  %5u", (unsigned)tmax);
        tft_text(buf, 86, 106, ed_max ? C_BLACK : C_BLUE, ed_max ? C_BLUE : C_BLACK, 1);

        // Live velocity this touch would play at (uses the values being edited)
        int vel = 0;
        if (active_touch) {
            float f = (tmax > thr) ? (float)(dv - thr) / (float)(tmax - thr) : 1.0f;
            if (f > 1.0f) f = 1.0f;
            if (f < 0.0f) f = 0.0f;
            vel = (int)((TOUCH_VEL_MIN + (1.0f - TOUCH_VEL_MIN) * f) * 100.0f + 0.5f);
        }
        if (vel) snprintf(buf, sizeof(buf), "VEL %3d%%", vel);
        else     snprintf(buf, sizeof(buf), "VEL  ---");
        tft_text(buf, 104, 30, vel ? C_GREEN : C_LTGRAY, C_BLACK, 1);
    }
}

void ui_draw_calib(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    if (ui_cal_editing) {
        // ── Single-pad edit ─────────────────────────────────────
        char phdr[12];
        if (ui_cal_pad < TOUCH_NOTE_PADS)
            snprintf(phdr, sizeof(phdr), "PAD %u", (uint8_t)(ui_cal_pad+1));
        else
            snprintf(phdr, sizeof(phdr), ui_cal_pad == TOUCH_NOTE_PADS ? "OCT-" : "OCT+");
        tft_text_bold(phdr, 4, 24, C_ORANGE, C_BLACK, 2);
        tft_fill(0, 42, TFT_W-1, 43, C_LTGRAY);

        s_cal_last_tx = -1;      // screen was just cleared — no markers on it
        s_cal_last_mx = -1;
        ui_draw_calib_live();

        tft_fill(0, 117, TFT_W-1, 118, C_LTGRAY);
        const char *hint = (ui_cal_pad >= TOUCH_NOTE_PADS) ? "ROT:THR  BTN:SAVE  HLD:BACK"
                         : (ui_cal_stage == 0)              ? "ROT:MIN  BTN:NEXT  HLD:BACK"
                                                            : "ROT:MAX  BTN:SAVE  HLD:BACK";
        tft_text(hint, 2, 121, C_LTGRAY, C_BLACK, 1);
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
        bool sel = (p == ui_cal_pad);
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
