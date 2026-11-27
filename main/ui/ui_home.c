// HOME screen (redesign 2026-11, see docs/ui_sketch.html):
//   y 23..86   oscilloscope of the real output (amber trace, tinted fill)
//   y 89..116  PARAMS / PRESETS / SETTINGS tiles; the selected icon animates and
//              the amber selection frame slides between tiles
//   y 118..127 loop bar, only while a loop exists
// Redrawn every UI tick while visible (the framebuffer diff keeps SPI cheap).

#include <math.h>
#include <string.h>
#include "looper.h"
#include "ui_internal.h"

#define SCOPE_X0   2
#define SCOPE_N    156             // columns 2..157
#define SCOPE_MID  54
#define SCOPE_TOP  24
#define SCOPE_BOT  85
#define SCOPE_AMP  31              // px at full deflection
// Fixed (not auto-zoom) soft curve: y = AMP * tanh(s / KNEE). A firm single note
// (~3800) uses about half the height, chords fill it without hard clipping.
#define SCOPE_KNEE 6000.0f

static const int s_tile_x[3][2] = {{1, 52}, {54, 105}, {107, 158}};
static const char * const s_tile_lbl[3] = {"PARAMS", "PRESETS", "SETTINGS"};

void ui_home_tile_rect(int i, int r[4])
{
    r[0] = s_tile_x[i][0]; r[1] = 89; r[2] = s_tile_x[i][1]; r[3] = 116;
}

// ── Scope ─────────────────────────────────────────────────────
// Trigger on the most recent rising zero crossing that leaves a full window,
// so a held note stands still (like a real oscilloscope). No trigger → newest data.
static void draw_scope(void)
{
    static int16_t buf[SCOPE_RING];
    output_scope_read(buf, SCOPE_RING);

    const int hyst = 150;
    int start = SCOPE_RING - SCOPE_N;
    for (int i = SCOPE_RING - SCOPE_N; i >= 8 && i > SCOPE_RING - SCOPE_N - 320; i--) {
        if (buf[i - 1] < 0 && buf[i] >= 0) {
            int lo = 0;
            for (int k = i - 8; k < i; k++) if (buf[k] < lo) lo = buf[k];
            if (lo < -hyst) { start = i; break; }
        }
    }

    int ys[SCOPE_N];
    for (int i = 0; i < SCOPE_N; i++) {
        int y = SCOPE_MID - (int)lrintf(SCOPE_AMP * tanhf((float)buf[start + i] / SCOPE_KNEE));
        ys[i] = y < SCOPE_TOP ? SCOPE_TOP : y > SCOPE_BOT ? SCOPE_BOT : y;
    }

    tft_line_dash(SCOPE_X0, SCOPE_MID, SCOPE_X0 + SCOPE_N - 1, SCOPE_MID, 3, C_GRID);
    for (int i = 0; i < SCOPE_N; i++) {            // tinted fill between trace and centre
        int x = SCOPE_X0 + i, y = ys[i];
        if (y < SCOPE_MID)      tft_rect(x, y + 1, x, SCOPE_MID, C_TINT);
        else if (y > SCOPE_MID) tft_rect(x, SCOPE_MID, x, y - 1, C_TINT);
    }
    for (int i = 1; i < SCOPE_N; i++)
        tft_line_thick(SCOPE_X0 + i - 1, ys[i - 1], SCOPE_X0 + i, ys[i], C_ORANGE);
}

// ── Tile icons (animate only when selected) ───────────────────
static void ico_params(int cx, int cy, uint16_t c, bool act, float t)
{
    for (int i = -1; i <= 1; i++) {
        int x = cx + i * 11;
        tft_ring(x, cy, 4, c);
        float a = act ? 0.6f * (float)M_PI * sinf(t * 2.2f + i * 1.9f) : i * 0.7f;
        tft_line(x, cy, x + (int)lrintf(sinf(a) * 3.4f), cy - (int)lrintf(cosf(a) * 3.4f), c);
    }
}

