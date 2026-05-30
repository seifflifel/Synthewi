# Synthewi — Hardware & UI Upgrade Plan

This document is the reference for the next build phase.  
Read it before touching any file. Update it when decisions change.

---

## Phase 1 — Hardware additions

### 1A. 3-position lever switch (waveform selector)

**Why:** Remove the WAVE section from the UI entirely. Physical lever = instant tactile recall of waveform, no menu navigation needed.

**Switch type:** 3-position lever / toggle with 6 pins — this is two SPDT switches ganged on one lever (2-pole 3-position). Only 2 GPIOs needed.

**Wiring:**

```
Lever pin layout (viewed from below):
   [A1] [C1] [B1]
   [A2] [C2] [B2]

C1, C2 → GND
A1 → GPIO_WAVE0  (with internal pull-up)
B1 → GPIO_WAVE1  (with internal pull-up)
A2, B2 → not connected (only one pole needed)

Position 1 (left)   → WAVE0=LOW,  WAVE1=HIGH  → SQUARE
Position 2 (center) → WAVE0=HIGH, WAVE1=HIGH  → SAW
Position 3 (right)  → WAVE0=HIGH, WAVE1=LOW   → TRIANGLE
```

**Suggested GPIOs:** if 41 42 can be used 

**Code changes needed:**
- `main.c`: configure GPIO 41 + 42 as INPUT with PULLUP, poll every main loop tick (20 ms), decode 2-bit state → wave_id, call `amy_engine_set_wave()` when state changes
- `ui.h`: remove entire `UI_WAVEFORM` section and its menu entry, remove `s_wave_cur` state
- The menu grid becomes 3 items (FX, ADSR, CALIB) — rethink layout or keep 2×2 with one slot repurposed
- Ribbon bar (new UI) shows the waveform symbol directly — lever change reflects live

**Waveform symbol drawing (ribbon bar, ~18×10 px):**

Draw pixel-art glyphs directly with `tft_fill` line segments:

```
SQUARE:  _|‾‾|_   (flat, up, flat, down, flat)
SAW:     /|  /|   (diagonal rise, instant drop)
TRIANGLE: /\  /\  (rise, fall, rise)
```

---

### 1B. Octave shift buttons

**GPIOs:** 9 (OCT DOWN, touch channel 9) and 10 (OCT UP, touch channel 10) — touch pads, same logic as note pads, added to CALIB as "OCT-" and "OCT+".

**Behavior (implemented):**
- Range: −2 to +2 (clamped, no wrap)
- Each press shifts all 8 MIDI notes in `s_notes[]` by ±12 via `apply_octave()`
- 500ms cooldown after each press prevents accidental double-trigger
- Menu cell 0 shows `OCT: 0` / `OCT:+1` / `OCT:-2` in cyan (ribbon bar placement deferred to Phase 3)
- No NVS persistence (resets to 0 on power cycle)


---

### 1C. Potentiometers via MUX (planned — do not implement until MUX is purchased)
**before you start go check how spark synth handle potentiometers and their states and then plan how to do it base on the notes under**

**Rationale:** 6 hardware pots for the most-touched params. Encoder + UI remain for filter type and presets.

**MUX chip:** CD4051 (8:1 analog multiplexer) — 1 output pin, 3 address select pins, works at 3.3V.

**Wiring:**

```
MUX output (pin 3) → ESP32-S3 ADC1 channel (e.g. GPIO 4)
MUX select A (pin 11) → GPIO_MUX_A (e.g. GPIO 5)
MUX select B (pin 10) → GPIO_MUX_B (e.g. GPIO 6)
MUX select C (pin 9)  → GPIO_MUX_C (e.g. GPIO 7)
MUX VCC (pin 16) → 3.3V
MUX GND (pin 8, 7) → GND
MUX INH (pin 6) → GND (always enabled)
```

**Channel assignment:**

| MUX CH | Select (CBA) | Parameter | Range |
|--------|--------------|-----------|-------|
| 0 | 000 | Attack | 0–10000 |
| 1 | 001 | Release | 0–10000 |
| 2 | 010 | Filter Cutoff | 0–10000 |
| 3 | 011 | Filter Q / Resonance | 0–10000 |
| 4 | 100 | Glide | 0–10000 |
| 5 | 101 | Echo (amt + fb linked) | 0–10000 → both `echo_amount` and `echo_feedback` |
| 6 | 110 | spare | — |
| 7 | 111 | spare | — |

