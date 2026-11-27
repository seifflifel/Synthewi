# Synthewi — Touch Synthesizer (ESP32-S3)

Polyphonic, touch-expressive synthesizer on an ESP32-S3 N8R2 (8 MB flash, 2 MB quad PSRAM).
8 capacitive pads play a C pentatonic scale through the AMY synth engine; a single rotary
encoder + 160×128 TFT drive the whole UI, car-radio style.

---

## What works

### Playing
| Feature | Detail |
|---|---|
| **8-pad polyphony** | C4 D4 E4 G4 A4 C5 D5 E5, one oscillator per pad, sustained while held |
| **Velocity** | Per-pad MIN (10 %) / MAX (100 %) touch thresholds, set in CAL |
| **Aftertouch / expression** | While a pad is held, volume *and* filter follow the touch continuously (gain: concave boost curve; filter: up to +3 octaves above the preset cutoff) |
| **Octave shift** | Two touch pads, −2…+2; label flashes red at the limit |
| **Waveforms** | Square · Saw · Triangle on a 3-position lever |
| **Glide** | 0–500 ms, glides between pads (cross-oscillator portamento, AMY patch) |
| **Panic** | 5 fast presses of the encoder button: all notes + echo/reverb tails cut, loop cleared |

### Sound engine (AMY)
| Feature | Range |
|---|---|
| **ADSR** | Attack 1 ms–3 s · Decay 5 ms–4 s · Sustain 0–100 % · Release 10 ms–8 s (exponential) |
| **Filter** | LPF / BPF / HPF · cutoff 60 Hz–18 kHz (exp) · Q 0.7–11.2 |
| **Filter envelope (EG1)** | Depth 0–5 octaves · Decay 5 ms–4 s |
| **LFO → filter** | 0.1–20 Hz (exp) · depth 0–3 octaves |
| **Echo** | Level · feedback · delay snapped to 1/16 … 1/2 s (500 ms buffer) |
| **Reverb** | Amount · size (wet 0–0.6, liveness 0.5–0.92) |
| **Output stage** | AMY at −6 dB internal headroom → +6 dB make-up → peak limiter (~−8 dBFS) → master volume |
| **Master volume** | Hold the encoder button + turn (5 %/click, perceptual curve), saved |

### Looper
Double-click the encoder button anywhere: **REC → PLAY → OVERDUB → PLAY …**; recording starts on
the 2nd click. Hold 2 s on the top screen to clear. ~11.4 s mono loop in PSRAM. Non-destructive
loop volume (engine + NVS ready, UI control pending). Progress API ready for a loop bar.

### Presets
6 NVS slots — LOAD / SAVE / CLR. Factory presets: **P1 ACID · P2 DREAMPAD · P3 KALIMBA ·
P4 WOBBLE · P5 WAHWAH · P6 BRASS** (names shown; SAVE/CLR clear the name). Old presets and saved
state are migrated automatically when parameter curves change.

### UI (encoder + ST7735)
- **Framebuffer** with diff-based DMA flush (only changed pixels are sent), SPI 40 MHz — no flicker.
- **Ribbon**: pad circles, waveform glyph, octave label, looper dot.
- **Top screen**: MAIN · PRST · CAL.
- **MAIN**: 6 cards (ENV · FLT · EG1 · LFO · FX · GLD). Inside a card: rotate = move between
  params, click = edit (inverted line), rotate = change value (subtle acceleration), click = confirm.
- **CAL**: per-pad live bar; note pads have MIN (orange) and MAX (blue) markers + live VEL %.

### Encoder gestures
| Gesture | Action |
|---|---|
| Turn | Move / change value (lists stop at the ends) |
| Click | Enter / edit / confirm (acts 250 ms after release, to detect double clicks) |
| Double click | Looper REC → PLAY → OVERDUB → PLAY |
| Hold 0.5 s | Back |
| Hold 2 s (top screen) | Clear loop |
| Hold + turn | Master volume |
| 5 fast presses | Panic |

---

## Hardware

