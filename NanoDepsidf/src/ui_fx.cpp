#include "ui_fx.hpp"
#include "ui_gfx.hpp"
#include "app_colors.h"
#include "esp_attr.h"
#include <math.h>
#include <string.h>

namespace ui {

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float lerpf(float a, float b, float k) { return a + (b - a) * k; }
static inline float ease_out(float k) { k = clampf(k, 0, 1); return 1 - (1 - k) * (1 - k) * (1 - k); }
static inline float ease_in_out(float k) {
    k = clampf(k, 0, 1);
    return k < 0.5f ? 4 * k * k * k : 1 - powf(-2 * k + 2, 3) / 2;
}
static inline bool in_circle(float x, float y, float r) {
    return (x - CX) * (x - CX) + (y - CY) * (y - CY) <= r * r;
}

// --- loading screen: Big bang ---

// "QUADRA" at scale 3 is ~130 blocks; a 12-character word (fx_set_word) at scale 2 fits too.
#define LOGO_MAX_BLOCKS 512
// The block lists live in PSRAM (EXT_RAM_BSS_ATTR): a few hundred entries a frame, which the
// cache keeps; internal RAM is short since the frame sprite moved there.
EXT_RAM_BSS_ATTR static int16_t s_logo[LOGO_MAX_BLOCKS][2];
static int s_logo_n = 0;
static int s_logo_scale = 3; // the block size: the largest scale at which the word fits
// Per-block scatter point and return delay, fixed at init (same hash as the mockup).
EXT_RAM_BSS_ATTR static int16_t s_logo_scatter[LOGO_MAX_BLOCKS][2];
EXT_RAM_BSS_ATTR static uint16_t s_logo_delay_ms[LOGO_MAX_BLOCKS];
EXT_RAM_BSS_ATTR static uint8_t s_logo_tint[LOGO_MAX_BLOCKS]; // 0 amber, 1 grey, 2 white while in flight

static const float SPARK_X = 120, SPARK_Y = 110;

static void boot_tail(uint32_t e, uint32_t t0) {
    static const char maker[] = "KAFI DEVICES";
    if (e > t0) {
        char buf[sizeof(maker)];
        uint32_t n = (e - t0) / 30;
        if (n > sizeof(maker) - 1) n = sizeof(maker) - 1;
        for (uint32_t i = 0; i < n; i++) buf[i] = maker[i];
        buf[n] = '\0';
        text(buf, 120, 128, GREY, 1, CENTER);
    }
    if (e > t0 + 500) {
        uint32_t n = (e - t0 - 500) / 60;
        if (n > 12) n = 12;
        for (uint32_t i = 0; i < 12; i++) rect(72 + i * 8, 152, 7, 5, i < n ? WHITE : DARK);
    }
}

void fx_boot(uint32_t e) {
    if (e < 380) {
        int st = e / 95, s = 1 + st * 2;
        rect(SPARK_X - s / 2.0f, SPARK_Y - s / 2.0f, s, s, (st % 2) ? WHITE : AMBER);
        return;
    }
    if (e < 700) ring(SPARK_X, SPARK_Y, (e - 380) * 0.45f, e < 540 ? WHITE : GREY, 2);
    float k1 = ease_out((e - 380) / 420.0f);
    for (int i = 0; i < s_logo_n; i++) {
        float sx = s_logo_scatter[i][0], sy = s_logo_scatter[i][1];
        float k2 = ease_in_out(((float)e - 1000.0f - s_logo_delay_ms[i]) / 480.0f);
        float x, y;
        if (e < 1000) {
            x = lerpf(SPARK_X, sx, k1);
            y = lerpf(SPARK_Y, sy, k1);
        } else {
            x = lerpf(sx, s_logo[i][0], k2);
            y = lerpf(sy, s_logo[i][1], k2);
        }
        bool home = e >= 1000 && k2 >= 1.0f;
        uint32_t col = home ? (e < 1900 ? AMBER : WHITE)
                     : s_logo_tint[i] == 0 ? AMBER : s_logo_tint[i] == 1 ? GREY : WHITE;
        rect(x, y, s_logo_scale, s_logo_scale, col);
    }
    boot_tail(e, 1950);
}

// --- attract: arcade attract mode ---
// The idle screen (DEVELOPMENT_PLAN.md "Idle screen: arcade attract mode"): the active app's
// 48x48 icon -- or the QUADRA wordmark at 2x -- in one of three routines, picked at random:
//   JUMP   always on screen: hops, a big jump with afterimages, a hard landing (squash,
//          shake, dust, debris), a gleam, side hops, a spinning jump, breathing with sparkles.
//   BOUNCE rattles around inside the glass: squash against the rim, rim flash, sparks,
//          afterimages; parallax stars in three sizes behind.
//   BOOM   fuse, pixel explosion (core, colour ring, smoke, debris, shake), a springy pop,
//          bobbing with sparkles, implode into a flash.
// When a routine's loop ends the next is picked (never the same twice running). Whole pixels
// only: the sprite is scaled nearest-neighbour. Colours: the UI palette plus three accent
// colours -- the profile's plasma_heat, sampled from its icon, or AMBER for QUADRA.

#define ATTRACT_ICON 48
static inline float rndf(int i, int k) { return rnd(i, k); }

// Everything the routines draw goes through here, offset by the screen shake.
static int s_ox = 0, s_oy = 0;
static inline void pt(float x, float y, uint32_t c) { rect(lroundf(x) + s_ox, lroundf(y) + s_oy, 1, 1, c); }
static inline void box(float x, float y, int w, int h, uint32_t c) { rect(lroundf(x) + s_ox, lroundf(y) + s_oy, w, h, c); }
static void seg(float fx0, float fy0, float fx1, float fy1, uint32_t c) {
    int x0 = lroundf(fx0), y0 = lroundf(fy0), x1 = lroundf(fx1), y1 = lroundf(fy1);
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        pt(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
// Dithered disc: dens 2 = solid, 1 = checkerboard, 0 = every fourth pixel.
static void blob(float fcx, float fcy, int r, uint32_t c, int dens) {
    int cx = lroundf(fcx), cy = lroundf(fcy);
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y > r * r + r * 0.6f) continue;
            int X = cx + x, Y = cy + y;
            if (dens == 1 && ((X + Y) & 1)) continue;
            if (dens == 0 && ((X & 1) | (Y & 1))) continue;
            pt(X, Y, c);
        }
    }
}
static void dotted_ring(float cx, float cy, float r, uint32_t c, int step) {
    int n = (int)lroundf(2 * (float)M_PI * r);
    if (n < 8) n = 8;
    for (int s = 0; s < n; s += step) {
        float t = (float)s / n * 2 * (float)M_PI;
        pt(cx + cosf(t) * r, cy + sinf(t) * r, c);
    }
}

