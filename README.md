# Synthewi — Touch Synthesizer (ESP32-S3)

Polyphonic touch synthesizer running on ESP32-S3 N8R2 (8 MB Flash + 2 MB Quad PSRAM).
8 capacitive touch pads play a C pentatonic scale through a software synth engine (AMY),
with a rotary-encoder + TFT UI for real-time sound shaping.

---

## Current state

### What works

| Feature | Detail |
|---------|--------|
| **8-pad polyphonic touch** | C4 D4 E4 G4 A4 C5 D5 E5 (C pentatonic), pressure-sensitive |
| **Octave shift** | Two touch pads (GPIO 9/10) shift all notes ±1 octave, range −2 to +2, 500ms debounce |
| **AMY synth engine** | CUSTOM mode (one osc per pad) or JUNO/DX7 PATCH mode |
| **Waveforms** | Square, Saw, Triangle — selected by 3-position physical lever (GPIO 41/42), no menu needed |
| **Filter** | LPF / BPF / HPF — Spark Juno exponential formula, capped at 12 kHz |
| **Filter envelope** | EG1 sweeps filter cutoff from attack peak to decay floor |
| **ADSR amplitude** | Attack 2–2000 ms · Decay 5–1000 ms · Sustain 0–100 % · Release 10–5000 ms |
| **Echo** | Stereo echo at 250 ms, level + feedback — delay buffer in PSRAM |
| **Glide (portamento)** | 0–500 ms, cross-pad: idle oscs pre-parked at last played freq |
| **LFO** | Rate 0.5–20 Hz, depth 0–5000 Hz — sine wave modulates filter cutoff |
| **TFT display** | ST7735 128×160, scale-2 font, 4-section grid menu |
| **Rotary encoder** | Navigation, value adjust; short press = select/confirm, long = back |
| **NVS persistence** | All params saved to flash on section exit; version-checked to reject stale data |
| **PSRAM** | 2 MB Quad PSRAM at 80 MHz — echo delay line (~256 KB used, ~1.74 MB free) |

### UI sections

```
┌────────┬────────┐
│  WAVE  │ CALIB  │   Top-left: live status (non-interactive)
│ ~~~    │ PADS   │   Others: short press → enter, long press → back
├────────┼────────┤
│   FX   │  ADSR  │
│ LPF    │ EDIT   │
└────────┴────────┘
```

**WAVE cell (top-left, non-interactive)** — live status: pixel-art waveform glyph (SAW/SQR/TRI), wave name, and octave offset (`OCT:+1`). Updates immediately when lever or octave pads change.

**CALIB** — touch threshold editor for all 10 touch inputs: pads 1–8 (notes) + OCT- and OCT+ (octave buttons). Rotate to choose, short press to edit, rotate to adjust.

**FX** — 8 scrollable items:

| Item | Description | Range |
|------|-------------|-------|
| FLT | Filter type | LPF / BPF / HPF |
| CUT | Cutoff frequency | 13–12 000 Hz (Spark Juno exponential) |
| Q | Resonance | Q 0.7–11.2 (Spark exponential) |
| ECH | Echo level | 0–100 % |
| EFB | Echo feedback | 0–90 % |
| GLD | Glide (portamento) | 0–500 ms |
| LFR | LFO rate | 0.5–20 Hz |
| LFD | LFO depth | 0–5000 Hz |

**ADSR** — Attack · Decay · Sustain · Release. Applied live on each encoder tick.

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
| Octave down (touch) | 9 |
| Octave up (touch) | 10 |

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

NVS writes happen only on explicit section exit (long press back), not on every encoder tick,
so real-time parameter changes are glitch-free.

---

## Next steps

### UI redesign (Phase 2–3 per HARDWARE_UI_PLAN.md)
- Ribbon bar (top 20px): 8 pad circles + waveform glyph + octave label
- 3-card horizontal layout: MAIN / PRESETS / CALIB
- MAIN card: Spark-style parameter columns (ENV, FLT, LFO, ECH, GLIDE)
- PRESETS card: 6 NVS-backed slots (3×2 grid, load/save)
- CALIB card: 8 pad columns + 2 oct columns, 50ms refresh

### Looper (after UI)
- ~1.74 MB PSRAM free after echo (~9.9 s mono at 44100 Hz)
- Event-based record/playback via FreeRTOS timer task on Core 1
- States: IDLE → ARM → RECORD → PLAY → OVERDUB → STOP

### MUX + potentiometers (after hardware purchase)
- CD4051 8:1 mux → 6 hardware pots for attack, release, cutoff, resonance, glide, echo
- Deadband filter (±50 counts) to suppress ADC noise
- Encoder still works; pot touch overrides encoder value
