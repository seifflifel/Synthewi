#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Called on touch state edge: pad index, true = finger down, false = finger lifted.
typedef void (*touch_event_cb_t)(uint8_t pad, bool is_touching);

// Called when a synth param command arrives from the bridge.
// param_id matches SYNTH_PARAM_* constants below; value is 0-10000 scaled.
typedef void (*touch_param_cb_t)(uint8_t param_id, uint16_t value);

// Synth param IDs carried in SET_SYNTH_PARAM commands
#define SYNTH_PARAM_WAVE          0
#define SYNTH_PARAM_REVERB_AMOUNT 1
#define SYNTH_PARAM_REVERB_DECAY  2
#define SYNTH_PARAM_ECHO_AMOUNT   3
#define SYNTH_PARAM_ECHO_FEEDBACK 4
#define SYNTH_PARAM_FILTER_CUTOFF 5
#define SYNTH_PARAM_FILTER_RES    6
#define SYNTH_PARAM_ENV_ATTACK    7
#define SYNTH_PARAM_ENV_RELEASE   8
#define SYNTH_PARAM_FILTER_TYPE   9
#define SYNTH_PARAM_FENV_DEPTH    10
#define SYNTH_PARAM_FENV_DECAY    11
#define SYNTH_PARAM_LFO_RATE      12
#define SYNTH_PARAM_LFO_DEPTH     13
#define SYNTH_PARAM_CHORUS        14
#define SYNTH_PARAM_PRESSURE_DEPTH 15  // 0-10000 → 0-8000 Hz added to filter cutoff at max press
#define SYNTH_PARAM_PRESSURE_RANGE 16  // sent as 100-500 (= 1.0×-5.0× threshold ceiling)
#define SYNTH_PARAM_GLIDE          17  // 0-10000 → 0-500 ms portamento time
#define SYNTH_PARAM_MODE           18  // 0=CUSTOM 1=JUNO 2=DX7 (scale=1, raw integer)
#define SYNTH_PARAM_PATCH          19  // 0-127 patch within current bank (scale=1, raw integer)
#define SYNTH_PARAM_OCTAVE         20  // 0=neutral 1=+1 oct 2=-1 oct (momentary, released → 0)

// Called continuously (30 Hz) while a pad is held.
// pressure_norm: 0.0 = just triggered at threshold, 1.0 = full press (ceiling reached).
typedef void (*touch_pressure_cb_t)(uint8_t pad, float pressure_norm);

esp_err_t touch_telemetry_start(void);
void      touch_telemetry_set_event_cb(touch_event_cb_t cb);
void      touch_telemetry_set_param_cb(touch_param_cb_t cb);
void      touch_telemetry_set_pressure_cb(touch_pressure_cb_t cb);
void      touch_telemetry_set_pressure_range(uint16_t range_x100); // 100=1.0× … 500=5.0×
