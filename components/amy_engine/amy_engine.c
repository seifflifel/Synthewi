#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "amy.h"
#include "amy_engine.h"

static const char *TAG = "AMY_ENGINE";

// ---------------------------------------------------------------------------
// Session synth state (all uint16 params use 0-10000 scale)
// ---------------------------------------------------------------------------
// Bump when param curves change. v3 → v4 (2026-11): times/cutoff/LFO rate went
// exponential; v3 saves are migrated on load (amy_engine_migrate_v3), not reset.
#define NVS_SYNTH_VERSION 4

static uint8_t  s_wave_id            = AMY_ENGINE_WAVE_SQUARE;
static uint8_t  s_filter_type        = AMY_ENGINE_FILTER_LPF;
static uint16_t s_filter_cutoff      = 5000; // ~1 kHz LPF — audible starting point
static uint16_t s_filter_resonance   = 0;
static uint16_t s_env_attack         = 1500;  // ~3 ms
static uint16_t s_env_decay          = 6000;  // ~275 ms
static uint16_t s_env_sustain        = 7000;  // 70 %
static uint16_t s_env_release        = 6000;  // ~550 ms
static uint16_t s_filter_env_depth   = 0;
static uint16_t s_filter_env_decay   = 5000;  // ~140 ms
static uint16_t s_lfo_rate           = 4000;  // ~0.8 Hz
static uint16_t s_lfo_depth          = 0;
static uint16_t s_chorus_amount      = 0;
static uint16_t s_reverb_amount      = 0;     // 0-10000 → reverb level 0-1 (0 = off, no CPU)
static uint16_t s_reverb_decay       = 7000;  // 0-10000 → liveness 0.5-0.95 (~0.8)
static uint16_t s_pressure_depth     = 0; // 0 = feature off
static uint16_t    s_portamento         = 0; // 0 = instant (no glide)
static uint16_t s_echo_amount        = 0;    // 0 = off
static uint16_t s_echo_feedback      = 3000; // ~27% feedback
static uint16_t s_echo_delay_scaled  = 5000; // 0-10000 → 50-500 ms; 5000 ≈ 275 ms
static synth_mode_t s_mode             = SYNTH_MODE_CUSTOM;
static uint8_t      s_patch_num        = 0;  // 0-127 within current bank

static bool    s_pad_active[SYNTH_PAD_COUNT]    = {false};
static uint8_t s_pad_midi_note[SYNTH_PAD_COUNT] = {0}; // last midi note played per pad

// AMY filter type constants (FILTER_NONE=0, FILTER_LPF=1, FILTER_BPF=2, FILTER_HPF=3)
static const uint8_t s_filter_type_map[3] = { FILTER_LPF, FILTER_BPF, FILTER_HPF };

// CUSTOM mode uses raw oscillators directly (one per pad), matching how
// the startup bleep works — no voice allocator involved.
// PATCH mode (Juno/DX7) still goes through the AMY synth/voice system.
#define SYNTH_CH_PATCH  1  // built-in Juno / DX7 patches

static bool s_initialized = false;

// ---------------------------------------------------------------------------
// Scaling helpers (0-10000 → physical units for AMY)
// Oscillator index reserved for the sine LFO modulator (beyond the 8 pad oscs).
#define LFO_OSC  8

// ---------------------------------------------------------------------------
// Parameter curves (0-10000 → physical) — format v4, 2026-11.
// Single source of truth: the UI displays values through the amy_engine_*()
// wrappers below, so screen and sound can never disagree.
//
// Times and frequencies are EXPONENTIAL: every encoder click (500) multiplies
// the value by a constant ratio, giving fine control at the short/low end.
//   attack      1 ms – 3 s        decay / EG1 decay   5 ms – 4 s
//   release    10 ms – 8 s        filter cutoff      60 Hz – 18 kHz
//   LFO rate  0.1 Hz – 20 Hz      resonance Q        0.7 – 11.2
// Linear: sustain 0–100 %, EG1 depth 0–5 oct, LFO depth 0–3 oct,
//         glide 0–500 ms, echo level/feedback, echo delay 50–500 ms.
//
// NOTE: AMY combines filter-frequency controls in the log2 domain. Only
// filter_freq_coefs[COEF_CONST] is in Hz; COEF_EG1 / COEF_MOD are in OCTAVES.
// ---------------------------------------------------------------------------
#define ATK_MIN_MS     1.0f
#define ATK_MAX_MS  3000.0f
#define DEC_MIN_MS     5.0f
#define DEC_MAX_MS  4000.0f
#define REL_MIN_MS    10.0f
#define REL_MAX_MS  8000.0f
#define FLT_MIN_HZ    60.0f
#define FLT_MAX_HZ 18000.0f
#define LFO_MIN_HZ     0.1f
#define LFO_MAX_HZ    20.0f
#define FENV_DEC_MIN_MS    5.0f
#define FENV_DEC_MAX_MS 4000.0f
#define LFO_DEPTH_MAX_OCT  3.0f
#define FENV_DEPTH_MAX_OCT 5.0f

