#include "ui_gfx.hpp"
#include <string.h>
#include "fonts/ui_font_silkscreen.h"
#include <math.h>

namespace ui {

static LGFX_Sprite *s_g = nullptr;

const Sprite SPR_USB = {5, 8,
    "..#.." ".###." "..#.." "#.#.#" "#.#.#" ".###." "..#.." "..#.."};
const Sprite SPR_SPK = {9, 7,
    "...#....." "..##..#.." "####...#." "####.#.#." "####...#." "..##..#.." "...#....."};
const Sprite SPR_KBD = {13, 7,
    "#############" "#.#.#.#.#.#.#" "#############" "#.#.#.#.#.#.#" "#############" "#..#######..#" "#############"};
const Sprite SPR_MOUSE = {7, 8,
    ".#####." "#..#..#" "#..#..#" "#######" "#.....#" "#.....#" "#.....#" ".#####."};
const Sprite SPR_NOTE = {7, 6,
    "..#####" "..#...#" "..#...#" "..#...#" "###.###" "###.###"};
const Sprite SPR_TERM = {11, 7,
    "###########" "#.........#" "#.#.......#" "#..#......#" "#.#..###..#" "#.........#" "###########"};
const Sprite SPR_CUBE = {9, 9,
    "..#######" ".#.....##" "#######.#" "#.....#.#" "#.....#.#" "#.....#.#" "#.....#.#" "#.....##." "#######.."};
const Sprite SPR_TRI_L = {3, 5, "..#" ".##" "###" ".##" "..#"};
const Sprite SPR_TRI_R = {3, 5, "#.." "##." "###" "##." "#.."};
const Sprite SPR_TRI_U = {5, 3, "..#.." ".###." "#####"};
const Sprite SPR_TRI_D = {5, 3, "#####" ".###." "..#.."};
const Sprite SPR_STEPS = {7, 4, "#.#.#.#" "#.#.#.#" "......." "#######"};
const Sprite SPR_SNAP = {7, 6, "##...##" "##...##" "##...##" "##...##" ".#...#." "..###.."};
const Sprite SPR_DAMP = {7, 7, "...#..." "..###.." ".#####." "#######" "#######" ".#####." "..###.."};
const Sprite SPR_PITCH = {7, 4, "......#" "....#.#" "..#.#.#" "#.#.#.#"};

// 1.5x set: the same icons redrawn by hand at 1.5x, since a 1.5x scale can't be done by
// repeating pixels. Draw these at 1x (1.5x) or 2x (3x).
const Sprite SPR_USB_M = {7, 12,
    "...#..." "..###.." ".#####." "...#..." "##.#.##" "##.#.##" ".#.#.#." "..###.." "...#..." "...#..."
    "..###.." "..###.."};
const Sprite SPR_SPK_M = {12, 11,
    ".....#......" "....##...#.." "...###....#." "######.#...#" "######..#..#" "######..#..#"
    "######..#..#" "######.#...#" "...###....#." "....##...#.." ".....#......"};
const Sprite SPR_KBD_M = {18, 9,
    "##################" "#................#" "#.##.##.##.##.##.#" "#................#" "#.##.##.##.##.##.#"
    "#................#" "#.##.########.##.#" "#................#" "##################"};
const Sprite SPR_MOUSE_M = {11, 12,
    ".#########." "#....#....#" "#....#....#" "#....#....#" "###########" "#.........#" "#.........#"
    "#.........#" "#.........#" "#.........#" ".#.......#." "..#######.."};
const Sprite SPR_NOTE_M = {11, 9,
    "...########" "...########" "...#......#" "...#......#" "...#......#" ".###....###" "####...####"
    "####...####" ".##.....##."};
const Sprite SPR_TERM_M = {16, 10,
    "################" "################" "#..............#" "#..#...........#" "#...#..........#"
    "#....#.........#" "#...#..........#" "#..#...#####...#" "#..............#" "################"};
const Sprite SPR_CUBE_M = {13, 13,
    "...##########" "..#........##" ".#........#.#" "##########..#" "#........#..#" "#........#..#"
    "#........#..#" "#........#..#" "#........#..#" "#........#..#" "#........#.#." "#........##.."
    "##########..."};
const Sprite SPR_TRI_L_M = {4, 7, "...#" "..##" ".###" "####" ".###" "..##" "...#"};
const Sprite SPR_TRI_R_M = {4, 7, "#..." "##.." "###." "####" "###." "##.." "#..."};
const Sprite SPR_STEPS_M = {11, 6, "#.#.#.#.#.#" "#.#.#.#.#.#" "#.#.#.#.#.#" "#.#.#.#.#.#" "..........." "###########"};
const Sprite SPR_SNAP_M = {11, 9,
    "###.....###" "###.....###" "###.....###" "###.....###" "###.....###" "###.....###" ".###...###."
    "..#######.." "...#####..."};
const Sprite SPR_DAMP_M = {11, 11,
    ".....#....." "....###...." "....###...." "...#####..." "..#######.." ".#########." "###########"
    "###########" "###########" ".#########." "...#####..."};
const Sprite SPR_SHAPE_M = {11, 6,
    ".........##" "........###" ".......###." ".....####.." "#######...." "#####......"};
const Sprite SPR_PITCH_M = {11, 6,
    "..........#" "........#.#" "......#.#.#" "....#.#.#.#" "..#.#.#.#.#" "#.#.#.#.#.#"};
// HOME (home.h): a bulb, its outline and, apart, its glass -- a lit lamp's glass is drawn in
// the lamp's own colour under the outline.
const Sprite SPR_BULB = {7, 10,
    "..###.." ".#...#." "#.....#" "#.....#" "#.....#" ".#...#." "..#.#.." "..###.." "..###.." "...#..."};
const Sprite SPR_BULB_LIT = {7, 10,
    "......." "..###.." ".#####." ".#####." ".#####." "..###.." "...#..." "......." "......." "......."};
const Sprite SPR_BULB_M = {11, 14,
    "...#####..." ".##.....##." ".#.......#." "#.........#" "#.........#" "#.........#" "#.........#"
    ".#.......#." "..#.....#.." "...#...#..." "...#####..." "...#####..." "....###...." ".....#....."};
const Sprite SPR_BULB_LIT_M = {11, 14,
    "..........." "...#####..." "..#######.." ".#########." ".#########." ".#########." ".#########."
    "..#######.." "...#####..." "....###...." "..........." "..........." "..........." "..........."};

// MIDI: the synth makers' wordmarks, small, drawn in whole pixels after their logos (one colour,
// the caller's). 7 px tall like scale-1 caps; scale 2 for scale-2 text.
const Sprite SPR_LOGO_KORG = {27, 7,
    "##..##..####..#####...#####" "##.##..##..##.##..##.##...." "####...##..##.##..##.##...." "###....##..##.#####..##.###" "####...##..##.####...##..##" "##.##..##..##.##.##..##..##" "##..##..####..##..##..#####"};
const Sprite SPR_LOGO_ROLAND = {27, 7,
    "####.......#..............#" "#...#......#..............#" "#...#..##..#..###.###...###" "####..#..#.#.#..#.#..#.#..#" "#.#...#..#.#.#..#.#..#.#..#" "#..#..#..#.#.#..#.#..#.#..#" "#...#..##..#..###.#..#..###"};

// The loading screen's two tiles. The Q is in the manner of the Binaris wordmark (one stroke
// weight, a boxy bowl with rounded corners, a dot where the last corner would be: the logo's S
// is a 45 degree line and dots), picked from five sketches on 2026-10-07.
const Sprite SPR_LOGO_Q = {24, 30,
    "...################....." ".####################..." "######################.." "######################.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####..............####.." "####...................." "####...................." "####...................." "####...............###.." "#################.#####." "#################.#####." ".################.#####." "...##############..###.."};
// Espressif's mark: the user's reference picture (2026-10-07) brought down to 29 px, a pixel
// lit where red covers half of it or more.
const Sprite SPR_LOGO_ESPRESSIF = {29, 29,
    "..............#####.........." "...............#######......." "....#...#####....######......" "...##..#########...######...." "...#..############..######..." "..#....############...####..." "..#.........########...####.." ".#....###.....#######...####." ".#..########....######..####." "##.###########...######..###." "#..#############..######..###" "#..##############..######.###" "#.#####....#######..#####..##" "#..#####.....######..#####.##" "#..########...#####..#####..#" "#..#########...#####..####..." "##..#########..#####..#####.." ".#.....#######..#####..####.." ".#.......######..####..####.." ".##.......#####..####..####.." "..#...###..####..####..####.." "..##.#####.#####.#####.###..." "...#..###..#####.#####......." "....#..#...#####.#####......." ".....#.....####..####....##.." "......##...####..####...##..." ".......###............###...." ".........###.......####......" "............########........."};

const Sprite *maker_logo(const char *maker) {
    if (strcmp(maker, "KORG") == 0) return &SPR_LOGO_KORG;
    if (strcmp(maker, "ROLAND") == 0) return &SPR_LOGO_ROLAND;
    return nullptr;
}

void bind(LGFX_Sprite *target) {
    s_g = target;
}

void clip(int x, int y, int w, int h) {
    s_g->setClipRect(x, y, w, h);
}

void unclip() {
    s_g->clearClipRect();
}

// target(): rect() (and so cut(), disc()) into a small RGB565 big-endian image instead of the frame.
static uint16_t *s_tgt = nullptr;
static int s_tgt_w = 0, s_tgt_h = 0;
void target(uint16_t *be565, int w, int h) {
    s_tgt = be565;
    s_tgt_w = w;
    s_tgt_h = h;
}

static inline uint16_t to_be565(uint32_t c) {
    uint16_t v = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
    return (uint16_t)(v >> 8 | v << 8);
}

void rect(float x, float y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    int x0 = (int)lroundf(x), y0 = (int)lroundf(y);
    if (s_tgt != nullptr) {
        uint16_t v = to_be565(c);
        for (int j = y0 < 0 ? 0 : y0; j < y0 + h && j < s_tgt_h; j++)
            for (int i = x0 < 0 ? 0 : x0; i < x0 + w && i < s_tgt_w; i++) s_tgt[j * s_tgt_w + i] = v;
        return;
    }
    s_g->fillRect(x0, y0, w, h, c);
}

void cut(int x, int y, int w, int h, uint32_t c) {
    rect(x + 1, y, w - 2, h, c);
    rect(x, y + 1, w, h - 2, c);
}

void frame_box(int x, int y, int w, int h, uint32_t c) {
    rect(x + 1, y, w - 2, 1, c);
    rect(x + 1, y + h - 1, w - 2, 1, c);
    rect(x, y + 1, 1, h - 2, c);
    rect(x + w - 1, y + 1, 1, h - 2, c);
}

void disc(float cx, float cy, float r, uint32_t c) {
    int rows = (int)(2 * r);
    for (int yy = 0; yy < rows; yy++) {
        float dy = yy - r + 0.5f;
        float hw = sqrtf(fmaxf(0.0f, r * r - dy * dy));
        int x0 = (int)lroundf(cx - hw), x1 = (int)lroundf(cx + hw);
        rect(x0, lroundf(cy - r) + yy, x1 - x0, 1, c);
    }
}

uint16_t *frame565() {
    return (uint16_t *)s_g->getBuffer();
}

void put565(int x, int y, uint16_t be) {
    uint16_t v = (uint16_t)(be << 8 | be >> 8);
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    s_g->fillRect(x, y, 1, 1, (r * 255 / 31) << 16 | (g * 255 / 63) << 8 | (b * 255 / 31));
}

void shade_disc(float cx, float cy, float r, float k) {
    uint16_t *px = (uint16_t *)s_g->getBuffer(); // RGB565, byte-swapped
    if (k <= 0 || px == nullptr) return;
    const uint32_t m = (uint32_t)lroundf((1 - fminf(k, 1.0f)) * 256);
    const int w = s_g->width(), h = s_g->height(), rows = (int)(2 * r);
    for (int yy = 0; yy < rows; yy++) { // the same rows as disc()
        int y = (int)lroundf(cy - r) + yy;
        if (y < 0 || y >= h) continue;
        float dy = yy - r + 0.5f;
        float hw = sqrtf(fmaxf(0.0f, r * r - dy * dy));
        int x0 = (int)lroundf(cx - hw), x1 = (int)lroundf(cx + hw);
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < w; x++) {
            uint16_t v = px[y * w + x];
            v = (uint16_t)(v << 8 | v >> 8);
            v = (uint16_t)((((v >> 11) & 31) * m >> 8) << 11 | (((v >> 5) & 63) * m >> 8) << 5 | ((v & 31) * m >> 8));
            px[y * w + x] = (uint16_t)(v << 8 | v >> 8);
        }
    }
}