// The sprite: the icon (RGB565 BE, black = transparent) or the wordmark's 1-bit mask.
#define WORD_MAX_W 80 // 12 characters of the 10px font, drawn 1x (JUMP keeps it inside the glass)
#define WORD_MAX_H 12
// Widest wordmark (1x px) still drawn at 2x: JUMP's +-34 px hops with a 1.38x squash need the
// sprite under ~119 px to stay inside the glass.
#define WORD_2X_MAX_W 56
EXT_RAM_BSS_ATTR static uint8_t s_word[WORD_MAX_W * WORD_MAX_H];
static int s_word_w = 0, s_word_h = 0, s_word_scale = 2;
static bool s_word_custom = false; // fx_set_word() got the user's own text
struct Spr {
    const uint8_t *icon; // nullptr = the wordmark
    int w, h, scale;
};
// The user's word is lit with a sheen: white at the top into the app's accent at the bottom
// (fx_attract sets it per frame); the stock QUADRA stays plain white.
static uint32_t s_word_sheen = WHITE;
static inline uint32_t spr_px(const Spr &sp, int i, int j) {
    if (sp.icon == nullptr) {
        if (!s_word[j * WORD_MAX_W + i]) return 0;
        if (!s_word_custom || s_word_h < 2) return WHITE;
        float t = (float)j / (s_word_h - 1);
        uint32_t r = 255 + (uint32_t)((((s_word_sheen >> 16) & 0xFF) - 255.0f) * t);
        uint32_t g = 255 + (uint32_t)((((s_word_sheen >> 8) & 0xFF) - 255.0f) * t);
        uint32_t b = 255 + (uint32_t)(((s_word_sheen & 0xFF) - 255.0f) * t);
        return r << 16 | g << 8 | b | 0x010101; // never 0 (= transparent)
    }
    const uint8_t *p = sp.icon + (j * sp.w + i) * 2;
    uint16_t v = (uint16_t)(p[0] << 8 | p[1]);
    if (v == 0) return 0;
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return (r * 255 / 31) << 16 | (g * 255 / 63) << 8 | (b * 255 / 31) | 0x010101; // never 0
}
static inline int rest_w(const Spr &sp) { return sp.w * sp.scale; }
static inline int rest_h(const Spr &sp) { return sp.h * sp.scale; }
// Bottom centre at (cx, by), stretched sx / sy. solid != 0: a one-colour silhouette; dither:
// every other pixel (afterimages); gleam >= 0: a white diagonal band at that offset.
static void spr_draw(const Spr &sp, float cx, float by, float sx, float sy, uint32_t solid = 0,
                     bool dither = false, bool flip = false, int gleam = -1) {
    int W = (int)lroundf(sp.w * sp.scale * sx), H = (int)lroundf(sp.h * sp.scale * sy);
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    int x0 = lroundf(cx - W / 2.0f), y0 = lroundf(by - H);
    for (int j = 0; j < H; j++) {
        int sj = j * sp.h / H;
        for (int i = 0; i < W; i++) {
            int si = i * sp.w / W;
            uint32_t c = spr_px(sp, flip ? sp.w - 1 - si : si, sj);
            if (!c) continue;
            int X = x0 + i, Y = y0 + j;
            if (dither && ((X + Y) & 1)) continue;
            if (solid) c = solid;
            if (gleam >= 0) { int d = i + j - gleam; if (d >= 0 && d < 4 + sp.scale * 2) c = WHITE; }
            pt(X, Y, c);
        }
    }
}
// Ground shadow: a dithered ellipse that shrinks as the sprite rises.
static void shadow(float cx, float gy, float w, float lift) {
    float k = 1 - lift / 140;
    if (k < 0.25f) k = 0.25f;
    int rx = lroundf(w / 2 * k), ry = lroundf(3 * k);
    if (ry < 1) ry = 1;
    for (int y = -ry; y <= ry; y++)
        for (int x = -rx; x <= rx; x++)
            if ((float)(x * x) / (rx * rx) + (float)(y * y) / (ry * ry) <= 1 && !((x + y) & 1)) pt(cx + x, gy + y, DARK);
}

