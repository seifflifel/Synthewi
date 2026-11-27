// PARAMS screen (redesign 2026-11, see docs/ui_sketch.html) — replaces ui_draw_main().
//
// Grid: 6 tiles (ENV FLT EG1 LFO FX GLD) with small icons. The selected tile is
// alive: ENV / FLT / EG1 morph between shapes, LFO / FX / GLD move with notes.
// The amber selection frame slides between tiles.
// Edit view: a big live icon of the group, then one column per parameter; the
// focus frame slides between columns, editing fills it amber.
//
// Icons are drawn from the engine's real values (tweened, so shapes ease into
// new values) and real timings (envelope, filter-envelope decay, LFO rate, echo
// delay). Redrawn every UI tick while visible.
//
// Input / state stays in ui.c (ui_main_card, ui_main_in_card, ui_main_param,
// ui_main_editing and the ui_main_* value mirrors).

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "ui_internal.h"

typedef struct { int x0, y0, x1, y1; } box_t;
typedef struct {
    uint16_t line;      // curve / outline colour
    int32_t  fill;      // fill under curves (-1 = none)
    bool     thick;     // ~2 px lines (big view)
    bool     live;      // follows notes (envelope dot, echo pulses, ...)
    bool     morph;     // grid "hover": cycles through shapes
    bool     big;       // edit view (extra guides, labels)
    int      focus;     // focused parameter (edit view), -1 = none
} ico_t;

static const int s_gx[3][2] = {{1, 52}, {54, 105}, {107, 158}};
static const int s_gy[2][2] = {{23, 74}, {76, 127}};
static const char * const s_short[6] = {"ENV", "FLT", "EG1", "LFO", "FX", "GLD"};
static const char * const s_name[6]  = {"ENVELOPE", "FILTER", "FILTER ENV", "LFO", "EFFECTS", "GLIDE"};
static const int s_np[6] = {4, 3, 2, 2, 5, 1};
static const char * const s_lbl[6][5] = {
    {"ATK", "DEC", "SUS", "REL"}, {"CUT", "RES", "TYPE"}, {"DEPTH", "DECAY"},
    {"RATE", "DEPTH"}, {"ECHO", "FDBK", "TIME", "REV", "SIZE"}, {"TIME"},
};

void ui_params_tile_rect(int i, int r[4])
{
    r[0] = s_gx[i % 3][0]; r[1] = s_gy[i / 3][0]; r[2] = s_gx[i % 3][1]; r[3] = s_gy[i / 3][1];
}
static void param_rect(int card, int i, int r[4])
{
    float cw = 160.0f / (float)s_np[card];
    r[0] = (int)lrintf(i * cw) + 1; r[1] = 95; r[2] = (int)lrintf((i + 1) * cw) - 2; r[3] = 123;
}

// ── Displayed values (0..1, filter type 0..2), eased toward the engine ──
enum { V_A, V_D, V_S, V_R, V_CUT, V_RES, V_TYPE, V_EGD, V_EGT, V_LR, V_LD,
       V_EA, V_EF, V_ET, V_RA, V_RS, V_GLD, V_N };
static float   s_v[V_N], s_vt[V_N];
static float   s_now;                       // seconds (ui_now_s) for this frame
static int64_t s_on_us, s_off_us;           // last note on / all-notes-off
static bool    s_held;
static float   s_lfo_phase, s_lfo_last;

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float u01(uint16_t v) { return (float)v / 10000.0f; }
static inline int   ri(float v) { return (int)lrintf(v); }

