#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "amy_engine.h"

#define MUX_GPIO_A    13
#define MUX_GPIO_B    14
#define MUX_GPIO_C    47
#define MUX_ADC_UNIT  ADC_UNIT_2
#define MUX_ADC_CH    ADC_CHANNEL_0   // GPIO 11

#define MUX_CH_COUNT  6
#define MUX_DEADBAND_IDLE     74    // ~2% change after 10s idle — prevents drift
#define MUX_DEADBAND_ACTIVE   18    // ~0.5% change while moving — fine control
#define MUX_IDLE_TIMEOUT_US   10000000LL  // 10 seconds
#define MUX_LOG_MIN   200   // min val change (0-10000) to print — only intentional moves

// ESP32-S3 ADC practical range with ADC_ATTEN_DB_12 and 3.3V pot supply.
// Max doesn't reach 4095 — adjust MUX_ADC_MIN/MAX once you observe the extremes.
#define MUX_ADC_MAX  3800
#define MUX_ADC_MIN  100

// Set to 1 to print raw ADC for every channel every 3s — no smoothing, no deadband
#define MUX_DEBUG_RAW 0

static const char *MUX_TAG = "mux";

// ─────────────────────────────────────────────────────────
// Parameter ranges (0-10000): each pot can map to custom min/max
// Easy to tweak here instead of changing everywhere
// ─────────────────────────────────────────────────────────
typedef struct {
    uint16_t min;  // 0-10000 minimum value
    uint16_t max;  // 0-10000 maximum value
    const char *name;
} mux_param_range_t;

// [0]=attack [1]=release [2]=cutoff [3]=res [4]=echo [5]=glide
static const mux_param_range_t s_param_range[MUX_CH_COUNT] = {
    {0, 10000, "attack"},      // 0-10000 → 2-2000 ms in amy_engine
    {0, 10000, "release"},     // 0-10000 → 10-5000 ms in amy_engine
    {0, 10000, "cutoff"},      // 0-10000 → 13-12000 Hz (exponential) in amy_engine
    {0, 10000, "resonance"},   // 0-10000 → Q 0.7-11 (exponential) in amy_engine
    {0, 10000, "echo"},        // 0-10000 → 0.0-1.0 level in amy_engine
    {0, 10000, "glide"},       // 0-10000 → 0-500 ms portamento in amy_engine
};

// 0-10000 cached param values — [0]=attack [1]=release [2]=cutoff [3]=res [4]=echo [5]=glide
static uint16_t s_v[MUX_CH_COUNT];

// Non-pot params: seeded from engine at init, not pot-controlled
static uint16_t s_decay, s_sustain;

static int32_t  s_smooth[MUX_CH_COUNT];    // EMA-filtered ADC values
static int32_t  s_accepted[MUX_CH_COUNT];  // last deadband-passed smooth value
static uint16_t s_log_val[MUX_CH_COUNT];    // last printed val for log threshold
static int64_t  s_log_time[MUX_CH_COUNT];  // last print timestamp (µs)

// Pot locking on preset load: prevents parameter jumps when preset loads at different pot position
static bool     s_locked[MUX_CH_COUNT];     // true = follow preset, false = follow pot
static uint16_t s_preset_val[MUX_CH_COUNT]; // 0-10000 preset values (from engine) when locked

// Dual-threshold hysteresis: tracks idle time to use 2% threshold when resting, 0.5% when moving
static int64_t  s_last_move_us[MUX_CH_COUNT]; // timestamp of last accepted pot movement

static uint8_t s_ch;
static adc_oneshot_unit_handle_t s_adc2;

static inline void mux_select(uint8_t ch) {
    gpio_set_level(MUX_GPIO_A, (ch >> 0) & 1);
    gpio_set_level(MUX_GPIO_B, (ch >> 1) & 1);
    gpio_set_level(MUX_GPIO_C, (ch >> 2) & 1);
}

