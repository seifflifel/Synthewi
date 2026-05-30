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
| `hardware/amy-i2s-notes` | AMY synthesizer → I²S — plays SINE notes C4 E4 G4 C5 in a loop | `git checkout hardware/amy-i2s-notes && idf.py flash` |

---

## Current milestone — AMY synthesizer over I²S

### What this firmware does

Runs the AMY polyphonic synthesizer engine on ESP32-S3 and outputs audio through
the MAX98357A Class D amplifier via I²S. `main.c` loops through four SINE wave notes
(C4 → E4 → G4 → C5) at 500 ms each, confirming the full audio path: CPU → AMY → I²S → speaker.

### Key findings (this milestone)

- AMY's `freq_coefs[COEF_CONST]` (Hz) must be used for direct-osc frequency on direct-osc
  events (`e.osc = n`). The `e.midi_note` field targets the voice-allocator path (`e.synth`)
  and is silent on a bare osc — this was the root cause of no audio.
- Pan defaults to 0 (left channel only) in AMY. Always set `pan_coefs[COEF_CONST] = 0.5f`
  for center output when using mono speakers.
- Reverb and echo delay lines exhaust SRAM after WiFi init fragments the heap — both disabled.
- `features.default_synths = 0` is required; default synths would claim oscs 0–N and
  conflict with direct-osc events on those indices.

### How a note event works (bleep pattern)

```c
amy_event e = amy_default_event();
e.osc                    = pad;           // osc index 0-7, one per touch pad
e.wave                   = SINE;
e.freq_coefs[COEF_CONST] = 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
e.pan_coefs[COEF_CONST]  = 0.5f;         // center; default is left-only
e.velocity               = 1.0f;         // triggers note_on
amy_add_event(&e);
```

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

---

## Next milestones (planned)

- [ ] Touch pads → live note_on / note_off (MPR121 or direct capacitive)
- [ ] Rotary encoder → parameter control
- [ ] OLED (SSD1306, I²C) — display synthesis state
- [ ] Full ADSR envelope + filter layered back onto confirmed audio path
- [ ] UI architecture

---

## Build and flash

```powershell
idf.py build
idf.py -p COM5 flash monitor
```

No bridge or web UI needed — this branch is standalone firmware only.
