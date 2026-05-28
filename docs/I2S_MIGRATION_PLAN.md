# Synthewi — I²S Migration Plan

Switching from USB audio + WiFi web UI to I²S + MAX98357A + physical potentiometer(s).

---

## ⚠️ The Threshold Problem — Read First

**Problem**: Thresholds are currently tuned and saved via the web UI. Once WiFi is removed,
there is no way to adjust them from the UI. But you don't have the final hardware rig yet,
so the thresholds you set now may not match the final pad geometry.

**Answer: use two firmware modes and flash between them.**

NVS (the partition where thresholds and audio params are stored) **survives reflashing**.
`idf.py flash` only writes the app partition — it does not touch NVS unless you explicitly
run `idf.py erase-flash`. This means:

1. Flash the current WiFi firmware (what you have now).
2. Open the web UI, tune all 8 thresholds until detection feels right.
3. Flash the new I²S firmware — NVS values are still there, thresholds intact.
4. If you rebuild the rig or add the final pads and thresholds are wrong, go back to step 1.

**Workflow going forward:**

```
WiFi firmware  ──►  tune thresholds in web UI  ──►  idf.py flash (I²S firmware)
     ▲                                                         │
     └────────────── re-tune needed? flash WiFi again ◄────────┘
```

Once the final hardware is built, Phase 7 (startup auto-calibration) will make this ritual
unnecessary. Until then, the two-firmware toggle is the clean solution.

---

## Scope of This Migration

| Remove | Add |
|--------|-----|
| USB UAC audio output | I²S driver (ESP-IDF built-in, no new component) |
| WiFi manager + config server | MAX98357A breakout (3 wires) |
| Web UI (bridge + browser) | 1 potentiometer → release parameter |
| `espressif__usb_device_uac` CMake dep | Dedicated audio task pinned to Core 0 |
| `esp_wifi`, `lwip`, `esp_http_server`, `esp_netif` CMake deps | (nothing else) |

Touch, NVS, AMY engine, and all synth params stay exactly as they are.

---

## Phase 1 — Switch Audio Output to I²S

**What changes**: `main.c` only. The USB callback is replaced by an audio task.

**Hardware needed**: MAX98357A breakout ×1 (mono test first), 3 jumper wires.

**Wiring** (pick free GPIOs, finalize later):
```
ESP32-S3 GPIO38 → MAX98357A BCLK
ESP32-S3 GPIO39 → MAX98357A LRCLK
ESP32-S3 GPIO40 → MAX98357A DIN
5V rail        → MAX98357A VIN
GND            → MAX98357A GND
MAX98357A GAIN → leave floating (9 dB default)
MAX98357A SD   → leave floating (always on)
MAX98357A OUT+ / OUT– → speaker
```

**What the code does**:
- Remove: `usb_uac_device_init()`, all 4 USB callbacks, `#include "usb_device_uac.h"`,
  `is_muted`, `volume_factor`, `s_usb_in_cb_count`, the 1-second USB watchdog loop.
- Add: `i2s_driver_install()` + `i2s_set_pin()` in init.
- Add: a FreeRTOS task pinned to Core 0 that loops:
  `amy_engine_render_mono_16()` → gain/clip → `i2s_write()`.
- The `app_main` while loop becomes just `vTaskDelay(portMAX_DELAY)` — it has nothing
  left to do.

**CMake**: remove `espressif__usb_device_uac` from REQUIRES. Add `driver` (already there).

**Prompt to give Claude when ready**:
```
Implement Phase 1 of docs/I2S_MIGRATION_PLAN.md. Remove all USB UAC code from main/main.c
and main/CMakeLists.txt. Add I2S driver init for the MAX98357A on BCLK=GPIO38,
LRCLK=GPIO39, DOUT=GPIO40, sample rate 48000 Hz, 16-bit mono. Add an audio task pinned
to Core 0 that renders AMY blocks and writes to I2S DMA. Keep OUTPUT_GAIN_BOOST=2 and
the same saturation clip logic. Keep all touch, NVS, and synth param code untouched.
```

---

## Phase 2 — Remove WiFi

**What changes**: `main.c`, `main/CMakeLists.txt`.

**Do this after Phase 1 is confirmed working** — you want to hear audio before cutting WiFi.

**What the code does**:
- Remove: `wifi_manager_start()` call and `#include "wifi_manager.h"`.
- Remove from CMakeLists REQUIRES: `esp_wifi`, `lwip`, `esp_http_server`, `esp_netif`,
  `esp_event` (unless touch_telemetry uses it — check first).