// Cassette: the reels spin when selected.
static void ico_presets(int cx, int cy, uint16_t c, bool act, float t)
{
    int x0 = cx - 13, x1 = cx + 13, y0 = cy - 8, y1 = cy + 6, ry = cy - 2;
    tft_rrect(x0, y0, x1, y1, c);
    tft_rrect(x0 + 4, y0 + 2, x1 - 4, y0 + 10, c);
    tft_line(x0 + 6, y1, x0 + 8, y1 - 3, c);
    tft_line(x0 + 8, y1 - 3, x1 - 8, y1 - 3, c);
    tft_line(x1 - 8, y1 - 3, x1 - 6, y1, c);
    float a0 = act ? t * 4.0f : 0.3f;
    for (int k = -1; k <= 1; k += 2) {
        int rx = cx + k * 6;
        tft_ring(rx, ry, 3, c);
        for (int s = 0; s < 3; s++) {
            float a = a0 + s * 2.0f * (float)M_PI / 3.0f;
            tft_pixel(rx + (int)lrintf(cosf(a) * 1.6f), ry + (int)lrintf(sinf(a) * 1.6f), c);
        }
    }
}

static void ico_gear(int cx, int cy, uint16_t c, bool act, float t)
{
    float a0 = act ? t * 1.4f : 0.0f;
    for (int k = 0; k < 8; k++) {
        float a = a0 + k * (float)M_PI / 4.0f;
        tft_disc(cx + (int)lrintf(cosf(a) * 5.5f), cy + (int)lrintf(sinf(a) * 5.5f), 1, c);
    }
    tft_ring(cx, cy, 4, c);
    tft_disc(cx, cy, 1, c);
}

// ── Loop bar ──────────────────────────────────────────────────
// 4 groups of 9 bars = the whole loop buffer. A shorter loop just colours part
// of it (no resizing, no numbers). The playhead bar is taller and dark.
static void draw_loop_bar(int y)
{
    looper_progress_t lp;
    looper_get_progress(&lp);
    if (lp.state == LOOPER_IDLE || lp.max_length == 0) return;
    float lf, hf;
    if (lp.state == LOOPER_RECORDING) lf = hf = (float)lp.position / (float)lp.max_length;
    else { lf = (float)lp.length / (float)lp.max_length; hf = (float)lp.position / (float)lp.max_length; }
    uint16_t col = lp.state == LOOPER_RECORDING ? C_RED : lp.state == LOOPER_PLAYING ? C_GREEN : C_BLUE;

    const int N = 9, bw = 2, gap = 2, gW = N * (bw + gap) - gap, gGap = 6, x0 = 3;
    int head = (int)(hf * 36.0f);
    if (head > 35) head = 35;
    for (int g = 0; g < 4; g++) for (int i = 0; i < N; i++) {
        int idx = g * N + i, x = x0 + g * (gW + gGap) + i * (bw + gap);
        if (idx == head) { tft_rect(x, y - 1, x + bw - 1, y + 8, C_WHITE); continue; }
        bool rec = ((float)idx + 0.5f) / 36.0f <= lf;
        int top = (i == 0) ? y : y + 1;            // first bar of each quarter: 1 px taller
        tft_rect(x, top, x + bw - 1, y + 6, rec ? col : C_GRID);
    }
}

// ── Screen ────────────────────────────────────────────────────
void ui_draw_home(void)
{
    tft_rect(0, 22, TFT_W - 1, TFT_H - 1, C_BLACK);
    draw_scope();

    float t = ui_now_s();
    for (int i = 0; i < 3; i++) {
        int x0 = s_tile_x[i][0], x1 = s_tile_x[i][1], cx = (x0 + x1) / 2;
        bool sel = (i == ui_card_cur);
        uint16_t c = sel ? C_ORANGE : C_LTGRAY;
        tft_rrect(x0, 89, x1, 116, C_LTGRAY);
        if (i == 0)      ico_params(cx, 99, c, sel, t);
        else if (i == 1) ico_presets(cx, 99, c, sel, t);
        else             ico_gear(cx, 99, c, sel, t);
        int tw = (int)strlen(s_tile_lbl[i]) * 6 - 1;
        tft_text(s_tile_lbl[i], cx - tw / 2, 107, sel ? C_ORANGE : C_DKGRAY, C_BLACK, 1);
    }

    // Sliding selection frame
    static float fr[4];
    static float last[4];
    int target[4];
    ui_home_tile_rect(ui_card_cur, target);
    for (int k = 0; k < 4; k++) fr[k] = ui_tween(fr[k], (float)target[k], &last[k]);
    tft_rrect((int)lrintf(fr[0]), (int)lrintf(fr[1]), (int)lrintf(fr[2]), (int)lrintf(fr[3]), C_ORANGE);

    draw_loop_bar(119);
}
