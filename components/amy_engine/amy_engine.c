#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "amy.h"
#include "amy_engine.h"

static const char *TAG = "AMY_ENGINE";

extern const uint8_t _binary_sleepwalk_wav_start[];
extern const uint8_t _binary_sleepwalk_wav_end[];

#define AMY_TEST_VOICES 4

typedef struct {
    const uint8_t *data_start;
    size_t data_size;
    size_t data_offset;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t sample_rate;
    size_t frame_count;
    size_t frame_cursor;
    bool ready;
} embedded_wav_t;

static SemaphoreHandle_t s_state_lock = NULL;
static bool s_initialized = false;
static bool s_voice_active[AMY_TEST_VOICES] = {false};
static uint8_t s_next_voice = 0;
static const uint8_t s_voice_notes[AMY_TEST_VOICES] = {60, 64, 67, 72};

static const int16_t *s_block = NULL;
static size_t s_block_frame_pos = AMY_BLOCK_SIZE;
static embedded_wav_t s_wav = {0};

static uint16_t read_u16_le(const uint8_t *ptr)
{
    return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *ptr)
{
    return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
}

static int16_t read_s16_le(const uint8_t *ptr)
{
    return (int16_t)read_u16_le(ptr);
}

static bool embedded_wav_init(void)
{
    const uint8_t *start = _binary_sleepwalk_wav_start;
    const uint8_t *end = _binary_sleepwalk_wav_end;
    size_t total_size = (size_t)(end - start);

    if (total_size < 44) {
        ESP_LOGE(TAG, "Embedded WAV too small: %u bytes", (unsigned)total_size);
        return false;
    }

    if (memcmp(start, "RIFF", 4) != 0 || memcmp(start + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Embedded WAV has invalid RIFF/WAVE header");
        return false;
    }

    size_t offset = 12;
    bool fmt_found = false;
    bool data_found = false;
    embedded_wav_t wav = {
        .data_start = start,
        .data_size = total_size,
        .data_offset = 0,
        .channels = 0,
        .bits_per_sample = 0,
        .sample_rate = 0,
        .frame_count = 0,
        .frame_cursor = 0,
        .ready = false,
    };

    while ((offset + 8U) <= total_size) {
        const uint8_t *chunk = start + offset;
        uint32_t chunk_size = read_u32_le(chunk + 4);
        size_t chunk_data_offset = offset + 8U;
        size_t chunk_end = chunk_data_offset + (size_t)chunk_size;

        if (chunk_end > total_size) {
            ESP_LOGE(TAG, "Embedded WAV chunk exceeds file size");
            return false;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16U) {
                ESP_LOGE(TAG, "Embedded WAV fmt chunk too small");
                return false;
            }

            uint16_t audio_format = read_u16_le(start + chunk_data_offset + 0);
            wav.channels = read_u16_le(start + chunk_data_offset + 2);
            wav.sample_rate = read_u32_le(start + chunk_data_offset + 4);
            wav.bits_per_sample = read_u16_le(start + chunk_data_offset + 14);

            if (audio_format != 1U || wav.channels == 0U || wav.channels > 2U || wav.bits_per_sample != 16U) {
                ESP_LOGE(TAG, "Unsupported WAV format: format=%u channels=%u bits=%u", audio_format, wav.channels, wav.bits_per_sample);
                return false;
            }

            fmt_found = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            wav.data_offset = chunk_data_offset;
            wav.frame_count = (size_t)chunk_size / ((size_t)wav.channels * sizeof(int16_t));
            data_found = true;
            break;
        }

        offset = chunk_end + (chunk_size & 1U);
    }

    if (!fmt_found || !data_found || wav.frame_count == 0U) {
        ESP_LOGE(TAG, "Embedded WAV missing fmt or data chunk");
        return false;
    }

    wav.ready = true;
    s_wav = wav;

    ESP_LOGI(TAG, "Embedded WAV ready: %u Hz, %u channels, %u-bit, %u frames", (unsigned)s_wav.sample_rate,
             (unsigned)s_wav.channels, (unsigned)s_wav.bits_per_sample, (unsigned)s_wav.frame_count);
    return true;
}

static void embedded_wav_render_mono_16(int16_t *out, size_t samples)
{
    if ((out == NULL) || (samples == 0U)) {
        return;
    }

    if (!s_wav.ready || s_wav.frame_count == 0U) {
        memset(out, 0, samples * sizeof(int16_t));
        return;
    }

    const size_t bytes_per_frame = (size_t)s_wav.channels * sizeof(int16_t);
    const uint8_t *pcm = s_wav.data_start + s_wav.data_offset;

    for (size_t i = 0; i < samples; i++) {
        if (s_wav.frame_cursor >= s_wav.frame_count) {
            s_wav.frame_cursor = 0;
        }

        size_t frame_index = s_wav.frame_cursor * bytes_per_frame;
        int32_t mixed = 0;

        if (s_wav.channels == 1U) {
            mixed = (int32_t)read_s16_le(pcm + frame_index);
        } else {
            int32_t left = (int32_t)read_s16_le(pcm + frame_index);
            int32_t right = (int32_t)read_s16_le(pcm + frame_index + sizeof(int16_t));
            mixed = (left + right) / 2;
        }

        out[i] = (int16_t)mixed;
        s_wav.frame_cursor++;
    }
}

static float midi_note_to_hz(uint8_t midi_note)
{
    return 440.0f * powf(2.0f, ((float)midi_note - 69.0f) / 12.0f);
}

