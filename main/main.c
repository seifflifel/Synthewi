#include <stdint.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "amy_engine.h"

static const char *TAG = "Synthewi";

#define OUTPUT_GAIN_BOOST 1
#define AUDIO_BLOCK_SAMPLES 256

#define I2S_BCLK_GPIO  GPIO_NUM_38
#define I2S_LRCLK_GPIO GPIO_NUM_39
#define I2S_DOUT_GPIO  GPIO_NUM_40

static i2s_chan_handle_t s_i2s_tx = NULL;

static void i2s_audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_LRCLK_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));
    ESP_LOGI(TAG, "I2S ready — BCLK=%d LRCLK=%d DOUT=%d", I2S_BCLK_GPIO, I2S_LRCLK_GPIO, I2S_DOUT_GPIO);
}

static void audio_task(void *arg)
{
    (void)arg;
    static int16_t mono[AUDIO_BLOCK_SAMPLES];
    static int32_t out32[AUDIO_BLOCK_SAMPLES * 2];
    int32_t peak = 0;
    uint32_t block_count = 0;
    TickType_t last_stats_tick = xTaskGetTickCount();

    for (;;) {
        amy_engine_render_wav_mono_16(mono, AUDIO_BLOCK_SAMPLES);

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            int32_t s = (int32_t)mono[i] * OUTPUT_GAIN_BOOST;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            int32_t a = s < 0 ? -s : s;
            if (a > peak) peak = a;
            int32_t s32 = s << 16;
            out32[i * 2]     = s32; // L
            out32[i * 2 + 1] = s32; // R
        }

        size_t written = 0;
        i2s_channel_write(s_i2s_tx, out32, sizeof(out32), &written, portMAX_DELAY);

        block_count++;
        TickType_t now = xTaskGetTickCount();
        if ((now - last_stats_tick) >= pdMS_TO_TICKS(3000)) {
            ESP_LOGI(TAG, "blocks=%" PRIu32 " peak=%ld written=%u", block_count, (long)peak, (unsigned)written);
            peak = 0;
            last_stats_tick = now;
        }
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("AMY_ENGINE", ESP_LOG_INFO);

    ESP_LOGI(TAG, "WAV I2S test — build %s %s", __DATE__, __TIME__);

    amy_engine_init();
    i2s_audio_init();

    xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 10, NULL, 0);

    ESP_LOGI(TAG, "playing WAV loop through I2S");
    vTaskDelay(portMAX_DELAY);
}
