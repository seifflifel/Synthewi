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
static uint8_t  s_wave_id            = AMY_ENGINE_WAVE_SQUARE;
static uint8_t  s_filter_type        = AMY_ENGINE_FILTER_LPF;
// reverb and echo removed — delay lines need large contiguous heap blocks that are
// unavailable after WiFi init fragments SRAM on ESP32-S3.
static uint16_t s_filter_cutoff      = 10000; // fully open by default
static uint16_t s_filter_resonance   = 0;
static uint16_t s_env_attack         = 50;    // ~15 ms
static uint16_t s_env_release        = 5000;  // ~2.5 s
static uint16_t s_filter_env_depth   = 0;
static uint16_t s_filter_env_decay   = 1000;  // ~205 ms
static uint16_t s_lfo_rate           = 2000;  // ~2 Hz
static uint16_t s_lfo_depth          = 0;
static uint16_t s_chorus_amount      = 0;
static uint16_t s_pressure_depth     = 0; // 0 = feature off
static uint16_t    s_portamento         = 0; // 0 = instant (no glide)
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
// ---------------------------------------------------------------------------
static float    sc_filter_hz(uint16_t v)     { return 200.0f + (v / 10000.0f) * 9800.0f; }
static float    sc_filter_res(uint16_t v)    { return (v / 10000.0f) * 0.9f; }
static uint32_t sc_attack_ms(uint16_t v)     { return (uint32_t)(5.0f  + (v / 10000.0f) * 1995.0f); }
static uint32_t sc_release_ms(uint16_t v)    { return (uint32_t)(50.0f + (v / 10000.0f) * 4950.0f); }
static float    sc_lfo_depth_hz(uint16_t v)  { return (v / 10000.0f) * 5000.0f; }
static float    sc_fenv_depth_hz(uint16_t v) { return (v / 10000.0f) * 8000.0f; }
static uint32_t sc_fenv_decay_ms(uint16_t v) { return (uint32_t)(5.0f  + (v / 10000.0f) * 1995.0f); }
// KS feedback: high value = long string decay. Map env_release to 0.85-0.995.
static float    sc_ks_feedback(uint16_t v)   { return 0.85f + (v / 10000.0f) * 0.145f; }
static uint16_t sc_portamento_ms(uint16_t v) { return (uint16_t)((v / 10000.0f) * 500.0f); }

