#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef void (*touch_event_cb_t)(uint8_t pad, bool is_touching);
typedef void (*touch_param_cb_t)(uint8_t param_id, uint16_t value);
typedef void (*touch_pressure_cb_t)(uint8_t pad, float pressure_norm);

#define SYNTH_PARAM_WAVE           0
#define SYNTH_PARAM_REVERB_AMOUNT  1
#define SYNTH_PARAM_REVERB_DECAY   2
#define SYNTH_PARAM_ECHO_AMOUNT    3
#define SYNTH_PARAM_ECHO_FEEDBACK  4
#define SYNTH_PARAM_FILTER_CUTOFF  5
#define SYNTH_PARAM_FILTER_RES     6
#define SYNTH_PARAM_ENV_ATTACK     7
#define SYNTH_PARAM_ENV_RELEASE    8
#define SYNTH_PARAM_FILTER_TYPE    9
#define SYNTH_PARAM_FENV_DEPTH     10
#define SYNTH_PARAM_FENV_DECAY     11
#define SYNTH_PARAM_LFO_RATE       12
#define SYNTH_PARAM_LFO_DEPTH      13
#define SYNTH_PARAM_CHORUS         14
#define SYNTH_PARAM_PRESSURE_DEPTH 15
#define SYNTH_PARAM_PRESSURE_RANGE 16
#define SYNTH_PARAM_GLIDE          17
#define SYNTH_PARAM_MODE           18
#define SYNTH_PARAM_PATCH          19
#define SYNTH_PARAM_OCTAVE         20

esp_err_t touch_telemetry_start(void);
void      touch_telemetry_set_event_cb(touch_event_cb_t cb);
void      touch_telemetry_set_param_cb(touch_param_cb_t cb);
void      touch_telemetry_set_pressure_cb(touch_pressure_cb_t cb);
void      touch_telemetry_set_pressure_range(uint16_t range_x100);

// Direct data access (no WiFi needed)
uint16_t  touch_telemetry_get_raw(uint8_t pad);
uint16_t  touch_telemetry_get_baseline(uint8_t pad);
uint16_t  touch_telemetry_get_threshold(uint8_t pad);
void      touch_telemetry_set_threshold(uint8_t pad, uint16_t threshold);