static void update_values(void)
{
    const float tg[V_N] = {
        u01(ui_main_adsr[0]), u01(ui_main_adsr[1]), u01(ui_main_adsr[2]), u01(ui_main_adsr[3]),
        u01(ui_main_flt_cut), u01(ui_main_flt_res), (float)ui_main_flt_type,
        u01(ui_main_eg1_depth), u01(ui_main_eg1_decay), u01(ui_main_lfo_rate), u01(ui_main_lfo_depth),
        u01(ui_main_ech_amt), u01(ui_main_ech_fb), u01(ui_main_ech_dly),
        u01(ui_main_rev_amt), u01(ui_main_rev_dec), u01(ui_main_gld),
    };
    for (int i = 0; i < V_N; i++) s_v[i] = ui_tween_eps(s_v[i], tg[i], &s_vt[i], 0.002f);

    s_now = ui_now_s();
    s_held = ui_note_state(&s_on_us, &s_off_us);

    float dt = s_now - s_lfo_last;
    s_lfo_last = s_now;
    if (dt < 0.0f || dt > 0.5f) dt = 0.0f;
    float hz = ui_lfo_hz(ui_main_lfo_rate);
    s_lfo_phase += 2.0f * (float)M_PI * (hz > 4.0f ? 4.0f : hz) * dt;   // display capped at 4 Hz
    if (s_lfo_phase > 2000.0f * (float)M_PI) s_lfo_phase -= 2000.0f * (float)M_PI;
}

static float since_us(int64_t t) { return t ? (float)(esp_timer_get_time() - t) / 1e6f : 1e9f; }
static float bump(float t) { return (t < 0.0f || t > 1.0f) ? 0.0f : expf(-t * 6.0f); }
static float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Grid morph: hold a shape, then ease into the next one (like LPF -> BPF -> HPF).
static float morph_seq(const float *seq, int n, float speed)
{
    float t = s_now * speed, fl = floorf(t);
    int i = ((int)fl) % n;
    float e = smooth(clampf((t - fl - 0.35f) / 0.65f, 0.0f, 1.0f));
    return seq[i] + (seq[(i + 1) % n] - seq[i]) * e;
}

// Curve helper: ys[i] = y of column x0+i. Fill toward `base`, then the line.
static void draw_ys(int x0, const float *ys, int n, int base, uint16_t lc, int32_t fc, bool thick)
{
    if (fc >= 0) for (int i = 0; i < n; i++) {
        int y = ri(ys[i]);
        if (y < base)      tft_rect(x0 + i, y + 1, x0 + i, base, (uint16_t)fc);
        else if (y > base) tft_rect(x0 + i, base, x0 + i, y - 1, (uint16_t)fc);
    }
    for (int i = 1; i < n; i++) {
        if (thick) tft_line_thick(x0 + i - 1, ri(ys[i - 1]), x0 + i, ri(ys[i]), lc);
        else       tft_line(x0 + i - 1, ri(ys[i - 1]), x0 + i, ri(ys[i]), lc);
    }
}

static void live_dot(float x, float y, const ico_t *st)
{
    tft_disc(ri(x), ri(y), st->big ? 3 : 2, st->line);
    tft_disc(ri(x), ri(y), st->big ? 1 : 0, C_BLACK);
}

// ── ENV ───────────────────────────────────────────────────────
typedef struct { float a, d, s, r; } env4_t;
static const env4_t s_env_shapes[4] = {   // pluck, pad, organ, swell
    {0.02f, 0.30f, 0.00f, 0.25f}, {0.65f, 0.40f, 0.75f, 0.75f},
    {0.02f, 0.10f, 1.00f, 0.12f}, {0.30f, 0.60f, 0.35f, 0.50f},
};

static void env_pts(const box_t *b, env4_t v, float px[5], float py[5])
{
    float w = (float)(b->x1 - b->x0);
    float aw = (0.03f + v.a * 0.27f) * w, dw = (0.03f + v.d * 0.27f) * w, sw = 0.18f * w, rw = (0.03f + v.r * 0.27f) * w;
    float tot = aw + dw + sw + rw, x0 = b->x0 + (w - tot) / 2.0f, top = b->y0 + 2.0f, bot = (float)b->y1;
    float sy = bot - v.s * (bot - top);
    px[0] = x0;                 py[0] = bot;
    px[1] = x0 + aw;            py[1] = top;
    px[2] = x0 + aw + dw;       py[2] = sy;
    px[3] = x0 + aw + dw + sw;  py[3] = sy;
    px[4] = x0 + tot;           py[4] = bot;
}