static void configure_voice(uint8_t voice)
{
    float freq_hz = midi_note_to_hz(s_voice_notes[voice]);

    amy_event e = amy_default_event();
    e.osc = voice;
    e.wave = SINE;
    e.freq_coefs[COEF_CONST] = freq_hz;
    e.freq_coefs[COEF_NOTE] = 0.0f;
    e.freq_coefs[COEF_BEND] = 0.0f;
    e.amp_coefs[COEF_CONST] = 0.60f;
    e.amp_coefs[COEF_VEL] = 0.0f;
    e.amp_coefs[COEF_EG0] = 0.0f;
    e.amp_coefs[COEF_EG1] = 0.0f;
    amy_add_event(&e);
}

static void note_on(uint8_t voice, uint8_t midi_note)
{
    float freq_hz = midi_note_to_hz(midi_note);

    amy_event reset = amy_default_event();
    reset.osc = voice;
    reset.reset_osc = voice;
    amy_add_event(&reset);

    amy_event e = amy_default_event();
    e.osc = voice;
    e.wave = SINE;
    e.freq_coefs[COEF_CONST] = freq_hz;
    e.freq_coefs[COEF_NOTE] = 0.0f;
    e.freq_coefs[COEF_BEND] = 0.0f;
    e.amp_coefs[COEF_CONST] = 0.60f;
    e.amp_coefs[COEF_VEL] = 0.0f;
    e.amp_coefs[COEF_EG0] = 0.0f;
    e.amp_coefs[COEF_EG1] = 0.0f;
    e.velocity = 1.0f;
    amy_add_event(&e);
}

static void note_off(uint8_t voice)
{
    amy_event e = amy_default_event();
    e.osc = voice;
    e.amp_coefs[COEF_CONST] = 0.0f;
    e.velocity = 0.0f;
    amy_add_event(&e);
}

void amy_engine_init(void)
{
    if (s_initialized) {
        return;
    }

    s_state_lock = xSemaphoreCreateMutex();
    if (s_state_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create AMY state mutex");
        return;
    }

    amy_config_t cfg = amy_default_config();
    cfg.audio = AMY_AUDIO_IS_NONE;
    cfg.midi = AMY_MIDI_IS_NONE;
    cfg.features.startup_bleep = 0;
    cfg.features.default_synths = 0;  // Skip default patches to avoid allocation crash
    cfg.features.reverb = 0;
    cfg.features.echo = 0;
    cfg.features.chorus = 0;
    // Disable multicore/multithread to avoid FreeRTOS task priority issues
    cfg.platform.multicore = 0;
    cfg.platform.multithread = 0;

    amy_start(cfg);
    embedded_wav_init();

    s_initialized = true;
    ESP_LOGI(TAG, "AMY initialized (%d Hz, block=%d)", AMY_SAMPLE_RATE, AMY_BLOCK_SIZE);
}

uint8_t amy_engine_toggle_next_voice(void)
{
    if ((!s_initialized) || (s_state_lock == NULL)) {
        ESP_LOGW(TAG, "toggle_voice called but not initialized");
        return 0;
    }

    uint8_t voice = s_next_voice;
    s_next_voice = (uint8_t)((s_next_voice + 1) % AMY_TEST_VOICES);

    xSemaphoreTake(s_state_lock, portMAX_DELAY);

    // Lazily initialize voices on first use (only when accessing voice 0 for first time)
    static bool initialized_once = false;
    if (!initialized_once) {
        ESP_LOGI(TAG, "Initializing AMY voices on first button press");
        initialized_once = true;
        for (uint32_t v = 0; v < AMY_TEST_VOICES; v++) {
            configure_voice((uint8_t)v);
        }
    }

    if (s_voice_active[voice]) {
        // Stop voice
        note_off(voice);
        s_voice_active[voice] = false;
        ESP_LOGI(TAG, "Voice %u stopped", voice);
    } else {
        // Play voice with configured note
        note_on(voice, s_voice_notes[voice]);
        s_voice_active[voice] = true;
        ESP_LOGI(TAG, "Voice %u playing note %u", voice, s_voice_notes[voice]);
    }

    xSemaphoreGive(s_state_lock);
    return voice;
}

void amy_engine_all_notes_off(void)
{
    if ((!s_initialized) || (s_state_lock == NULL)) {
        return;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);

    for (uint32_t voice = 0; voice < AMY_TEST_VOICES; voice++) {
        note_off((uint8_t)voice);
        s_voice_active[voice] = false;
    }

    xSemaphoreGive(s_state_lock);
}

void amy_engine_render_mono_16(int16_t *out, size_t samples)
{
    if ((out == NULL) || (samples == 0)) {
        return;
    }

    if ((!s_initialized) || (s_state_lock == NULL)) {
        memset(out, 0, samples * sizeof(int16_t));
        return;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);

    for (size_t i = 0; i < samples; i++) {
        if ((s_block == NULL) || (s_block_frame_pos >= AMY_BLOCK_SIZE)) {
            s_block = amy_simple_fill_buffer();
            s_block_frame_pos = 0;
        }

        out[i] = s_block[s_block_frame_pos * AMY_NCHANS];
        s_block_frame_pos++;
    }

    xSemaphoreGive(s_state_lock);
}

void amy_engine_render_wav_mono_16(int16_t *out, size_t samples)
{
    if ((out == NULL) || (samples == 0U)) {
        return;
    }

    if ((!s_initialized) || (s_state_lock == NULL)) {
        memset(out, 0, samples * sizeof(int16_t));
        return;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    embedded_wav_render_mono_16(out, samples);
    xSemaphoreGive(s_state_lock);
}
