# Synthewi Sound Engine Analysis
## Reference Synths: Spark + TouchedOut — What We Can Build

---

## 1. Spark Synth — Deep Analysis

Spark is an ESP32-S3 hardware synth built by the AMY authors themselves. It is the single most instructive reference for Synthewi because it shares the same MCU and the same sound engine. Everything Spark does, we can do.

### Architecture

Spark runs a dual-core design: Core 0 polls the keyboard matrix at 2 ms intervals, Core 1 handles UI, effects, and instrument switching. Six instrument types share a common base class that manages AMY event dispatch, a 16-potentiometer parameter system, and OLED rendering.

### Instrument Engines

#### Analog (Dual Oscillator Subtractive)
The closest thing to what Synthewi currently is — but significantly more capable.

- **5 oscillators per voice, 4 voices polyphony**
  - OSC_0: primary wave (SINE/SAW_UP/SAW_DOWN/PULSE/NOISE)
  - OSC_1: secondary wave, same waveform set, independent detune ±1 octave
  - OSC_2: noise generator
  - OSC_3: silent LFO modulating filter (via `mod_source`)
  - Oscillators chained: OSC_0 → OSC_1 → OSC_2 → OSC_LFO
- **Filter**: LPF, cutoff 50–8050 Hz exponential, resonance 0.5–5.0
- **LFO**: 0–10 Hz sine, routes to filter_freq via CtrlCoef `mod` slot
- **Envelope**: ADSR via AMY breakpoints — attack (6–1022 ms linear), decay/release (exponential matching vintage curves)
- **Detune math**: center-deadzone mapping, frequency multiplier `2^octaves`
- **OSC balance pot**: 0 = OSC1 only → 0.5 = equal mix → 1 = OSC2 only

Key AMY calls used:
```c
e.chained_osc = next_osc;                        // chain oscillators
e.mod_source  = OSC_LFO;                          // LFO routes to filter
e.filter_freq_coefs[COEF_MOD] = lfo_depth;        // depth in CtrlCoef
e.amp_coefs[COEF_CONST/VEL/EG0] = ...;            // amp from velocity × EG
e.filter_type = 1;                                 // LPF
```

#### Juno (5-Oscillator Analog Emulation)
Full Roland Juno-60/106 emulation. 128 sysex patches stored in PROGMEM.

- **5 oscillators per voice, 5 voices polyphony**
  - OSC_PWM: pulse wave with duty cycle modulated by LFO or manual pot
  - OSC_LFO: LFO with fade-in delay (18–2000 ms) → routes to PWM depth AND filter cutoff
  - OSC_SAW: sawtooth generator
  - OSC_SUB: sub-oscillator, 1 octave below
  - OSC_NOISE: noise
- **VCF**: Juno-style, cutoff `13 × 2^(0.0938 × vcf_freq × 127)` Hz
- **Resonance**: `0.7 × 2^(4 × vcf_res)`
- **LFO rate**: `0.6 × 2^(0.04 × lfo_rate × 127) - 0.1` Hz
- **Always-on oscillators**: all 5 kept active at amp=0.005 minimum so patch parameters can be adjusted during sustained notes
- **Chorus**: AMY chorus effect enabled/disabled per patch

#### DX7 (FM Synthesis)
128 preset patches (patches 128–255 in AMY). 32 operator routing algorithms.

- **6 FM operators** per voice, 8 voices
- Algorithm visual diagram rendered on OLED (operator boxes + connection lines)
- `e.patch_number = patch_index + 128` — AMY handles all DX7 operator routing internally
- Patch library of 1024 DX7 presets in flash

#### Piano
Single line: `e.patch_number = 256`. AMY's internal piano patch — partial-driven physical piano model.

#### Sampler
Records from I2S mic (ICS-43434) to PSRAM. 5 seconds at 44.1 kHz.

