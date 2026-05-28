# Synthewi — Final Hardware Brainstorm

Session: 2026-05-26. Prototype is stable. This document covers the delta to a finished standalone instrument.

---

## Current State

What works well on the prototype:
- 8 touch pads, pentatonic scale, CUSTOM + PATCH (Juno/DX7) modes
- Filter, attack/release envelope, LFO, pressure-to-filter, glide — all working
- Threshold persistence in NVS

What is prototype-only and will be replaced:
- **USB audio** → replaced by I²S DAC + Class D amp driving the laptop speakers
- **WiFi web UI** → replaced by physical controls (pots, switches, encoder, OLED)
- **Software threshold tuning** → startup auto-calibration (Phase 7)

---

## Software: Missing Fun Features

### 1. Octave Shift (momentary buttons)

Two physical buttons: **OCT–** and **OCT+**. Momentary (held = active, released = return).
While held, all pad MIDI notes shift ±12. This makes the instrument range: C2–E7 with ±2 octaves.

Implementation: shift `s_pad_notes[i]` by ±12 while button is pressed. Very cheap.

---

### 2. Full ADSR (add Decay + Sustain)

Currently: Attack + Release only. AMY supports full ADSR via its breakpoint envelope.

| Stage | Current | Missing |
|-------|---------|---------|
| Attack | ✅ `bp0_time` | — |
| Decay | ❌ implicit 0 | `bp1_time`: time to fall from peak to sustain level |
| Sustain | ❌ implicit 1.0 | `bp1_amp`: level held while pad is pressed |
| Release | ✅ `bp2_time` | — |

2 new pots or sliders. Decay makes plucked sounds possible (short attack, fast decay, low sustain).
Sustain makes organs / pads possible (full sustain level).

---

### 3. Vibrato

Two candidate implementations — decide after testing the physical rig:

