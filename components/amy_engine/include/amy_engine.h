#ifndef AMY_ENGINE_H
#define AMY_ENGINE_H

#include <stddef.h>
#include <stdint.h>

void amy_engine_init(void);
uint8_t amy_engine_toggle_next_voice(void);
void amy_engine_all_notes_off(void);
void amy_engine_render_mono_16(int16_t *out, size_t samples);
void amy_engine_render_wav_mono_16(int16_t *out, size_t samples);

#endif
