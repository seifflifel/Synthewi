#include "touch_control.h"
#include "driver/touch_sens.h"
#include "driver/touch_version_types.h"
#include "esp_log.h"
#include <stddef.h>

static const char *TAG = "TOUCH";

static touch_sensor_handle_t touch_handle = NULL;
static bool touch_scanning_started = false;

// Signal conditioning constants
static const uint16_t TOUCH_DEFAULT_DELTA = 100;      // If threshold is 0
static const uint16_t TOUCH_RELEASE_HYST = 50;        // Hysteresis to avoid chatter
static const uint16_t TOUCH_INTENSITY_SPAN = 1000;    // Delta range mapped to 0-127
static const uint8_t TOUCH_DEBOUNCE_SAMPLES = 2;      // Consecutive samples to flip state
static const uint16_t TOUCH_DYNAMIC_DELTA_MARGIN = 2000;
static const uint16_t TOUCH_DYNAMIC_JUMP_MARGIN = 1200;
static const uint8_t TOUCH_IDLE_ABS_EMA_SHIFT = 4;    // 1/16 smoothing
static const uint8_t TOUCH_IDLE_JUMP_EMA_SHIFT = 3;   // 1/8 smoothing

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return (a > b) ? a : b;
}

static bool ensure_touch_started(void)
{
    if (touch_handle == NULL) {
        return false;
    }

    if (touch_scanning_started) {
        return true;
    }

    esp_err_t ret = touch_sensor_enable(touch_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch_sensor_enable failed: %s", esp_err_to_name(ret));
        return false;
    }

    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ret = touch_sensor_config_filter(touch_handle, &filter_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch_sensor_config_filter failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = touch_sensor_start_continuous_scanning(touch_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch_sensor_start_continuous_scanning failed: %s", esp_err_to_name(ret));
        return false;
    }

    touch_scanning_started = true;
    return true;
}

void touch_sensor_init(touch_sensor_t *sensor, uint8_t adc_channel, uint16_t threshold)
{
    sensor->touch_channel = adc_channel;
    sensor->threshold = (threshold == 0) ? TOUCH_DEFAULT_DELTA : threshold;
    sensor->is_touching = false;
    sensor->raw_value = 0;
    sensor->baseline_value = 0;
    sensor->normalized_value = 0;
    sensor->jump_value = 0;
    sensor->adaptive_delta_on = 0;
    sensor->adaptive_jump_on = 0;
    sensor->state_change_count = 0;
    sensor->prev_raw_value = 0;
    sensor->idle_abs_ema = 0;
    sensor->idle_jump_ema = 0;
    sensor->chan_handle = NULL;

    // Initialize touch controller once.
    if (touch_handle == NULL) {
        touch_sensor_sample_config_t sample_cfg =
            TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(600, TOUCH_VOLT_LIM_L_0V7, TOUCH_VOLT_LIM_H_2V4);
        touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, &sample_cfg);

        esp_err_t ret = touch_sensor_new_controller(&sens_cfg, &touch_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create touch controller: %s", esp_err_to_name(ret));
            touch_handle = NULL;
            return;
        }
    }

    // Register this touch channel with relative threshold.
    touch_channel_config_t chan_cfg = {
        .active_thresh = {sensor->threshold},
        .charge_speed = TOUCH_CHARGE_SPEED_4,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };

    esp_err_t ret = touch_sensor_new_channel(touch_handle, adc_channel, &chan_cfg, &sensor->chan_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register touch channel %d: %s", adc_channel, esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "Touch channel %d initialized (delta=%u)", adc_channel, sensor->threshold);
}

uint16_t touch_sensor_read(touch_sensor_t *sensor)
{
    if ((sensor == NULL) || (sensor->chan_handle == NULL)) {
        return 0;
    }

    if (!ensure_touch_started()) {
        return 0;
    }

    uint32_t smooth = 0;
    esp_err_t ret = touch_channel_read_data(sensor->chan_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &smooth);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read smooth failed on ch%d: %s", sensor->touch_channel, esp_err_to_name(ret));
        return sensor->raw_value;
    }

    if (smooth > 0xFFFF) {
        smooth = 0xFFFF;
    }

    return (uint16_t)smooth;
}

