# Synthewi — AMY Synth Engine Checkpoint

ESP32-S3 capacitive-touch synthesizer. Four touch pads drive an onboard AMY sound engine streamed over USB Audio Class. A Node.js bridge relays telemetry and control commands between the device and a browser-based web UI.

## What works in this checkpoint

- **Touch → synthesis**: each pad triggers note-on/note-off through AMY. Sustained notes while held, clean release on lift.
- **AMY engine**: sine, pulse, saw (down/up), and triangle waveforms. Per-session oscillator selection applied to all four pads.
- **Live FX controls via web UI**: filter cutoff + resonance, reverb amount + decay, echo amount + feedback, envelope attack + release. All adjustable in real time from the browser.
- **NVS persistence**: synth parameters survive reboot. State is restored on `amy_engine_init()`.
- **Touch telemetry v2**: UDP broadcast at 30 Hz includes per-channel touch state and current synth state. Bridge forwards both to WebSocket clients.
- **USB Audio Class**: ESP32-S3 enumerates as a USB mic device. AMY renders audio into the UAC callback.
- **WiFi + bridge**: ESP connects to AP, bridge (Node.js) binds UDP 4210/4211 and serves WebSocket on 8787.
- **Web UI state sync**: sliders and wave buttons populate from first telemetry packet on connect.

## Stability fixes applied this session

- `DELTA_BLOCK_SIZE` reduced 2048 → 256 (saves ~35 KB heap); NULL guard in `deltas_pool_alloc` prevents crash when malloc fails.
- `amy_execute_deltas()` called before every `amy_add_event()` so the delta pool drains even without a USB host connected.
- `touch_telem` task stack raised to 6 KB; `touch_cmd` to 4 KB — prevents stack overflow corrupting adjacent touch channel heap objects (was causing `LoadProhibited` in the touch ISR).

## Repository structure

```text
Synthewi/
├── main/
│   ├── main.c               # App entry, touch/synth callback wiring
│   ├── touch_telemetry.c/h  # UDP telemetry + command tasks, v2 protocol
│   └── wifi_manager.c/h     # WiFi STA connection
├── components/
│   ├── amy_engine/          # AMY wrapper: note on/off, FX, NVS persistence
│   └── touch_control/       # ESP-IDF touch sensor driver abstraction
├── third_party/amy/         # AMY synth engine (submodule, patched for ESP32-S3)
├── bridge/
│   ├── src/index.ts         # Node.js UDP↔WebSocket bridge source
│   └── dist/index.js        # Compiled bridge (run with: node dist/index.js)
├── web-ui/
│   ├── src/ui/app.ts        # Browser UI source
│   └── dist/                # Built web UI (served by bridge or any static host)
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

## Next steps

- **Velocity response**: map touch delta magnitude to AMY amplitude or filter envelope depth. Requires threshold auto-calibration per pad.
- **Per-pad note mapping**: configurable MIDI note per pad (currently hardcoded to a fixed scale). UI control or NVS profile.
- **Polyphony**: AMY supports multiple oscillators — allow simultaneous notes from all four pads at once (currently each pad steals its oscillator).
- **USB MIDI mode**: expose a USB MIDI interface alongside (or instead of) UAC so the device can drive a DAW.
- **More pads**: `SYNTH_PAD_COUNT` and `TOUCH_CHANNEL_COUNT` are the only constants to raise; hardware allows up to 14 capacitive channels on ESP32-S3.
- **Enclosure and hardware BOM**: prototype physical case, document pad materials, wiring lengths, and grounding for reproducible builds.
- **Latency measurement**: end-to-end touch-to-audio timing. Current path: touch ISR → FreeRTOS task → AMY delta → USB audio callback. Target < 20 ms.
- **Preset save/load**: named NVS profiles for different tuning configurations (thresholds, wave, FX).
