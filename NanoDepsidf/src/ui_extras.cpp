#include "ui_extras.hpp"
#include "ui_gfx.hpp"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_attr.h"
extern "C" {
#include "ui_state.h"
}

namespace ui {

static const uint32_t ALLOW_GREEN = 0x22DD66u, DENY_RED = 0xFF3B30u, PANEL = 0x111111u;

static uint32_t scale_rgb(uint32_t c, float k) {
    if (k > 1) k = 1;
    uint32_t r = (uint32_t)(((c >> 16) & 0xFF) * k), g = (uint32_t)(((c >> 8) & 0xFF) * k), b = (uint32_t)((c & 0xFF) * k);
    return r << 16 | g << 8 | b;
}

static float breath(uint32_t t_ms, float period_ms) {
    return 0.5f - 0.5f * cosf(t_ms * 2 * (float)M_PI / period_ms);
}

// --- LIGHTS ---

void draw_lights(const menu_render_snapshot_t &snap, const LightsInputs &in) {
    // The rim mirrors the LED ring. The LEDs run at a few % of full brightness, so the frame
    // is scaled up as a whole (relative levels kept: a comet still looks like a comet).
    if (in.ring) {
        int peak = 0;
        for (int i = 0; i < 60; i++)
            for (int k = 0; k < 3; k++) peak = in.ring[i][k] > peak ? in.ring[i][k] : peak;
        float gain = peak ? 255.0f / peak : 0;
        if (gain > 12) gain = 12;
        for (int i = 0; i < 60; i++) {
            float a = i * 2 * (float)M_PI / 60 - (float)M_PI / 2;
            float x = CX + cosf(a) * 112, y = CY + sinf(a) * 112;
            uint32_t c = (uint32_t)fminf(in.ring[i][0] * gain, 255) << 16 | (uint32_t)fminf(in.ring[i][1] * gain, 255) << 8
                       | (uint32_t)fminf(in.ring[i][2] * gain, 255);
            if (c == 0) frame_box((int)lroundf(x) - 2, (int)lroundf(y) - 2, 5, 5, DARK);
            else cut((int)lroundf(x) - 2, (int)lroundf(y) - 2, 5, 5, c);
        }
    }

    text("LIGHTS", CX, 26, GREY, 1, CENTER);
    rect(70, 40, 100, 1, DARK);
    for (int i = 0; i < snap.row_count && i < MENU_LIGHTS_ROW_COUNT; i++) {
        const menu_render_row_t &r = snap.rows[i];
        const int y = 52 + i * 16;
        if (r.selected) cut(42, y - 4, 156, 15, AMBER);
        uint32_t lc = r.selected ? BLACK : GREY;
        uint32_t vc = r.selected ? BLACK : r.muted ? DARK : WHITE;
        int lw = text(r.label, 50, y, lc);
        text(r.value, 190, y, vc, 1, RIGHT);
        if (i == MENU_LIGHTS_ROW_COLOR) {
            float sx = 50 + lw + 10, sy = y + 3;
            disc(sx, sy, 4, in.swatch);
            if (r.selected) ring(sx, sy, 5, BLACK, 1);
        }
    }
    if (snap.dirty && in.blink_on) text("F2 SAVE", CX, 154, AMBER, 1, CENTER);
    else text("TURN  F1 NEXT  F3 BACK", CX, 154, DARK, 1, CENTER);
}

// --- DISPLAY, SLEEP ---

// LIGHTS' rows, under a title; returns the y below the last.
static int setting_rows(const char *title, const menu_render_snapshot_t &snap) {
    text(title, CX, 26, GREY, 1, CENTER);
    rect(70, 40, 100, 1, DARK);
    int y = 52;
    for (int i = 0; i < snap.row_count; i++, y += 16) {
        const menu_render_row_t &r = snap.rows[i];
        if (r.selected) cut(42, y - 4, 156, 15, AMBER);
        text(r.label, 50, y, r.selected ? BLACK : GREY);
        text(r.value, 190, y, r.selected ? BLACK : r.muted ? DARK : WHITE, 1, RIGHT);
    }
    return y;
}

static void setting_hint(const menu_render_snapshot_t &snap, bool blink_on) {
    if (snap.dirty && blink_on) text("F2 SAVE", CX, 154, AMBER, 1, CENTER);
    else text("TURN  F1 NEXT  F3 BACK", CX, 154, DARK, 1, CENTER);
}

void draw_display(const menu_render_snapshot_t &snap, bool blink_on) {
    int y = setting_rows("DISPLAY", snap) + 6;
    const char *about = "";
    switch (snap.selected) {
        case MENU_DISPLAY_ROW_ROTATION: about = "TURNS THE SCREEN"; break;
        case MENU_DISPLAY_ROW_BRIGHT: about = "HOW BRIGHT THE SCREEN IS"; break;
        case MENU_DISPLAY_ROW_AFTER: about = "IDLE BEFORE THE SAVER"; break;
        case MENU_DISPLAY_ROW_SAVER: {
            const char *v = snap.rows[MENU_DISPLAY_ROW_SAVER].value;
            about = !strcmp(v, "AUTO")     ? "MUSIC, CLOCK OR ICON"
                  : !strcmp(v, "ICON")     ? "THE ICON JUMPS"
                  : !strcmp(v, "BOUNCE")   ? "THE ICON BOUNCES"
                  : !strcmp(v, "CLOCK")    ? "THE TIME"
                  : !strcmp(v, "MUSIC")    ? "THE TRACK PLAYING"
                  : !strcmp(v, "BLANK")    ? "THE SCREEN GOES OFF"
                                           : "THE SCREEN STAYS ON";
            break;
        }
        default: break;
    }
    text(about, CX, y, GREY, 1, CENTER);
    setting_hint(snap, blink_on);
}

void draw_sleep(const menu_render_snapshot_t &snap, const SleepInputs &in) {
    int y = setting_rows("SLEEP", snap) + 4;
    if (!in.on) {
        text("SCREEN AND LIGHTS OFF", CX, y, GREY, 1, CENTER);
        text("AT NIGHT", CX, y + 12, GREY, 1, CENTER);
    } else if (!in.trusted) { // the fail-safe: no trusted local time, no sleep hours
        text("WAITING FOR THE TIME", CX, y, AMBER, 1, CENTER);
        text("RUN THE QUADRA SERVICE", CX, y + 12, GREY, 1, CENTER);
    } else {
        text(in.sleeping ? "SLEEPING NOW" : "AWAKE NOW", CX, y, GREY, 1, CENTER);
        text("A TOUCH WAKES IT", CX, y + 12, GREY, 1, CENTER);
    }
    if (snap.dirty && in.blink_on) text("F2 SAVE", CX, 184, AMBER, 1, CENTER);
    else text("TURN  F1 NEXT  F3 BACK", CX, 184, DARK, 1, CENTER);
}

void draw_clock_saver(const ClockSaverInputs &in) {
    char t[12], d[16];
    if (!in.valid) {
        snprintf(t, sizeof(t), "--:--");
    } else {
        int h = in.h24 ? in.hour : in.hour % 12 ? in.hour % 12 : 12;
        snprintf(t, sizeof(t), "%d:%02d", h % 24, in.minute % 60);
    }
    static const char *const WDAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const MON[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    snprintf(d, sizeof(d), "%s %d %s", WDAY[in.wday % 7], in.mday % 100, MON[in.mon % 12]);
    // A new spot each minute, well inside the glass.
    const int dx = (int)lroundf((rnd((int)in.minute_index, 1) - 0.5f) * 40);
    const int dy = (int)lroundf((rnd((int)in.minute_index, 2) - 0.5f) * 50);
    const int sc = fit_scale("00:00", 150, 6), y = (int)CY - cap_height(sc) / 2 - 8 + dy;
    text(t, CX + dx, y, GREY, sc, CENTER);
    if (in.valid) {
        text(d, CX + dx, y + cap_height(sc) + 12, DARK, 1, CENTER);
        if (!in.h24) text(in.hour < 12 ? "AM" : "PM", CX + dx, y - 14, DARK, 1, CENTER);
    }
}

// --- agent notification ---

// The agents' marks, drawn black on the coloured badge.
static const Sprite MARK[4] = {
    {9, 9, // Claude: the spark
     "....#...."
     ".#..#..#."
     "..#.#.#.."
     "...###..."
     "#########"
     "...###..."
     "..#.#.#.."
     ".#..#..#."
     "....#...."},
    {9, 9, // Codex: a prompt
     "........."
     "##......."
     ".##......"
     "..##....."
     "...##...."
     "..##....."
     ".##......"
     "##..#####"
     "........."},
    {8, 10, // Cursor: the pointer
     "#......."
     "##......"
     "###....."
     "####...."
     "#####..."
     "######.."
     "#######."
     "####...."
     "#..##..."
     "....##.."},
    {9, 9, // anything else: a bell
     "....#...."
     "...###..."
     "..#####.."
     "..#####.."
     "..#####.."
     ".#######."
     "#########"
     "........."
     "...###..."},
};

// Rings of light just inside the glass: solid, then two dithered rings fading inward -- a glow
// in whole pixels. Several agents waiting: one arc each in queue order from 12 o'clock, the
// first breathing, the rest steady.
static void glow(const uint32_t *cols, int n, float b) {
    static const float R[3] = {118, 114, 110}, LEVEL[3] = {1.0f, 0.55f, 0.3f};
    static const int STEP[3] = {1, 2, 3};
    for (int k = 0; k < 3; k++) {
        int steps = (int)(2 * (float)M_PI * R[k]);
        for (int s = 0; s < steps; s += STEP[k]) {
            float u = (float)s / steps; // 0..1 clockwise from 12 o'clock
            int seg = n > 1 ? (int)(u * n) : 0;
            if (n > 1 && fmodf(u * n, 1.0f) < 0.012f * n) continue; // a gap between arcs
            float lv = LEVEL[k] * (seg == 0 ? 0.25f + 0.75f * b : 0.45f);
            float a = u * 2 * (float)M_PI - (float)M_PI / 2;
            int th = k == 0 ? 2 : 1;
            rect(CX + cosf(a) * R[k] - th / 2.0f, CY + sinf(a) * R[k] - th / 2.0f, th, th, scale_rgb(cols[seg], lv));
        }
    }
}

// Holding F1: a thick green arc from 12 o'clock with a bright head.
static void hold_arc(float frac) {
    const float r = 115;
    int n = (int)(frac * 2 * (float)M_PI * r);
    float a = 0;
    for (int s = 0; s <= n; s++) {
        a = s / r - (float)M_PI / 2;
        rect(CX + cosf(a) * r - 2.5f, CY + sinf(a) * r - 2.5f, 5, 5, ALLOW_GREEN);
    }
    disc(CX + cosf(a) * r, CY + sinf(a) * r, 4, WHITE);
}

// Greedy word wrap to `max_w`, at most `lines` lines; a word too long for a line is cut, and
// text that doesn't fit ends in "..".
static int wrap(const char *s, int max_w, char out[][48], int lines) {
    int n = 0;
    while (*s && n < lines) {
        while (*s == ' ') s++;
        if (!*s) break;
        int len = 0, fit = 0;
        while (s[len] && len < 47) {
            char buf[48];
            memcpy(buf, s, len + 1);
            buf[len + 1] = '\0';
            if (text_width(buf) > max_w) break;
            len++;
            if (s[len] == ' ' || s[len] == '\0') fit = len;
        }
        if (fit == 0) fit = len > 0 ? len : 1;
        memcpy(out[n], s, fit);
        out[n][fit] = '\0';
        s += fit;
        n++;
    }
    while (*s == ' ') s++;
    if (*s && n == lines) {
        size_t l = strlen(out[n - 1]);
        if (l > 2) memcpy(out[n - 1] + l - 2, "..", 3);
    }
    return n;
}

void draw_notify(const NotifyInputs &in) {
    const float b = breath(in.t_ms, 2400);
    uint32_t one[1] = {in.color};
    glow(in.waiting > 1 && in.queue ? in.queue : one, in.waiting > 1 && in.queue ? in.waiting : 1, b);
    if (in.hold > 0) hold_arc(in.hold);

    // Badge: the agent's mark and name, black on its colour.
    const Sprite &mark = MARK[in.agent >= 0 && in.agent < 4 ? in.agent : 3];
    int tw = text_width(in.source), bw = 7 + mark.w + 5 + tw + 8;
    int bx = (int)lroundf(CX - bw / 2.0f);
    cut(bx, 19, bw, 16, in.color);
    sprite(mark, bx + 7, 19 + (16 - mark.h) / 2, BLACK);
    text(in.source, bx + 7 + mark.w + 5, 24, BLACK);

    text(in.title, CX, 44, WHITE, fit_scale(in.title, 176, 3), CENTER);

    // The command (or message) in a terminal-like panel.
    char lines[3][48];
    cut(28, 72, 184, 44, PANEL);
    frame_box(28, 72, 184, 44, DARK);
    if (in.ask) {
        int n = wrap(in.body, 150, lines, 3);
        text(">", 36, 77, in.color);
        for (int i = 0; i < n; i++) text(lines[i], 46, 77 + i * 12, WHITE);
    } else {
        int n = wrap(in.body, 168, lines, 3);
        int y0 = 94 - n * 6;
        for (int i = 0; i < n; i++) text(lines[i], CX, y0 + i * 12, GREY, 1, CENTER);
    }

    // Who else is waiting: one dot each, in queue order.
    if (in.waiting > 1 && in.queue) {
        int n = in.waiting > 8 ? 8 : in.waiting;
        float x0 = CX - (n - 1) * 5.0f;
        for (int i = 0; i < n; i++) disc(x0 + i * 10, 124, i == 0 ? 3 : 2, in.queue[i]);
    }

    static const char *const keys[4] = {"F1", "F2", "F3", "F4"};
    static const int xs[4] = {60, 100, 140, 180};
    static const int ys[4] = {146, 154, 154, 146};
    if (in.ask) {
        bool holding = in.hold > 0;
        text(holding ? "KEEP HOLDING" : "HOLD F1 TO ALLOW", CX, 132, holding ? ALLOW_GREEN : GREY, 1, CENTER);
        static const char *const acts[4] = {"ALLOW", "LATER", "DENY", "LATER"};
        for (int i = 0; i < 4; i++) {
            keycap(xs[i] - KEY_W / 2, ys[i], keys[i], (in.buttons >> i) & 1, false);
            uint32_t c = i == 0 ? ALLOW_GREEN : i == 2 ? DENY_RED : WHITE;
            text(acts[i], xs[i], ys[i] + KEY_H + 5, c, 1, CENTER);
        }
    } else {
        for (int i = 0; i < 4; i++) keycap(xs[i] - KEY_W / 2, ys[i], keys[i], (in.buttons >> i) & 1, false);
        text("ANY KEY TO DISMISS", CX, 182, GREY, 1, CENTER);
    }
}

// --- MUSIC: now playing ---

// A triangle h px tall pointing right (or left), h/2+1 wide, top-left at (x, y).
static void tri(float x, float y, int h, bool right, uint32_t c) {
    int half = h / 2;
    for (int i = 0; i < h; i++) {
        int w = (i <= half ? i : h - 1 - i) + 1;
        rect(right ? x : x + half + 1 - w, y + i, w, 1, c);
    }
}

static void glyph(int g, float cx, float cy, uint32_t c) {
    const int h = 27, w = h / 2 + 1;
    switch (g) {
        case NP_GLYPH_PLAY: tri(cx - w / 2.0f + 2, cy - h / 2.0f, h, true, c); break;
        case NP_GLYPH_PAUSE:
            rect(cx - 10, cy - 12, 7, 25, c);
            rect(cx + 3, cy - 12, 7, 25, c);
            break;
        case NP_GLYPH_NEXT:
            tri(cx - w - 2, cy - h / 2.0f, h, true, c);
            tri(cx - 2, cy - h / 2.0f, h, true, c);
            rect(cx + w - 2, cy - h / 2.0f, 4, h, c);
            break;
        case NP_GLYPH_PREV:
            rect(cx - w - 2, cy - h / 2.0f, 4, h, c);
            tri(cx - w + 2, cy - h / 2.0f, h, false, c);
            tri(cx + 2, cy - h / 2.0f, h, false, c);
            break;
        default: break;
    }
}

// Where each cover style (user_prefs.h cover_style_t) puts the title, the artist, and the middle
// of the volume number / key glyph.
struct NpLayout {
    int title_y, artist_y, overlay_y;
};
static const NpLayout NP_LAYOUT[4] = {
    {174, 194, CY}, // FLAT: on the cover's darkened lower part
    {180, 199, CY - 4}, // RECORD: over the grooves, under the label; the number on the label, over the spindle
    {162, 182, CY}, // SLIDE: under the sleeve
    {188, 206, CY}, // BLEED: under the sleeve
};

// The volume ring: a 1px circle all the way round, a 2px arc over it up to the volume, a dot at
// its end. Every pixel is placed by where its middle is from the middle of the glass, so the
// steps come out even all the way round.
constexpr int VOL_RING_R = 113;

// How far round the glass a pixel is, 0..1: from the bottom, clockwise (as the LED arc).
static float turn_of(float dx, float dy) {
    float t = (atan2f(dy, dx) - (float)M_PI / 2) / (2 * (float)M_PI);
    return t - floorf(t);
}
// A 1px circle: one pixel per row or column, mirrored into the eight octants (no doubled corners).
static void ring_thin(int r, uint32_t c) {
    for (int k = 0;; k++) {
        float dy = k + 0.5f;
        int j = (int)floorf(sqrtf(fmaxf(0.0f, (float)r * r - dy * dy)));
        if (j < k) break;
        rect(CX + j, CY + k, 1, 1, c), rect(CX - 1 - j, CY + k, 1, 1, c);
        rect(CX + j, CY - 1 - k, 1, 1, c), rect(CX - 1 - j, CY - 1 - k, 1, 1, c);
        if (j == k) continue;
        rect(CX + k, CY + j, 1, 1, c), rect(CX - 1 - k, CY + j, 1, 1, c);
        rect(CX + k, CY - 1 - j, 1, 1, c), rect(CX - 1 - k, CY - 1 - j, 1, 1, c);
    }
}
// Every pixel whose middle is r0..r1 from the middle of the glass, from the bottom up to turn t1.
static void arc_band(float r0, float r1, float t1, uint32_t c) {
    for (int y = (int)floorf(CY - r1); y <= (int)ceilf(CY + r1); y++) {
        float dy = y + 0.5f - CY;
        if (fabsf(dy) >= r1) continue;
        int ho = (int)ceilf(sqrtf(r1 * r1 - dy * dy)), hi = fabsf(dy) < r0 ? (int)floorf(sqrtf(r0 * r0 - dy * dy)) : 0;
        for (int i = hi > 0 ? hi - 1 : 0; i <= ho; i++) {
            for (int side = 0; side < 2; side++) {
                int x = side ? CX - 1 - i : CX + i;
                float dx = x + 0.5f - CX, d2 = dx * dx + dy * dy;
                if (d2 >= r0 * r0 && d2 < r1 * r1 && turn_of(dx, dy) <= t1) rect(x, y, 1, 1, c);
            }
        }
    }
}
static void vol_ring(int volume, uint32_t track, uint32_t fill, uint32_t head) {
    ring_thin(VOL_RING_R, track);
    if (volume <= 0) return;
    const float v = volume / 100.0f, a = v * 2 * (float)M_PI + (float)M_PI / 2;
    arc_band(VOL_RING_R - 1, VOL_RING_R + 1, v, fill);
    cut((int)floorf(CX + cosf(a) * VOL_RING_R - 2), (int)floorf(CY + sinf(a) * VOL_RING_R - 2), 5, 5, head);
}

void draw_now_playing(const NowPlayingInputs &in) {
    if (!in.has_cover && in.icon48) image565(CX - 48, 46, 48, 48, in.icon48, 1.0f, 2); // stands in for the cover
    const NpLayout &lay = NP_LAYOUT[in.style >= 0 && in.style < 4 ? in.style : 0];

    const char *title = in.title && in.title[0] ? in.title : "NOW PLAYING";
    text(title, CX, lay.title_y, WHITE, fit_scale(title, 172, 2), CENTER);
    if (in.artist && in.artist[0]) text(in.artist, CX, lay.artist_y, 0xBDBDBD, 1, CENTER);

    if (in.badge || !in.playing) { // a small badge at the top: the style just picked, or paused
        const char *word = in.badge ? in.badge : "PAUSED";
        int w = text_width(word) + (in.badge ? 12 : 22);
        cut((int)(CX - w / 2.0f), 22, w, 15, 0x000000);
        frame_box((int)(CX - w / 2.0f), 22, w, 15, in.badge ? AMBER : DARK);
        if (in.badge) {
            text(word, CX, 26, AMBER, 1, CENTER);
        } else {
            rect(CX - w / 2.0f + 6, 26, 2, 7, WHITE);
            rect(CX - w / 2.0f + 10, 26, 2, 7, WHITE);
            text(word, CX - w / 2.0f + 16, 26, WHITE);
        }
    }
    const float oy = lay.overlay_y;

    // Volume: a ring round the glass and the number in the middle, while the knob turns.
    if (in.volume_k > 0 && in.volume >= 0) {
        vol_ring(in.volume, scale_rgb(DARK, in.volume_k), scale_rgb(in.accent, in.volume_k), scale_rgb(WHITE, in.volume_k));
        shade_disc(CX, oy - 8, 30, 0.85f * in.volume_k); // fades with the ring: no dark spot left behind
        char v[8];
        snprintf(v, sizeof(v), "%d", in.volume);
        text("VOL", CX, oy - 27, scale_rgb(GREY, in.volume_k), 1, CENTER);
        text(v, CX, oy - 15, scale_rgb(WHITE, in.volume_k), 3, CENTER);
    } else if (in.glyph != NP_GLYPH_NONE && in.glyph_k > 0) { // a media key, just pressed
        shade_disc(CX, oy - 8, 30, 0.85f * in.glyph_k);
        glyph(in.glyph, CX, oy - 8, scale_rgb(WHITE, in.glyph_k));
    }
}

// --- CLOCK ---
// Two screens (clock.h CLOCK_BOARD): a flap clock -- four dark cards with white digits, the
// date a row of small flaps -- and an airline board, a row of small flaps per zone. A flap
// turns in sixths: the top half of the old character falls to the hinge, then the bottom half
// of the new one comes down from it. A card turns straight to its digit; a small flap steps
// through its drum (the alphabet, or the digits) until it shows the right character.
// The approved preview: https://claude.ai/artifact/Jfoo7AsJwtCcd4x5o7SDmH (round 2).

static const char FLAP_ALL[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:+-";
static const char FLAP_DIGITS[] = "0123456789";
constexpr uint32_t FLAP_CARD_MS = 300, FLAP_STEP_MS = 60;
// Dark flaps, white characters: white cards were too bright on the panel (2026-10-07).
constexpr uint32_t FLAP_CARD = 0x242424, FLAP_CARD_SHADE = 0x141414; // the flap clock's
constexpr uint32_t FLAP_DARK = 0x1E1E1E, FLAP_DARK_SHADE = 0x101010; // the board's
constexpr int BOARD_COLS = 14; // 7 of the zone, a gap, HH:MM, A / P
constexpr int BOARD_ROWS = 5;  // clock.h CLOCK_SLOTS

struct FlapCell {
    char cur, next;
    uint32_t t0;
};
static FlapCell s_flaps[4 + 10 + BOARD_ROWS * BOARD_COLS]; // the cards, the date, the board
static bool s_flap_busy;

static char flap_char(char c) { // what a drum can show
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return c && strchr(FLAP_ALL, c) ? c : ' ';
}
static char flap_after(char cur, char target) {
    const char *set = strchr(FLAP_DIGITS, cur) && strchr(FLAP_DIGITS, target) ? FLAP_DIGITS : FLAP_ALL;
    const char *p = strchr(set, cur) + 1;
    return *p ? *p : set[0];
}

// One flap at (x, y): `a` turning into `b`, phase 0..1. `big`: a card (a 2px hinge with a notch
// at each end, corners cut by 2); else a small flap, whose 1px hinge goes round the letter.
static void flap_draw(int x, int y, int w, int h, int s, char a, char b, float phase, bool big, uint32_t fg,
                      uint32_t bg, uint32_t shade, uint32_t hinge) {
    const CellGlyph A = cell_glyph(a, w, h, s), B = cell_glyph(b, w, h, s);
    const int hh = h / 2, sixth = (int)lroundf(phase * 6);
    const float p = sixth / 6.0f;
    const int fall = p > 0 && p < 0.5f ? (int)lroundf(hh * (1 - 2 * p)) : 0; // the old top half, this tall
    const int rise = p >= 0.5f && p < 1 ? (int)lroundf(hh * (2 * p - 1)) : 0; // the new bottom half
    const int cut = big ? 2 : 1;
    for (int j = 0; j < h; j++) {
        const bool top = j < hh;
        const int edge = j < cut ? cut - j : j >= h - cut ? cut - (h - 1 - j) : 0; // corner pixels
        int run = 0;
        uint32_t run_c = 0;
        for (int i = 0; i <= w; i++) {
            uint32_t c = 0;
            if (i < w) {
                if (p <= 0) c = cell_ink(A, i, j) ? fg : bg;
                else if (p >= 1) c = cell_ink(B, i, j) ? fg : bg;
                else {
                    c = cell_ink(top ? B : A, i, j) ? fg : bg;
                    if (fall > 0 && top && j >= hh - fall) c = cell_ink(A, i, (j - (hh - fall)) * hh / fall) ? fg : shade;
                    if (rise > 0 && !top && j < hh + rise) c = cell_ink(B, i, hh + (j - hh) * hh / rise) ? fg : shade;
                }
                if (big) {
                    if (j == hh - 1 || j == hh) c = BLACK;
                    if ((i < 2 || i >= w - 2) && j >= hh - 3 && j < hh + 3) c = BLACK;
                } else if (j == hh && c != fg) {
                    c = hinge;
                }
                if (i < edge || i >= w - edge) c = BLACK;
            }
            if (i == w || (run > 0 && c != run_c)) {
                if (run > 0) rect(x + i - run, y + j, run, 1, run_c);
                run = 0;
            }
            run_c = c;
            run++;
        }
    }
}

// Flap `id` shows `target`: draws it where it is on the way there, and notes if it is still turning.
static void flap_cell(int id, char target, bool step, uint32_t now, int x, int y, int w, int h, int s, uint32_t fg,
                      uint32_t bg, uint32_t shade, uint32_t hinge) {
    FlapCell &c = s_flaps[id];
    const uint32_t ms = step ? FLAP_STEP_MS : FLAP_CARD_MS;
    target = flap_char(target);
    if (!c.cur) c.cur = c.next = ' '; // the first time: from blank
    float phase = 0;
    if (c.cur != c.next) {
        if (now - c.t0 >= ms) c.cur = c.next;
        else phase = (float)(now - c.t0) / ms;
    }
    if (c.cur == c.next && c.cur != target) {
        c.next = step ? flap_after(c.cur, target) : target;
        c.t0 = now;
    }
    if (c.cur != c.next) s_flap_busy = true;
    flap_draw(x, y, w, h, s, c.cur, c.next, phase, !step, fg, bg, shade, hinge);
}

static void clock_date(const ClockInputs &in, char d[16]) {
    static const char *const WDAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const MON[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    snprintf(d, 16, "%s %02d %s", WDAY[in.wday % 7], in.mday % 100, MON[in.mon % 12]);
}
static void clock_hhmm(bool valid, bool h24, int hour, int minute, char out[5]) { // "HHMM", "--" while unknown
    if (!valid) {
        memcpy(out, "----", 5);
        return;
    }
    const int h = (unsigned)(h24 ? hour : hour % 12 ? hour % 12 : 12) % 24, m = (unsigned)minute % 60;
    out[0] = h >= 10 || h24 ? (char)('0' + h / 10) : ' ';
    out[1] = (char)('0' + h % 10);
    out[2] = (char)('0' + m / 10);
    out[3] = (char)('0' + m % 10);
    out[4] = 0;
}

static void clock_flaps(const ClockInputs &in) {
    // The zone, and its offset from UTC.
    char head[40];
    int a = in.offset_min < 0 ? -in.offset_min : in.offset_min;
    if (a % 60) snprintf(head, sizeof head, "%s  UTC%c%d:%02d", in.label, in.offset_min < 0 ? '-' : '+', a / 60, a % 60);
    else snprintf(head, sizeof head, "%s  UTC%c%d", in.label, in.offset_min < 0 ? '-' : '+', a / 60);
    text(in.valid ? head : in.label, CX, 44, GREY, 1, CENTER);

    // The time: a card a digit, a pair of dots between the hours and the minutes.
    static const int CARD_X[4] = {31, 74, 127, 170};
    char hm[5];
    clock_hhmm(in.valid, in.h24, in.hour, in.minute, hm);
    for (int i = 0; i < 4; i++)
        flap_cell(i, hm[i], false, in.t_ms, CARD_X[i], 64, 40, 62, 7, WHITE, FLAP_CARD, FLAP_CARD_SHADE, BLACK);
    rect(118, 82, 4, 4, WHITE);
    rect(118, 104, 4, 4, WHITE);
    if (in.valid && !in.h24) text(in.hour < 12 ? "AM" : "PM", 34, 54, in.accent, 1, LEFT);

    // The date, a small flap a character and nothing for a space; or where the time is coming from.
    if (!in.valid) {
        text("WAITING FOR THE TIME", CX, 140, GREY, 1, CENTER);
        text("WIFI, OR THE MAC SERVICE", CX, 154, DARK, 1, CENTER);
    } else if (in.date) {
        char d[16];
        clock_date(in, d);
        for (int i = 0; i < 10 && d[i]; i++) {
            if (d[i] != ' ') flap_cell(4 + i, d[i], true, in.t_ms, 46 + i * 15, 136, 13, 20, 2, WHITE, FLAP_CARD, FLAP_CARD_SHADE, BLACK);
        }
    }

    // A square per zone, the one on show lit.
    if (in.zones > 1) {
        const int y = in.date || !in.valid ? 174 : 150;
        for (int i = 0; i < in.zones; i++) {
            int x = (int)lroundf(CX + (i - (in.zones - 1) / 2.0f) * 12);
            if (i == in.zone) rect(x - 2, y - 2, 5, 5, in.accent);
            else rect(x - 1, y - 1, 3, 3, DARK);
        }
    }
}

static void clock_board(const ClockInputs &in) {
    const int n = in.zones < BOARD_ROWS ? in.zones : BOARD_ROWS, cols = in.h24 ? BOARD_COLS - 1 : BOARD_COLS;
    const int x0 = in.h24 ? 42 : 36, y0 = 62 + (BOARD_ROWS - n) * 21 / 2;
    text("ZONE", x0 + 2, y0 - 14, GREY, 1, LEFT);
    text("TIME", x0 + 8 * 12 + 2, y0 - 14, GREY, 1, LEFT);
    for (int r = 0; r < n; r++) {
        const ClockZone &z = in.rows[r];
        char row[BOARD_COLS + 1], hm[5];
        clock_hhmm(z.valid, in.h24, z.hour, z.minute, hm);
        snprintf(row, sizeof row, "%-8.7s%c%c:%c%c%c", z.label ? z.label : "", hm[0], hm[1], hm[2], hm[3],
                 !z.valid ? ' ' : z.hour < 12 ? 'A' : 'P');
        for (int i = 0; i < cols; i++) {
            if (i == 7) continue; // the gap between the zone and its time
            flap_cell(14 + r * BOARD_COLS + i, row[i], true, in.t_ms, x0 + i * 12, y0 + r * 21, 11, 18, 2,
                      r == in.zone ? in.accent : WHITE, FLAP_DARK, FLAP_DARK_SHADE, BLACK);
        }
    }
    const int below = y0 + n * 21 + 7;
    if (!in.valid) {
        text("WAITING FOR THE TIME", CX, below, GREY, 1, CENTER);
    } else if (in.date) {
        char d[16];
        clock_date(in, d);
        text(d, CX, below, WHITE, 1, CENTER);
    }
}

bool draw_clock(const ClockInputs &in) {
    // The seconds: a small dot each round the glass, lit up to now; the five-second marks and
    // the second itself a pixel bigger.
    if (in.seconds && in.valid) {
        for (int s = 0; s < 60; s++) {
            float a = s * 2 * (float)M_PI / 60 - (float)M_PI / 2;
            int sz = s % 5 == 0 || s == in.second ? 2 : 1;
            uint32_t c = s == in.second ? WHITE : s < in.second ? in.accent : DARK;
            rect(floorf(CX + cosf(a) * 112 - sz / 2.0f + 0.5f), floorf(CY + sinf(a) * 112 - sz / 2.0f + 0.5f), sz, sz, c);
        }
    }
    s_flap_busy = false;
    if (in.board && in.rows) clock_board(in);
    else clock_flaps(in);
    return s_flap_busy;
}

// --- AGENTS: the dashboard ---

void draw_agent_board(const AgentRowView *rows, int n, uint32_t t_ms) {
    if (n == 0) {
        text("NO AGENTS RUNNING", CX, 86, GREY, 1, CENTER);
        text("CLAUDE  CODEX  CURSOR", CX, 102, DARK, 1, CENTER);
        return;
    }
    static const char *const STATE[4] = {"IDLE", "WORKING", "YOUR TURN", "ASKING"};
    const int y0 = n <= 2 ? 78 : 62;
    for (int i = 0; i < n && i < 4; i++) {
        const AgentRowView &r = rows[i];
        int y = y0 + i * 19;
        int st = r.state >= 0 && r.state < 4 ? r.state : 0;
        bool blink = ((t_ms / 450) % 2) == 0;
        disc(52, y + 3, 3, st == 3 && !blink ? scale_rgb(r.color, 0.35f) : r.color);
        text(r.name, 62, y, WHITE);
        uint32_t sc = st == 1 ? AMBER : st == 2 ? WHITE : st == 3 ? (blink ? r.color : WHITE) : GREY;
        if (st == 1) { // working: three dots that walk
            int w = text(STATE[st], 178, y, sc, 1, RIGHT);
            (void)w;
            for (int d = 0; d < 3; d++) rect(182 + d * 4, y + 5, 2, 2, (int)((t_ms / 300) % 4) > d ? AMBER : DARK);
        } else {
            text(STATE[st], 190, y, sc, 1, RIGHT);
        }
    }
}

// --- HOME ---
// Design round 2 (2026-10-04): the lamps drawn after the real devices, the Mi badge at the top;
// the scale being turned shows on the LED ring (led_task.c), not on the screen. A lamp's colour (its colour or its
// white, dimmed with its brightness), the white and hue scales and the Mi orange are HOME's
// palette exceptions (PIXEL_ART.md section 2); a lamp that's off or unplugged keeps to the palette.

static const uint32_t MI_ORANGE = 0xFF6900u;
static const uint8_t BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
static inline bool dith(int x, int y, float d) { return BAYER4[y & 3][x & 3] < d * 16; }

// A lamp's light at a brightness: its colour, dimmed, but never below a third (or it reads as off).
static uint32_t lamp_lit(uint32_t rgb, int bright) { return scale_rgb(rgb, 0.34f + 0.66f * bright / 100.0f); }

// How a lamp is drawn: its lines, its body, and its light (0 = none: off or unplugged).
struct LampLook {
    uint32_t line, body, light;
    int bright;
};
static LampLook lamp_look(const home_lamp_view_t &l, bool chosen) {
    if (!l.online) return {DARK, BLACK, 0, 0};
    return {chosen ? WHITE : GREY, DARK, (l.known && l.on) ? l.rgb : 0, l.bright};
}

// A line `w` pixels wide in whole-pixel steps.
static void home_thick(int xa, int ya, int xb, int yb, int w, uint32_t c) {
    int n = abs(xb - xa) > abs(yb - ya) ? abs(xb - xa) : abs(yb - ya);
    for (int i = 0; i <= n; i++) rect(lroundf(xa + (xb - xa) * (float)i / n), lroundf(ya + (yb - ya) * (float)i / n), w, 1, c);
}

// A desk lamp's light: a widening cone of dithered pixels in its colour, thinner further down
// and with less brightness.
static void home_cone(int x0, int x1, int y0, int y1, float spread, uint32_t rgb, int bright) {
    for (int y = y0; y <= y1; y++) {
        float d = (y - y0) / (float)(y1 > y0 ? y1 - y0 : 1), half = (y - y0) * spread;
        float dens = (0.06f + 0.32f * bright / 100.0f) * (1 - 0.7f * d);
        for (int x = (int)lroundf(x0 - half); x <= (int)lroundf(x1 + half); x++)
            if (dith(x, y, dens)) rect(x, y, 1, 1, rgb);
    }
}

// The lamps, about 36 x 32 px (big) and 18 x 16 px (small), centred on (cx, cy).
static void lamp_big(int kind, int cx, int cy, const LampLook &o) {
    const uint32_t glow = o.light ? lamp_lit(o.light, o.bright) : 0;
    switch (kind) {
        case HOME_KIND_DESK: // Desk Lamp 2: a block foot, a straight column, a broad head
            if (o.light) home_cone(cx - 6, cx + 16, cy - 11, cy + 13, 0.3f, o.light, o.bright);
            cut(cx - 17, cy + 10, 22, 5, o.line); rect(cx - 16, cy + 11, 20, 3, o.body); rect(cx - 2, cy + 12, 4, 1, o.line);
            rect(cx - 13, cy - 13, 5, 24, o.line); rect(cx - 12, cy - 12, 3, 22, o.body);
            cut(cx - 13, cy - 17, 32, 6, o.line); rect(cx - 12, cy - 16, 30, 4, o.body);
            rect(cx - 6, cy - 12, 23, 1, glow ? glow : DARK);
            break;
        case HOME_KIND_DESK_ARM: // Desk Lamp 1S: a round foot, one slim arm, a long thin light bar
            if (o.light) home_cone(cx - 1, cx + 14, cy - 11, cy + 13, 0.32f, o.light, o.bright);
            cut(cx - 16, cy + 10, 16, 5, o.line); rect(cx - 15, cy + 11, 14, 3, o.body);
            home_thick(cx - 9, cy + 9, cx - 3, cy - 11, 2, o.line);
            cut(cx - 5, cy - 15, 4, 4, o.line);
            cut(cx - 3, cy - 15, 21, 4, o.line); rect(cx - 2, cy - 14, 19, 2, o.body);
            rect(cx - 1, cy - 12, 16, 1, glow ? glow : DARK);
            break;
        case HOME_KIND_STRIP: { // Lightstrip: the controller, then the strip in a wave with its LEDs
            auto ys = [&](int x) { return cy + 1 + (int)lroundf(4 * sinf((x - (cx - 10)) / 26.0f * 2 * (float)M_PI)); };
            if (o.light)
                for (int x = cx - 10; x <= cx + 18; x++)
                    for (int d = -4; d <= 4; d++)
                        if (abs(d) > 1 && dith(x, ys(x) + d, (0.06f + 0.26f * o.bright / 100.0f) * (1 - abs(d) / 5.0f))) rect(x, ys(x) + d, 1, 1, o.light);
            rect(cx - 21, cy, 3, 1, o.line);
            cut(cx - 18, cy - 3, 8, 7, o.line); rect(cx - 17, cy - 2, 6, 5, o.body); rect(cx - 15, cy, 2, 1, o.light ? o.light : GREY);
            int prev = ys(cx - 10);
            for (int x = cx - 10; x <= cx + 18; x++) {
                int y = ys(x), lo = y < prev ? y : prev, hi = y < prev ? prev : y;
                rect(x, lo - 1, 1, hi - lo + 1, o.line);
                rect(x, lo + 1, 1, hi - lo + 1, o.line);
                rect(x, y, 1, 1, (x & 1) == 0 ? (glow ? glow : DARK) : o.body);
                prev = y;
            }
            break;
        }
        default: { // a bulb: glass, neck, threads; rays that grow with brightness
            const int gy = cy - 6;
            if (o.light) {
                int n = 1 + (int)lroundf(o.bright / 50.0f);
                static const int deg[5] = {180, 225, 270, 315, 0};
                for (int a : deg) {
                    float r = a * (float)M_PI / 180;
                    for (int k = 0; k < n; k++) rect(lroundf(cx + cosf(r) * (15 + k * 4)) - 1, lroundf(gy + sinf(r) * (15 + k * 4)) - 1, 2, 2, o.light);
                }
            }
            disc(cx, gy, 11, o.line);
            disc(cx, gy, 10, glow ? glow : BLACK);
            static const int neck[6] = {7, 6, 6, 5, 5, 4};
            for (int i = 0; i < 6; i++) {
                rect(cx - neck[i], gy + 8 + i, 2 * neck[i], 1, o.line);
                rect(cx - neck[i] + 1, gy + 8 + i, 2 * neck[i] - 2, 1, glow ? glow : BLACK);
            }
            for (int j = 0; j < 6; j++) rect(cx - ((j & 1) ? 4 : 5), gy + 14 + j, (j & 1) ? 8 : 10, 1, (j & 1) ? o.body : o.line);
            rect(cx - 2, gy + 20, 4, 1, o.line);
            if (o.light) { rect(cx - 6, gy - 5, 2, 3, WHITE); rect(cx - 4, gy - 7, 2, 1, WHITE); }
            break;
        }
    }
}

static void lamp_small(int kind, int cx, int cy, const LampLook &o) {
    const uint32_t glow = o.light ? lamp_lit(o.light, o.bright) : DARK;
    switch (kind) {
        case HOME_KIND_DESK:
            rect(cx - 8, cy + 6, 10, 2, o.line); rect(cx - 6, cy - 5, 2, 11, o.line); rect(cx - 6, cy - 8, 15, 3, o.line);
            rect(cx - 3, cy - 5, 11, 1, glow);
            break;
        case HOME_KIND_DESK_ARM:
            rect(cx - 7, cy + 6, 7, 2, o.line); home_thick(cx - 4, cy + 5, cx - 1, cy - 5, 1, o.line); rect(cx - 2, cy - 7, 11, 2, o.line);
            rect(cx - 1, cy - 5, 9, 1, glow);
            break;
        case HOME_KIND_STRIP:
            rect(cx - 9, cy - 1, 3, 3, o.line);
            for (int x = cx - 5; x <= cx + 8; x++) rect(x, cy + lroundf(2 * sinf((x - cx + 5) / 13.0f * 2 * (float)M_PI)), 1, 1, (x & 1) == 0 ? glow : o.line);
            break;
        default:
            disc(cx, cy - 2, 6, o.line); disc(cx, cy - 2, 5, o.light ? glow : BLACK);
            rect(cx - 3, cy + 4, 6, 1, o.line); rect(cx - 2, cy + 5, 4, 1, o.body); rect(cx - 3, cy + 6, 6, 1, o.line); rect(cx - 1, cy + 7, 2, 1, o.line);
            break;
    }
}

// Xiaomi's mark: the orange square with Quadra's cut corners, "mi" in 1 px white strokes.
static const Sprite SPR_MI = {9, 9,
    "........." "........." ".#####.#." ".#.#.#.#." ".#.#.#.#." ".#.#.#.#." ".#.#.#.#." "........." "........."};
static void mi_badge(int x, int y) {
    cut(x, y, 9, 9, MI_ORANGE);
    sprite(SPR_MI, x, y, WHITE, 1);
}

// The status strip: the Mi badge and a name (HOME, or the lamp being changed).
static void home_top(const char *name) {
    int tw = text_width(name), total = 9 + 6 + tw;
    int x0 = (int)lroundf(CX - total / 2.0f);
    mi_badge(x0, 30);
    text(name, x0 + 15, 31, WHITE);
    rect(56, 48, 128, 1, DARK);
}

// F1-F4 on the arc of the glass, as on the Main Screen; "" = the key does nothing here.
static void home_legend(const char *const acts[4], uint8_t buttons) {
    static const char *const keys[4] = {"F1", "F2", "F3", "F4"};
    static const int xs[4] = {60, 100, 140, 180};
    static const int ys[4] = {146, 154, 154, 146};
    for (int i = 0; i < 4; i++) {
        bool off = acts[i][0] == '\0';
        keycap(xs[i] - KEY_W / 2, ys[i], keys[i], (buttons >> i) & 1, off);
        text(off ? "--" : acts[i], xs[i], ys[i] + KEY_H + 5, off ? GREY : WHITE, 1, CENTER);
    }
}

static const char *home_status(const home_lamp_view_t &l) {
    if (!l.online) return "OFFLINE";
    if (!l.known || l.failed) return "NO REPLY";
    return l.on ? "ON" : "OFF";
}

static void draw_home_scan(const HomeInputs &in) {
    const home_snapshot_t &s = *in.snap;
    home_top("HOME");
    const int cy = 86;
    for (int i = 0; i < 3; i++) { // rings go out from the bulb and fade: white, grey, dark
        float k = fmodf(in.t_ms / 1500.0f + i / 3.0f, 1.0f);
        ring(CX, cy, 20 + k * 18, k < 0.34f ? WHITE : k < 0.67f ? GREY : DARK, 1);
    }
    lamp_big(HOME_KIND_BULB, CX, cy, {WHITE, DARK, 0, 0});
    text("SEARCHING", CX, 122, GREY, 1, CENTER);
    char found[16];
    snprintf(found, sizeof(found), "%d OF %d", s.found, s.count);
    text(found, CX, 134, WHITE, 1, CENTER);
    static const char *const acts[4] = {"", "", "", "MENU"};
    home_legend(acts, in.buttons);
}

static void draw_home_message(const HomeInputs &in, const char *msg, const char *hint) {
    home_top("HOME");
    lamp_big(HOME_KIND_BULB, CX, 82, {DARK, BLACK, 0, 0});
    text(msg, CX, 108, WHITE, 2, CENTER);
    text(hint, CX, 126, GREY, 1, CENTER);
    static const char *const acts[4] = {"", "", "", "MENU"};
    home_legend(acts, in.buttons);
}

// Ten blocks, dim to full in the lamp's colour, lit up to its brightness.
static void home_meter(int x, int y, const home_lamp_view_t &l) {
    int n = (l.bright + 5) / 10;
    for (int i = 0; i < 10; i++) rect(x + i * 4, y, 3, 5, i < n ? lamp_lit(l.rgb, (i + 1) * 10) : DARK);
}

static void draw_home_list(const HomeInputs &in) {
    const home_snapshot_t &s = *in.snap;
    home_top("HOME");
    const int cy = 84;
    clip(16, 50, 208, 70);
    for (int j = -2; j <= 2; j++) {
        int i = s.selected + j;
        if (i < 0 || i >= s.count) continue;
        int x = (int)lroundf(CX + j * 66 + in.slide_px);
        if (abs(x - CX) > 112) continue;
        const home_lamp_view_t &l = s.lamps[i];
        if (abs(x - CX) < 33) lamp_big(l.kind, x, cy, lamp_look(l, true));
        else lamp_small(l.kind, x, cy, lamp_look(l, false));
    }
    unclip();
    if (s.selected > 0) sprite(SPR_TRI_L_M, 20, cy - 3, AMBER);
    if (s.selected < s.count - 1) sprite(SPR_TRI_R_M, 216, cy - 3, AMBER);
    const home_lamp_view_t &l = s.lamps[s.selected];
    text(l.name, CX, 112, l.online ? WHITE : GREY, fit_scale(l.name, 176, 2), CENTER);
    const char *st = home_status(l);
    if (l.online && l.known && l.on && !l.failed) {
        int w = text_width(st) + 6 + 39, x0 = (int)lroundf(CX - w / 2.0f);
        text(st, x0, 129, WHITE);
        home_meter(x0 + text_width(st) + 6, 130, l);
    } else {
        text(st, CX, 129, l.failed ? AMBER : GREY, 1, CENTER);
    }
    bool live = l.online && l.known;
    const char *acts[4] = {l.online ? (live ? "EDIT" : "") : "RETRY", live ? "POWER" : "", "SCAN", "MENU"};
    home_legend(acts, in.buttons);
}

static const char *const HOME_OPT_NAME[HOME_OPT_COUNT] = {"BRIGHT", "TEMP", "COLOR"};
static bool home_has(const home_lamp_view_t &l, int o) {
    return l.caps & (o == HOME_OPT_BRIGHT ? HOME_CAP_BRIGHT : o == HOME_OPT_TEMP ? HOME_CAP_TEMP : HOME_CAP_COLOR);
}

static void draw_home_edit(const HomeInputs &in) {
    const home_snapshot_t &s = *in.snap;
    const home_lamp_view_t &l = s.lamps[s.selected];
    home_top(l.name);

    // The options this lamp has, the one being turned in amber.
    int widths = 0, n = 0;
    for (int o = 0; o < HOME_OPT_COUNT; o++) {
        if (home_has(l, o)) widths += text_width(HOME_OPT_NAME[o]), n++;
    }
    float x = lroundf(CX - (widths + (n - 1) * 12) / 2.0f);
    for (int o = 0; o < HOME_OPT_COUNT; o++) {
        if (!home_has(l, o)) continue;
        x += text(HOME_OPT_NAME[o], x, 56, o == s.option ? AMBER : GREY) + 12;
    }

    char v[12];
    const char *caption = "";
    if (s.option == HOME_OPT_BRIGHT) {
        snprintf(v, sizeof(v), "%u%%", l.bright);
    } else if (s.option == HOME_OPT_TEMP) {
        snprintf(v, sizeof(v), "%uK", l.ct);
        caption = l.ct < 3300 ? "WARM" : l.ct < 5000 ? "NEUTRAL" : "COOL";
    } else {
        snprintf(v, sizeof(v), "%u", l.hue);
        caption = "HUE";
    }
    int sc = fit_scale(v, 110, 3);
    int w = text(v, CX, 70, AMBER, sc, CENTER);
    edit_arrows(CX, 70, w, cap_height(sc), AMBER);
    if (l.failed) text("NO REPLY", CX, 92, AMBER, 1, CENTER);
    else if (!l.on) text("OFF", CX, 92, GREY, 1, CENTER);
    else if (caption[0]) text(caption, CX, 92, GREY, 1, CENTER);

    lamp_big(l.kind, CX, 120, lamp_look(l, true));
    static const char *const acts[4] = {"NEXT", "POWER", "BACK", "MENU"};
    home_legend(acts, in.buttons);
}

void draw_home(const HomeInputs &in) {
    const home_snapshot_t &s = *in.snap;
    switch (s.phase) {
        case HOME_PHASE_NO_WIFI: draw_home_message(in, "NO WIFI", "SET UP: QUADRA.PY WIFI"); break;
        case HOME_PHASE_EMPTY: draw_home_message(in, "NO LAMPS", "QUADRA.PY HOME IMPORT"); break;
        case HOME_PHASE_LIST:
            if (s.count > 0) draw_home_list(in);
            break;
        case HOME_PHASE_EDIT:
            if (s.count > 0) draw_home_edit(in);
            break;
        default: draw_home_scan(in); break; // SCAN, and OFF for the moment before the first snapshot
    }
}

// --- MIDI ---
#define MIDI_PROG_SHOW_MS 1500

// The status strip: the maker's wordmark (or its name in grey), the synth in white.
static void midi_top(const midi_synth_t &sy) {
    const Sprite *logo = maker_logo(sy.maker);
    int wm = logo ? logo->w : text_width(sy.maker), wn = text_width(sy.name), total = wm + 7 + wn;
    int x0 = (int)lroundf(CX - total / 2.0f);
    if (logo) sprite(*logo, x0, 31, WHITE);
    else text(sy.maker, x0, 31, GREY);
    text(sy.name, x0 + wm + 7, 31, WHITE);
    rect(56, 48, 128, 1, DARK);
}

static void midi_value_text(const midi_param_t &p, int v, char *out, size_t n) {
    if (v < 0) {
        snprintf(out, n, "--");
    } else if (p.n_opts > 1) {
        snprintf(out, n, "%s", p.opt_name[v]);
    } else if (p.bipolar) { // around the middle: -64..+63, -512..+511
        int d = v - (midi_param_max(&p) + 1) / 2;
        snprintf(out, n, d > 0 ? "+%d" : "%d", d);
    } else {
        snprintf(out, n, "%d", v);
    }
}

// A continuous value: 31 blocks, lit up to it (a centred value: from the middle out). A switch:
// one box per option, the chosen one amber.
static void midi_meter(const midi_param_t &p, int v, int y) {
    if (p.n_opts > 1) {
        const int gap = 4, w = (124 - (p.n_opts - 1) * gap) / p.n_opts;
        int x = (int)lroundf(CX - (w * p.n_opts + gap * (p.n_opts - 1)) / 2.0f);
        for (int o = 0; o < p.n_opts; o++, x += w + gap) cut(x, y, w, 5, o == v ? AMBER : DARK);
        return;
    }
    const int N = 31, max = midi_param_max(&p);
    const int at = v < 0 ? -1 : (int)lroundf((float)v / max * (N - 1)), mid = N / 2;
    for (int i = 0; i < N; i++) {
        bool lit = at >= 0 && (p.bipolar ? (at >= mid ? i >= mid && i <= at : i >= at && i <= mid) : i <= at);
        uint32_t c = lit ? WHITE : (p.bipolar && i == mid) ? GREY : DARK;
        rect(58 + i * 4, y, 3, 5, c);
    }
}

// F1 held: the synth's parameters, five at a time, the one under the knob on a dark bar.
static void draw_midi_list(const midi_snapshot_t &s, const midi_synth_t &sy) {
    char idx[24];
    snprintf(idx, sizeof(idx), "%d/%d", s.param + 1, sy.n_params);
    text(idx, CX, 56, GREY, 1, CENTER);
    for (int k = -2; k <= 2; k++) {
        int i = s.param + k;
        if (i < 0 || i >= sy.n_params) continue;
        const midi_param_t &p = sy.params[i];
        char row[32];
        snprintf(row, sizeof(row), "%s %s", p.group, p.name);
        int y = 97 + k * 14;
        if (k == 0) {
            cut(46, y - 3, 148, 13, DARK);
            text(row, CX, y, AMBER, 1, CENTER);
        } else {
            text(row, CX, y, abs(k) == 1 ? GREY : DARK, 1, CENTER);
        }
    }
}

void draw_midi(const MidiInputs &in) {
    const midi_snapshot_t &s = *in.snap;
    const midi_synth_t &sy = *midi_synth_get(s.synth);
    const midi_param_t &p = sy.params[s.param < sy.n_params ? s.param : 0];
    midi_top(sy);
    if (s.browsing) {
        draw_midi_list(s, sy);
        static const char *const acts[4] = {"PICK", "", "", "MENU"};
        home_legend(acts, in.buttons);
        return;
    }
    text(p.group, CX, 56, GREY, 1, CENTER);
    text(p.name, CX, 68, WHITE, fit_scale(p.name, 170, 2), CENTER);
    char v[24];
    midi_value_text(p, s.value, v, sizeof(v));
    int sc = fit_scale(v, 150, 3);
    int w = text(v, CX, 88, s.value < 0 ? GREY : AMBER, sc, CENTER);
    if (s.value >= 0) edit_arrows(CX, 88, w, cap_height(sc), AMBER);
    midi_meter(p, s.value, 116);

    // The ports, or the program just sent.
    char line[32];
    if (s.prog >= 0 && in.t_ms - s.prog_ms < MIDI_PROG_SHOW_MS) {
        snprintf(line, sizeof(line), "PROG %03d", s.prog + 1);
        text(line, CX, 129, AMBER, 1, CENTER);
    } else {
        snprintf(line, sizeof(line), "CH %02d", s.channel);
        int wc = text_width(line), wu = text_width("USB"), wt = text_width("TRS");
        int x0 = (int)lroundf(CX - (wc + 10 + wu + 10 + wt) / 2.0f);
        text(line, x0, 129, WHITE);
        text("USB", x0 + wc + 10, 129, s.usb ? WHITE : DARK);
        text("TRS", x0 + wc + 10 + wu + 10, 129, s.trs ? WHITE : DARK);
    }
    static const char *const acts[4] = {"NEXT", "PROG-", "PROG+", "MENU"};
    home_legend(acts, in.buttons);
}

// The idle screen's icon in HOME: the lamp as drawn on the list, lit in its colour, into a 48 x 48
// RGB565 big-endian image (black = see-through, the icon format). Its three accents (dark to
// bright) are the lamp's colour at a third, two thirds and full.
void home_idle_icon(const home_lamp_view_t &l, uint8_t *icon48, uint32_t accents[3]) {
    memset(icon48, 0, 48 * 48 * 2);
    target((uint16_t *)icon48, 48, 48);
    LampLook o = lamp_look(l, true);
    lamp_big(l.kind, 24, 26, o);
    target(nullptr, 0, 0);
    uint32_t c = o.light ? o.light : GREY;
    accents[0] = scale_rgb(c, 0.34f);
    accents[1] = scale_rgb(c, 0.67f);
    accents[2] = c;
}

} // namespace ui
