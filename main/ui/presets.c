// Presets — NVS load/save/clear/migrate, names, factory presets. See presets.h.

#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "esp_log.h"
#include "amy_engine.h"
#include "presets.h"

static preset_t s_presets[PRESET_COUNT];
static bool     s_preset_valid[PRESET_COUNT];
// Preset display names, stored as separate NVS strings ("n0".."n5") so the
// preset_t blob format stays unchanged. Empty = generic "P1".."P6" (shows the
// wave name instead). Factory presets are named; SAVE and CLR clear the name.
#define PRESET_NAME_LEN 8   // max chars that fit in a preset cell (6 px/char)
static char     s_preset_name[PRESET_COUNT][PRESET_NAME_LEN + 1];

bool            preset_is_valid(int slot) { return s_preset_valid[slot]; }
const preset_t *preset_get(int slot)      { return &s_presets[slot]; }
const char     *preset_get_name(int slot) { return s_preset_name[slot]; }

// ── Preset helpers ────────────────────────────────────────────

// Convert a preset saved with the old v3 (linear) curves to the current ones,
// keeping its physical attack/decay/release/cutoff/EG1-decay/LFO-rate.
static void preset_migrate(preset_t *p)
{
    if (p->fmt == 0) {
        // preset_t is packed — copy through aligned locals (no pointers to packed members)
        uint16_t atk = p->env_atk, dec = p->env_dec, rel = p->env_rel;
        uint16_t cut = p->flt_cut, fdec = p->eg1_decay, lfo = p->lfo_rt;
        amy_engine_migrate_v3(&atk, &dec, &rel, &cut, &fdec, &lfo);
        p->env_atk = atk; p->env_dec = dec; p->env_rel = rel;
        p->flt_cut = cut; p->eg1_decay = fdec; p->lfo_rt = lfo;
    }
    p->fmt = PRESET_FMT;
}

