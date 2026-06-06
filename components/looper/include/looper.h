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

looper_state_t looper_get_state(void);
uint32_t       looper_get_position(void);   // current playback position in samples
uint32_t       looper_get_length(void);     // recorded loop length in samples
