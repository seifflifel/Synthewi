#include "usb_midi.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "class/midi/midi_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "USB_MIDI";

static bool usb_initialized = false;

enum interface_count {
    ITF_NUM_MIDI = 0,
    ITF_NUM_MIDI_STREAMING,
    ITF_COUNT
};

enum usb_endpoints {
    EP_EMPTY = 0,
    EPNUM_MIDI,
};

#define TUSB_DESCRIPTOR_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_MIDI * TUD_MIDI_DESC_LEN)

static const char *s_midi_str_desc[5] = {
    (char[]){0x09, 0x04},
    "Synthewi",
    "Synthewi MIDI",
    "123456",
    "Touch MIDI",
};

static const uint8_t s_midi_fs_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUSB_DESCRIPTOR_TOTAL_LEN, 0, 100),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 4, EPNUM_MIDI, (0x80 | EPNUM_MIDI), 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t s_midi_hs_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUSB_DESCRIPTOR_TOTAL_LEN, 0, 100),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 4, EPNUM_MIDI, (0x80 | EPNUM_MIDI), 512),
};
#endif

/**
 * Initialize TinyUSB with native MIDI device class
 */
void usb_midi_init(void)
{
    ESP_LOGI(TAG, "Initializing TinyUSB MIDI device...");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.string = s_midi_str_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_midi_str_desc) / sizeof(s_midi_str_desc[0]);
    tusb_cfg.descriptor.full_speed_config = s_midi_fs_cfg_desc;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_midi_hs_cfg_desc;
    tusb_cfg.descriptor.qualifier = NULL;
#endif

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(ret));
        return;
    }

    // Wait briefly for host enumeration.
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    usb_initialized = true;
    ESP_LOGI(TAG, "TinyUSB MIDI ready");
}

/**
 * Send MIDI message (3 bytes) via TinyUSB MIDI class.
 */
static void send_midi_message(uint8_t status, uint8_t data1, uint8_t data2)
{
    if (!usb_initialized) {
        return;
    }

    // Only send after MIDI interface is mounted.
    if (!tud_midi_mounted()) {
        return;
    }

    uint8_t midi_msg[3] = {status, data1, data2};
    (void)tud_midi_stream_write(0, midi_msg, sizeof(midi_msg));
}

void usb_midi_note_on(uint8_t note, uint8_t velocity)
{
    if (!usb_initialized) return;

    uint8_t note_clean = note & 0x7F;
    uint8_t vel_clean = velocity & 0x7F;
    
    send_midi_message(MIDI_NOTE_ON, note_clean, vel_clean);
}

void usb_midi_note_off(uint8_t note, uint8_t velocity)
{
    if (!usb_initialized) return;

    uint8_t note_clean = note & 0x7F;
    uint8_t vel_clean = velocity & 0x7F;
    
    send_midi_message(MIDI_NOTE_OFF, note_clean, vel_clean);
}

void usb_midi_control_change(uint8_t cc, uint8_t value)
{
    if (!usb_initialized) return;

    uint8_t cc_clean = cc & 0x7F;
    uint8_t val_clean = value & 0x7F;
    
    send_midi_message(MIDI_CC, cc_clean, val_clean);
}

void usb_midi_pitch_bend(uint16_t bend_value)
{
    if (!usb_initialized) return;

    // Pitch bend uses 14-bit value (split across 2 data bytes)
    uint8_t lsb = (bend_value) & 0x7F;
    uint8_t msb = (bend_value >> 7) & 0x7F;
    
    send_midi_message(MIDI_PITCH_BEND, lsb, msb);
}

bool usb_midi_is_connected(void)
{
    return usb_initialized && tud_midi_mounted();
}
