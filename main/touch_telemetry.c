#include "touch_telemetry.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "touch_control.h"
#include "amy_engine.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "touch_telemetry";

#define TOUCH_TELEM_HZ        30
#define TOUCH_TELEM_PERIOD_MS (1000 / TOUCH_TELEM_HZ)
#define TOUCH_CHANNEL_COUNT   8

static const uint8_t s_touch_channels[TOUCH_CHANNEL_COUNT] = {4, 5, 6, 7, 8, 12, 1, 2};

static touch_sensor_t    s_touch_pads[TOUCH_CHANNEL_COUNT];
static volatile uint16_t s_thresholds[TOUCH_CHANNEL_COUNT] = {100,100,100,100,100,100,100,100};
static bool              s_prev_touching[TOUCH_CHANNEL_COUNT] = {false};
static bool              s_started = false;

static volatile touch_event_cb_t    s_event_cb    = NULL;
static volatile touch_param_cb_t    s_param_cb    = NULL;
static volatile touch_pressure_cb_t s_pressure_cb = NULL;
static volatile uint16_t            s_pressure_range = 200;

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
static void nvs_save_thresholds(void)
{
    nvs_handle_t h;
    if (nvs_open("touch", NVS_READWRITE, &h) != ESP_OK) return;
    char key[6];
    for (int i = 0; i < TOUCH_CHANNEL_COUNT; i++) {
        snprintf(key, sizeof(key), "thr%d", i);
        nvs_set_u16(h, key, s_thresholds[i]);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_thresholds(void)
{
    nvs_handle_t h;
    if (nvs_open("touch", NVS_READONLY, &h) != ESP_OK) return;
    char key[6];
    uint16_t val;
    for (int i = 0; i < TOUCH_CHANNEL_COUNT; i++) {
        snprintf(key, sizeof(key), "thr%d", i);
        if (nvs_get_u16(h, key, &val) == ESP_OK && val > 0)
            s_thresholds[i] = val;
    }
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// Public callback registration
// ---------------------------------------------------------------------------
void touch_telemetry_set_event_cb(touch_event_cb_t cb)       { s_event_cb    = cb; }
void touch_telemetry_set_param_cb(touch_param_cb_t cb)       { s_param_cb    = cb; }
void touch_telemetry_set_pressure_cb(touch_pressure_cb_t cb) { s_pressure_cb = cb; }
void touch_telemetry_set_pressure_range(uint16_t r)          { s_pressure_range = r ? r : 100; }

// ---------------------------------------------------------------------------
// Public data accessors
// ---------------------------------------------------------------------------
uint16_t touch_telemetry_get_raw(uint8_t pad)
{
    if (pad >= TOUCH_CHANNEL_COUNT) return 0;
    return s_touch_pads[pad].raw_value;
}

uint16_t touch_telemetry_get_baseline(uint8_t pad)
{
    if (pad >= TOUCH_CHANNEL_COUNT) return 0;
    return s_touch_pads[pad].baseline_value;
}

uint16_t touch_telemetry_get_threshold(uint8_t pad)
{
    if (pad >= TOUCH_CHANNEL_COUNT) return 0;
    return s_thresholds[pad];
}

void touch_telemetry_set_threshold(uint8_t pad, uint16_t threshold)
{
    if (pad >= TOUCH_CHANNEL_COUNT || threshold == 0) return;
    s_thresholds[pad] = threshold;
    s_touch_pads[pad].threshold = threshold;
    nvs_save_thresholds();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint16_t abs_delta(const touch_sensor_t *s)
{
    int32_t d = (int32_t)s->raw_value - (int32_t)s->baseline_value;
    return (uint16_t)(d < 0 ? -d : d);
}

// ---------------------------------------------------------------------------
// Telemetry task — samples touch at 30 Hz, fires callbacks
// ---------------------------------------------------------------------------
static void touch_telemetry_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t period    = pdMS_TO_TICKS(TOUCH_TELEM_PERIOD_MS);

    while (1) {
        for (int i = 0; i < TOUCH_CHANNEL_COUNT; i++) {
            touch_sensor_sample(&s_touch_pads[i]);
            uint16_t delta    = abs_delta(&s_touch_pads[i]);
            uint16_t thresh   = s_thresholds[i];
            bool     touching = (delta >= thresh);

            if (touching != s_prev_touching[i]) {
                s_prev_touching[i] = touching;
                touch_event_cb_t cb = s_event_cb;
                if (cb) cb((uint8_t)i, touching);
            }

            if (touching) {
                touch_pressure_cb_t pcb = s_pressure_cb;
                if (pcb) {
                    float ceiling = (float)thresh * (s_pressure_range / 100.0f);
                    float norm    = (ceiling > 0.0f)
                                    ? ((float)delta - (float)thresh) / ceiling
                                    : 0.0f;
                    if (norm < 0.0f) norm = 0.0f;
                    if (norm > 1.0f) norm = 1.0f;
                    pcb((uint8_t)i, norm);
                }
            }
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------
esp_err_t touch_telemetry_start(void)
{
    if (s_started) return ESP_OK;

    nvs_load_thresholds();

    for (int i = 0; i < TOUCH_CHANNEL_COUNT; i++) {
        touch_sensor_init(&s_touch_pads[i], s_touch_channels[i], s_thresholds[i]);
        if (!s_touch_pads[i].chan_handle) {
            ESP_LOGE(TAG, "touch init failed ch%u", s_touch_channels[i]);
            return ESP_FAIL;
        }
    }

    if (xTaskCreate(touch_telemetry_task, "touch_telem", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create telemetry task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "started — %d pads", TOUCH_CHANNEL_COUNT);
    return ESP_OK;
}
