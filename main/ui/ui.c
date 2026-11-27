// UI core — section state machine, ui_init(), ui_tick() (all input handling).
// Screens draw themselves in ui_*.c; pixels go through tft.c only.

#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "touch_telemetry.h"
#include "looper.h"
#include "presets.h"
#include "ui.h"
#include "ui_internal.h"

// ── UI state ─────────────────────────────────────────────────
typedef enum { UI_CARDS, UI_MAIN, UI_CALIB, UI_EFFECTS, UI_ADSR, UI_PRESETS } ui_section_t;

static ui_section_t s_sec       = UI_CARDS;
int                 ui_card_cur = 0;  // 0=MAIN 1=PRST 2=CAL
static bool         s_dirty     = true;

// CAL edit live refresh: redraw only bar + numbers, not the whole screen.
static bool s_cal_live    = false;

// MAIN 6-card overview + per-card edit
int      ui_main_card    = 0;    // 0-5: selected card in overview
bool     ui_main_in_card = false; // false=overview, true=card edit
int      ui_main_param   = 0;    // param index within card edit (focus)
bool     ui_main_editing = false; // card edit: false = focus mode (rotate moves between params),
                                 //            true  = edit mode (rotate changes the focused value)
// Param counts per card: ENV FLT EG1 LFO ECH GLD
// Param counts per card: ENV FLT EG1 LFO FX GLD   (FX = echo AMT/FB/DLY + reverb REV/SIZE)
static const int s_card_param_max[6] = {4, 3, 2, 2, 5, 1};
// Mirror of engine state for display (loaded on enter, pot-synced in overview)
uint16_t ui_main_adsr[4];        // 0=A 1=D 2=S 3=R
uint16_t ui_main_flt_cut;
uint16_t ui_main_flt_res;
uint8_t  ui_main_flt_type;       // 0=LPF 1=BPF 2=HPF
uint16_t ui_main_lfo_rate;
uint16_t ui_main_lfo_depth;
uint16_t ui_main_ech_amt;
uint16_t ui_main_ech_fb;
uint16_t ui_main_ech_dly;
uint16_t ui_main_rev_amt;        // reverb amount (0 = off)
uint16_t ui_main_rev_dec;        // reverb size / decay
uint16_t ui_main_eg1_depth;
uint16_t ui_main_eg1_decay;
uint16_t ui_main_gld;
// Echo delay snap values (0-10000 → 1/16, 1/8, 1/6, 1/4, 1/3, 1/2 second)
static const uint16_t s_echo_delay_snaps[6] = {277, 1667, 2593, 4444, 6296, 10000};

static inline uint16_t u16clamp(int v) { return (uint16_t)(v < 0 ? 0 : v > 10000 ? 10000 : v); }

int  ui_prst_cur    = 0;     // 0..5 currently highlighted slot
bool ui_prst_action = false; // in action sub-menu
int  ui_prst_act    = 0;     // 0=LOAD 1=SAVE 2=CLR

// Calibration section
int      ui_cal_pad     = 0;
bool     ui_cal_editing = false;
uint16_t ui_cal_thr     = 100;

