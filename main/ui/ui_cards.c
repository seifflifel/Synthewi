// Top-level CARDS screen (MAIN / PRST / CAL).

#include <string.h>
#include "presets.h"
#include "ui_internal.h"

// ---- CARDS (top-level navigation) ---------------------------
// 3 rounded cards with gap borders, TE-style amber accent for selected.
void ui_draw_cards(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    static const char * const names[3] = {"MAIN", "PRST", "CAL"};
    // Card x ranges with 1px gaps between them
    static const int cxs[3] = {1,  54, 108};
    static const int cxe[3] = {52, 105, 158};

    for (int i = 0; i < 3; i++) {
        bool sel = (i == ui_card_cur);
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
        uint16_t ec = (ui_card_cur == 0) ? C_ORANGE : C_DKGRAY;
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
            bool occ = preset_is_valid(j);
            if (occ)
                tft_rrect_fill(sx, sy, sx+12, sy+12, (ui_card_cur==1) ? C_ORANGE : C_LTGRAY);
            else
                tft_rrect(sx, sy, sx+12, sy+12, C_DKGRAY);
            char n[2] = {(char)('1'+j), 0};
            uint16_t nc = occ ? C_BLACK  : C_DKGRAY;
            uint16_t nb = occ ? ((ui_card_cur==1) ? C_ORANGE : C_LTGRAY) : C_BLACK;
            tft_text(n, sx+4, sy+3, nc, nb, 1);
        }
    }

    // ── CAL body: decorative bar + threshold marker (card 2, x=108..158) ──
    {
        uint16_t mc = (ui_card_cur == 2) ? C_ORANGE : C_DKGRAY;
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