// --- shared effects ---
static int shake_at(float t, float t0, int amp, float dur) {
    float k = (t - t0) / dur;
    if (k < 0 || k > 1) return 0;
    int f = (int)((t - t0) / 33);
    return (int)lroundf(amp * (1 - k)) * (f & 1 ? -1 : 1);
}
// Dust: puffs rolling out along the ground from both feet.
static void dust(float t, float t0, float cx, float gy, float half_w, int n, int seed) {
    float k = (t - t0) / 520;
    if (k < 0 || k > 1) return;
    for (int i = 0; i < n; i++) {
        float side = (i & 1) ? 1 : -1, spd = 18 + rndf(seed + i, 1) * 26;
        float x = cx + side * (half_w - 2 + spd * sqrtf(k)), y = gy - 1 - k * (3 + rndf(seed + i, 2) * 5);
        blob(x, y, lroundf(2 + k * 3), k < 0.45f ? GREY : DARK, k < 0.7f ? 1 : 0);
    }
}
// Debris: small chunks thrown up and out, falling with gravity.
static void debris(float t, float t0, float cx, float cy, int n, const uint32_t *cols, int seed, float power) {
    float dt = (t - t0) / 1000;
    if (dt < 0 || dt > 0.9f) return;
    for (int i = 0; i < n; i++) {
        float a = -(float)M_PI / 2 + (rndf(seed + i, 3) - 0.5f) * 2.6f, v = (70 + rndf(seed + i, 4) * 90) * power;
        float x = cx + cosf(a) * v * dt, y = cy + sinf(a) * v * dt + 260 * dt * dt;
        int s = i % 3 == 0 ? 3 : 2;
        box(x, y, s, s, i % 4 == 0 ? WHITE : cols[i % 3]);
    }
}
// Sparkles around the icon in a mix of sizes: 1 px pluses up to glints with diagonal rays.
static const int SPARK_SIZE[7] = {1, 3, 2, 4, 1, 2, 3};
static void sparkles(float t, float cx, float cy, float spread, const uint32_t *cols) {
    for (int i = 0; i < 7; i++) {
        float per = 1100 + i * 190, ph = t + i * 530, u = fmodf(ph, per) / per;
        if (u > 0.55f) continue;
        int n = (int)(ph / per);
        float a = rndf(n + i * 31, 13) * 2 * (float)M_PI, d = spread + rndf(n + i * 17, 14) * 22;
        int x = lroundf(cx + cosf(a) * d), y = lroundf(cy + sinf(a) * d);
        float k = sinf(u / 0.55f * (float)M_PI);
        int s = lroundf(SPARK_SIZE[i] * k);
        uint32_t c = i % 3 == 0 ? WHITE : cols[i % 3];
        pt(x, y, s >= 2 ? WHITE : c);
        for (int r = 1; r <= s; r++) { pt(x - r, y, c); pt(x + r, y, c); pt(x, y - r, c); pt(x, y + r, c); }
        for (int r = 1; r <= s - 2; r++) { pt(x - r, y - r, c); pt(x + r, y - r, c); pt(x - r, y + r, c); pt(x + r, y + r, c); }
        if (s >= 2) { pt(x - 1, y, WHITE); pt(x + 1, y, WHITE); pt(x, y - 1, WHITE); pt(x, y + 1, WHITE); }
    }
}
// Parallax stars drifting left: far dark specks, mid grey dots, a few near 2 px stars.
static void stars(float t) {
    for (int i = 0; i < 50; i++) {
        bool near = i < 6, mid = i < 20;
        float v = near ? 0.036f : mid ? 0.02f : 0.011f;
        float x = fmodf(rndf(i, 8) * 260 - t * v, 260);
        if (x < 0) x += 260;
        x -= 10;
        float y = rndf(i, 9) * 240;
        if (near) rect(lroundf(x), lroundf(y), 2, 2, GREY);
        else rect(lroundf(x), lroundf(y), 1, 1, mid ? GREY : DARK);
    }
}

