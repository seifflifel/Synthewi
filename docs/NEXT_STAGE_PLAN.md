# Synthewi — Next Stage Implementation Plan

Session: 2026-05-25. Starting from commit `27e7c6c` (working prototype: filter, filter env, LFO, envelope).

---

## Goal

Two synthesis modes on 8 touch pads, pentatonic scale, fully controllable from a redesigned web UI.

| Mode | Description |
|------|-------------|
| **CUSTOM** | Our manual engine: oscillator choice, filter, LFO, filter envelope, pressure expressiveness |
| **PATCH** | AMY built-in patches — Juno (0–127), DX7 (128–255), Piano (256). Touch triggers notes. No filter/pressure override. |

---

## Architecture Decisions (Settled)

- **1 audio osc + 1 LFO osc per pad** — correct for this instrument. Each pad is a fixed zone, 1:1 ratio, no voice stealing needed. LFO oscs are SINE running continuously once started. With 8 pads: `max_oscs = 16`.
- **Pentatonic scale**: pads 0–7 → MIDI notes 60, 62, 64, 67, 69, 72, 74, 76 (C D E G A C D E across two octaves).
- **Pressure**: computed on the ESP using `abs_delta` and `threshold` (both already available in the touch ISR). Two UI sliders: **depth** (Hz, gain) + **range** (× threshold, ceiling). No separate calibration ritual — user adjusts both per material session alongside the threshold sliders.
- **PATCH mode filter**: no filter cutoff, no pressure expressiveness in patch mode. Patch plays as-is.

---

## Phase 1 — Expand to 8 Pads

**Why first**: everything else depends on the correct pad count.

### ESP (`amy_engine.h`, `amy_engine.c`, `touch_telemetry.h`, `touch_telemetry.c`, `main.c`)

- `SYNTH_PAD_COUNT` 4 → 8
- `CHANNELS` 4 → 8
- `cfg.max_oscs` = 16 (8 audio + 8 LFO, indices 0–7 audio, 8–15 LFO)
- Pad→MIDI note table: `{60, 62, 64, 67, 69, 72, 74, 76}` — C pentatonic ascending two octaves
- `s_pad_active[8]`, `s_lfo_started[8]`, `s_pan_pos[8]` = `{0.1, 0.2, 0.35, 0.45, 0.55, 0.65, 0.8, 0.9}` (spread L→R)
- `main.c` note_on: `midi_note = s_pad_notes[pad]` (remove hardcoded `60 + pad * 2`)
- Telemetry: `CHANNELS=8` — UDP packet grows by 4× additional channel structs; `touch_telemetry_synth_t` unchanged

### Bridge (`bridge/src/index.ts`)

- `CHANNELS` 4 → 8
- Parse loop runs 8 channels

### Web UI (`web-ui/src/ui/app.ts`)

- Touch card loop runs 8 iterations
- Layout: 2×4 grid (4 cards per row)

### Test checkpoint
Flash, open web UI: 8 touch cards visible, all 8 pads trigger notes, telemetry updates for all channels.

---

## Phase 2 — Pressure Expressiveness (CUSTOM mode only)

### How pressure is computed

The touch ISR already has `abs_delta` and `threshold` per channel every tick. We compute:

```c
// In touch_telemetry.c, sent continuously while pad is active:
float pressure_norm = (float)(abs_delta - threshold) / (threshold * pressure_range);
pressure_norm = clamp(pressure_norm, 0.0f, 1.0f);
// pressure_norm → passed to amy_engine_set_pressure(pad, pressure_norm)
```

`pressure_range` (default 2.0) is a runtime parameter sent from UI → bridge → ESP as `SYNTH_PARAM_PRESSURE_RANGE`. It is the ceiling multiplier: range=2 means full press = 2× threshold delta. User adjusts per material.

### How it routes to the filter

```c
// amy_engine.c — called from touch task while pad is active
void amy_engine_set_pressure(uint8_t pad, float pressure_0to1) {
    // takes render lock, fires update to active osc filter freq
    float new_cutoff = sc_filter_hz(s_filter_cutoff)
                     + sc_pressure_depth(s_pressure_depth) * pressure_0to1;
    // amy_add_event updates filter_freq_coefs[COEF_CONST] for osc=pad
}
```

