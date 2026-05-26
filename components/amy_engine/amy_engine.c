#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "amy.h"
#include "amy_engine.h"

static const char *TAG = "AMY_ENGINE";

// ---------------------------------------------------------------------------
// Embedded WAV state (kept for USB transport quality testing)
// ---------------------------------------------------------------------------
extern const uint8_t _binary_sleepwalk_wav_start[];
extern const uint8_t _binary_sleepwalk_wav_end[];

typedef struct {
    const uint8_t *data_start;
    size_t         data_offset;
    uint16_t       channels;
    uint32_t       sample_rate;
    size_t         frame_count;
    size_t         frame_cursor;
    bool           ready;
} embedded_wav_t;

static embedded_wav_t s_wav = {0};

// ---------------------------------------------------------------------------
// Session synth state (all uint16 params use 0-10000 scale)
// ---------------------------------------------------------------------------
static uint8_t  s_wave_id            = AMY_ENGINE_WAVE_PULSE;
static uint8_t  s_filter_type        = AMY_ENGINE_FILTER_LPF;
// reverb and echo removed — delay lines need large contiguous heap blocks that are
// unavailable after WiFi init fragments SRAM on ESP32-S3.
static uint16_t s_filter_cutoff      = 10000; // fully open by default
static uint16_t s_filter_resonance   = 0;
static uint16_t s_env_attack         = 50;    // ~6 ms
static uint16_t s_env_release        = 200;   // ~149 ms
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
static bool    s_lfo_started[SYNTH_PAD_COUNT]   = {false};
static uint8_t s_pad_midi_note[SYNTH_PAD_COUNT] = {0}; // last midi note played per pad

// AMY filter type constants (FILTER_NONE=0, FILTER_LPF=1, FILTER_BPF=2, FILTER_HPF=3)
static const uint8_t s_filter_type_map[3] = { FILTER_LPF, FILTER_BPF, FILTER_HPF };

// Per-pad stereo pan (left→right across 8 pads)
static const float s_pan_pos[SYNTH_PAD_COUNT] = { 0.1f, 0.2f, 0.35f, 0.45f, 0.55f, 0.65f, 0.8f, 0.9f };

// LFO oscillators occupy indices SYNTH_PAD_COUNT .. SYNTH_PAD_COUNT*2-1
#define LFO_OSC(pad) ((uint16_t)(SYNTH_PAD_COUNT + (pad)))

// ---------------------------------------------------------------------------
// Render state — protected by s_render_lock against the USB audio callback
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_render_lock    = NULL;
static const int16_t    *s_block          = NULL;
static size_t            s_block_frame_pos = 0;
static bool              s_initialized    = false;

// ---------------------------------------------------------------------------
// Scaling helpers (0-10000 → physical units for AMY)
// ---------------------------------------------------------------------------
static float    sc_filter_hz(uint16_t v)     { return 200.0f + (v / 10000.0f) * 9800.0f; }
static float    sc_filter_res(uint16_t v)    { return (v / 10000.0f) * 0.9f; }
static uint32_t sc_attack_ms(uint16_t v)     { return (uint32_t)(5.0f  + (v / 10000.0f) * 1995.0f); }
static uint32_t sc_release_ms(uint16_t v)    { return (uint32_t)(50.0f + (v / 10000.0f) * 4950.0f); }
static float    sc_lfo_hz(uint16_t v)        { return 0.1f + (v / 10000.0f) * 9.9f; }
static float    sc_lfo_depth_hz(uint16_t v)  { return (v / 10000.0f) * 5000.0f; }
static float    sc_fenv_depth_hz(uint16_t v) { return (v / 10000.0f) * 8000.0f; }
static uint32_t sc_fenv_decay_ms(uint16_t v) { return (uint32_t)(5.0f  + (v / 10000.0f) * 1995.0f); }
// KS feedback: high value = long string decay. Map env_release to 0.85-0.995.
static float    sc_ks_feedback(uint16_t v)   { return 0.85f + (v / 10000.0f) * 0.145f; }
static float    sc_chorus_level(uint16_t v)  { return v / 10000.0f; }
static float    sc_pressure_depth_hz(uint16_t v) { return (v / 10000.0f) * 8000.0f; }
static uint16_t sc_portamento_ms(uint16_t v)     { return (uint16_t)((v / 10000.0f) * 500.0f); }