- Records as PCM preset into AMY's sample space (`pcm_load()`)
- Trim knobs for start/end points with 150 ms debounce
- Gain knob applied at note-on velocity (not during recording)
- Waveform display with trim markers on OLED

### Controls System

**16 potentiometers** via 4-bit mux, with dynamic locking:
- Pots stay locked (2% threshold) until user physically moves them past baseline
- Lock resets when switching instruments — prevents value jumps on preset load
- Exponential moving average filter (α = 0.16) for responsive but jitter-free reads

Standard pot routing across all instruments:
```
0-3:    Custom per instrument (waveform, detune, etc.)
4-5:    Filter cutoff + resonance
6-7:    LFO rate + LFO depth
8-11:   ADSR attack, decay, sustain, release
12-14:  Reverb, delay freq, delay amount
15:     Volume / noise level
```

**Global effects** applied per pot movement:
```c
// Reverb: wet 0-2x, decay 0.85 fixed, damping 0.5 fixed
config_reverb(wet, 0.85, 0.5, 3000.0);

// Echo: level, freq component, 3-second buffer, feedback = wet * 0.8
config_echo(delay_amp, delay_freq, 3000, delay_amp * 0.8, 0.0);
```

**Startup EQ** (applied globally to cut hardware artifacts):
```c
e.eq_l = +4.0 dB   // boost lows
e.eq_m = -3.0 dB   // cut midrange honk
e.eq_h = -6.0 dB   // roll off highs
e.volume = 4.0
```

### OLED UI Philosophy

Each instrument has its own display layout:
- **DX7**: Patch name + FM algorithm diagram (operator routing as visual blocks)
- **Juno**: 4 parameter columns (DCO/VCF/EG/VCA) with vertical slider widgets
- **Sampler**: Waveform view + trim markers + recording progress bar
- **BLE**: Bluetooth icon, filled when connected

The display throttles to 10 Hz to avoid blocking audio. This is the UI language we should emulate in the web — parameter columns with visual slider widgets per "instrument type."

---

## 2. TouchedOut Synth — Deep Analysis

TouchedOut runs on Daisy Seed (STM32H750 ARM Cortex-M7), a completely different stack from ours. But its design philosophy and expressiveness techniques are exactly what makes a touch synthesizer feel alive. We study the *ideas*, not the code.

### Architecture

**Subtractive synthesis** with polyphonic voice architecture.

```
Touch Input (2× MPR121, 24 pads)
  → Voice Manager (8 voices)
    → PolyBLEP Oscillator (saw/tri/square)
      → Moog Ladder Filter (24 dB/octave)
        → ADSR Envelope
          → Main Bus
            → Variable-Speed Looper
              → ReverbSc
                → Stereo Output
```

### What makes it expressive

#### Pressure-Based Aftertouch
This is the key insight. The MPR121 gives both raw capacitance and a tracked baseline. TouchedOut computes:
```
pressure = baseline - filtered_value
aftertouch = pressure / 150.0   (normalized 0-1)
```
This measures how *deeply* a finger is pressing — not just whether it's touching. That aftertouch signal routes to:
- **Filter cutoff**: `cutoff = aftertouch² × 8000 + 600 Hz` — pressing harder opens the filter
- **Amplitude**: pressure controls volume (tremolo-like control while held)
- **PWM width**: for square wave timbres

The result: a note doesn't just start and play — it *breathes* with your finger.

#### Per-Voice Filter
Every one of the 8 voices has its own `MoogLadder` instance. This means polyphonic filter modulation — two fingers pressing with different pressure give two notes with different filter brightness simultaneously.

#### Filter Envelope vs Amplitude Envelope
Two independent ADSR paths: one for amplitude (always on), one optionally for filter. When the filter envelope is active, the filter tracks the envelope shape independently from the volume shape. This produces the classic "wah → sustain" sound that's fundamental to expressive synthesis.

