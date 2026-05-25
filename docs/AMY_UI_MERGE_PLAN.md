## AMY Web UI & Touch Synth Integration Plan

Date: 2026-05-20

Goal
- Improve the web UI realtime experience (no "fighting" with sliders).
- Reuse the best UI/navigation patterns from SparkSynth (LCD) and TouchedOutSynth (touch + looper) for the web interface.
- Map the merged UI to the AMY engine while preserving existing transport constraints (UDP telemetry broadcast port 4210 and UDP command port 4211, WebSocket bridge).

Summary of findings

- SparkSynth UI (embedded LCD):
  - UI is column-based with grouped controls (DCO / HPF / VCF / LFO / ENV).
  - Controls read from ADCs are smoothed and exposed via an EMA filter.
  - Controls implement a "lock/pickup" mechanism: pots only start sending when movement exceeds a dynamic threshold, preventing parameter jumps when physical and UI positions disagree.
  - The UI emphasizes compact, glanceable visualizations (slider bars, dual sliders, small glyphs).

- TouchedOutSynth (Daisy-based):
  - Touch pads mapped to notes and special functions (looper, octave up/down).
  - Looper pad: short press toggles record/play; long hold clears loop (user wants to keep this behaviour, but drop reverse mode).
  - Controls are read continuously and mapped into audio callbacks; aftertouch is supported when enabled.

Key UX problems observed in web UI
- "Fighting" sliders: the UI and device both push values causing jitter when the user moves a slider.
- Sliders/knobs on web need a pickup/lock model and smoothing to match hardware behaviour.

Design goals for web UI
- Implement a pickup/lock model for each control so the web client only takes control when user movement crosses a threshold relative to the device's last-known value.
- Keep UI responsive: local optimistic updates with coalesced commands, but device remains authoritative until pickup.
- Support grouped pages (DCO / HPF / VCF / LFO / ENV) matching SparkSynth layout for muscle memory.
- Expose TouchedOutSynth pad behaviors: looper button, octave up/down, aftertouch toggle, excluding reverse-looper functionality.
- Keep transport unchanged: use existing UDP command packets from bridge to device; add optional heartbeat only if needed (bridge-side) — avoid changing device protocol unless necessary.

Detailed proposals

1) Pickup / Lock behavior (port SparkSynth Controls logic to web)
- Maintain two values per parameter on the client:
  - deviceValue: last value received from device telemetry (0..1 or integer range)
  - uiValue: current UI control position
- On UI render, show the deviceValue visually (track) but allow uiValue to be moved.
- When user starts dragging, compute delta = uiValue - deviceValue. Only when |delta| > pickupThreshold (configurable per-control; e.g. 3–8%) do we consider the control "picked up" and start sending commands to device.
- While not picked-up, the UI should display a small lock icon and ignore clicks that would set values unless the threshold is crossed.
- When picked up, send coalesced updates at a controlled rate (e.g., 20–60 Hz) and apply a small exponential smoothing on the device side if necessary.

2) Deadzone & smoothing
- Apply EMA smoothing on client for rendering and for the values being sent; use a fast smoothing constant when the user is actively manipulating and slower otherwise.
- Use the same dynamic threshold approach as `Controls::readPot()` where recent movement lowers the acceptance threshold (fine control) and inactivity raises it (prevents accidental jumps).

3) Pages & Layout (SparkSynth inspired)
- Implement pages (tabs) for the grouped controls: `DCO`, `HPF`, `VCF`, `LFO`, `ENV`.
- Each page shows the compact column visual (sliders with ticks and small labels) so users familiar with SparkSynth will transfer knowledge.
- Keep keyboard area (touch pad visual) and looper controls on a primary page for immediate play.

4) Touch / Pad behavior (TouchedOutSynth mapping)
- Map pad 0 to Looper: short press toggles record/play, long press (>1s) clears loop.
- Map pad 1/2 to octave down/up and send a 'note off all' to avoid hanging notes when octave changes (same workaround as TouchSynth).
- Pad notes send `WsClientToDevice.type='note'` messages to the bridge; bridge already sends CMD_NOTE packets.
- Add an Aftertouch toggle in the UI to enable/disable sending aftertouch values (match `disableAfterTouch` option).

