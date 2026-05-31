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

### 1C. Potentiometers via MUX (wired — implement when ready)

**Rationale:** 6 hardware pots for the most-touched params. Encoder + UI remain for filter type and presets.

**MUX chip:** CD4067BE (16-channel analog multiplexer, DIP-24). Only channels 0–5 are used; select line D is hardwired to GND so the upper 8 channels are never addressed, saving one GPIO.

---

#### CD4067BE wiring (DIP-24)

```
CD4067BE pin 24 (VDD)       → 3.3V
CD4067BE pin 12 (VSS)       → GND
CD4067BE pin 19 (E, enable) → GND  ← always enabled, active LOW
CD4067BE pin 23 (D, bit 3)  → GND  ← tie low; channels 0-5 never need D=1
CD4067BE pin 20 (A, bit 0)  → GPIO 12
CD4067BE pin 21 (B, bit 1)  → GPIO 13
CD4067BE pin 22 (C, bit 2)  → GPIO 14
CD4067BE pin 18 (Z, common) → GPIO 11  ← ADC2 CH0, analog read
```

Bypass cap: 100 nF ceramic between VDD (pin 24) and GND (pin 12), placed close to chip.

---

#### Potentiometer channel assignment

| CH | Addr C,B,A | Pin (DIP-24) | Parameter | Internal range |
|----|-----------|--------------|-----------|---------------|
| C0 | 0,0,0 | Pin 7 | Attack | 0–10000 → 2–2000 ms |
| C1 | 0,0,1 | Pin 6 | Release | 0–10000 → 10–5000 ms |
| C2 | 0,1,0 | Pin 5 | Filter Cutoff | 0–10000 → 13–12000 Hz (Spark exp.) |
| C3 | 0,1,1 | Pin 4 | Resonance (Q) | 0–10000 → Q 0.7–11 (Spark exp.) |
| C4 | 1,0,0 | Pin 3 | Glide | 0–10000 → 0–500 ms |
| C5 | 1,0,1 | Pin 2 | Echo | 0–10000 → both `echo_amount` and `echo_feedback` linked |
| C6–C15 | — | Pins 1, 8–17 | Not used | Leave unconnected |

---

#### Each potentiometer (10 kΩ linear)

```
Pot end 1 (CCW) → GND
Pot wiper       → MUX channel pin (C0..C5)
Pot end 2 (CW)  → 3.3V
```

Full CCW = 0 (GND), full CW = 4095 ADC (3.3V) → maps to 0–10000 internal scale.

---

#### New GPIO allocations

| GPIO | Function |
|------|----------|
| 11 | MUX Z (ADC2 CH0 — analog in) |
| 12 | MUX select A (bit 0) |
| 13 | MUX select B (bit 1) |
| 14 | MUX select C (bit 2) |

---

#### Scanning strategy (for implementation)

- Read one channel per main loop tick (20 ms) — full 6-channel scan every 120 ms
- Allow ~10 µs after asserting select pins before triggering ADC read (MUX settle time)
- Deadband ±50 raw counts to suppress ADC noise — only call `amy_engine_set_*` on change outside deadband
- Encoder still works; when pot moves outside deadband it takes over; encoder adjusts from the new pot position
- No NVS save from pot changes — pots are live; NVS save only on explicit long-press exit as today

---

#### Code changes needed (when implementing)

- `main.c`: ADC2 CH0 config, select GPIO config (digital out), round-robin channel scan, deadband filter, `amy_engine_set_*` calls
- `ui.h` MAIN screen: pot-driven params show a `●` dot next to value instead of `>` cursor
- Params controlled by pots migrate out of encoder-edit path (encoder stays as fine-tune override)
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
3. [x] New UI layout: ribbon bar + 3-card grid (landscape 160×128, ribbon y=0..21, cards y=22..127)
4. [x] MAIN screen content (Spark-style 5 columns: ENV / FLT / LFO / ECH / GLD)
5. [x] PRESETS card (6 NVS slots, 2×3 grid, LOAD / SAVE / CLR actions)
6. [ ] CALIB card redesign (8 pad columns + 2 oct columns, 50 ms refresh)
7. [ ] Looper (PSRAM event-based record/playback)
8. [ ] MUX + pots (CD4067BE wired — see section 1C)