void touch_sensor_sample(touch_sensor_t *sensor)
{
    if ((sensor == NULL) || (sensor->chan_handle == NULL)) {
        return;
    }

    if (!ensure_touch_started()) {
        return;
    }

    uint16_t current_raw = touch_sensor_read(sensor);
    if (sensor->prev_raw_value == 0) {
        sensor->prev_raw_value = current_raw;
    }
    sensor->jump_value = (current_raw >= sensor->prev_raw_value)
                             ? (uint16_t)(current_raw - sensor->prev_raw_value)
                             : (uint16_t)(sensor->prev_raw_value - current_raw);
    sensor->raw_value = current_raw;
    sensor->prev_raw_value = current_raw;

    uint32_t benchmark = 0;
    esp_err_t ret = touch_channel_read_data(sensor->chan_handle, TOUCH_CHAN_DATA_TYPE_BENCHMARK, &benchmark);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read benchmark failed on ch%d: %s", sensor->touch_channel, esp_err_to_name(ret));
        benchmark = sensor->baseline_value;
    }

    if (benchmark > 0xFFFF) {
        benchmark = 0xFFFF;
    }
    sensor->baseline_value = (uint16_t)benchmark;
}

bool touch_sensor_update(touch_sensor_t *sensor)
{
    if ((sensor == NULL) || (sensor->chan_handle == NULL)) {
        return false;
    }

    touch_sensor_sample(sensor);

    int32_t delta = (int32_t)sensor->raw_value - (int32_t)sensor->baseline_value;
    if (delta < 0) {
        delta = -delta;
    }
    uint32_t abs_delta = (uint32_t)delta;
    uint32_t abs_jump = sensor->jump_value;

    // Learn idle behavior only while not touching.
    if (!sensor->is_touching) {
        int32_t abs_err = (int32_t)abs_delta - (int32_t)sensor->idle_abs_ema;
        sensor->idle_abs_ema = (uint32_t)((int32_t)sensor->idle_abs_ema + (abs_err >> TOUCH_IDLE_ABS_EMA_SHIFT));

        int32_t jump_err = (int32_t)abs_jump - (int32_t)sensor->idle_jump_ema;
        sensor->idle_jump_ema = (uint32_t)((int32_t)sensor->idle_jump_ema + (jump_err >> TOUCH_IDLE_JUMP_EMA_SHIFT));
    }

    uint32_t base_delta_margin = max_u32((uint32_t)sensor->threshold * 200U, TOUCH_DYNAMIC_DELTA_MARGIN);
    uint32_t base_jump_margin = max_u32((uint32_t)sensor->threshold * 40U, TOUCH_DYNAMIC_JUMP_MARGIN);

    uint32_t on_delta_u32 = sensor->idle_abs_ema + base_delta_margin;
    uint32_t on_jump_u32 = (sensor->idle_jump_ema * 4U) + base_jump_margin;

    if (on_delta_u32 > 0xFFFFU) on_delta_u32 = 0xFFFFU;
    if (on_jump_u32 > 0xFFFFU) on_jump_u32 = 0xFFFFU;
    sensor->adaptive_delta_on = (uint16_t)on_delta_u32;
    sensor->adaptive_jump_on = (uint16_t)on_jump_u32;

    uint16_t on_delta = sensor->adaptive_delta_on;
    uint16_t off_delta = (on_delta > TOUCH_RELEASE_HYST) ? (on_delta - TOUCH_RELEASE_HYST) : (on_delta / 2);
    uint16_t on_jump = sensor->adaptive_jump_on;
    uint16_t off_jump = (on_jump > TOUCH_RELEASE_HYST) ? (on_jump - TOUCH_RELEASE_HYST) : (on_jump / 2);

    bool instant_touch = false;
    if (!sensor->is_touching) {
        instant_touch = (abs_delta >= on_delta) || (abs_jump >= on_jump);
    } else {
        instant_touch = (abs_delta >= off_delta) || (abs_jump >= off_jump);
    }

    // Debounce transitions to avoid one-sample flicker.
    if (instant_touch != sensor->is_touching) {
        sensor->state_change_count++;
        if (sensor->state_change_count >= TOUCH_DEBOUNCE_SAMPLES) {
            sensor->is_touching = instant_touch;
            sensor->state_change_count = 0;
        }
    } else {
        sensor->state_change_count = 0;
    }

    if (!sensor->is_touching) {
        sensor->normalized_value = 0;
    } else {
        uint16_t effective = (abs_delta > off_delta) ? (uint16_t)(abs_delta - off_delta) : 0;
        if (effective >= TOUCH_INTENSITY_SPAN) {
            sensor->normalized_value = 127;
        } else {
            sensor->normalized_value = (uint16_t)((effective * 127U) / TOUCH_INTENSITY_SPAN);
        }
    }

    return sensor->is_touching;
}

uint8_t touch_sensor_get_intensity(touch_sensor_t *sensor)
{
    if (!sensor->is_touching) {
        return 0;
    }
    
    // Return intensity as percentage (0-100)
    return (sensor->normalized_value * 100) / 127;
}
