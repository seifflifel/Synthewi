// Key + scale for the note pads — see scale.h.

#include "scale.h"
#include "nvs.h"

static const char * const s_keys[SCALE_KEYS] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char * const s_types[SCALE_TYPES] = {"PENTA", "MAJOR", "MINOR", "MIN PENTA", "DORIAN", "BLUES", "CHROMATIC"};
// Semitones from the root for the 8 pads.
static const uint8_t s_steps[SCALE_TYPES][8] = {
    {0, 2, 4, 7, 9, 12, 14, 16},    // major pentatonic (original layout)
    {0, 2, 4, 5, 7, 9, 11, 12},     // major
    {0, 2, 3, 5, 7, 8, 10, 12},     // natural minor
    {0, 3, 5, 7, 10, 12, 15, 17},   // minor pentatonic
    {0, 2, 3, 5, 7, 9, 10, 12},     // dorian
    {0, 3, 5, 6, 7, 10, 12, 15},    // blues
    {0, 1, 2, 3, 4, 5, 6, 7},       // chromatic
};

static volatile uint8_t s_key = 0, s_type = 0;

void scale_init(void)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        uint8_t k = 0, t = 0;
        if (nvs_get_u8(h, "key", &k) == ESP_OK && k < SCALE_KEYS) s_key = k;
        if (nvs_get_u8(h, "scale", &t) == ESP_OK && t < SCALE_TYPES) s_type = t;
        nvs_close(h);
    }
}

void scale_set(uint8_t key, uint8_t type)
{
    if (key < SCALE_KEYS) s_key = key;
    if (type < SCALE_TYPES) s_type = type;
}

void scale_save(void)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "key", s_key);
        nvs_set_u8(h, "scale", s_type);
        nvs_commit(h);
        nvs_close(h);
    }
}

uint8_t     scale_key(void)               { return s_key; }
uint8_t     scale_type(void)              { return s_type; }
const char *scale_key_name(uint8_t key)   { return s_keys[key % SCALE_KEYS]; }
const char *scale_type_name(uint8_t type) { return s_types[type % SCALE_TYPES]; }

// Root C4 (60) + key; keys above F# start an octave lower so the pads stay
// around the original range.
uint8_t scale_pad_note(int pad)
{
    int root = 60 + s_key - (s_key > 6 ? 12 : 0);
    return (uint8_t)(root + s_steps[s_type % SCALE_TYPES][pad & 7]);
}