// --- JUMP ---
enum { MV_REST, MV_GLEAM, MV_CROUCH, MV_JUMP, MV_SPIN, MV_LAND };
enum { IMP_NONE, IMP_SMALL, IMP_MID, IMP_BIG };
struct Move {
    uint8_t type;
    uint16_t ms;
    float a, b, c; // CROUCH / LAND: sx, sy; JUMP: height, dx; SPIN: height
    uint8_t flag;  // JUMP: trail; LAND: impact
};
static const Move MOVES[] = {
    {MV_REST, 600, 0, 0, 0, 0},
    {MV_CROUCH, 130, 1.2f, 0.8f, 0, 0}, {MV_JUMP, 300, 18, 0, 0, 0}, {MV_LAND, 132, 1.15f, 0.85f, 0, IMP_SMALL},
    {MV_REST, 220, 0, 0, 0, 0},
    {MV_CROUCH, 130, 1.2f, 0.8f, 0, 0}, {MV_JUMP, 300, 18, 0, 0, 0}, {MV_LAND, 132, 1.15f, 0.85f, 0, IMP_SMALL},
    {MV_REST, 300, 0, 0, 0, 0},
    {MV_CROUCH, 230, 1.34f, 0.66f, 0, 0}, {MV_JUMP, 720, 72, 0, 0, 1}, {MV_LAND, 198, 1.38f, 0.62f, 0, IMP_BIG},
    {MV_JUMP, 220, 10, 0, 0, 0}, {MV_LAND, 100, 1.1f, 0.9f, 0, IMP_NONE},
    {MV_GLEAM, 900, 0, 0, 0, 0},
    {MV_CROUCH, 120, 1.18f, 0.82f, 0, 0}, {MV_JUMP, 380, 24, -34, 0, 1}, {MV_LAND, 132, 1.18f, 0.82f, 0, IMP_SMALL},
    {MV_CROUCH, 120, 1.18f, 0.82f, 0, 0}, {MV_JUMP, 480, 30, 68, 0, 1}, {MV_LAND, 132, 1.18f, 0.82f, 0, IMP_SMALL},
    {MV_CROUCH, 120, 1.18f, 0.82f, 0, 0}, {MV_JUMP, 380, 24, -34, 0, 1}, {MV_LAND, 132, 1.15f, 0.85f, 0, IMP_SMALL},
    {MV_REST, 500, 0, 0, 0, 0},
    {MV_CROUCH, 180, 1.28f, 0.72f, 0, 0}, {MV_SPIN, 820, 50, 0, 0, 1}, {MV_LAND, 166, 1.3f, 0.7f, 0, IMP_MID},
    {MV_REST, 1700, 0, 0, 0, 0},
    {MV_CROUCH, 90, 1.12f, 0.88f, 0, 0}, {MV_JUMP, 230, 12, 0, 0, 0}, {MV_LAND, 90, 1.1f, 0.9f, 0, IMP_SMALL},
    {MV_CROUCH, 90, 1.12f, 0.88f, 0, 0}, {MV_JUMP, 230, 12, 0, 0, 0}, {MV_LAND, 90, 1.1f, 0.9f, 0, IMP_SMALL},
    {MV_REST, 500, 0, 0, 0, 0},
};
#define N_MOVES ((int)(sizeof(MOVES) / sizeof(MOVES[0])))
static uint32_t s_move_t[N_MOVES]; // start time of each move
static int16_t s_move_x[N_MOVES];  // x offset at its start
static uint32_t s_jump_ms = 0;     // loop length
static const uint8_t IMPACT[4][3] = {{0, 0, 0}, {4, 0, 0}, {8, 2, 6}, {10, 4, 10}}; // dust, shake px, debris