#### Variable-Speed Looper
A 1-minute stereo buffer with fractional playback speed (−2x to +2x). The joystick X-axis controls speed with a center deadzone. Going negative reverses the loop. Windowed boundaries prevent clicks. This is a creative tool we haven't explored at all — it turns the synth into an ambient texture machine.

#### LFO Routing
One sine LFO (0.1–10 Hz) with two modes via toggle switch:
- **LFO → Filter cutoff**: classic filter wobble
- **LFO → Amplitude**: tremolo

The LFO depth and rate are on dedicated knobs. Simple, focused, effective.

### Key parameter scaling insight

Every perceptually non-linear parameter uses `x²` exponential curves:
```
cutoff = value² × 10000 + 600
lfo_rate = value² × 10.0 + 0.1
envelope_ms = value² × range + min
```
This matches human perception of frequency and time — equal physical movement produces equal *felt* change across the whole range.

We already use 0–10000 scaling on the ESP side. We should apply this same `x²` curve server-side in `amy_engine.c` when mapping our 0–10000 values to actual Hz/seconds.

---

## 3. What We Can Build in Synthewi

### Full AMY Feature Inventory (Available Now)

AMY gives us all of this, most of it currently unused:

| Feature | AMY API | Currently Used |
|---------|---------|----------------|
| SINE, PULSE, SAW_DOWN, SAW_UP, TRIANGLE | `wave` 0–4 | ✓ |
| NOISE | `wave=5` | ✗ |
| Karplus-Strong (plucked string) | `wave=6`, `feedback` | ✗ |
| PCM samples (808 drums + custom) | `wave=7`, `preset` | ✗ |
| FM synthesis (DX7 algorithms) | `wave=8`, `algorithm`, `algo_source` | ✗ |
| Juno-6 patches (0–127) | `patch_number` | ✗ |
| DX7 patches (128–255) | `patch_number` | ✗ |
| Piano (256) | `patch_number=256` | ✗ |
| Wavetable | `wave=19` | ✗ |
| Filter LPF/BPF/HPF | `filter_type` 1–3 | LPF only |
| Double-order LPF | `filter_type=4` | ✗ |
| Filter resonance 0.5–16 | `resonance` | ✓ (limited) |
| Envelope Generator 0 (amplitude) | `bp0` / eg0_times/values | ✓ |
| Envelope Generator 1 (any target) | `bp1` / eg1_times/values | ✗ |
| Filter envelope (EG1 → filter_freq) | `filter_freq_coefs[eg1]` | ✗ |
| Amplitude via CtrlCoef | `amp_coefs[]` | partial |
| LFO (osc as mod_source) | `mod_source` | ✗ |
| PWM duty cycle | `duty_coefs[]` | ✗ |
| Chained oscillators | `chained_osc` | ✗ |
| Sub-oscillator (chained, -1 octave) | chained + ratio=0.5 | ✗ |
| Portamento | `portamento` ms | ✗ |
| Reverb | `reverb` level/liveness/damping | ✓ |
| Echo | `echo` level/delay/feedback | ✓ |
| Chorus | `chorus` level/delay/freq/depth | ✗ |
| 3-band EQ | `eq_l`, `eq_m`, `eq_h` | ✗ |
| Global pitch bend | `pitch_bend` | ✗ |
| Pan per oscillator | `pan_coefs[]` | ✗ |
| External coefficient (touch pressure → CtrlCoef) | `ext0`, `ext1` | ✗ |
| Sequencer / BPM | `sequence`, `tempo` | ✗ |
| Synth/voice management with stealing | `synth`, `num_voices` | partial |
| MIDI USB output | `AMY_MIDI_IS_USB_GADGET` | ✗ |

---

## 4. Implementation Plan

Organized in three phases by impact-to-complexity ratio. Each phase is independently useful.

---

### Phase 1 — Expressiveness (Highest Impact, Relatively Low Complexity)

These transform the instrument from a demo into something genuinely playable.

#### 1.1 Pressure/Aftertouch → Filter Cutoff

