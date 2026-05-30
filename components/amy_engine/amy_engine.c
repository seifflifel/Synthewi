#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "amy.h"
#include "amy_engine.h"

static const char *TAG = "AMY_ENGINE";

// ---------------------------------------------------------------------------
// Session synth state (all uint16 params use 0-10000 scale)
// ---------------------------------------------------------------------------
#define NVS_SYNTH_VERSION 2  // bump when param formulas change; forces fresh defaults

static uint8_t  s_wave_id            = AMY_ENGINE_WAVE_SQUARE;
static uint8_t  s_filter_type        = AMY_ENGINE_FILTER_LPF;
static uint16_t s_filter_cutoff      = 5000; // ~814 Hz LPF — audible starting point
static uint16_t s_filter_resonance   = 0;
static uint16_t s_env_attack         = 50;    // ~12 ms
static uint16_t s_env_decay          = 2000;  // ~204 ms
static uint16_t s_env_sustain        = 7000;  // 70 %
static uint16_t s_env_release        = 5000;  // ~2.5 s
static uint16_t s_filter_env_depth   = 0;
static uint16_t s_filter_env_decay   = 1000;  // ~205 ms
static uint16_t s_lfo_rate           = 2000;  // ~2 Hz
static uint16_t s_lfo_depth          = 0;
static uint16_t s_chorus_amount      = 0;
static uint16_t s_pressure_depth     = 0; // 0 = feature off
static uint16_t    s_portamento         = 0; // 0 = instant (no glide)
static uint16_t s_echo_amount        = 0;    // 0 = off
static uint16_t s_echo_feedback      = 3000; // ~27% feedback
static synth_mode_t s_mode             = SYNTH_MODE_CUSTOM;
static uint8_t      s_patch_num        = 0;  // 0-127 within current bank

static float   s_last_freq_hz                   = 0.0f;  // last note freq for cross-pad portamento
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
// Scaling helpers
// Filter cutoff: Spark Synth (Juno) exponential — 13 Hz at min, ~20 kHz at max.
static float    sc_filter_hz(uint16_t v)     { return fminf(13.0f * powf(2.0f, 0.0938f * ((v / 10000.0f) * 127.0f)), 12000.0f); }
// Filter resonance: Spark Synth exponential — Q≈0.7 (flat) at 0, Q≈11 at max.
static float    sc_filter_res(uint16_t v)    { return 0.7f  * powf(2.0f, 4.0f   *  (v / 10000.0f)); }
// LFO rate: Juno formula — ~0.5 Hz at 0, ~20 Hz at max.
static float    sc_lfo_rate_hz(uint16_t v)   { return fmaxf(0.6f * powf(2.0f, 0.04f * ((v / 10000.0f) * 127.0f)) - 0.1f, 0.001f); }
// LFO depth: linear 0–5000 Hz filter swing.
static float    sc_lfo_depth_hz(uint16_t v)  { return (v / 10000.0f) * 5000.0f; }
static uint32_t sc_attack_ms(uint16_t v)     { return (uint32_t)(2.0f  + (v / 10000.0f) * 1998.0f); }
static uint32_t sc_decay_ms(uint16_t v)      { return (uint32_t)(5.0f  + (v / 10000.0f) *  995.0f); }
static float    sc_sustain_f(uint16_t v)     { return v / 10000.0f; }
static uint32_t sc_release_ms(uint16_t v)    { return (uint32_t)(10.0f + (v / 10000.0f) * 4990.0f); }
static float    sc_fenv_depth_hz(uint16_t v) { return (v / 10000.0f) * 8000.0f; }
static uint32_t sc_fenv_decay_ms(uint16_t v) { return (uint32_t)(5.0f  + (v / 10000.0f) * 1995.0f); }
static float    sc_ks_feedback(uint16_t v)   { return 0.85f + (v / 10000.0f) * 0.145f; }
static uint16_t sc_portamento_ms(uint16_t v) { return (uint16_t)((v / 10000.0f) * 500.0f); }

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
    // Version check — stale NVS (different formula) gets ignored; fresh defaults used.
    if (nvs_get_u8(h, "ver", &u8) != ESP_OK || u8 != NVS_SYNTH_VERSION) {
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
    if (nvs_get_u16(h, "echo_amt",  &u16) == ESP_OK) s_echo_amount     = u16;
    if (nvs_get_u16(h, "echo_fb",   &u16) == ESP_OK) s_echo_feedback   = u16;
    nvs_close(h);
}

