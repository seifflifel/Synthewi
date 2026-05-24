#ifndef AMY_ENGINE_H
#define AMY_ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Wave IDs (match AMY constants: SINE=0 PULSE=1 SAW_DOWN=2 SAW_UP=3 TRIANGLE=4)
#define AMY_ENGINE_WAVE_SINE      0
#define AMY_ENGINE_WAVE_PULSE     1
#define AMY_ENGINE_WAVE_SAW_DOWN  2
#define AMY_ENGINE_WAVE_SAW_UP    3
#define AMY_ENGINE_WAVE_TRIANGLE  4
#define AMY_ENGINE_WAVE_COUNT     5

// Maximum simultaneous pads. Increase to support more touch channels.
#define SYNTH_PAD_COUNT 4

// All 0-10000 scaled values map to a physical range inside amy_engine.
typedef struct {
    uint8_t  wave_id;
    uint16_t reverb_amount;    // 0-10000 → level 0.0-2.0
    uint16_t reverb_decay;     // 0-10000 → liveness 0.5-0.95
    uint16_t echo_amount;      // 0-10000 → level 0.0-1.0
    uint16_t echo_feedback;    // 0-10000 → feedback 0.0-0.9
    uint16_t filter_cutoff;    // 0-10000 → 200-10000 Hz
    uint16_t filter_resonance; // 0-10000 → Q 0.0-0.9
    uint16_t env_attack;       // 0-10000 → 5-2000 ms
    uint16_t env_release;      // 0-10000 → 50-5000 ms
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
void amy_engine_set_envelope(uint16_t attack, uint16_t release);

// Returns current state for telemetry broadcast.
void amy_engine_get_state(amy_engine_state_t *out);

// Audio render — called from USB audio callback (must not block long).
void amy_engine_render_mono_16(int16_t *out, size_t samples);

// WAV transport test — keep for USB audio quality validation.
void amy_engine_render_wav_mono_16(int16_t *out, size_t samples);

#endif // AMY_ENGINE_H