static void mux_pots_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << MUX_GPIO_A) | (1ULL << MUX_GPIO_B) | (1ULL << MUX_GPIO_C),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = MUX_ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc2));

    adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc2, MUX_ADC_CH, &ch_cfg));

    // Seed engine params
    amy_engine_state_t st;
    amy_engine_get_state(&st);
    s_v[0]    = st.env_attack;
    s_v[1]    = st.env_release;
    s_v[2]    = st.filter_cutoff;
    s_v[3]    = st.filter_resonance;
    s_v[4]    = st.echo_amount;
    s_v[5]    = st.glide;
    s_decay   = st.env_decay;
    s_sustain = st.env_sustain;

    // Pre-warm EMA: read each channel 8× so smooth[] seeds at actual pot position
    for (int c = 0; c < MUX_CH_COUNT; c++) {
        mux_select(c);
        vTaskDelay(pdMS_TO_TICKS(5));  // let MUX switch settle
        int32_t acc = 0;
        for (int j = 0; j < 8; j++) {
            int tmp = 2048;
            adc_oneshot_read(s_adc2, MUX_ADC_CH, &tmp);
            acc += tmp;
        }
        s_smooth[c]   = acc / 8;
        s_accepted[c] = s_smooth[c];
        int32_t sv = (s_smooth[c] - MUX_ADC_MIN) * 10000L / (MUX_ADC_MAX - MUX_ADC_MIN);
        if (sv < 0)     sv = 0;
        if (sv > 10000) sv = 10000;
        s_log_val[c] = (uint16_t)sv;
    }

    // Initialize pot locking state (start unlocked; lock only on preset load)
    for (int c = 0; c < MUX_CH_COUNT; c++) {
        s_locked[c]       = false;
        s_preset_val[c]   = s_v[c];
        s_last_move_us[c] = esp_timer_get_time();
    }

    s_ch = 0;
    mux_select(0);
    ESP_LOGI(MUX_TAG, "init ok  A=GPIO%d B=GPIO%d C=GPIO%d Z=GPIO11",
             MUX_GPIO_A, MUX_GPIO_B, MUX_GPIO_C);
}