static float poly_y(const float *px, const float *py, int n, float x)
{
    if (x <= px[0]) return py[0];
    for (int i = 1; i < n; i++) if (x <= px[i]) {
        float dx = px[i] - px[i - 1];
        return dx < 0.001f ? py[i] : py[i - 1] + (py[i] - py[i - 1]) * (x - px[i - 1]) / dx;
    }
    return py[n - 1];
}

// Where the envelope is right now: segment 0..3 + fraction, from the real A/D/R times.
static bool env_now(int *seg, float *frac)
{
    float ta = ui_a_ms(ui_main_adsr[0]) / 1000.0f + 0.005f;
    float td = ui_d_ms(ui_main_adsr[1]) / 1000.0f + 0.005f;
    float tr = ui_r_ms(ui_main_adsr[3]) / 1000.0f + 0.005f;
    if (s_held) {
        float t = since_us(s_on_us);
        if (t < ta)      { *seg = 0; *frac = t / ta; return true; }
        if (t < ta + td) { *seg = 1; *frac = (t - ta) / td; return true; }
        *seg = 2; *frac = clampf((t - ta - td) / 0.8f, 0.0f, 1.0f); return true;
    }
    float t = since_us(s_off_us);
    if (s_off_us && s_off_us > s_on_us && t < tr) { *seg = 3; *frac = t / tr; return true; }
    return false;
}

static void draw_env(const box_t *b, const ico_t *st)
{
    env4_t v = {s_v[V_A], s_v[V_D], s_v[V_S], s_v[V_R]};
    if (st->morph) {
        float sa[4], sd[4], ss[4], sr[4];
        for (int i = 0; i < 4; i++) { sa[i] = s_env_shapes[i].a; sd[i] = s_env_shapes[i].d; ss[i] = s_env_shapes[i].s; sr[i] = s_env_shapes[i].r; }
        v.a = morph_seq(sa, 4, 0.5f); v.d = morph_seq(sd, 4, 0.5f); v.s = morph_seq(ss, 4, 0.5f); v.r = morph_seq(sr, 4, 0.5f);
    }
    float px[5], py[5];
    env_pts(b, v, px, py);
    if (st->big) {
        for (int i = 1; i < 4; i++) tft_line_dash(ri(px[i]), b->y0, ri(px[i]), b->y1, 2, C_GRID);
        tft_line(b->x0, b->y1, b->x1, b->y1, C_GRID);
    }
    for (int x = ri(px[0]); x <= ri(px[4]); x++) {
        int y = ri(poly_y(px, py, 5, (float)x));
        if (y < b->y1 && st->fill >= 0) tft_rect(x, y + 1, x, b->y1, (uint16_t)st->fill);
    }
    for (int i = 0; i < 4; i++) {
        uint16_t c = (st->focus == i) ? C_WHITE : st->line;
        if (st->thick) tft_line_thick(ri(px[i]), ri(py[i]), ri(px[i + 1]), ri(py[i + 1]), c);
        else           tft_line(ri(px[i]), ri(py[i]), ri(px[i + 1]), ri(py[i + 1]), c);
    }
    int seg; float f;
    if (st->live && env_now(&seg, &f))
        live_dot(px[seg] + (px[seg + 1] - px[seg]) * f, py[seg] + (py[seg + 1] - py[seg]) * f, st);
}

// ── FILTER ────────────────────────────────────────────────────
// x = log frequency across the box (2.4 decades), y = dB (+20 top .. -30 bottom).
static float flt_db(float f, float c, float q, int t)
{
    float r = powf(10.0f, (f - c) * 2.4f);
    float den = sqrtf((1.0f - r * r) * (1.0f - r * r) + (r / q) * (r / q));
    float m = (t == 0) ? 1.0f / den : (t == 2) ? r * r / den : (r / q) / den;
    return 20.0f * log10f(m > 1e-5f ? m : 1e-5f);
}

