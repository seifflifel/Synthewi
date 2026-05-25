# Synthewi — Implemented Sound Engine Features

Session: 2026-05-25. Reference baseline: `checkpoint/usb-audio` branch.

---

## What was added

### Waveforms: NOISE and KS

- **NOISE** (wave_id=5): white noise. Uses EG0 for amplitude shaping. Good for percussion and
  textural pads when combined with filter resonance.
- **KS** (wave_id=6): Karplus-Strong physical string model. `env.release` slider is repurposed
  as **string decay** — low values = short pluck, high values = long sustained string. Frequency
  sets the pitch of the plucked string. EG0 still gates the output.
- Web UI: wave buttons now show all 7 choices: sine, pulse, saw↓, saw↑, tri, noise, ks.

### Filter Type Selector

- LPF / BPF / HPF selectable per session.
- Sent as a direct integer (0/1/2); mapped to AMY `FILTER_LPF/BPF/HPF` internally.
- Applied live to all currently-playing oscillators on change.
- Persisted in NVS as `flt_typ`.
- Web UI: three toggle buttons in the filter card (same row style as wave buttons).

### Filter Envelope (EG1 → filter cutoff)

- Two new parameters: **depth** (0–8000 Hz above cutoff) and **decay** (5–2000 ms).
- On each note-on: EG1 rises to full depth in 5 ms, then decays to 0 over `decay` ms.
- The filter hears `base_cutoff + depth * EG1 + lfo_depth * LFO`.  When depth=0 there is
  no envelope effect (COEF_EG1=0).
- Not applied live to playing notes (would retrigger the envelope mid-note).
- Persisted: `flt_envd`, `flt_envc`.
- Web UI: "filter env" card with depth and decay sliders.

### LFO → filter (per-pad SINE oscillators)

- Each pad has a dedicated SINE LFO oscillator (indices 4–7) running continuously once the
  first note fires on that pad.
- **Rate** (0.1–10 Hz) and **depth** (0–5000 Hz filter swing).
- The LFO is wired via AMY's `mod_source` + `filter_freq_coefs[COEF_MOD]` mechanism.
- Rate changes are applied live to running LFO oscillators. Depth changes are applied live
  to active audio oscillators.
- Persisted: `lfo_rt`, `lfo_dp`.
- Web UI: "lfo → filter" card with rate and depth sliders.

### Chorus

- AMY's built-in chorus (0.5 Hz LFO, 320-sample max delay, 50% depth, level 0–1).
- Chorus buffer is allocated at startup (`features.chorus=1`); level is driven by
  `config_chorus()` so it can be set to 0 without heap issues.
- Persisted: `chorus`.
- Web UI: "chorus" card with a single mix slider (0–100%).

### Per-pad Stereo Pan

- Four pads are spread across the stereo field: 0.2 / 0.4 / 0.6 / 0.8 (L→R).
- Applied on every note-on via `pan_coefs[COEF_CONST]`.
- No UI control (fixed spread); AMY renders stereo, USB audio takes left channel only.

---

## Protocol changes

| Version | Synth state bytes | New fields |
|---------|-------------------|------------|
| v2      | 18                | — (baseline) |
| **v3**  | **28**            | filter_type (+1), filter_env_depth (+2), filter_env_decay (+2), lfo_rate (+2), lfo_depth (+2), chorus_amount (+2) |

The command packet format is unchanged. The ESP command handler now accepts any non-zero
protocol version so old bridge builds still send threshold commands.

---

## New bridge paths

| UI path | Param ID | Scale |
|---------|----------|-------|
| `fx.filter.type` | 9 | 1 (integer 0/1/2) |
| `fx.filter.env.depth` | 10 | 10000 |
| `fx.filter.env.decay` | 11 | 10000 |
| `fx.lfo.rate` | 12 | 10000 |
| `fx.lfo.depth` | 13 | 10000 |
| `fx.chorus` | 14 | 10000 |

---

## Files changed

| File | Change |
|------|--------|
| `components/amy_engine/include/amy_engine.h` | New wave/filter constants, 6 new state fields, 4 new API functions |
| `components/amy_engine/amy_engine.c` | Full rewrite: LFO oscs, KS feedback, filter env (EG1), pan, chorus, extended NVS |
| `main/touch_telemetry.h` | SYNTH_PARAM_* 9–14 |
| `main/touch_telemetry.c` | Proto v3, synth struct 18→28 bytes, fill new fields |
| `main/main.c` | Wire params 9–14 in on_synth_param |
| `bridge/src/index.ts` | Proto v3, 6 new PATH_TO_CMD entries, parse 28-byte synth struct |
| `bridge/dist/index.js` | Rebuilt |
| `web-ui/src/proto/types.ts` | SynthState extended |
| `web-ui/src/ui/app.ts` | Filter type buttons, filter env card, LFO card, chorus slider, NOISE/KS waves, applySynthState updated |
| `web-ui/dist/` | Rebuilt |

---

## Known constraints

- **KS and `ks_oscs`**: AMY requires `ks_oscs ≥ 1` at startup to pre-allocate Karplus-Strong
  buffers. Set to `SYNTH_PAD_COUNT=4`. If you try KS with `ks_oscs=0` the wave renders silence.
- **Filter env not live**: depth/decay take effect on the next note-on, not on currently-ringing
  notes, to avoid mid-note envelope retriggers.
- **LFO starts on first note-on per pad**: the LFO SINE oscillator is started once and runs
  continuously. Phase resets are avoided after the first note.
- **Chorus buffer**: allocated at `amy_start` (not lazily). Always costs ~320 samples of SRAM
  even when amount=0.
- **USB audio is mono**: AMY renders 2-channel stereo but `amy_engine_render_mono_16` takes
  only the left channel (`[frame * AMY_NCHANS]`). Pan has no audible effect over USB.
  Pan will matter when I2S stereo output is added.