The single biggest expressive improvement. The ESP32-S3 touch driver gives us `baseline - filtered` pressure data. The existing touch ISR already provides this — we just don't use it.

**ESP side:**
- Add `touch_sensor_get_delta()` in the touch callback to read continuous pressure
- Map pressure (0–150 typical range) to 0–10000
- Send as `SYNTH_PARAM_AFTERTOUCH` per pad via a new parameter path or fold into telemetry
- In `amy_engine.c`: use `ext0` CtrlCoef slot — call `amy_set_external_coef(pad_index, pressure_normalized)`, then set `filter_freq_coefs[COEF_EXT0] = depth` on oscillator init

**The AMY hook:**
```c
// In amy_engine.h, expose:
void amy_engine_set_pressure(uint8_t pad, float pressure_0to1);
// Internally calls: AMY ext coef hook with normalized pressure
// Routed to filter_freq via: filter_freq_coefs[7] = pressure_depth
```

**Web UI:** A "pressure → filter" depth slider (0–100%) — how much pressing harder opens the filter.

#### 1.2 Filter Envelope (EG1 → Filter Cutoff)

Add a second envelope that modulates filter cutoff independently from amplitude. This gives the classic "wah-then-sustain" sound.

**AMY calls:**
```c
// Set up EG1 to modulate filter:
e.eg1_times[0]  = env_decay_ms;
e.eg1_values[0] = 1.0;         // peak
e.eg1_times[1]  = env_release_ms;
e.eg1_values[1] = sustain_level;

// Route EG1 into filter frequency:
e.filter_freq_coefs[COEF_EG1] = filter_env_depth; // e.g. 0.0–5000.0 Hz depth
```

**New parameters:**
- `filter_env_depth` (0–10000): how far the filter opens on attack
- `filter_env_decay` (0–10000): how fast it falls back to cutoff
- Reuse existing `filter_cutoff` as the base floor

**Web UI:** A "filter env" section alongside the existing filter card: depth slider + decay slider.

#### 1.3 LFO

Dedicate one oscillator as a silent LFO modulating another oscillator's filter or amplitude.

**AMY setup per pad voice:**
```c
// LFO oscillator (silent):
e.osc  = lfo_osc_index;
e.wave = SINE;
e.freq = lfo_rate_hz;    // 0.1 – 10 Hz
e.amp  = lfo_depth;      // silent, used as mod source

// Audio oscillator:
e.osc        = audio_osc_index;
e.mod_source = lfo_osc_index;
e.filter_freq_coefs[COEF_MOD] = 4000.0; // LFO swing range in Hz
```

**New parameters:**
- `lfo_rate` (0–10000 → 0.1–10 Hz exponential)
- `lfo_depth` (0–10000 → 0–1.0)
- `lfo_target`: filter | amplitude | pitch (enum, 3 options)

**Web UI:** LFO card with rate/depth sliders and a target toggle (3 buttons: filter/amp/pitch).

#### 1.4 Portamento (Pitch Slide)

When a new note is triggered without releasing the previous one, AMY slides the pitch smoothly rather than jumping.

```c
e.portamento = portamento_ms; // 0 = instant, 50–200 = expressive
```

One new parameter, `portamento` (0–10000 → 0–500 ms exponential). Zero means off (current behavior).

**Web UI:** A single slider in the oscillator section. Labeled "glide."

---

### Phase 2 — Sound Palette Expansion

More synthesis options, each exposing a fundamentally different character.

#### 2.1 Karplus-Strong (Plucked String)

One of AMY's best synthesis modes for a touch instrument. A plucked string sound that decays naturally, with brightness controlled by feedback.

```c
e.wave     = KS;          // wave=6
e.feedback = 0.996;       // near 1.0 = bright & long, 0.9 = dark & short
e.midi_note = midi_note;
e.velocity  = 1.0;
```