struct JState { float x, up, sx, sy; bool flip, trail, rest; float gleam; };
static JState jump_state(float t, float hs) {
    t = fmodf(t, (float)s_jump_ms);
    if (t < 0) t += s_jump_ms;
    int i = 0;
    while (i + 1 < N_MOVES && s_move_t[i + 1] <= t) i++;
    const Move &m = MOVES[i];
    float k = (t - s_move_t[i]) / m.ms;
    JState st = {(float)s_move_x[i], 0, 1, 1, false, false, false, -1};
    switch (m.type) {
        case MV_REST:
        case MV_GLEAM: {
            st.rest = true;
            float in = fmodf(t - s_move_t[i], 1200);
            if (m.ms > 1000 && in > 1000 && in < 1132) { st.sx = 1.04f; st.sy = 0.96f; } // breathing
            if (m.type == MV_GLEAM) st.gleam = k;
            break;
        }
        case MV_CROUCH: {
            float e = k * 2 > 1 ? 1 : k * 2;
            st.sx = 1 + (m.a - 1) * e;
            st.sy = 1 + (m.b - 1) * e;
            break;
        }
        case MV_JUMP:
        case MV_SPIN: {
            float a = fabsf(1 - 2 * k);
            st.up = 4 * m.a * hs * k * (1 - k);
            if (m.type == MV_JUMP) st.x += m.b * k;
            st.sx = 1 - 0.18f * a;
            st.sy = 1 + 0.26f * a;
            st.trail = m.flag != 0;
            if (m.type == MV_SPIN) {
                float c = cosf(k * 4 * (float)M_PI);
                st.sx *= fabsf(c) > 0.1f ? fabsf(c) : 0.1f;
                st.flip = c < 0;
            }
            break;
        }
        case MV_LAND: {
            float e = k < 0.5f ? 1 : 1 - (k - 0.5f) * 2;
            st.sx = 1 + (m.a - 1) * e;
            st.sy = 1 + (m.b - 1) * e;
            break;
        }
    }
    return st;
}
static void routine_jump(float t, const Spr &sp, const uint32_t *cols) {
    float gy = 120 + rest_h(sp) / 2 + 10;
    float hs = (gy - rest_h(sp) - 14) / 72;
    if (hs > 1) hs = 1; // the big jump always stays on the glass
    for (int i = 0; i < N_MOVES; i++) {
        const Move &m = MOVES[i];
        if (m.type != MV_LAND || m.flag == IMP_NONE || s_move_t[i] > t) continue;
        const uint8_t *imp = IMPACT[m.flag];
        float lx = 120 + s_move_x[i];
        if (imp[1]) { int sx = shake_at(t, s_move_t[i], imp[1], 240); if (sx) s_ox = sx; }
        dust(t, s_move_t[i], lx, gy, rest_w(sp) * 0.62f, imp[0], i * 7);
        if (imp[2]) debris(t, s_move_t[i], lx, gy - 4, imp[2], cols, i * 13, 0.9f);
    }
    JState st = jump_state(t, hs);
    shadow(120 + st.x, gy + 2, rest_w(sp), st.up);
    if (st.trail) {
        static const int BACK[3] = {6, 4, 2};
        for (int k = 0; k < 3; k++) {
            JState p = jump_state(t - BACK[k] * 33.33f, hs);
            spr_draw(sp, 120 + p.x, gy - p.up, p.sx, p.sy, k < 2 ? DARK : GREY, true, p.flip);
        }
    }
    int gleam = st.gleam >= 0 ? (int)lroundf(st.gleam * (rest_w(sp) + rest_h(sp) + 16)) - 12 : -1;
    spr_draw(sp, 120 + st.x, gy - st.up, st.sx, st.sy, 0, false, st.flip, gleam);
    if (st.rest) sparkles(t, 120 + st.x, gy - rest_h(sp) / 2.0f, rest_w(sp) / 2.0f + 8, cols);
}

