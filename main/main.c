#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "usb_device_uac.h"
#include "amy_engine.h"
#include "wifi_manager.h"
#include "touch_telemetry.h"

static const char *TAG = "Synthewi";

// Set to 1 to stream the embedded WAV instead of live synthesis.
// Useful for validating USB audio transport quality without the synth.
#define TEST_WAV_PLAYBACK 0

#define OUTPUT_GAIN_BOOST 4

static bool     is_muted     = false;
static uint32_t volume_factor = 100;
static volatile uint32_t s_usb_in_cb_count = 0;

// ---------------------------------------------------------------------------
// Touch → synth bridge (called from touch_telemetry task on state edges)
// ---------------------------------------------------------------------------
static void on_touch_event(uint8_t pad, bool is_touching)
{
    if (is_touching) {
        // Note mapping is owned by amy_engine (s_pad_notes[]).
        // We pass pad; amy_engine resolves the MIDI note internally.
        amy_engine_note_on(pad, 60 + pad * 2); // C4, D4, E4, F4 — mirrors s_pad_notes
    } else {
        amy_engine_note_off(pad);
    }
}

// ---------------------------------------------------------------------------
// Synth param commands from bridge (called from touch_cmd task)
// ---------------------------------------------------------------------------
static void on_synth_param(uint8_t param_id, uint16_t value)
{
    switch (param_id) {
    case SYNTH_PARAM_WAVE:
        amy_engine_set_wave((uint8_t)value);
        break;
    case SYNTH_PARAM_REVERB_AMOUNT:
    case SYNTH_PARAM_REVERB_DECAY: {
        // We need both values; fetch current state for the one not changing.
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        uint16_t amt = (param_id == SYNTH_PARAM_REVERB_AMOUNT) ? value : st.reverb_amount;
        uint16_t dec = (param_id == SYNTH_PARAM_REVERB_DECAY)  ? value : st.reverb_decay;
        amy_engine_set_reverb(amt, dec);
        break;
    }
    case SYNTH_PARAM_ECHO_AMOUNT:
    case SYNTH_PARAM_ECHO_FEEDBACK: {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        uint16_t amt = (param_id == SYNTH_PARAM_ECHO_AMOUNT)   ? value : st.echo_amount;
        uint16_t fb  = (param_id == SYNTH_PARAM_ECHO_FEEDBACK) ? value : st.echo_feedback;
        amy_engine_set_echo(amt, fb);
        break;
    }
    case SYNTH_PARAM_FILTER_CUTOFF:
    case SYNTH_PARAM_FILTER_RES: {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        uint16_t cut = (param_id == SYNTH_PARAM_FILTER_CUTOFF) ? value : st.filter_cutoff;
        uint16_t res = (param_id == SYNTH_PARAM_FILTER_RES)    ? value : st.filter_resonance;
        amy_engine_set_filter(cut, res);
        break;
    }
    case SYNTH_PARAM_ENV_ATTACK:
    case SYNTH_PARAM_ENV_RELEASE: {
        amy_engine_state_t st;
        amy_engine_get_state(&st);
        uint16_t atk = (param_id == SYNTH_PARAM_ENV_ATTACK)  ? value : st.env_attack;
        uint16_t rel = (param_id == SYNTH_PARAM_ENV_RELEASE) ? value : st.env_release;
        amy_engine_set_envelope(atk, rel);
        break;
    }
    default:
        ESP_LOGW(TAG, "unknown synth param %u", param_id);
        break;
    }
}

// ---------------------------------------------------------------------------
// USB Audio Class callbacks
// ---------------------------------------------------------------------------
static esp_err_t usb_uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    (void)buf; (void)len; (void)arg;
    return ESP_OK;
}

