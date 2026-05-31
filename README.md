# Synthewi — Touch Synthesizer (ESP32-S3)

Polyphonic touch synthesizer running on ESP32-S3 N8R2 (8 MB Flash + 2 MB Quad PSRAM).
8 capacitive touch pads play a C pentatonic scale through a software synth engine (AMY),
with a rotary-encoder + TFT UI for real-time sound shaping.

---

## Current state

### What works

| Feature | Detail |
|---------|--------|
| **8-pad polyphonic touch** | C4 D4 E4 G4 A4 C5 D5 E5 (C pentatonic), sustained while held |
| **Octave shift** | Two touch pads (GPIO 9/10) shift all notes ±1 octave, range −2 to +2, 500 ms debounce |
| **AMY synth engine** | CUSTOM mode — one osc per pad, all params live-editable |
| **Waveforms** | Square, Saw, Triangle — 3-position physical lever (GPIO 41/42), no menu needed |
| **Filter** | LPF / BPF / HPF — Spark Juno exponential formula, capped at 12 kHz |
| **ADSR amplitude** | Attack 2–2000 ms · Decay 5–1000 ms · Sustain 0–100 % · Release 10–5000 ms |
| **Echo** | Level + feedback — delay buffer in PSRAM |
| **Glide (portamento)** | 0–500 ms, cross-pad: idle oscs pre-parked at last played freq |
| **LFO** | Rate 0.5–20 Hz, depth 0–5000 Hz — sine wave modulates filter cutoff |
| **NVS persistence** | All params saved to flash on section exit; version-checked |
| **Presets** | 6 NVS-backed slots — save / load / clear current sound |
| **PSRAM** | 2 MB Quad PSRAM at 80 MHz — echo delay line + ~1.74 MB free for looper |

---

### UI layout (ST7735, 160×128 landscape)

```
y=0..21   ┌──────────────────────────────────────────────────────────────────┐
          │  ● ● ● ● ● ● ● ●    [~~~]    OCT 0                              │  ← ribbon bar
          │  pad circles       waveform   octave                             │
y=22..21  ├──────────────────────────────────────────────────────────────────┤
y=22..127 │                CARDS  (encoder navigates, short press enters)    │
          │     MAIN          │      PRST         │       CAL               │
          └───────────────────┴──────────────────-┴─────────────────────────┘
```

**Ribbon bar** (always visible, y=0..21): 8 pad circles (green=active), waveform glyph, octave label. Redraws on any touch or lever event.

**MAIN card** — 5 Spark-style columns (encoder navigates columns, short press enters edit, long press saves + exits):

| Column | Controls |
|--------|---------|
| ENV | ADSR envelope shape — A · D · S · R |
| FLT | Filter cutoff · Resonance · Type (LPF/BPF/HPF) |
| LFO | Rate · Depth |
| ECH | Echo amount · Feedback |
| GLD | Glide (portamento) |

**PRST card** — 6 preset slots in 2×3 grid. Each slot shows wave glyph + filter + attack summary. Short press → action menu (LOAD / SAVE / CLR), long press back to cards.

**CAL card** — touch threshold editor for all 10 inputs (8 note pads + OCT− OCT+). Rotate to select pad, short press to edit threshold, long press to save and exit.

---

## Hardware

### Pinout

| Function | GPIO |
|----------|------|
| I²S BCLK (→ MAX98357A) | 38 |
| I²S LRC | 39 |
| I²S DOUT | 40 |
| TFT SCLK (SPI2) | 36 |
| TFT MOSI | 35 |
| TFT CS | 37 |
| TFT DC | 45 |
| TFT RST | 21 |
| Encoder CLK | 15 |
| Encoder DT | 16 |
| Encoder SW | 17 |
| Waveform lever A (SQUARE side) | 41 |
| Waveform lever B (TRIANGLE side) | 42 |
| Octave down (touch CH8) | 9 |
| Octave up (touch CH9) | 10 |
| Note pads 1–8 (touch CH0–CH7) | 1–8 |

### Wired but not yet in firmware

| Function | GPIO |
|----------|------|
| CD4067BE Z (ADC in) | 11 |
| CD4067BE select A | 12 |
| CD4067BE select B | 13 |
| CD4067BE select C | 14 |

See `docs/HARDWARE_UI_PLAN.md` § 1C for full CD4067BE + potentiometer wiring.

### Board

ESP32-S3-DevKitC-1 N8R2 — 8 MB Flash, 2 MB Quad PSRAM, 240 MHz dual-core.

---

## Build & flash

```powershell
idf.py build
idf.py -p COM5 flash monitor
```

Target: `esp32s3`. PSRAM (Quad, 80 MHz) is enabled in `sdkconfig`.

---

## Architecture

- **Core 0** — AMY render task (audio DMA loop, never blocked)
- **Core 1** — `app_main`: encoder polling (20 ms), touch telemetry task, UI tick

NVS writes happen only on explicit section exit (long press back), not on every encoder tick.
Preset saves and loads are explicit actions inside the PRST card.

---

## Next steps

### Looper (next)
- ~1.74 MB PSRAM free — event-based record/playback on Core 1
- States: IDLE → ARM → RECORD → PLAY → OVERDUB
- Trigger: long-press OCT− (prototype) → dedicated hardware button (final)
- Ribbon bar shows LOOP status when active

### CALIB card redesign
- 8 pad columns + 2 oct columns with live bar graphs
- 50 ms timed refresh (hardware touch values update without user input)

### MUX + potentiometers (CD4067BE already wired)
- 6 pots: Attack · Release · Cutoff · Resonance · Glide · Echo
- Pot overrides encoder for its parameter; encoder resumes from pot position
- See `docs/HARDWARE_UI_PLAN.md` § 1C for full details
