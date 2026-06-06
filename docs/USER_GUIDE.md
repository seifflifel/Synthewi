# Synthewi — User Guide

Synthewi is a capacitive-touch synthesizer built on the ESP32-S3. Eight touch pads play notes, a physical lever selects the waveform, a rotary encoder navigates the UI, and a built-in looper lets you record and layer phrases in real time.

---

## Table of Contents

1. [Interface Overview](#interface-overview)
2. [Playing Notes](#playing-notes)
3. [Waveform Selection](#waveform-selection)
4. [Octave Shift](#octave-shift)
5. [Sound Editor — MAIN](#sound-editor--main)
6. [Presets — PRST](#presets--prst)
7. [Looper](#looper)
8. [Calibration — CAL](#calibration--cal)
9. [Quick Reference](#quick-reference)

---

## Interface Overview

The screen is divided into two permanent zones:

### Ribbon Bar (top strip)

> ![Ribbon bar overview](images/ribbon_bar.jpg)

| Element | Position | Meaning |
|---------|----------|---------|
| 8 circles | Left | One per touch pad — **green** = pad held, outline = idle |
| Waveform glyph | Center | Current waveform (square / saw / triangle) drawn in orange |
| OCT label | Right-center | Current octave offset (`OCT 0`, `OCT+1`, `OCT-2`, …) |
| Looper dot | Top-right corner | **Red** = recording, **Green** = playing, **Yellow** = overdub, invisible = idle |

### Cards Area (main body)

Three cards sit side by side. The selected card is outlined in orange with a bold title. Rotate the encoder to move between cards; short-press to enter.

> ![Cards view](images/cards_view.jpg)

| Card | Preview shown | What it opens |
|------|--------------|---------------|
| **MAIN** | Live ADSR envelope shape | Sound parameter editor |
| **PRST** | 6 small squares (orange = slot occupied) | Preset manager |
| **CAL** | 10 threshold-level bars | Touch calibration |

---

## Playing Notes

Touch any of the eight capacitive pads to play a note. Notes are **sustained** for as long as you hold the pad — lift your finger to release.

> ![Touch pads](images/touch_pads.jpg)

The default scale is **C pentatonic** across two octaves:

| Pad | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|-----|---|---|---|---|---|---|---|---|
| Note | C4 | D4 | E4 | G4 | A4 | C5 | D5 | E5 |

Multiple pads can be held simultaneously for chords.

---

## Waveform Selection

The three-position lever on the left side of the device selects the oscillator waveform instantly — no menu required.

> ![Waveform lever](images/waveform_lever.jpg)

| Lever position | Waveform | Glyph |
|---------------|----------|-------|
| Left | Square | `⌐¬` |
| Center | Sawtooth | `/|` |
| Right | Triangle | `/\` |

The ribbon bar glyph updates immediately when you move the lever.

---

## Octave Shift

Two dedicated touch buttons shift all eight pads up or down by one octave per press.

> ![Octave buttons](images/octave_buttons.jpg)

- **OCT+** button — shifts up one octave (max +2)
- **OCT−** button — shifts down one octave (max −2)

The `OCT` label in the ribbon bar turns yellow when shifted away from zero. A 500 ms cooldown prevents accidental double-triggers. Octave resets to 0 on power cycle.

---

## Sound Editor — MAIN

Enter by selecting the **MAIN** card and pressing the encoder.

> ![MAIN view overview](images/main_view.jpg)

The editor shows five columns. Rotate the encoder to navigate between columns (the selected column glows orange with an underline). Press the encoder to start editing the selected column.

### Navigating parameters

| Action | Result |
|--------|--------|
| Rotate | Move between columns (browse) / adjust value (edit) |
| Short press | Enter edit mode / cycle to next sub-parameter |
| Long press | Save to NVS and return to cards |

Each sub-parameter is highlighted in yellow while active. Unselected parameters in an active column are dimmed.

---

### ENV — Envelope

> ![ENV column](images/main_env.jpg)

Controls the amplitude shape of each note. Drawn as an ADSR line directly in the column.

| Sub-param | Symbol | Range | Description |
|-----------|--------|-------|-------------|
| Attack | A | 2 – 2000 ms | Time to reach full volume after touch |
| Decay | D | 5 – 1000 ms | Time to fall from peak to sustain level |
| Sustain | S | 0 – 100 % | Volume held while pad is pressed |
| Release | R | 10 – 5000 ms | Time to silence after pad is released |

---

### FLT — Filter

> ![FLT column](images/main_flt.jpg)

A resonant filter shaping the harmonic content. Filter type is shown as a small label at the top of the column (`LPF` / `BPF` / `HPF`).

| Sub-param | Symbol | Description |
|-----------|--------|-------------|
| Cutoff | F | Frequency threshold (exponential, 13 Hz – 12 kHz) |
| Resonance | Q | Filter peak / self-oscillation (Q 0.7 – 11) |
| Type | — | Cycle through LPF → BPF → HPF |

---

### LFO — Low-Frequency Oscillator

> ![LFO column](images/main_lfo.jpg)

Modulates the oscillator pitch at a sub-audio rate, creating vibrato and wobble effects.

| Sub-param | Symbol | Range |
|-----------|--------|-------|
| Rate | R | 0.1 – 10 Hz |
| Depth | D | 0 – 100 % |

---

### ECH — Echo

> ![ECH column](images/main_ech.jpg)

A digital delay effect applied to the output.

| Sub-param | Symbol | Description |
|-----------|--------|-------------|
| Amount | A | Wet/dry mix of the echo signal |
| Feedback | F | How much of the echo feeds back into itself (longer tail) |

---

### GLD — Glide

> ![GLD column](images/main_gld.jpg)

Portamento — when a new note is triggered while another is held, pitch slides between them.

| Parameter | Range |
|-----------|-------|
| Glide time | 0 – 500 ms |

Set to 0 for no glide (instant pitch change).

---

## Presets — PRST

Enter by selecting the **PRST** card and pressing the encoder.

> ![Presets view](images/presets_view.jpg)

Six slots are displayed in a 2 × 3 grid. An **orange filled square** means the slot holds a saved preset; a **gray outline** means it is empty. The selected slot is highlighted with an orange border.

### Navigating and acting on a slot

1. **Rotate** to move between the six slots.
2. **Short press** to open the action menu for the highlighted slot.
3. Inside the action menu, **rotate** to choose an action, then **short press** to execute.
4. **Long press** at any point to go back.

| Action | Available when | Effect |
|--------|---------------|--------|
| LOAD | Slot is occupied | Applies all saved parameters immediately |
| SAVE | Always | Captures current MAIN parameters into this slot |
| CLR | Slot is occupied | Erases the slot |

Presets store: waveform, filter type, cutoff, resonance, attack, decay, sustain, release, LFO rate, LFO depth, echo amount, echo feedback, and glide. Presets survive power cycles (stored in NVS flash).

---

## Looper

The looper captures audio in a PCM buffer in PSRAM (~19 seconds at 48 kHz). It is controlled entirely from the **cards view** using hold gestures on the encoder button — no need to enter any screen.

> ![Looper dot states](images/looper_dot.jpg)

### States

| Dot color | State | Description |
|-----------|-------|-------------|
| Invisible | IDLE | No loop active |
| Red | RECORDING | Capturing audio into the buffer |
| Green | PLAYING | Loop playing back continuously |
| Yellow | OVERDUB | Playing back while recording new material on top |

### Controls (from the cards view only)

| Gesture | Action |
|---------|--------|
| Hold encoder 0.8 s | Cycle forward: IDLE → REC → PLAY → OVERDUB → PLAY → … |
| Hold encoder 3.5 s | Clear loop and return to IDLE |

**Typical workflow:**
1. Hold 0.8 s → dot turns **red** — play your phrase.
2. Hold 0.8 s again → dot turns **green** — loop plays back.
3. Hold 0.8 s again → dot turns **yellow** — play over the loop (overdub).
4. Hold 0.8 s to exit overdub back to playing.
5. Hold 3.5 s at any time to wipe the loop.

> **Note:** When you hold for 3.5 s, the 0.8 s threshold fires first (cycling the state once), then the 3.5 s threshold fires (clearing). This is by design — a single long hold always results in a clean stop.

---

## Calibration — CAL

Touch sensitivity differs between pads and environments. Calibration lets you set the detection threshold for each pad individually.

### Overview screen

Enter by selecting the **CAL** card and pressing the encoder.

> ![Calibration overview](images/calib_overview.jpg)

Ten vertical bars are shown — one per pad (labeled 1–8) plus the two octave buttons (labeled − and +). Bar height represents the stored threshold level relative to the highest-set pad. Rotate the encoder to highlight a pad; short-press to edit it.

### Single-pad edit

> ![Calibration single pad](images/calib_edit.jpg)

The selected pad's name appears at the top. A large horizontal bar shows:

| Element | Description |
|---------|-------------|
| **Gray fill** | Current touch delta (how much the pad deviates from its resting baseline) |
| **Green fill** | Same as above, but turns green when the delta exceeds the threshold (pad is "triggered") |
| **Orange vertical line** | Current threshold position — moves left/right as you adjust |

Touch the pad while in this screen to see the live delta grow toward the threshold line.

| Action | Effect |
|--------|--------|
| Rotate encoder | Move threshold up / down (steps of 50) |
| Short press | Save threshold and return to overview |
| Long press | Save threshold and return to cards |

**Setting a good threshold:** Gently rest your finger on the pad. The bar should fill noticeably but the orange line should sit just above the resting level. Adjust until light touches trigger reliably and accidental brushes do not.

---

## Quick Reference

### Encoder — cards view

| Input | Action |
|-------|--------|
| Rotate | Navigate between cards |
| Short press | Enter selected card |
| Hold 0.8 s | Looper: cycle state |
| Hold 3.5 s | Looper: clear |

### Encoder — MAIN view

| Input | Browse mode | Edit mode |
|-------|------------|-----------|
| Rotate | Select column | Adjust parameter |
| Short press | Enter edit | Next sub-parameter |
| Long press | — | Save + back to cards |

### Encoder — PRST view

| Input | Browse mode | Action menu |
|-------|------------|-------------|
| Rotate | Select slot | Select action |
| Short press | Open action menu | Execute action |
| Long press | Back to cards | Cancel (stay in PRST) |

### Encoder — CAL view

| Input | Overview | Edit |
|-------|---------|------|
| Rotate | Select pad | Adjust threshold |
| Short press | Edit selected pad | Save + back to overview |
| Long press | Back to cards | Save + back to cards |