static void flt_ys(const box_t *b, float cut, float type, float *ys)
{
    float q = clampf(ui_filter_q((uint16_t)(s_v[V_RES] * 10000.0f)), 0.5f, 15.0f);
    float top = b->y0 + 1.0f, bot = (float)b->y1;
    int t0 = (int)floorf(type), t1 = t0 < 2 ? t0 + 1 : 2;
    float fr = type - (float)t0, w = (float)(b->x1 - b->x0);
    for (int x = b->x0; x <= b->x1; x++) {
        float f = (float)(x - b->x0) / w;
        float m = flt_db(f, cut, q, t0);
        if (fr > 0.001f) m += (flt_db(f, cut, q, t1) - m) * fr;
        ys[x - b->x0] = clampf(top + (20.0f - m) / 50.0f * (bot - top), top, bot);
    }
}

static void draw_flt(const box_t *b, const ico_t *st)
{
    static float ys[TFT_W];
    float cut = s_v[V_CUT];
    if (st->big) {
        int cx = ri(b->x0 + cut * (b->x1 - b->x0));
        tft_line_dash(cx, b->y0, cx, b->y1, 2, st->focus == 0 ? C_LTGRAY : C_GRID);
    }
    static const float seq[4] = {0, 1, 2, 1};
    float type = st->morph ? morph_seq(seq, 4, 0.55f) : s_v[V_TYPE];
    flt_ys(b, cut, type, ys);
    draw_ys(b->x0, ys, b->x1 - b->x0 + 1, b->y1, st->line, st->fill, st->thick);
}

// ── FILTER ENV ────────────────────────────────────────────────
// Dotted: where the envelope pushes the cutoff. Filled: the cutoff right now
// (jumps up on each note, falls back over the real decay time).
static void draw_eg1(const box_t *b, const ico_t *st)
{
    static float ys[TFT_W];
    float cut = s_v[V_CUT];
    float shift = amy_engine_fenv_depth_oct((uint16_t)(s_v[V_EGD] * 10000.0f)) / 8.0f;   // ~8 octaves across the box
    float peak = clampf(cut + shift, 0.0f, 1.0f);
    int n = b->x1 - b->x0 + 1;
    flt_ys(b, peak, s_v[V_TYPE], ys);
    for (int i = 0; i < n; i += 2) tft_pixel(b->x0 + i, ri(ys[i]), C_LTGRAY);

    float now_cut = cut;
    if (st->morph) {
        static const float seq[2] = {0, 1};
        now_cut = cut + (peak - cut) * morph_seq(seq, 2, 0.6f);
    } else if (st->live) {
        float tau = ui_eg1_decay_ms(ui_main_eg1_decay) / 3000.0f + 0.01f;
        float t = since_us(s_on_us);
        now_cut = cut + (peak - cut) * expf(-t / tau);
    }
    flt_ys(b, now_cut, s_v[V_TYPE], ys);
    draw_ys(b->x0, ys, n, b->y1, st->line, st->fill, st->thick);
}

// ── LFO ───────────────────────────────────────────────────────
static void draw_lfo(const box_t *b, const ico_t *st)
{
    static float ys[TFT_W];
    int n = b->x1 - b->x0 + 1, cy = (b->y0 + b->y1) / 2;
    float A = (b->y1 - b->y0) / 2.0f - 2.0f, cyc = 1.0f + s_v[V_LR] * 3.0f, ph = st->live ? s_lfo_phase : 0.0f;
    for (int i = 0; i < n; i++)
        ys[i] = cy - s_v[V_LD] * A * sinf(2.0f * (float)M_PI * ((float)i / (float)(n - 1) * cyc) - ph);
    tft_line_dash(b->x0, cy, b->x1, cy, 3, C_GRID);
    draw_ys(b->x0, ys, n, cy, st->focus == 1 ? C_WHITE : st->line, st->fill, st->thick);
    if (st->live) live_dot((float)(b->x1 - 1), ys[n - 2], st);
}

// ── FX: echo (Ableton-style tunnel of arcs) + reverb (rotating wireframe room) ──
static int echo_idx(uint16_t v)
{
    return v < 978 ? 0 : v < 2133 ? 1 : v < 3511 ? 2 : v < 5378 ? 3 : v < 8156 ? 4 : 5;
}
static const float s_echo_s[6] = {1.0f / 16, 1.0f / 8, 1.0f / 6, 1.0f / 4, 1.0f / 3, 1.0f / 2};

