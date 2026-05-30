#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "amy_engine.h"
#include "touch_telemetry.h"
#include "ui.h"

static const char *TAG = "Synthewi";

// C pentatonic: C4 D4 E4 G4 A4 C5 D5 E5
static const uint8_t s_notes[8] = {60, 62, 64, 67, 69, 72, 74, 76};

static void on_touch(uint8_t pad, bool on)
{
    if (pad >= 8) return;
    if (on) {
        ESP_LOGI(TAG, "pad %u ON  note %u", pad + 1, s_notes[pad]);
        amy_engine_note_on(pad, s_notes[pad]);
    } else {
        ESP_LOGI(TAG, "pad %u OFF", pad + 1);
        amy_engine_note_off(pad);
    }
}

// ── Rotary encoder ───────────────────────────────────────────
#define ENC_CLK  GPIO_NUM_15
#define ENC_DT   GPIO_NUM_16
#define ENC_SW   GPIO_NUM_17

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

    ui_init();
    ESP_LOGI(TAG, "running");

    int32_t last_steps   = 0;
    bool    btn_pending  = false;
    bool    long_fired   = false;
    int64_t btn_start_us = 0;

    for (;;) {
        amy_engine_park_idle_oscs();

        // Encoder rotation
        int32_t steps = s_enc_steps;
        int delta = (int)(steps - last_steps);
        if (delta) { last_steps = steps; ui_tick(delta, false, false); }

        // Non-blocking long/short press detection
        if (s_btn_flag && !btn_pending) {
            s_btn_flag   = 0;
            btn_pending  = true;
            long_fired   = false;
            btn_start_us = s_btn_press_us;
        }
        if (btn_pending) {
            int64_t held = esp_timer_get_time() - btn_start_us;
            if (!long_fired && held >= 800000) {
                long_fired = true;
                ui_tick(0, false, true);  // long press fires at 800ms, no release needed
            }
            if (gpio_get_level(ENC_SW) == 1) {  // button released
                btn_pending = false;
                if (!long_fired) ui_tick(0, true, false); // short press
                long_fired = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
