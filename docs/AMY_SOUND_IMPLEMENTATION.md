# AMY Sound Implementation: SparkSynth + TouchedOut Analysis and Mapping

Date: 2026-05-20

Purpose
- Deeply analyze how SparkSynth and TouchedOutSynth generate and shape sound.
- Produce a concrete mapping and implementation plan to realize TouchedOutSynth-style behavior using the AMY engine on the ESP32‑S3.
- Prioritize sound engine fidelity and real‑time constraints; UI is secondary.

High‑level constraints & assumptions
- Target platform: ESP32‑S3 (ESP‑IDF) with AMY synth core (native C). Limited RAM/CPU versus desktop, but sufficient for a modest polyphonic synth with simple effects and a small looper.
- AMY is treated as the single-source synth engine; any heavy buffering (looper) is implemented in a separate audio path outside AMY (pre/post processing) to avoid interfering with AMY's real‑time delta/event queues.
- All AMY calls must be serialized (we added `synth_control_task`). Parameter smoothing, voice allocation and priority must be managed to avoid glitches.

Part A — SparkSynth sound architecture (summary)
- Oscillators (DCO): multiple voices with selectable wave shapes (saw, square, PWM) and sub/ sync levels. PWM implemented by changing duty or using combined oscillators.
- Mixer: per-voice level mix (saw/sub/noise) and global volume.
- Filter (VCF): resonant low‑pass with frequency and resonance controls; often modulated by envelope and/or LFO.
- HPF: high‑pass control prior to VCF for tonal shaping.
- Envelope (ENV): standard ADSR per voice; used for amp and filter modulation.
- LFO: global or per-voice low‑frequency oscillator routed to pitch, PWM, filter cutoff, or amplitude.
- Visual layer: compact sliders + lock pickup model; smoothing of pot values on hardware before exposing to the synth.

Part B — TouchedOutSynth core behavior (summary)
- Touch mapping: pads mapped to MIDI note numbers; on touch -> NoteOn with velocity/aftertouch; on release -> NoteOff.
- Looper: circular buffer in RAM, record/play toggle on a pad; hold to clear. Reverse mode exists in original but will be excluded.
- Effects: Reverb post-looper; lightweight DSP (one or two effects) used in audio callback.
- Controls: analog pots for ADSR, LFO, reverb, joystick for rate/time; controls influence vox (synth module) parameters in real time.
- Aftertouch and octave shift: implemented by changing pitch and sending NoteOff to all voices on octave changes to avoid hanging notes.

Part C — How to implement TouchedOut behavior using AMY (mapping)

1) Voices & Oscillators
- Spark/TouchedOut: per-voice oscillator(s) with waveform selection and PWM.
- AMY mapping:
  - Create an AMY voice per physical voice (e.g., 8 voices max depending on CPU). Use AMY APIs to set oscillator type per voice via event calls (amy_add_event or equivalent API).
  - PWM: emulate by either (a) using AMY's oscillator PWM parameter if present, or (b) modulate amplitude of a sub-oscillator to simulate duty change, or (c) use fine oscillator synchronisation and waveform interpolation.

2) Envelopes & Routing
- Spark/TouchedOut: ADSR for amp and optionally filter.
- AMY mapping:
  - Use AMY's delta/event system to schedule envelope deltas for amp and filter cutoff on NoteOn. Implement simple ADSR by enqueueing delta targets and rates.
  - Use separate AMY event channels for amp and filter so they can be independently controlled and released.

3) Filter (VCF) + HPF
- Implement VCF as AMY filter primitive if available; otherwise emulate via state‑variable or one‑pole chains implemented as small DSP objects called from AMY's processing path or just in global audio callback post-AMY.
- Routing: input AMY oscillator output -> filter -> amp envelope -> output.

4) LFO
- LFO targets: filter cutoff, PWM amount, pitch vibrato, amplitude tremolo.
- AMY mapping:
  - Implement LFO as a low‑rate delta generator inside AMY or as a global periodic modulation that writes modulation values to voice parameters each audio block via the serialized synth task.

5) Aftertouch & Pitch/Octave
- Aftertouch: map touch pressure to velocity/amp or filter amount; send periodic parameter updates or events while held.
- Octave shift: implement pitch transpose per voice; when octave change occurs, send 'note off all' then re-trigger notes if necessary.

6) Looper (normal mode only)
- Design choice: implement looper outside AMY in the audio path (recommended):
  - Capture device audio output (mono) into a circular buffer at audio sample rate. When looper toggled to record, write buffer; when in playback, mix buffer into output.
  - Keep looper processing in the audio task (not in AMY) to avoid stuffing AMY queues with large PCM operations.
  - Control messages (start/stop/clear) come from touch events mapped to command handlers.

7) Effects (Reverb)
- Use a lightweight reverb in audio path after AMY/looper mixing. Implement simple Schroeder/feedback comb network or use existing small reverb library tuned for embedded.

Part D — Concurrency, performance, and stability rules
- Single-thread AMY interaction: all amy_add_event, note_on/off calls must be serialized through `synth_control_task`.
- Avoid large stack buffers in tasks that call network or filesystem — keep telemetry small and static.
- Limit per-voice processing: prefer simple filter implementations and small block sizes (AMY_BLOCK_SIZE) to control CPU.
- Parameter smoothing: apply small interpolation internally when param jumps occur to avoid zipper noise.

Part E — Implementation plan (concrete milestones)

Milestone 1 — Core voice & envelopes (1 week)
- Implement per-voice creation and control via AMY (voice table, set waveform, set note pitch).
- Implement envelope deltas on NoteOn/NoteOff using AMY event API.
- Test polyphony and voice stealing policy.

Milestone 2 — Filter and LFO (1 week)
- Add filter control as AMY param or external DSP.
- Implement LFO as modulation source and route to pitch/PWM/filter.

Milestone 3 — Aftertouch, octave, UI mapping (1 week)
- Map touch pressure to aftertouch events.
- Implement octave up/down logic with safe note-off handling.

Milestone 4 — Looper and effects (1–2 weeks)
- Implement circular buffer looper in audio callback; integrate record/play/clear behavior.
- Add lightweight reverb post‑mixer; tune parameters.

Milestone 5 — Integration, tuning, and testing (1–2 weeks)
- Integrate with bridge and web UI pickup/lock model.
- Stress test: long sessions, rapid control moves, network drop/reconnect.

Appendix: file targets to edit
- `components/amy_engine/*` — voice management, helper functions to add envelope and LFO APIs.
- `main/main.c` — ensure synth queue exists and route touch events to synth task.
- `main/audio_processing.c` (new) — implement looper and reverb mixing after AMY render.
- `web-ui` — pickup/lock UI and coalesced sends.

If you approve this plan I will start with Milestone 1 (voice and envelope implementation) and produce patches for `components/amy_engine` and `main/` to expose the necessary AMY APIs and a test harness. 
