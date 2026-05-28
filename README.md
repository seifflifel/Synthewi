# Synthewi — Working Prototype

ESP32-S3 capacitive-touch synthesizer. Eight touch pads drive an onboard AMY sound engine streamed over USB Audio Class. A Node.js bridge relays telemetry and control commands between the device and a browser-based web UI.

## What works in this checkpoint

- **8 touch pads**: pentatonic scale C D E G A C D E across two octaves (MIDI 60–76). Touch channels GPIO 4,5,6,7,8,12,1,2 — avoids flash-bus pins 9/10/11 and power-rail-adjacent saturated pins 13/14.
- **Two synthesis modes**:
  - **CUSTOM**: full manual control — oscillator, filter, LFO, filter envelope, pressure expressiveness, glide.
  - **PATCH**: AMY built-in presets — Juno (patches 0–127) and DX7 (128–255). Touch triggers notes; no filter/pressure override.
- **Waveforms**: pulse, saw, triangle, square. Selectable per session.
- **Filter**: LPF / BPF / HPF selectable. Cutoff (200–10 000 Hz) and resonance (0–0.9) adjustable live. LFO depth clamped so cutoff never drops below 50 Hz.
- **Filter envelope**: depth (0–8 000 Hz above cutoff) and decay (5–2 000 ms). Triggers on each note-on.
- **LFO → filter**: per-pad SINE LFO modulating filter cutoff. Rate and depth adjustable live.
- **Amplitude envelope**: attack and release via web UI sliders.
- **Pressure expressiveness**: finger pressure (delta above threshold) modulates filter cutoff in real time at 30 Hz. Depth and range adjustable.
- **Portamento / glide**: smooth pitch slide on pad re-trigger (0–500 ms).
- **Octave shift**: momentary OCT− / OCT+ buttons in the web UI shift all pad notes ±12 semitones while held.
- **Patch browser**: prev/next buttons browse Juno and DX7 preset banks in PATCH mode.
- **NVS persistence**: all synth parameters survive reboot and are restored on init.
- **USB Audio Class**: ESP32-S3 enumerates as a USB mic. AMY renders at 48 kHz, 16-bit mono into the UAC callback. Output gain 2× compensates AMY's ESP-specific internal bit-shift.
- **WiFi + bridge**: ESP connects to AP; bridge (Node.js) binds UDP 4210/4211 and serves WebSocket on 8787.
- **Web UI**: 4-column control layout (OSC / FILTER / MOD / ENV), mode tabs, patch browser, 8-pad grid with live touch indicators and per-pad threshold sliders.

## Known constraints

- **Reverb and echo disabled** — AMY delay lines need large contiguous heap blocks. WiFi stack fragments SRAM before they can allocate. Will be re-enabled on final hardware once WiFi is removed.
- **KS (Karplus-Strong) not available** — requires pre-allocated oscillators; heap too tight with WiFi active.
- **USB audio is mono** — AMY renders stereo; only the left channel is used. Per-pad stereo pan is set but inaudible over USB.
- **Chorus disabled** — same heap reason as reverb.

## Repository structure

```text
Synthewi/
├── main/
│   ├── main.c               # App entry, USB audio callback, touch/synth wiring
│   ├── touch_telemetry.c/h  # Touch state machine, pressure, UDP telemetry + command tasks
│   ├── wifi_manager.c/h     # WiFi AP connection
│   └── wifi_config_server.c # HTTP config server
├── components/
│   ├── amy_engine/          # AMY wrapper: note on/off, filter, LFO, filter env, pressure, NVS
│   └── touch_control/       # ESP-IDF touch sensor driver abstraction
├── third_party/amy/         # AMY synth engine (patched: 48 kHz for ESP32-S3)
├── bridge/
│   ├── src/index.ts         # Node.js UDP↔WebSocket bridge source
│   └── dist/index.js        # Compiled bridge (run with: node dist/index.js)
├── web-ui/
│   ├── src/ui/app.ts        # Browser UI source
│   └── dist/                # Built web UI (served by bridge or any static host)
├── docs/
│   ├── notes.md             # Touch sensor physics, pin selection rationale
│   ├── NEXT_STAGE_PLAN.md   # Prototype phase plan and technical stack
│   ├── FINAL_HARDWARE_BRAINSTORM.md  # Final hardware design (MAX98357A, pots, OLED, enclosure)
│   └── I2S_MIGRATION_PLAN.md        # Plan for switching USB→I²S + removing WiFi
├── partitions.csv           # Custom partition table (2 MB factory slot)
└── README.md
```

## Build and run

### Firmware

```powershell
idf.py build
idf.py -p COM5 flash
idf.py -p COM5 monitor
```

If flashing after a parameter-schema change, erase NVS first:

```powershell
idf.py -p COM5 erase-flash
idf.py -p COM5 flash monitor
```

### Bridge

```powershell
cd bridge
npm install
node dist/index.js
```

### Web UI (dev)

```powershell
cd web-ui
npm install
npm run dev
```

Or serve `web-ui/dist/` statically — no build step needed for the pre-built assets.

## Firmware tags

| Tag | Description |
|-----|-------------|
| `firmware/wifi-usb` | Current checkpoint — USB audio + WiFi web UI fully working |

To flash a tagged version: `git checkout firmware/wifi-usb && idf.py flash`

## Next steps — hardware stage

See [`docs/I2S_MIGRATION_PLAN.md`](docs/I2S_MIGRATION_PLAN.md) for the full plan. Summary:

- **Switch audio output**: USB UAC → I²S + MAX98357A Class D amplifier (×2 for stereo)
- **Remove WiFi**: free heap unlocks reverb, echo, chorus
- **Physical controls**: 6 potentiometers, OLED display, rotary encoder, 3-position toggle, DIP switch, OCT buttons
- **3D-printed enclosure**: 8-pad row (aluminum tape), speaker grilles, panel-mount controls
- **Core pinning**: dedicated audio task on Core 0, all other tasks on Core 1
- **Phase 6 / 7**: looper (PSRAM buffer) + startup auto-calibration (final hardware only)
