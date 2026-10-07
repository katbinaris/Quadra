#pragma once
// Pixel UI toolkit: the drawing primitives every screen is built from -- a direct port of the
// helpers in the approved browser mockups (DEVELOPMENT_PLAN.md "Pixel UI"), so layouts carry
// over pixel for pixel. Everything draws into one bound sprite (display_task.cpp's full-frame
// canvas) with plain fillRect()s: no anti-aliasing anywhere, by design.

#include <stdint.h>
#include "lgfx_config.hpp"
#include "haptic_params.h"

namespace ui {

constexpr int CX = 120; // panel center
constexpr int CY = 120;

// Palette rule: black background plus four colors -- anything new has to earn its place.
// uint32_t = RGB888 to LovyanGFX (a plain int would be read as RGB565).
constexpr uint32_t BLACK = 0x000000u;
constexpr uint32_t WHITE = 0xFFFFFFu;
constexpr uint32_t GREY = 0x6E6E6Eu;  // labels, hints
constexpr uint32_t DARK = 0x3A3A3Au;  // lines, empty parts
constexpr uint32_t AMBER = 0xFFC94Du; // focus, editing, pressed
// Keycap shading -- part of the keycap sprite itself, not general UI colors.
constexpr uint32_t KEY_FACE = 0xE4E4E4u;
constexpr uint32_t KEY_SIDE = 0x8A8A8Au;
constexpr uint32_t KEY_OFF_FACE = 0x4A4A4Au;
constexpr uint32_t KEY_OFF_SIDE = 0x262626u;

enum Align { LEFT, CENTER, RIGHT };

// 1-bit sprite: `rows` is w*h chars, '#' = set, row after row.
struct Sprite {
    uint8_t w;
    uint8_t h;
    const char *rows;
};
extern const Sprite SPR_USB, SPR_SPK, SPR_KBD, SPR_MOUSE, SPR_NOTE, SPR_TERM, SPR_CUBE;
extern const Sprite SPR_TRI_L, SPR_TRI_R, SPR_TRI_U, SPR_TRI_D;
extern const Sprite SPR_STEPS, SPR_SNAP, SPR_DAMP, SPR_PITCH;
// The same icons hand-redrawn at 1.5x (repeating pixels can't make 1.5x): draw at 1x for
// 1.5x, at 2x for 3x.
extern const Sprite SPR_USB_M, SPR_SPK_M, SPR_KBD_M, SPR_MOUSE_M, SPR_NOTE_M, SPR_TERM_M, SPR_CUBE_M;
extern const Sprite SPR_TRI_L_M, SPR_TRI_R_M;
extern const Sprite SPR_STEPS_M, SPR_SNAP_M, SPR_DAMP_M, SPR_SHAPE_M, SPR_PITCH_M;
// HOME's bulb: outline and glass, at 1x and hand-drawn 1.5x.
extern const Sprite SPR_BULB, SPR_BULB_LIT, SPR_BULB_M, SPR_BULB_LIT_M;
// MIDI: KORG's and Roland's wordmarks (7 px tall), and the one for a synth's maker (nullptr: none).
extern const Sprite SPR_LOGO_KORG, SPR_LOGO_ROLAND;
extern const Sprite SPR_LOGO_Q, SPR_LOGO_ESPRESSIF; // the loading screen's tiles
const Sprite *maker_logo(const char *maker);

void bind(LGFX_Sprite *target);
void clip(int x, int y, int w, int h);
void unclip();

void rect(float x, float y, int w, int h, uint32_t c);
// Until target(nullptr, 0, 0): rect(), cut() and disc() draw into `be565` (w x h, RGB565
// big-endian, the icon format) instead of the frame -- HOME's lamp as an idle-screen icon.
void target(uint16_t *be565, int w, int h);
void cut(int x, int y, int w, int h, uint32_t c);       // rect with the 4 corner pixels cut
void frame_box(int x, int y, int w, int h, uint32_t c); // 1px outline, corners cut
void disc(float cx, float cy, float r, uint32_t c);
void shade_disc(float cx, float cy, float r, float k); // darken what's under a disc by k (0..1)
// Per-pixel art that can't be made of rectangles (MUSIC's record, ui_vinyl.cpp): the frame as
// RGB565 big-endian, or nullptr on the host preview -- then put565() draws one pixel at a time.
uint16_t *frame565();
void put565(int x, int y, uint16_t be);
void sprite(const Sprite &s, float x, float y, uint32_t c, int scale = 1);
// Full-color w x h image, RGB565 big-endian (the HID icon upload format), drawn 1:1; black
// pixels are left alone. `brightness` < 1 dims it (still pixel-exact): full-colour icons
// can't take the palette's GREY the way 1-bit sprites do, so "inactive" is a dimmed copy.
// `scale` repeats each pixel scale x scale (whole-pixel, so still pixel-perfect).
void image565(int x, int y, int w, int h, const uint8_t *be, float brightness = 1.0f, int scale = 1);

// Silkscreen, drawn pixel by pixel. `scale` 1 = the 10px font (small text, caps 7px); 2, 3, 4
// = the 8px font doubled/tripled/quadrupled (caps 10/15/20px) -- whole-pixel scaling keeps it
// crisp. `y` is the top of the caps; alignment uses the ink bounds, not the advance box, so
// centered text is optically centered. Both return the ink width.
int cap_height(int scale);
int text_width(const char *s, int scale = 1);
int text(const char *s, float x, float y, uint32_t c, int scale = 1, Align align = LEFT);
// Largest scale <= max_scale at which `s` fits in max_w.
int fit_scale(const char *s, int max_w, int max_scale);
// Top-left corners of the scale x scale blocks that make up `s` (centered on cx) -- the
// loading screen's particles. Returns the count written (at most max).
int text_blocks(const char *s, float cx, float y, int scale, int16_t (*out)[2], int max);

// Force-curve shapes for the three haptic types, one period over u in [0,1).
float wave_y(haptic_type_t type, float u);
// The Haptics SHAPE setting (0..0.9): every SAW curve drawn after this bends with it.
void set_saw_shape(float shape);
// A curve `w` px wide, `periods` periods, shifted by `phase` (periods). With morph_from >= 0
// it blends from that type's shape (blend 0) to `type`'s (blend 1).
void plot_curve(haptic_type_t type, int x, int y, int w, float amp, float periods, float phase,
                uint32_t c, int thickness, int morph_from, float blend);
inline void wave(haptic_type_t type, int x, int y, int w, float amp, float periods, uint32_t c) {
    plot_curve(type, x, y, w, amp, periods, 0.0f, c, 1, -1, 1.0f);
}

// 3D keycap sized for a 10px label: face + 4px side, face drops 3px when pressed.
void keycap(int x, int y, const char *label, bool pressed, bool disabled);
constexpr int KEY_W = 26;
constexpr int KEY_H = 22;

void edit_arrows(float cx, float y, int w, int h, uint32_t c); // < value > while editing (1.5x arrows)
void ring(float cx, float cy, float r, uint32_t c, int thickness);
void iris_mask(float r); // black outside a centered circle of radius r (6px steps)

// Deterministic per-(i, k) pseudo-random in [0,1) -- the same hash as the mockups, so the
// particle layouts match them exactly.
float rnd(int i, int k);

} // namespace ui
