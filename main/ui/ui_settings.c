// SETTINGS screen (redesign 2026-11): CALIBRATE (opens the CAL screen) and
// THEME (light / dark). Dark = the panel's own colour inversion (ST7735 INVON),
// i.e. a true inversion of every colour, persisted in NVS "ui"/"dark".

#include <math.h>
#include <string.h>
#include "nvs.h"
#include "scale.h"
#include "ui_internal.h"

int  ui_set_cur = 0;          // focused row
bool ui_set_editing = false;  // KEY / SCALE: turning changes the value
bool ui_dark    = false;
static uint8_t s_orig_key, s_orig_type;   // restored when the picker is cancelled

// KEY / SCALE picker pop-up: turning applies at once (audible), click keeps it,
// long press restores the value from before.
void ui_settings_picker_open(void)
{
    s_orig_key = scale_key(); s_orig_type = scale_type();
    ui_set_editing = true;
}
void ui_settings_picker_close(bool keep)
{
    if (keep) scale_save(); else scale_set(s_orig_key, s_orig_type);
    ui_set_editing = false;
}

// Generic picker box: title + grid of items, sliding frame on the selection.
static void draw_picker(float p, const char *title, const char * const *items, int n,
                        int cols, int cell_w, int cell_h, int sel, int hw, int hh)
{
    float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
    int w = (int)lrintf(hw * e), h = (int)lrintf(hh * e), cx = 80, cy = 76;
    tft_rrect_fill(cx - w, cy - h, cx + w, cy + h, C_BLACK);
    tft_rrect(cx - w, cy - h, cx + w, cy + h, C_ORANGE);
    if (w > 4) tft_rrect(cx - w + 1, cy - h + 1, cx + w - 1, cy + h - 1, C_ORANGE);
    if (p < 0.9f) return;

    tft_text(title, cx - ((int)strlen(title) * 6 - 1) / 2, cy - hh + 4, C_LTGRAY, C_BLACK, 1);
    int rows = (n + cols - 1) / cols;
    int gx = cx - cols * cell_w / 2, gy = cy - hh + 15 + ((2 * hh - 19) - rows * cell_h) / 2;
    static float fr[4], ft[4];
    int sx = gx + (sel % cols) * cell_w, sy = gy + (sel / cols) * cell_h;
    int t[4] = {sx + 1, sy, sx + cell_w - 2, sy + cell_h - 1};
    for (int k = 0; k < 4; k++) fr[k] = ui_tween(fr[k], (float)t[k], &ft[k]);
    tft_rrect((int)lrintf(fr[0]), (int)lrintf(fr[1]), (int)lrintf(fr[2]), (int)lrintf(fr[3]), C_ORANGE);
    for (int i = 0; i < n; i++) {
        int x = gx + (i % cols) * cell_w + cell_w / 2, y = sy - (sel / cols) * cell_h + (i / cols) * cell_h + (cell_h - 8) / 2 + 1;
        tft_text(items[i], x - ((int)strlen(items[i]) * 6 - 1) / 2, y, i == sel ? C_WHITE : C_DKGRAY, C_BLACK, 1);
    }
}

static const char * const s_rows[UI_SET_ROWS] = {"CALIBRATE", "KEY", "SCALE", "THEME"};

void ui_settings_row_rect(int i, int r[4])
{
    r[0] = 2; r[1] = 26 + i * 17; r[2] = 157; r[3] = 40 + i * 17;
}

void ui_theme_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "dark", &v); nvs_close(h); }
    ui_dark = (v != 0);
    tft_set_invert(ui_dark);
}

void ui_theme_toggle(void)
{
    ui_dark = !ui_dark;
    tft_set_invert(ui_dark);
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "dark", ui_dark ? 1 : 0); nvs_commit(h); nvs_close(h); }
}

void ui_draw_settings(void)
{
    tft_rect(0, 22, TFT_W - 1, TFT_H - 1, C_BLACK);

    static float fr[4], ft[4], knob = -1.0f, kt;
    int t[4];
    ui_settings_row_rect(ui_set_cur, t);
    for (int k = 0; k < 4; k++) fr[k] = ui_tween(fr[k], (float)t[k], &ft[k]);
    tft_rrect((int)lrintf(fr[0]), (int)lrintf(fr[1]), (int)lrintf(fr[2]), (int)lrintf(fr[3]), C_ORANGE);

    for (int i = 0; i < UI_SET_ROWS; i++) {
        int y = 26 + i * 17;
        bool sel = (i == ui_set_cur);
        tft_text(s_rows[i], 7, y + 4, sel ? C_WHITE : C_DKGRAY, C_BLACK, 1);
        if (i == 0) {
            tft_text(">", 147, y + 4, sel ? C_ORANGE : C_LTGRAY, C_BLACK, 1);
        } else if (i == 1 || i == 2) {
            const char *v = (i == 1) ? scale_key_name(scale_key()) : scale_type_name(scale_type());
            tft_text(v, 147 - ((int)strlen(v) * 6 - 1) - 4, y + 4, sel ? C_ORANGE : C_LTGRAY, C_BLACK, 1);
            tft_text(">", 147, y + 4, sel ? C_ORANGE : C_LTGRAY, C_BLACK, 1);
        } else {
            // Toggle: the knob slides; the track fills amber when DARK.
            if (knob < 0.0f) knob = ui_dark ? 1.0f : 0.0f;
            knob = ui_tween_eps(knob, ui_dark ? 1.0f : 0.0f, &kt, 0.01f);
            const char *lbl = ui_dark ? "DARK" : "LIGHT";
            tft_text(lbl, 122 - ((int)strlen(lbl) * 6 - 1), y + 4, sel ? C_ORANGE : C_LTGRAY, C_BLACK, 1);
            if (knob > 0.5f) tft_rrect_fill(126, y + 3, 152, y + 11, C_ORANGE);
            else             tft_rrect(126, y + 3, 152, y + 11, C_LTGRAY);
            tft_disc(132 + (int)lrintf(knob * 15.0f), y + 7, 3, knob > 0.5f ? C_BLACK : C_LTGRAY);
        }
    }

    // KEY / SCALE picker (grows open like the other pop-ups)
    static float pp, pt;
    static int which = 1;
    if (ui_set_editing) which = ui_set_cur;
    pp = ui_tween_eps(pp, ui_set_editing ? 1.0f : 0.0f, &pt, 0.02f);
    if (pp > 0.02f) {
        static const char *keys[SCALE_KEYS], *types[SCALE_TYPES];
        for (int i = 0; i < SCALE_KEYS; i++)  keys[i]  = scale_key_name((uint8_t)i);
        for (int i = 0; i < SCALE_TYPES; i++) types[i] = scale_type_name((uint8_t)i);
        if (which == 1) draw_picker(pp, "KEY", keys, SCALE_KEYS, 6, 18, 13, scale_key(), 60, 27);
        else            draw_picker(pp, "SCALE", types, SCALE_TYPES, 2, 64, 11, scale_type(), 68, 34);
    }
}
