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
static uint8_t  s_wave_id          = AMY_ENGINE_WAVE_SINE;
static uint16_t s_reverb_amount    = 0;
static uint16_t s_reverb_decay     = 5000;
static uint16_t s_echo_amount      = 0;
static uint16_t s_echo_feedback    = 3000;
static uint16_t s_filter_cutoff    = 10000; // fully open by default
static uint16_t s_filter_resonance = 0;
static uint16_t s_env_attack       = 50;    // ~6 ms
static uint16_t s_env_release      = 200;   // ~149 ms

static bool s_pad_active[SYNTH_PAD_COUNT] = {false};

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
static float    sc_reverb_level(uint16_t v) { return (v / 10000.0f) * 2.0f; }
static float    sc_reverb_live(uint16_t v)  { return 0.5f + (v / 10000.0f) * 0.45f; }
static float    sc_echo_level(uint16_t v)   { return v / 10000.0f; }
static float    sc_echo_fb(uint16_t v)      { return (v / 10000.0f) * 0.9f; }
static float    sc_filter_hz(uint16_t v)    { return 200.0f + (v / 10000.0f) * 9800.0f; }
static float    sc_filter_res(uint16_t v)   { return (v / 10000.0f) * 0.9f; }
static uint32_t sc_attack_ms(uint16_t v)    { return (uint32_t)(5.0f   + (v / 10000.0f) * 1995.0f); }
static uint32_t sc_release_ms(uint16_t v)   { return (uint32_t)(50.0f  + (v / 10000.0f) * 4950.0f); }

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
    nvs_set_u16(h, "rev_amt",  s_reverb_amount);
    nvs_set_u16(h, "rev_dec",  s_reverb_decay);
    nvs_set_u16(h, "echo_amt", s_echo_amount);
    nvs_set_u16(h, "echo_fb",  s_echo_feedback);
    nvs_set_u16(h, "flt_cut",  s_filter_cutoff);
    nvs_set_u16(h, "flt_res",  s_filter_resonance);
    nvs_set_u16(h, "env_atk",  s_env_attack);
    nvs_set_u16(h, "env_rel",  s_env_release);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open("synth", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t  u8;
    uint16_t u16;
    if (nvs_get_u8(h,  "wave",     &u8)  == ESP_OK) s_wave_id          = u8;
    if (nvs_get_u16(h, "rev_amt",  &u16) == ESP_OK) s_reverb_amount    = u16;
    if (nvs_get_u16(h, "rev_dec",  &u16) == ESP_OK) s_reverb_decay     = u16;
    if (nvs_get_u16(h, "echo_amt", &u16) == ESP_OK) s_echo_amount      = u16;
    if (nvs_get_u16(h, "echo_fb",  &u16) == ESP_OK) s_echo_feedback    = u16;
    if (nvs_get_u16(h, "flt_cut",  &u16) == ESP_OK) s_filter_cutoff    = u16;
    if (nvs_get_u16(h, "flt_res",  &u16) == ESP_OK) s_filter_resonance = u16;
    if (nvs_get_u16(h, "env_atk",  &u16) == ESP_OK) s_env_attack       = u16;
    if (nvs_get_u16(h, "env_rel",  &u16) == ESP_OK) s_env_release      = u16;
    nvs_close(h);
}

