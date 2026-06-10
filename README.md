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
| **KS pluck (easter egg)** | Karplus-Strong synthesis — load preset P6 to activate; ribbon shows bold "P"; lever overrides back to waveform |
| **Filter** | LPF / BPF / HPF — exponential curve, 200 Hz min · 12 kHz max |
| **Filter envelope (EG1)** | Depth 0–8000 Hz · Decay 5–2000 ms — sweeps cutoff on each note |
| **ADSR amplitude** | Attack 2–2000 ms · Decay 5–1000 ms · Sustain 0–100 % · Release 10–5000 ms |
| **Echo** | Level · Feedback · Delay (snaps to 1/16 · 1/8 · 1/6 · 1/4 · 1/3 · 1/2 s) — 500 ms buffer in PSRAM |
| **Glide (portamento)** | 0–500 ms, cross-pad: idle oscs pre-parked at last played freq |
| **LFO** | Rate · Depth — sine wave modulates filter cutoff *(currently unreliable)* |
| **Looper** | Event-based record / playback / overdub in PSRAM |
| **NVS persistence** | All params saved on card exit; version-checked (v3); stale saves auto-reset |
| **Presets** | 6 NVS-backed slots — save / load / clear; P6 factory-seeded with KS pluck; pots lock on load, smooth takeover on move |
| **6-pot MUX** | Attack · Release · Filter · Resonance · Echo · Glide via CD4067BE; EMA smoothing, dual deadband, preset lock |
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

**MAIN card** — 6-card 3×2 grid overview (TE-inspired):

```
┌──────────┬──────────┬──────────┐
│   ENV    │   FLT    │   EG1   │
│ [graph]  │ F  Q typ │ dep dec │
├──────────┼──────────┼──────────┤
│   LFO    │   ECHO   │   GLD   │
│ rate dep │ % 1/frac │  ms     │
└──────────┴──────────┴──────────┘
```

Encoder scrolls highlight. Short press → per-card full-screen edit (ROT adjusts, BTN cycles params, hold → save + back). Long press → save NVS + back to CARDS.

| Card | Params |
|------|--------|
| ENV | Attack · Decay · Sustain · Release |
| FLT | Cutoff (200–12 kHz) · Resonance · Type |
| EG1 | Filter-env Depth · Filter-env Decay |
| LFO | Rate · Depth |
| ECHO | Amount · Feedback · Delay (musical fraction) |
| GLD | Glide time |

**PRST card** — 6 preset slots. Short press → LOAD / SAVE / CLR action menu. P6 factory-seeded with KS pluck.

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

### Potentiometer MUX (live)

| Function | GPIO |
|----------|------|
| CD4067BE Z (ADC in) | 11 (ADC2 CH0) |
| CD4067BE select A | 13 |
| CD4067BE select B | 14 |
| CD4067BE select C | 47 |

6 pots wired and active — see [docs/POTENTIOMETER_MUX_SYSTEM.md](docs/POTENTIOMETER_MUX_SYSTEM.md) for full wiring and firmware details.

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

- **KS pluck sound quality** — P6 easter egg works but tone character needs tuning (feedback, filter, echo params)
- **Better sounds** — KS pluck tuning; Juno/DX7 preset mode exploration; more factory presets
- **Solder Nickel Plates**
- **Remodel top panel** — sleeker touch key layout, keys embedded under structure and connected to nickel plates
- **Reset button** — dedicated physical reset input
- **Looper button** — dedicated physical trigger instead of long-press shortcut

### Long term

- **Standalone power** — USB → buck-boost converter for battery operation
- **Cable management** — internal routing, clean PCB layout
- **MIDI mode** — USB MIDI device class output
- **TRRS BREAKOUT** — 3.5 mm audio output jack
- **Easter egg button** — randomize params