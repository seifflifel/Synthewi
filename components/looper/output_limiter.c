// Final audio stage: looper mix + make-up gain + output peak limiter.
//
// Gain staging (measured 2026-11-26): on ESP32 AMY halves its final output
// (amy.c: "uintval >>= 1"), so its absolute ceiling is 16383 = -6 dBFS, and its
// soft/hard clipper sits right below that. With long releases, reverb/echo
// feedback, resonance and 8 voices the summed level hit that ceiling and the
// output saturated (loud, squashed, distorted).
//
// Fix: amy_engine runs AMY 6 dB quieter (volume 0.5) so the internal sum has
// headroom and never reaches AMY's clipper. Here we add the 6 dB back
// (LIM_MAKEUP) for normal playing, and a peak limiter keeps build-ups below
// LIM_THRESHOLD by turning the whole mix down smoothly (~500 ms recovery).
//
// Levels measured 2026-11-27 with make-up x2 were far below the limiter (it never
// engaged): soft note -41 dBFS, firm note -31, 3-pad chord -22, 8 pads -16,
// DREAMPAD chord -19. Make-up is now x8 (+12 dB more): a firm note ≈ -19 dBFS,
// 3-pad chords stay just under the threshold, only 8-pad chords get limited.
//
// Runs in AMY's fill task (core 1) once per block: no logging, no allocation,
// no locks. Cost: one peak scan + one multiply per sample.

#include <math.h>
#include "looper.h"

#define LIM_MAKEUP        8.0f       // +18 dB: +6 undoes amy_engine's volume 0.5, +12 for level (see above)
#define LIM_THRESHOLD     13000.0f   // ~-8 dBFS: just under AMY's old -6 dBFS ceiling
#define LIM_RELEASE_COEF  0.0106f    // per block: 1-exp(-Tblock/500ms), Tblock = 256/48k (measured: 188 blocks/s)

static float s_gain = LIM_MAKEUP;    // current total gain (audio thread only)

// Master volume, applied after the limiter (so it never changes how hard the
// limiter works). Set from the UI task; the audio thread ramps toward it.
static volatile float s_master_target = 1.0f;
static float          s_master        = 1.0f;   // audio thread only

// Scope ring (written by the audio thread only; see output_scope_read)
static int16_t           s_scope[SCOPE_RING];
static volatile uint32_t s_scope_w = 0;

// Diagnostics (written by audio thread, read+reset by output_limiter_get_stats)
static volatile int16_t s_stat_peak     = 0;
static volatile float   s_stat_min_gain = 1.0f;

void output_limiter_process(int16_t *block, int n_frames)
{
    int n = n_frames * 2;   // interleaved stereo

    int32_t peak = 0;
    for (int i = 0; i < n; i++) {
        int32_t a = block[i] < 0 ? -(int32_t)block[i] : block[i];
        if (a > peak) peak = a;
    }

    // Highest gain that keeps this block's peak under the threshold, capped at make-up.
    float target = LIM_MAKEUP;
    if (peak > 0 && peak * LIM_MAKEUP > LIM_THRESHOLD) target = LIM_THRESHOLD / (float)peak;

    float g0 = s_gain, g1;
    if (target < g0) {
        // Attack: the whole block is known in advance (one-block look-ahead),
        // so apply the reduced gain from its first sample — no overshoot.
        g1 = target;
        g0 = target;
    } else {
        // Release: recover smoothly toward make-up gain.
        g1 = g0 + (target - g0) * LIM_RELEASE_COEF;
    }
    s_gain = g1;

    float out_peak = peak * LIM_MAKEUP;
    if (out_peak > s_stat_peak) s_stat_peak = (int16_t)(out_peak > 32767.0f ? 32767.0f : out_peak);
    float red = g1 / LIM_MAKEUP;  // 1.0 = no limiting
    if (red < s_stat_min_gain) s_stat_min_gain = red;

    // Scope tap: limiter gain only (no master volume), so the display size
    // doesn't depend on the volume knob. Every SCOPE_DECIM-th frame, mono.
    {
        float sg = g0, sstep = (g1 - g0) * SCOPE_DECIM / (float)n_frames;
        uint32_t w = s_scope_w;
        for (int f = 0; f < n_frames; f += SCOPE_DECIM) {
            int32_t m = (int32_t)lrintf((block[f * 2] + block[f * 2 + 1]) * 0.5f * sg);
            s_scope[w & (SCOPE_RING - 1)] = (int16_t)(m > 32767 ? 32767 : m < -32768 ? -32768 : m);
            w++;
            sg += sstep;
        }
        s_scope_w = w;
    }

    // Master volume: move toward the target within one block (no zipper noise)
    float m0 = s_master, m1 = s_master_target;
    s_master = m1;
    g0 *= m0;
    g1 *= m1;

    // Ramp g0 → g1 across the block (no zipper noise on gain changes)
    float step = (g1 - g0) / (float)n_frames;
    float g = g0;
    for (int f = 0; f < n_frames; f++) {
        g += step;
        int32_t l = (int32_t)lrintf(block[f * 2]     * g);
        int32_t r = (int32_t)lrintf(block[f * 2 + 1] * g);
        block[f * 2]     = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        block[f * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

void output_scope_read(int16_t *dst, int n)
{
    if (n > SCOPE_RING) n = SCOPE_RING;
    uint32_t w = s_scope_w;                    // newest sample is w-1
    for (int i = 0; i < n; i++)
        dst[i] = s_scope[(w - (uint32_t)n + (uint32_t)i) & (SCOPE_RING - 1)];
}

void output_set_master_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    float v = percent / 100.0f;
    s_master_target = v * v;   // perceptual curve: 50 % ≈ -12 dB, 25 % ≈ -24 dB, 0 = mute
}

void output_limiter_get_stats(limiter_stats_t *out)
{
    out->peak_in  = s_stat_peak;
    out->min_gain = s_stat_min_gain;
    s_stat_peak     = 0;
    s_stat_min_gain = 1.0f;
}

// Strong override of AMY's weak hook (third_party/amy/src/i2s.c): runs on every
// rendered block just before it is written to I2S.
void amy_audio_block_hook(int16_t *block, int n_frames)
{
    looper_process_block(block, n_frames);    // loop playback mixed in first …
    output_limiter_process(block, n_frames);  // … so live + loop are limited together
}
