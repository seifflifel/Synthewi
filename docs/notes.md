# Synthewi Project Notes

## Touch Module Notes

### How the ESP32-S3 capacitive touch sensor works

The ESP32-S3 touch peripheral measures capacitance by counting charge/discharge cycles over a fixed time window — it does **not** use an ADC or measure voltage directly. The result is a raw integer:

- **Raw value**: the cycle count for the current measurement window. Higher = more cycles = less capacitance detected.
- **Baseline (benchmark)**: a hardware-auto-calibrated reference of the "untouched" raw value. The peripheral updates this slowly over time using a built-in filter so it adapts to slow environmental drift (humidity, temperature) but not to fast events like finger contact.
- **Delta**: `|raw − baseline|`. When a finger touches a pad, the added capacitance slows the charge cycles, so the raw value **decreases**. The baseline stays where it was, so the delta grows.
- **Threshold**: a delta value chosen per-pad. When `delta ≥ threshold`, the pad is considered touched.
- **Intensity**: scaled as `(delta / threshold) × 100`, clamped to 100. At exactly the threshold delta, intensity = 100%. Values above 100% are possible if the delta exceeds the threshold (deep press), but are clamped in the firmware.
- **Max raw value**: 65535 (16-bit counter ceiling). A baseline near 65535 means the pin has almost no charge headroom — almost no room to decrease further on touch — making touch detection unreliable.

### Why some pins have very high or saturated baselines

A pin near 65535 baseline is essentially at the hardware maximum and cannot register a meaningful delta on touch. This can happen because:

1. **Electrical coupling from nearby power rails**: pins physically adjacent to GND or VBUS pads on the PCB/header pick up noise that artificially loads the capacitance measurement.
2. **Trace/cable capacitance**: long or parallel wires add parasitic capacitance, raising the apparent load and pushing the raw value higher.
3. **GPIO function conflict**: GPIOs 9, 10, and 11 on all ESP32-S3-WROOM modules are internally connected to the SPI flash bus (D2, D3, CMD). Using them as touch inputs puts them in conflict with the flash peripheral and produces near-saturated, unusable readings.

### Pin selection rationale for this project

The ESP32-S3 has 14 touch-capable channels: **T1–T14 (GPIO 1–14)**. However:

- **GPIO 9, 10, 11 (T9–T11)**: hardwired to the internal SPI flash bus on all WROOM modules (N4, N8, N8R2, N8R8 variants). Baselines saturate near 65535. **Must not be used for touch.**
- **GPIO 1, 2 (T1, T2)**: located on the **right side** of the DevKitC-1 header. These are electrically clean with no competing function on the ESP32-S3 (UART0 is on GPIO43/44, boot mode is GPIO0). They consistently show stable, mid-range baselines.
- **GPIO 13, 14 (T13, T14)**: located at the bottom of the **left side** header, directly above the GND and 5V pins. In testing, these showed baselines of **63785** and **64314** respectively — within ~1200–1750 counts of the 65535 ceiling. This was attributed to capacitive coupling from the adjacent power rail pads and was insufficient for reliable detection.

### Final channel assignment

After iterative testing, the 8 touch channels were assigned as follows:

| Channel | GPIO | Touch | Physical side | Notes |
|---------|------|-------|---------------|-------|
| ch0     | 4    | T4    | Left          | stable |
| ch1     | 5    | T5    | Left          | stable |
| ch2     | 6    | T6    | Left          | stable |
| ch3     | 7    | T7    | Left          | stable |
| ch4     | 8    | T8    | Left          | stable |
| ch5     | 12   | T12   | Left          | stable |
| ch6     | 1    | T1    | Right         | replaced GPIO13 (saturated) |
| ch7     | 2    | T2    | Right         | replaced GPIO14 (saturated) |

GPIO 9/10/11 were initially tested and immediately rejected due to saturation from the flash bus conflict. GPIO 13/14 were used briefly but replaced with GPIO 1/2 after observing near-ceiling baselines caused by proximity to the power rail header pins.
