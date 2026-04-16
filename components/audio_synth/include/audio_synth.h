#ifndef AUDIO_SYNTH_H
#define AUDIO_SYNTH_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SYNTH_SINE = 0,
    SYNTH_SQUARE = 1,
    SYNTH_TRIANGLE = 2,
    SYNTH_SAWTOOTH = 3,
} synth_waveform_t;

typedef struct {
    uint16_t frequency;
    uint16_t volume;
    synth_waveform_t waveform;
    bool is_playing;
} synth_voice_t;

/**
 * Initialize audio synthesis engine
 * @param gpio_pin GPIO pin for PWM output (DAC or PWM pin)
 */
void audio_synth_init(uint8_t gpio_pin);

/**
 * Create a new synthesizer voice
 * @param voice Pointer to voice structure
 * @param frequency Frequency in Hz (20-20000)
 * @param volume Volume (0-255)
 * @param waveform Waveform type
 */
void synth_voice_init(synth_voice_t *voice, uint16_t frequency, uint16_t volume, synth_waveform_t waveform);

/**
 * Start playing a voice
 * @param voice Pointer to voice structure
 */
void synth_voice_play(synth_voice_t *voice);

/**
 * Stop playing a voice
 * @param voice Pointer to voice structure
 */
void synth_voice_stop(synth_voice_t *voice);

/**
 * Set frequency of a voice
 * @param voice Pointer to voice structure
 * @param frequency New frequency in Hz
 */
void synth_voice_set_frequency(synth_voice_t *voice, uint16_t frequency);

/**
 * Set volume of a voice
 * @param voice Pointer to voice structure
 * @param volume New volume (0-255)
 */
void synth_voice_set_volume(synth_voice_t *voice, uint16_t volume);

#endif // AUDIO_SYNTH_H
