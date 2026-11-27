#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    LOOPER_IDLE,
    LOOPER_RECORDING,
    LOOPER_PLAYING,
    LOOPER_OVERDUB,
} looper_state_t;

// Call once after amy_engine_init() — allocates PSRAM buffer.
void looper_init(void);

// Advance state machine: IDLE→REC→PLAY→OD→PLAY→...
void looper_cycle(void);

// Stop playback and clear buffer — back to IDLE.
void looper_clear(void);

// Audio-thread entry points (called from amy_audio_block_hook, output_limiter.c)
void looper_process_block(int16_t *block, int n_frames);   // record / play / overdub

// Output stage: +6 dB make-up (AMY runs at volume 0.5) and a peak limiter
// that keeps build-ups below ~-8 dBFS without clipping.
// Stats are for diagnostics (read from any task; values are approximate).
typedef struct {
    int16_t  peak_in;        // highest |sample| (incl. make-up) before limiting since last read
    float    min_gain;       // lowest limiter gain since last read (1.0 = no limiting)
} limiter_stats_t;
void output_limiter_process(int16_t *block, int n_frames);
void output_limiter_get_stats(limiter_stats_t *out);       // read + reset
// Master volume 0-100 % (perceptual curve), applied after the limiter. Any task.
void output_set_master_volume(uint8_t percent);

looper_state_t looper_get_state(void);
uint32_t       looper_get_position(void);   // current playback position in samples
uint32_t       looper_get_length(void);     // recorded loop length in samples
