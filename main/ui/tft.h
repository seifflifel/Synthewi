#pragma once
// ST7735 160x128 TFT driver + 5x8 font + drawing primitives.
// The ONLY module that talks to the panel's SPI bus. Drawing functions write
// into a RAM framebuffer; nothing reaches the panel until tft_flush(), which
// sends only the pixels that changed since the last flush.

#include <stdint.h>
#include <stdbool.h>

#define TFT_W     160
#define TFT_H     128

// RGB565 palette — panel is BGR (MADCTL bit3=1), so R and B channels are physically swapped.
// All values below are corrected for BGR: what we call C_RED sends B=31 on the wire so
// the panel displays it as red.  Green (middle channel) is unaffected.
#define C_BLACK   0xFFFFu  // light mode: C_BLACK renders WHITE on screen
#define C_WHITE   0x0000u  // light mode: C_WHITE renders BLACK on screen
#define C_GREEN   0x07E0u  // G channel symmetric — unchanged
#define C_RED     0x001Fu  // BGR-corrected (was 0xF800, appeared blue)
#define C_BLUE    0xC9E2u  // deep blue (~R16 G60 B206 on panel) — secondary accent; replaced
                           // yellow (0x07FF), which was unreadable on the white background
#define C_CYAN    0xFFE0u  // BGR-corrected (was 0x07FFu, appeared yellow)
#define C_ORANGE  0x029Fu  // warm amber — TE accent (R=255,G=160,B=0 as seen on panel)
#define C_DKGRAY  0x2104u
#define C_LTGRAY  0x8410u
#define C_TINT    0xBE9Fu  // pale amber (C_ORANGE at ~25 % on white) — fill under curves / scope
#define C_GRID    0xE71Cu  // faint gray (~224) — guide lines, unrecorded loop bars

// SPI bus + panel pins + ST7735 init sequence + framebuffer. Call once from the UI task.
void tft_init(void);

// Send everything drawn since the last flush (changed pixels only). Call once
// per UI tick, after all drawing. UI task only.
void tft_flush(void);
// Last non-empty flush: duration (us) and pixels sent. Diagnostics.
void tft_get_flush_stats(uint32_t *us, uint32_t *pixels);

void tft_fill(uint8_t x0,uint8_t y0,uint8_t x1,uint8_t y1, uint16_t c);
void tft_line(int x0, int y0, int x1, int y1, uint16_t c);
// Filled (active=green) or outline ring (inactive=gray) pad circle, radius 3.
void tft_pad_circle(int cx, int cy, bool active);
// draw_text with configurable scale (1=tiny, 2=normal). Font is ASCII 32-90 (no lowercase).
void tft_text(const char *str, int x, int y, uint16_t fg, uint16_t bg, int sc);
// Bold text: draw twice with 1px x-offset for a heavier stroke.
void tft_text_bold(const char *str, int x, int y, uint16_t fg, uint16_t bg, int sc);
// Outline rounded rectangle (corner radius ≈ 2 px).
void tft_rrect(int x0, int y0, int x1, int y1, uint16_t c);
// Filled rounded rectangle (corner radius ≈ 2 px).
void tft_rrect_fill(int x0, int y0, int x1, int y1, uint16_t c);

// Clipped primitives (any int coordinates; off-screen parts are skipped).
void tft_pixel(int x, int y, uint16_t c);
void tft_rect(int x0, int y0, int x1, int y1, uint16_t c);          // filled, clipped
void tft_line_dash(int x0, int y0, int x1, int y1, int every, uint16_t c); // 1 px every `every`
void tft_line_thick(int x0, int y0, int x1, int y1, uint16_t c);    // ~2 px (right/down)
void tft_disc(int cx, int cy, int r, uint16_t c);                   // filled circle
void tft_ring(int cx, int cy, int r, uint16_t c);                   // circle outline
// Arc of radius r from angle a0 to a1 (radians, 0 = right, +y = down).
void tft_arc(int cx, int cy, int r, float a0, float a1, uint16_t c);
