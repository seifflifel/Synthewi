#ifndef TOUCH_CONTROL_H
#define TOUCH_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/touch_sens_types.h"

typedef struct {
    uint8_t touch_channel;
    uint16_t threshold;          // Base sensitivity hint for adaptive detector
    uint16_t raw_value;          // Latest smooth touch data
    uint16_t baseline_value;     // Latest benchmark value from driver
    uint16_t normalized_value;   // Normalized to 0-127 (MIDI velocity range)
    uint16_t jump_value;         // |current_raw - previous_raw|
    uint16_t adaptive_delta_on;  // Learned ON threshold for absolute delta
    uint16_t adaptive_jump_on;   // Learned ON threshold for jump detector
    bool is_touching;
    uint8_t state_change_count;  // Debounce counter for state changes
    uint16_t prev_raw_value;
    uint32_t idle_abs_ema;
    uint32_t idle_jump_ema;
    touch_channel_handle_t chan_handle;
} touch_sensor_t;

/**
 * Initialize a touch sensor on a specific ADC channel
 * @param sensor Pointer to touch sensor structure
 * @param adc_channel Touch channel ID to use (for ESP32-S3 GPIO6/7 use channels 6/7)
 * @param threshold Touch activation delta from benchmark (typical 80-200)
 */
void touch_sensor_init(touch_sensor_t *sensor, uint8_t adc_channel, uint16_t threshold);

/**
 * Read raw touch sensor value
 * @param sensor Pointer to touch sensor structure
 * @return Smooth touch data from hardware filter
 */
uint16_t touch_sensor_read(touch_sensor_t *sensor);

/**
 * Sample live touch data without changing touch on/off state.
 * Updates raw_value and baseline_value so callers can observe data directly.
 * @param sensor Pointer to touch sensor structure
 */
void touch_sensor_sample(touch_sensor_t *sensor);

/**
 * Update touch state - reads analog value and detects if touching
 * Normalizes raw ADC value to 0-127 range (useful for MIDI velocity)
 * @param sensor Pointer to touch sensor structure
 * @return true if sensor is being touched
 */
bool touch_sensor_update(touch_sensor_t *sensor);

/**
 * Get the intensity/strength of touch as a percentage
 * @param sensor Pointer to touch sensor structure
 * @return 0-100 representing touch strength (100 = full contact)
 */
uint8_t touch_sensor_get_intensity(touch_sensor_t *sensor);

#endif // TOUCH_CONTROL_H