- Remove source files from CMakeLists SRCS: `wifi_manager.c`, `wifi_config_server.c`.
- The touch telemetry task (UDP) can stay or be removed — without a bridge running on
  the PC it does nothing, but it doesn't harm anything either.

**Benefit**: WiFi removal frees ~100 KB heap + unfragments PSRAM. This is when reverb/echo
may become allocatable again (test it).

**Prompt to give Claude when ready**:
```
Implement Phase 2 of docs/I2S_MIGRATION_PLAN.md. Remove wifi_manager_start() and its
include from main/main.c. Remove wifi_manager.c and wifi_config_server.c from
main/CMakeLists.txt SRCS. Remove esp_wifi, lwip, esp_http_server, esp_netif from
CMakeLists REQUIRES — but first check if touch_telemetry.c uses esp_event and keep it
if so. Do not touch any touch, AMY, or NVS code.
```

---

## Phase 3 — Potentiometer for Release

**What changes**: `main.c` (ADC read + param update), `main/CMakeLists.txt` (driver already there).

**Hardware needed**: 1× 10kΩ linear pot (Alps RK09L or similar).

**Wiring**:
```
POT left pin  → 3.3V
POT right pin → GND
POT wiper     → GPIO3 (ADC1 channel 2) + 100nF cap to GND
```

**Why GPIO3**: it's ADC1, which works while WiFi is disabled. After Phase 2, all ADC2 pins
are also free, but GPIO3 is clean and already in the hardware plan.

**What the code does**:
- Add a `pot_task` (or inline in the existing main loop) that reads ADC at ~10 Hz,
  applies a deadband (±1% of range to avoid jitter), maps 0–4095 → release range
  (10–4000 ms), and calls `amy_engine_set_envelope(current_attack, new_release)`.
- NVS persistence: optionally save the pot-derived release to NVS on change, so it
  survives reboot. Or skip NVS for pot params (pot position is always the truth).

**Prompt to give Claude when ready**:
```
Implement Phase 3 of docs/I2S_MIGRATION_PLAN.md. Add ADC reading of a release
potentiometer on GPIO3 (ADC1 channel 2). Poll at 10 Hz in a new task or in the main
loop. Apply a 1% deadband to avoid jitter. Map the ADC range 0–4095 to release time
10–4000 ms using an exponential curve (small values feel musical). Call
amy_engine_set_envelope() with the current attack value from amy_engine_get_state()
and the new release value. Do not save to NVS — pot position is the source of truth.
```

---

## Phase 4 — Core 0 / Core 1 Split (Cleanup)

**This may already be done by Phase 1** (audio task pinned to Core 0).  
This phase formalizes it and pins the remaining tasks correctly.

**Target assignment**:
| Task | Core | Priority | Reason |
|------|------|----------|--------|
| Audio (I²S render + write) | Core 0 | High (10) | Real-time, must not be preempted |
| touch_telemetry_task | Core 1 | 5 | State machine, can tolerate jitter |
| touch_cmd_task | Core 1 | 5 | Param updates, non-real-time |
| pot_task | Core 1 | 3 | Slow ADC polling |

**What changes**: replace `xTaskCreate` with `xTaskCreatePinnedToCore` for all tasks,
set core IDs as above.

**Prompt to give Claude when ready**:
```
Implement Phase 4 of docs/I2S_MIGRATION_PLAN.md. Change all xTaskCreate calls in
main/touch_telemetry.c and main/main.c to xTaskCreatePinnedToCore. Pin the audio task
to Core 0 with priority 10. Pin touch_telemetry_task, touch_cmd_task, and pot_task
(if added) to Core 1 with their existing priorities. Do not change stack sizes.
```

---

## Suggested Order

```
1. Wire MAX98357A (jumper wires, temporary)
2. Flash Phase 1 — confirm you hear sound
3. Flash Phase 2 — confirm touch still works without WiFi
4. Wire potentiometer on GPIO3
5. Flash Phase 3 — confirm pot changes release in real time
6. Flash Phase 4 — confirm no audio glitches after core pinning
```

Each phase is independently flashable and testable. If anything sounds wrong,
you can always re-flash the WiFi firmware to check thresholds haven't drifted.

---

## Open Questions (Decide Before Phase 1)

| # | Question | Options |
|---|----------|---------|
| 1 | I²S GPIO pins | GPIO38/39/40 are tentative — check for conflicts with your current DevKitC-1 pinout |
| 2 | Stereo now or later | Phase 1 can wire both MAX98357A modules (L+R) or just one for testing — your call |
| 3 | Reverb after WiFi removal | Test if reverb/echo allocates successfully after heap is freed — may work for free |
| 4 | ADC pot count | Start with 1 (release) then add more in Phase 3 iterations, one pot per param |