KS works differently from oscillator synthesis: the note always decays — there's no sustained hold. Instead of an ADSR envelope, `feedback` controls decay length. Our "attack" control becomes irrelevant for KS; instead expose just `feedback` (decay time) and maybe `bp0` for initial attack shaping.

**Web UI:** When wave = KS, replace ADSR card with a single "decay" / "brightness" slider.

#### 2.2 Sub-Oscillator

Chain a second oscillator at exactly 1 octave below using `chained_osc` and `ratio=0.5`. Adds bass weight to any waveform.

```c
// Sub oscillator:
e_sub.osc        = sub_osc_index;
e_sub.wave       = SAW_DOWN;
e_sub.ratio      = 0.5;    // 1 octave below
e_sub.amp_coefs[COEF_VEL] = 0.5; // quieter than primary
e_sub.chained_osc = audio_osc_index;

// Primary oscillator chains to sub:
// (chaining means note-on propagates automatically)
```

**New parameter:** `sub_level` (0–10000 → 0–1.0 amplitude ratio).

#### 2.3 Oscillator Detune / Unison

A second oscillator slightly detuned from the first. AMY's `freq_coefs[COEF_CONST]` can offset frequency independently per oscillator.

```c
// Oscillator 2, slightly above primary:
e2.freq_coefs[COEF_CONST] = base_freq * detune_multiplier;
// detune_multiplier = 2^(cents/1200), e.g. 2^(7/1200) ≈ 1.004
```

**New parameter:** `detune_cents` (0–10000 → 0–50 cents). At 0 it's off. At 7–15 cents it creates the classic "super saw" detuned sound. At >20 cents it becomes dramatic beating.

#### 2.4 Noise Wave + Filter

Noise through a resonant filter produces breath-like textures, wind, and percussion. Useful as a blend with the tonal oscillator.

```c
e.wave = NOISE; // wave=5
// Pair with resonant filter: resonance 2.0–8.0 makes it tonal
```

**New parameter:** `noise_level` — blend of noise into the main oscillator signal (0–10000 → 0–1.0).

#### 2.5 Chorus Effect

AMY has chorus built in, we just don't expose it. A wide modulated delay that thickens the sound dramatically.

```c
// chorus: level, max_delay_samples (320), lfo_freq (0.5 Hz), depth (0.5)
e.chorus_level    = chorus_amount;
e.chorus_max_delay = 320;
e.chorus_lfo_freq = 0.5;
e.chorus_depth    = chorus_depth;
```

**New parameters:** `chorus_amount` + `chorus_depth`. Two sliders.

#### 2.6 Filter Type Selection (LPF / BPF / HPF / Double LPF)

AMY supports 4 filter topologies. We currently hardcode LPF.

```c
e.filter_type = 1; // LPF
e.filter_type = 2; // BPF — peak resonance, sounds like a vowel formant
e.filter_type = 3; // HPF — removes bass, sounds harsh/bright
e.filter_type = 4; // Double LPF — 48 dB/oct, very aggressive
```

BPF with high resonance produces vowel-like sounds ideal for touch expression. A 4-button selector in the UI.

---

### Phase 3 — Patch System + FM + Polish

#### 3.1 AMY Preset Patches (Juno + DX7)

AMY has 300+ baked-in patches. To use them: set `synth`, `num_voices`, and `patch_number`. That's it — AMY handles all oscillator setup internally.

```c
// Juno patch (0-127):
e.synth       = SYNTH_JUNO;
e.num_voices  = 4;
e.patch_number = patch_idx;

// DX7 patch (128-255):
e.synth       = SYNTH_DX7;
e.num_voices  = 4;
e.patch_number = 128 + patch_idx;

// Piano:
e.patch_number = 256;
```

**New concept for Synthewi:** a "mode" selector: CUSTOM (current manual engine) | JUNO (128 patches) | DX7 (128 patches) | PIANO. In CUSTOM mode all our current controls apply. In JUNO/DX7 mode the pots map to patch-specific parameters (filter + ADSR still work on top of the patch).