// TIME = spacing between arcs, FDBK = how many stay bright, ECHO = brightness.
// ext widens the arcs past a half circle (small tile, to match the cube's height).
static void echo_arcs(int cx, int cy, float rmax, const ico_t *st, float ext)
{
    float a0 = (float)M_PI - ext, a1 = 2.0f * (float)M_PI + ext;
    if (s_v[V_EA] < 0.02f) { tft_arc(cx, cy, ri(rmax), a0, a1, C_LTGRAY); return; }
    float k = 0.84f - s_v[V_ET] * 0.3f, fbk = 0.3f + s_v[V_EF] * 0.62f, delay = s_echo_s[echo_idx(ui_main_ech_dly)];
    float r = rmax, a = 0.4f + 0.6f * s_v[V_EA];
    for (int i = 0; r >= 2.0f && a > 0.03f; i++) {
        bool hit = st->live && bump(since_us(s_on_us) - i * delay) > 0.35f;
        uint16_t c = hit ? C_WHITE : a > 0.35f ? ((st->focus == 0 && i == 0) ? C_WHITE : st->line)
                                   : a > 0.12f ? C_LTGRAY : C_GRID;
        int rr = ri(r);
        tft_arc(cx, cy, rr, a0, a1, c);
        tft_disc(cx, cy - rr, (st->big && hit) ? 1 : 0, C_WHITE);
        r *= k; a *= fbk;
    }
}

static void draw_echo_big(const box_t *b, const ico_t *st)
{
    int cx = (b->x0 + b->x1) / 2, row = b->y1 - 3, cy = b->y1 - 9, sel = echo_idx(ui_main_ech_dly);
    for (int i = 0; i < 6; i++) {                  // sync row: 1/16 .. 1/2
        int x = ri(b->x0 + 5 + (b->x1 - b->x0 - 10) * i / 5.0f);
        if (i == sel) {
            uint16_t c = st->focus == 2 ? C_WHITE : st->line;
            tft_rect(x - 3, row, x + 3, row, c); tft_rect(x, row - 3, x, row + 3, c); tft_rect(x - 1, row - 1, x + 1, row + 1, c);
        } else {
            tft_rect(x - 2, row, x + 2, row, C_LTGRAY); tft_rect(x, row - 2, x, row + 2, C_LTGRAY);
        }
    }
    float rmax = fminf((b->x1 - b->x0) / 2.0f - 1.0f, (float)(cy - b->y0 - 1));
    echo_arcs(cx, cy, rmax, st, 0.0f);
}

