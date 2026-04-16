# Synthewi - ESP32-S3 Touch to USB MIDI

Current firmware state for this repository checkpoint.

This README is intentionally written as a "resume point" before pushing to GitHub, so future-you can come back and continue from exactly this stage.

## Project Direction (Current)

Synthewi is currently focused on a stable core pipeline:

1. Read 4 ESP32-S3 touch channels
2. Apply manual threshold gating
3. Send USB MIDI Note On/Off events reliably
4. Tune hardware and thresholds before adding richer expression logic

At this checkpoint, reliability and calibration are prioritized over advanced musical features.

## What Is Working Right Now

- ESP32-S3 project builds and flashes with ESP-IDF
- TinyUSB MIDI device initializes and enumerates on host
- 4 touch inputs are sampled continuously
- Manual hysteresis thresholding prevents rapid note chatter
- MIDI Note On/Off is sent per pad

## Current Runtime Configuration

Defined in `main/main.c`:

- `NUM_TOUCH_PADS = 4`
- `TOUCH_SCOPE_MODE = 0` (normal MIDI mode)
- `TOUCH_LOOP_MS = 20` (50 Hz loop)
- `TOUCH_MANUAL_ON_DELTA = 11000`
- `TOUCH_MANUAL_OFF_DELTA = 9000`
- `TOUCH_NOTE_VELOCITY = 100`

Touch channels currently used:

- ch4, ch5, ch6, ch7

MIDI notes currently mapped:

- 60 (C4), 62 (D4), 64 (E4), 65 (F4)

## Important Notes About This Stage

- The app is currently using fixed velocity for Note On.
- Expression CC streaming is not active in the current main loop.
- Scope/tuning mode exists (`TOUCH_SCOPE_MODE = 1`) for raw/baseline/delta observation.
- `touch_control` already contains adaptive logic helpers, but the current app path uses a fixed manual gate for predictable behavior.

## Quick Start (Resume Workflow)

1. Open ESP-IDF PowerShell environment
2. Build the project
3. Flash to the ESP32-S3
4. Monitor logs and verify thresholds

Example commands:

```powershell
. "C:\esp\v5.5.3\esp-idf\export.ps1"
cd "c:\Users\Seifo\Documents\Study 2025\PPP\Synth\Synthewi"

idf.py build
idf.py -p COM5 flash
idf.py -p COM5 monitor
```

## TODO - Next Implementations and Iterations

- [ ] Build a Web UI + WebSocket control panel to tune thresholds live
- [ ] Add automatic threshold calibration routine aimed at enabling velocity response
- [ ] Reintroduce expressive MIDI behavior (velocity from touch delta and optional held-note CC)
- [ ] Implement a non-MIDI mode where ESP32-S3 directly drives an onboard sound engine
- [ ] Explore Moozi and AMY as candidate synth engines for onboard sound generation
- [ ] Implement USB Audio Class (UAC) mode so ESP32-S3 can appear as a USB sound device
- [ ] Define USB mode strategy (MIDI only, UAC only, or selectable profile at boot/runtime)
- [ ] Add per-pad note mapping and optional MIDI channel configuration
- [ ] Add profile save/load for tuning presets (thresholds, notes, sensitivity)
- [ ] Create a repeatable latency and stability test plan (touch-to-note timing, jitter, dropouts)
- [ ] Validate behavior across different pad materials, sizes, wiring lengths, and grounding setups
- [ ] Prototype and iterate the physical case/enclosure
- [ ] Document a known-good hardware BOM and assembly notes for reproducible builds

## Repository Structure

```text
Synthewi/
|- main/main.c                     # App loop and MIDI gate logic
|- components/touch_control/       # Touch sensing and signal processing
|- components/usb_midi/            # TinyUSB MIDI transport
|- FIRST_CONNECTION.md             # Practical bring-up checklist
|- SETUP_GUIDE.md                  # Setup details
|- README.md                       # This checkpoint document
```

## Reference Docs

- [FIRST_CONNECTION.md](FIRST_CONNECTION.md)
- [SETUP_GUIDE.md](SETUP_GUIDE.md)
- [ESP-IDF (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)
- [MIDI Note Reference](https://en.wikipedia.org/wiki/MIDI_note)