void sprite(const Sprite &s, float x, float y, uint32_t c, int scale) {
    int x0 = (int)lroundf(x), y0 = (int)lroundf(y);
    for (int j = 0; j < s.h; j++) {
        for (int i = 0; i < s.w; i++) {
            if (s.rows[j * s.w + i] == '#') s_g->fillRect(x0 + i * scale, y0 + j * scale, scale, scale, c);
        }
    }
}

void image565(int x, int y, int w, int h, const uint8_t *be, float brightness, int scale) {
    for (int j = 0; j < h; j++) {
        const uint8_t *row = be + j * w * 2;
        int i = 0;
        while (i < w) { // one fillRect per horizontal run of equal pixels
            int run = 1;
            while (i + run < w && row[(i + run) * 2] == row[i * 2] && row[(i + run) * 2 + 1] == row[i * 2 + 1]) run++;
            uint16_t v = (uint16_t)(row[i * 2] << 8 | row[i * 2 + 1]);
            if (v != 0) {
                uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
                r = r << 3 | r >> 2;
                g = g << 2 | g >> 4;
                b = b << 3 | b >> 2;
                if (brightness < 1.0f) {
                    r = (uint32_t)(r * brightness);
                    g = (uint32_t)(g * brightness);
                    b = (uint32_t)(b * brightness);
                }
                rect(x + i * scale, y + j * scale, run * scale, scale, (r << 16) | (g << 8) | b);
            }
            i += run;
        }
    }
}