// --- BOUNCE ---
// Simulated in 33 ms steps from the routine's start; state carried between frames.
#define BOUNCE_STEP 33
#define BOUNCE_HIST 16
#define BOUNCE_HITS 8
static struct {
    uint32_t step;
    float x, y, vx, vy, r;
    float hx[BOUNCE_HIST], hy[BOUNCE_HIST];
    uint32_t hit_t[BOUNCE_HITS];
    float hit_nx[BOUNCE_HITS], hit_ny[BOUNCE_HITS];
    int n_hits;
} s_b;
static void bounce_reset(float radius) {
    memset(&s_b, 0, sizeof(s_b));
    s_b.x = 0; s_b.y = -20; s_b.vx = 0.071f; s_b.vy = 0.052f; s_b.r = radius;
    s_b.hx[0] = s_b.x; s_b.hy[0] = s_b.y;
    for (int i = 0; i < BOUNCE_HITS; i++) s_b.hit_t[i] = UINT32_MAX;
}
static void bounce_advance(uint32_t to_step) {
    while (s_b.step < to_step) {
        s_b.x += s_b.vx * BOUNCE_STEP;
        s_b.y += s_b.vy * BOUNCE_STEP;
        float d = sqrtf(s_b.x * s_b.x + s_b.y * s_b.y);
        if (d > s_b.r) {
            float nx = s_b.x / d, ny = s_b.y / d, dot = s_b.vx * nx + s_b.vy * ny;
            s_b.vx -= 2 * dot * nx;
            s_b.vy -= 2 * dot * ny;
            s_b.x = nx * s_b.r;
            s_b.y = ny * s_b.r;
            int h = s_b.n_hits++ % BOUNCE_HITS;
            s_b.hit_t[h] = s_b.step * BOUNCE_STEP;
            s_b.hit_nx[h] = nx;
            s_b.hit_ny[h] = ny;
        }
        s_b.step++;
        s_b.hx[s_b.step % BOUNCE_HIST] = s_b.x;
        s_b.hy[s_b.step % BOUNCE_HIST] = s_b.y;
    }
}
static void routine_bounce(float t, const Spr &sp, const uint32_t *cols, bool restart) {
    int big = rest_w(sp) > rest_h(sp) ? rest_w(sp) : rest_h(sp);
    uint32_t f = (uint32_t)(t / BOUNCE_STEP);
    if (restart || f < s_b.step) bounce_reset(116 - big / 2.0f - 2);
    bounce_advance(f);
    stars(t);
    int last = -1;
    for (int i = 0; i < BOUNCE_HITS; i++)
        if (s_b.hit_t[i] != UINT32_MAX && s_b.hit_t[i] <= t && (last < 0 || s_b.hit_t[i] > s_b.hit_t[last])) last = i;
    if (last >= 0) s_ox = shake_at(t, s_b.hit_t[last], 2, 130);
    static const int BACK[3] = {9, 6, 3};
    for (int k = 0; k < 3; k++) {
        uint32_t s = f >= (uint32_t)BACK[k] ? f - BACK[k] : 0;
        spr_draw(sp, 120 + s_b.hx[s % BOUNCE_HIST], 120 + s_b.hy[s % BOUNCE_HIST] + rest_h(sp) / 2.0f, 1, 1, k < 2 ? DARK : GREY, true);
    }
    float sx = 1, sy = 1;
    if (last >= 0 && t - s_b.hit_t[last] < 132) {
        float sq = 0.3f * (t - s_b.hit_t[last] < 66 ? 1 : 0.5f);
        if (fabsf(s_b.hit_nx[last]) > fabsf(s_b.hit_ny[last])) { sx = 1 - sq; sy = 1 + sq * 0.8f; }
        else { sy = 1 - sq; sx = 1 + sq * 0.8f; }
    }
    float x = s_b.hx[f % BOUNCE_HIST], y = s_b.hy[f % BOUNCE_HIST];
    spr_draw(sp, 120 + x, 120 + y + rest_h(sp) * sy / 2, sx, sy);
    for (int i = 0; i < BOUNCE_HITS; i++) {
        if (s_b.hit_t[i] == UINT32_MAX) continue;
        float k = (t - s_b.hit_t[i]) / 260;
        if (k < 0 || k > 1) continue;
        float nx = s_b.hit_nx[i], ny = s_b.hit_ny[i], ax = 120 + nx * 117, ay = 120 + ny * 117, a0 = atan2f(ny, nx);
        for (float d = -0.2f; d <= 0.2f; d += 0.01f) pt(120 + cosf(a0 + d) * 117, 120 + sinf(a0 + d) * 117, k < 0.4f ? WHITE : cols[1]);
        for (int j = 0; j < 7; j++) {
            float a = a0 + (float)M_PI + (j - 3) * 0.32f, r0 = 4 + k * 16, r1 = r0 + (k < 0.5f ? 6 : 3);
            seg(ax + cosf(a) * r0, ay + sinf(a) * r0, ax + cosf(a) * r1, ay + sinf(a) * r1, (j & 1) ? WHITE : cols[j % 3]);
        }
    }
}

