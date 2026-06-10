# Potentiometer MUX System (CD4067BE)

## Overview

The Synthewi uses a **CD4067BE 16-channel analog multiplexer** to read **6 potentiometers** with a single ADC input, freeing up GPIO pins for other features.

- **MUX Chip**: CD4067BE (16-channel multiplexer)
- **ADC Input**: GPIO 11 (ADC2 CH0)
- **Select Lines**: GPIO 13 (A), GPIO 14 (B), GPIO 47 (C)
- **Channels Used**: 6 of 16 possible
- **Read Rate**: ~8 Hz per channel (20 ms main loop × 6 channels = 120 ms full cycle)

---

## Channel Mapping

| Channel | GPIO Selects | Parameter | Range | Engine Scaling |
|---------|--------------|-----------|-------|-----------------|
| CH0 | A=0 B=0 C=0 | **Attack** | 0-10000 | 2-2000 ms |
| CH1 | A=1 B=0 C=0 | **Release** | 0-10000 | 10-5000 ms |
| CH2 | A=0 B=1 C=0 | **Filter Cutoff** | 0-10000 | 13-12000 Hz (exponential) |
| CH3 | A=1 B=1 C=0 | **Filter Resonance** | 0-10000 | Q 0.7-11 (exponential) |
| CH4 | A=0 B=0 C=1 | **Echo (Amt + Fb)** | 0-10000 | Both: 0.0-1.0 level |
| CH5 | A=1 B=0 C=1 | **Glide (Portamento)** | 0-10000 | 0-500 ms |

**Note**: GPIO 47 (C select) was added in this version to support 3-bit addressing for future expansion.

---

## Signal Flow

```
Raw ADC → EMA Smoothing → Dual-Threshold Hysteresis → 0.0-1.0 Normalization 
    ↓                                                         ↓
    │                                                  Param Min/Max Mapping
    │                                                         ↓
    └─────────────────────────────────────────→ amy_engine_set_*() calls
```

---

## Core Features

### 1. EMA Smoothing (Exponential Moving Average)

**Purpose**: Noise rejection without lag

**Formula**: `smooth = (smooth × 7 + raw) / 8`  
**Alpha**: 0.125 (slightly more responsive than standard)

This runs **before** threshold checking, so smoothing is clean and stable.

```c
s_smooth[s_ch] = (s_smooth[s_ch] * 7 + raw) >> 3;
```

---

### 2. Dual-Threshold Hysteresis

**Purpose**: Prevents drift while playing, responsive during sweeps

Automatically selects threshold based on **idle time**:

- **Idle (>10s no movement)**: Threshold = 74 ADC counts (~2%)
  - Prevents drift from pot noise or thermal creep
  - Won't trigger accidental param changes while sustaining notes

- **Active (pot moving)**: Threshold = 18 ADC counts (~0.5%)
  - Fine control during live sweeps
  - Responds immediately to user input

**Implementation**:
```c
int64_t idle_us = now - s_last_move_us[s_ch];
int32_t deadband = (idle_us > MUX_IDLE_TIMEOUT_US) ? MUX_DEADBAND_IDLE : MUX_DEADBAND_ACTIVE;
```

---

### 3. Pot Locking on Preset Load

**Purpose**: Load presets without parameter jumps

**Problem**: User loads preset (cutoff=2000), but physical pot is at 70% (7000 scale). Without locking, next pot touch snaps cutoff to 7000, breaking the preset.

**Solution**: Smart selective locking

**Flow**:
1. **Preset loads** → `preset_apply()` calls all `amy_engine_set_*()`
2. **Calls `mux_pots_on_preset_load()`** → Locks all 6 pot channels to loaded values
3. **User leaves pots alone** → Stays at preset values (even if pot phys position differs)
4. **User moves a pot** → That channel **unlocks** with smooth takeover: EMA is seeded at the preset position so the parameter ramps gradually toward the physical pot over ~800 ms
5. **Other pots stay locked** until their pots move

**Logs**:
- `ch0 unlocked (smooth takeover)` = user moved that pot, EMA now ramping from preset to pot position