**Web UI:** Mode toggle at top of synth section. In patch modes, add a patch number selector (prev/next buttons or a number input).

#### 3.2 FM Synthesis (Direct ALGO Control)

For users who want to go deeper than preset DX7 patches:

```c
e.wave      = ALGO;        // FM mode
e.algorithm = 1;           // DX7 algorithm 1–32
e.algo_source = "0,1,2,3"; // which AMY oscs are the 4 operators
e.ratio     = 2.0;         // operator frequency ratio
e.feedback  = 0.5;         // operator feedback (self-FM for brightness)
```

Start with algorithm presets (a few well-known timbres: electric piano, bass, bell, brass) before exposing raw algorithm selection.

#### 3.3 Per-Pad Pan / Stereo Spread

Map each of the 4 pads to a position in the stereo field. Pad 0 = left, pad 3 = right, with 1 and 2 between.

```c
e.pan_coefs[COEF_CONST] = pan_0to1; // 0.0 = left, 1.0 = right
```

No new parameter needed — just set it at init time based on pad index. Immediate improvement to the stereo image.

#### 3.4 3-Band EQ (Startup Calibration)

Apply Spark's EQ strategy: boost bass slightly, cut midrange honk from the I2S codec, roll off harsh highs. Hardcode on boot rather than expose as UI controls.

```c
e.eq_l = +3.0; // low shelf +3 dB at ~800 Hz
e.eq_m = -2.0; // mid cut -2 dB at ~2500 Hz  
e.eq_h = -4.0; // high shelf -4 dB at ~7500 Hz
```

#### 3.5 Velocity Sensitivity

Use touch delta magnitude at the moment of touch (not continuous pressure) to set note-on velocity.

```
velocity = clamp(delta / threshold, 0.0, 1.0)
```

The touch ISR already has `delta` and `threshold`. A "velocity sensitivity" parameter (0 = fixed velocity, 10000 = full dynamic) scales the contribution:

```
effective_velocity = fixed_base + sensitivity × delta_velocity
```

Routes directly into AMY via `e.velocity`. Zero sensitivity = current behavior.

#### 3.6 Web UI — Instrument Mode Pages (Inspired by Spark's OLED)

Restructure the web UI around pages/modes instead of one flat column of sliders:

- **OSCILLATOR page**: wave selector, sub level, detune, noise blend
- **FILTER page**: type selector, cutoff, resonance, filter envelope (depth + decay)
- **MODULATION page**: LFO rate/depth/target, portamento
- **EFFECTS page**: reverb, echo, chorus
- **ENVELOPE page**: attack, release (decay + sustain to add)

This matches Spark's per-instrument UI structure. On screen (when we have hardware) each page maps to a physical display layout. On web, it maps to tab navigation.

---

## 5. Implementation Order — Recommended Sequence

| # | Feature | Complexity | Impact | Why Now |
|---|---------|-----------|--------|---------|
| 1 | Per-pad stereo pan | Low | High | One line of C, immediate improvement |
| 2 | Portamento / glide | Low | High | One parameter, sounds instantly musical |
| 3 | LFO (filter target) | Medium | Very High | Core expressiveness, well-understood AMY path |
| 4 | Filter envelope (EG1) | Medium | Very High | Most expressive single addition |
| 5 | Pressure → filter | Medium | Very High | Makes touch itself expressive |
| 6 | Chorus | Low | High | One AMY call, already exposed in config |
| 7 | Filter type selector | Low | Medium | 4 options, trivial AMY change |
| 8 | Velocity sensitivity | Medium | High | Requires threshold calibration work |
| 9 | Karplus-Strong mode | Low | High | Wave=6, expose feedback as "decay" |
| 10 | Sub-oscillator | Medium | High | Chained osc, needs osc budget increase |
| 11 | Detune / unison | Medium | High | Chained osc + freq_coef offset |
| 12 | Noise blend | Low | Medium | Wave=5 osc chained at low amp |
| 13 | 3-band EQ startup | Trivial | Medium | Hardcode values in amy_engine_init |
| 14 | Juno/DX7 patches | Medium | Very High | Massive palette unlocked, no DSP work |
| 15 | FM direct ALGO mode | High | High | New synthesis mode, needs UI planning |
| 16 | UI page system | High | High | Restructure app.ts into tabbed pages |

