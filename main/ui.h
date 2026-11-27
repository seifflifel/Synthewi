#pragma once
// UI public API — TFT screens, encoder navigation, presets (implementation in ui/).
// Called by main.c only; everything else is internal to the ui/ modules.

#include <stdint.h>
#include <stdbool.h>

// TFT + presets init and first full draw. Call once, from the main (UI) task.
void ui_init(void);

// Called from main loop every ~20 ms.
// delta: encoder steps since last call (signed); accel: 1..3, applied to value edits only.
// Button gestures (from main.c):
//   short_press  : click — normal navigation (reported 250 ms after release)
//   long_press   : 0.5 s hold — back (on the top screen: arms loop clear)
//   vlong_press  : 2 s hold — clears the loop, only if the hold STARTED on the
//                  top screen (a "back" hold that continues can't wipe the loop)
//   double_click : looper REC → PLAY → OVERDUB → PLAY … from any screen
void ui_tick(int delta, int accel, bool short_press, bool long_press, bool vlong_press, bool double_click);

// Ribbon-state setters — safe to call from the touch task: they only store the
// new state; ui_tick() repaints the ribbon element (the UI task owns SPI).
void ui_set_pad_active(uint8_t pad, bool active);
void ui_set_octave(int shift);   // UI copy only — main.c owns the octave shift
void ui_octave_limit_hit(void);  // octave pad pressed at ±2: flash the label red (any task)
void ui_notify_lever(void);