| Function | GPIO |
|---|---|
| I²S BCLK / LRC / DOUT (→ MAX98357A) | 38 / 39 / 40 |
| TFT SCLK / MOSI / CS / DC / RST (SPI2, 40 MHz) | 36 / 35 / 37 / 45 / 21 |
| Encoder CLK / DT / SW | 15 / 16 / 17 |
| Waveform lever A / B | 41 / 42 |
| Note pads 1–8 (touch channel = GPIO) | 4, 5, 6, 7, 8, **3**, 1, 2 |
| Octave − / + | 9 / 10 |
| CD4067BE pot MUX Z / A / B / C (wired, firmware disabled) | 11 / 13 / 14 / 47 |

Board: ESP32-S3-DevKitC-1 N8R2, 240 MHz, `-O2`. Serial logs on the UART connector (COM5, CH343).

---

## Build & flash

```powershell
. C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1
idf.py build
idf.py -p COM5 flash monitor
```
Clone with submodules: `git clone --recurse-submodules …` — `third_party/amy` is the
[seifflifel/amy](https://github.com/seifflifel/amy) fork, branch `synthewi`, which carries our AMY
patches (48 kHz, portamento/LFO fixes, mod-source guards, audio block hook). Any new AMY change must
be committed and pushed there (`git push fork main:synthewi`), then the gitlink committed here.

---

## Architecture

| Task | Core | Role |
|---|---|---|
| AMY render | 0 (prio 24) | voice rendering |
| AMY fill / I²S | 1 (prio 24) | mixing, effects, `amy_audio_block_hook` → looper + limiter + volume |
| touch telemetry | 1 (prio 5) | 100 Hz polling, note on/off, velocity, expression |
| app_main | 0 (prio 1) | lever, gestures, UI tick + framebuffer flush (50 Hz) |
| esp_timer | — | 1 kHz encoder + button polling |

Code layout:
```
main/main.c               app, gestures, octave, master/loop volume, panic
main/touch_telemetry.c    touch → notes, MIN/MAX thresholds, velocity, expression
main/ui/                  tft.c (driver + framebuffer, only SPI user), presets.c,
                          ui.c (state + input), ui_ribbon / ui_cards / ui_main /
                          ui_presets / ui_calib .c, ui_legacy.c (unused screens, kept)
components/amy_engine/    AMY wrapper: params, curves, NVS state, expression, panic
components/looper/        looper + output_limiter.c (make-up, limiter, master volume)
components/touch_control/ ESP32-S3 touch driver wrapper (32-bit readings)
third_party/amy/          AMY (fork)
```
NVS: `synth` (engine state v4), `pst` (presets + names `n0..n5`, factory set `fac`),
`touch` (`thrN`, `tmxN`), `ui` (`mvol`, `lvol`).

---

## Roadmap — what's left

### Next: UI redesign
- New **home screen** with an **oscilloscope** of the output
- **PARAMS / PRESETS / SETTINGS** tabs; parameters as **animated, procedurally drawn icons**
  (envelope reshapes, filter curve moves, LFO scrolls) instead of sliders
- Car-style **pop-ups**: volume (icon + slider), SAVED / CLR / LOADED messages, big value readout
- **Loaded-preset indicator** (and "modified" mark)
- **Loop bar** with position/length (data API ready: `looper_get_progress`) + loop volume control
- **SETTINGS**: pad calibration, scales / key, light / dark theme, expression options
  (target amp / filter / both, filter depth, amp curve, velocity → filter)

### Touch
- Tune velocity / expression feel on hardware
- **Auto-calibration routine** (idle noise, then firm press per pad → MIN/MAX)

### Sound / features
- Scales / key selection (with SETTINGS)
- More factory presets; Juno/DX7 patch mode exploration

### Known / small
- Looper record→play switch can glitch a few ms (state change not block-aligned)
- Boot relies on `vTaskDelay(200)` so AMY allocates echo/reverb before the looper
- A layout-sensitive boot crash (bad `mod_source`) was seen once; guarded in AMY, root cause unknown
- `docs/` is partly outdated; dead code (wifi, usb_midi, audio_synth, legacy screens) kept on purpose

### Hardware
- Pad 6 wire: route separately, ~470 Ω series resistor (saturated when the lid squeezed the bundle)
- Solder nickel plates, remodel top panel, optional hardware volume slider
- Standalone power, 3.5 mm output, USB MIDI mode