// Update filter on any currently-playing oscillators (live param change).
static void update_active_osc_filter(void)
{
    amy_execute_deltas(); // drain pool before queuing more filter events
    for (uint8_t i = 0; i < SYNTH_PAD_COUNT; i++) {
        if (!s_pad_active[i]) continue;
        amy_event e = amy_default_event();
        e.osc = i;
        e.filter_type = FILTER_LPF;
        e.filter_freq_coefs[COEF_CONST] = sc_filter_hz(s_filter_cutoff);
        e.resonance = sc_filter_res(s_filter_resonance);
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
// Public API
// ---------------------------------------------------------------------------

void amy_engine_init(void)
{
    if (s_initialized) return;

    s_render_lock = xSemaphoreCreateMutex();
    if (!s_render_lock) { ESP_LOGE(TAG, "render mutex alloc failed"); return; }

    nvs_load_all();

    amy_config_t cfg       = amy_default_config();
    cfg.audio              = AMY_AUDIO_IS_NONE;
    cfg.midi               = AMY_MIDI_IS_NONE;
    cfg.features.startup_bleep  = 0;
    cfg.features.default_synths = 0;
    cfg.features.reverb    = 0; // managed manually — see amy_engine_set_reverb
    cfg.features.echo      = 0; // managed manually — see amy_engine_set_echo
    cfg.features.chorus    = 0;
    cfg.features.partials  = 0; // not used — saves heap
    cfg.features.custom    = 0; // not used — saves heap
    cfg.platform.multicore = 0;
    cfg.platform.multithread = 0;
    // Keep footprint small — we use 4 oscillators (one per pad).
    // Set unused subsystems to 1 (not 0) to avoid malloc(0) edge-cases in AMY internals.
    cfg.max_oscs             = SYNTH_PAD_COUNT + 4; // 8 — 4 pads + 4 headroom for reset events
    cfg.max_sequencer_tags   = 1;
    cfg.max_voices           = 1;
    cfg.max_synths           = 1;
    cfg.max_memory_patches   = 1;
    cfg.ks_oscs              = 0;  // no Karplus-Strong
    amy_start(cfg);

    // Restore non-zero reverb/echo from NVS (first call allocates delay lines once).
    // Skip if zero — config_reverb(level=0) would mark reverb uninitialized,
    // causing double-allocation on the next call (heap corruption).
    if (s_reverb_amount > 0)
        config_reverb(sc_reverb_level(s_reverb_amount), sc_reverb_live(s_reverb_decay), 0.5f, 3000.0f);
    if (s_echo_amount > 0)
        config_echo(sc_echo_level(s_echo_amount), 200.0f, 2000.0f, sc_echo_fb(s_echo_feedback), 0.0f);

    embedded_wav_init();

    s_block_frame_pos = (size_t)AMY_BLOCK_SIZE; // force refill on first render
    s_initialized = true;
    ESP_LOGI(TAG, "init — wave=%u rev=%u/%u echo=%u/%u flt=%u/%u atk=%u rel=%u",
             s_wave_id, s_reverb_amount, s_reverb_decay, s_echo_amount, s_echo_feedback,
             s_filter_cutoff, s_filter_resonance, s_env_attack, s_env_release);
}

void amy_engine_note_on(uint8_t pad, uint8_t midi_note)
{
    if (!s_initialized || pad >= SYNTH_PAD_COUNT) return;

    if (xSemaphoreTake(s_render_lock, portMAX_DELAY) != pdTRUE) return;

    // Drain any pending deltas before adding new ones.
    // When no USB host is connected, amy_simple_fill_buffer() is never called, so
    // deltas accumulate until the pool is exhausted.  amy_execute_deltas() processes
    // and releases all due deltas (all ours have time=0) without rendering audio,
    // keeping the pool available.
    amy_execute_deltas();

    // Reset oscillator cleanly before reconfiguring
    amy_event rst = amy_default_event();
    rst.osc = pad;
    rst.reset_osc = pad;
    amy_add_event(&rst);

    uint32_t atk = sc_attack_ms(s_env_attack);
    uint32_t rel = sc_release_ms(s_env_release);

    amy_event e = amy_default_event();
    e.osc  = pad;
    e.wave = s_wave_id;
    e.freq_coefs[COEF_CONST] = midi_note_to_hz(midi_note);

    // Amplitude driven entirely by EG0
    e.amp_coefs[COEF_EG0] = 1.0f;

    // EG0: attack → hold at sustain → release on note_off (velocity=0)
    e.eg0_times[0]  = atk;  e.eg0_values[0] = 1.0f;
    e.eg0_times[1]  = 0;    e.eg0_values[1] = 1.0f; // hold
    e.eg0_times[2]  = rel;  e.eg0_values[2] = 0.0f;

    e.filter_type = FILTER_LPF;
    e.filter_freq_coefs[COEF_CONST] = sc_filter_hz(s_filter_cutoff);
    e.resonance = sc_filter_res(s_filter_resonance);

    e.velocity = 1.0f;
    amy_add_event(&e);

    s_pad_active[pad] = true;
    xSemaphoreGive(s_render_lock);
    ESP_LOGD(TAG, "note_on pad=%u note=%u wave=%u atk=%ums rel=%ums", pad, midi_note, s_wave_id, atk, rel);
}

void amy_engine_note_off(uint8_t pad)
{
    if (!s_initialized || pad >= SYNTH_PAD_COUNT) return;

    if (xSemaphoreTake(s_render_lock, portMAX_DELAY) != pdTRUE) return;

    amy_execute_deltas(); // keep pool drained

    amy_event e = amy_default_event();
    e.osc      = pad;
    e.velocity = 0.0f; // triggers EG0 release phase
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
    s_reverb_amount = amount;
    s_reverb_decay  = decay;
    // config_reverb(level=0) re-allocates delay lines without freeing the old ones.
    // Keep reverb silent by leaving level at 0 (already the AMY default).
    if (amount > 0)
        config_reverb(sc_reverb_level(amount), sc_reverb_live(decay), 0.5f, 3000.0f);
    nvs_save_all();
    ESP_LOGI(TAG, "reverb amt=%u dec=%u", amount, decay);
}

void amy_engine_set_echo(uint16_t amount, uint16_t feedback)
{
    s_echo_amount   = amount;
    s_echo_feedback = feedback;
    // Same guard as reverb — config_echo(level=0) double-allocates delay lines.
    if (amount > 0)
        config_echo(sc_echo_level(amount), 200.0f, 2000.0f, sc_echo_fb(feedback), 0.0f);
    nvs_save_all();
    ESP_LOGI(TAG, "echo amt=%u fb=%u", amount, feedback);
}

void amy_engine_set_filter(uint16_t cutoff, uint16_t resonance)
{
    s_filter_cutoff    = cutoff;
    s_filter_resonance = resonance;
    // Apply to any already-playing oscillators immediately
    if (xSemaphoreTake(s_render_lock, portMAX_DELAY) == pdTRUE) {
        update_active_osc_filter();
        xSemaphoreGive(s_render_lock);
    }
    nvs_save_all();
    ESP_LOGI(TAG, "filter cut=%u res=%u", cutoff, resonance);
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
    out->reverb_amount    = s_reverb_amount;
    out->reverb_decay     = s_reverb_decay;
    out->echo_amount      = s_echo_amount;
    out->echo_feedback    = s_echo_feedback;
    out->filter_cutoff    = s_filter_cutoff;
    out->filter_resonance = s_filter_resonance;
    out->env_attack       = s_env_attack;
    out->env_release      = s_env_release;
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
