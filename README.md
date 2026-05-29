# Synthewi — Hardware Prototyping Branch

This branch documents the step-by-step hardware bring-up of the final Synthewi instrument.
Each milestone is tagged so it can be flashed independently as a reference.

For the full working software prototype (USB audio + WiFi web UI), see branch
`checkpoint/usb-audio` or flash tag `firmware/wifi-usb`.

---

## Hardware Tags (flash reference points)

| Tag | What it tests | How to flash |
|-----|--------------|--------------|
| `hardware/speaker-wav-test` | I²S + MAX98357A + speaker — plays embedded WAV loop | `git checkout hardware/speaker-wav-test && idf.py flash` |

---

## Current milestone — Speaker / I²S output

### What this firmware does

Plays the embedded `sleepwalk.wav` (44100 Hz mono) in a continuous loop through the
MAX98357A Class D amplifier via I²S. No WiFi, no touch, no synthesis — pure audio
transport test.

### Wiring

| ESP32-S3 GPIO | MAX98357A pin | Notes |
|---------------|--------------|-------|
| GPIO38 | BCLK | I²S bit clock |
| GPIO39 | LRC | I²S left/right clock (word select) |
| GPIO40 | DIN | I²S data |
| 5V header pin | VIN | Power |
| GND | GND | Ground |
| — | SD | Leave floating (board pull-up enables amp) |
| — | GAIN | Leave floating = 9 dB gain |
| OUT+ | Speaker + | Direct wire, no resistors |
| OUT– | Speaker – | Direct wire, no resistors |

### I²S format

32-bit MSB-justified stereo at 48 kHz. Each int16 audio sample is left-shifted 16 bits
into an int32 word and duplicated to both L and R channels. This matches AMY's proven
ESP32-S3 I²S format and is correctly decoded by the MAX98357A.

### Key findings

- `I2S_SLOT_MODE_MONO` breaks the LRCLK framing the MAX98357A expects — must use stereo.
- MSB 32-bit format (`I2S_STD_MSB_SLOT_DEFAULT_CONFIG`) works; Philips 16-bit also
  accepted by the MAX98357A but not tested on this rig.
- GAIN pin to GND = 3 dB (very quiet on laptop speakers). Leave floating for 9 dB.
- `OUTPUT_GAIN_BOOST 1` (no software boost) is the right setting with GAIN floating.
- GPIO38/39/40 are clean on ESP32-S3-DevKitC-1 N8R2 — no conflicts with JTAG or PSRAM.

---

## Next milestones (planned)

- [ ] OLED (SSD1306, I²C) — display test
- [ ] Potentiometer (ADC read → parameter) — single pot on GPIO3
- [ ] Full synthesis via I²S (port WiFi firmware audio engine to this transport)
- [ ] Physical controls (encoder, toggle switch, buttons)

---

## Build and flash

```powershell
idf.py build
idf.py -p COM5 flash monitor
```

No bridge or web UI needed — this branch is standalone firmware only.