// --- BOOM ---
static const float POP[8] = {0.2f, 0.62f, 1.3f, 1.16f, 0.9f, 0.95f, 1.05f, 1.0f}; // one per 66 ms (on twos)
static const float WOB[8] = {0, 0.14f, -0.12f, 0.08f, -0.06f, 0.04f, -0.02f, 0};
static void routine_boom(float t, const Spr &sp, const uint32_t *cols) {
    const float cy = 120;
    s_ox = shake_at(t, 320, 5, 320);
    s_oy = t > 320 && t < 480 ? (((int)(t / 33) & 1) ? 2 : -2) : 0;
    if (t < 320 && ((int)(t / 100)) % 2 == 0) box(119, 119, 3, 3, cols[1]); // the fuse
    float e = t - 320;
    if (e >= 0 && e < 1000) {
        for (int i = 0; i < 8; i++) { // smoke
            float k = e / 1000, a = i / 8.0f * 2 * (float)M_PI + 0.3f, d = 14 + k * 34;
            if (k > 0.15f) blob(120 + cosf(a) * d, cy + sinf(a) * d - k * 16, lroundf(5 + k * 6), k < 0.55f ? GREY : DARK, k < 0.75f ? 1 : 0);
        }
        if (e < 200) blob(120, cy, lroundf(4 + e / 200 * 18), WHITE, 2); // core
        if (e >= 120 && e < 460) {
            float k = (e - 120) / 340, r = lroundf(20 + k * 22);
            for (int w = 0; w < 3; w++) dotted_ring(120, cy, r - w, k < 0.5f ? cols[2] : cols[0], k < 0.6f ? 1 : 2);
        }
        debris(t, 320, 120, cy, 16, cols, 11, 1.2f);
    }
    float s = 0, w = 0;
    int bob = 0;
    const float pop_end = 420 + 8 * 66;
    if (e >= 100 && t < pop_end) { int i = (int)((e - 100) / 66); s = POP[i]; w = WOB[i]; }
    else if (t >= pop_end && t < 5300) { s = 1; bob = lroundf(2 * sinf((int)(t / 66) * 0.42f)); }
    else if (t >= 5300 && t < 5560) { float k = (t - 5300) / 260; s = 1 - k > 0.05f ? 1 - k : 0.05f; w = -0.3f * k; }
    if (s > 0) {
        float H = rest_h(sp) * s * (1 - w);
        if (t < 5300) shadow(120, cy + rest_h(sp) / 2.0f + 6, rest_w(sp) * 0.9f, bob > 0 ? 8 : 0);
        int gleam = t > 1600 && t < 2000 ? (int)lroundf((t - 1600) / 400 * (rest_w(sp) + rest_h(sp) + 12)) - 12 : -1;
        spr_draw(sp, 120, cy + H / 2 - bob, s * (1 + w), s * (1 - w), 0, false, false, gleam);
        if (t > 1100 && t < 5300) sparkles(t, 120, cy, rest_w(sp) / 2.0f + 12, cols);
    }
    if (t >= 5560 && t < 5700) blob(120, cy, lroundf(8 - (t - 5560) / 20), WHITE, 2);
}

// The accent colours: the profile's plasma_heat, or sampled from its icon (app_colors.c,
// shared with the LEDs). Cached per icon / heat.
static uint32_t s_accents[3];
static const uint8_t *s_acc_icon = nullptr;
static const uint32_t *s_acc_heat = nullptr;

// --- the sequence ---
static const uint32_t BOUNCE_MS = 16000, BOOM_MS = 6000;
static uint32_t routine_ms(int r) { return r == ATTRACT_JUMP ? s_jump_ms : r == ATTRACT_BOUNCE ? BOUNCE_MS : BOOM_MS; }
static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
// Which routines the random pick may choose. BOUNCE is switched off (user, 2026-09-30) but
// kept: set it back to true to bring it back. The host preview can still pin it (`only`).
static const bool ROUTINE_ON[ATTRACT_ROUTINES] = {
    [ATTRACT_JUMP] = true,
    [ATTRACT_BOUNCE] = false,
    [ATTRACT_BOOM] = true,
};
// The k-th enabled routine, skipping `except` (-1 = none); count = how many that leaves.
static int pick_routine(uint32_t h, int except) {
    int list[ATTRACT_ROUTINES], n = 0;
    for (int r = 0; r < ATTRACT_ROUTINES; r++)
        if (ROUTINE_ON[r] && r != except) list[n++] = r;
    if (n == 0) return except >= 0 ? except : ATTRACT_JUMP; // only one enabled: it repeats
    return list[h % n];
}
static struct {
    uint32_t seed, last_t, start, n;
    int routine;
} s_seq = {0, UINT32_MAX, 0, 0, -1};