**Code hook** (in `ui.h` preset_apply):
```c
mux_pots_on_preset_load();  // Lock all pots to prevent jumps
```

---

### 4. Scaling: 0.0-1.0 Normalization + Per-Param Mapping

**Purpose**: Clean, flexible architecture; easy to tweak ranges

**Pipeline**:

```
Raw ADC (MUX_ADC_MIN to MUX_ADC_MAX)
    ↓
norm = (smooth - MUX_ADC_MIN) / (MUX_ADC_MAX - MUX_ADC_MIN)
    ↓
Clamp to [0.0, 1.0]
    ↓
mapped = param_min + (norm × (param_max - param_min))
    ↓
Send to amy_engine_set_*()
```

**Per-Param Config** (in `mux_pots.h`):
```c
typedef struct {
    uint16_t min;  // 0-10000 minimum
    uint16_t max;  // 0-10000 maximum
    const char *name;
} mux_param_range_t;

static const mux_param_range_t s_param_range[MUX_CH_COUNT] = {
    {0, 10000, "attack"},
    {0, 10000, "release"},
    {0, 10000, "cutoff"},
    {0, 10000, "resonance"},
    {0, 10000, "echo"},
    {0, 10000, "glide"},
};
```

**To customize ranges**, edit `s_param_range[]`. Example:
```c
{2000, 8000, "cutoff"},  // Cutoff only 20-80% of full range
```

---

### 5. Echo Parameter Binding

**Purpose**: One pot controls both echo amount and feedback together

**Implementation**:
- CH4 pot position → `echo_amount` scales 0-10000
- `echo_feedback` tracks amount 1:1 — same pot value for both
- User moves pot up → reverb gets wetter (more amount, more feedback)
- User moves pot down → reverb gets drier

**Code**:
```c
case 4:
    amy_engine_set_echo(val, val);
    break;
```

**Log output**:
```
echo           amt=5000 fb=5000
```

---

## UI Integration

### Real-Time Display Updates

The **MAIN card** displays all pot changes **live**:

- When `ui_tick()` runs and user is not editing, it syncs display variables from engine state:
```c
amy_engine_state_t st;
amy_engine_get_state(&st);
s_main_adsr[0] = st.env_attack;      // Attack from pot
s_main_flt_cut = st.filter_cutoff;   // Cutoff from pot
// ... etc
```

- Encoder editing takes priority (local edits shown until exit)
- Exiting edit mode re-syncs from engine (picking up any pot changes during edit)

---

## ADC Configuration

**Hardware Setup** (in `mux_pots_init()`):

```c
#define MUX_ADC_UNIT   ADC_UNIT_2
#define MUX_ADC_CH     ADC_CHANNEL_0    // GPIO 11
#define MUX_ADC_MAX    3800             // Observed max (not 4095)
#define MUX_ADC_MIN    100              // Observed min
```

**Attenuator**: `ADC_ATTEN_DB_12` — 3.3V supply with full scale range

**Practical Range**: 100-3800 (3700 ADC counts usable range)

---

## State Variables

### Per-Channel Tracking
```c
static uint16_t s_v[6];                   // Current 0-10000 values
static int32_t  s_smooth[6];              // EMA-filtered ADC values
static int32_t  s_accepted[6];            // Last threshold-passed smooth value
static bool     s_locked[6];              // true = following preset, false = following pot
static uint16_t s_preset_val[6];          // Preset values when locked
static int64_t  s_last_move_us[6];        // Idle time tracking
```

### Global Parameters
```c
static uint16_t s_decay, s_sustain;       // Non-pot ADSR params (menu-controlled)
// Echo feedback is computed inline as (echo_amount * 0.6) — no separate state needed
```

---

## API Functions

### Public Functions (exposed via header)

```c
// Initialize MUX system (called from app_main)
static void mux_pots_init(void);

// Read & process one channel (called 20ms from main loop)
static void mux_pots_tick(void);

// Lock all pots to preset (called after preset loads)
static void mux_pots_on_preset_load(void);

// Get current value (0-10000) for UI display
static uint16_t mux_pots_get_value(uint8_t ch);

// Check if pot is locked to preset
static bool mux_pots_is_locked(uint8_t ch);
```