// lo · (hi/lo)^(v/10000)
static float exp_map(uint16_t v, float lo, float hi) { return lo * powf(hi / lo, v / 10000.0f); }
// Inverse of exp_map, clamped to 0..10000 (used for migrating old saved values).
static uint16_t exp_unmap(float x, float lo, float hi)
{
    if (x <= lo) return 0;
    if (x >= hi) return 10000;
    return (uint16_t)lroundf(10000.0f * logf(x / lo) / logf(hi / lo));
}

static float    sc_filter_hz(uint16_t v)      { return exp_map(v, FLT_MIN_HZ, FLT_MAX_HZ); }
// Filter resonance: Q≈0.7 (flat) at 0, Q≈11.2 at max.
static float    sc_filter_res(uint16_t v)     { return 0.7f * powf(2.0f, 4.0f * (v / 10000.0f)); }
static float    sc_lfo_rate_hz(uint16_t v)    { return exp_map(v, LFO_MIN_HZ, LFO_MAX_HZ); }
// LFO depth: ± octaves of filter swing around the cutoff.
static float    sc_lfo_depth_oct(uint16_t v)  { return (v / 10000.0f) * LFO_DEPTH_MAX_OCT; }
static uint32_t sc_attack_ms(uint16_t v)      { return (uint32_t)lroundf(exp_map(v, ATK_MIN_MS, ATK_MAX_MS)); }
static uint32_t sc_decay_ms(uint16_t v)       { return (uint32_t)lroundf(exp_map(v, DEC_MIN_MS, DEC_MAX_MS)); }
static float    sc_sustain_f(uint16_t v)      { return v / 10000.0f; }
static uint32_t sc_release_ms(uint16_t v)     { return (uint32_t)lroundf(exp_map(v, REL_MIN_MS, REL_MAX_MS)); }
// Filter envelope (EG1) depth: octaves above the cutoff at the envelope peak.
static float    sc_fenv_depth_oct(uint16_t v) { return (v / 10000.0f) * FENV_DEPTH_MAX_OCT; }
static uint32_t sc_fenv_decay_ms(uint16_t v)  { return (uint32_t)lroundf(exp_map(v, FENV_DEC_MIN_MS, FENV_DEC_MAX_MS)); }
static float    sc_ks_feedback(uint16_t v)    { return 0.85f + (v / 10000.0f) * 0.145f; }
static uint16_t sc_portamento_ms(uint16_t v)  { return (uint16_t)((v / 10000.0f) * 500.0f); }
static float    sc_echo_delay_ms(uint16_t v)  { return fmaxf(50.0f + (v / 10000.0f) * 450.0f, 10.0f); }

// Touch expression (velocity at note-on + continuous pressure while held).
// e is 0.10..1.00 (touch MIN..MAX calibration), f = normalized 0..1.
//   amplitude = 0.10 + 0.90 * f^EXPR_AMP_CURVE   (concave "boost" curve: partial
//               pressure is already loud — linear felt like the volume was fixed
//               because typical touches sit at 20-40 %; MIN/MAX levels unchanged)
//   cutoff    = preset cutoff * 2^(EXPR_FILTER_OCT * f)   (light touch = preset sound)
// Future SETTINGS (UI redesign): target amp / filter / both, filter depth
// (user found +2 oct too little → 3 for now), amp curve.
#define EXPR_E_MIN       0.10f
#define EXPR_FILTER_OCT  3.0f
#define EXPR_AMP_CURVE   0.6f
static bool s_expr_amp    = true;
static bool s_expr_filter = true;

