#pragma once
// Preset storage: 6 slots in NVS namespace "pst" (blobs "p0".."p5", names
// "n0".."n5", factory-set version "fac"), factory sounds, capture/apply.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PRESET_COUNT  6
#define PRESET_MAGIC  0xA5u
// Param-curve format stored in preset_t.fmt (was the always-0 "_pad" byte).
// 0 = v3 linear curves (migrated on load), 4 = current curves (amy_engine v4).
#define PRESET_FMT    4u
#define PRESET_NVS_NS "pst"

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  wave_id;
    uint8_t  flt_type;
    uint8_t  fmt;      // param-curve format, see PRESET_FMT (old saves: 0)
    uint16_t flt_cut;
    uint16_t flt_res;
    uint16_t env_atk;
    uint16_t env_dec;
    uint16_t env_sus;
    uint16_t env_rel;
    uint16_t lfo_rt;
    uint16_t lfo_dp;
    uint16_t ech_amt;
    uint16_t ech_fb;
    uint16_t gld;
    uint16_t eg1_depth;
    uint16_t eg1_decay;
    uint16_t ech_dly;
    // ── appended 2026-11 (reverb). Blobs saved before this are shorter
    //    (PRESET_SIZE_V1); they load with reverb off. Only ever APPEND fields.
    uint16_t rev_amt;
    uint16_t rev_dec;
} preset_t;
#define PRESET_SIZE_V1   offsetof(preset_t, rev_amt)   // 34-byte pre-reverb layout
#define PRESET_REV_DEC_DEFAULT 7000

// Boot: load all slots, (re)seed P6 "Juno Brass" and the factory set P1-P5.
void presets_init(void);

bool            preset_is_valid(int slot);
const preset_t *preset_get(int slot);
// Display name ("" = none → show the wave name)
const char     *preset_get_name(int slot);

void preset_nvs_load(int slot);
void preset_nvs_save(int slot);
// Set (name != NULL/"") or remove (NULL/"") a preset's display name.
void preset_name_set(int slot, const char *name);
void preset_nvs_clear(int slot);
void preset_capture(int slot);
void preset_apply(int slot);