// --- text ---

// Small text uses the 10px font at 1:1; larger sizes scale the 8px font by whole pixels.
struct FontPick {
    const lgfx::GFXfont *font;
    int px; // pixel multiplier
};
static FontPick font_for(int scale) {
    if (scale <= 1) return {&ui_font_silkscreen_10_regular, 1};
    return {&ui_font_silkscreen_8_regular, scale};
}

int cap_height(int scale) {
    return scale <= 1 ? 7 : 5 * scale;
}

static const lgfx::GFXglyph &glyph(const lgfx::GFXfont &f, char ch) {
    uint8_t code = (uint8_t)ch;
    if (code < f.first || code > f.last) code = f.first; // space
    return f.glyph[code - f.first];
}

// Ink bounds (in font pixels) relative to the pen start.
static void measure(const lgfx::GFXfont &f, const char *s, int *left, int *width) {
    int pen = 0, min_x = 1 << 30, max_x = -(1 << 30);
    for (const char *p = s; *p; p++) {
        const lgfx::GFXglyph &g = glyph(f, *p);
        if (*p != ' ') {
            if (pen + g.xOffset < min_x) min_x = pen + g.xOffset;
            if (pen + g.xOffset + g.width > max_x) max_x = pen + g.xOffset + g.width;
        }
        pen += g.xAdvance;
    }
    if (min_x > max_x) {
        *left = 0;
        *width = 0;
    } else {
        *left = min_x;
        *width = max_x - min_x;
    }
}