// Kick the dedicated LFO oscillator with current rate/depth.
static void setup_lfo_osc(void)
{
    amy_event e = amy_default_event();
    e.osc = LFO_OSC;
    e.wave = SINE;
    e.freq_coefs[COEF_CONST] = sc_lfo_rate_hz(s_lfo_rate);
    e.amp_coefs[COEF_CONST]  = 1.0f;  // normalized — depth scaled by COEF_MOD on voice oscs
    e.amp_coefs[COEF_EG0]    = 0.0f;  // disable EG0 so amplitude never gates to 0
    e.amp_coefs[COEF_VEL]    = 0.0f;  // don't scale by velocity
    e.freq_coefs[COEF_NOTE]  = 0.0f;
    e.amp_coefs[COEF_NOTE]   = 0.0f;
    e.velocity               = 1.0f;  // transition osc from SYNTH_OFF → SYNTH_RUNNING
    amy_add_event(&e);
}

// Lightweight filter+LFO update — sends filter fields + mod_source wiring to all pad oscs.
static void apply_filter_to_all_oscs(void)
{
    float base_hz  = sc_filter_hz(s_filter_cutoff);
    float depth_hz = sc_lfo_depth_hz(s_lfo_depth);
    for (uint8_t pad = 0; pad < SYNTH_PAD_COUNT; pad++) {
        amy_event e = amy_default_event();
        e.osc                           = pad;
        e.mod_source                    = LFO_OSC;            // wire LFO osc as modulator
        e.filter_type                   = s_filter_type_map[s_filter_type];
        e.filter_freq_coefs[COEF_CONST] = base_hz;
        e.filter_freq_coefs[COEF_EG1]   = sc_fenv_depth_hz(s_filter_env_depth);
        e.filter_freq_coefs[COEF_MOD]   = depth_hz;          // LFO output × depth_hz = Hz swing
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
        e.filter_freq_coefs[COEF_EG1]   = sc_fenv_depth_hz(s_filter_env_depth);
        e.filter_freq_coefs[COEF_MOD]   = sc_lfo_depth_hz(s_lfo_depth);
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
    cfg.features.reverb         = 0; // removed — frees ~108 KB PSRAM for future looping
    cfg.features.echo           = 1;
    cfg.features.chorus         = 0;
    cfg.ram_caps_delay          = MALLOC_CAP_SPIRAM; // route all delay lines to 2MB PSRAM
    cfg.features.partials       = 0;
    cfg.features.custom         = 0;
    cfg.platform.multicore      = 1;
    cfg.platform.multithread    = 1;
    cfg.max_oscs             = SYNTH_PAD_COUNT * 2; // 16
    cfg.ks_oscs              = 0;
    cfg.max_sequencer_tags   = 1;
    cfg.max_voices           = 10;
    cfg.max_synths           = 3;  // 0=reserved, 1=custom, 2=patch
    cfg.max_memory_patches   = 0;  // not using memory patches
    amy_start(cfg);
    // Give the render task a moment to start its loop before we queue any events.
    vTaskDelay(pdMS_TO_TICKS(50));

    setup_lfo_osc();

    amy_event vol_e = amy_default_event();
    vol_e.volume = 1.0f; // headroom for up to 8 simultaneous oscs at full amplitude
    amy_add_event(&vol_e);

    {
        amy_event e = amy_default_event();
        e.echo_level        = s_echo_amount / 10000.0f;
        e.echo_delay_ms     = 250.0f;
        e.echo_max_delay_ms = 500.0f;  // pre-allocates PSRAM buffer; set only at boot
        e.echo_feedback     = (s_echo_feedback / 10000.0f) * 0.9f;
        e.echo_filter_coef  = 0.5f;
        amy_add_event(&e);
    }
    apply_filter_to_all_oscs();  // prime filter + LFO wiring on all pad oscs at boot

    s_initialized = true;
    ESP_LOGI(TAG, "init OK — I2S on GPIO 38/39/40, wave=%u atk=%u rel=%u",
             s_wave_id, s_env_attack, s_env_release);
}

void amy_engine_note_on(uint8_t pad, uint8_t midi_note)
{
    if (!s_initialized || pad >= SYNTH_PAD_COUNT) return;
    s_pad_midi_note[pad] = midi_note;

    amy_event e = amy_default_event();

    if (s_mode == SYNTH_MODE_CUSTOM) {
        float freq = 440.0f * powf(2.0f, ((float)midi_note - 69.0f) / 12.0f);

        // Cross-pad portamento: prime the idle osc at the last played frequency so AMY's
        // portamento glides from there rather than from logfreq=0.
        // The prep event sets the freq target; after one render block the osc's last_logfreq
        // is updated, so the following note_on glides correctly.
        if (s_portamento > 0 && s_last_freq_hz > 0.0f && !s_pad_active[pad]) {
            amy_event prep = amy_default_event();
            prep.osc = pad;
            prep.freq_coefs[COEF_CONST] = s_last_freq_hz;
            amy_add_event(&prep);
        }

        e.osc                    = pad;
        e.freq_coefs[COEF_CONST] = freq;
        e.pan_coefs[COEF_CONST]  = 0.5f;
        e.amp_coefs[COEF_CONST]  = 1.0f;
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
        e.filter_freq_coefs[COEF_CONST] = sc_filter_hz(s_filter_cutoff);
        e.filter_freq_coefs[COEF_EG1]   = sc_fenv_depth_hz(s_filter_env_depth);
        e.filter_freq_coefs[COEF_MOD]   = sc_lfo_depth_hz(s_lfo_depth);
        e.resonance                     = sc_filter_res(s_filter_resonance);
        e.eg1_times[0] = 5;                                    e.eg1_values[0] = 1.0f;
        e.eg1_times[1] = sc_fenv_decay_ms(s_filter_env_decay); e.eg1_values[1] = 0.0f;
        e.eg1_times[2] = 0;                                    e.eg1_values[2] = 0.0f;
        e.portamento_ms = sc_portamento_ms(s_portamento);
        e.velocity = 1.0f;
        s_last_freq_hz = freq;
    } else {
        e.synth     = SYNTH_CH_PATCH;
        e.midi_note = (float)midi_note;
        e.velocity  = 1.0f;
    }

    amy_add_event(&e);
    s_pad_active[pad] = true;
    ESP_LOGD(TAG, "note_on pad=%u note=%u", pad, midi_note);
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

void amy_engine_set_reverb(uint16_t amount, uint16_t decay)
{
    (void)amount; (void)decay; // reverb disabled — no PSRAM allocated
}

void amy_engine_set_echo(uint16_t amount, uint16_t feedback)
{
    s_echo_amount   = amount;
    s_echo_feedback = feedback;
    amy_event e = amy_default_event();
    e.echo_level       = amount / 10000.0f;
    e.echo_delay_ms    = 250.0f;
    // echo_max_delay_ms intentionally omitted — buffer already allocated in init
    e.echo_feedback    = (feedback / 10000.0f) * 0.9f;
    e.echo_filter_coef = 0.5f;
    amy_add_event(&e);
    ESP_LOGI(TAG, "echo amt=%u fb=%u", amount, feedback);
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
    out->reverb_amount    = 0;
    out->reverb_decay     = 0;
    out->echo_amount      = s_echo_amount;
    out->echo_feedback    = s_echo_feedback;
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

void amy_engine_save_state(void)
{
    nvs_save_all();
}

void amy_engine_park_idle_oscs(void)
{
    if (!s_initialized || s_portamento == 0 || s_last_freq_hz <= 0.0f) return;
    if (s_mode != SYNTH_MODE_CUSTOM) return;
    for (uint8_t pad = 0; pad < SYNTH_PAD_COUNT; pad++) {
        if (s_pad_active[pad]) continue;
        amy_event e = amy_default_event();
        e.osc = pad;
        e.freq_coefs[COEF_CONST] = s_last_freq_hz;
        amy_add_event(&e);
    }
}
