#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_heap_caps.h"
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

// Encoder push button, debounced by enc_poll_cb (1 kHz). Edge counters let the
// 50 Hz main loop see every press/release even if a tap is shorter than a tick.
#define BTN_DEBOUNCE_MS   8          // level must be stable this long to count
static volatile bool     s_btn_down        = false;
static volatile uint32_t s_btn_press_seq   = 0;
static volatile uint32_t s_btn_release_seq = 0;
static volatile int64_t  s_btn_press_us    = 0;
static volatile int64_t  s_btn_release_us  = 0;

// Button gestures (detected in the main loop)
#define BTN_LONG_US     500000LL     // hold → long press (back); was 0.8 s
#define BTN_VLONG_US   2000000LL     // hold → very long press (clear loop on top screen); was 3.5 s
#define BTN_DOUBLE_US   250000LL     // 2nd press within this after a release → double click

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

    // Push button (active low): integrate-and-dump debounce. The old edge ISR
    // used a 200 ms lockout plus a 400 ms dispatch lockout, which made a
    // double click impossible.
    static uint8_t btn_cnt = 0;
    bool raw_down = (gpio_get_level(ENC_SW) == 0);
    if (raw_down != s_btn_down) {
        if (++btn_cnt >= BTN_DEBOUNCE_MS) {
            btn_cnt = 0;
            s_btn_down = raw_down;
            if (raw_down) { s_btn_press_us   = esp_timer_get_time(); s_btn_press_seq++;   }
            else          { s_btn_release_us = esp_timer_get_time(); s_btn_release_seq++; }
        }
    } else {
        btn_cnt = 0;
    }
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
    looper_init(); // allocates remaining PSRAM after AMY's echo + reverb buffers
    ESP_LOGI(TAG, "memory after init: PSRAM free %u KB (largest %u KB), internal free %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
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

    // Encoder push button: plain input, debounced in enc_poll_cb
    gpio_config_t enc_sw = {
        .pin_bit_mask = (1ULL<<ENC_SW),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&enc_sw);

    gpio_config_t lever_cfg = {
        .pin_bit_mask = (1ULL<<WAVE_GPIO0)|(1ULL<<WAVE_GPIO1),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&lever_cfg);

    ui_init();
    ESP_LOGI(TAG, "running");

    int32_t last_steps        = 0;
    uint8_t lever_wave        = 0xFF; // sentinel: force apply on first tick

    // Button gesture state machine
    enum { G_IDLE, G_DOWN1, G_WAIT2, G_DOWN2 } g_state = G_IDLE;
    uint32_t g_last_press = 0, g_last_release = 0;
    int64_t  g_t_down = 0, g_t_up = 0;
    bool     g_long_fired = false, g_vlong_fired = false;
    bool     g_turned_held = false;   // encoder turned while the button was held
    int      deferred_delta = 0;      // rotation held back one tick (see WAIT2)

    // Encoder acceleration (values only, never list navigation): smoothed
    // detents/second → small multiplier. Deliberately subtle (max ×3).
    float    enc_rate = 0.0f;

    // Master volume (hold + turn), persisted in NVS "ui"/"mvol"
    uint8_t  master_vol = 100;
    {
        nvs_handle_t h;
        if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v;
            if (nvs_get_u8(h, "mvol", &v) == ESP_OK && v <= 100) master_vol = v;
            nvs_close(h);
        }
        output_set_master_volume(master_vol);
        ESP_LOGI(TAG, "master volume %u %%", master_vol);
    }

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

        // Acceleration: EMA of detents/s (loop period 20 ms)
        enc_rate = 0.7f * enc_rate + 0.3f * ((delta < 0 ? -delta : delta) / 0.02f);
        int accel = (enc_rate > 20.0f) ? 3 : (enc_rate > 10.0f) ? 2 : 1;

        delta += deferred_delta;
        deferred_delta = 0;

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

        // Button gestures → flags (one ui_tick call at end of loop)
        //   click        : press+release < 0.8 s, no 2nd press within 250 ms
        //                  (a single click is reported 250 ms after release)
        //   double click : 2nd press within 250 ms of the 1st release (fires on press)
        //   long         : held 0.5 s  (fires while held)
        //   very long    : held 2 s    (fires while held, after long)
        //   hold + turn  : master volume (cancels long / click for that press)
        bool short_press = false, long_press = false, vlong_press = false, double_click = false;
        {
            int64_t  now      = esp_timer_get_time();
            uint32_t presses  = s_btn_press_seq;
            uint32_t releases = s_btn_release_seq;
            bool new_press   = (presses  != g_last_press);
            bool new_release = (releases != g_last_release);
            g_last_press = presses; g_last_release = releases;
            bool down = s_btn_down;

            if (g_state == G_IDLE && new_press) {
                g_state = G_DOWN1; g_t_down = s_btn_press_us;
                g_long_fired = g_vlong_fired = g_turned_held = false;
                new_press = false;
            }
            if (g_state == G_DOWN1) {
                if (down && !new_release) {
                    // Hold + turn: master volume. Takes the rotation away from the UI
                    // and cancels the long press for this hold (unless already fired).
                    if (delta && !g_long_fired) {
                        g_turned_held = true;
                        int v = (int)master_vol + delta * 5 * accel;   // 5 % per detent
                        master_vol = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
                        output_set_master_volume(master_vol);
                        delta = 0;
                    }
                    if (!g_turned_held) {
                        if (!g_long_fired  && now - g_t_down >= BTN_LONG_US)  { g_long_fired  = true; long_press  = true; }
                        if (!g_vlong_fired && now - g_t_down >= BTN_VLONG_US) { g_vlong_fired = true; vlong_press = true; }
                    }
                } else {                                   // released
                    if (g_turned_held) {                   // volume gesture: persist, no click
                        nvs_handle_t h;
                        if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
                            nvs_set_u8(h, "mvol", master_vol); nvs_commit(h); nvs_close(h);
                        }
                        ESP_LOGI(TAG, "master volume %u %%", master_vol);
                        g_state = G_IDLE;
                    } else if (g_long_fired) {
                        g_state = G_IDLE;                  // hold gestures end here
                    } else {
                        g_state = G_WAIT2; g_t_up = s_btn_release_us;
                    }
                    new_release = false;
                }
            }
            if (g_state == G_WAIT2) {
                if (new_press)                         { double_click = true; g_state = G_DOWN2; }
                else if (now - g_t_up >= BTN_DOUBLE_US){ short_press  = true; g_state = G_IDLE;  }
                else if (delta) {
                    // Turning right after a click: deliver the click NOW and the
                    // rotation next tick, so the click acts on what was selected.
                    short_press = true; g_state = G_IDLE;
                    deferred_delta = delta; delta = 0;
                }
            }
            if (g_state == G_DOWN2 && !down && (new_release || !new_press)) {
                g_state = G_IDLE;                      // wait for the 2nd release
            }
        }

        // mux_pots_tick();

        // Single ui_tick per loop — always called so dirty flag and timed refresh work
        ui_tick(delta, accel, short_press, long_press, vlong_press, double_click);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
