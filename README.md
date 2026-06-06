# Synthewi — Touch Synthesizer (ESP32-S3)

**Prototype v0** — fully functional breadboard prototype with printed enclosure.

Polyphonic touch synthesizer running on ESP32-S3 N8R2 (8 MB Flash + 2 MB Quad PSRAM).
8 capacitive touch pads play a C pentatonic scale through a software synth engine (AMY),
with a rotary-encoder + TFT UI for real-time sound shaping.

---

## Current state (prototype v0)

### What works

| Feature | Detail |
|---------|--------|
| **8-pad polyphonic touch** | C4 D4 E4 G4 A4 C5 D5 E5 (C pentatonic), sustained while held |
| **Octave shift** | Two touch pads (GPIO 9/10) shift all notes ±1 octave, range −2 to +2 |
| **AMY synth engine** | CUSTOM mode — one osc per pad, all params live-editable |
| **Waveforms** | Square, Saw, Triangle — 3-position physical lever (GPIO 41/42) |
| **Filter** | LPF / BPF / HPF — exponential curve, capped at 12 kHz |
| **ADSR amplitude** | Attack 2–2000 ms · Decay 5–1000 ms · Sustain 0–100 % · Release 10–5000 ms |
| **Echo** | Level + feedback — delay buffer in PSRAM |
| **Glide (portamento)** | 0–500 ms, cross-pad: idle oscs pre-parked at last played freq |
| **LFO** | Rate · Depth — sine wave modulates filter cutoff *(currently unreliable)* |
| **Looper** | Event-based record / playback / overdub in PSRAM |
| **NVS persistence** | All params saved to flash on section exit; version-checked |
| **Presets** | 6 NVS-backed slots — save / load / clear current sound |
| **Menu UI** | Encoder + ST7735 TFT — MAIN · PRST · CAL cards |
| **Speaker** | MAX98357A I2S amplifier, 3W |
| **Power** | USB from PC |
| **Enclosure** | 3D-printed top shell — complete |

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

**Ribbon bar** (always visible, y=0..21): 8 pad circles (green=active), waveform glyph, octave label.

**MAIN card** — 5 Spark-style columns:

| Column | Controls |
|--------|---------|
| ENV | ADSR envelope — A · D · S · R |
| FLT | Filter cutoff · Resonance · Type (LPF/BPF/HPF) |
| LFO | Rate · Depth |
| ECH | Echo amount · Feedback |
| GLD | Glide (portamento) |

**PRST card** — 6 preset slots. Short press → LOAD / SAVE / CLR action menu.

**CAL card** — touch threshold editor for all 10 inputs (8 note pads + OCT− OCT+).

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
| Note pads 1–8 (touch CH0–CH5, CH1–CH2) | 4,5,6,7,8,12,1,2 |

### Wired, firmware pending

| Function | GPIO |
|----------|------|
| CD4067BE Z (ADC in) | 11 (ADC2 CH0) |
| CD4067BE select A | 13 |
| CD4067BE select B | 14 |
| CD4067BE select C | 47 |

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

---

## Roadmap

### Short term

- **Fix LFO** — currently unreliable; needs debugging in AMY engine integration
- **Better sounds** — pluck (Karplus-Strong), Juno/DX7 preset mode exploration
- **Potentiometers via MUX** — 6 pots (Attack · Release · Filter · Resonance · Echo · Glide) through CD4067BE; firmware for `mux_pots.h` planned
- **Remodel top panel** — sleeker touch key layout, keys embedded under structure and connected to nickel plates
- **Reset button** — dedicated physical reset input
- **Looper button** — dedicated physical trigger instead of long-press shortcut

### Long term

- **Standalone power** — USB → buck-boost converter for battery operation
- **Cable management** — internal routing, clean PCB layout
- **MIDI mode** — USB MIDI device class output