int text_width(const char *s, int scale) {
    FontPick fp = font_for(scale);
    int l, w;
    measure(*fp.font, s, &l, &w);
    return w * fp.px;
}

int fit_scale(const char *s, int max_w, int max_scale) {
    int sc = max_scale;
    while (sc > 1 && text_width(s, sc) > max_w) sc--;
    return sc;
}

static int pen_start(const lgfx::GFXfont &f, int px, const char *s, float x, Align align) {
    int l, w;
    measure(f, s, &l, &w);
    float x0 = (align == CENTER) ? x - (w * px) / 2.0f - l * px
             : (align == RIGHT) ? x - w * px - l * px
             : x - l * px;
    return (int)lroundf(x0);
}

template <typename F_PX>
static void for_each_pixel(const lgfx::GFXfont &f, const char *s, int x0, int base, int px, F_PX fn) {
    int pen = 0;
    for (const char *p = s; *p; p++) {
        const lgfx::GFXglyph &g = glyph(f, *p);
        const uint8_t *bits = f.bitmap + g.bitmapOffset;
        for (int i = 0; i < g.width * g.height; i++) {
            if (bits[i >> 3] & (0x80 >> (i & 7))) {
                fn(x0 + (pen + g.xOffset + (i % g.width)) * px, base + (g.yOffset + i / g.width) * px);
            }
        }
        pen += g.xAdvance;
    }
}

int text(const char *s, float x, float y, uint32_t c, int scale, Align align) {
    FontPick fp = font_for(scale);
    int x0 = pen_start(*fp.font, fp.px, s, x, align);
    int base = (int)lroundf(y) + cap_height(scale);
    for_each_pixel(*fp.font, s, x0, base, fp.px, [&](int px, int py) { s_g->fillRect(px, py, fp.px, fp.px, c); });
    return text_width(s, scale);
}

CellGlyph cell_glyph(char ch, int w, int h, int s) {
    const lgfx::GFXfont &f = ui_font_silkscreen_10_regular;
    const lgfx::GFXglyph &g = glyph(f, ch);
    return {f.bitmap + g.bitmapOffset, g.width, g.height, (w - g.width * s) / 2, (h - 7 * s) / 2 + (7 + g.yOffset) * s, s};
}

