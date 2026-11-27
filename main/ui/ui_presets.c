// PRESETS screen — 2x3 slot grid with LOAD/SAVE/CLR overlay (draw only).

#include <string.h>
#include <stdio.h>
#include "presets.h"
#include "ui_internal.h"

// ---- PRESETS — 2×3 slot grid --------------------------------
// Columns: x=0..52, 54..106, 108..159.  Rows: y=22..73 / y=75..127.
void ui_draw_presets(void)
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
        bool sel = (i == ui_prst_cur);

        // Header strip (10px) — orange for selected, dark gray for others
        uint16_t hbg = sel ? C_ORANGE : C_DKGRAY;
        uint16_t hfg = sel ? C_BLACK  : C_LTGRAY;
        tft_fill(cx, ry, cx + cw - 1, ry + 9, hbg);
        char hdr[4]; snprintf(hdr, sizeof(hdr), "P%d", i + 1);
        tft_text(hdr, cx + 2, ry + 1, hfg, hbg, 1);
        if (preset_is_valid(i)) {
            int dx = cx + cw - 5;
            tft_fill((uint8_t)dx, (uint8_t)(ry+3), (uint8_t)(dx+2), (uint8_t)(ry+5), hfg);
        }

        // Body
        int body_top = ry + 10;
        int body_bot = (row == 0) ? ry + 42 : TFT_H - 1;
        tft_fill(cx, body_top, cx + cw - 1, body_bot, C_BLACK);

        if (ui_prst_action && sel) {
            // Action overlay: LOAD / SAVE / CLR
            for (int a = 0; a < 3; a++) {
                bool avail = (a == 1) || preset_is_valid(i);
                bool asel  = (a == ui_prst_act);
                uint16_t fg = !avail ? C_DKGRAY : (asel ? C_ORANGE : C_LTGRAY);
                uint16_t bg = (asel && avail) ? C_DKGRAY : C_BLACK;
                int ay = body_top + 4 + a * 12;
                if (asel && avail)
                    tft_fill(cx, (uint8_t)ay, cx+cw-1, (uint8_t)(ay+8), C_DKGRAY);
                int llen = (int)strlen(act_lbl[a]);
                tft_text(act_lbl[a], cx + (cw - llen*6)/2, ay, fg, bg, 1);
            }
        } else if (preset_is_valid(i)) {
            const preset_t *p = preset_get(i);
            uint16_t fc = sel ? C_ORANGE : C_LTGRAY;

            // Mini wave glyph (20×12, centered)
            int gx0 = cx + (cw - 20) / 2;
            ui_draw_wave_glyph(p->wave_id, gx0, body_top+2, gx0+19, body_top+13, fc);

            // Wave name (2 chars)
            // Preset name if it has one (factory presets), else the wave name
            const char *wname = preset_get_name(i)[0] ? preset_get_name(i)
                              : (p->wave_id < 7) ? wn[p->wave_id] : "??";
            tft_text(wname, cx+2, body_top+16, fc, C_BLACK, 1);

            // Filter cutoff summary
            float fc_hz = ui_filter_hz(p->flt_cut);
            char fstr[6];
            snprintf(fstr, sizeof(fstr), fc_hz >= 1000.0f ? "F%uK" : "F%u",
                     fc_hz >= 1000.0f ? (unsigned)(fc_hz/1000.0f+0.5f) : (unsigned)fc_hz);
            tft_text(fstr, cx+2, body_top+26, fc, C_BLACK, 1);

            // Attack summary
            uint32_t atk = ui_a_ms(p->env_atk);
            char astr[6];
            snprintf(astr, sizeof(astr), atk >= 1000 ? "A%uK" : "A%u",
                     (unsigned)(atk >= 1000 ? atk/1000 : atk));
            tft_text(astr, cx+26, body_top+26, fc, C_BLACK, 1);
        } else {
            tft_text("----", cx+(cw-4*6)/2, body_top+16, C_DKGRAY, C_BLACK, 1);
        }
    }
}