void fx_attract(uint32_t t_ms, const uint8_t *icon48, const uint32_t *heat, uint32_t seed, int only) {
    // A new idle session (time went back, or a new seed): start a fresh random sequence.
    bool restart = false;
    if (t_ms < s_seq.last_t || seed != s_seq.seed || s_seq.routine < 0) {
        s_seq.seed = seed;
        s_seq.start = 0;
        s_seq.n = 0;
        s_seq.routine = only >= 0 ? only : pick_routine(hash32(seed), -1);
        restart = true;
    }
    s_seq.last_t = t_ms;
    while (t_ms - s_seq.start >= routine_ms(s_seq.routine)) {
        s_seq.start += routine_ms(s_seq.routine);
        s_seq.n++;
        if (only < 0) s_seq.routine = pick_routine(hash32(seed + s_seq.n), s_seq.routine);
        restart = true;
    }
    float t = (float)(t_ms - s_seq.start);

    // The user's own word takes turns with the app's icon, one routine each (the word first);
    // the stock QUADRA wordmark only shows without an icon, as before.
    bool word = icon48 == nullptr || (s_word_custom && (s_seq.n & 1) == 0);
    Spr sp = word ? Spr{nullptr, s_word_w, s_word_h, s_word_scale} : Spr{icon48, ATTRACT_ICON, ATTRACT_ICON, 1};
    if (icon48 != s_acc_icon || heat != s_acc_heat || s_acc_icon == nullptr) {
        app_accents(icon48, heat, s_accents); // AMBER x3 without an icon (QUADRA)
        s_acc_icon = icon48;
        s_acc_heat = heat;
    }
    const uint32_t *cols = s_accents;
    s_word_sheen = cols[2];
    s_ox = s_oy = 0;
    switch (s_seq.routine) {
        case ATTRACT_JUMP: routine_jump(t, sp, cols); break;
        case ATTRACT_BOUNCE: routine_bounce(t, sp, cols, restart); break;
        default: routine_boom(t, sp, cols); break;
    }
    s_ox = s_oy = 0;
}

void fx_set_word(const char *text) {
    s_word_custom = text != nullptr && text[0] != '\0';
    const char *w = s_word_custom ? text : "QUADRA";

    // Loading screen: the word's own pixels, at the largest scale that fits the glass.
    s_logo_scale = fit_scale(w, 200, 3);
    s_logo_n = text_blocks(w, 120, 96, s_logo_scale, s_logo, LOGO_MAX_BLOCKS);
    for (int i = 0; i < s_logo_n; i++) {
        float a = rnd(i, 1) * 2.0f * (float)M_PI, d = 30 + rnd(i, 2) * 80;
        s_logo_scatter[i][0] = (int16_t)lroundf(SPARK_X + cosf(a) * d);
        s_logo_scatter[i][1] = (int16_t)lroundf(SPARK_Y + sinf(a) * d);
        s_logo_delay_ms[i] = (uint16_t)(rnd(i, 3) * 260);
        float r4 = rnd(i, 4);
        s_logo_tint[i] = r4 < 0.3f ? 0 : r4 < 0.55f ? 1 : 2;
    }

    // The idle wordmark: the word's pixels at 1x, as a mask (drawn at 2x when it fits, squashable).
    EXT_RAM_BSS_ATTR static int16_t word[LOGO_MAX_BLOCKS][2]; // scratch, kept off the task stack
    int nw = text_blocks(w, 0, 0, 1, word, LOGO_MAX_BLOCKS);
    int x0 = 1 << 15, y0 = 1 << 15, x1 = -(1 << 15), y1 = -(1 << 15);
    for (int k = 0; k < nw; k++) {
        if (word[k][0] < x0) x0 = word[k][0];
        if (word[k][1] < y0) y0 = word[k][1];
        if (word[k][0] > x1) x1 = word[k][0];
        if (word[k][1] > y1) y1 = word[k][1];
    }
    memset(s_word, 0, sizeof(s_word));
    s_word_w = nw ? x1 - x0 + 1 : 1;
    s_word_h = nw ? y1 - y0 + 1 : 1;
    if (s_word_w > WORD_MAX_W) s_word_w = WORD_MAX_W;
    if (s_word_h > WORD_MAX_H) s_word_h = WORD_MAX_H;
    for (int k = 0; k < nw; k++) {
        int x = word[k][0] - x0, y = word[k][1] - y0;
        if (x < WORD_MAX_W && y < WORD_MAX_H) s_word[y * WORD_MAX_W + x] = 1;
    }
    s_word_scale = s_word_w <= WORD_2X_MAX_W ? 2 : 1;
}

void fx_init() {
    fx_set_word(nullptr);

    // Jump choreography: each move's start time and x offset.
    uint32_t t = 0;
    int x = 0;
    for (int i = 0; i < N_MOVES; i++) {
        s_move_t[i] = t;
        s_move_x[i] = (int16_t)x;
        t += MOVES[i].ms;
        if (MOVES[i].type == MV_JUMP) x += (int)MOVES[i].b;
    }
    s_jump_ms = t;
}

} // namespace ui
