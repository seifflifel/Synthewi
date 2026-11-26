#include "touch_telemetry.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "touch_control.h"
#include "amy_engine.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "touch_telemetry";

#define TOUCH_TELEM_HZ        100   // was 30 — polling period bounds note-on latency
#define TOUCH_TELEM_PERIOD_MS (1000 / TOUCH_TELEM_HZ)

// Diagnostic: logs every 5 s how often each channel's smoothed value changes
// (≈ hardware scan rate). Measured 2026-11-26 with charge_times=600: ~31 updates/s
// per channel (~32 ms full sweep). Set to 1 to re-measure after changing sampling.
#define TOUCH_DIAG_RATE       0
#define TOUCH_CHANNEL_COUNT   TOUCH_TOTAL_PADS  // 10: 8 note pads + oct- + oct+

// Touch channel IDs: pads 0-7 = notes, pad 8 = oct- (GPIO9, ch9), pad 9 = oct+ (GPIO10, ch10)
static const uint8_t s_touch_channels[TOUCH_CHANNEL_COUNT] = {4, 5, 6, 7, 8, 3, 1, 2, 9, 10};

static touch_sensor_t    s_touch_pads[TOUCH_CHANNEL_COUNT];
static volatile uint16_t s_thresholds[TOUCH_CHANNEL_COUNT] = {100,100,100,100,100,100,100,100,100,100};
static bool              s_prev_touching[TOUCH_CHANNEL_COUNT] = {false};
static uint8_t           s_off_count[TOUCH_CHANNEL_COUNT]    = {0};
// Samples below threshold before firing note_off. Time-based so it stays ~60 ms
// whatever the polling rate (was 2 samples at 30 Hz ≈ 66 ms).
#define TOUCH_OFF_DEBOUNCE_MS  60
#define TOUCH_OFF_DEBOUNCE     ((TOUCH_OFF_DEBOUNCE_MS + TOUCH_TELEM_PERIOD_MS - 1) / TOUCH_TELEM_PERIOD_MS)
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
uint32_t touch_telemetry_get_raw(uint8_t pad)
{
    if (pad >= TOUCH_CHANNEL_COUNT) return 0;
    return s_touch_pads[pad].raw_value;
}

uint32_t touch_telemetry_get_baseline(uint8_t pad)
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
static uint32_t abs_delta(const touch_sensor_t *s)
{
    return (s->raw_value >= s->baseline_value) ? (s->raw_value - s->baseline_value)
                                               : (s->baseline_value - s->raw_value);
}

// ---------------------------------------------------------------------------
// Telemetry task — samples touch at 30 Hz, fires callbacks
// ---------------------------------------------------------------------------
static void touch_telemetry_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t period    = pdMS_TO_TICKS(TOUCH_TELEM_PERIOD_MS);
    if (period == 0) period = 1;

#if TOUCH_DIAG_RATE
    uint32_t diag_prev[TOUCH_CHANNEL_COUNT] = {0};
    uint32_t diag_chg[TOUCH_CHANNEL_COUNT]  = {0};
    uint32_t diag_polls = 0;
    int64_t  diag_t0    = esp_timer_get_time();
#endif

    while (1) {
#if TOUCH_DIAG_RATE
        diag_polls++;
#endif
        for (int i = 0; i < TOUCH_CHANNEL_COUNT; i++) {
            touch_sensor_sample(&s_touch_pads[i]);
#if TOUCH_DIAG_RATE
            if (s_touch_pads[i].raw_value != diag_prev[i]) {
                diag_chg[i]++;
                diag_prev[i] = s_touch_pads[i].raw_value;
            }
#endif
            uint32_t delta    = abs_delta(&s_touch_pads[i]);
            uint32_t thresh   = s_thresholds[i];
            bool     touching = (delta >= thresh);

            if (touching) {
                s_off_count[i] = 0;
                if (!s_prev_touching[i]) {
                    s_prev_touching[i] = true;
                    touch_event_cb_t cb = s_event_cb;
                    if (cb) cb((uint8_t)i, true);
                }
            } else {
                if (s_prev_touching[i]) {
                    s_off_count[i]++;
                    if (s_off_count[i] >= TOUCH_OFF_DEBOUNCE) {
                        s_off_count[i] = 0;
                        s_prev_touching[i] = false;
                        touch_event_cb_t cb = s_event_cb;
                        if (cb) cb((uint8_t)i, false);
                    }
                }
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

#if TOUCH_DIAG_RATE
        int64_t diag_now = esp_timer_get_time();
        if (diag_now - diag_t0 >= 5000000LL) {
            float secs = (diag_now - diag_t0) / 1e6f;
            ESP_LOGI(TAG, "DIAG polls/s=%.0f  updates/s per ch: %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f | %.0f %.0f",
                     diag_polls / secs,
                     diag_chg[0]/secs, diag_chg[1]/secs, diag_chg[2]/secs, diag_chg[3]/secs,
                     diag_chg[4]/secs, diag_chg[5]/secs, diag_chg[6]/secs, diag_chg[7]/secs,
                     diag_chg[8]/secs, diag_chg[9]/secs);
            memset(diag_chg, 0, sizeof(diag_chg));
            diag_polls = 0;
            diag_t0    = diag_now;
        }
#endif
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

    if (xTaskCreatePinnedToCore(touch_telemetry_task, "touch_telem", 6144, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to create telemetry task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "started — %d note pads + 2 oct buttons", TOUCH_NOTE_PADS);
    return ESP_OK;
}