// Nearest echo delay snap index (for encoder step-through).
static int echo_snap_idx(uint16_t v) {
    int best = 0, best_d = abs((int)v - (int)s_echo_delay_snaps[0]);
    for (int i = 1; i < 6; i++) {
        int d = abs((int)v - (int)s_echo_delay_snaps[i]);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static void ui_redraw(void)
{
    switch (s_sec) {
        case UI_CARDS:   ui_draw_cards();   break;
        case UI_MAIN:    ui_draw_main();    break;
        case UI_CALIB:   ui_draw_calib();   break;
        case UI_EFFECTS: ui_draw_effects(); break;
        case UI_ADSR:    ui_draw_adsr();    break;
        case UI_PRESETS: ui_draw_presets(); break;
    }
    ui_draw_ribbon(); // always last — overwrites y=0..21
}

// ── Section enter helpers ─────────────────────────────────────
static void ui_enter(ui_section_t sec)
{
    s_sec = sec;
    s_dirty = true;

    if (sec == UI_ADSR) {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        ui_adsr[0] = st.env_attack;
        ui_adsr[1] = st.env_decay;
        ui_adsr[2] = st.env_sustain;
        ui_adsr[3] = st.env_release;
        ui_adsr_cur     = 0;
        ui_adsr_editing = false;
    }
    if (sec == UI_MAIN) {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        ui_main_adsr[0]   = st.env_attack;
        ui_main_adsr[1]   = st.env_decay;
        ui_main_adsr[2]   = st.env_sustain;
        ui_main_adsr[3]   = st.env_release;
        ui_main_flt_cut   = st.filter_cutoff;
        ui_main_flt_res   = st.filter_resonance;
        ui_main_flt_type  = st.filter_type;
        ui_main_lfo_rate  = st.lfo_rate;
        ui_main_lfo_depth = st.lfo_depth;
        ui_main_ech_amt   = st.echo_amount;
        ui_main_ech_fb    = st.echo_feedback;
        ui_main_ech_dly   = st.echo_delay;
        ui_main_rev_amt   = st.reverb_amount;
        ui_main_rev_dec   = st.reverb_decay;
        ui_main_eg1_depth = st.filter_env_depth;
        ui_main_eg1_decay = st.filter_env_decay;
        ui_main_gld       = st.glide;
        ui_main_card      = 0;
        ui_main_in_card   = false;
        ui_main_param     = 0;
    }
    if (sec == UI_CALIB) {
        ui_cal_editing = false;
    }
    if (sec == UI_PRESETS) {
        for (int i = 0; i < PRESET_COUNT; i++)
            preset_nvs_load(i);
        ui_prst_cur    = 0;
        ui_prst_action = false;
        ui_prst_act    = 0;
    }
    if (sec == UI_EFFECTS) {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        ui_fx_flt_type = st.filter_type;
        ui_fx_vals[0]  = st.filter_cutoff;
        ui_fx_vals[1]  = st.filter_resonance;
        ui_fx_vals[2]  = st.echo_amount;
        ui_fx_vals[3]  = st.echo_feedback;
        ui_fx_vals[4]  = st.glide;
        ui_fx_vals[5]  = st.lfo_rate;
        ui_fx_vals[6]  = st.lfo_depth;
        ui_fx_cur      = 0;
        ui_fx_scroll   = 0;
        ui_fx_editing  = false;
    }
}

static void ui_exit_to_cards(void)
{
    if (s_sec == UI_CALIB && ui_cal_editing) {
        touch_telemetry_set_threshold((uint8_t)ui_cal_pad, ui_cal_thr);
        ui_cal_editing = false;
    }
    if (s_sec == UI_MAIN || s_sec == UI_EFFECTS || s_sec == UI_ADSR) {
        amy_engine_save_state();
    }
    ui_enter(UI_CARDS);
}

void ui_init(void)
{
    tft_init();   // SPI bus + DC/RST pins + panel init (bus setup was inline here)
    presets_init();
    ui_redraw();
    tft_flush();
}

// Called from main loop every ~20 ms.
// delta: encoder steps since last call (signed).
// Button gestures (from main.c):
//   short_press  : click — normal navigation (reported 250 ms after release)
//   long_press   : 0.5 s hold — back (on the top screen: arms loop clear)
//   vlong_press  : 2 s hold — clears the loop, only if the hold STARTED on the
//                  top screen (a "back" hold that continues can't wipe the loop)
//   double_click : looper REC → PLAY → OVERDUB → PLAY … from any screen
void ui_tick(int delta, int accel, bool short_press, bool long_press, bool vlong_press, bool double_click)
{
    // accel (1..3): subtle encoder acceleration from main.c. Applied ONLY to
    // value changes; list/focus navigation always moves one item per click.
    int vdelta = delta * accel;
    // Looper: double click works everywhere; recording starts on the 2nd click.
    if (double_click) looper_cycle();   // ribbon dot updates via ui_ribbon_update()

    // A loop clear is armed only by a hold whose long-press fired on the top
    // screen. Every hold fires long_press before vlong_press, so this is
    // re-evaluated for each hold.
    static bool s_clear_armed = false;
    if (long_press) s_clear_armed = (s_sec == UI_CARDS);

    switch (s_sec) {

    case UI_CARDS:
        if (delta) {
            ui_card_cur = ui_card_cur + delta;              // lists stop at the ends (no wrap)
            if (ui_card_cur < 0) ui_card_cur = 0;
            if (ui_card_cur > 2) ui_card_cur = 2;
            s_dirty = true;
        }
        if (short_press) {
            switch (ui_card_cur) {
                case 0: ui_enter(UI_MAIN);    break;
                case 1: ui_enter(UI_PRESETS); break;
                case 2: ui_enter(UI_CALIB);   break;
            }
        }
        // long_press on the top screen only arms the clear (see above)
        if (vlong_press && s_clear_armed) {
            looper_clear(); // 2 s hold started here — stop and clear loop
            s_clear_armed = false;
        }
        break;

    case UI_MAIN:
        // Pot display sync — 10 Hz, overview only; triggers redraw on pot-controlled changes
        if (!ui_main_in_card) {
            static int64_t s_pot_sync_us = 0;
            int64_t t_now = esp_timer_get_time();
            if (t_now - s_pot_sync_us >= 100000LL) {
                s_pot_sync_us = t_now;
                amy_engine_state_t st;
                amy_engine_get_state(&st);
                bool ch = ui_main_adsr[0] != st.env_attack       ||
                          ui_main_adsr[1] != st.env_decay         ||
                          ui_main_adsr[2] != st.env_sustain       ||
                          ui_main_adsr[3] != st.env_release       ||
                          ui_main_flt_cut  != st.filter_cutoff    ||
                          ui_main_flt_res  != st.filter_resonance ||
                          ui_main_ech_amt  != st.echo_amount      ||
                          ui_main_ech_fb   != st.echo_feedback    ||
                          ui_main_gld      != st.glide;
                ui_main_adsr[0]   = st.env_attack;
                ui_main_adsr[1]   = st.env_decay;
                ui_main_adsr[2]   = st.env_sustain;
                ui_main_adsr[3]   = st.env_release;
                ui_main_flt_cut   = st.filter_cutoff;
                ui_main_flt_res   = st.filter_resonance;
                ui_main_flt_type  = st.filter_type;
                ui_main_lfo_rate  = st.lfo_rate;
                ui_main_lfo_depth = st.lfo_depth;
                ui_main_ech_amt   = st.echo_amount;
                ui_main_ech_fb    = st.echo_feedback;
                ui_main_ech_dly   = st.echo_delay;
                ui_main_rev_amt   = st.reverb_amount;
                ui_main_rev_dec   = st.reverb_decay;
                ui_main_eg1_depth = st.filter_env_depth;
                ui_main_eg1_decay = st.filter_env_decay;
                ui_main_gld       = st.glide;
                if (ch) s_dirty = true;
            }
        }

        if (!ui_main_in_card) {
            // Overview: encoder scrolls cards, short press enters card, long press → CARDS
            if (delta) {
                ui_main_card = ui_main_card + delta;        // stop at the ends
                if (ui_main_card < 0) ui_main_card = 0;
                if (ui_main_card > 5) ui_main_card = 5;
                s_dirty = true;
            }
            if (short_press) { ui_main_in_card = true; ui_main_param = 0; ui_main_editing = false; s_dirty = true; }
            if (long_press)  { amy_engine_save_state(); ui_enter(UI_CARDS); }
        } else {
            // Card edit, two modes (car-radio style "direct manipulation"):
            //   focus: rotate moves between params (stops at the ends), click → edit
            //   edit : rotate changes the focused value (with acceleration), click → back to focus
            //   hold (either mode): save and return to the overview
            int p = ui_main_param;
            bool edit_entered_now = false;
            if (!ui_main_editing) {
                if (delta) {
                    int np = ui_main_param + delta;
                    if (np < 0) np = 0;
                    if (np > s_card_param_max[ui_main_card] - 1) np = s_card_param_max[ui_main_card] - 1;
                    if (np != ui_main_param) { ui_main_param = np; s_dirty = true; }
                }
                if (short_press) { ui_main_editing = true; edit_entered_now = true; s_dirty = true; }
                delta = 0;   // rotation was used for focus — don't fall into value edits below
            }
            if (delta) {
                switch (ui_main_card) {
                case 0: // ENV
                    ui_main_adsr[p] = u16clamp((int)ui_main_adsr[p] + vdelta * 500);
                    amy_engine_set_adsr(ui_main_adsr[0],ui_main_adsr[1],ui_main_adsr[2],ui_main_adsr[3]);
                    break;
                case 1: // FLT
                    if (p == 0)      { ui_main_flt_cut = u16clamp((int)ui_main_flt_cut + vdelta*500); amy_engine_set_filter(ui_main_flt_cut,ui_main_flt_res); }
                    else if (p == 1) { ui_main_flt_res = u16clamp((int)ui_main_flt_res + vdelta*500); amy_engine_set_filter(ui_main_flt_cut,ui_main_flt_res); }
                    else { // TYPE: LPF/BPF/HPF, stops at the ends
                        int t = (int)ui_main_flt_type + (delta > 0 ? 1 : -1);
                        if (t < 0) t = 0;
                        if (t > 2) t = 2;
                        ui_main_flt_type = (uint8_t)t;
                        amy_engine_set_filter_type(ui_main_flt_type);
                    }
                    break;
                case 2: // EG1
                    if (p == 0) ui_main_eg1_depth = u16clamp((int)ui_main_eg1_depth + vdelta*500);
                    else        ui_main_eg1_decay  = u16clamp((int)ui_main_eg1_decay  + vdelta*500);
                    amy_engine_set_filter_env(ui_main_eg1_depth, ui_main_eg1_decay);
                    break;
                case 3: // LFO
                    if (p == 0) ui_main_lfo_rate  = u16clamp((int)ui_main_lfo_rate  + vdelta*500);
                    else        ui_main_lfo_depth = u16clamp((int)ui_main_lfo_depth + vdelta*500);
                    amy_engine_set_lfo(ui_main_lfo_rate, ui_main_lfo_depth);
                    break;
                case 4: // FX: 0 echo AMT, 1 echo FB, 2 echo DLY, 3 reverb REV, 4 reverb SIZE
                    if (p == 0)      { ui_main_ech_amt = u16clamp((int)ui_main_ech_amt + vdelta*500); amy_engine_set_echo(ui_main_ech_amt,ui_main_ech_fb); }
                    else if (p == 1) { ui_main_ech_fb  = u16clamp((int)ui_main_ech_fb  + vdelta*500); amy_engine_set_echo(ui_main_ech_amt,ui_main_ech_fb); }
                    else if (p == 3) { ui_main_rev_amt = u16clamp((int)ui_main_rev_amt + vdelta*500); amy_engine_set_reverb(ui_main_rev_amt,ui_main_rev_dec); }
                    else if (p == 4) { ui_main_rev_dec = u16clamp((int)ui_main_rev_dec + vdelta*500); amy_engine_set_reverb(ui_main_rev_amt,ui_main_rev_dec); }
                    else { // DLY: snap through fractions
                        int idx = echo_snap_idx(ui_main_ech_dly) + (delta > 0 ? 1 : -1);
                        if (idx < 0) idx = 0;
                        if (idx > 5) idx = 5;
                        ui_main_ech_dly = s_echo_delay_snaps[idx];
                        amy_engine_set_echo_delay(ui_main_ech_dly);
                    }
                    break;
                case 5: // GLD
                    ui_main_gld = u16clamp((int)ui_main_gld + vdelta*500);
                    amy_engine_set_glide(ui_main_gld);
                    break;
                }
                s_dirty = true;
            }
            if (short_press && ui_main_editing && !edit_entered_now) {
                ui_main_editing = false;   // confirm → back to focus mode
                s_dirty = true;
            }
            if (long_press) {
                amy_engine_save_state();
                ui_main_in_card = false;
                ui_main_editing = false;
                ui_main_param   = 0;
                s_dirty        = true;
            }
        }
        break;

    case UI_CALIB:
        if (!ui_cal_editing) {
            if (delta) {
                ui_cal_pad = ui_cal_pad + delta;            // stop at the ends
                if (ui_cal_pad < 0) ui_cal_pad = 0;
                if (ui_cal_pad > TOUCH_TOTAL_PADS - 1) ui_cal_pad = TOUCH_TOTAL_PADS - 1;
                s_dirty = true;
            }
            if (short_press) {
                ui_cal_thr     = touch_telemetry_get_threshold((uint8_t)ui_cal_pad);
                ui_cal_editing = true;
                s_dirty       = true;
            }
            if (long_press) ui_exit_to_cards();
        } else {
            if (delta) {
                int t = (int)ui_cal_thr + vdelta * 50;
                if (t < 10) t = 10;
                ui_cal_thr  = (uint16_t)t;
                s_cal_live = true;   // only bar/marker/THR change — no full redraw
            }
            if (short_press) {
                touch_telemetry_set_threshold((uint8_t)ui_cal_pad, ui_cal_thr);
                ui_cal_editing = false;
                s_dirty       = true;
            }
            if (long_press) {
                touch_telemetry_set_threshold((uint8_t)ui_cal_pad, ui_cal_thr);
                ui_exit_to_cards();
            }
        }
        break;

    case UI_EFFECTS:
        if (!ui_fx_editing) {
            if (delta) {
                ui_fx_cur += delta;
                if (ui_fx_cur < 0) ui_fx_cur = 0;
                if (ui_fx_cur > 7) ui_fx_cur = 7;
                if (ui_fx_cur < ui_fx_scroll)       ui_fx_scroll = ui_fx_cur;
                if (ui_fx_cur >= ui_fx_scroll + 4)  ui_fx_scroll = ui_fx_cur - 3;
                s_dirty = true;
            }
            if (short_press) {
                if (ui_fx_cur == 0) {
                    ui_fx_flt_type = (ui_fx_flt_type + 1) % 3;
                    // Per-type defaults recalculated for Spark exponential formula.
                    // LPF≈8kHz, BPF≈1.3kHz, HPF≈250Hz
                    static const uint16_t s_flt_cutoff_dflt[3] = {7778, 5579, 3581};
                    ui_fx_vals[0] = s_flt_cutoff_dflt[ui_fx_flt_type];
                    amy_engine_set_filter_type(ui_fx_flt_type);
                    amy_engine_set_filter(ui_fx_vals[0], ui_fx_vals[1]);
                    s_dirty = true;
                } else {
                    ui_fx_editing = true;
                    s_dirty = true;
                }
            }
        } else {
            if (delta) {
                int v = (int)ui_fx_vals[ui_fx_cur - 1] + delta * 500;
                if (v < 0)     v = 0;
                if (v > 10000) v = 10000;
                ui_fx_vals[ui_fx_cur - 1] = (uint16_t)v;
                switch (ui_fx_cur) {
                    case 1: case 2:
                        amy_engine_set_filter(ui_fx_vals[0], ui_fx_vals[1]); break;
                    case 3: case 4:
                        amy_engine_set_echo(ui_fx_vals[2], ui_fx_vals[3]);   break;
                    case 5:
                        amy_engine_set_glide(ui_fx_vals[4]); break;
                    case 6: case 7:
                        amy_engine_set_lfo(ui_fx_vals[5], ui_fx_vals[6]); break;
                }
                s_dirty = true;
            }
            if (short_press) { ui_fx_editing = false; s_dirty = true; }
        }
        if (long_press) ui_exit_to_cards();
        break;

    case UI_ADSR:
        if (!ui_adsr_editing) {
            if (delta) {
                ui_adsr_cur += delta;
                if (ui_adsr_cur < 0) ui_adsr_cur = 0;
                if (ui_adsr_cur > 3) ui_adsr_cur = 3;
                s_dirty = true;
            }
            if (short_press) { ui_adsr_editing = true; s_dirty = true; }
        } else {
            if (delta) {
                int v = (int)ui_adsr[ui_adsr_cur] + delta * 500;
                if (v < 0)     v = 0;
                if (v > 10000) v = 10000;
                ui_adsr[ui_adsr_cur] = (uint16_t)v;
                amy_engine_set_adsr(ui_adsr[0], ui_adsr[1], ui_adsr[2], ui_adsr[3]);
                s_dirty = true;
            }
            if (short_press) { ui_adsr_editing = false; s_dirty = true; }
        }
        if (long_press) ui_exit_to_cards();
        break;

    case UI_PRESETS:
        if (!ui_prst_action) {
            if (delta) {
                ui_prst_cur = ui_prst_cur + delta;          // stop at the ends
                if (ui_prst_cur < 0) ui_prst_cur = 0;
                if (ui_prst_cur > PRESET_COUNT - 1) ui_prst_cur = PRESET_COUNT - 1;
                s_dirty = true;
            }
            if (short_press) {
                ui_prst_action = true;
                // Auto-select sensible default: LOAD for occupied, SAVE for empty
                ui_prst_act = preset_is_valid(ui_prst_cur) ? 0 : 1;
                s_dirty = true;
            }
            if (long_press) ui_exit_to_cards();
        } else {
            if (delta) {
                ui_prst_act = ui_prst_act + delta;          // stop at the ends
                if (ui_prst_act < 0) ui_prst_act = 0;
                if (ui_prst_act > 2) ui_prst_act = 2;
                s_dirty = true;
            }
            if (short_press) {
                switch (ui_prst_act) {
                case 0: // LOAD — no-op on empty slot
                    if (preset_is_valid(ui_prst_cur)) preset_apply(ui_prst_cur);
                    break;
                case 1: // SAVE — always available
                    preset_capture(ui_prst_cur);
                    preset_name_set(ui_prst_cur, NULL);   // user save → generic "Pn"
                    preset_nvs_save(ui_prst_cur);
                    break;
                case 2: // CLR — no-op on empty slot
                    if (preset_is_valid(ui_prst_cur)) preset_nvs_clear(ui_prst_cur);
                    break;
                }
                ui_prst_action = false;
                s_dirty = true;
            }
            if (long_press) {
                ui_prst_action = false; // cancel action, stay in presets
                s_dirty = true;
            }
        }
        break;
    }

    // CAL edit live readout — RAW/BASE change without user input, refresh at 20 Hz
    static TickType_t last_refresh = 0;
    TickType_t now = xTaskGetTickCount();
    if (s_sec == UI_CALIB && ui_cal_editing && (now - last_refresh) >= pdMS_TO_TICKS(50))
        s_cal_live = true;

    if (s_dirty) {
        // Layout / section change → full repaint (body + ribbon)
        s_dirty      = false;
        s_cal_live   = false;
        last_refresh = now;
        ui_redraw();
    } else {
        // Partial refresh only
        if (s_cal_live) {
            s_cal_live   = false;
            last_refresh = now;
            if (s_sec == UI_CALIB && ui_cal_editing) ui_draw_calib_live();
        }
        ui_ribbon_update();
    }

    // Everything above only drew into the framebuffer; send the changes now.
    tft_flush();
}