**Important** keep the same range of the parameters that we have now and jast map the 10k pots to those ranges

**Echo dual-control:** `echo_amount = val`, `echo_feedback = val `

**ADC scanning strategy:**
- Read one channel per main loop tick (20 ms) — full scan every 160 ms (6 channels × 20 ms + MUX settle time)
- Apply a deadband (±50 raw counts) to avoid noise-driven updates — only call `amy_engine_set_*` when value moves outside deadband
- No NVS save from pot changes (pots are live, NVS only from section exit as today)

**Code changes needed when implementing:**
- `main.c`: `adc1_config`, MUX select GPIO config, round-robin channel scan, deadband filter, route readings to `amy_engine_set_*`
- `ui.h` MAIN screen: show pot values live (override encoder-set values visually)
- Params controlled by pots migrate out of FX section (FX section keeps only filter TYPE selection)

we will keep the encoder changing the params but when you touch the potentiometer it goes back to the potentiometer value so we have to be smart about it
---

## Spark Synth UI — Key Patterns to Reuse

Reference: `third_party/spark-synth/src/` — JunoUI.cpp, DisplayManager.h, Instrument.h

### Header bar pattern

```cpp
// Spark header: OCT left | INSTRUMENT NAME center (bold) | battery right
// Separator: drawHLine(0, 10, 128)
// Content area starts at y=12
```

We adapt this as:  
`[● ● ● ● ● ● ● ●]  [~wave~]  [OCT:+1]`  
`────────────────────────────────────`

### Smart redraw flag

Spark uses `needsUIRedraw` (one-shot) and `liveUI` (persistent live update) flags. We should adopt the same pattern:
- `s_dirty = true` → one-shot, clears after draw (already done)
- Add a `s_live_ui` flag for calibration screen (high refresh rate) — already partially done via timed refresh

### Slider + dual-triangle pattern (Spark JunoUI)

Spark draws a vertical track with 4 horizontal tick marks, then two small triangles (left-side and right-side) pointing inward to show two parameter positions on the same track.

```
         |
    ─────┼───── ← tick (wide = top)
         |
      ───┼───     ← tick
         |
      ───┼───     ← tick  ← ▷ F (freq triangle, left side)
         |
    ─────┼───── ← tick (wide = bottom) ← ▷ R (res triangle, right side)
```

Replicate in C using `tft_fill()` for vertical/horizontal lines. Use this for the MAIN screen's filter and LFO columns.

### Envelope drawing (Spark JunoUI)

```
Attack:  line from (0, bottom) to (attackWidth, top)
Decay:   line from (attackWidth, top) to (sustainX, sustainY)
Sustain: horizontal line from (sustainX, sustainY) to (releaseX, sustainY)
Release: line from (releaseX, sustainY) to (end, bottom)
```

Map our 0–10000 values to pixel widths. Use this for the ENV column in MAIN screen.

### DCO waveform glyph (Spark JunoUI)

Spark draws a pulse-width preview in a small box. Adapt:
- SQUARE: draw a centered pulse glyph (H lines + V lines)
- SAW: draw a rising diagonal + instant drop vertical
- TRIANGLE: draw up-slope + down-slope

These glyphs go in the ribbon bar (center slot, ~18×10 px).

---

## UI Redesign — New Layout

**Screen:** ST7735 128×160 px (portrait), RGB565, our custom SPI driver.

### Ribbon bar (top, 0–19 px)

```
[● ● ● ● ● ● ● ●]   [~~~]   OCT:+1
 0                63   80      112
```

- **Pad circles (x=2..62, y=8):** 8 circles, 5px radius, spaced 8px apart. Filled = pad active, outline = idle.
- **Waveform glyph (x=68..88, y=4..16):** 20×12 px pixel-art of SAW / SQR / TRI. Redrawn when lever changes.
- **Octave label (x=92..126, y=13):** `"OCT:+1"` at scale-1 font. Redrawn when octave buttons pressed.

### Separator line (y=20)