static float midi_note_to_hz(uint8_t note)
{
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
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

// Apply filter params to all currently-playing oscillators (live param change).
static void update_active_osc_filter(void)
{
    if (s_mode != SYNTH_MODE_CUSTOM) return; // don't touch patch-managed oscs
    amy_execute_deltas();
    uint8_t amy_ftype = s_filter_type_map[s_filter_type];
    for (uint8_t i = 0; i < SYNTH_PAD_COUNT; i++) {
        if (!s_pad_active[i]) continue;
        amy_event e = amy_default_event();
        e.osc = i;
        e.filter_type = amy_ftype;
        e.filter_freq_coefs[COEF_CONST] = sc_filter_hz(s_filter_cutoff);
        e.filter_freq_coefs[COEF_EG1]   = sc_fenv_depth_hz(s_filter_env_depth);
        e.filter_freq_coefs[COEF_MOD]   = sc_lfo_depth_hz(s_lfo_depth);
        e.resonance = sc_filter_res(s_filter_resonance);
        amy_add_event(&e);
    }
}

// Update LFO frequency on all started LFO oscillators.
static void update_lfo_rate(void)
{
    if (s_mode != SYNTH_MODE_CUSTOM) return;
    float hz = sc_lfo_hz(s_lfo_rate);
    amy_execute_deltas();
    for (uint8_t i = 0; i < SYNTH_PAD_COUNT; i++) {
        if (!s_lfo_started[i]) continue;
        amy_event e = amy_default_event();
        e.osc = LFO_OSC(i);
        e.freq_coefs[COEF_CONST] = hz;
        amy_add_event(&e);
    }
}

// ---------------------------------------------------------------------------
// Embedded WAV helpers
// ---------------------------------------------------------------------------
static uint16_t read_u16_le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static bool embedded_wav_init(void)
{
    const uint8_t *start = _binary_sleepwalk_wav_start;
    size_t total = (size_t)(_binary_sleepwalk_wav_end - start);
    if (total < 44 || memcmp(start, "RIFF", 4) || memcmp(start+8, "WAVE", 4)) return false;

    embedded_wav_t w = {.data_start = start};
    size_t off = 12;
    bool has_fmt = false, has_data = false;

    while ((off + 8) <= total) {
        const uint8_t *chunk = start + off;
        uint32_t csz  = read_u32_le(chunk + 4);
        size_t   coff = off + 8;
        if (!memcmp(chunk, "fmt ", 4) && csz >= 16) {
            uint16_t fmt  = read_u16_le(start + coff);
            w.channels    = read_u16_le(start + coff + 2);
            w.sample_rate = read_u32_le(start + coff + 4);
            uint16_t bits = read_u16_le(start + coff + 14);
            if (fmt != 1 || w.channels == 0 || w.channels > 2 || bits != 16) return false;
            has_fmt = true;
        } else if (!memcmp(chunk, "data", 4)) {
            w.data_offset = coff;
            w.frame_count = (size_t)csz / (w.channels * 2);
            has_data = true;
            break;
        }
        off = coff + csz + (csz & 1);
    }
    if (!has_fmt || !has_data || !w.frame_count) return false;
    w.ready = true;
    s_wav = w;
    ESP_LOGI(TAG, "WAV ready: %u Hz %u-ch %u frames", (unsigned)w.sample_rate,
             (unsigned)w.channels, (unsigned)w.frame_count);
    return true;
}

static void embedded_wav_render_mono_16(int16_t *out, size_t samples)
{
    if (!s_wav.ready) { memset(out, 0, samples * sizeof(int16_t)); return; }
    const size_t bpf = s_wav.channels * 2;
    const uint8_t *pcm = s_wav.data_start + s_wav.data_offset;
    for (size_t i = 0; i < samples; i++) {
        if (s_wav.frame_cursor >= s_wav.frame_count) s_wav.frame_cursor = 0;
        size_t fi = s_wav.frame_cursor * bpf;
        int32_t v = (int32_t)(int16_t)read_u16_le(pcm + fi);
        if (s_wav.channels == 2)
            v = (v + (int32_t)(int16_t)read_u16_le(pcm + fi + 2)) / 2;
        out[i] = (int16_t)v;
        s_wav.frame_cursor++;
    }
}

// ---------------------------------------------------------------------------
// PATCH mode helpers
// ---------------------------------------------------------------------------

// Reset all oscs and set up one AMY synth with the current patch + 2 voices.
// Must be called with s_render_lock held.
static void apply_patch_synth(void)
{
    amy_execute_deltas();
    amy_event rst = amy_default_event();
    rst.reset_osc = RESET_ALL_OSCS;
    amy_add_event(&rst);

    uint16_t actual_patch = (s_mode == SYNTH_MODE_DX7)
                            ? (uint16_t)(128 + s_patch_num)
                            : (uint16_t)s_patch_num; // JUNO: 0-127 direct

    amy_event e = amy_default_event();
    e.patch_number = actual_patch;
    e.num_voices   = 2; // 2 voices = 12 oscs (Juno) or 16 oscs (DX7), fits max_oscs=16
    e.synth        = 0;
    amy_add_event(&e);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void amy_engine_init(void)
{
    if (s_initialized) return;

    s_render_lock = xSemaphoreCreateMutex();
    if (!s_render_lock) { ESP_LOGE(TAG, "render mutex alloc failed"); return; }

    nvs_load_all();

    amy_config_t cfg            = amy_default_config();
    cfg.audio                   = AMY_AUDIO_IS_NONE;
    cfg.midi                    = AMY_MIDI_IS_NONE;
    cfg.features.startup_bleep  = 0;
    cfg.features.default_synths = 0;
    cfg.features.reverb         = 0; // managed manually — config_reverb guards re-alloc
    cfg.features.echo           = 0; // managed manually — config_echo guards re-alloc
    cfg.features.chorus         = 0; // chorus needs a mod-source osc at index max_oscs+1; heap too tight after WiFi+reverb
    cfg.features.partials       = 0;
    cfg.features.custom         = 0;
    cfg.platform.multicore      = 0;
    cfg.platform.multithread    = 0;
    // Oscs 0-3: audio (one per pad). Oscs 4-7: LFO (one per pad, SINE mod sources).
    // Chorus allocates its own mod source at index max_oscs (index 8), handled internally.
    cfg.max_oscs             = SYNTH_PAD_COUNT * 2; // 16 (8 audio + 8 LFO)
    cfg.ks_oscs              = 0; // KS delay lines (~32 KB) exhaust heap before USB task stack
    cfg.max_sequencer_tags   = 1;
    cfg.max_voices           = 2; // 2 voices for PATCH mode (2×Juno=12 oscs, 2×DX7=16 oscs)
    cfg.max_synths           = 1;
    cfg.max_memory_patches   = 1;
    amy_start(cfg);

    embedded_wav_init();

    s_block_frame_pos = (size_t)AMY_BLOCK_SIZE; // force refill on first render
    s_initialized = true;
    ESP_LOGI(TAG, "init wave=%u ftype=%u flt=%u/%u atk=%u rel=%u fenv=%u/%u lfo=%u/%u chorus=%u",
             s_wave_id, s_filter_type,
             s_filter_cutoff, s_filter_resonance, s_env_attack, s_env_release,
             s_filter_env_depth, s_filter_env_decay, s_lfo_rate, s_lfo_depth,
             s_chorus_amount);
}

void amy_engine_note_on(uint8_t pad, uint8_t midi_note)
{
    if (!s_initialized || pad >= SYNTH_PAD_COUNT) return;

    if (xSemaphoreTake(s_render_lock, portMAX_DELAY) != pdTRUE) return;

    s_pad_midi_note[pad] = midi_note;
    amy_execute_deltas();

    if (s_mode != SYNTH_MODE_CUSTOM) {
        // PATCH mode: let AMY synth voice allocator handle the note
        amy_event e = amy_default_event();
        e.synth     = 0;
        e.midi_note = (float)midi_note;
        e.velocity  = 1.0f;
        amy_add_event(&e);
        s_pad_active[pad] = true;
        xSemaphoreGive(s_render_lock);
        return;
    }

    // Reset audio oscillator cleanly before reconfiguring.
    amy_event rst = amy_default_event();
    rst.osc = pad;
    rst.reset_osc = pad;
    amy_add_event(&rst);

    uint32_t atk = sc_attack_ms(s_env_attack);
    uint32_t rel = sc_release_ms(s_env_release);
    uint8_t  amy_ftype = s_filter_type_map[s_filter_type];

    amy_event e = amy_default_event();
    e.osc  = pad;
    // PULSE and SQUARE both use AMY's PULSE oscillator, differing only in duty cycle:
    //   PULSE  → duty=0.2 (narrow, nasal/buzzy classic synth timbre)
    //   SQUARE → duty=0.5 (symmetric square, fuller/darker)
    // All other wave IDs map directly to AMY constants (SAW_DOWN=2, TRIANGLE=4, KS=6).
    if (s_wave_id == AMY_ENGINE_WAVE_SQUARE) {
        e.wave = PULSE;
        e.duty_coefs[COEF_CONST] = 0.5f;
    } else if (s_wave_id == AMY_ENGINE_WAVE_PULSE) {
        e.wave = PULSE;
        e.duty_coefs[COEF_CONST] = 0.2f;
    } else {
        e.wave = s_wave_id;
    }
    e.freq_coefs[COEF_CONST] = midi_note_to_hz(midi_note);

    // Pan: spread pads across stereo field
    e.pan_coefs[COEF_CONST] = s_pan_pos[pad];

    // Amplitude driven entirely by EG0
    e.amp_coefs[COEF_EG0] = 1.0f;

    // EG0: attack → hold → release on note_off (velocity=0)
    e.eg0_times[0]  = atk; e.eg0_values[0] = 1.0f;
    e.eg0_times[1]  = 0;   e.eg0_values[1] = 1.0f; // hold
    e.eg0_times[2]  = rel; e.eg0_values[2] = 0.0f;

    // KS: use env_release to set string decay (feedback coefficient)
    if (s_wave_id == AMY_ENGINE_WAVE_KS)
        e.feedback = sc_ks_feedback(s_env_release);

    // Filter: base cutoff + filter envelope (EG1) + LFO mod
    e.filter_type = amy_ftype;
    e.filter_freq_coefs[COEF_CONST] = sc_filter_hz(s_filter_cutoff);
    e.filter_freq_coefs[COEF_EG1]   = sc_fenv_depth_hz(s_filter_env_depth);
    e.filter_freq_coefs[COEF_MOD]   = sc_lfo_depth_hz(s_lfo_depth);
    e.resonance = sc_filter_res(s_filter_resonance);

    // EG1: filter envelope — quick rise to full depth, then decay to base
    e.eg1_times[0]  = 5;                              e.eg1_values[0] = 1.0f;
    e.eg1_times[1]  = sc_fenv_decay_ms(s_filter_env_decay); e.eg1_values[1] = 0.0f;
    e.eg1_times[2]  = 0;                              e.eg1_values[2] = 0.0f;

    // LFO mod source: each audio osc tracks its own LFO osc
    e.mod_source = LFO_OSC(pad);

    e.portamento_ms = sc_portamento_ms(s_portamento);
    e.velocity = 1.0f;
    amy_add_event(&e);

    // Start LFO osc for this pad on its first note (runs continuously after that).
    if (!s_lfo_started[pad]) {
        amy_event lfo = amy_default_event();
        lfo.osc = LFO_OSC(pad);
        lfo.wave = SINE;
        lfo.amp_coefs[COEF_CONST]  = 1.0f;
        lfo.freq_coefs[COEF_CONST] = sc_lfo_hz(s_lfo_rate);
        lfo.velocity = 1.0f;
        amy_add_event(&lfo);
        s_lfo_started[pad] = true;
    }

    s_pad_active[pad] = true;
    xSemaphoreGive(s_render_lock);
    ESP_LOGD(TAG, "note_on pad=%u note=%u wave=%u ftype=%u atk=%ums rel=%ums",
             pad, midi_note, s_wave_id, s_filter_type, atk, rel);
}

void amy_engine_note_off(uint8_t pad)
{
    if (!s_initialized || pad >= SYNTH_PAD_COUNT) return;

    if (xSemaphoreTake(s_render_lock, portMAX_DELAY) != pdTRUE) return;

    amy_execute_deltas();
    amy_event e = amy_default_event();

    if (s_mode != SYNTH_MODE_CUSTOM) {
        // PATCH mode: release by MIDI note through the synth voice allocator
        e.synth     = 0;
        e.midi_note = (float)s_pad_midi_note[pad];
        e.velocity  = 0.0f;
    } else {
        e.osc      = pad;
        e.velocity = 0.0f; // triggers EG0 release phase
    }

    amy_add_event(&e);
    s_pad_active[pad] = false;
    xSemaphoreGive(s_render_lock);
    ESP_LOGD(TAG, "note_off pad=%u", pad);
}

void amy_engine_set_wave(uint8_t wave_id)
{
    if (wave_id >= AMY_ENGINE_WAVE_COUNT) return;
    s_wave_id = wave_id;
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
    if (xSemaphoreTake(s_render_lock, portMAX_DELAY) == pdTRUE) {
        update_active_osc_filter();
        xSemaphoreGive(s_render_lock);
    }
    nvs_save_all();
    ESP_LOGI(TAG, "filter cut=%u res=%u", cutoff, resonance);
}

void amy_engine_set_filter_type(uint8_t type)
{
    if (type > AMY_ENGINE_FILTER_HPF) return;
    s_filter_type = type;
    if (xSemaphoreTake(s_render_lock, portMAX_DELAY) == pdTRUE) {
        update_active_osc_filter();
        xSemaphoreGive(s_render_lock);
    }
    nvs_save_all();
    ESP_LOGI(TAG, "filter_type → %u", type);
}

void amy_engine_set_filter_env(uint16_t depth, uint16_t decay)
{
    s_filter_env_depth = depth;
    s_filter_env_decay = decay;
    // EG1 depth applied on next note_on; no live update needed (would retrigger envelope)
    nvs_save_all();
    ESP_LOGI(TAG, "filter_env depth=%u decay=%u", depth, decay);
}

void amy_engine_set_lfo(uint16_t rate, uint16_t depth)
{
    s_lfo_rate  = rate;
    s_lfo_depth = depth;
    if (xSemaphoreTake(s_render_lock, portMAX_DELAY) == pdTRUE) {
        update_lfo_rate();
        update_active_osc_filter(); // refresh COEF_MOD depth on active oscs
        xSemaphoreGive(s_render_lock);
    }
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

    if (xSemaphoreTake(s_render_lock, portMAX_DELAY) != pdTRUE) return;
    for (int i = 0; i < SYNTH_PAD_COUNT; i++) {
        s_pad_active[i]  = false;
        s_lfo_started[i] = false;
    }
    if (s_mode == SYNTH_MODE_CUSTOM) {
        // Reset oscs; CUSTOM note_on will reconfigure fresh on next touch
        amy_execute_deltas();
        amy_event rst = amy_default_event();
        rst.reset_osc = RESET_ALL_OSCS;
        amy_add_event(&rst);
    } else {
        apply_patch_synth();
    }
    xSemaphoreGive(s_render_lock);
    ESP_LOGI(TAG, "mode → %u (patch=%u)", mode, s_patch_num);
}

void amy_engine_set_patch(uint8_t patch_in_bank)
{
    s_patch_num = patch_in_bank > 127 ? 127 : patch_in_bank;
    if (s_mode == SYNTH_MODE_CUSTOM) return; // no-op in CUSTOM

    if (xSemaphoreTake(s_render_lock, portMAX_DELAY) != pdTRUE) return;
    apply_patch_synth();
    xSemaphoreGive(s_render_lock);
    ESP_LOGI(TAG, "patch → bank=%u actual=%u",
             s_patch_num, s_mode == SYNTH_MODE_DX7 ? 128 + s_patch_num : s_patch_num);
}

void amy_engine_update_pressure(uint8_t pad, float pressure_norm)
{
    if (!s_initialized || pad >= SYNTH_PAD_COUNT || !s_pad_active[pad]) return;
    if (s_mode != SYNTH_MODE_CUSTOM) return; // no pressure override in PATCH mode
    if (!s_pressure_depth) return; // fast path: feature off

    // 2 ms timeout — called from touch task at 30 Hz, must not stall telemetry
    if (xSemaphoreTake(s_render_lock, pdMS_TO_TICKS(2)) != pdTRUE) return;
    amy_execute_deltas();
    amy_event e = amy_default_event();
    e.osc = pad;
    e.filter_freq_coefs[COEF_CONST] = sc_filter_hz(s_filter_cutoff)
                                    + sc_pressure_depth_hz(s_pressure_depth) * pressure_norm;
    e.filter_freq_coefs[COEF_EG1]   = sc_fenv_depth_hz(s_filter_env_depth);
    e.filter_freq_coefs[COEF_MOD]   = sc_lfo_depth_hz(s_lfo_depth);
    e.resonance = sc_filter_res(s_filter_resonance);
    amy_add_event(&e);
    xSemaphoreGive(s_render_lock);
}

void amy_engine_set_envelope(uint16_t attack, uint16_t release)
{
    s_env_attack  = attack;
    s_env_release = release;
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

void amy_engine_render_mono_16(int16_t *out, size_t samples)
{
    if (!out || !samples) return;
    if (!s_initialized || !s_render_lock) {
        memset(out, 0, samples * sizeof(int16_t));
        return;
    }
    // 2 ms timeout — output silence rather than stalling the USB callback
    if (xSemaphoreTake(s_render_lock, pdMS_TO_TICKS(2)) != pdTRUE) {
        memset(out, 0, samples * sizeof(int16_t));
        return;
    }
    for (size_t i = 0; i < samples; i++) {
        if (!s_block || s_block_frame_pos >= (size_t)AMY_BLOCK_SIZE) {
            s_block = amy_simple_fill_buffer();
            s_block_frame_pos = 0;
        }
        out[i] = s_block[s_block_frame_pos * AMY_NCHANS];
        s_block_frame_pos++;
    }
    xSemaphoreGive(s_render_lock);
}

void amy_engine_render_wav_mono_16(int16_t *out, size_t samples)
{
    if (!out || !samples) return;
    if (!s_initialized || !s_render_lock) {
        memset(out, 0, samples * sizeof(int16_t));
        return;
    }
    if (xSemaphoreTake(s_render_lock, pdMS_TO_TICKS(2)) != pdTRUE) {
        memset(out, 0, samples * sizeof(int16_t));
        return;
    }
    embedded_wav_render_mono_16(out, samples);
    xSemaphoreGive(s_render_lock);
}
