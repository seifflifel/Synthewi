#pragma once
// Key + scale for the 8 note pads (pad i = scale degree i from the root).
// Global setting, persisted in NVS "ui"/"key" + "ui"/"scale". Default C PENTA
// (= the original fixed layout C D E G A C D E).

#include <stdint.h>

#define SCALE_KEYS   12
#define SCALE_TYPES  7

void        scale_init(void);                       // load from NVS (after nvs_flash_init)
void        scale_set(uint8_t key, uint8_t type);   // apply now (no NVS write)
void        scale_save(void);                       // persist current key + type
uint8_t     scale_key(void);
uint8_t     scale_type(void);
const char *scale_key_name(uint8_t key);            // "C", "C#", ...
const char *scale_type_name(uint8_t type);          // "MAJOR", "PENTA", ...
uint8_t     scale_pad_note(int pad);                // MIDI note for pad 0..7 (octave shift not applied)
