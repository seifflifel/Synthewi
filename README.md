# Synthewi — Working Prototype

ESP32-S3 capacitive-touch synthesizer. Four touch pads drive an onboard AMY sound engine streamed over USB Audio Class. A Node.js bridge relays telemetry and control commands between the device and a browser-based web UI.

## What works in this checkpoint

- **Touch → synthesis**: each pad triggers note-on/note-off through AMY. Notes sustain while held, release on lift.
- **Waveforms**: pulse (duty 0.2 — nasal/buzzy), saw↓, triangle, square (duty 0.5 — fuller). Default is pulse.
- **Filter**: LPF / BPF / HPF selectable per session. Cutoff (200–10 000 Hz) and resonance (0–0.9) adjustable live.
- **Filter envelope (EG1)**: depth (0–8 000 Hz above cutoff) and decay (5–2 000 ms). Triggers on each note-on for a classic synth filter sweep.
- **LFO → filter**: per-pad SINE LFO modulating filter cutoff. Rate (0.1–10 Hz) and depth (0–5 000 Hz) adjustable live.
- **Amplitude envelope (EG0)**: attack (5–2 000 ms) and release (50–5 000 ms) via web UI sliders.
- **NVS persistence**: all synth parameters survive reboot and are restored on init.
- **Touch telemetry v3**: UDP broadcast at 30 Hz. 28-byte synth struct appended (wave, filter type, 13 additional params).
- **USB Audio Class**: ESP32-S3 enumerates as a USB mic. AMY renders audio mono 16-bit into the UAC callback.
- **WiFi + bridge**: ESP connects to AP; bridge (Node.js) binds UDP 4210/4211 and serves WebSocket on 8787.
- **Web UI state sync**: all controls populate from the first telemetry packet on connect.

## Known constraints

- **Reverb and echo disabled** — AMY delay lines need large contiguous heap blocks (~16 KB each). WiFi stack runs first and fragments SRAM, so allocation silently fails. Removed from UI and bridge to avoid confusion.
- **KS (Karplus-Strong) not available** — requires `ks_oscs > 0` pre-allocated at startup (~32 KB); heap too tight with WiFi active.
- **USB audio is mono** — AMY renders stereo; only the left channel is passed to the USB callback. Per-pad stereo pan is set in the engine but has no audible effect over USB.

## Repository structure

```text
Synthewi/
├── main/
│   ├── main.c               # App entry, touch/synth callback wiring
│   ├── touch_telemetry.c/h  # UDP telemetry + command tasks, v3 protocol
│   └── wifi_manager.c/h     # WiFi STA connection
├── components/
│   ├── amy_engine/          # AMY wrapper: note on/off, filter, LFO, filter env, NVS
│   └── touch_control/       # ESP-IDF touch sensor driver abstraction
├── third_party/amy/         # AMY synth engine (submodule, patched for ESP32-S3)
├── bridge/
│   ├── src/index.ts         # Node.js UDP↔WebSocket bridge source
│   └── dist/index.js        # Compiled bridge (run with: node dist/index.js)
├── web-ui/
│   ├── src/ui/app.ts        # Browser UI source
│   └── dist/                # Built web UI (served by bridge or any static host)
├── docs/
│   └── IMPLEMENTED_FEATURES.md  # Detailed feature log
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

If flashing after a parameter-schema change, erase NVS first to avoid stale values:

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

## Next steps — final prototype stage

- **More touch pads**: raise `SYNTH_PAD_COUNT` and `TOUCH_CHANNEL_COUNT`. ESP32-S3 supports up to 14 capacitive channels. Add one LFO oscillator per new pad (already structured for it).
- **More waveforms / sounds**: explore AMY's full palette (partials, PCM samples). Design a small library of preset patches that sound good out of the box.
- **Polyphony across all pads**: currently each pad has its own oscillator — they are already independent. Verify simultaneous multi-pad play sounds clean.
- **Better web UI**: redesigned layout for the expanded pad count, preset selection, visual feedback per pad (color/glow on touch).
- **Velocity response**: map touch delta magnitude to AMY amplitude or filter envelope depth.
- **USB audio quality test**: measure end-to-end latency (touch ISR → AMY → USB host). Target < 20 ms. Validate with the embedded WAV test mode (`TEST_WAV_PLAYBACK 1`).
- **Hardware build**: finalize BOM (touch pad material, wiring, enclosure). Run hardware tests with the firmware locked at this checkpoint before moving to the expanded pad count.