### New parameters

| Parameter | ID | Scale | Description |
|-----------|-----|-------|-------------|
| `touch.pressure.depth` | 15 | 10000 | Hz the filter opens at max press (0–8000 Hz) |
| `touch.pressure.range` | 16 | 100 | Ceiling multiplier ×threshold (1.0–5.0, sent as ×100 int) |

### UI

- Filter card: two new sliders below resonance: **pressure depth** (0–8000 Hz) + **pressure range** (1×–5× threshold)
- Pressure range is a one-time-per-material setting, same workflow as threshold tuning

### Files touched
`touch_telemetry.h` (2 new SYNTH_PARAMs), `touch_telemetry.c` (call pressure update in active-pad tick), `amy_engine.h` (new API + state), `amy_engine.c` (pressure state, setter, NVS key), bridge (2 new PATH_TO_CMD entries), web UI (2 new sliders)

---

## Phase 3 — Portamento / Glide (CUSTOM mode)

One parameter, large musical impact. When a pad is re-triggered, AMY slides pitch smoothly.

```c
// note_on in amy_engine.c:
e.portamento = sc_portamento_ms(s_portamento); // 0 = instant
```

Scaling: 0–10000 → 0–500 ms, exponential curve (small values feel musical, large values are dramatic).

### New parameter

| Parameter | ID | Scale | Description |
|-----------|-----|-------|-------------|
| `synth.glide` | 17 | 10000 | Portamento time 0–500 ms |

### UI

Single slider "glide" in the oscillator/synth section.

### Files touched
`touch_telemetry.h` (1 new SYNTH_PARAM), `amy_engine.h/.c`, bridge, web UI

---

## Phase 4 — PATCH Mode (Juno / DX7 / Piano)

### Architecture

New enum in `amy_engine.h`:
```c
typedef enum {
    SYNTH_MODE_CUSTOM = 0,
    SYNTH_MODE_JUNO   = 1,  // patch_number 0–127
    SYNTH_MODE_DX7    = 2,  // patch_number 128–255
    SYNTH_MODE_PIANO  = 3,  // patch_number 256
} synth_mode_t;
```

On mode switch or patch change:
1. `amy_reset()` — clears all oscillator state
2. Re-apply global config (max_oscs, no reverb/echo/chorus — same constraints)
3. In PATCH mode, `note_on` fires: `e.patch_number = patch_number; e.midi_note = s_pad_notes[pad]; e.velocity = 1.0`
4. AMY's internal voice manager handles oscillator allocation

In PATCH mode: no filter override, no pressure, no LFO from our engine. Patch plays exactly as AMY defines it.

Switching back to CUSTOM mode: `amy_reset()` + restore oscillator state from our static vars.

### New parameters

| Parameter | ID | Scale | Description |
|-----------|-----|-------|-------------|
| `synth.mode` | 18 | 1 | 0=CUSTOM 1=JUNO 2=DX7 3=PIANO |
| `synth.patch` | 19 | 1 | Patch number 0–256 |

### UI (PATCH mode section, replaces the synth/fx/envelope cards)

```
[ CUSTOM | JUNO | DX7 | PIANO ]    ← mode selector tabs

PATCH mode shows:
  ◀ patch 042 ▶     "STRINGS 1"   ← prev/next buttons + patch number display
  (no other controls — patch plays as-is)
```

CUSTOM mode shows all existing controls (oscillator, filter, filter env, LFO, envelope, pressure).

### Files touched
`amy_engine.h` (mode enum, patch_number state, new API), `amy_engine.c` (mode switch, patch note_on/off), `touch_telemetry.h` (2 new SYNTH_PARAMs), `touch_telemetry.c`, bridge, web UI (mode tabs, patch browser)

---

## Phase 5 — UI Overhaul

### Layout

