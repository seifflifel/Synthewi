#pragma once
// Shared state + helpers for the UI screen modules (not part of the public API,
// see ../ui.h). State is owned by ui.c (input / section logic) unless noted;
// the ui_draw_* screens only read it.

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "amy_engine.h"
#include "tft.h"

// ── Shared UI state ───────────────────────────────────────────
extern int      ui_card_cur;      // HOME tile: 0=PARAMS (MAIN) 1=PRESETS 2=SETTINGS (CAL for now)
extern bool     ui_on_home;       // HOME is the current screen (ribbon separator shows loop progress elsewhere)

// MAIN 6-card overview + per-card edit
extern int      ui_main_card;     // 0-5: selected card in overview
extern bool     ui_main_in_card;  // false=overview, true=card edit
extern int      ui_main_param;    // param index within card edit (focus)
extern bool     ui_main_editing;  // card edit: false = focus mode, true = edit mode
// Mirror of engine state for display (loaded on enter, pot-synced in overview)
extern uint16_t ui_main_adsr[4];  // 0=A 1=D 2=S 3=R
extern uint16_t ui_main_flt_cut;
extern uint16_t ui_main_flt_res;
extern uint8_t  ui_main_flt_type; // 0=LPF 1=BPF 2=HPF
extern uint16_t ui_main_lfo_rate;
extern uint16_t ui_main_lfo_depth;
extern uint16_t ui_main_ech_amt;
extern uint16_t ui_main_ech_fb;
extern uint16_t ui_main_ech_dly;
extern uint16_t ui_main_rev_amt;  // reverb amount (0 = off)
extern uint16_t ui_main_rev_dec;  // reverb size / decay
extern uint16_t ui_main_eg1_depth;
extern uint16_t ui_main_eg1_decay;
extern uint16_t ui_main_gld;

// Presets screen
extern int      ui_prst_cur;      // 0..5 currently highlighted slot
extern bool     ui_prst_action;   // in action sub-menu
extern int      ui_prst_act;      // 0=LOAD 1=SAVE 2=CLR

// Calibration screen
extern int      ui_cal_pad;
extern bool     ui_cal_editing;
extern uint16_t ui_cal_thr;       // MIN threshold being edited (trigger, 10 % velocity)
extern uint16_t ui_cal_thr_max;   // MAX threshold being edited (100 % velocity, note pads)
extern int      ui_cal_stage;     // 0 = editing MIN, 1 = editing MAX (note pads only)

// Legacy EFFECTS / ADSR screens (unreachable; defined in ui_legacy.c)
extern int      ui_fx_cur;
extern int      ui_fx_scroll;
extern bool     ui_fx_editing;
extern uint8_t  ui_fx_flt_type;   // 0=LPF 1=BPF 2=HPF
extern uint16_t ui_fx_vals[7];    // [0]=cut [1]=res [2]=ech [3]=efb [4]=gld [5]=lfr [6]=lfd
extern int      ui_adsr_cur;      // 0=A 1=D 2=S 3=R
extern bool     ui_adsr_editing;
extern uint16_t ui_adsr[4];       // loaded from engine on enter

// ── Display conversion ────────────────────────────────────────
// Thin wrappers over the engine's curves (amy_engine.c is the single source of
// truth) — the screen always shows exactly what the engine plays.
static inline uint32_t ui_a_ms(uint16_t v)         { return amy_engine_attack_ms(v); }
static inline uint32_t ui_d_ms(uint16_t v)         { return amy_engine_decay_ms(v); }
static inline uint32_t ui_s_pct(uint16_t v)        { return v / 100u; }
static inline uint32_t ui_r_ms(uint16_t v)         { return amy_engine_release_ms(v); }
static inline float    ui_filter_hz(uint16_t v)    { return amy_engine_filter_hz(v); }
static inline float    ui_filter_q(uint16_t v)     { return amy_engine_filter_q(v); }
static inline float    ui_lfo_hz(uint16_t v)       { return amy_engine_lfo_hz(v); }
static inline uint32_t ui_eg1_decay_ms(uint16_t v) { return amy_engine_fenv_decay_ms(v); }
static inline uint32_t ui_glide_ms(uint16_t v)     { return amy_engine_glide_ms(v); }
// EG1 / LFO filter depth in tenths of an octave
static inline unsigned ui_fenv_oct10(uint16_t v)   { return (unsigned)lroundf(amy_engine_fenv_depth_oct(v) * 10.0f); }
static inline unsigned ui_lfo_oct10(uint16_t v)    { return (unsigned)lroundf(amy_engine_lfo_depth_oct(v) * 10.0f); }

// ── Motion (ui_anim.c) ────────────────────────────────────────
extern const int UI_BODY_RECT[4];             // body area below the ribbon (transition target)
float ui_now_s(void);                         // seconds (wraps hourly) for animations
float ui_tween(float cur, float target, float *last_s); // time-based ease toward target (pixels)
float ui_tween_eps(float cur, float target, float *last_s, float eps);
void  ui_trans_start(const int from[4], const int to[4], int tag);
bool  ui_trans_active(void);
int   ui_trans_step(void);                    // returns tag once the transition finished, else -1
void  ui_trans_draw(void);
bool  ui_trans_rect(int r[4]);                // current transition window
bool  ui_popup_active(void);
void  ui_popup_draw(void);
void  ui_msg_popup(const char *text);         // SAVED / LOADED / CLEARED box (~0.7 s)
bool  ui_anim_busy(void);                     // a transition or pop-up needs per-tick redraws

// ── Screen draw functions ─────────────────────────────────────
// ui_home.c
void ui_draw_home(void);
void ui_home_tile_rect(int i, int r[4]);
// ui_presets.c
extern int ui_prst_loaded;
void ui_presets_tile_rect(int i, int r[4]);
// ui_settings.c
#define UI_SET_ROWS 4                          // CALIBRATE, KEY, SCALE, THEME
extern int  ui_set_cur;
extern bool ui_set_editing;                    // KEY / SCALE row being edited
extern bool ui_dark;
void ui_draw_settings(void);
void ui_settings_row_rect(int i, int r[4]);
void ui_theme_load(void);
void ui_theme_toggle(void);
void ui_settings_picker_open(void);
void ui_settings_picker_close(bool keep);
// ui_params.c (PARAMS grid + edit view, replaces ui_draw_main)
void ui_draw_params(void);
void ui_params_tile_rect(int i, int r[4]);
// ui_ribbon.c — note activity for live icons: any pad held + last on/off times (us)
bool ui_note_state(int64_t *on_us, int64_t *off_us);
// ui_ribbon.c
void ui_draw_wave_glyph(uint8_t wave_id, int x0, int y0, int x1, int y1, uint16_t fg);
void ui_draw_ribbon(void);
void ui_ribbon_update(void);
// ui_cards.c
void ui_draw_cards(void);
// ui_main.c
void ui_draw_main(void);
// ui_presets.c
void ui_draw_presets(void);
// ui_calib.c
void ui_draw_calib_live(void);
void ui_draw_calib(void);
// ui_legacy.c
void ui_draw_effects(void);
void ui_draw_adsr(void);
