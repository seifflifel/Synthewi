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

#define OUTPUT_GAIN_BOOST 4
#define TEST_WAV_PLAYBACK 1

static bool is_muted = false;
static uint32_t volume_factor = 100;
static volatile uint32_t s_usb_in_cb_count = 0;
static esp_err_t usb_uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    (void)buf;
    (void)len;
    (void)arg;
    return ESP_OK;
}

static esp_err_t usb_uac_device_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *arg)
{
    (void)arg;
    size_t samples = len / sizeof(int16_t);
    int16_t *out = (int16_t *)buf;
    static uint32_t cb_count = 0;
    static TickType_t last_stats_tick = 0;
    int32_t peak = 0;

#if TEST_WAV_PLAYBACK
    amy_engine_render_wav_mono_16(out, samples);
#else
    amy_engine_render_mono_16(out, samples);
#endif

    for (size_t i = 0; i < samples; i++) {
        int32_t sample = is_muted ? 0 : out[i];
        sample = (sample * (int32_t)volume_factor) / 100;
        sample = sample * OUTPUT_GAIN_BOOST;

        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }

        int32_t abs_sample = (sample < 0) ? -sample : sample;
        if (abs_sample > peak) {
            peak = abs_sample;
        }

        out[i] = (int16_t)sample;
    }

    *bytes_read = samples * sizeof(int16_t);

    cb_count++;
    s_usb_in_cb_count = cb_count;
    TickType_t now = xTaskGetTickCount();
    if ((now - last_stats_tick) >= pdMS_TO_TICKS(1000)) {
        ESP_LOGI(TAG, "USB IN: cb=%" PRIu32 " samples=%u peak=%ld mute=%d vol=%" PRIu32,
                 cb_count, (unsigned)samples, (long)peak, (int)is_muted, volume_factor);
        last_stats_tick = now;
    }

    return ESP_OK;
}

static void usb_uac_device_set_mute_cb(uint32_t mute, void *arg)
{
    (void)arg;
    is_muted = (mute != 0);
    ESP_LOGI(TAG, "USB set mute: %u", (unsigned)mute);
}

static void usb_uac_device_set_volume_cb(uint32_t _volume, void *arg)
{
    (void)arg;
    int volume_ui = (int)_volume;
    if (volume_ui < 0) {
        volume_ui = 0;
    } else if (volume_ui > 100) {
        volume_ui = 100;
    }

    int volume_db = volume_ui / 2 - 50;
    if (volume_db >= 0) {
        volume_factor = 100;
    } else {
        float linear = powf(10.0f, (float)volume_db / 20.0f);
        uint32_t mapped = (uint32_t)(linear * 100.0f);
        // Keep an audible floor unless host explicitly mutes us.
        if ((!is_muted) && (mapped < 8U)) {
            mapped = 8U;
        }
        volume_factor = mapped;
    }

    ESP_LOGI(TAG, "USB set volume: raw=%u db=%d factor=%" PRIu32,
             (unsigned)_volume, volume_db, volume_factor);
}

static void usb_uac_device_init(void)
{
    uac_device_config_t config = {
        .output_cb = usb_uac_device_output_cb,
        .input_cb = usb_uac_device_input_cb,
        .set_mute_cb = usb_uac_device_set_mute_cb,
        .set_volume_cb = usb_uac_device_set_volume_cb,
        .cb_ctx = NULL,
    };

    ESP_ERROR_CHECK(uac_device_init(&config));
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("AMY_ENGINE", ESP_LOG_INFO);

    ESP_LOGI(TAG, "Synthewi - WAV + USB UAC transport test");
    ESP_LOGI(TAG, "Build: %s %s", __DATE__, __TIME__);
#if TEST_WAV_PLAYBACK
    ESP_LOGW(TAG, "TEST MODE: streaming embedded WAV through the USB mic path");
#endif

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    // Initialize Wi-Fi first (while heap is clean) - non-fatal on failure
    err = wifi_manager_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi initialization failed (err=0x%x): %s; continuing with USB audio only", err, esp_err_to_name(err));
    }

    // Initialize touch telemetry (non-fatal on failure)
    err = touch_telemetry_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Touch telemetry initialization failed (err=0x%x): %s; continuing with USB audio only", err, esp_err_to_name(err));
    }

    amy_engine_init();
    usb_uac_device_init();

    // Start Wi-Fi event loop task if needed (optional background monitoring)
    // if (xTaskCreate(wifi_init_task, "wifi_init", 2048, NULL, 3, NULL) != pdPASS) {
    //     ESP_LOGW(TAG, "Failed to create Wi-Fi init task");
    // }

    ESP_LOGI(TAG, "USB audio callback source: embedded WAV loop");


    uint32_t last_usb_cb_count = 0;
    TickType_t last_usb_check_tick = xTaskGetTickCount();
    TickType_t last_usb_warn_tick = xTaskGetTickCount();

    while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((now - last_usb_check_tick) >= pdMS_TO_TICKS(1000)) {
            uint32_t current_cb_count = s_usb_in_cb_count;
            if (current_cb_count == last_usb_cb_count) {
                if ((now - last_usb_warn_tick) >= pdMS_TO_TICKS(5000)) {
                    ESP_LOGW(TAG, "USB mic stream inactive: open/select Synthewi USB Audio input on host");
                    last_usb_warn_tick = now;
                }
            } else {
                last_usb_warn_tick = now;
            }

            last_usb_cb_count = current_cb_count;
            last_usb_check_tick = now;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
