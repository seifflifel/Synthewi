#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "amy_engine.h"
#include "touch_telemetry.h"
#include "looper.h"
#include "ui.h"
#include "mux_pots.h"

static const char *TAG = "Synthewi";

// C pentatonic: C4 D4 E4 G4 A4 C5 D5 E5
static const uint8_t s_base_notes[8] = {60, 62, 64, 67, 69, 72, 74, 76};
static uint8_t       s_notes[8]      = {60, 62, 64, 67, 69, 72, 74, 76};

static void apply_octave(void)
{
    for (int i = 0; i < 8; i++) {
        int n = (int)s_base_notes[i] + s_octave_shift * 12;
        s_notes[i] = (uint8_t)(n < 0 ? 0 : n > 127 ? 127 : n);
    }
}

static void on_touch(uint8_t pad, bool on)
{
    if (pad < 8) {
        ui_set_pad_active(pad, on);
        if (on) {
            ESP_LOGI(TAG, "pad %u ON  note %u", pad + 1, s_notes[pad]);
            amy_engine_note_on(pad, s_notes[pad]);
        } else {
            ESP_LOGI(TAG, "pad %u OFF", pad + 1);
            amy_engine_note_off(pad);
        }
    } else if (on) {
        static int64_t oct_last_us = 0;
        int64_t now = esp_timer_get_time();
        if (now - oct_last_us < 500000) return; // 500ms cooldown — prevents accidental double-trigger
        oct_last_us = now;
        int shift = s_octave_shift + (pad == 9 ? 1 : -1);
        if (shift >= -2 && shift <= 2) {
            s_octave_shift = shift;
            apply_octave();
            ui_set_octave(s_octave_shift);
            ESP_LOGI(TAG, "octave %+d", s_octave_shift);
        }
    }
}

// ── Rotary encoder ───────────────────────────────────────────
#define ENC_CLK  GPIO_NUM_15
#define ENC_DT   GPIO_NUM_16
#define ENC_SW   GPIO_NUM_17

// ── Waveform lever (3-position) ──────────────────────────────
#define WAVE_GPIO0  GPIO_NUM_41   // LEFT=LOW,  CENTER/RIGHT=HIGH
#define WAVE_GPIO1  GPIO_NUM_42   // RIGHT=LOW, CENTER/LEFT=HIGH
// Position 1 (left):   b0=LOW,  b1=HIGH → SQUARE
// Position 2 (center): b0=HIGH, b1=HIGH → SAW
// Position 3 (right):  b0=HIGH, b1=LOW  → TRIANGLE

static volatile int32_t s_enc_steps    = 0;
static volatile int     s_btn_flag     = 0;
static volatile int64_t s_btn_press_us = 0;

static void IRAM_ATTR enc_ab_isr(void *arg)
{
    static uint8_t old_AB = 3;
    static int8_t  acc    = 0;
    static const int8_t lut[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
    uint8_t ab = ((uint8_t)gpio_get_level(ENC_CLK)<<1)|(uint8_t)gpio_get_level(ENC_DT);
    acc += lut[(old_AB<<2)|ab];
    old_AB = ab;
    if      (acc >=  4) { s_enc_steps++; acc = 0; }
    else if (acc <= -4) { s_enc_steps--; acc = 0; }
}

static void IRAM_ATTR enc_sw_isr(void *arg)
{
    static int64_t last = 0;
    int64_t now = esp_timer_get_time();
    if (now - last < 200000) return; // 200 ms debounce — blocks release-bounce double-fire
    last = now;
    s_btn_press_us = now;
    s_btn_flag = 1;
}

// ── Entry point ──────────────────────────────────────────────
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    amy_engine_init();
    esp_log_level_set("AMY_ENGINE", ESP_LOG_WARN);
    vTaskDelay(pdMS_TO_TICKS(200)); // let AMY render task process echo event and allocate PSRAM before looper grabs it
    looper_init(); // allocates remaining PSRAM after AMY echo buffer
    mux_pots_init();

    touch_telemetry_set_event_cb(on_touch);
    err = touch_telemetry_start();
    if (err != ESP_OK) ESP_LOGW(TAG, "touch init failed (0x%x)", err);

    // Encoder GPIO
    gpio_config_t enc_ab = {
        .pin_bit_mask = (1ULL<<ENC_CLK)|(1ULL<<ENC_DT),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&enc_ab);
    gpio_config_t enc_sw = {
        .pin_bit_mask = (1ULL<<ENC_SW),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&enc_sw);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(ENC_CLK, enc_ab_isr, NULL);
    gpio_isr_handler_add(ENC_DT,  enc_ab_isr, NULL);
    gpio_isr_handler_add(ENC_SW,  enc_sw_isr, NULL);

    gpio_config_t lever_cfg = {
        .pin_bit_mask = (1ULL<<WAVE_GPIO0)|(1ULL<<WAVE_GPIO1),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&lever_cfg);

    ui_init();
    ESP_LOGI(TAG, "running");

    int32_t last_steps        = 0;
    bool    btn_pending       = false;
    bool    long_fired        = false;
    bool    vlong_fired       = false;
    int64_t btn_start_us      = 0;
    int64_t btn_dispatch_us   = 0; // time of last dispatched press (short or long)
    uint8_t lever_wave        = 0xFF; // sentinel: force apply on first tick

    for (;;) {
        amy_engine_park_idle_oscs();

        // Lever waveform selector
        {
            uint8_t b0 = (uint8_t)gpio_get_level(WAVE_GPIO0);
            uint8_t b1 = (uint8_t)gpio_get_level(WAVE_GPIO1);
            uint8_t wid = (!b0 &&  b1) ? AMY_ENGINE_WAVE_SQUARE   :
                          ( b0 && !b1) ? AMY_ENGINE_WAVE_TRIANGLE  :
                                         AMY_ENGINE_WAVE_SAW_DOWN;
            if (wid != lever_wave) {
                lever_wave = wid;
                amy_engine_set_wave(wid);
                ui_notify_lever();
            }
        }

        // Gather encoder delta
        int32_t steps = s_enc_steps;
        int delta = (int)(steps - last_steps);
        if (delta) last_steps = steps;

        // Gather button events as flags (one ui_tick call at end of loop)
        bool short_press = false, long_press = false, vlong_press = false;
        if (s_btn_flag && !btn_pending) {
            s_btn_flag = 0;
            if (esp_timer_get_time() - btn_dispatch_us >= 400000) {
                btn_pending  = true;
                long_fired   = false;
                vlong_fired  = false;
                btn_start_us = s_btn_press_us;
            }
        }
        if (btn_pending) {
            int64_t held = esp_timer_get_time() - btn_start_us;
            if (!long_fired && held >= 800000) {
                long_fired      = true;
                btn_dispatch_us = esp_timer_get_time();
                long_press      = true;
            }
            if (!vlong_fired && held >= 3500000) {
                vlong_fired     = true;
                btn_dispatch_us = esp_timer_get_time();
                vlong_press     = true;
            }
            if (gpio_get_level(ENC_SW) == 1) {
                btn_pending = false;
                if (!long_fired && !vlong_fired) {
                    btn_dispatch_us = esp_timer_get_time();
                    short_press     = true;
                }
                long_fired  = false;
                vlong_fired = false;
            }
        }

        mux_pots_tick();

        // Single ui_tick per loop — always called so dirty flag and timed refresh work
        ui_tick(delta, short_press, long_press, vlong_press);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
