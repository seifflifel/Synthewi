# Synthewi Setup Guide (Current State)

This guide reflects the current repository direction: touch input on ESP32-S3 with USB MIDI output.

## Current Stack

- `components/touch_control/`: touch sensing and conditioning
- `components/usb_midi/`: TinyUSB MIDI transport
- `main/main.c`: 4-pad touch gate loop and MIDI Note On/Off logic

## Hardware Mapping (Current Firmware)

- Pad 1 -> GPIO4 -> MIDI 60 (C4)
- Pad 2 -> GPIO5 -> MIDI 62 (D4)
- Pad 3 -> GPIO6 -> MIDI 64 (E4)
- Pad 4 -> GPIO7 -> MIDI 65 (F4)

## Build, Flash, Monitor

```powershell
. "C:\esp\v5.5.3\esp-idf\export.ps1"
cd "c:\Users\Seifo\Documents\Study 2025\PPP\Synth\Synthewi"

idf.py build
idf.py -p COM5 flash
idf.py -p COM5 monitor
```

## Flash vs MIDI Device Selection

- Flash and serial monitor use `COMx` (`USB Serial JTAG`).
- Performance/MIDI use the USB MIDI device exposed by TinyUSB.
- In DAW/MIDI software, choose the MIDI input device (for example `Synthewi MIDI`/`Touch MIDI`), not the COM port.

## Runtime Behavior Snapshot

- 4 touch channels sampled continuously
- Manual hysteresis gate using:
  - `TOUCH_MANUAL_ON_DELTA`
  - `TOUCH_MANUAL_OFF_DELTA`
- Note On velocity is fixed (`TOUCH_NOTE_VELOCITY`)
- Note Off velocity is fixed (`64`)

## Tuning

Tune in `main/main.c`:

- `TOUCH_MANUAL_ON_DELTA`
- `TOUCH_MANUAL_OFF_DELTA`
- `TOUCH_LOOP_MS`
- `TOUCH_SCOPE_MODE` (`1` for raw/baseline/delta observation)

## Known Direction

- Prioritize stable triggering and repeatable hardware behavior first
- Reintroduce expressive velocity/CC behavior in next iterations

## References

- `README.md`
- `FIRST_CONNECTION.md`
- [ESP-IDF (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)