int text_blocks(const char *s, float cx, float y, int scale, int16_t (*out)[2], int max) {
    FontPick fp = font_for(scale);
    int x0 = pen_start(*fp.font, fp.px, s, cx, CENTER);
    int base = (int)lroundf(y) + cap_height(scale);
    int n = 0;
    for_each_pixel(*fp.font, s, x0, base, fp.px, [&](int px, int py) {
        if (n < max) {
            out[n][0] = (int16_t)px;
            out[n][1] = (int16_t)py;
            n++;
        }
    });
    return n;
}

// --- curves ---

static float s_saw_shape = 0.0f;

void set_saw_shape(float shape) {
    s_saw_shape = shape;
}

float wave_y(haptic_type_t type, float u) {
    switch (type) {
        case HAPTIC_TYPE_SINE: return -sinf(u * 2.0f * (float)M_PI);
        case HAPTIC_TYPE_SAW: {
            // The control loop's law (control_task.c): the straight line, bent by SHAPE.
            float e = 1.0f - 2.0f * u;
            return e * (1.0f - s_saw_shape + s_saw_shape * e * e);
        }
        default: return 0.2f * sinf(u * 4.0f * (float)M_PI); // VISCOSE: drag, barely any shape
    }
}

void plot_curve(haptic_type_t type, int x, int y, int w, float amp, float periods, float phase,
                uint32_t c, int thickness, int morph_from, float blend) {
    int prev = 0;
    bool have_prev = false;
    for (int i = 0; i < w; i++) {
        float u = fmodf((float)i / w * periods + phase, 1.0f);
        if (u < 0) u += 1.0f;
        float v = wave_y(type, u);
        if (morph_from >= 0 && blend < 1.0f) v = wave_y((haptic_type_t)morph_from, u) * (1.0f - blend) + v * blend;
        int yy = (int)lroundf(y + v * amp);
        if (!have_prev) {
            prev = yy;
            have_prev = true;
        }
        int a = prev < yy ? prev : yy, b = prev < yy ? yy : prev;
        rect(x + i, a, thickness, b - a + thickness, c);
        prev = yy;
    }
}

// --- composite pieces ---

void keycap(int x, int y, const char *label, bool pressed, bool disabled) {
    const int face_h = KEY_H - 5;
    int fy = pressed ? 3 : 0;
    cut(x, y + 4, KEY_W, KEY_H - 4, disabled ? KEY_OFF_SIDE : KEY_SIDE);
    cut(x, y + fy, KEY_W, face_h, pressed ? AMBER : disabled ? KEY_OFF_FACE : KEY_FACE);
    text(label, x + KEY_W / 2.0f, y + fy + (face_h - cap_height(1)) / 2, BLACK, 1, CENTER);
}

void edit_arrows(float cx, float y, int w, int h, uint32_t c) {
    float ty = lroundf(y + (h - SPR_TRI_L_M.h) / 2.0f);
    sprite(SPR_TRI_L_M, cx - w / 2.0f - 11, ty, c);
    sprite(SPR_TRI_R_M, cx + w / 2.0f + 7, ty, c);
}

void ring(float cx, float cy, float r, uint32_t c, int thickness) {
    if (r <= 0) return;
    int n = (int)(r * 2.0f * (float)M_PI / 1.5f);
    if (n < 12) n = 12;
    for (int i = 0; i < n; i++) {
        float a = (float)i / n * 2.0f * (float)M_PI;
        float x = cx + r * cosf(a), y = cy + r * sinf(a);
        if ((x - CX) * (x - CX) + (y - CY) * (y - CY) < 119.0f * 119.0f) {
            rect(x - thickness / 2.0f, y - thickness / 2.0f, thickness, thickness, c);
        }
    }
}

void iris_mask(float r) {
    int rr = (int)lroundf(r / 6.0f) * 6;
    if (rr < 0) rr = 0;
    for (int y = 0; y < 240; y++) {
        float dy = y + 0.5f - CY;
        if (fabsf(dy) >= rr) {
            s_g->fillRect(0, y, 240, 1, BLACK);
            continue;
        }
        int hw = (int)lroundf(sqrtf(rr * rr - dy * dy));
        s_g->fillRect(0, y, CX - hw, 1, BLACK);
        s_g->fillRect(CX + hw, y, CX - hw, 1, BLACK);
    }
}

float rnd(int i, int k) {
    uint32_t x = (uint32_t)(i + 1) * 0x9e3779b1u ^ (uint32_t)(k + 7) * 0x85ebca77u;
    x ^= x >> 15;
    x *= 0x2c1b3c6du;
    x ^= x >> 12;
    return (float)(x % 100000u) / 100000.0f;
}

} // namespace ui