// LFO depth clamped so it can never swing the filter cutoff below 50 Hz.
static float safe_lfo_depth_hz(uint16_t lfo_depth, float base_cutoff_hz) {
    float depth = sc_lfo_depth_hz(lfo_depth);
    float max   = base_cutoff_hz - 50.0f;
    return depth < max ? depth : (max > 0.0f ? max : 0.0f);
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
    nvs_set_u16(h, "env_rel",  s_env_release);
    nvs_set_u16(h, "flt_envd", s_filter_env_depth);
    nvs_set_u16(h, "flt_envc", s_filter_env_decay);
    nvs_set_u16(h, "lfo_rt",   s_lfo_rate);
    nvs_set_u16(h, "lfo_dp",   s_lfo_depth);
    nvs_set_u16(h, "chorus",   s_chorus_amount);
    nvs_set_u16(h, "pres_dep", s_pressure_depth);
    nvs_set_u16(h, "portamento", s_portamento);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open("synth", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t  u8;
    uint16_t u16;
    if (nvs_get_u8(h,  "wave",     &u8)  == ESP_OK) s_wave_id            = u8;
    if (nvs_get_u8(h,  "flt_typ",  &u8)  == ESP_OK) s_filter_type        = u8;
    if (nvs_get_u16(h, "flt_cut",  &u16) == ESP_OK) s_filter_cutoff      = u16;
    if (nvs_get_u16(h, "flt_res",  &u16) == ESP_OK) s_filter_resonance   = u16;
    if (nvs_get_u16(h, "env_atk",  &u16) == ESP_OK) s_env_attack         = u16;
    if (nvs_get_u16(h, "env_rel",  &u16) == ESP_OK) s_env_release        = u16;
    if (nvs_get_u16(h, "flt_envd", &u16) == ESP_OK) s_filter_env_depth   = u16;
    if (nvs_get_u16(h, "flt_envc", &u16) == ESP_OK) s_filter_env_decay   = u16;
    if (nvs_get_u16(h, "lfo_rt",   &u16) == ESP_OK) s_lfo_rate           = u16;
    if (nvs_get_u16(h, "lfo_dp",   &u16) == ESP_OK) s_lfo_depth          = u16;
    if (nvs_get_u16(h, "chorus",   &u16) == ESP_OK) s_chorus_amount      = u16;
    if (nvs_get_u16(h, "pres_dep",  &u16) == ESP_OK) s_pressure_depth    = u16;
    if (nvs_get_u16(h, "portamento",&u16) == ESP_OK) s_portamento        = u16;
    nvs_close(h);
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

        float base_hz = sc_filter_hz(s_filter_cutoff);
        e.filter_type = s_filter_type_map[s_filter_type];
        e.filter_freq_coefs[COEF_CONST] = base_hz;
        e.filter_freq_coefs[COEF_EG1]   = sc_fenv_depth_hz(s_filter_env_depth);
        e.filter_freq_coefs[COEF_MOD]   = safe_lfo_depth_hz(s_lfo_depth, base_hz);
        e.resonance = sc_filter_res(s_filter_resonance);

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
    cfg.features.reverb         = 0;
    cfg.features.echo           = 0;
    cfg.features.chorus         = 0;
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

    amy_event vol_e = amy_default_event();
    vol_e.volume = 10.0f;
    amy_add_event(&vol_e);

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
        // Exact bleep pattern: SINE + freq_coefs + pan + velocity.
        // Using freq_coefs instead of midi_note and no EG/filter to match
        // the one confirmed-working path (startup bleep on osc 15).
        float freq = 440.0f * powf(2.0f, ((float)midi_note - 69.0f) / 12.0f);
        e.osc                    = pad;
        e.wave                   = SINE;
        e.freq_coefs[COEF_CONST] = freq;
        e.pan_coefs[COEF_CONST]  = 0.5f;
        e.velocity               = 1.0f;
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
    if (s_mode == SYNTH_MODE_CUSTOM) setup_custom_synth();
    nvs_save_all();
    ESP_LOGI(TAG, "wave → %u", wave_id);
}

void amy_engine_set_reverb(uint16_t amount, uint16_t decay)
{
    (void)amount; (void)decay; // reverb disabled — delay lines exhaust heap after WiFi init
}

void amy_engine_set_echo(uint16_t amount, uint16_t feedback)
{
    (void)amount; (void)feedback; // echo disabled — delay lines exhaust heap after WiFi init
}

void amy_engine_set_filter(uint16_t cutoff, uint16_t resonance)
{
    s_filter_cutoff    = cutoff;
    s_filter_resonance = resonance;
    if (s_mode == SYNTH_MODE_CUSTOM) setup_custom_synth();
    nvs_save_all();
    ESP_LOGI(TAG, "filter cut=%u res=%u", cutoff, resonance);
}

void amy_engine_set_filter_type(uint8_t type)
{
    if (type > AMY_ENGINE_FILTER_HPF) return;
    s_filter_type = type;
    if (s_mode == SYNTH_MODE_CUSTOM) setup_custom_synth();
    nvs_save_all();
    ESP_LOGI(TAG, "filter_type → %u", type);
}

void amy_engine_set_filter_env(uint16_t depth, uint16_t decay)
{
    s_filter_env_depth = depth;
    s_filter_env_decay = decay;
    if (s_mode == SYNTH_MODE_CUSTOM) setup_custom_synth();
    nvs_save_all();
    ESP_LOGI(TAG, "filter_env depth=%u decay=%u", depth, decay);
}

void amy_engine_set_lfo(uint16_t rate, uint16_t depth)
{
    s_lfo_rate  = rate;
    s_lfo_depth = depth;
    if (s_mode == SYNTH_MODE_CUSTOM) setup_custom_synth();
    nvs_save_all();
    ESP_LOGI(TAG, "lfo rate=%u depth=%u", rate, depth);
}

void amy_engine_set_chorus(uint16_t amount)
{
    (void)amount; // chorus disabled (features.chorus=0 — heap too tight after WiFi+reverb)
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
    nvs_save_all();
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
    s_env_attack  = attack;
    s_env_release = release;
    if (s_mode == SYNTH_MODE_CUSTOM) setup_custom_synth();
    nvs_save_all();
    ESP_LOGI(TAG, "envelope atk=%u rel=%u", attack, release);
}

void amy_engine_get_state(amy_engine_state_t *out)
{
    if (!out) return;
    out->wave_id          = s_wave_id;
    out->filter_type      = s_filter_type;
    out->reverb_amount    = 0;
    out->reverb_decay     = 0;
    out->echo_amount      = 0;
    out->echo_feedback    = 0;
    out->filter_cutoff    = s_filter_cutoff;
    out->filter_resonance = s_filter_resonance;
    out->env_attack       = s_env_attack;
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


