// PRESETS screen (redesign 2026-11): 6 tiles like the PARAMS grid (slot, wave
// glyph, name), a sliding selection frame, and the LOAD / SAVE / CLR menu as a
// centered pop-up box (grows open, focus frame slides between the actions).
// The loaded preset's dot is filled. Results show as a message pop-up (ui.c).

#include <math.h>
#include <string.h>
#include <stdio.h>
#include "presets.h"
#include "ui_internal.h"

int ui_prst_loaded = -1;      // last loaded slot (dot filled), -1 = none

static const int s_gx[3][2] = {{1, 52}, {54, 105}, {107, 158}};
static const int s_gy[2][2] = {{23, 74}, {76, 127}};
static const char * const s_act[3] = {"LOAD", "SAVE", "CLR"};

void ui_presets_tile_rect(int i, int r[4])
{
    r[0] = s_gx[i % 3][0]; r[1] = s_gy[i / 3][0]; r[2] = s_gx[i % 3][1]; r[3] = s_gy[i / 3][1];
}

static inline int ri(float v) { return (int)lrintf(v); }

static void text_c(const char *s, int cx, int y, uint16_t fg, uint16_t bg)
{
    tft_text(s, cx - ((int)strlen(s) * 6 - 1) / 2, y, fg, bg, 1);
}

static void draw_menu(float p)
{
    float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
    int hw = ri(56 * e), hh = ri(26 * e), cx = 80, cy = 66;
    tft_rrect_fill(cx - hw, cy - hh, cx + hw, cy + hh, C_BLACK);
    tft_rrect(cx - hw, cy - hh, cx + hw, cy + hh, C_ORANGE);
    if (hw > 4) tft_rrect(cx - hw + 1, cy - hh + 1, cx + hw - 1, cy + hh - 1, C_ORANGE);
    if (p < 0.9f) return;

    int slot = ui_prst_cur;
    char title[24];
    const char *nm = preset_is_valid(slot) ? preset_get_name(slot) : "";
    if (nm[0]) snprintf(title, sizeof(title), "P%d %s", slot + 1, nm);
    else       snprintf(title, sizeof(title), "P%d", slot + 1);
    text_c(title, cx, 46, C_LTGRAY, C_BLACK);

    static float fr[4], ft[4];
    int t[4] = {cx - 51 + ui_prst_act * 34, 62, cx - 51 + ui_prst_act * 34 + 32, 78};
    for (int k = 0; k < 4; k++) fr[k] = ui_tween(fr[k], (float)t[k], &ft[k]);
    tft_rrect(ri(fr[0]), ri(fr[1]), ri(fr[2]), ri(fr[3]), C_ORANGE);
    for (int a = 0; a < 3; a++) {
        bool avail = (a == 1) || preset_is_valid(slot);
        bool sel = (a == ui_prst_act);
        text_c(s_act[a], cx - 35 + a * 34, 66, !avail ? C_GRID : sel ? C_WHITE : C_DKGRAY, C_BLACK);
    }
}

void ui_draw_presets(void)
{
    tft_rect(0, 22, TFT_W - 1, TFT_H - 1, C_BLACK);

    for (int i = 0; i < PRESET_COUNT; i++) {
        int r[4];
        ui_presets_tile_rect(i, r);
        bool sel = (i == ui_prst_cur), valid = preset_is_valid(i);
        uint16_t c = sel ? C_ORANGE : C_LTGRAY;
        int cx = (r[0] + r[2]) / 2;
        tft_rrect(r[0], r[1], r[2], r[3], C_LTGRAY);
        if (i == ui_prst_loaded) tft_disc(r[0] + 5, r[1] + 5, 2, c);
        else                     tft_ring(r[0] + 5, r[1] + 5, 2, c);
        char hdr[4];
        snprintf(hdr, sizeof(hdr), "P%d", i + 1);
        tft_text(hdr, r[0] + 10, r[1] + 2, sel ? C_WHITE : C_DKGRAY, C_BLACK, 1);
        if (valid) {
            const preset_t *p = preset_get(i);
            ui_draw_wave_glyph(p->wave_id, cx - 10, r[1] + 16, cx + 9, r[1] + 27, c);
            const char *nm = preset_get_name(i);
            text_c(nm[0] ? nm : "USER", cx, r[1] + 36, sel ? C_WHITE : C_DKGRAY, C_BLACK);
        } else {
            text_c("EMPTY", cx, r[1] + 26, C_GRID, C_BLACK);
        }
    }

    static float fr[4], ft[4];
    int t[4];
    ui_presets_tile_rect(ui_prst_cur, t);
    for (int k = 0; k < 4; k++) fr[k] = ui_tween(fr[k], (float)t[k], &ft[k]);
    tft_rrect(ri(fr[0]), ri(fr[1]), ri(fr[2]), ri(fr[3]), C_ORANGE);

    static float mp, mt;
    mp = ui_tween_eps(mp, ui_prst_action ? 1.0f : 0.0f, &mt, 0.02f);
    if (mp > 0.02f) draw_menu(mp);
}

