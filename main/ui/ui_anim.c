// UI motion: screen transitions and the volume pop-up (redesign 2026-11).
//
// Both use the same "box" animation (approved in docs/ui_sketch.html):
//   - entering a screen: the selected tile's frame grows to fill the body;
//   - going back: a full-body box shrinks into the tile you came from;
//   - volume: a centered box grows open while you hold + turn, shrinks on release.
// Progress is time based (esp_timer), so it looks the same at any tick rate:
// p = 1 - exp(-t / TAU), eased again with 1 - (1 - p)^3 for the box size.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "ui.h"
#include "ui_internal.h"

#define ANIM_TAU_S   0.065f   // ≈ the sketch's 0.4-per-frame tween at 30 fps
#define ANIM_DONE_P  0.97f

const int UI_BODY_RECT[4] = {1, 23, 158, 127};

// Seconds, wrapped every hour so a float keeps ms precision. Used for icon phases
// and tweens (which treat a negative / huge dt as "snap"); transitions use int64.
float ui_now_s(void) { return (float)((esp_timer_get_time() / 1000) % 3600000LL) / 1000.0f; }

// Advance an exponential tween toward `target` over the time since *last_s.
float ui_tween(float cur, float target, float *last_s)
{
    float now = ui_now_s(), dt = now - *last_s;
    *last_s = now;
    if (dt < 0.0f || dt > 0.5f) return target;          // first use / long gap: snap
    float k = 1.0f - expf(-dt / ANIM_TAU_S);
    float v = cur + (target - cur) * k;
    return fabsf(target - v) < 0.3f ? target : v;
}

// Double amber frame (the pop-up's look), filled with the background.
static void anim_box(const int r[4])
{
    tft_rrect_fill(r[0], r[1], r[2], r[3], C_BLACK);
    tft_rrect(r[0], r[1], r[2], r[3], C_ORANGE);
    if (r[2] - r[0] > 8 && r[3] - r[1] > 8) tft_rrect(r[0] + 1, r[1] + 1, r[2] - 1, r[3] - 1, C_ORANGE);
}

// ── Screen transition ─────────────────────────────────────────
static struct {
    bool  active;
    int   from[4], to[4];
    int   tag;              // returned by ui_trans_step() when finished (-1 = none)
    int64_t t0_us;
} s_tr;

void ui_trans_start(const int from[4], const int to[4], int tag)
{
    memcpy(s_tr.from, from, sizeof(s_tr.from));
    memcpy(s_tr.to, to, sizeof(s_tr.to));
    s_tr.tag = tag;
    s_tr.t0_us = esp_timer_get_time();
    s_tr.active = true;
}

bool ui_trans_active(void) { return s_tr.active; }

static float trans_p(void) { return 1.0f - expf(-(float)(esp_timer_get_time() - s_tr.t0_us) / 1e6f / ANIM_TAU_S); }

int ui_trans_step(void)
{
    if (!s_tr.active || trans_p() < ANIM_DONE_P) return -1;
    s_tr.active = false;
    return s_tr.tag;
}

void ui_trans_draw(void)
{
    if (!s_tr.active) return;
    float p = trans_p(), e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
    int r[4];
    for (int i = 0; i < 4; i++) r[i] = (int)lrintf(s_tr.from[i] + (s_tr.to[i] - s_tr.from[i]) * e);
    anim_box(r);
}

// ── Volume pop-up ─────────────────────────────────────────────
static volatile uint8_t s_vol_pct = 100;
static volatile bool    s_vol_open = false;
static float s_vol_p = 0.0f, s_vol_p_last = 0.0f;     // open amount 0..1 (UI task)
static float s_vol_disp = -1.0f, s_vol_disp_last = 0.0f;

void ui_volume_popup(uint8_t percent, bool active)
{
    s_vol_pct = percent > 100 ? 100 : percent;
    s_vol_open = active;
}

bool ui_popup_active(void) { return s_vol_open || s_vol_p > 0.02f; }

void ui_popup_draw(void)
{
    // Tweens are advanced here (called once per redraw while active).
    float now = ui_now_s(), dt = now - s_vol_p_last;
    s_vol_p_last = now;
    float target = s_vol_open ? 1.0f : 0.0f;
    if (dt < 0.0f || dt > 0.5f) dt = 0.02f;
    s_vol_p += (target - s_vol_p) * (1.0f - expf(-dt / ANIM_TAU_S));
    if (!s_vol_open && s_vol_p < 0.02f) { s_vol_p = 0.0f; s_vol_disp = -1.0f; return; }

    float vt = (float)s_vol_pct;
    if (s_vol_disp < 0.0f) { s_vol_disp = vt; s_vol_disp_last = now; }
    else s_vol_disp = ui_tween(s_vol_disp, vt, &s_vol_disp_last);

    float p = s_vol_p, e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
    int hw = (int)lrintf(52 * e), hh = (int)lrintf(25 * e), cx = 80, cy = 64;
    int r[4] = {cx - hw, cy - hh, cx + hw, cy + hh};
    anim_box(r);
    if (p < 0.9f) return;

    float v = s_vol_disp / 100.0f;
    tft_text("VOLUME", 35, 45, C_LTGRAY, C_BLACK, 1);
    // Speaker, vertically centred between the label (ends y52) and the bar (starts y75)
    const int sy = 64;
    tft_rect(35, sy - 3, 38, sy + 3, C_WHITE);
    for (int i = 0; i <= 4; i++) tft_rect(39 + i, sy - 3 - i, 39 + i, sy + 3 + i, C_WHITE);
    int lit = s_vol_pct > 5 ? (s_vol_pct > 40 ? (s_vol_pct > 75 ? 3 : 2) : 1) : 0;
    for (int k = 0; k < 3; k++) tft_arc(43, sy, 4 + k * 3, -0.75f, 0.75f, k < lit ? C_ORANGE : C_GRID);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)s_vol_pct);
    int tw = (int)strlen(buf) * 18 - 3;
    tft_text(buf, 125 - tw, 47, C_WHITE, C_BLACK, 3);
    tft_rrect(34, 75, 125, 82, C_LTGRAY);
    tft_rect(36, 77, 36 + (int)lrintf(v * 87), 80, C_ORANGE);
}

bool ui_anim_busy(void) { return s_tr.active || ui_popup_active(); }