---

## Modifying Parameters

### Change Pot-to-Channel Mapping

Edit `mux_pots.h` channel mapping in the switch statement:

```c
switch (s_ch) {
    case 0: case 1:
        amy_engine_set_adsr(s_v[0], s_decay, s_sustain, s_v[1]);
        break;
    // Add new cases here for new pot functions
}
```

### Adjust Min/Max Ranges

Edit `s_param_range[]`:

```c
static const mux_param_range_t s_param_range[MUX_CH_COUNT] = {
    {0, 10000, "attack"},           // Full range
    {2000, 5000, "release"},        // Custom: 20-50% of 0-10000
    // ...
};
```

### Change EMA Alpha

Edit smoothing coefficient in `mux_pots_tick()`:

```c
// Currently: alpha = 0.125 (7/8 weight on old)
s_smooth[s_ch] = (s_smooth[s_ch] * 7 + raw) >> 3;

// Higher alpha = more smoothing, more lag:
s_smooth[s_ch] = (s_smooth[s_ch] * 15 + raw) >> 4;  // alpha = 0.0625

// Lower alpha = less smoothing, more responsive:
s_smooth[s_ch] = (s_smooth[s_ch] * 3 + raw) >> 2;   // alpha = 0.25
```

### Change Threshold Strategy

Edit `MUX_DEADBAND_IDLE`, `MUX_DEADBAND_ACTIVE`, and `MUX_IDLE_TIMEOUT_US`:

```c
#define MUX_DEADBAND_IDLE     74      // ~2% — increase for more drift immunity
#define MUX_DEADBAND_ACTIVE   18      // ~0.5% — decrease for finer control
#define MUX_IDLE_TIMEOUT_US   10000000LL  // 10s — time before switching to idle threshold
```

---

## Debugging

### Enable Raw ADC Logging

Set `MUX_DEBUG_RAW 1` in `mux_pots.h`:

```c
#define MUX_DEBUG_RAW 1
```

Logs raw ADC values every 3 seconds across all 6 channels (no smoothing/deadband applied).

### Enable Detailed Movement Logging

Logs appear whenever threshold is crossed:

```
[mux] ch0 unlocked (pot moved)
[mux]    attack               5000
```

Lock marker `[L]` shows locked channels.

---

## Performance & Latency

- **Per-channel update rate**: 1 / (20ms × 6 channels) = ~8 Hz per channel
- **Full cycle time**: ~120 ms (all 6 pots read once)
- **EMA lag**: ~1-2 frames (20-40ms at 20ms loop rate)
- **Idle detection**: 10 seconds before threshold changes
- **Preset lock time**: Immediate (next pot touch unlocks)

---

## Troubleshooting

### Pot Changes Not Appearing in UI
- Check `ui_tick()` syncs from engine state
- Verify `amy_engine_get_state()` is called
- Confirm pot is not locked (`mux_pots_is_locked()`)

### Preset Load Causes Parameter Jump
- Ensure `mux_pots_on_preset_load()` is called from `preset_apply()`
- Check that `preset_nvs_load()` → `preset_apply()` chain is intact

### Pot Reads Drifting
- Increase `MUX_DEADBAND_IDLE` (currently 74 counts)
- Increase idle timeout before switching thresholds
- Check ADC power supply (3.3V should be stable)

### Pot Too Responsive / Jittery
- Increase EMA alpha (reduce responsiveness, more smoothing)
- Increase `MUX_DEADBAND_ACTIVE` threshold

---

## Future Enhancements

1. **Add CH5-CH15** (10 more pots via same MUX) — just cycle through all 16
2. **Per-channel preset overrides** — lock only specific pots, not all
3. **Conditional pot locking** — lock pots used in current preset, free unused ones
4. **Midi CC export** — send pot values as MIDI CC for external control
5. **Automation recording** — record pot movements as time-series for playback

---

## References

- **CD4067BE Datasheet**: 16-ch multiplexer, TTL/CMOS compatible
- **ESP32-S3 ADC**: 12-bit SAR ADC, 0-3.3V input range, dual channel
- **AMY Engine**: Param ranges all normalized to 0-10000 for consistency
