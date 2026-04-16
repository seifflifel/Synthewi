#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "touch_control.h"
#include "usb_midi.h"

static const char *TAG = "Synthewi";
static const char *TOUCH_TAG = "TOUCH";

#define NUM_TOUCH_PADS 4
#define TOUCH_SCOPE_MODE 0
#define TOUCH_LOOP_MS 20
#define TOUCH_DEBUG_PRINT_MS 1000
#define TOUCH_MANUAL_ON_DELTA 11000
#define TOUCH_MANUAL_OFF_DELTA 9000
#define TOUCH_NOTE_VELOCITY 100

// Touch sensors
touch_sensor_t touch_pad[NUM_TOUCH_PADS];

#if !TOUCH_SCOPE_MODE
// MIDI note for each pad
static const uint8_t midi_notes[NUM_TOUCH_PADS] = {
    60,  // C4
    62,  // D4
    64,  // E4
    65,  // F4
};

// Track which notes are currently playing
static bool note_playing[NUM_TOUCH_PADS] = {false};
#endif

static uint16_t abs_delta_u16(const touch_sensor_t *sensor)
{
    int32_t d = (int32_t)sensor->raw_value - (int32_t)sensor->baseline_value;
    if (d < 0) {
        d = -d;
    }
    return (uint16_t)d;
}

void app_main(void)
{
    // Keep global logs minimal, then enable only relevant tags for tuning.
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set(TOUCH_TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "Synthewi - Expressive Touch MIDI Controller");
    ESP_LOGI(TAG, "Starting up...\n");

#if !TOUCH_SCOPE_MODE
    // Initialize USB MIDI
    usb_midi_init();
    ESP_LOGI(TAG, "USB MIDI initialized successfully");
#else
    ESP_LOGI(TAG, "TOUCH_SCOPE_MODE active: streaming raw data only (MIDI disabled)");
#endif

    // Touch profile: GPIO4/GPIO5/GPIO6/GPIO7 -> touch channels 4/5/6/7.
    uint8_t touch_channels[NUM_TOUCH_PADS] = {4, 5, 6, 7};
    
    for (int i = 0; i < NUM_TOUCH_PADS; i++) {
        ESP_LOGI(TAG, "Initializing touch pad %d on touch channel %d", i, touch_channels[i]);
        touch_sensor_init(&touch_pad[i], touch_channels[i], 80);
    }

    ESP_LOGI(TAG, "Initialization complete!");
#if TOUCH_SCOPE_MODE
    ESP_LOGI(TAG, "Watch smooth/base/delta values and decide threshold manually\n");
#else
    ESP_LOGI(TAG, "Manual threshold mode: ON >= %d, OFF < %d\n", TOUCH_MANUAL_ON_DELTA, TOUCH_MANUAL_OFF_DELTA);
#endif

    uint32_t tick_count = 0;

    // Main control loop
    while (1) {
#if TOUCH_SCOPE_MODE
        // Stream raw values for visual threshold tuning.
        for (int i = 0; i < NUM_TOUCH_PADS; i++) {
            touch_sensor_sample(&touch_pad[i]);
        }

        uint32_t debug_period_ticks = TOUCH_DEBUG_PRINT_MS / TOUCH_LOOP_MS;
        if ((debug_period_ticks > 0) && ((tick_count % debug_period_ticks) == 0)) {
                ESP_LOGI(TAG,
                     "CH4 s=%u b=%u d=%u | CH5 s=%u b=%u d=%u | CH6 s=%u b=%u d=%u | CH7 s=%u b=%u d=%u",
                     touch_pad[0].raw_value,
                     touch_pad[0].baseline_value,
                     abs_delta_u16(&touch_pad[0]),
                     touch_pad[1].raw_value,
                     touch_pad[1].baseline_value,
                     abs_delta_u16(&touch_pad[1]),
                     touch_pad[2].raw_value,
                     touch_pad[2].baseline_value,
                     abs_delta_u16(&touch_pad[2]),
                     touch_pad[3].raw_value,
                     touch_pad[3].baseline_value,
                     abs_delta_u16(&touch_pad[3]));
        }
#else
        for (int i = 0; i < NUM_TOUCH_PADS; i++) {
            // Use raw sampled data and a fixed manual threshold gate.
            touch_sensor_sample(&touch_pad[i]);

            uint16_t abs_delta = abs_delta_u16(&touch_pad[i]);

            uint32_t debug_period_ticks = TOUCH_DEBUG_PRINT_MS / TOUCH_LOOP_MS;
            if ((debug_period_ticks > 0) && ((tick_count % debug_period_ticks) == 0)) {
                ESP_LOGI(TAG,
                         "P%d ch%u base=%u raw=%u delta=%u gate_on=%d gate_off=%d",
                         i,
                         touch_pad[i].touch_channel,
                         touch_pad[i].baseline_value,
                         touch_pad[i].raw_value,
                         abs_delta,
                         TOUCH_MANUAL_ON_DELTA,
                         TOUCH_MANUAL_OFF_DELTA);
            }

            bool touching_now = note_playing[i]
                                   ? (abs_delta >= TOUCH_MANUAL_OFF_DELTA)
                                   : (abs_delta >= TOUCH_MANUAL_ON_DELTA);

            if (touching_now) {
                if (!note_playing[i]) {
                    usb_midi_note_on(midi_notes[i], TOUCH_NOTE_VELOCITY);
                    note_playing[i] = true;
                    ESP_LOGI(TAG, "Pad %d: Note ON (note=%d, delta=%u)", i, midi_notes[i], abs_delta);
                }
            } else {
                if (note_playing[i]) {
                    usb_midi_note_off(midi_notes[i], 64);
                    note_playing[i] = false;
                    ESP_LOGI(TAG, "Pad %d: Note OFF (delta=%u)", i, abs_delta);
                }
            }
        }
#endif

        tick_count++;
        // Update at 20ms interval (50 Hz) for responsive MIDI
        vTaskDelay(TOUCH_LOOP_MS / portTICK_PERIOD_MS);
    }
}
