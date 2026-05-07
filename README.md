# Synthewi USB Audio Checkpoint

Current checkpoint for the USB audio branch of Synthewi.

This branch is being prepared as a GitHub checkpoint for the USB audio implementation. It does not overwrite the repository history by itself; pushing commits will update the selected branch tip on GitHub.

## What was tested

- ESP32-S3 enumerates as a USB Audio Class device on the host.
- The USB mic transport path works with a generated WAV source.
- `sounds/sleepwalk.wav` was embedded and streamed through the USB audio callback.
- The result was acceptable for transport validation, with some clipping from the current gain boost.

## Current state

- USB transport is the part we trust at this checkpoint.
- The embedded WAV source is exposed through `components/amy_engine/include/amy_engine.h`.
- The app uses a custom partition table in `partitions.csv` with a 2 MB factory app slot.

## TODO - Next Implementations and Iterations

- Build a Web UI + WebSocket control panel to tune thresholds live
- Add automatic threshold calibration routine aimed at enabling velocity response
- Reintroduce expressive MIDI behavior (velocity from touch delta and optional held-note CC)
- Implement a non-MIDI mode where ESP32-S3 directly drives an onboard sound engine
- Explore Moozi and AMY as candidate synth engines for onboard sound generation
- Implement USB Audio Class (UAC) mode so ESP32-S3 can appear as a USB sound device
- Define USB mode strategy (MIDI only, UAC only, or selectable profile at boot/runtime)
- Add per-pad note mapping and optional MIDI channel configuration
- Add profile save/load for tuning presets (thresholds, notes, sensitivity)
- Create a repeatable latency and stability test plan (touch-to-note timing, jitter, dropouts)
- Validate behavior across different pad materials, sizes, wiring lengths, and grounding setups
- Prototype and iterate the physical case/enclosure
- Document a known-good hardware BOM and assembly notes for reproducible builds

## Repository Structure

```text
Synthewi/
|- main/main.c                     # USB audio app loop and callback source
|- components/touch_control/       # Touch sensing and signal processing
|- components/usb_midi/            # TinyUSB MIDI transport
|- components/amy_engine/          # AMY wrapper and embedded WAV playback
|- sounds/                         # Tracked audio assets for checkpoint testing
|- FIRST_CONNECTION.md             # Practical bring-up checklist
|- SETUP_GUIDE.md                  # Setup details
|- README.md                       # This checkpoint document
```

## Reference Docs

- [FIRST_CONNECTION.md](FIRST_CONNECTION.md)
- [SETUP_GUIDE.md](SETUP_GUIDE.md)
- [ESP-IDF (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)
- [MIDI Note Reference](https://en.wikipedia.org/wiki/MIDI_note)

## Build and flash

```powershell
idf.py build
idf.py -p COM5 flash
idf.py -p COM5 monitor
```

## Next phase

Touch -> AMY -> USB audio integration.

The goal of the next branch step is to bring touch control back as the input source, feed AMY, and keep the same working USB audio transport path.
