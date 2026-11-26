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
// #include "mux_pots.h"  // MUX pots disabled — encoder-only control

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

// ── Encoder decoder ──────────────────────────────────────────
// This encoder makes one full quadrature cycle per click and rests at AB=11:
//   CW:  11 → 01 → 00 → 10 → 11      CCW: 11 → 10 → 00 → 01 → 11
// (AB = CLK<<1 | DT). Its contacts bounce heavily, and edge interrupts could
// coalesce bounces and read a stale level, so the pins are polled every 1 ms
// from a timer and fed into a full-step state machine (Ben Buxton's table).
// A step is emitted only when a complete cycle ends back at 11; bouncing
// between neighbouring states just moves the machine back and forth.
#define ENC_R_START      0x0
#define ENC_R_CW_FINAL   0x1
#define ENC_R_CW_BEGIN   0x2
#define ENC_R_CW_NEXT    0x3
#define ENC_R_CCW_BEGIN  0x4
#define ENC_R_CCW_FINAL  0x5
#define ENC_R_CCW_NEXT   0x6
#define ENC_DIR_CW       0x10
#define ENC_DIR_CCW      0x20

static const uint8_t s_enc_table[7][4] = {
    /* R_START     */ {ENC_R_START,     ENC_R_CW_BEGIN,  ENC_R_CCW_BEGIN, ENC_R_START},
    /* R_CW_FINAL  */ {ENC_R_CW_NEXT,   ENC_R_START,     ENC_R_CW_FINAL,  ENC_R_START | ENC_DIR_CW},
    /* R_CW_BEGIN  */ {ENC_R_CW_NEXT,   ENC_R_CW_BEGIN,  ENC_R_START,     ENC_R_START},
    /* R_CW_NEXT   */ {ENC_R_CW_NEXT,   ENC_R_CW_BEGIN,  ENC_R_CW_FINAL,  ENC_R_START},
    /* R_CCW_BEGIN */ {ENC_R_CCW_NEXT,  ENC_R_START,     ENC_R_CCW_BEGIN, ENC_R_START},
    /* R_CCW_FINAL */ {ENC_R_CCW_NEXT,  ENC_R_CCW_FINAL, ENC_R_START,     ENC_R_START | ENC_DIR_CCW},
    /* R_CCW_NEXT  */ {ENC_R_CCW_NEXT,  ENC_R_CCW_FINAL, ENC_R_CCW_BEGIN, ENC_R_START},
};

// Encoder debug: records every AB change seen by the poller; the main loop logs
// one line per burst ("ENC seq=1023 edges=4 steps=+1" = one clean CW click).
#define ENC_DEBUG 0
#if ENC_DEBUG
static volatile uint8_t  s_enc_dbg[256];
static volatile uint32_t s_enc_dbg_head = 0;
#endif

// esp_timer callback, 1 kHz. Only this function writes s_enc_steps.
static void enc_poll_cb(void *arg)
{
    static uint8_t state = ENC_R_START;
    uint8_t ab = ((uint8_t)gpio_get_level(ENC_CLK)<<1)|(uint8_t)gpio_get_level(ENC_DT);
#if ENC_DEBUG
    static uint8_t last_ab = 3;
    if (ab != last_ab) {
        s_enc_dbg[s_enc_dbg_head & 0xFF] = ab;
        s_enc_dbg_head++;
        last_ab = ab;
    }
#endif
    state = s_enc_table[state & 0x0F][ab];
    if      (state & ENC_DIR_CW)  s_enc_steps++;
    else if (state & ENC_DIR_CCW) s_enc_steps--;
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
    // mux_pots_init();

    touch_telemetry_set_event_cb(on_touch);
    err = touch_telemetry_start();
    if (err != ESP_OK) ESP_LOGW(TAG, "touch init failed (0x%x)", err);

    // Encoder A/B: plain inputs, polled at 1 kHz by enc_poll_cb (no interrupts)
    gpio_config_t enc_ab = {
        .pin_bit_mask = (1ULL<<ENC_CLK)|(1ULL<<ENC_DT),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&enc_ab);
    const esp_timer_create_args_t enc_timer_args = {
        .callback = enc_poll_cb,
        .name     = "enc_poll",
    };
    esp_timer_handle_t enc_timer;
    ESP_ERROR_CHECK(esp_timer_create(&enc_timer_args, &enc_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(enc_timer, 1000)); // 1 ms

    // Encoder push button: falling-edge interrupt
    gpio_config_t enc_sw = {
        .pin_bit_mask = (1ULL<<ENC_SW),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&enc_sw);
    gpio_install_isr_service(0);
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

#if ENC_DEBUG
        {
            // Log once the encoder has been quiet for 300 ms (one burst per click)
            static uint32_t dbg_tail = 0;
            static int64_t  dbg_last_us = 0;
            static int32_t  dbg_steps0 = 0;
            uint32_t head = s_enc_dbg_head;
            int64_t  tnow = esp_timer_get_time();
            if (head != dbg_tail && dbg_last_us == 0) { dbg_last_us = tnow; dbg_steps0 = steps - delta; }
            if (head != dbg_tail && tnow - dbg_last_us > 300000) {
                char line[200]; int n = 0;
                for (uint32_t i = dbg_tail; i != head && n < (int)sizeof(line) - 4; i++)
                    n += snprintf(line + n, sizeof(line) - n, "%u", s_enc_dbg[i & 0xFF]);
                ESP_LOGI(TAG, "ENC seq=%s  edges=%u  steps=%+d",
                         line, (unsigned)(head - dbg_tail), (int)(steps - dbg_steps0));
                dbg_tail = head;
                dbg_last_us = 0;
            }
        }
#endif

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

        // mux_pots_tick();

        // Single ui_tick per loop — always called so dirty flag and timed refresh work
        ui_tick(delta, short_press, long_press, vlong_press);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