static esp_err_t usb_uac_device_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *arg)
{
    (void)arg;
    size_t   samples = len / sizeof(int16_t);
    int16_t *out     = (int16_t *)buf;
    static uint32_t   cb_count = 0;
    static TickType_t last_stats_tick = 0;
    int32_t peak = 0;

#if TEST_WAV_PLAYBACK
    amy_engine_render_wav_mono_16(out, samples);
#else
    amy_engine_render_mono_16(out, samples);
#endif

    for (size_t i = 0; i < samples; i++) {
        int32_t s = is_muted ? 0 : out[i];
        s = (s * (int32_t)volume_factor) / 100;
        s = s * OUTPUT_GAIN_BOOST;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        int32_t a = s < 0 ? -s : s;
        if (a > peak) peak = a;
        out[i] = (int16_t)s;
    }

    *bytes_read = samples * sizeof(int16_t);

    cb_count++;
    s_usb_in_cb_count = cb_count;
    TickType_t now = xTaskGetTickCount();
    if ((now - last_stats_tick) >= pdMS_TO_TICKS(1000)) {
        ESP_LOGI(TAG, "USB IN cb=%" PRIu32 " samples=%u peak=%ld mute=%d vol=%" PRIu32,
                 cb_count, (unsigned)samples, (long)peak, (int)is_muted, volume_factor);
        last_stats_tick = now;
    }
    return ESP_OK;
}

static void usb_uac_device_set_mute_cb(uint32_t mute, void *arg)
{
    (void)arg;
    is_muted = (mute != 0);
    ESP_LOGI(TAG, "mute → %u", (unsigned)mute);
}

static void usb_uac_device_set_volume_cb(uint32_t _volume, void *arg)
{
    (void)arg;
    int db = (int)(_volume > 100 ? 100 : _volume) / 2 - 50;
    if (db >= 0) {
        volume_factor = 100;
    } else {
        float lin = powf(10.0f, (float)db / 20.0f);
        uint32_t f = (uint32_t)(lin * 100.0f);
        if (!is_muted && f < 8U) f = 8U;
        volume_factor = f;
    }
    ESP_LOGI(TAG, "volume raw=%u db=%d factor=%" PRIu32, (unsigned)_volume, db, volume_factor);
}

static void usb_uac_device_init(void)
{
    uac_device_config_t cfg = {
        .output_cb    = usb_uac_device_output_cb,
        .input_cb     = usb_uac_device_input_cb,
        .set_mute_cb  = usb_uac_device_set_mute_cb,
        .set_volume_cb = usb_uac_device_set_volume_cb,
        .cb_ctx       = NULL,
    };
    ESP_ERROR_CHECK(uac_device_init(&cfg));
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("AMY_ENGINE", ESP_LOG_INFO);

    ESP_LOGI(TAG, "Synthewi starting — build %s %s", __DATE__, __TIME__);
#if TEST_WAV_PLAYBACK
    ESP_LOGW(TAG, "TEST MODE: streaming embedded WAV (synth disabled)");
#endif

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    err = wifi_manager_start();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "WiFi init failed (0x%x) — continuing without network", err);

    // Register callbacks before starting telemetry so no edge is missed
    touch_telemetry_set_event_cb(on_touch_event);
    touch_telemetry_set_param_cb(on_synth_param);

    err = touch_telemetry_start();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "touch telemetry init failed (0x%x)", err);

    amy_engine_init();
    usb_uac_device_init();

    ESP_LOGI(TAG, "running");

    uint32_t  last_cb_count  = 0;
    TickType_t last_cb_tick  = xTaskGetTickCount();
    TickType_t last_warn_tick = xTaskGetTickCount();

    while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((now - last_cb_tick) >= pdMS_TO_TICKS(1000)) {
            uint32_t cur = s_usb_in_cb_count;
            if (cur == last_cb_count) {
                if ((now - last_warn_tick) >= pdMS_TO_TICKS(5000)) {
                    ESP_LOGW(TAG, "USB mic stream inactive — open Synthewi USB Audio on host");
                    last_warn_tick = now;
                }
            } else {
                last_warn_tick = now;
            }
            last_cb_count = cur;
            last_cb_tick  = now;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