void preset_nvs_load(int slot)
{
    char key[4];
    snprintf(key, sizeof(key), "p%d", slot);
    nvs_handle_t h;
    if (nvs_open(PRESET_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        s_preset_valid[slot] = false;
        s_preset_name[slot][0] = '\0';
        return;
    }
    size_t sz = sizeof(preset_t);
    esp_err_t err = nvs_get_blob(h, key, &s_presets[slot], &sz);
    char nkey[4];
    snprintf(nkey, sizeof(nkey), "n%d", slot);
    size_t nlen = sizeof(s_preset_name[slot]);
    if (nvs_get_str(h, nkey, s_preset_name[slot], &nlen) != ESP_OK)
        s_preset_name[slot][0] = '\0';
    nvs_close(h);
    s_preset_valid[slot] = (err == ESP_OK
                            && (sz == sizeof(preset_t) || sz == PRESET_SIZE_V1)
                            && s_presets[slot].magic == PRESET_MAGIC);
    if (s_preset_valid[slot] && sz == PRESET_SIZE_V1) {
        // Pre-reverb preset: fill the appended fields and re-save in the new size
        s_presets[slot].rev_amt = 0;
        s_presets[slot].rev_dec = PRESET_REV_DEC_DEFAULT;
        preset_nvs_save(slot);
    }
    if (s_preset_valid[slot] && s_presets[slot].fmt != PRESET_FMT) {
        preset_migrate(&s_presets[slot]);
        preset_nvs_save(slot);   // persist converted values once
    }
}

void preset_nvs_save(int slot)
{
    char key[4];
    snprintf(key, sizeof(key), "p%d", slot);
    nvs_handle_t h;
    if (nvs_open(PRESET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, &s_presets[slot], sizeof(preset_t));
    nvs_commit(h);
    nvs_close(h);
}

// Set (name != NULL/"") or remove (NULL/"") a preset's display name.
void preset_name_set(int slot, const char *name)
{
    char nkey[4];
    snprintf(nkey, sizeof(nkey), "n%d", slot);
    nvs_handle_t h;
    if (nvs_open(PRESET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (name && name[0]) {
        snprintf(s_preset_name[slot], sizeof(s_preset_name[slot]), "%s", name);
        nvs_set_str(h, nkey, s_preset_name[slot]);
    } else {
        s_preset_name[slot][0] = '\0';
        nvs_erase_key(h, nkey);
    }
    nvs_commit(h);
    nvs_close(h);
}

void preset_nvs_clear(int slot)
{
    char key[4];
    snprintf(key, sizeof(key), "p%d", slot);
    nvs_handle_t h;
    if (nvs_open(PRESET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
    s_preset_valid[slot] = false;
    memset(&s_presets[slot], 0, sizeof(preset_t));
    preset_name_set(slot, NULL);   // cleared slot loses its name
}

void preset_capture(int slot)
{
    amy_engine_state_t st;
    amy_engine_get_state(&st);
    preset_t *p = &s_presets[slot];
    p->magic    = PRESET_MAGIC;
    p->wave_id  = st.wave_id;
    p->flt_type = st.filter_type;
    p->fmt      = PRESET_FMT;
    p->flt_cut  = st.filter_cutoff;
    p->flt_res  = st.filter_resonance;
    p->env_atk  = st.env_attack;
    p->env_dec  = st.env_decay;
    p->env_sus  = st.env_sustain;
    p->env_rel  = st.env_release;
    p->lfo_rt   = st.lfo_rate;
    p->lfo_dp   = st.lfo_depth;
    p->ech_amt   = st.echo_amount;
    p->ech_fb    = st.echo_feedback;
    p->gld       = st.glide;
    p->eg1_depth = st.filter_env_depth;
    p->eg1_decay = st.filter_env_decay;
    p->ech_dly   = st.echo_delay;
    p->rev_amt   = st.reverb_amount;
    p->rev_dec   = st.reverb_decay;
    s_preset_valid[slot] = true;
}

void preset_apply(int slot)
{
    if (!s_preset_valid[slot]) return;
    preset_t *p = &s_presets[slot];
    amy_engine_set_wave(p->wave_id);
    amy_engine_set_filter_type(p->flt_type);
    amy_engine_set_filter(p->flt_cut, p->flt_res);
    amy_engine_set_adsr(p->env_atk, p->env_dec, p->env_sus, p->env_rel);
    amy_engine_set_lfo(p->lfo_rt, p->lfo_dp);
    amy_engine_set_echo(p->ech_amt, p->ech_fb);
    amy_engine_set_echo_delay(p->ech_dly);
    amy_engine_set_reverb(p->rev_amt, p->rev_dec);
    amy_engine_set_filter_env(p->eg1_depth, p->eg1_decay);
    amy_engine_set_glide(p->gld);
    amy_engine_save_state();
    // mux_pots_on_preset_load();  // MUX disabled
}

// ── Factory presets P1–P5 ─────────────────────────────────────
// Written once into slots 1-5 (P6 is left alone). Re-seeding happens only when
// FACTORY_SET_VERSION is bumped, so user saves in P1-P5 survive reboots.
// Values use the v4 curves (see amy_engine.c header table). Approx physical values
// are noted per line; the encoder moves in 500 steps, all values sit on that grid.
// v1: P1-P5 contents.  v2: preset names (only applied to slots whose contents
// still exactly match the factory preset, so user edits are never renamed).
// v3: P5 renamed VOWEL -> WAHWAH (names re-applied with the same rule).
// v4: reverb added to P1-P5 (only slots still holding the untouched factory sound).
#define FACTORY_SET_VERSION 4

static const char * const s_factory_names[5] = { "ACID", "DREAMPAD", "KALIMBA", "WOBBLE", "WAHWAH" };

static const preset_t s_factory_presets[5] = {
    // P1 "ACID LINE" — squelchy 303-style: low resonant LPF slammed open by a fast
    // filter envelope, short notes, slides between pads, 1/8 echo. Try OCT -1.
    { .magic = PRESET_MAGIC, .wave_id = AMY_ENGINE_WAVE_SAW_DOWN, .flt_type = AMY_ENGINE_FILTER_LPF,
      .fmt = PRESET_FMT,
      .flt_cut = 2000,  /* ~190 Hz   */  .flt_res = 8500,  /* Q ~7.4    */
      .env_atk = 0,     /* 1 ms      */  .env_dec = 6000,  /* ~275 ms   */
      .env_sus = 4000,  /* 40 %      */  .env_rel = 3500,  /* ~105 ms   */
      .lfo_rt  = 4000,  .lfo_dp = 0,
      .ech_amt = 2500,  .ech_fb = 3500,  .ech_dly = 1667,  /* 1/8 s    */
      .gld     = 1000,  /* 50 ms      */
      .eg1_depth = 7000, /* +3.5 oct */  .eg1_decay = 5500, /* ~200 ms */
      .rev_amt = 1000,   /* small room */ .rev_dec = 5000, },

    // P2 "DREAM PAD" — slow-blooming saw pad: soft attack, long release, slow LFO
    // breathing on the filter, gentle EG1 bloom, wide 1/4 echo. Hold chords.
    { .magic = PRESET_MAGIC, .wave_id = AMY_ENGINE_WAVE_SAW_DOWN, .flt_type = AMY_ENGINE_FILTER_LPF,
      .fmt = PRESET_FMT,
      .flt_cut = 5500,  /* ~1.4 kHz  */  .flt_res = 2500,  /* Q ~1.4    */
      .env_atk = 8000,  /* ~600 ms   */  .env_dec = 8500,  /* ~1.5 s    */
      .env_sus = 8000,  /* 80 %      */  .env_rel = 8500,  /* ~3 s      */
      .lfo_rt  = 2000,  /* ~0.3 Hz   */  .lfo_dp = 3000,   /* ±0.9 oct  */
      .ech_amt = 3500,  .ech_fb = 5000,  .ech_dly = 4444,  /* 1/4 s    */
      .gld     = 0,
      .eg1_depth = 1000, /* +0.5 oct */  .eg1_decay = 8000, /* ~1 s    */
      .rev_amt = 4500,   /* big hall  */ .rev_dec = 9000, },

    // P3 "KALIMBA" — plucked triangle: instant attack, no sustain, bright filter
    // "tick" from a very short EG1, 1/6 echo for a thumb-piano shimmer.
    { .magic = PRESET_MAGIC, .wave_id = AMY_ENGINE_WAVE_TRIANGLE, .flt_type = AMY_ENGINE_FILTER_LPF,
      .fmt = PRESET_FMT,
      .flt_cut = 7000,  /* ~3.3 kHz  */  .flt_res = 1500,  /* Q ~1.1    */
      .env_atk = 0,     /* 1 ms      */  .env_dec = 7000,  /* ~535 ms   */
      .env_sus = 0,                      .env_rel = 6000,  /* ~550 ms   */
      .lfo_rt  = 4000,  .lfo_dp = 0,
      .ech_amt = 3000,  .ech_fb = 4000,  .ech_dly = 2593,  /* 1/6 s    */
      .gld     = 0,
      .eg1_depth = 4000, /* +2 oct   */  .eg1_decay = 4000, /* ~70 ms  */
      .rev_amt = 2500,   /* room      */ .rev_dec = 6500, },

    // P4 "WOBBLE BASS" — square through a resonant low LPF swept by a ~4 Hz LFO
    // (restarts on every note, so each hit wobbles the same way). Try OCT -1/-2.
    { .magic = PRESET_MAGIC, .wave_id = AMY_ENGINE_WAVE_SQUARE, .flt_type = AMY_ENGINE_FILTER_LPF,
      .fmt = PRESET_FMT,
      .flt_cut = 3000,  /* ~330 Hz   */  .flt_res = 7500,  /* Q ~5.6    */
      .env_atk = 0,     /* 1 ms      */  .env_dec = 5000,
      .env_sus = 10000, /* 100 %     */  .env_rel = 4000,  /* ~145 ms   */
      .lfo_rt  = 7000,  /* ~4 Hz     */  .lfo_dp = 6500,   /* ±1.95 oct */
      .ech_amt = 0,     .ech_fb = 0,     .ech_dly = 4444,
      .gld     = 0,
      .eg1_depth = 0,                    .eg1_decay = 5000,
      .rev_amt = 0,      /* dry bass  */ .rev_dec = PRESET_REV_DEC_DEFAULT, },

    // P5 "WAHWAH" — saw through a band-pass: each note says "wah" (EG1 sweep),
    // fast LFO shimmer on the formant, glides between pads, 1/4 echo.
    { .magic = PRESET_MAGIC, .wave_id = AMY_ENGINE_WAVE_SAW_DOWN, .flt_type = AMY_ENGINE_FILTER_BPF,
      .fmt = PRESET_FMT,
      .flt_cut = 5000,  /* ~1 kHz    */  .flt_res = 5000,  /* Q ~2.8    */
      .env_atk = 3500,  /* ~16 ms    */  .env_dec = 7500,  /* ~750 ms   */
      .env_sus = 7000,  /* 70 %      */  .env_rel = 5500,  /* ~400 ms   */
      .lfo_rt  = 7500,  /* ~5.3 Hz   */  .lfo_dp = 1000,   /* ±0.3 oct  */
      .ech_amt = 3000,  .ech_fb = 4500,  .ech_dly = 4444,  /* 1/4 s    */
      .gld     = 2500,  /* 125 ms     */
      .eg1_depth = 3000, /* +1.5 oct */  .eg1_decay = 6500, /* ~390 ms */
      .rev_amt = 2000,   /* plate-ish */ .rev_dec = 7000, },
};

// P6 factory "Juno Brass" (SAW + warm LPF + filter env). Values are in the old
// v3 curves and migrated, so they match what earlier firmware stored in P6.
static void preset_p6_factory(preset_t *p)
{
    memset(p, 0, sizeof(*p));
    p->rev_dec   = PRESET_REV_DEC_DEFAULT;   // no reverb on P6 (rev_amt 0)
    p->magic     = PRESET_MAGIC;
    p->wave_id   = AMY_ENGINE_WAVE_SAW_DOWN; // SAW — Juno character
    p->flt_type  = AMY_ENGINE_FILTER_LPF;
    p->fmt       = 0;     // values below use the v3 curves → migrated before saving
    p->flt_cut   = 5000;  // ~1.5 kHz — warm Juno tone
    p->flt_res   = 500;   // very slight resonance
    p->env_atk   = 1000;  // ~202 ms — slow enough for pad feel
    p->env_dec   = 2000;  // ~204 ms
    p->env_sus   = 7500;  // 75%
    p->env_rel   = 3000;  // ~1.5 s
    p->lfo_rt    = 2000;
    p->lfo_dp    = 0;
    p->ech_amt   = 2000;  // light echo
    p->ech_fb    = 2000;
    p->gld       = 0;
    p->eg1_depth = 2500;  // +2000 Hz filter sweep on attack
    p->eg1_decay = 1000;  // ~205 ms filter env decay
    p->ech_dly   = 4444;  // 1/4 s delay
    preset_migrate(p);
}

static void preset_seed_factory(void)
{
    nvs_handle_t h;
    uint8_t ver = 0;
    if (nvs_open(PRESET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_u8(h, "fac", &ver);
    nvs_close(h);
    if (ver >= FACTORY_SET_VERSION) return;

    if (ver < 1) {   // first time: write the factory sounds into P1-P5
        for (int i = 0; i < 5; i++) {
            s_presets[i]       = s_factory_presets[i];
            s_preset_valid[i]  = true;
            preset_nvs_save(i);
        }
    } else if (ver < 4) {
        // v4: give untouched factory presets their new reverb settings. A slot is
        // "untouched" when everything before the reverb fields matches the factory
        // preset and its reverb is still the pre-reverb default (off).
        for (int i = 0; i < 5; i++) {
            if (s_preset_valid[i]
                && memcmp(&s_presets[i], &s_factory_presets[i], PRESET_SIZE_V1) == 0
                && s_presets[i].rev_amt == 0
                && s_presets[i].rev_dec == PRESET_REV_DEC_DEFAULT) {
                s_presets[i] = s_factory_presets[i];
                preset_nvs_save(i);
            }
        }
    }
    // Names: only for slots that still hold the untouched factory sound
    for (int i = 0; i < 5; i++) {
        if (s_preset_valid[i] && memcmp(&s_presets[i], &s_factory_presets[i], sizeof(preset_t)) == 0)
            preset_name_set(i, s_factory_names[i]);
    }
    preset_t p6;
    preset_p6_factory(&p6);
    if (s_preset_valid[5] && memcmp(&s_presets[5], &p6, sizeof(preset_t)) == 0)
        preset_name_set(5, "BRASS");
    if (nvs_open(PRESET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "fac", FACTORY_SET_VERSION);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI("ui", "factory preset set applied (v%u)", FACTORY_SET_VERSION);
}

void presets_init(void)
{
    // Pre-load preset validity so the PRST squares in the cards view are correct on first draw
    for (int i = 0; i < PRESET_COUNT; i++)
        preset_nvs_load(i);
    // Factory preset: slot 6 = Juno Brass (SAW + warm LPF + filter env).
    // Re-seed if empty or still contains the old KS preset.
    if (!s_preset_valid[5] || s_presets[5].wave_id == AMY_ENGINE_WAVE_KS) {
        preset_p6_factory(&s_presets[5]);
        s_preset_valid[5] = true;
        preset_nvs_save(5);
        preset_name_set(5, "BRASS");
    }
    preset_seed_factory();
}
