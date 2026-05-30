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
| **AMY synth engine** | CUSTOM mode (one osc per pad) or JUNO/DX7 PATCH mode |
| **Waveforms** | Square, Saw, Triangle (all 8 oscs simultaneously) |
| **Filter** | LPF / BPF / HPF with auto-tuned default cutoff per type |
| **Filter envelope** | EG1 sweeps filter cutoff from attack peak to decay floor |
| **ADSR amplitude** | Attack 2–2000 ms · Decay 5–1000 ms · Sustain 0–100 % · Release 10–5000 ms |
| **Reverb** | Stereo hall reverb, level + liveness — delay lines in PSRAM |
| **Echo** | Stereo echo at 250 ms, level + feedback — delay buffer in PSRAM |
| **Glide (portamento)** | 0–500 ms, cross-pad: idle oscs pre-parked at last played freq |
| **LFO** | Rate 0.1–10 Hz, depth 0–5000 Hz — modulates filter cutoff |
| **TFT display** | ST7735 128×160, scale-2 font, 4-section grid menu |
| **Rotary encoder** | Navigation, value adjust; short press = select/confirm, long = back |
| **NVS persistence** | All params saved to flash on section exit |
| **PSRAM** | 2 MB Quad PSRAM at 80 MHz — reverb + echo delay lines (~364 KB used) |

### UI sections

```
┌────────┬────────┐
│  WAVE  │ CALIB  │   Short press → enter section
│ SQR    │ PADS   │   Long press  → back to menu
├────────┼────────┤
│   FX   │  ADSR  │
│ LPF    │ EDIT   │
└────────┴────────┘
```

**WAVE** — cycle Square / Saw / Triangle. Short press selects and saves.

**CALIB** — per-pad touch threshold (rotate to choose pad, short press to edit, rotate to adjust).

**FX** — 10 scrollable items:

| Item | Description | Range |
|------|-------------|-------|
| FLT | Filter type | LPF / BPF / HPF (auto-sets cutoff default) |
| CUT | Cutoff frequency | 200–10 000 Hz |
| RES | Resonance | 0–90 % |
| RVB | Reverb level | 0–100 % |
| RDC | Reverb decay (liveness) | 0–100 % |
| ECH | Echo level | 0–100 % |
| EFB | Echo feedback | 0–90 % |
| GLD | Glide (portamento) | 0–500 ms |
| LFR | LFO rate | 0.1–10.0 Hz |
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