static const int8_t s_cv[8][3] = {{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
static const uint8_t s_ce[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};

static void cube(float cx, float cy, float s, float ang, uint16_t c, bool dash)
{
    const float tilt = 0.45f, ca = cosf(ang), sa = sinf(ang), ct = cosf(tilt), stl = sinf(tilt);
    int P[8][2];
    for (int i = 0; i < 8; i++) {
        float x = s_cv[i][0], y = s_cv[i][1], z = s_cv[i][2];
        float x1 = x * ca - z * sa, z1 = x * sa + z * ca;
        float y2 = y * ct - z1 * stl, z2 = y * stl + z1 * ct;
        float k = 3.2f / (3.2f + z2);
        P[i][0] = ri(cx + x1 * s * k); P[i][1] = ri(cy + y2 * s * k);
    }
    for (int e = 0; e < 12; e++) {
        const int *p = P[s_ce[e][0]], *q = P[s_ce[e][1]];
        if (dash) tft_line_dash(p[0], p[1], q[0], q[1], 2, c); else tft_line(p[0], p[1], q[0], q[1], c);
    }
}

// SIZE = cube size; the amount adds nested "rooms" (reflections, big view only).
static void verb_cube(float cx, float cy, float rr, const ico_t *st, bool rooms)
{
    float kick = st->live ? bump(since_us(s_on_us)) * 0.1f : 0.0f, ang = st->live ? s_now * 0.7f : 0.6f;
    float s = rr * (0.42f + 0.22f * s_v[V_RS]) * (1.0f + kick);
    if (s_v[V_RA] < 0.02f) { cube(cx, cy, s, ang, C_LTGRAY, true); return; }
    if (rooms) {
        int inner = s_v[V_RA] > 0.66f ? 2 : s_v[V_RA] > 0.33f ? 1 : 0;
        for (int i = inner; i >= 1; i--) cube(cx, cy, s * (1.0f - i * 0.3f), -ang * (1.0f + i * 0.5f), C_LTGRAY, true);
    }
    cube(cx, cy, s, ang, (st->focus >= 3) ? C_WHITE : st->line, false);
}

static void draw_fx(const box_t *b, const ico_t *st)
{
    int mid = (b->x0 + b->x1) / 2;
    if (st->big) {
        tft_line_dash(mid, b->y0, mid, b->y1, 2, C_GRID);
        tft_text("ECHO", b->x0 + 1, b->y0, (st->focus >= 0 && st->focus <= 2) ? C_WHITE : C_LTGRAY, C_BLACK, 1);
        tft_text("REVERB", mid + 3, b->y0, st->focus >= 3 ? C_WHITE : C_LTGRAY, C_BLACK, 1);
        int top = b->y0 + 10;
        box_t eb = {b->x0, top, mid - 2, b->y1};
        draw_echo_big(&eb, st);
        verb_cube((mid + 2 + b->x1) / 2.0f, (top + b->y1) / 2.0f, fminf((float)(b->x1 - mid - 2), (float)(b->y1 - top)) / 2.0f * 1.25f, st, true);
        return;
    }
    // Small tile: echo and reverb the same height, each centred in its half,
    // both on the tile's horizontal centre line.
    float hw = (b->x1 - b->x0) / 4.0f, my = (b->y0 + b->y1) / 2.0f, ext = 0.5f;
    float rm = fminf(hw - 1.0f, (b->y1 - b->y0) / 2.0f - 1.0f);
    echo_arcs(ri(b->x0 + hw), ri(my + rm * (1.0f - sinf(ext)) / 2.0f), rm, st, ext);
    verb_cube(b->x1 - hw, my, rm, st, false);
}

// ── GLIDE ─────────────────────────────────────────────────────
static void draw_gld(const box_t *b, const ico_t *st)
{
    static float ys[TFT_W];
    float g = s_v[V_GLD], w = (float)(b->x1 - b->x0), lo = b->y1 - 3.0f, hi = b->y0 + 4.0f;
    float xa = b->x0 + w * 0.28f, gw = g * w * 0.45f;
    int n = b->x1 - b->x0 + 1;
    for (int i = 0; i < n; i++) {
        float x = (float)(b->x0 + i);
        if (x <= xa) ys[i] = lo;
        else if (x >= xa + gw) ys[i] = hi;
        else ys[i] = lo + (hi - lo) * smooth((x - xa) / gw);
    }
    if (st->big && g > 0.02f) {           // dotted: the same move without glide
        tft_line_dash(b->x0, ri(lo), ri(xa), ri(lo), 2, C_LTGRAY);
        tft_line_dash(ri(xa), ri(lo), ri(xa), ri(hi), 2, C_LTGRAY);
        tft_line_dash(ri(xa), ri(hi), b->x1, ri(hi), 2, C_LTGRAY);
    }
    draw_ys(b->x0, ys, n, b->y1, st->line, st->fill, st->thick);
    if (st->big && g < 0.01f) tft_text("OFF", ri(b->x0 + w * 0.7f) - 8, b->y1 - 12, C_LTGRAY, C_BLACK, 1);
    if (st->live) {
        float u = fmodf(s_now * 0.45f, 1.25f);
        int i = ri((u > 1.0f ? 1.0f : u) * (n - 1));
        live_dot((float)(b->x0 + i), ys[i] - (st->big ? 3.0f : 2.0f), st);
    }
}

typedef void (*icon_fn)(const box_t *, const ico_t *);
static const icon_fn s_draw[6] = {draw_env, draw_flt, draw_eg1, draw_lfo, draw_fx, draw_gld};

// ── Value text (real units, uppercase font) ───────────────────
static void fmt_ms(char *b, size_t n, uint32_t ms)
{
    if (ms < 1000) snprintf(b, n, "%uMS", (unsigned)ms);
    else           snprintf(b, n, "%u.%uS", (unsigned)(ms / 1000), (unsigned)((ms % 1000) / 100));
}

static void fmt_param(int card, int p, char *b, size_t n)
{
    static const char * const flt[3] = {"LPF", "BPF", "HPF"};
    static const char * const frac[6] = {"1/16", "1/8", "1/6", "1/4", "1/3", "1/2"};
    switch (card) {
    case 0:
        if (p == 0) fmt_ms(b, n, ui_a_ms(ui_main_adsr[0]));
        else if (p == 1) fmt_ms(b, n, ui_d_ms(ui_main_adsr[1]));
        else if (p == 2) snprintf(b, n, "%u%%", (unsigned)ui_s_pct(ui_main_adsr[2]));
        else fmt_ms(b, n, ui_r_ms(ui_main_adsr[3]));
        break;
    case 1:
        if (p == 0) {
            float hz = ui_filter_hz(ui_main_flt_cut);
            if (hz >= 1000.0f) { unsigned d = (unsigned)lrintf(hz / 100.0f); snprintf(b, n, "%u.%uK", d / 10, d % 10); }
            else snprintf(b, n, "%uHZ", (unsigned)hz);
        } else if (p == 1) {
            unsigned q = (unsigned)lrintf(ui_filter_q(ui_main_flt_res) * 10.0f);
            snprintf(b, n, "Q%u.%u", q / 10, q % 10);
        } else snprintf(b, n, "%s", flt[ui_main_flt_type % 3]);
        break;
    case 2:
        if (p == 0) { unsigned t = ui_fenv_oct10(ui_main_eg1_depth); snprintf(b, n, "+%u.%u OCT", t / 10, t % 10); }
        else fmt_ms(b, n, ui_eg1_decay_ms(ui_main_eg1_decay));
        break;
    case 3:
        if (p == 0) { unsigned r = (unsigned)lrintf(ui_lfo_hz(ui_main_lfo_rate) * 10.0f); snprintf(b, n, "%u.%uHZ", r / 10, r % 10); }
        else { unsigned t = ui_lfo_oct10(ui_main_lfo_depth); snprintf(b, n, "%u.%u OCT", t / 10, t % 10); }
        break;
    case 4: {
        const uint16_t v[5] = {ui_main_ech_amt, ui_main_ech_fb, 0, ui_main_rev_amt, ui_main_rev_dec};
        if (p == 2) snprintf(b, n, "%s", frac[echo_idx(ui_main_ech_dly)]);
        else snprintf(b, n, "%u%%", (unsigned)(v[p] / 100));
        break;
    }
    default: {
        uint32_t ms = ui_glide_ms(ui_main_gld);
        if (ms == 0) snprintf(b, n, "OFF"); else fmt_ms(b, n, ms);
        break;
    }
    }
}

// Bar position 0..1 for a parameter (stepped ones by step).
static float param_frac(int card, int p)
{
    switch (card) {
    case 0: return u01(ui_main_adsr[p]);
    case 1: return p == 0 ? u01(ui_main_flt_cut) : p == 1 ? u01(ui_main_flt_res) : ui_main_flt_type / 2.0f;
    case 2: return p == 0 ? u01(ui_main_eg1_depth) : u01(ui_main_eg1_decay);
    case 3: return p == 0 ? u01(ui_main_lfo_rate) : u01(ui_main_lfo_depth);
    case 4: {
        const uint16_t v[5] = {ui_main_ech_amt, ui_main_ech_fb, 0, ui_main_rev_amt, ui_main_rev_dec};
        return p == 2 ? echo_idx(ui_main_ech_dly) / 5.0f : u01(v[p]);
    }
    default: return u01(ui_main_gld);
    }
}

// ── Screens ───────────────────────────────────────────────────
static float s_fr[4], s_fr_t[4];   // sliding selection / focus frame

static void slide_frame(const int target[4], bool filled)
{
    for (int k = 0; k < 4; k++) s_fr[k] = ui_tween(s_fr[k], (float)target[k], &s_fr_t[k]);
    if (filled) tft_rrect_fill(ri(s_fr[0]), ri(s_fr[1]), ri(s_fr[2]), ri(s_fr[3]), C_ORANGE);
    else        tft_rrect(ri(s_fr[0]), ri(s_fr[1]), ri(s_fr[2]), ri(s_fr[3]), C_ORANGE);
}

static void draw_grid(void)
{
    for (int i = 0; i < 6; i++) {
        int r[4];
        ui_params_tile_rect(i, r);
        bool sel = (i == ui_main_card);
        tft_rrect(r[0], r[1], r[2], r[3], C_LTGRAY);
        tft_disc(r[0] + 5, r[1] + 5, 2, sel ? C_ORANGE : C_LTGRAY);
        tft_text(s_short[i], r[0] + 10, r[1] + 2, sel ? C_WHITE : C_DKGRAY, C_BLACK, 1);
        box_t b = {r[0] + 4, r[1] + 13, r[2] - 4, r[3] - 5};
        ico_t st = {sel ? C_ORANGE : C_LTGRAY, sel ? C_TINT : C_GRID, false, sel && i >= 3, sel && i < 3, false, -1};
        s_draw[i](&b, &st);
    }
    int t[4];
    ui_params_tile_rect(ui_main_card, t);
    slide_frame(t, false);
}

static void draw_edit(void)
{
    int c = ui_main_card, np = s_np[c];
    tft_disc(5, 27, 2, C_ORANGE);
    tft_text(s_name[c], 10, 24, C_WHITE, C_BLACK, 1);
    tft_rrect(1, 33, 158, 92, C_GRID);
    box_t b = {6, 37, 153, 88};
    ico_t st = {C_ORANGE, C_TINT, true, true, false, true, ui_main_param};
    s_draw[c](&b, &st);

    int t[4];
    param_rect(c, ui_main_param, t);
    slide_frame(t, ui_main_editing);
    for (int i = 0; i < np; i++) {
        int r[4];
        param_rect(c, i, r);
        bool f = (i == ui_main_param), ed = f && ui_main_editing;
        uint16_t lab = ed ? C_BLACK : f ? C_ORANGE : C_LTGRAY;
        uint16_t val = ed ? C_BLACK : f ? C_WHITE : C_DKGRAY;
        uint16_t bar = ed ? C_BLACK : f ? C_ORANGE : C_LTGRAY;
        uint16_t trk = ed ? C_TINT : C_GRID;
        int cx = (r[0] + r[2]) / 2;
        const char *l = s_lbl[c][i];
        tft_text(l, cx - ((int)strlen(l) * 6 - 1) / 2, 98, lab, ed ? C_ORANGE : C_BLACK, 1);
        char buf[32];
        fmt_param(c, i, buf, sizeof(buf));
        tft_text(buf, cx - ((int)strlen(buf) * 6 - 1) / 2, 108, val, ed ? C_ORANGE : C_BLACK, 1);
        int bx0 = r[0] + 4, bx1 = r[2] - 4;
        tft_rect(bx0, 119, bx1, 119, trk);
        tft_rect(bx0, 118, bx0 + ri(param_frac(c, i) * (bx1 - bx0)), 119, bar);
    }
}

void ui_draw_params(void)
{
    update_values();
    // Grid <-> edit view: the transition box covers the move, so the frame snaps.
    static int s_mode = -1;
    int mode = ui_main_in_card ? 100 + ui_main_card : 0;
    if (mode != s_mode) { s_mode = mode; for (int k = 0; k < 4; k++) s_fr_t[k] = -1000.0f; }
    tft_rect(0, 22, TFT_W - 1, TFT_H - 1, C_BLACK);
    if (ui_main_in_card) draw_edit();
    else                 draw_grid();
}
