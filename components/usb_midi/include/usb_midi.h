#ifndef USB_MIDI_H
#define USB_MIDI_H

#include <stdint.h>
#include <stdbool.h>

// MIDI Status Bytes
#define MIDI_NOTE_OFF       0x80
#define MIDI_NOTE_ON        0x90
#define MIDI_CC             0xB0
#define MIDI_PITCH_BEND     0xE0

// MIDI Control Change Numbers
#define MIDI_CC_MOD_WHEEL   0x01
#define MIDI_CC_VOLUME      0x07
#define MIDI_CC_EXPRESSION  0x0B
#define MIDI_CC_BREATH      0x02

typedef struct {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
} midi_message_t;

/**
 * Initialize USB MIDI device
 * Starts the USB peripheral and gets ready to send MIDI messages
 */
void usb_midi_init(void);

/**
 * Send a MIDI Note On message
 * @param note MIDI note number (0-127)
 * @param velocity Note velocity (0-127, where 0 = note off)
 */
void usb_midi_note_on(uint8_t note, uint8_t velocity);

/**
 * Send a MIDI Note Off message
 * @param note MIDI note number (0-127)
 * @param velocity Release velocity (typically 64)
 */
void usb_midi_note_off(uint8_t note, uint8_t velocity);

/**
 * Send a MIDI Control Change message (for expression, vibrato, etc.)
 * @param cc Control change number (0-127)
 * @param value Value (0-127)
 */
void usb_midi_control_change(uint8_t cc, uint8_t value);

/**
 * Send a MIDI Pitch Bend message
 * @param bend_value 0-16383 (8192 = center/no bend)
 */
void usb_midi_pitch_bend(uint16_t bend_value);

/**
 * Check if USB is connected and ready
 * @return true if ready to send MIDI
 */
bool usb_midi_is_connected(void);

#endif // USB_MIDI_H