`tft_fill(0, 20, 127, 21, C_DKGRAY)`

### Cards area 

3 equal cards side by side with rounded edges

rough sketch
```
┌──────────┬ ┬─────────┬ ┬────────────────┐  
│  MAIN    │ │ PRESETS │ │   CALIBRATION  │ 
├──────────┤ ├─────────┤ ┼────────────────┤ 
```

Selected card has a yellow header background. Short press → enter. Long press → back (from inside) 


---

### Card 1 — MAIN screen (parameter display)

Columns layout (inspired directly by Spark Juno UI columns):
cards side by side with rounded edges

this is roughly what it will be
```
┌──────┬────       ┬──────┬──── ┬──────┐
│ ENV  │FILTER     │ LFO  │ECHO │   │
│      │   type    │      │     │      │
│ ADSR │CUT        │ R  D │AMT  │ GLIDE  │
│ line │ Q         │      │     │      │
└──────┴────       ┴──────┴──── ┴──────┘
```

- **ENV column:** Spark-style ADSR line drawing. Values come from engine state (or pots when connected).
- **FLT column:** dual slider for CUT (F triangle) and Q (R triangle) on one vertical track.
- **LFO column:** dual slider for Rate (R) and Depth (D).
- **ECH column:** single slider for echo amount.
- **PAD column:** glide value as a single slider.

Filter TYPE shown as small label under FLT header (`LPF` / `BPF` / `HPF`).

When pot migration happens: pot-driven params show a filled dot `●` next to value to indicate hardware control; encoder-editable params show `>` cursor as today.
but before it hapens you can short press to enter the field of the card (dont change ui just make the changeable param another color) and you can change it and then long press to nvs and go out so you can pass to another param like echo or glide
---

### Card 2 — PRESETS
cards side by side with rounded edges

```
1 2 3 
4 5 6

```

Cursor navigates 3×2 grid (encoder rotate). Short press two times = **load** preset. Long press on a slot = **save current params** into that slot . Slots stored in NVS as `preset_N_*` keys.

---

### Card 3 — CALIBRATION

Entry view: 8 vertical bar columns (one per pad) . with rounded edges if possible
+ two circles for octv + and -
Each bar: A small horizontal line marks the threshold level.

**Interaction:**
- Encoder rotate: highlight a pad column
- Short press → enter single-pad edit (full screen)
- Inside edit: same as current CALIB section — rotate adjusts threshold, short press saves, long press back
- **Live refresh rate in CALIB: 50 ms** (was 120 ms) — call `ui_redraw()` every other main loop tick when `s_sec == UI_CALIB`

---

## Looper (after hardware phase)

- ~1.74 MB PSRAM free (~9.9 s mono at 44100 Hz)
- Buffer: `float* loop_buf` allocated via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
- States: IDLE → ARM → RECORD → PLAY → OVERDUB → STOP
- Trigger: dedicated button (long-press on OCT DOWN for now, migrate to hardware button later)
- Record: intercept `amy_engine_note_on/off` calls, timestamp them into an event list
- Playback: replay event list via a FreeRTOS timer task on Core 1
- Display: loop status shown in ribbon bar right side when active (`LOOP REC`, `LOOP PLAY`)

---

## Implementation order

1. [x] 3-position lever + code (remove WAVE UI section, waveform glyph in menu cell 0)
2. [x] Octave touch buttons (GPIO 9/10) + OCT display in menu cell 0
3. [ ] New UI layout: ribbon bar + 3-card grid shell (navigation only, no inner content)
4. [ ] MAIN screen content (Spark-style columns)
5. [ ] PRESETS card (NVS save/load)
6. [ ] CALIB card redesign (8 columns + high refresh)
7. [ ] **Future — encoder-selectable octave mode (low priority, implement after Phase 3):**

Two modes selectable via encoder in a settings menu or long-press shortcut:
- **Latching** (current): press oct+/oct- → shift stays until pressed again
- **Momentary**: press and hold oct+ → shift up while held, release → return to original octave

Add a toggle in the UI so the user can choose which behavior they prefer. This adds flexibility for performance use (momentary) vs. composition use (latching).

8. [ ] Looper
9. [ ] MUX + pots (after hardware purchase)

