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

// Synthesis mode
typedef enum {
    SYNTH_MODE_CUSTOM = 0,  // Manual oscillator + filter + LFO + pressure
    SYNTH_MODE_JUNO   = 1,  // AMY built-in Juno patches (patch 0-127)
    SYNTH_MODE_DX7    = 2,  // AMY built-in DX7 patches  (patch 128-255)
} synth_mode_t;

// Maximum simultaneous pads.
#define SYNTH_PAD_COUNT 8

// All 0-10000 scaled values map to a physical range inside amy_engine.
typedef struct {
    uint8_t  wave_id;
    uint8_t  filter_type;       // 0=LPF 1=BPF 2=HPF
    uint16_t reverb_amount;     // 0-10000 → level 0.0-2.0
    uint16_t reverb_decay;      // 0-10000 → liveness 0.5-0.95
    uint16_t echo_amount;       // 0-10000 → level 0.0-1.0
    uint16_t echo_feedback;     // 0-10000 → feedback 0.0-0.9
    uint16_t echo_delay;        // 0-10000 → 50-500 ms
    uint16_t filter_cutoff;     // 0-10000 → 60 Hz-18 kHz (exp)
    uint16_t filter_resonance;  // 0-10000 → Q 0.7-11.2 (exp)
    uint16_t env_attack;        // 0-10000 → 1 ms-3 s (exp)
    uint16_t env_decay;         // 0-10000 → 5 ms-4 s (exp)
    uint16_t env_sustain;       // 0-10000 → 0-100 %
    uint16_t env_release;       // 0-10000 → 10 ms-8 s (exp)
    uint16_t filter_env_depth;  // 0-10000 → 0-5 octaves above cutoff at EG1 peak
    uint16_t filter_env_decay;  // 0-10000 → 5 ms-4 s (exp)
    uint16_t lfo_rate;          // 0-10000 → 0.1-20 Hz (exp)
    uint16_t lfo_depth;         // 0-10000 → 0-3 octaves filter swing (±)
    uint16_t chorus_amount;     // 0-10000 → level 0.0-1.0
    uint16_t pressure_depth;   // 0-10000 → 0-8000 Hz added to filter at max press
    uint16_t glide;            // 0-10000 → 0-500 ms portamento time
    uint8_t  synth_mode;       // 0=CUSTOM 1=JUNO 2=DX7
    uint8_t  patch_num;        // 0-127 within current bank
} amy_engine_state_t;

void amy_engine_init(void);

// Touch → note. pad must be < SYNTH_PAD_COUNT.
void amy_engine_note_on(uint8_t pad, uint8_t midi_note);
void amy_engine_note_off(uint8_t pad);

// Session-level params (all values 0-10000). Applied immediately; persisted by amy_engine_save_state().
void amy_engine_set_wave(uint8_t wave_id);
void amy_engine_set_reverb(uint16_t amount, uint16_t decay);
void amy_engine_set_echo(uint16_t amount, uint16_t feedback);
void amy_engine_set_echo_delay(uint16_t delay);               // 0-10000 → 50-500 ms
void amy_engine_set_filter(uint16_t cutoff, uint16_t resonance);
void amy_engine_set_filter_type(uint8_t type);                        // 0=LPF 1=BPF 2=HPF
void amy_engine_set_filter_env(uint16_t depth, uint16_t decay);       // EG1 → filter
void amy_engine_set_envelope(uint16_t attack, uint16_t release); // legacy
void amy_engine_set_adsr(uint16_t attack, uint16_t decay, uint16_t sustain, uint16_t release);
void amy_engine_set_lfo(uint16_t rate, uint16_t depth);               // sine LFO → filter
void amy_engine_set_chorus(uint16_t amount);
void amy_engine_set_pressure_depth(uint16_t depth); // 0-10000 → Hz range added at max press
void amy_engine_set_glide(uint16_t glide);          // 0-10000 → 0-500 ms portamento time
void amy_engine_set_mode(uint8_t mode);             // synth_mode_t: 0=CUSTOM 1=JUNO 2=DX7
void amy_engine_set_patch(uint8_t patch_in_bank);  // 0-127 within current bank

// Called from touch task at 30 Hz while pad is held. Updates per-pad filter cutoff live.
void amy_engine_update_pressure(uint8_t pad, float pressure_norm);

// Returns current state for telemetry broadcast.
void amy_engine_get_state(amy_engine_state_t *out);

// Persist current state to NVS (call on section exit / confirm, NOT on every encoder tick).
void amy_engine_save_state(void);

// Silence all pad voices and flush echo/reverb tails immediately; effect
// settings are restored automatically ~0.7 s later. (Looper: caller's job.)
void amy_engine_panic(void);

// Param (0-10000) → physical value, using the exact curves the engine plays with.
// Use these for display so the UI always matches the sound.
uint32_t amy_engine_attack_ms(uint16_t v);       // 1 ms – 3 s (exp)
uint32_t amy_engine_decay_ms(uint16_t v);        // 5 ms – 4 s (exp)
uint32_t amy_engine_release_ms(uint16_t v);      // 10 ms – 8 s (exp)
float    amy_engine_filter_hz(uint16_t v);       // 60 Hz – 18 kHz (exp)
float    amy_engine_filter_q(uint16_t v);        // Q 0.7 – 11.2 (exp)
float    amy_engine_lfo_hz(uint16_t v);          // 0.1 – 20 Hz (exp)
float    amy_engine_lfo_depth_oct(uint16_t v);   // 0 – 3 octaves
float    amy_engine_fenv_depth_oct(uint16_t v);  // 0 – 5 octaves
uint32_t amy_engine_fenv_decay_ms(uint16_t v);   // 5 ms – 4 s (exp)
uint32_t amy_engine_glide_ms(uint16_t v);        // 0 – 500 ms

// Convert values saved with the old v3 (linear) curves to the current curves,
// preserving their physical meaning. Any pointer may be NULL.
void amy_engine_migrate_v3(uint16_t *atk, uint16_t *dec, uint16_t *rel,
                           uint16_t *cut, uint16_t *fenv_dec, uint16_t *lfo_rt);

#endif // AMY_ENGINE_H
