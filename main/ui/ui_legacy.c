// Legacy EFFECTS / ADSR screens — unreachable (superseded by the MAIN cards),
// kept until the dead-code removal step. Their input handling is in ui_tick().

#include <stdio.h>
#include <inttypes.h>
#include <math.h>
#include "ui_internal.h"

// Effects section
// items 0-7: FltType CUT RES ECH EFB GLD LFRT LFDP
int      ui_fx_cur      = 0;
int      ui_fx_scroll   = 0;
bool     ui_fx_editing  = false;
uint8_t  ui_fx_flt_type = 0;       // 0=LPF 1=BPF 2=HPF
uint16_t ui_fx_vals[7];            // [0]=cut [1]=res [2]=ech [3]=efb [4]=gld [5]=lfr [6]=lfd

// ADSR section (0-10000 internal scale)
int      ui_adsr_cur     = 0;  // 0=A 1=D 2=S 3=R
bool     ui_adsr_editing = false;
uint16_t ui_adsr[4];           // loaded from engine on enter

// ---- EFFECTS (legacy — now superseded by UI_MAIN columns) ---
void ui_draw_effects(void)
{
    static const char * const s_flt_names[3] = {"LPF", "BPF", "HPF"};
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    for (int vi = 0; vi < 4; vi++) {
        int item = ui_fx_scroll + vi;
        if (item >= 8) break;

        bool is_cur = (item == ui_fx_cur);
        bool editing = is_cur && ui_fx_editing;
        char cur = editing ? '#' : (is_cur ? '>' : ' ');

        char row[16];
        switch (item) {
            case 0:
                snprintf(row, sizeof(row), "%cFLT %s", cur, s_flt_names[ui_fx_flt_type]);
                break;
            case 1: {
                uint32_t hz = (uint32_t)ui_filter_hz(ui_fx_vals[0]);
                snprintf(row, sizeof(row), "%cCUT%5u", cur, (unsigned)hz);
                break;
            }
            case 2: {
                float q = 0.7f * powf(2.0f, 4.0f * (ui_fx_vals[1] / 10000.0f));
                uint32_t qi = (uint32_t)(q * 10.0f);
                snprintf(row, sizeof(row), "%cQ %2u.%u ", cur, (unsigned)(qi/10), (unsigned)(qi%10));
                break;
            }
            case 3:
                snprintf(row, sizeof(row), "%cECH %3u%%", cur, (unsigned)(ui_fx_vals[2] / 100));
                break;
            case 4:
                snprintf(row, sizeof(row), "%cEFB %3u%%", cur, (unsigned)(ui_fx_vals[3] / 100));
                break;
            case 5: {
                uint32_t ms = ((uint32_t)ui_fx_vals[4] * 500) / 10000;
                snprintf(row, sizeof(row), "%cGLD %3uMS", cur, (unsigned)ms);
                break;
            }
            case 6: {
                uint32_t x10 = 1 + ((uint32_t)ui_fx_vals[5] * 99) / 10000;
                snprintf(row, sizeof(row), "%cLFR%u.%uHZ", cur,
                         (unsigned)(x10 / 10), (unsigned)(x10 % 10));
                break;
            }
            case 7: {
                uint32_t hz = ((uint32_t)ui_fx_vals[6] * 5000) / 10000;
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
    if (ui_fx_editing)
        tft_text("ROT:ADJ BTN:DONE HLD:BACK", 2, 118, C_DKGRAY, C_BLACK, 1);
    else if (ui_fx_cur == 0)
        tft_text("BTN:CYCLE        HLD:BACK", 2, 118, C_DKGRAY, C_BLACK, 1);
    else
        tft_text("BTN:EDIT         HLD:BACK", 2, 118, C_DKGRAY, C_BLACK, 1);
}

// ---- ADSR (legacy — now part of UI_MAIN ENV column) ---------
void ui_draw_adsr(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    typedef uint32_t (*conv_fn)(uint16_t);
    const conv_fn conv[4]  = { ui_a_ms, ui_d_ms, ui_s_pct, ui_r_ms };
    const char * const units[4] = { "MS", "MS", "% ", "MS" };
    const char * const lbls[4]  = { "A", "D", "S", "R" };

    for (int i = 0; i < 4; i++) {
        bool is_cur  = (i == ui_adsr_cur);
        bool editing = is_cur && ui_adsr_editing;
        char cursor  = editing ? '#' : (is_cur ? '>' : ' ');
        uint32_t val = conv[i](ui_adsr[i]);

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
    if (ui_adsr_editing)
        tft_text("ROT:ADJ  BTN:DONE  HLD:BACK", 2, 117, C_DKGRAY, C_BLACK, 1);
    else
        tft_text("ROT:MOVE BTN:EDIT  HLD:BACK", 2, 117, C_DKGRAY, C_BLACK, 1);
}

