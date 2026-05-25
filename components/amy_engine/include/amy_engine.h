#ifndef AMY_ENGINE_H
#define AMY_ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Wave IDs (match AMY constants directly: SINE=0 … KS=6)
#define AMY_ENGINE_WAVE_SINE      0
#define AMY_ENGINE_WAVE_PULSE     1
#define AMY_ENGINE_WAVE_SAW_DOWN  2
#define AMY_ENGINE_WAVE_SAW_UP    3
#define AMY_ENGINE_WAVE_TRIANGLE  4
#define AMY_ENGINE_WAVE_SQUARE    5   // PULSE at 50% duty — true square wave
#define AMY_ENGINE_WAVE_KS        6   // Karplus-Strong (requires ks_oscs>0 at startup)
#define AMY_ENGINE_WAVE_COUNT     7

// Filter types (our enum, 0-based; mapped to AMY FILTER_LPF/BPF/HPF internally)
#define AMY_ENGINE_FILTER_LPF     0
#define AMY_ENGINE_FILTER_BPF     1
#define AMY_ENGINE_FILTER_HPF     2

// Maximum simultaneous pads.
#define SYNTH_PAD_COUNT 4

// All 0-10000 scaled values map to a physical range inside amy_engine.
typedef struct {
    uint8_t  wave_id;
    uint8_t  filter_type;       // 0=LPF 1=BPF 2=HPF
    uint16_t reverb_amount;     // 0-10000 → level 0.0-2.0
    uint16_t reverb_decay;      // 0-10000 → liveness 0.5-0.95
    uint16_t echo_amount;       // 0-10000 → level 0.0-1.0
    uint16_t echo_feedback;     // 0-10000 → feedback 0.0-0.9
    uint16_t filter_cutoff;     // 0-10000 → 200-10000 Hz
    uint16_t filter_resonance;  // 0-10000 → Q 0.0-0.9
    uint16_t env_attack;        // 0-10000 → 5-2000 ms
    uint16_t env_release;       // 0-10000 → 50-5000 ms
    uint16_t filter_env_depth;  // 0-10000 → 0-8000 Hz above cutoff
    uint16_t filter_env_decay;  // 0-10000 → 5-2000 ms
    uint16_t lfo_rate;          // 0-10000 → 0.1-10 Hz
    uint16_t lfo_depth;         // 0-10000 → 0-5000 Hz filter swing
    uint16_t chorus_amount;     // 0-10000 → level 0.0-1.0
} amy_engine_state_t;

void amy_engine_init(void);

// Touch → note. pad must be < SYNTH_PAD_COUNT.
void amy_engine_note_on(uint8_t pad, uint8_t midi_note);
void amy_engine_note_off(uint8_t pad);

// Session-level params (all values 0-10000). Applied immediately and saved to NVS.
void amy_engine_set_wave(uint8_t wave_id);
void amy_engine_set_reverb(uint16_t amount, uint16_t decay);
void amy_engine_set_echo(uint16_t amount, uint16_t feedback);
void amy_engine_set_filter(uint16_t cutoff, uint16_t resonance);
void amy_engine_set_filter_type(uint8_t type);                        // 0=LPF 1=BPF 2=HPF
void amy_engine_set_filter_env(uint16_t depth, uint16_t decay);       // EG1 → filter
void amy_engine_set_envelope(uint16_t attack, uint16_t release);
void amy_engine_set_lfo(uint16_t rate, uint16_t depth);               // sine LFO → filter
void amy_engine_set_chorus(uint16_t amount);

// Returns current state for telemetry broadcast.
void amy_engine_get_state(amy_engine_state_t *out);

// Audio render — called from USB audio callback (must not block long).
void amy_engine_render_mono_16(int16_t *out, size_t samples);

// WAV transport test — keep for USB audio quality validation.
void amy_engine_render_wav_mono_16(int16_t *out, size_t samples);

#endif // AMY_ENGINE_H
