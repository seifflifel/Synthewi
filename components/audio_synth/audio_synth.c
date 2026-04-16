#include "audio_synth.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "AUDIO";

static uint8_t output_pin = 0;
static synth_voice_t *active_voice = NULL;

void audio_synth_init(uint8_t gpio_pin)
{
    output_pin = gpio_pin;

    gpio_config_t io_config = {
        .pin_bit_mask = (1ULL << output_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_config);

    // Initialize LEDC for PWM
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .gpio_num = output_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);

    ESP_LOGI(TAG, "Audio synthesis initialized on GPIO %d", gpio_pin);
}

void synth_voice_init(synth_voice_t *voice, uint16_t frequency, uint16_t volume, synth_waveform_t waveform)
{
    voice->frequency = frequency;
    voice->volume = volume;
    voice->waveform = waveform;
    voice->is_playing = false;

    ESP_LOGI(TAG, "Voice initialized: freq=%dHz, vol=%d, waveform=%d", frequency, volume, waveform);
}

void synth_voice_play(synth_voice_t *voice)
{
    if (voice->is_playing) return;

    active_voice = voice;
    voice->is_playing = true;

    // Set LEDC frequency to the voice frequency
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, voice->frequency);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, voice->volume);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    ESP_LOGI(TAG, "Playing voice at %dHz", voice->frequency);
}

void synth_voice_stop(synth_voice_t *voice)
{
    if (!voice->is_playing) return;

    voice->is_playing = false;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    if (active_voice == voice) {
        active_voice = NULL;
    }

    ESP_LOGI(TAG, "Stopped voice");
}

void synth_voice_set_frequency(synth_voice_t *voice, uint16_t frequency)
{
    voice->frequency = frequency;
    if (voice->is_playing) {
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, frequency);
    }
}

void synth_voice_set_volume(synth_voice_t *voice, uint16_t volume)
{
    voice->volume = volume;
    if (voice->is_playing) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, volume);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}
