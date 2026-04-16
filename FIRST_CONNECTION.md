# Synthewi - First Connection Guide (Current Firmware State)

This guide matches the current firmware behavior in this repository.

## What You Have

- ESP32-S3 firmware that sends USB MIDI Note On/Off
- 4 touch pads mapped to notes C4, D4, E4, F4
- Manual threshold gate for stable triggering

## Important Port Flow (Flash vs Use)

The workflow is:

1. Use COM port (`USB Serial/JTAG`) for flashing and optional serial monitoring
2. After flashing, use the USB MIDI device in your DAW/MIDI app for performance

In short: flash on COM, play on USB MIDI.

---

## STEP 1: Prepare Hardware

Use 4 conductive pads (copper tape recommended) and wire them to the touch GPIOs used by current firmware.

Current mapping in firmware:

- Pad 1 -> GPIO4 -> MIDI 60 (C4)
- Pad 2 -> GPIO5 -> MIDI 62 (D4)
- Pad 3 -> GPIO6 -> MIDI 64 (E4)
- Pad 4 -> GPIO7 -> MIDI 65 (F4)

Also ensure a solid ground reference based on your pad construction.

---

## STEP 2: Flash via COM Port

### 2A. Connect Device

1. Connect ESP32-S3 to PC using data-capable USB cable
2. Open Device Manager -> Ports (COM & LPT)
3. Note the `USB Serial JTAG` COM port (example: `COM5`)

### 2B. Flash

```powershell
. "C:\esp\v5.5.3\esp-idf\export.ps1"
cd "c:\Users\Seifo\Documents\Study 2025\PPP\Synth\Synthewi"

# Replace COM5 with your detected port
idf.py -p COM5 flash
```

Optional log check:

```powershell
idf.py -p COM5 monitor
```

Expected startup log includes lines similar to:

- `Synthewi - Expressive Touch MIDI Controller`
- `Initializing TinyUSB MIDI device...`
- `Touch channel 4 initialized (delta=80)` ... up to channel 7

Press `Ctrl+C` to stop monitor.

---

## STEP 3: Switch to USB MIDI Device for Playing

After flash, your DAW or MIDI tool should use the USB MIDI interface (not COM).

In MIDI apps/DAWs, look for a MIDI input such as:

- `Synthewi MIDI`
- `Touch MIDI`
- or the ESP32-S3 USB MIDI device name shown by your OS

If you still have serial monitor open on COM, close it before normal MIDI testing.

---

## STEP 4: Quick MIDI Test

1. Arm a MIDI track in your DAW
2. Select the ESP32-S3 USB MIDI input device
3. Touch each pad

You should receive Note On/Off for:

- 60 (C4)
- 62 (D4)
- 64 (E4)
- 65 (F4)

Current firmware sends fixed Note On velocity (`100`) and Note Off velocity (`64`).

---

## Troubleshooting

### Flash fails

- Re-check COM port in Device Manager
- Use a data USB cable (not charge-only)
- Re-run ESP-IDF export script before flashing

### No MIDI in DAW

- Confirm DAW input is USB MIDI device, not COM
- Replug USB and reselect MIDI input
- Close any app locking serial/COM monitoring

### Touch not triggering notes

- Verify wiring on GPIO4/5/6/7
- Improve pad conductivity 
- Tune thresholds in `main/main.c`:
  - `TOUCH_MANUAL_ON_DELTA`
  - `TOUCH_MANUAL_OFF_DELTA`

---

## Next Iteration Ideas

1. Add startup calibration helper for threshold suggestions
2. Add configurable note mapping profile
3. Reintroduce expressive CC/velocity behavior after baseline stability is confirmed

You are now aligned with the current code state and ready to iterate safely.