---

## 6. Key Architectural Notes for Implementation

### Oscillator Budget
Current: `max_oscs = SYNTH_PAD_COUNT + 4 = 8`. Once we add sub-osc + LFO per pad, we need at minimum:
- 4 pads × (audio + sub + LFO) = 12 oscillators
- Plus 4 headroom = **16 total**

Adjust in `amy_engine_init()`:
```c
cfg.max_oscs = (SYNTH_PAD_COUNT * 3) + 4; // 16
```
Each oscillator uses ~2 KB of RAM (fixed-point samples + state). 16 × 2 KB = 32 KB — well within the 512 KB SRAM of ESP32-S3.

### Pressure Data Path
The ESP-IDF touch driver gives `raw_value` and `baseline_value` per channel. We already compute `abs_delta`. For pressure we need *signed* delta when above threshold:
```c
int32_t pressure = (int32_t)s->baseline_value - (int32_t)s->raw_value;
// Positive when pressing (capacitance increases, raw decreases from baseline)
uint16_t pressure_norm = (uint16_t)clamp(pressure * 10000 / PRESSURE_MAX, 0, 10000);
```
This feeds into `amy_external_coef_hook` which AMY calls each block to read `ext0`/`ext1` values.

### CtrlCoef Strategy
The CtrlCoef system is what makes Spark and TouchedOut expressive. Our current engine ignores it almost entirely. The minimum useful setup for each oscillator:

```c
// Amplitude: velocity × EG0 (standard)
e.amp_coefs[COEF_CONST] = 0;
e.amp_coefs[COEF_VEL]   = 1;
e.amp_coefs[COEF_EG0]   = 1;

// Filter: base cutoff + EG1 envelope depth + pressure (ext0) depth
e.filter_freq_coefs[COEF_CONST] = base_cutoff_hz;
e.filter_freq_coefs[COEF_EG1]   = filter_env_depth_hz;
e.filter_freq_coefs[COEF_EXT0]  = pressure_depth_hz;
e.filter_freq_coefs[COEF_MOD]   = lfo_depth_hz;
```

This single setup gives us filter controlled by: static cutoff + filter envelope + pressure + LFO — all simultaneously, all real-time adjustable. That's the full expressive signal chain of a professional touch synth.

### NVS Expansion
Current `amy_engine_state_t` stores 9 fields. New fields to add:
- `lfo_rate`, `lfo_depth`, `lfo_target`
- `filter_env_depth`, `filter_env_decay`
- `portamento_ms`
- `sub_level`, `detune_cents`, `noise_level`
- `chorus_amount`, `chorus_depth`
- `filter_type`
- `mode` (CUSTOM / JUNO / DX7 / PIANO)
- `patch_number` (for JUNO/DX7 mode)

NVS key names are short strings. Keep them under 15 chars. One NVS entry per parameter.

---

## 7. Summary

Spark shows us that AMY is capable of a professional multi-instrument synthesizer on exactly our hardware. TouchedOut shows us that a touch instrument lives or dies on **pressure expressiveness** and **per-voice filter modulation**. AMY gives us the tools for both.

The three things that will transform Synthewi from a proof-of-concept into an instrument:

1. **Pressure → filter** — the finger becomes a continuous controller, not just a trigger
2. **Filter envelope** — every note has a timbral shape, not just a volume shape
3. **LFO → filter** — the sound breathes on its own between touches

Everything else — KS, FM, Juno patches, detune, chorus — adds palette. But expressiveness comes first.