5) AMY integration and concurrency constraints
- Device-side: AMY must be called only from a single task/context (we added `synth_control_task` and a queue in `main.c`). Keep that.
- Command mapping: reuse existing `buildCmdPacket` shapes (CMD_SET_WAVE, CMD_SET_GAIN, CMD_SET_MUTE, CMD_NOTE). We will map UI controls to CMD_SET_GAIN / CMD_SET_WAVE etc.
- For parameter updates (e.g., VCF frequency), send `CMD_SET_*` style packets. If adding new per-parameter commands is needed, extend the command packet with a parameter ID field, but prefer coalescing many parameter changes into fewer packets where possible.

6) Network & UX robustness
- Client-side: coalesce parameter changes with a short debounce (50–100 ms) and send at most 20–30 updates/sec per parameter.
- Bridge-side: keep bridge's `lastDevice` logic — it depends on telemetry. If telemetry gaps occur, bridge should continue to allow commands for a small grace period (e.g., 10s) after lastTelemetry before rejecting UI actions; show a visual warning when device is unreachable.
- Optional: add an occasional telemetry heartbeat from device (very small UDP packet) so bridge and UI retain presence; implement only if telemetry remains unreliable.

Implementation plan (step-by-step)

Phase 1 — UI behaviour fixes (web client)
1. Implement pickup/lock logic in `web-ui/src` for sliders and knobs. Add a small lock icon overlay when control not picked up.
2. Implement local EMA smoothing for UI values and a controlled send loop (coalesce + throttle) to call existing ws.send({type:'set', path, value}).
3. Group UI into pages (DCO/HPF/VCF/LFO/ENV) and port the visual slider drawing style from `third_party/spark-synth/src/JunoUI.cpp` (layout, tick marks, dual sliders). Keep visuals lightweight (SVG/CSS) so realtime redraw is cheap.

Phase 2 — Touch & Pad UX
1. Update the on-screen keyboard to show pad states and implement looper button with short/long press semantics.
2. Add Aftertouch toggle and octave up/down pad behaviors; ensure web sends `note` messages and `set` messages mapped by the bridge.

Phase 3 — Device-side / AMY mapping and stability
1. Ensure existing `touch_telemetry.c` telemetry contains the minimal param snapshots (we already broadcast core values). Consider adding RSSI/IP to telemetry packet (small extension) for debugging.
2. Keep `synth_control_task` and command parsing in `touch_cmd_task`; map incoming CMD_SET_* to proper AMY APIs.
3. If needed, add a small parameter-change queue and apply smoothing on the device before calling AMY.

Phase 4 — Testing & tuning
1. Test in local network with the bridge and web client running. Tune pickup thresholds and smoothing constants.
2. Stress test with rapid slider movement and long sessions to verify no stack/Concurrency/heap faults.
3. Run with telemetry intermittently disabled to verify the bridge warns but UI recovers cleanly.

Files to update (suggested)
- web-ui/src/ui/app.ts and related components: implement pickup logic, coalescing, pages.
- bridge/src/index.ts: optionally add grace window for lastDevice and heartbeat handling.
- main/touch_telemetry.c: optionally include RSSI/IP in telemetry packet (small extension) for debugging.
- main/main.c: keep `synth_control_task` (already added) and ensure AMY calls are serialized.

Notes on constraints
- Do not change existing UDP port numbers or the basic command packet layout unless strictly necessary. If a param ID expansion is needed, do it as an additive extension and keep older packets supported.
- Keep realtime audio paths free of logging and large stack allocations.

Suggested next tasks (pick one)
- A: Implement pickup/lock in `web-ui` controls (high UX impact). I can implement the client-side change and demo with the bridge.
- B: Add RSSI/IP to `touch_telemetry` packet for debugging and extend the bridge to display it (helps figure out disconnect causes).
- C: Implement looper button web UX (short/long press) and map to existing CMD_NOTE semantics.

If you want I can start with A (web pickup/lock) and create the PR + patch. Tell me which task to start.

++
Notes: this doc draws directly from `third_party/spark-synth/src/JunoUI.cpp` and `third_party/TouchedOutSynth/TouchSynth/TouchSynth.ino` patterns for UI layout and pad/looper behavior.