**Option A — Hall effect sensor (preferred for expressiveness)**
- Attach a small magnet to the underside of each pad (or the pad's foam/spring base)
- Place a Hall effect sensor (e.g. SS49E, linear output, 3.3V compatible) below each pad
- Wiggling the pad side-to-side produces an analog voltage proportional to lateral magnet displacement
- Route this to an ESP32-S3 ADC channel → modulate pitch (LFO pitch target on that pad's osc)
- Requires: 8 Hall sensors + 8 analog pins (or a multiplexer, e.g. CD4051, 1 ADC + 3 GPIO)
- Hardware cost: ~€2–5 total

**Option B — Pressure-triggered (no extra hardware)**
- Already have `pressure_norm` per pad while held
- Above a threshold (e.g. press > 80%), activate pitch LFO on that pad's osc
- Same gesture as filter-open → conflicts slightly, but musically intuitive
- Zero hardware cost

Leave as an open decision until enclosure prototype is testable.

---

### 4. Scale Selection (rotary encoder + OLED)

The rotary encoder browses scale presets. OLED shows the current scale name + root note.

Candidate scales:
| Name | Intervals | Feel |
|------|-----------|------|
| Pentatonic Major | 0 2 4 7 9 | Current default, upbeat |
| Pentatonic Minor | 0 3 5 7 10 | Blues/soul |
| Blues | 0 3 5 6 7 10 | Gritty |
| Major | 0 2 4 5 7 9 11 | Classical |
| Natural Minor | 0 2 3 5 7 8 10 | Melancholic |
| Dorian | 0 2 3 5 7 9 10 | Jazz/funk |
| Chromatic | all | Free, experimental |

8 pads get the first 8 notes of the selected scale, starting from a root note.
Root note also selectable via encoder push + rotate.

Implementation: lookup table in `amy_engine.c`, recalculate `s_pad_notes[]` on change.

---

### 5. Chord Mode

When enabled, a single pad press fires a triad instead of a single note.
Each pad plays its root note + major third (+4 semitones) + perfect fifth (+7 semitones).

Implementation concern: currently 1 audio osc per pad. Triads need 3 oscs per pad.

**Options:**
- **Drop LFO in chord mode** — reuse the 8 LFO osc slots (indices 8–15) as the chord upper voices. This gives: 8 root oscs + 8 fifth oscs + no LFO. Only 2-note chords (dyads), not triads.
- **Increase `max_oscs` to 24** — 8 root + 8 third + 8 fifth. Test if AMY + WiFi heap allows it (may not on prototype; final hardware likely OK with freed memory).
- **Chord mode only in PATCH mode** — AMY's voice allocator handles it naturally with no osc count change.

Recommendation: start with PATCH mode chord (free), then test 24-osc CUSTOM mode on final hardware.

Toggle via DIP switch or OLED menu.

---

### 6. Arpeggiator

Hold one or more pads → ESP cycles through the held notes at a rhythmic tempo.

- **Rate**: controlled by a pot (BPM 40–240) or synced to a tap-tempo button
- **Pattern**: ascending, descending, random (selectable via OLED menu)
- **Gate**: note held for 50% of step duration (punchy), adjustable
- CPU cost: a single FreeRTOS timer task at BPM intervals calling `note_on`/`note_off`. Negligible.
- Works in both CUSTOM and PATCH mode

State: track which pads are currently held (already available in `s_prev_touching[]`). Arp timer cycles through the held-pad list.

---

### 7. Hold Mode

Notes continue to ring after finger is lifted — like a sustain pedal. Second press releases.
Enables playing ambient textures: press multiple pads, lift fingers, let them wash.

Toggle: a physical button or DIP switch position.
Implementation: gate the `note_off` call in `touch_cmd_task` when hold mode is active.

---

## Hardware Stack

### Audio Output

**Module: MAX98357A** (one per speaker, so ×2 for stereo)

- Accepts I²S directly from ESP32-S3 (BCLK, LRCLK, DIN)
- Built-in Class D amplifier, 3W into 4Ω
- HP Victus 16-E laptop speakers are typically 4Ω, ~2W — well within spec
- **No resistors needed** on the speaker connection — direct wire from module OUT+/OUT– to speaker terminals
- GAIN pin sets output level: leave floating = 9 dB (default), tie to GND = 3 dB, tie to VCC = 12 dB, tie to VCC via 100 kΩ = 15 dB
- SD pin: leave floating (always on), or tie to a GPIO for mute control
- Add a 10 µF + 100 nF decoupling cap on the 5V supply rail
- Available as ready-made breakout boards for ~€2–3 each on AliExpress / Mouser

**Firmware change**: audio output must switch from USB UAC to I²S. In `main.c`, replace the USB audio init with `i2s_driver_install()` and the DMA write path. AMY already renders to a buffer; just change the sink.

---

### Potentiometers (~6 pots)

**Type: 10 kΩ linear taper, 6mm shaft, panel-mount**  
Alps RK09L or Bourns PTV09 series — long-life, smooth, widely used in synths.

| Pot | Parameter | Range |
|-----|-----------|-------|
| 1 | Filter cutoff | 20 Hz – 8 kHz |
| 2 | Filter resonance | 0–100% |
| 3 | LFO rate | 0.1–20 Hz |
| 4 | Env attack | 1–2000 ms |
| 5 | Env release / decay | 10–4000 ms |
| 6 | Pressure depth (or glide) | 0–8 kHz / 0–500 ms |

Read via ADC: ESP32-S3 has 20 ADC channels. Wire each pot: one outer pin to 3.3V, other outer pin to GND, wiper to ADC input. Add 100 nF cap from wiper to GND for noise filtering.

---

### Physical Controls

| Component | Part | Purpose |
|-----------|------|---------|
| OLED | 0.96" SSD1306, 128×64, I²C | Scale name, patch number, arp mode, BPM |
| Rotary encoder | EC11 (with push button) | Browse scales / patches / params |
| DIP switch 2-pin | Interrupteur DIP 2 broches 2.54 mm | Mode: CUSTOM vs PATCH (or analog vs MIDI) |
| Toggle switch | MTS-203, 3-position, 6-pin | Oscillator type: SINE / SAW / SQUARE |
| Momentary buttons ×2 | 12mm tactile or metal cap | OCT– / OCT+ |
| Momentary button ×1 | 12mm tactile | Tap-tempo (for arpeggiator) — optional |

The 3-position toggle replacing the oscillator type buttons is a great UX choice — instantaneous and tactile.

---

### Touch Pads

**Material: aluminum adhesive tape**
- Confirmed: copper saturates the raw capacitance value; aluminum gives a better dynamic range
- Pad dimensions: to match 3D printed bays (suggest 30×50 mm per pad, 5 mm gap between pads)
- Attach to bottom face of the top panel (tape inside, finger touches through a thin plastic skin or directly)
- Or: expose the tape on the surface with a thin protective lacquer layer
- Each pad connects to the ESP32-S3 touch channel pin via a short wire — keep traces < 10 cm to minimize stray capacitance

---

### Enclosure (3D printed)

**Material: White PLA**

Key dimensions to design around:
- 8 pads in a row, ~30×50 mm each with 5 mm gap = ~300 mm total width minimum
- Speaker grille(s) on top or angled front face
- 6 pot holes: 7 mm diameter for 6 mm shaft, panel-mount
- 1× OLED window
- 1× encoder hole
- 1× DIP switch recess
- 1× 3-position toggle hole
- 2× OCT button holes
- Power input (USB-C or barrel jack 5V)
- On/off switch

Consider a two-piece design: bottom tray (holds PCB/wiring) + top panel (pads, controls, speakers). 

---

## Open Decisions

| # | Question | Options | Status |
|---|----------|---------|--------|
| 1 | Vibrato source | Hall effect sensor vs pressure gesture | Decide on physical rig |
| 2 | Chord mode implementation | PATCH-only vs 24-osc CUSTOM | Test max_oscs on final hardware |
| 3 | Arp rate control | Dedicated pot vs OLED + encoder | Decide on enclosure layout |
| 4 | Hold mode trigger | DIP switch vs button toggle | Layout decision |
| 5 | MIDI out | DIN-5 jack or TRS-A 3.5mm | Only if MIDI mode is used |
| 6 | Power supply | USB-C PD (5V 3A) vs 9V barrel + LDO | Depends on amp headroom needed |

---

---

## Wiring Diagram

### Block Diagram

```
                         ┌─────────────────────────────────────────────┐
  USB-C 5V 2A ──────────►│  5V rail                                    │
                         │                                             │
                         │  ┌─────────────────────────────────────┐   │
                         │  │        ESP32-S3-DevKitC-1           │   │
                         │  │              (N8R2)                  │   │
                         │  │                                     │   │
     ┌───────────────────┼──┤ I²S BCLK  ──────────────────────┐  │   │
     │   ┌───────────────┼──┤ I²S LRCLK ─────────────────┐   │  │   │
     │   │   ┌───────────┼──┤ I²S DOUT  ─────────────┐   │   │  │   │
     │   │   │           │  │                         │   │   │  │   │
     │   │   │           │  ├─ I²C SDA ───────────────┼───┼───┼──┼───┼──► OLED SSD1306
     │   │   │           │  ├─ I²C SCL ───────────────┼───┼───┼──┼───┼──► OLED SSD1306
     │   │   │           │  │                         │   │   │  │   │
     │   │   │           │  ├─ TOUCH CH4 (GPIO4) ─────┼───┼───┼──┼───┼──► PAD 0
     │   │   │           │  ├─ TOUCH CH5 (GPIO5) ─────┼───┼───┼──┼───┼──► PAD 1
     │   │   │           │  ├─ TOUCH CH6 (GPIO6) ─────┼───┼───┼──┼───┼──► PAD 2
     │   │   │           │  ├─ TOUCH CH7 (GPIO7) ─────┼───┼───┼──┼───┼──► PAD 3
     │   │   │           │  ├─ TOUCH CH8 (GPIO8) ─────┼───┼───┼──┼───┼──► PAD 4
     │   │   │           │  ├─ TOUCH CH12(GPIO12)─────┼───┼───┼──┼───┼──► PAD 5
     │   │   │           │  ├─ TOUCH CH1 (GPIO1) ─────┼───┼───┼──┼───┼──► PAD 6
     │   │   │           │  ├─ TOUCH CH2 (GPIO2) ─────┼───┼───┼──┼───┼──► PAD 7
     │   │   │           │  │                         │   │   │  │   │
     │   │   │           │  ├─ ADC (GPIO3)  ──────────┼───┼───┼──┼───┼──► POT 1  filter cutoff
     │   │   │           │  ├─ ADC (GPIO9)  ──────────┼───┼───┼──┼───┼──► POT 2  resonance
     │   │   │           │  ├─ ADC (GPIO10) ──────────┼───┼───┼──┼───┼──► POT 3  LFO rate
     │   │   │           │  ├─ ADC (GPIO11) ──────────┼───┼───┼──┼───┼──► POT 4  attack
     │   │   │           │  ├─ ADC (GPIO13) ──────────┼───┼───┼──┼───┼──► POT 5  release
     │   │   │           │  ├─ ADC (GPIO14) ──────────┼───┼───┼──┼───┼──► POT 6  pressure/glide
     │   │   │           │  │                         │   │   │  │   │
     │   │   │           │  ├─ GPIO15 ────────────────┼───┼───┼──┼───┼──► EC11 CLK
     │   │   │           │  ├─ GPIO16 ────────────────┼───┼───┼──┼───┼──► EC11 DT
     │   │   │           │  ├─ GPIO17 ────────────────┼───┼───┼──┼───┼──► EC11 SW (push)
     │   │   │           │  │                         │   │   │  │   │
     │   │   │           │  ├─ GPIO18 ────────────────┼───┼───┼──┼───┼──► DIP pin A  (mode)
     │   │   │           │  │                         │   │   │  │   │
     │   │   │           │  ├─ GPIO19 ────────────────┼───┼───┼──┼───┼──► MTS-203 pos A (SINE)
     │   │   │           │  ├─ GPIO20 ────────────────┼───┼───┼──┼───┼──► MTS-203 pos B (SQUARE)
     │   │   │           │  │         center → GND   │   │   │  │   │    (center = SAW = both LOW)
     │   │   │           │  │                         │   │   │  │   │
     │   │   │           │  ├─ GPIO21 ────────────────┼───┼───┼──┼───┼──► OCT– button → GND
     │   │   │           │  ├─ GPIO47 ────────────────┼───┼───┼──┼───┼──► OCT+ button → GND
     │   │   │           │  │                         │   │   │  │   │
     │   │   │           │  └─────────────────────────┘   │   │  │   │
     │   │   │           └─────────────────────────────────┘   │  │   │
     │   │   └─────────────────────────────────────────────────┘  │   │
     │   │                                                         │   │
     │   │                                                         │   │
     ▼   ▼   ▼                                                     │   │
 ┌──────────────┐   GAIN/SD → GND (left channel)                  │   │
 │ MAX98357A  L │◄──────────────────────────────────────────────────┘   │
 │  (Adafruit   │  5V from rail                                         │
 │   #3006)     │                                                        │
 │  OUT+ / OUT–─┼──────────────────► SPEAKER LEFT (4Ω)                 │
 └──────────────┘                                                        │
                                                                         │
 ┌──────────────┐   GAIN/SD → VDD (right channel)                       │
 │ MAX98357A  R │◄───────────────────────────────────────────────────────┘
 │  (Adafruit   │  5V from rail
 │   #3006)     │
 │  OUT+ / OUT–─┼──────────────────► SPEAKER RIGHT (4Ω)
 └──────────────┘
```

> **GPIO note**: GPIO assignments above are a first draft — do a full pin conflict check
> before layout. On N8R2, the Quad PSRAM shares the internal flash SPI bus (GPIO26–32,
> not exposed on headers), so no extra GPIO pins are consumed — all header pins are free.
> ADC2 (GPIO11–20) is fine on final hardware since WiFi is disabled.
> Touch channel GPIOs (1,2,4,5,6,7,8,12) must not be shared with ADC reads.

---

### Potentiometer Wiring (each pot identical)

```
  3.3V ──┬──────────────────────────┐
         │                          │ (outer pins)
         [POT]
         │
         ├──────► ADC GPIO          (wiper, center pin)
         │
        [100nF]                      (noise cap, wiper to GND)
         │
  GND  ──┴──────────────────────────┘
```

---

### MTS-203 Toggle (3 positions → oscillator type)

```
   MTS-203 pin layout (front view):
   ┌─────────────────┐
   │  1    2    3    │  ← pole A: throw1 / common / throw2
   │                 │
   │  4    5    6    │  ← pole B (unused or second function)
   └─────────────────┘

   Wiring for oscillator select:
   pin 2 (common A) → GND
   pin 1 (throw A1) → GPIO19 (with 10kΩ pull-up to 3.3V)
   pin 3 (throw A2) → GPIO20 (with 10kΩ pull-up to 3.3V)

   Position 1 (up)   : GPIO19=LOW,  GPIO20=HIGH → SINE
   Position 2 (center): GPIO19=HIGH, GPIO20=HIGH → SAW
   Position 3 (down) : GPIO19=HIGH, GPIO20=LOW  → SQUARE
```

---

### Enclosure Top-Panel Layout (birds-eye)

```
 ┌────────────────────────────────────────────────────────────────────┐
 │                                                                    │
 │  [OLED 26×27]  [EC11]     [DIP]   [OCT–] [OCT+]   [MTS-203]        │
 │                                                                    │
 │  [POT1] [POT2] [POT3]           [POT4]  [POT5]  [POT6]             │
 │  cutoff  reso  LFO rate          attack  release  pres/glide       │
 │                                                                    │
 │  ┌──────┐┌──────┐┌──────┐┌──────┐  ┌──────┐┌──────┐┌──────┐┌──────┐│
 │  │ PAD 0││ PAD 1││ PAD 2││ PAD 3│  │ PAD 4││ PAD 5││ PAD 6││ PAD 7││
 │  │  C4  ││  D4  ││  E4  ││  G4  │  │  A4  ││  C5  ││  D5  ││  E5  ││
 │  └──────┘└──────┘└──────┘└──────┘  └──────┘└──────┘└──────┘└──────┘│
 │                                                                    │
 │  ◄──── SPEAKER LEFT ────────────────── SPEAKER RIGHT ────────►     │
 └────────────────────────────────────────────────────────────────────┘
                                                        ↑ USB-C power
```

---

### Module Dimensions Reference (for 3D model)

| Module | Reference | PCB size | Panel hole | 3D model |
|--------|-----------|----------|------------|----------|
| ESP32-S3 | ESP32-S3-DevKitC-1 (N8R2) | 68.4 × 25.4 mm | — (internal mount) | Espressif GitHub hardware repo |
| I²S amp | MAX98357A (Adafruit #3006) | 23 × 17.6 mm | — (internal mount) | GrabCAD: "MAX98357A" |
| OLED | SSD1306 0.96" 128×64 4-pin | 26 × 27.3 mm | 24 × 12 mm window | GrabCAD: "SSD1306 OLED" |
| Encoder | EC11 with push, 15mm shaft | 13 × 13 mm body | ⌀ 7 mm | GrabCAD: "EC11 rotary encoder" |
| Pot | Alps RK09L or Bourns PTV09A | 9 × 9 mm body | ⌀ 7 mm | GrabCAD: "Alps RK09L" |
| Toggle | MTS-203 3-pos 6-pin | 13 × 13 mm body | ⌀ 6 mm | GrabCAD: "MTS-203 toggle switch" |
| DIP sw | DIP switch 2-pos 2.54mm | 7 × 5 mm | — (panel recess) | GrabCAD: "DIP switch 2 position" |
| Buttons | 12mm tactile + metal cap | 12 × 12 mm | ⌀ 12 mm | GrabCAD: "12mm tactile button" |
| Speakers | HP Victus 16-E (salvaged) | measure yours | grille cutout | — |

---

## Implementation Order (after hardware is built)
1. Full ADSR (2 new params: decay + sustain)
1. Switch audio output from USB to I²S → MAX98357A (prerequisite for everything else) ! will need to change alot in software (no more usb transport layer and no more wifi) (develop two modes that can be selected or two modes for flashing)
2. Read 6 potentiometers via ADC → map to existing synth params
3. Wire OLED + encoder → scale selection + patch browsing
4. Wire physical switches → oscillator toggle, mode DIP, oct buttons
6. Hold mode
7. Arpeggiator
8. Scale selection engine
9. Chord mode (test osc count)
10. Vibrato (hall effect or pressure — whichever is decided)
11. Looper (Phase 6, if PSRAM is sufficient on final chip)
12. Startup auto-calibration (Phase 7)