static void mux_pots_tick(void) {
    int raw;
    if (adc_oneshot_read(s_adc2, MUX_ADC_CH, &raw) != ESP_OK) goto advance;

#if MUX_DEBUG_RAW
    {
        static int dbg_raw[6] = {0};
        static int64_t dbg_last = 0;
        dbg_raw[s_ch] = raw;
        int64_t dbg_now = esp_timer_get_time();
        if ((dbg_now - dbg_last) >= 3000000LL) {
            dbg_last = dbg_now;
            static const char *n[] = {"ATK", "REL", "CUT", "RES", "ECH", "GLD"};
            ESP_LOGI(MUX_TAG, "raw:%s%4d %s%4d %s%4d %s%4d %s%4d %s%4d",
                     n[0], dbg_raw[0], n[1], dbg_raw[1], n[2], dbg_raw[2],
                     n[3], dbg_raw[3], n[4], dbg_raw[4], n[5], dbg_raw[5]);
        }
    }
#endif

    // EMA: alpha=0.125 → smooth = (smooth×7 + raw) / 8
    s_smooth[s_ch] = (s_smooth[s_ch] * 7 + raw) >> 3;

    int64_t now = esp_timer_get_time();
    int64_t idle_us  = now - s_last_move_us[s_ch];
    int32_t deadband = (idle_us > MUX_IDLE_TIMEOUT_US) ? MUX_DEADBAND_IDLE : MUX_DEADBAND_ACTIVE;

    if (abs(s_smooth[s_ch] - s_accepted[s_ch]) < deadband) goto advance;

    s_last_move_us[s_ch] = now;

    if (s_locked[s_ch]) {
        // Smooth takeover: seed EMA at the preset ADC equivalent so the parameter
        // ramps from the loaded preset value toward the physical pot position.
        int32_t preset_adc = (int32_t)s_preset_val[s_ch] * (MUX_ADC_MAX - MUX_ADC_MIN) / 10000 + MUX_ADC_MIN;
        s_smooth[s_ch]   = preset_adc;
        s_accepted[s_ch] = preset_adc;
        s_locked[s_ch]   = false;
        ESP_LOGI(MUX_TAG, "ch%d unlocked (smooth takeover)", s_ch);
        goto advance;  // EMA converges toward pot on next ticks — no setter jump this tick
    }

    // ── Scaling: ADC → 0.0-1.0 normalization → param-specific min/max ──
    s_accepted[s_ch] = s_smooth[s_ch];
    {
        float norm = (float)(s_smooth[s_ch] - MUX_ADC_MIN) / (float)(MUX_ADC_MAX - MUX_ADC_MIN);
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;

        uint16_t pmin = s_param_range[s_ch].min;
        uint16_t pmax = s_param_range[s_ch].max;
        uint16_t val  = pmin + (uint16_t)(norm * (float)(pmax - pmin));

        s_v[s_ch] = val;

        switch (s_ch) {
            case 0: case 1:
                amy_engine_set_adsr(s_v[0], s_decay, s_sustain, s_v[1]);
                break;
            case 2: case 3:
                amy_engine_set_filter(s_v[2], s_v[3]);
                break;
            case 4: {
                // Echo feedback tracks amount at 60% — wetter without runaway feedback
                amy_engine_set_echo(val, val);
                break;
            }
            case 5:
                amy_engine_set_glide(s_v[5]);
                break;
        }

        // Print at most once per second per channel, and only on significant moves
        if (abs((int)val - (int)s_log_val[s_ch]) >= MUX_LOG_MIN &&
            (now - s_log_time[s_ch]) >= 1000000LL) {
            static const char *names[] = {"attack","release","cutoff","resonance","echo","glide"};
            s_log_val[s_ch]  = val;
            s_log_time[s_ch] = now;
            if (s_ch == 4) {
                ESP_LOGI(MUX_TAG, "%-10s amt=%5u fb=%5u", names[s_ch], val, val);
            } else {
                ESP_LOGI(MUX_TAG, "%-10s %5u", names[s_ch], val);
            }
        }
    }

advance:
    // Advance to next channel — MUX settles during the 20ms vTaskDelay in main loop
    s_ch = (s_ch + 1) % MUX_CH_COUNT;
    mux_select(s_ch);
}

// ─────────────────────────────────────────────────────────
// Call this when a preset is loaded to lock all pots.
// Reads the preset values from the engine (not from pot positions)
// so the engine's loaded state is preserved until each pot is moved.
// ─────────────────────────────────────────────────────────
static void mux_pots_on_preset_load(void) {
    amy_engine_state_t st;
    amy_engine_get_state(&st);
    s_preset_val[0] = st.env_attack;
    s_preset_val[1] = st.env_release;
    s_preset_val[2] = st.filter_cutoff;
    s_preset_val[3] = st.filter_resonance;
    s_preset_val[4] = st.echo_amount;
    s_preset_val[5] = st.glide;

    // Also refresh non-pot ADSR params so the engine call stays in sync
    s_decay   = st.env_decay;
    s_sustain = st.env_sustain;

    for (int c = 0; c < MUX_CH_COUNT; c++) {
        s_locked[c] = true;
        // Don't touch s_smooth / s_accepted — keep tracking real pot so unlock is natural
    }
    ESP_LOGI(MUX_TAG, "all pots locked to preset");
}

// ─────────────────────────────────────────────────────────
// Get current pot value (0-10000) for UI display
// ─────────────────────────────────────────────────────────
static uint16_t __attribute__((unused)) mux_pots_get_value(uint8_t ch) {
    if (ch >= MUX_CH_COUNT) return 0;
    return s_v[ch];
}
 
// ─────────────────────────────────────────────────────────
// Get whether a pot is currently locked (following preset)
// ─────────────────────────────────────────────────────────
static bool __attribute__((unused)) mux_pots_is_locked(uint8_t ch) {
    if (ch >= MUX_CH_COUNT) return false;
    return s_locked[ch];
}