static float expr_norm(float e)
{
    float f = (e - EXPR_E_MIN) / (1.0f - EXPR_E_MIN);
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

static float expr_gain(float e)
{
    return EXPR_E_MIN + (1.0f - EXPR_E_MIN) * powf(expr_norm(e), EXPR_AMP_CURVE);
}

static float expr_cutoff_hz(float e)
{
    float base = sc_filter_hz(s_filter_cutoff);
    if (!s_expr_filter) return base;
    return base * powf(2.0f, EXPR_FILTER_OCT * expr_norm(e));
}

// Public display wrappers (see amy_engine.h)
uint32_t amy_engine_attack_ms(uint16_t v)       { return sc_attack_ms(v); }
uint32_t amy_engine_decay_ms(uint16_t v)        { return sc_decay_ms(v); }
uint32_t amy_engine_release_ms(uint16_t v)      { return sc_release_ms(v); }
float    amy_engine_filter_hz(uint16_t v)       { return sc_filter_hz(v); }
float    amy_engine_filter_q(uint16_t v)        { return sc_filter_res(v); }
float    amy_engine_lfo_hz(uint16_t v)          { return sc_lfo_rate_hz(v); }
float    amy_engine_lfo_depth_oct(uint16_t v)   { return sc_lfo_depth_oct(v); }
float    amy_engine_fenv_depth_oct(uint16_t v)  { return sc_fenv_depth_oct(v); }
uint32_t amy_engine_fenv_decay_ms(uint16_t v)   { return sc_fenv_decay_ms(v); }
uint32_t amy_engine_glide_ms(uint16_t v)        { return sc_portamento_ms(v); }

// Convert values saved with the v3 (linear) curves to the v4 curves so saved
// sounds keep their physical attack/decay/release/cutoff/EG1-decay/LFO-rate.
// Any pointer may be NULL.
void amy_engine_migrate_v3(uint16_t *atk, uint16_t *dec, uint16_t *rel,
                           uint16_t *cut, uint16_t *fenv_dec, uint16_t *lfo_rt)
{
    if (atk)      *atk      = exp_unmap(2.0f  + (*atk / 10000.0f) * 1998.0f, ATK_MIN_MS, ATK_MAX_MS);
    if (dec)      *dec      = exp_unmap(5.0f  + (*dec / 10000.0f) *  995.0f, DEC_MIN_MS, DEC_MAX_MS);
    if (rel)      *rel      = exp_unmap(10.0f + (*rel / 10000.0f) * 4990.0f, REL_MIN_MS, REL_MAX_MS);
    if (cut)      *cut      = exp_unmap(fminf(200.0f * powf(2.0f, 5.9f * (*cut / 10000.0f)), 12000.0f),
                                        FLT_MIN_HZ, FLT_MAX_HZ);
    if (fenv_dec) *fenv_dec = exp_unmap(5.0f + (*fenv_dec / 10000.0f) * 1995.0f,
                                        FENV_DEC_MIN_MS, FENV_DEC_MAX_MS);
    if (lfo_rt)   *lfo_rt   = exp_unmap(fmaxf(0.6f * powf(2.0f, 0.04f * ((*lfo_rt / 10000.0f) * 127.0f)) - 0.1f, 0.001f),
                                        LFO_MIN_HZ, LFO_MAX_HZ);
}

// ---------------------------------------------------------------------------
// NVS persistence
// ---------------------------------------------------------------------------
static void nvs_save_all(void)
{
    nvs_handle_t h;
    if (nvs_open("synth", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h,  "wave",     s_wave_id);
    nvs_set_u8(h,  "flt_typ",  s_filter_type);
    nvs_set_u16(h, "flt_cut",  s_filter_cutoff);
    nvs_set_u16(h, "flt_res",  s_filter_resonance);
    nvs_set_u16(h, "env_atk",  s_env_attack);
    nvs_set_u16(h, "env_dec",  s_env_decay);
    nvs_set_u16(h, "env_sus",  s_env_sustain);
    nvs_set_u16(h, "env_rel",  s_env_release);
    nvs_set_u16(h, "flt_envd", s_filter_env_depth);
    nvs_set_u16(h, "flt_envc", s_filter_env_decay);
    nvs_set_u16(h, "lfo_rt",   s_lfo_rate);
    nvs_set_u16(h, "lfo_dp",   s_lfo_depth);
    nvs_set_u16(h, "chorus",   s_chorus_amount);
    nvs_set_u16(h, "pres_dep",  s_pressure_depth);
    nvs_set_u16(h, "portamento",s_portamento);
    nvs_set_u16(h, "echo_amt",  s_echo_amount);
    nvs_set_u16(h, "echo_fb",   s_echo_feedback);
    nvs_set_u16(h, "echo_dly",  s_echo_delay_scaled);
    nvs_set_u16(h, "rvb_amt",   s_reverb_amount);
    nvs_set_u16(h, "rvb_dec",   s_reverb_decay);
    nvs_set_u8(h,  "ver",       NVS_SYNTH_VERSION);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open("synth", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t  u8  = 0;
    uint16_t u16 = 0;
    // Version check — v3 is migrated below; anything else unknown → fresh defaults.
    uint8_t ver = 0;
    if (nvs_get_u8(h, "ver", &ver) != ESP_OK || (ver != NVS_SYNTH_VERSION && ver != 3)) {
        nvs_close(h);
        ESP_LOGI(TAG, "NVS version mismatch — using defaults");
        return;
    }
    if (nvs_get_u8(h,  "wave",     &u8)  == ESP_OK) s_wave_id            = u8;
    if (nvs_get_u8(h,  "flt_typ",  &u8)  == ESP_OK) s_filter_type        = u8;
    if (nvs_get_u16(h, "flt_cut",  &u16) == ESP_OK) s_filter_cutoff      = u16;
    if (nvs_get_u16(h, "flt_res",  &u16) == ESP_OK) s_filter_resonance   = u16;
    if (nvs_get_u16(h, "env_atk",  &u16) == ESP_OK) s_env_attack         = u16;
    if (nvs_get_u16(h, "env_dec",  &u16) == ESP_OK) s_env_decay          = u16;
    if (nvs_get_u16(h, "env_sus",  &u16) == ESP_OK) s_env_sustain        = u16;
    if (nvs_get_u16(h, "env_rel",  &u16) == ESP_OK) s_env_release        = u16;
    if (nvs_get_u16(h, "flt_envd", &u16) == ESP_OK) s_filter_env_depth   = u16;
    if (nvs_get_u16(h, "flt_envc", &u16) == ESP_OK) s_filter_env_decay   = u16;
    if (nvs_get_u16(h, "lfo_rt",   &u16) == ESP_OK) s_lfo_rate           = u16;
    if (nvs_get_u16(h, "lfo_dp",   &u16) == ESP_OK) s_lfo_depth          = u16;
    if (nvs_get_u16(h, "chorus",   &u16) == ESP_OK) s_chorus_amount      = u16;
    if (nvs_get_u16(h, "pres_dep",  &u16) == ESP_OK) s_pressure_depth  = u16;
    if (nvs_get_u16(h, "portamento",&u16) == ESP_OK) s_portamento      = u16;
    if (nvs_get_u16(h, "echo_amt",  &u16) == ESP_OK) s_echo_amount        = u16;
    if (nvs_get_u16(h, "echo_fb",   &u16) == ESP_OK) s_echo_feedback      = u16;
    if (nvs_get_u16(h, "echo_dly",  &u16) == ESP_OK) s_echo_delay_scaled  = u16;
    // Reverb keys are new (2026-11); older saves simply keep the defaults (off).
    // Named rvb_* because older firmware left stale "rev_*" keys in NVS.
    if (nvs_get_u16(h, "rvb_amt",   &u16) == ESP_OK) s_reverb_amount      = u16;
    if (nvs_get_u16(h, "rvb_dec",   &u16) == ESP_OK) s_reverb_decay       = u16;
    nvs_close(h);

    if (ver == 3) {
        amy_engine_migrate_v3(&s_env_attack, &s_env_decay, &s_env_release,
                              &s_filter_cutoff, &s_filter_env_decay, &s_lfo_rate);
        nvs_save_all(); // persist as v4
        ESP_LOGI(TAG, "NVS synth state migrated v3 → v4");
    }
}

// Configure the dedicated LFO oscillator (rate only — depth is COEF_MOD on the pad oscs).
// It must NOT get a note-on: velocity>0 sets its status to SYNTH_AUDIBLE, which
// mixed a full-scale 2 Hz sine into the output and advanced its phase twice per
// block. As a mod source (status set by the pads' mod_source) AMY evaluates it
// on demand via compute_mod_value(); each pad note-on re-triggers its phase.
// With all amp coefs except CONST at 0, its amplitude is a constant 1.0.
static void setup_lfo_osc(void)
{
    amy_event e = amy_default_event();
    e.osc = LFO_OSC;
    e.wave = SINE;
    e.freq_coefs[COEF_CONST] = sc_lfo_rate_hz(s_lfo_rate);
    e.freq_coefs[COEF_NOTE]  = 0.0f;
    e.amp_coefs[COEF_CONST]  = 1.0f;
    e.amp_coefs[COEF_EG0]    = 0.0f;
    e.amp_coefs[COEF_VEL]    = 0.0f;
    e.amp_coefs[COEF_NOTE]   = 0.0f;
    amy_add_event(&e);
}

// Lightweight filter+LFO update — sends filter fields + mod_source wiring to all pad oscs.
static void apply_filter_to_all_oscs(void)
{
    float base_hz   = sc_filter_hz(s_filter_cutoff);
    float depth_oct = sc_lfo_depth_oct(s_lfo_depth);
    for (uint8_t pad = 0; pad < SYNTH_PAD_COUNT; pad++) {
        amy_event e = amy_default_event();
        e.osc                           = pad;
        e.mod_source                    = LFO_OSC;            // wire LFO osc as modulator
        e.filter_type                   = s_filter_type_map[s_filter_type];
        e.filter_freq_coefs[COEF_CONST] = base_hz;            // Hz
        e.filter_freq_coefs[COEF_EG1]   = sc_fenv_depth_oct(s_filter_env_depth); // octaves
        e.filter_freq_coefs[COEF_MOD]   = depth_oct;          // LFO (±1) × octaves
        e.resonance                     = sc_filter_res(s_filter_resonance);
        e.eg1_times[0] = 5;                                    e.eg1_values[0] = 1.0f;
        e.eg1_times[1] = sc_fenv_decay_ms(s_filter_env_decay); e.eg1_values[1] = 0.0f;
        e.eg1_times[2] = 0;                                    e.eg1_values[2] = 0.0f;
        amy_add_event(&e);
    }
}

// Configure pads as raw oscillators — one osc per pad (osc index = pad index).
// This is the same mechanism as the startup bleep: direct e.osc control,
// no synth voice allocator involved, so it cannot silently fail to route notes.
static void setup_custom_synth(void)
{
    uint32_t atk = sc_attack_ms(s_env_attack);
    uint32_t rel = sc_release_ms(s_env_release);

    for (uint8_t pad = 0; pad < SYNTH_PAD_COUNT; pad++) {
        amy_event e = amy_default_event();
        e.osc = pad;

        if (s_wave_id == AMY_ENGINE_WAVE_SQUARE) {
            e.wave = PULSE;
            e.duty_coefs[COEF_CONST] = 0.5f;
        } else if (s_wave_id == AMY_ENGINE_WAVE_PULSE) {
            e.wave = PULSE;
            e.duty_coefs[COEF_CONST] = 0.2f;
        } else {
            e.wave = s_wave_id;
        }
        if (s_wave_id == AMY_ENGINE_WAVE_KS)
            e.feedback = sc_ks_feedback(s_env_release);

        e.amp_coefs[COEF_CONST]  = 1.0f;
        e.amp_coefs[COEF_VEL]    = 1.0f;
        e.amp_coefs[COEF_EG0]    = 1.0f;
        e.pan_coefs[COEF_CONST]  = 0.5f; // center pan — pan=0 (default) routes to left only

        e.eg0_times[0] = atk;  e.eg0_values[0] = 1.0f;
        e.eg0_times[1] = 200;  e.eg0_values[1] = 0.7f;
        e.eg0_times[2] = rel;  e.eg0_values[2] = 0.0f;

        e.mod_source                    = LFO_OSC;
        e.filter_type                   = s_filter_type_map[s_filter_type];
        e.filter_freq_coefs[COEF_CONST] = sc_filter_hz(s_filter_cutoff);
        e.filter_freq_coefs[COEF_EG1]   = sc_fenv_depth_oct(s_filter_env_depth);
        e.filter_freq_coefs[COEF_MOD]   = sc_lfo_depth_oct(s_lfo_depth);
        e.resonance                     = sc_filter_res(s_filter_resonance);
        e.eg1_times[0] = 5;                                    e.eg1_values[0] = 1.0f;
        e.eg1_times[1] = sc_fenv_decay_ms(s_filter_env_decay); e.eg1_values[1] = 0.0f;
        e.eg1_times[2] = 0;                                    e.eg1_values[2] = 0.0f;

        e.portamento_ms = sc_portamento_ms(s_portamento);

        amy_add_event(&e);
    }
}

// ---------------------------------------------------------------------------
// PATCH mode helpers
// ---------------------------------------------------------------------------

static void apply_patch_synth(void)
{
    amy_event rst = amy_default_event();
    rst.reset_osc = RESET_ALL_OSCS;
    amy_add_event(&rst);

    uint16_t actual_patch = (s_mode == SYNTH_MODE_DX7)
                            ? (uint16_t)(128 + s_patch_num)
                            : (uint16_t)s_patch_num; // JUNO: 0-127 direct

    amy_event e = amy_default_event();
    e.patch_number = actual_patch;
    e.num_voices   = 2; // 2 voices = 12 oscs (Juno) or 16 oscs (DX7), fits max_oscs=16
    e.synth        = SYNTH_CH_PATCH;
    amy_add_event(&e);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void amy_engine_init(void)
{
    if (s_initialized) return;

    nvs_load_all();

    amy_config_t cfg            = amy_default_config();
    // AMY drives I2S directly — same as Spark Synth
    cfg.audio                   = AMY_AUDIO_IS_I2S;
    cfg.i2s_bclk                = 38;
    cfg.i2s_lrc                 = 39;
    cfg.i2s_dout                = 40;
    cfg.midi                    = AMY_MIDI_IS_NONE;
    cfg.features.startup_bleep  = 1; // boot beep confirms audio path works
    cfg.features.default_synths = 0;
    cfg.features.reverb         = 1; // ~108 KB PSRAM delay lines, allocated in init (before the looper)
    cfg.features.echo           = 1;
    cfg.features.chorus         = 0;
    cfg.ram_caps_delay          = MALLOC_CAP_SPIRAM; // route all delay lines to 2MB PSRAM
    cfg.features.partials       = 0;
    cfg.features.custom         = 0;
    cfg.platform.multicore      = 1;
    cfg.platform.multithread    = 1;
    cfg.max_oscs             = SYNTH_PAD_COUNT * 2; // 16
    cfg.ks_oscs              = 4;
    cfg.max_sequencer_tags   = 1;
    cfg.max_voices           = 10;
    cfg.max_synths           = 3;  // 0=reserved, 1=custom, 2=patch
    cfg.max_memory_patches   = 0;  // not using memory patches
    amy_start(cfg);
    // Give the render task a moment to start its loop before we queue any events.
    vTaskDelay(pdMS_TO_TICKS(50));

    setup_lfo_osc();

    amy_event vol_e = amy_default_event();
    // 0.5 = 6 dB of internal headroom: AMY's soft clipper (and its -6 dBFS
    // output ceiling on ESP32) is no longer reached by 8 voices + reverb/echo
    // build-ups. The 6 dB are added back after AMY by the output limiter
    // (components/looper/output_limiter.c, LIM_MAKEUP), which also catches peaks.
    vol_e.volume = 0.5f;
    amy_add_event(&vol_e);

    {
        // Echo delay lines are allocated by AMY the first time the echo level is
        // > 0 (500 ms → 32768 samples × 2 ch × 4 B ≈ 256 KB PSRAM). Always force
        // that allocation now, even if the saved level is 0, so it happens before
        // looper_init() claims the remaining PSRAM — otherwise turning echo on
        // later fails (256 KB doesn't fit in the looper's headroom).
        amy_event e = amy_default_event();
        e.echo_level        = 0.01f;   // > 0 → allocate
        e.echo_delay_ms     = sc_echo_delay_ms(s_echo_delay_scaled);
        e.echo_max_delay_ms = 500.0f;  // covers full 50-500 ms range
        e.echo_feedback     = (s_echo_feedback / 10000.0f) * 0.9f;
        e.echo_filter_coef  = 0.5f;
        amy_add_event(&e);
        amy_event lvl = amy_default_event();
        lvl.echo_level      = s_echo_amount / 10000.0f;  // real saved level (may be 0)
        amy_add_event(&lvl);
    }
    apply_filter_to_all_oscs();  // prime filter + LFO wiring on all pad oscs at boot

    // Reverb: AMY allocates its delay lines (~108 KB PSRAM) the first time the
    // level goes above 0, and only once. Force that now so the allocation
    // happens before looper_init() claims the remaining PSRAM, then apply the
    // real level (0 = off: AMY skips reverb processing entirely).
    {
        amy_event e = amy_default_event();
        e.reverb_level = 0.01f;
        amy_add_event(&e);
    }
    amy_engine_set_reverb(s_reverb_amount, s_reverb_decay);

    s_initialized = true;
    ESP_LOGI(TAG, "init OK — I2S on GPIO 38/39/40, wave=%u atk=%u rel=%u",
             s_wave_id, s_env_attack, s_env_release);
}

void amy_engine_note_on(uint8_t pad, uint8_t midi_note, float velocity)
{
    if (!s_initialized || pad >= SYNTH_PAD_COUNT) return;
    if (velocity < 0.01f) velocity = 0.01f;
    if (velocity > 1.0f)  velocity = 1.0f;
    s_pad_midi_note[pad] = midi_note;

    amy_event e = amy_default_event();

    if (s_mode == SYNTH_MODE_CUSTOM) {
        float freq = 440.0f * powf(2.0f, ((float)midi_note - 69.0f) / 12.0f);

        // Cross-pad glide is handled inside AMY (patched note-on): each note
        // glides from the previous note-on pitch of any pad when portamento_ms > 0.
        e.osc                    = pad;
        e.freq_coefs[COEF_CONST] = freq;
        e.pan_coefs[COEF_CONST]  = 0.5f;
        // Velocity is applied as the note's GAIN (amp CONST, linear: 0.1 = -20 dB),
        // not as AMY velocity: sending a new velocity to a sounding osc retriggers
        // it, while a new amp CONST just changes its level (see
        // amy_engine_set_expression). AMY velocity stays 1.0 = plain note-on.
        e.amp_coefs[COEF_CONST]  = s_expr_amp ? expr_gain(velocity) : 1.0f;
        e.amp_coefs[COEF_VEL]    = 0.0f;
        e.amp_coefs[COEF_EG0]    = 1.0f;
        e.eg0_times[0]  = sc_attack_ms(s_env_attack);   e.eg0_values[0] = 1.0f;
        e.eg0_times[1]  = sc_decay_ms(s_env_decay);     e.eg0_values[1] = sc_sustain_f(s_env_sustain);
        e.eg0_times[2]  = sc_release_ms(s_env_release); e.eg0_values[2] = 0.0f;
        if (s_wave_id == AMY_ENGINE_WAVE_SQUARE) {
            e.wave = PULSE; e.duty_coefs[COEF_CONST] = 0.5f;
        } else {
            e.wave = s_wave_id;
        }
        // Filter applied on every note so it stays current after wave/type changes.
        e.mod_source                    = LFO_OSC;
        e.filter_type                   = s_filter_type_map[s_filter_type];
        e.filter_freq_coefs[COEF_CONST] = expr_cutoff_hz(velocity);   // touch opens the filter
        e.filter_freq_coefs[COEF_EG1]   = sc_fenv_depth_oct(s_filter_env_depth);
        e.filter_freq_coefs[COEF_MOD]   = sc_lfo_depth_oct(s_lfo_depth);
        e.resonance                     = sc_filter_res(s_filter_resonance);
        e.eg1_times[0] = 5;                                    e.eg1_values[0] = 1.0f;
        e.eg1_times[1] = sc_fenv_decay_ms(s_filter_env_decay); e.eg1_values[1] = 0.0f;
        e.eg1_times[2] = 0;                                    e.eg1_values[2] = 0.0f;
        e.portamento_ms = sc_portamento_ms(s_portamento);
        e.velocity = 1.0f;
    } else {
        e.synth     = SYNTH_CH_PATCH;
        e.midi_note = (float)midi_note;
        e.velocity  = velocity;
    }

    amy_add_event(&e);
    s_pad_active[pad] = true;
    ESP_LOGD(TAG, "note_on pad=%u note=%u", pad, midi_note);
}

// Continuous touch expression for a sounding note (CUSTOM mode only): updates
// its gain and filter cutoff without retriggering it. e = 0.10..1.00.
void amy_engine_set_expression(uint8_t pad, float e_in)
{
    if (!s_initialized || pad >= SYNTH_PAD_COUNT || s_mode != SYNTH_MODE_CUSTOM) return;
    if (!s_pad_active[pad]) return;
    float ex = e_in < 0.01f ? 0.01f : e_in > 1.0f ? 1.0f : e_in;
    amy_event e = amy_default_event();
    e.osc = pad;
    if (s_expr_amp)    e.amp_coefs[COEF_CONST]        = expr_gain(ex);
    if (s_expr_filter) e.filter_freq_coefs[COEF_CONST] = expr_cutoff_hz(ex);
    amy_add_event(&e);
}

void amy_engine_note_off(uint8_t pad)
{
    if (!s_initialized || pad >= SYNTH_PAD_COUNT) return;
    amy_event e = amy_default_event();
    if (s_mode == SYNTH_MODE_CUSTOM) {
        e.osc      = pad;
        e.velocity = 0.0f;
    } else {
        e.synth     = SYNTH_CH_PATCH;
        e.midi_note = (float)s_pad_midi_note[pad];
        e.velocity  = 0.0f;
    }
    amy_add_event(&e);
    s_pad_active[pad] = false;
    ESP_LOGD(TAG, "note_off pad=%u", pad);
}

void amy_engine_set_wave(uint8_t wave_id)
{
    if (wave_id >= AMY_ENGINE_WAVE_COUNT) return;
    s_wave_id = wave_id;
    ESP_LOGI(TAG, "wave → %u", wave_id);
}

// Wet level is added on top of the full dry mix and the tail feeds back on
// itself, so with long releases + fast playing the energy builds up. 1.0 / 0.95
// ran away into the output clipper (2026-11); 0.6 / 0.92 still gives a big hall.
#define REVERB_LEVEL_MAX     0.6f
#define REVERB_LIVENESS_MIN  0.5f
#define REVERB_LIVENESS_MAX  0.92f
void amy_engine_set_reverb(uint16_t amount, uint16_t decay)
{
    s_reverb_amount = amount;
    s_reverb_decay  = decay;
    amy_event e = amy_default_event();
    e.reverb_level    = (amount / 10000.0f) * REVERB_LEVEL_MAX;
    e.reverb_liveness = REVERB_LIVENESS_MIN + (decay / 10000.0f) * (REVERB_LIVENESS_MAX - REVERB_LIVENESS_MIN);
    amy_add_event(&e);
    ESP_LOGI(TAG, "reverb amt=%u decay=%u (level %.2f liveness %.2f)",
             amount, decay, e.reverb_level, e.reverb_liveness);
}

void amy_engine_set_echo(uint16_t amount, uint16_t feedback)
{
    s_echo_amount   = amount;
    s_echo_feedback = feedback;
    amy_event e = amy_default_event();
    e.echo_level       = amount / 10000.0f;
    e.echo_delay_ms    = sc_echo_delay_ms(s_echo_delay_scaled);
    // echo_max_delay_ms intentionally omitted — buffer already allocated in init
    e.echo_feedback    = (feedback / 10000.0f) * 0.9f;
    e.echo_filter_coef = 0.5f;
    amy_add_event(&e);
    ESP_LOGI(TAG, "echo amt=%u fb=%u dly=%.0fms", amount, feedback, sc_echo_delay_ms(s_echo_delay_scaled));
}

void amy_engine_set_echo_delay(uint16_t delay)
{
    s_echo_delay_scaled = delay;
    amy_event e = amy_default_event();
    e.echo_delay_ms = sc_echo_delay_ms(delay);
    amy_add_event(&e);
    ESP_LOGI(TAG, "echo_delay → %u (%.0f ms)", delay, sc_echo_delay_ms(delay));
}

void amy_engine_set_filter(uint16_t cutoff, uint16_t resonance)
{
    s_filter_cutoff    = cutoff;
    s_filter_resonance = resonance;
    if (s_mode == SYNTH_MODE_CUSTOM) apply_filter_to_all_oscs();
    ESP_LOGI(TAG, "filter cut=%u res=%u", cutoff, resonance);
}

void amy_engine_set_filter_type(uint8_t type)
{
    if (type > AMY_ENGINE_FILTER_HPF) return;
    s_filter_type = type;
    if (s_mode == SYNTH_MODE_CUSTOM) apply_filter_to_all_oscs();
    ESP_LOGI(TAG, "filter_type → %u", type);
}

void amy_engine_set_filter_env(uint16_t depth, uint16_t decay)
{
    s_filter_env_depth = depth;
    s_filter_env_decay = decay;
    if (s_mode == SYNTH_MODE_CUSTOM) apply_filter_to_all_oscs();
    ESP_LOGI(TAG, "filter_env depth=%u decay=%u", depth, decay);
}

void amy_engine_set_lfo(uint16_t rate, uint16_t depth)
{
    s_lfo_rate  = rate;
    s_lfo_depth = depth;
    setup_lfo_osc();                                          // update rate/depth on LFO osc
    if (s_mode == SYNTH_MODE_CUSTOM) apply_filter_to_all_oscs(); // update COEF_MOD on voice oscs
    ESP_LOGI(TAG, "lfo rate=%u depth=%u (%.2f Hz)", rate, depth, sc_lfo_rate_hz(rate));
}

void amy_engine_set_chorus(uint16_t amount)
{
    (void)amount; // chorus disabled (features.chorus=0)
}

void amy_engine_set_pressure_depth(uint16_t depth)
{
    s_pressure_depth = depth;
    nvs_save_all();
    ESP_LOGI(TAG, "pressure_depth → %u", depth);
}

void amy_engine_set_glide(uint16_t glide)
{
    s_portamento = glide;
    ESP_LOGI(TAG, "glide → %u (%u ms)", glide, sc_portamento_ms(glide));
}

void amy_engine_set_mode(uint8_t mode)
{
    if (mode > (uint8_t)SYNTH_MODE_DX7) return;
    s_mode = (synth_mode_t)mode;
    for (int i = 0; i < SYNTH_PAD_COUNT; i++) s_pad_active[i] = false;
    if (s_mode == SYNTH_MODE_CUSTOM) {
        amy_event rst = amy_default_event();
        rst.reset_osc = RESET_ALL_OSCS;
        amy_add_event(&rst);
        setup_custom_synth();
        setup_lfo_osc();  // restore LFO osc after reset
    } else {
        apply_patch_synth();
    }
    ESP_LOGI(TAG, "mode → %u (patch=%u)", mode, s_patch_num);
}

void amy_engine_set_patch(uint8_t patch_in_bank)
{
    s_patch_num = patch_in_bank > 127 ? 127 : patch_in_bank;
    if (s_mode == SYNTH_MODE_CUSTOM) return;
    apply_patch_synth();
    ESP_LOGI(TAG, "patch → bank=%u actual=%u",
             s_patch_num, s_mode == SYNTH_MODE_DX7 ? 128 + s_patch_num : s_patch_num);
}

void amy_engine_update_pressure(uint8_t pad, float pressure_norm)
{
    // Pressure-modulated filter requires direct osc access — not compatible with
    // the synth voice allocator used in CUSTOM mode (osc assignment is opaque).
    (void)pad; (void)pressure_norm;
}

void amy_engine_set_envelope(uint16_t attack, uint16_t release)
{
    amy_engine_set_adsr(attack, s_env_decay, s_env_sustain, release);
}

void amy_engine_set_adsr(uint16_t attack, uint16_t decay, uint16_t sustain, uint16_t release)
{
    s_env_attack  = attack;
    s_env_decay   = decay;
    s_env_sustain = sustain;
    s_env_release = release;
    ESP_LOGI(TAG, "adsr atk=%u dec=%u sus=%u rel=%u", attack, decay, sustain, release);
}

void amy_engine_get_state(amy_engine_state_t *out)
{
    if (!out) return;
    out->wave_id          = s_wave_id;
    out->filter_type      = s_filter_type;
    out->reverb_amount    = s_reverb_amount;
    out->reverb_decay     = s_reverb_decay;
    out->echo_amount      = s_echo_amount;
    out->echo_feedback    = s_echo_feedback;
    out->echo_delay       = s_echo_delay_scaled;
    out->filter_cutoff    = s_filter_cutoff;
    out->filter_resonance = s_filter_resonance;
    out->env_attack       = s_env_attack;
    out->env_decay        = s_env_decay;
    out->env_sustain      = s_env_sustain;
    out->env_release      = s_env_release;
    out->filter_env_depth = s_filter_env_depth;
    out->filter_env_decay = s_filter_env_decay;
    out->lfo_rate         = s_lfo_rate;
    out->lfo_depth        = s_lfo_depth;
    out->chorus_amount    = s_chorus_amount;
    out->pressure_depth   = s_pressure_depth;
    out->glide            = s_portamento;
    out->synth_mode       = (uint8_t)s_mode;
    out->patch_num        = s_patch_num;
}

// ── Panic ─────────────────────────────────────────────────────
// Silence everything right now: all pad voices AND the echo/reverb tails.
// Notes: amp CONST 0 + note-off on every pad osc (amp_coefs are re-sent by the
//        next note_on, so pads work normally afterwards).
// Tails: AMY skips reverb processing when its level is 0, so zeroing the level
//        would just freeze the tail and bring it back later. Instead feedback is
//        set to 0 (echo) / liveness 0 with a tiny level (reverb) so the delay
//        lines fill with silence; after PANIC_FLUSH_US (> 500 ms max echo delay)
//        the user's echo/reverb settings are restored.
#define PANIC_FLUSH_US 700000
static esp_timer_handle_t s_panic_timer = NULL;

static void panic_restore_cb(void *arg)
{
    (void)arg;
    amy_engine_set_echo(s_echo_amount, s_echo_feedback);
    amy_engine_set_reverb(s_reverb_amount, s_reverb_decay);
    ESP_LOGI(TAG, "panic: effects restored");
}

void amy_engine_panic(void)
{
    if (!s_initialized) return;
    for (uint8_t pad = 0; pad < SYNTH_PAD_COUNT; pad++) {
        amy_event e = amy_default_event();
        e.osc = pad;
        e.amp_coefs[COEF_CONST] = 0.0f;
        e.velocity = 0.0f;
        amy_add_event(&e);
        s_pad_active[pad] = false;
    }
    {   // flush echo
        amy_event e = amy_default_event();
        e.echo_level    = 0.0f;
        e.echo_feedback = 0.0f;
        amy_add_event(&e);
    }
    {   // flush reverb (keep it processing at a negligible level)
        amy_event e = amy_default_event();
        e.reverb_level    = 0.001f;
        e.reverb_liveness = 0.0f;
        amy_add_event(&e);
    }
    if (s_panic_timer == NULL) {
        const esp_timer_create_args_t args = { .callback = panic_restore_cb, .name = "panic" };
        if (esp_timer_create(&args, &s_panic_timer) != ESP_OK) s_panic_timer = NULL;
    }
    if (s_panic_timer) {
        esp_timer_stop(s_panic_timer);                 // restart if panicked again
        esp_timer_start_once(s_panic_timer, PANIC_FLUSH_US);
    }
    ESP_LOGW(TAG, "PANIC: all notes off, effect tails flushed");
}

void amy_engine_save_state(void)
{
    nvs_save_all();
}
