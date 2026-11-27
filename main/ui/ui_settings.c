// SETTINGS screen (redesign 2026-11): CALIBRATE (opens the CAL screen) and
// THEME (light / dark). Dark = the panel's own colour inversion (ST7735 INVON),
// i.e. a true inversion of every colour, persisted in NVS "ui"/"dark".

#include <math.h>
#include <string.h>
#include "nvs.h"
#include "ui_internal.h"

int  ui_set_cur = 0;          // focused row
bool ui_dark    = false;

static const char * const s_rows[UI_SET_ROWS] = {"CALIBRATE", "THEME"};

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
}