```
┌──────────────────────────────────────────────────────┐
│  Synthewi · web ui          [live]   [ws://...]  [⏎] │
├──────────────────────────────────────────────────────┤
│  [ CUSTOM | JUNO | DX7 | PIANO ]                     │
├──────────────────────────────────────────────────────┤
│  CUSTOM mode:                                        │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────┐  │
│  │ OSC      │ │ FILTER   │ │ MOD      │ │ ENV    │  │
│  │ wave btns│ │ type btns│ │ lfo rate │ │ attack │  │
│  │ glide    │ │ cutoff   │ │ lfo depth│ │ release│  │
│  │          │ │ resonance│ │ fenv dep │  │        │  │
│  │          │ │ pres dep │ │ fenv dec │  │        │  │
│  │          │ │ pres rng │ │          │  │        │  │
│  └──────────┘ └──────────┘ └──────────┘ └────────┘  │
│                                                      │
│  PATCH mode:                                         │
│  ┌──────────────────────────────────────────────┐    │
│  │  ◀  patch 042  ▶     "STRINGS 1"             │    │
│  └──────────────────────────────────────────────┘    │
├──────────────────────────────────────────────────────┤
│  touch pads                                          │
│  ┌────┐┌────┐┌────┐┌────┐ ┌────┐┌────┐┌────┐┌────┐  │
│  │ C4 ││ D4 ││ E4 ││ G4 │ │ A4 ││ C5 ││ D5 ││ E5 │  │
│  └────┘└────┘└────┘└────┘ └────┘└────┘└────┘└────┘  │
│  (threshold sliders below each pad card)             │
└──────────────────────────────────────────────────────┘
```

- Pad cards show note name, live touch badge, threshold slider — same as now but ×8
- CUSTOM controls split into 4 column cards (OSC / FILTER / MOD / ENV)
- Mode tabs at top switch between CUSTOM and PATCH views

---

## Execution Order

| # | Phase | Deliverable | Test |
|---|-------|-------------|------|
| 1 | 8 pads | ESP + bridge + web UI | All 8 pads trigger, telemetry shows 8 channels |
| 2 | Pressure | ESP pressure→filter + 2 UI sliders | Press hard on active pad → filter opens |
| 3 | Glide | 1 param in ESP + UI slider | Re-trigger pad → pitch slides |
| 4 | PATCH mode | Mode enum + note_on path + UI tabs + patch browser | Juno/DX7 patches play on touch |
| 5 | UI overhaul | Full app.ts restructure | All controls reachable, 8 pads displayed |

Phases 1–3 are pure extensions of the current engine — no architectural changes.
Phase 4 introduces the mode system — this is the one to review carefully on hardware.
Phase 5 is frontend-only — no firmware changes.

---

## Parameter Table (full, after all phases)

| UI path | Param ID | Scale | Added in |
|---------|----------|-------|----------|
| `synth.wave` | 0 | 1 | existing |
| `fx.reverb.amount` | 1 | — | removed |
| `fx.reverb.decay` | 2 | — | removed |
| `fx.echo.amount` | 3 | — | removed |
| `fx.echo.feedback` | 4 | — | removed |
| `fx.filter.cutoff` | 5 | 10000 | existing |
| `fx.filter.resonance` | 6 | 10000 | existing |
| `env.attack` | 7 | 10000 | existing |
| `env.release` | 8 | 10000 | existing |
| `fx.filter.type` | 9 | 1 | existing |
| `fx.filter.env.depth` | 10 | 10000 | existing |
| `fx.filter.env.decay` | 11 | 10000 | existing |
| `fx.lfo.rate` | 12 | 10000 | existing |
| `fx.lfo.depth` | 13 | 10000 | existing |
| `fx.chorus` | 14 | — | removed |
| `touch.pressure.depth` | 15 | 10000 | Phase 2 |
| `touch.pressure.range` | 16 | 100 | Phase 2 |
| `synth.glide` | 17 | 10000 | Phase 3 |
| `synth.mode` | 18 | 1 | Phase 4 |
| `synth.patch` | 19 | 1 | Phase 4 |

---

## Known Constraints (carry-forward)

- **Reverb/echo**: disabled — delay lines cannot allocate after WiFi fragments heap
- **KS wave**: disabled — `ks_oscs=0`, delay lines exhaust heap at 8 pads
- **Chorus**: disabled — `features.chorus=0`, same heap reason
- **USB mono**: AMY renders stereo, only left channel used; pan is set but inaudible over USB
- **Patch mode + AMY voice manager**: `amy_reset()` on mode switch clears all oscillator state — any currently-playing notes will cut off
