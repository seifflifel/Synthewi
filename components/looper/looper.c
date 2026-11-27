#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "looper.h"

static const char *TAG = "Looper";

// AMY renders 48000 Hz stereo — looper captures mono int16.
#define LOOPER_SAMPLE_RATE  48000
// Bytes left free after the loop buffer. Echo (256 KB) and reverb (~108 KB)
// are allocated by amy_engine_init() BEFORE looper_init(), so this only has to
// cover fragmentation and small later allocations.
#define LOOPER_PSRAM_HEADROOM  (150 * 1024)

static volatile looper_state_t s_state    = LOOPER_IDLE;
static int16_t * volatile      s_buf      = NULL;
static volatile uint32_t       s_max_len  = 0;  // allocated samples (mono)
static volatile uint32_t       s_loop_len = 0;  // recorded loop length in samples
static volatile uint32_t       s_pos      = 0;  // current position in samples

// Loop playback volume (mix fader for the loop, separate from master volume).
// Q15 gain, applied only when the loop is mixed into the output — the recorded
// layers keep their original level, so changing it is non-destructive.
static volatile int32_t        s_loop_gain_q15 = 32768;   // 1.0
static volatile uint8_t        s_loop_vol_pct  = 100;

void looper_init(void)
{
    size_t avail = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (avail <= LOOPER_PSRAM_HEADROOM) {
        ESP_LOGW(TAG, "insufficient PSRAM for looper (%u bytes free)", (unsigned)avail);
        return;
    }
    size_t alloc = avail - LOOPER_PSRAM_HEADROOM;
    s_buf = heap_caps_malloc(alloc, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGW(TAG, "PSRAM malloc failed");
        return;
    }
    s_max_len = (uint32_t)(alloc / sizeof(int16_t));
    uint32_t secs = s_max_len / LOOPER_SAMPLE_RATE;
    ESP_LOGI(TAG, "ready — %u samples (~%u s mono)", (unsigned)s_max_len, (unsigned)secs);
}

void looper_cycle(void)
{
    if (!s_buf) return;
    switch (s_state) {
    case LOOPER_IDLE:
        s_pos      = 0;
        s_loop_len = 0;
        s_state    = LOOPER_RECORDING;
        ESP_LOGI(TAG, "REC started (buffer %u samples)", (unsigned)s_max_len);
        break;
    case LOOPER_RECORDING:
        s_loop_len = s_pos;
        s_pos      = 0;
        s_state    = LOOPER_PLAYING;
        ESP_LOGI(TAG, "PLAY started (%u samples)", (unsigned)s_loop_len);
        break;
    case LOOPER_PLAYING:
        s_state = LOOPER_OVERDUB;
        ESP_LOGI(TAG, "OVERDUB started");
        break;
    case LOOPER_OVERDUB:
        s_state = LOOPER_PLAYING;
        ESP_LOGI(TAG, "back to PLAY");
        break;
    }
}

void looper_clear(void)
{
    s_state    = LOOPER_IDLE;
    s_pos      = 0;
    s_loop_len = 0;
    ESP_LOGI(TAG, "cleared");
}

void looper_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_loop_vol_pct = percent;
    float v = percent / 100.0f;
    s_loop_gain_q15 = (int32_t)(v * v * 32768.0f);   // same perceptual curve as master volume
}
uint8_t  looper_get_volume(void)     { return s_loop_vol_pct; }
uint32_t looper_get_max_length(void) { return s_max_len; }

void looper_get_progress(looper_progress_t *out)
{
    // Snapshot for the UI (loop bar). Fields are read individually from the
    // audio thread's variables, so they may be off by one block — fine for display.
    out->state      = s_state;
    out->position   = s_pos;
    out->length     = s_loop_len;
    out->max_length = s_max_len;
    out->sample_rate = LOOPER_SAMPLE_RATE;
}

looper_state_t looper_get_state(void)    { return s_state; }
uint32_t       looper_get_position(void) { return s_pos; }
uint32_t       looper_get_length(void)   { return s_loop_len; }

// ── Audio block processing ────────────────────────────────────
// Called (via amy_audio_block_hook in output_limiter.c) from AMY's
// esp_fill_audio_buffer_task on every audio block, between
// amy_fill_buffer() and amy_i2s_write().
// block: stereo int16, interleaved L/R, n_frames samples per channel.

static inline int16_t clamp16(int32_t v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

void looper_process_block(int16_t *block, int n_frames)
{
    looper_state_t state = s_state;
    if (state == LOOPER_IDLE || !s_buf) return;

    for (int i = 0; i < n_frames; i++) {
        int16_t live_l = block[i * 2];
        int16_t live_r = block[i * 2 + 1];

        switch (state) {

        case LOOPER_RECORDING:
            if (s_pos < s_max_len) {
                // Store mono mix of live stereo
                s_buf[s_pos++] = clamp16(((int32_t)live_l + live_r) >> 1);
                if (s_pos >= s_max_len) {
                    // Buffer full — auto-stop, start playing
                    s_loop_len = s_pos;
                    s_pos      = 0;
                    s_state    = LOOPER_PLAYING;
                    state      = LOOPER_PLAYING;
                }
            }
            break;

        case LOOPER_PLAYING: {
            if (s_loop_len == 0) break;
            int16_t loop_s = (int16_t)(((int32_t)s_buf[s_pos] * s_loop_gain_q15) >> 15);
            block[i * 2]     = clamp16((int32_t)live_l + loop_s);
            block[i * 2 + 1] = clamp16((int32_t)live_r + loop_s);
            if (++s_pos >= s_loop_len) s_pos = 0;
            break;
        }

        case LOOPER_OVERDUB: {
            if (s_loop_len == 0) break;
            int16_t live_mono = clamp16(((int32_t)live_l + live_r) >> 1);
            // Bake live audio into loop (additive mix, no decay — keep it simple)
            s_buf[s_pos] = clamp16((int32_t)s_buf[s_pos] + live_mono);
            int16_t loop_s = (int16_t)(((int32_t)s_buf[s_pos] * s_loop_gain_q15) >> 15);
            block[i * 2]     = clamp16((int32_t)live_l + loop_s);
            block[i * 2 + 1] = clamp16((int32_t)live_r + loop_s);
            if (++s_pos >= s_loop_len) s_pos = 0;
            break;
        }

        default: break;
        }
    }
}
