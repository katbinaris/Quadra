#include "ui_screens.hpp"
#include "ui_gfx.hpp"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
#include "ui_state.h"
}

// Layout note: small text is the 10px font (caps 7px) and icons are the hand-drawn 1.5x set
// (SPR_*_M; 3x where they were 2x). Tuned on hardware: 8px text / 1x icons read too small,
// 12px text / 2x icons too big. Positions are
// checked with the host preview (tools/ui_preview/run.sh), which renders this exact code.

namespace ui {

static const char *feel_name(haptic_type_t t) {
    return t == HAPTIC_TYPE_SAW ? "SAW" : t == HAPTIC_TYPE_SINE ? "SINE" : "VISCOSE";
}

static const Sprite &mode_icon(menu_hid_type_t m) {
    switch (m) {
        case MENU_HID_KEYBOARD: return SPR_KBD;
        case MENU_HID_MOUSE: return SPR_MOUSE;
        case MENU_HID_APP: return SPR_CUBE;
        case MENU_HID_HOME: return SPR_BULB;
        default: return SPR_NOTE;
    }
}

static const Sprite &mode_icon_m(menu_hid_type_t m) {
    switch (m) {
        case MENU_HID_KEYBOARD: return SPR_KBD_M;
        case MENU_HID_MOUSE: return SPR_MOUSE_M;
        case MENU_HID_APP: return SPR_CUBE_M;
        case MENU_HID_HOME: return SPR_BULB_M;
        default: return SPR_NOTE_M;
    }
}

static const char *mode_name(menu_hid_type_t m) {
    switch (m) {
        case MENU_HID_KEYBOARD: return "KEYBOARD";
        case MENU_HID_MOUSE: return "MOUSE";
        case MENU_HID_APP: return "APP";
        case MENU_HID_HOME: return "HOME";
        default: return "MIDI";
    }
}

// Sprite drawn at `scale`, centered on (cx, cy).
static void sprite_c(const Sprite &s, float cx, float cy, uint32_t c, int scale) {
    sprite(s, lroundf(cx - s.w * scale / 2.0f), lroundf(cy - s.h * scale / 2.0f), c, scale);
}

static void save_hint(float y, bool dirty, bool blink_on) {
    if (dirty && blink_on) text("F2 SAVE", CX, y, AMBER, 1, CENTER);
}

static void header(const char *title) {
    text(title, CX, 24, GREY, 1, CENTER);
    rect(60, 40, 120, 1, DARK);
}

// --- Main Screen: status strip + Cartridge layout (no logo) ---

// The knob's job (3x icon + name, centered on y=86) and the active feel (still curve + name).
static void draw_mode_and_feel(const MainInputs &in) {
    const Sprite &icon = mode_icon_m(in.mode);
    const char *name = mode_name(in.mode);
    int icon_w = icon.w * 2, tw = text_width(name, 2);
    float x0 = lroundf(CX - (icon_w + 10 + tw) / 2.0f);
    sprite_c(icon, x0 + icon_w / 2.0f, 86, WHITE, 2);
    text(name, x0 + icon_w + 10, 81, WHITE, 2);

    const char *feel = feel_name(in.feel);
    float fx = lroundf(CX - (28 + 8 + text_width(feel)) / 2.0f);
    plot_curve(in.feel, (int)fx, 122, 28, 5, 1, 0, WHITE, 2, -1, 1);
    text(feel, fx + 36, 119, GREY);
}

static void draw_status_and_middle(const MainInputs &in);
static void draw_legend(const MainInputs &in);

// 24x24 app icon + name, centered on (cx, cy); returns the pair's width.
static int app_badge(const char *name, const uint8_t *icon, float cx, float cy, uint32_t c) {
    int tw = text_width(name), total = (icon ? 24 + 6 : 0) + tw;
    int x0 = (int)lroundf(cx - total / 2.0f);
    if (icon) image565(x0, (int)lroundf(cy - 12), 24, 24, icon);
    text(name, x0 + (icon ? 30 : 0), cy - 3, c);
    return total;
}

// APP mode's status strip and middle: the app instead of HID/tone, and what the knob does
// right now instead of the mode icon + feel.
static void draw_app_top(const AppView &app) {
    app_badge(app.name, app.icon24, CX, 36, WHITE);
    rect(56, 52, 128, 1, DARK);
    if (app.echo) {
        if (app.echo_scene != nullptr) {
            draw_card(app.echo_scene, CX - CARD_W / 2, 56, app.echo_ms);
            text(app.echo_name, CX, 124, AMBER, 1, CENTER);
        } else {
            text(app.echo_name, CX, 92, AMBER, fit_scale(app.echo_name, 176, 2), CENTER);
        }
        return;
    }
    if (app.shape != nullptr) {
        // Micro-interaction: the shape follows the knob; under it, key (grey) + action.
        shape_scene(*app.shape);
        int kw = text_width(app.action_key), aw = text_width(app.action);
        float lx = lroundf(CX - (kw + 7 + aw) / 2.0f);
        text(app.action_key, lx, 131, GREY);
        text(app.action, lx + kw + 7, 131, app.flash ? AMBER : WHITE);
        return;
    }
    text(app.action_via, CX, 82, GREY, 1, CENTER);
    text(app.action, CX, 98, WHITE, fit_scale(app.action, 176, 3), CENTER);
}

void draw_main(const MainInputs &in) {
    if (in.param != nullptr) {
        draw_param(*in.param);
        return;
    }
    if (in.wheel != nullptr) {
        draw_wheel(*in.wheel);
        return;
    }
    if (in.app != nullptr) {
        draw_app_top(*in.app);
    } else {
        draw_status_and_middle(in);
    }
    draw_legend(in);
}

// Status strip (USB mode, click tone) + the knob's job: the non-APP main screen.
static void draw_status_and_middle(const MainInputs &in) {
    // Status strip: USB mode, click tone -- 1.5x icons, 10px text, vertically centered on y=36.
    const char *usb = in.usb_serial ? "SERIAL" : "HID";
    const char *tone = (in.tone == AUDIO_TIMBRE_WOOD_TOCK) ? "WOOD" : "THUD";
    int w_usb = text_width(usb), w_tone = text_width(tone);
    int total = SPR_USB_M.w + 5 + w_usb + 16 + SPR_SPK_M.w + 5 + w_tone;
    float x = lroundf(CX - total / 2.0f);
    sprite_c(SPR_USB_M, x + SPR_USB_M.w / 2.0f, 36, WHITE, 1);
    x += SPR_USB_M.w + 5;
    text(usb, x, 33, GREY);
    x += w_usb + 16;
    sprite_c(SPR_SPK_M, x + SPR_SPK_M.w / 2.0f, 36, WHITE, 1);
    x += SPR_SPK_M.w + 5;
    text(tone, x, 33, GREY);
    rect(56, 52, 128, 1, DARK);

    // An uploaded icon (HID vendor interface, RAM only) takes over the middle: it replaces
    // the mode icon + name and the feel line, drawn 1:1 centered between the strip and keys.
    if (in.icon != nullptr) {
        image565(CX - 24, 76, 48, 48, in.icon);
    } else {
        draw_mode_and_feel(in);
    }
}

// Key legend on the arc of the glass; a key lights amber while its button is held. APP mode:
// the keys are the profile's controls (long-press F4 is still the menu).
static void draw_legend(const MainInputs &in) {
    static const char *const keys[4] = {"F1", "F2", "F3", "F4"};
    static const char *const menu_acts[4] = {"SEL", "", "BACK", "MENU"}; // F2 has no job here
    const char *const *acts = in.app != nullptr ? in.app->legend : menu_acts;
    static const int xs[4] = {60, 100, 140, 180};
    static const int ys[4] = {146, 154, 154, 146};
    for (int i = 0; i < 4; i++) {
        bool disabled = acts[i][0] == '\0';
        keycap(xs[i] - KEY_W / 2, ys[i], keys[i], (in.buttons_held >> i) & 1, disabled);
        text(disabled ? "--" : acts[i], xs[i], ys[i] + KEY_H + 5, disabled ? GREY : WHITE, 1, CENTER);
    }
}

// --- Top-level menu ---
// The hardware-confirmed list: text scrolls past one fixed amber highlight; rows exactly two
// ranks from the selected one are dimmed.

void draw_menu_list(const menu_render_snapshot_t &snap, float scroll) {
    cut(20, 106, 200, 28, AMBER);
    clip(20, 40, 200, 160); // the visible band -- rows slide out of it, not off the glass
    for (int i = 0; i < snap.row_count; i++) {
        float cy = CY + i * 32 - scroll;
        if (cy < 30 || cy > 210) continue;
        int dist = abs(i - snap.selected);
        uint32_t c = (i == snap.selected) ? BLACK : dist == 2 ? GREY : WHITE;
        text(snap.rows[i].label, CX, cy - 5, c, 2, CENTER);
    }
    unclip();
}

// --- Haptics: Orbit with icons + labels ---
// Each ring item is a stack around its anchor point: 1.5x icon, label, value.

static const float ORBIT_R = 82;
static const float ORBIT_STEP_DEG = 360.0f / (int)MENU_HAPTIC_ROW_COUNT;

static void orbit_icon(int row, haptic_type_t feel, float cx, float cy, uint32_t c) {
    const Sprite *s = nullptr;
    switch (row) {
        case MENU_HAPTIC_ROW_STEPS: s = &SPR_STEPS_M; break;
        case MENU_HAPTIC_ROW_SNAP: s = &SPR_SNAP_M; break;
        case MENU_HAPTIC_ROW_DAMP: s = &SPR_DAMP_M; break;
        case MENU_HAPTIC_ROW_SHAPE: s = &SPR_SHAPE_M; break;
        case MENU_HAPTIC_ROW_AMP: s = &SPR_SPK_M; break;
        case MENU_HAPTIC_ROW_PITCH: s = &SPR_PITCH_M; break;
        default: break;
    }
    if (s == nullptr) { // FEEL: its own curve
        plot_curve(feel, (int)lroundf(cx - 12), (int)lroundf(cy), 24, 5, 1, 0, c, 2, -1, 1);
        return;
    }
    sprite_c(*s, cx, cy, c, 1);
}

// FEEL animation: the curve scrolls under a fixed knob dot (SAW, SINE); VISCOSE drifts slowly
// and the dot sways, dragging a fading trail.
static void feel_anim(haptic_type_t type, int x, int y, int w, float amp, float periods, uint32_t t,
                      uint32_t c, int morph_from, float blend) {
    float speed = type == HAPTIC_TYPE_VISCOSE ? 1 / 5200.0f : type == HAPTIC_TYPE_SAW ? 1 / 1500.0f : 1 / 1300.0f;
    float phase = fmodf(t * speed, 1.0f);
    plot_curve(type, x, y, w, amp, periods, phase, c, 2, morph_from, blend);
    auto dot_y = [&](float px) {
        float u = fmodf((px - x) / w * periods + phase, 1.0f);
        if (u < 0) u += 1.0f;
        float v = wave_y(type, u);
        if (morph_from >= 0 && blend < 1.0f) v = wave_y((haptic_type_t)morph_from, u) * (1 - blend) + v * blend;
        return y + v * amp;
    };
    if (type == HAPTIC_TYPE_VISCOSE) {
        float cx = x + w / 2.0f + sinf(t / 1700.0f) * (w * 0.28f);
        float dir = cosf(t / 1700.0f) >= 0 ? 1.0f : -1.0f;
        for (int k = 3; k >= 1; k--) {
            float tx = cx - dir * k * 6;
            rect(tx - 1, dot_y(tx) - 1, 3, 3, k == 1 ? WHITE : GREY);
        }
        rect(cx - 3, dot_y(cx) - 3, 7, 7, AMBER);
    } else {
        float cx = x + w / 2.0f;
        rect(cx - 3, dot_y(cx) - 3, 7, 7, AMBER);
    }
}

// STEPS illustration: a dial with one tick per step, and a dot hopping from tick to tick --
// the spacing is the real one.
static void steps_dial(int steps, float cx, float cy, float r, uint32_t t, uint32_t c) {
    if (steps < 1) { // SMOOTH: no steps -- an unbroken ring, the dot gliding round it
        for (float a = 0; a < 360.0f; a += 2.0f) {
            float ra = a * (float)M_PI / 180.0f;
            rect(lroundf(cx + r * cosf(ra)), lroundf(cy + r * sinf(ra)), 1, 1, c);
        }
        float ra = (t / 9.0f - 90.0f) * (float)M_PI / 180.0f;
        rect(lroundf(cx + r * cosf(ra)) - 2, lroundf(cy + r * sinf(ra)) - 2, 5, 5, AMBER);
        return;
    }
    int at = (int)((t / 280) % (uint32_t)steps);
    float hx = cx, hy = cy - r;
    for (int i = 0; i < steps; i++) {
        float a = (-90.0f + i * 360.0f / steps) * (float)M_PI / 180.0f;
        float x = cx + r * cosf(a), y = cy + r * sinf(a);
        if (steps > 24) rect(lroundf(x), lroundf(y), 1, 1, c);
        else rect(lroundf(x) - 1, lroundf(y) - 1, 2, 2, c);
        if (i == at) hx = x, hy = y;
    }
    rect(lroundf(hx) - 2, lroundf(hy) - 2, 5, 5, AMBER);
}

void draw_orbit(const menu_render_snapshot_t &snap, const OrbitInputs &in) {
    for (int i = 0; i < snap.row_count && i < MENU_HAPTIC_ROW_COUNT; i++) {
        float a = (-90.0f + i * ORBIT_STEP_DEG) * (float)M_PI / 180.0f;
        float x = CX + ORBIT_R * cosf(a), y = CY + ORBIT_R * sinf(a);
        bool f = (i == snap.selected);
        bool muted = snap.rows[i].muted; // not usable in this feel: all grey, "--"
        orbit_icon(i, in.feel, x, y - 14, muted ? GREY : f ? AMBER : WHITE);
        text(snap.rows[i].label, x, y - 6, f ? AMBER : GREY, 1, CENTER);
        text(snap.rows[i].value, x, y + 4, muted ? GREY : WHITE, 1, CENTER);
    }
    // Focus arc on the rim -- moves around the glass with the knob.
    if (snap.selected >= 0) {
        float a0 = -90.0f + snap.selected * ORBIT_STEP_DEG;
        for (float d = -17; d <= 17; d += 0.4f) {
            float a = (a0 + d) * (float)M_PI / 180.0f;
            float ca = cosf(a), sa = sinf(a);
            for (int r = 112; r <= 114; r++) rect(CX + r * ca, CY + r * sa, 1, 1, AMBER);
        }
    }
    if (snap.selected < 0) return;
    // Center column: x 85..155 is free between the side stacks.
    const menu_render_row_t &p = snap.rows[snap.selected];
    uint32_t vc = snap.editing ? AMBER : WHITE;
    if (snap.selected == MENU_HAPTIC_ROW_FEEL) {
        text(p.caption, CX, 72, GREY, 1, CENTER);
        feel_anim(in.feel, 86, 102, 68, 11, 2, in.t_ms, vc, in.morph_from, in.morph_blend);
        int sc = fit_scale(p.value, 70, 2);
        int w = text(p.value, CX, 124, vc, sc, CENTER);
        if (snap.editing) edit_arrows(CX, 124, w, cap_height(sc), AMBER);
        save_hint(146, snap.dirty, in.blink_on);
    } else if (snap.selected == MENU_HAPTIC_ROW_STEPS) {
        text(p.caption, CX, 72, GREY, 1, CENTER);
        steps_dial(in.steps, CX, 101, 15, in.t_ms, WHITE);
        int sc = fit_scale(p.value, 70, 2);
        int w = text(p.value, CX, 124, vc, sc, CENTER);
        if (snap.editing) edit_arrows(CX, 124, w, cap_height(sc), AMBER);
        save_hint(146, snap.dirty, in.blink_on);
    } else if (snap.selected == MENU_HAPTIC_ROW_SHAPE) {
        // The SAW curve itself, bending as the value turns (SHAPE is SAW only).
        text(p.caption, CX, 72, GREY, 1, CENTER);
        feel_anim(HAPTIC_TYPE_SAW, 86, 102, 68, 11, 2, in.t_ms, vc, -1, 1);
        int sc = fit_scale(p.value, 70, 2);
        int w = text(p.value, CX, 124, vc, sc, CENTER);
        if (snap.editing) edit_arrows(CX, 124, w, cap_height(sc), AMBER);
        save_hint(146, snap.dirty, in.blink_on);
    } else {
        text(p.caption, CX, 80, GREY, 1, CENTER);
        int sc = fit_scale(p.value, 70, 3);
        int w = text(p.value, CX, 96, vc, sc, CENTER);
        if (snap.editing) edit_arrows(CX, 96, w, cap_height(sc), AMBER);
        plot_curve(in.feel, 92, 130, 56, 5, 2, 0, GREY, 1, -1, 1);
        save_hint(146, snap.dirty, in.blink_on);
    }
}

// --- HID type: carousel ---

void draw_hid(const menu_render_snapshot_t &snap, const HidInputs &in) {
    header("PROFILES");
    bool type_focus = (snap.selected <= 0);
    int pos = menu_hid_type_pos(in.type); // display order: APP first (menu.h)
    for (int j = -2; j <= 2; j++) {
        int idx = (int)menu_hid_type_at(pos + j);
        float x = CX + j * 72 + in.slide_px;
        if (fabsf(x - CX) > 104) continue;
        bool center = fabsf(x - CX) < 36;
        // Center: 1.5x set at 2x (3x); sides: the 1x set at 2x.
        if (center) sprite_c(mode_icon_m((menu_hid_type_t)idx), x, 80, WHITE, 2);
        else sprite_c(mode_icon((menu_hid_type_t)idx), x, 80, GREY, 2);
    }
    if (type_focus) {
        sprite_c(SPR_TRI_L_M, 22, 80, AMBER, 1);
        sprite_c(SPR_TRI_R_M, 218, 80, AMBER, 1);
    }
    text(mode_name(in.type), CX, 108, type_focus ? WHITE : GREY, 2, CENTER);
    if (in.type == MENU_HID_APP && in.profile_name != nullptr) {
        // The chosen profile, for reference; F1 opens the PROFILE screen to change it.
        app_badge(in.profile_name, in.profile_icon, CX, 146, WHITE);
        text("F1 PROFILE", CX, 190, GREY, 1, CENTER);
    } else if ((in.type == MENU_HID_KEYBOARD || in.type == MENU_HID_MOUSE) && snap.row_count > 1) {
        // The haptic profile this mode uses; F1 moves between the mode and it.
        bool hp_focus = snap.selected == 1;
        text("HAPTIC", CX, 132, GREY, 1, CENTER);
        int w = text(snap.rows[1].value, CX, 146, hp_focus ? AMBER : WHITE, 2, CENTER);
        if (hp_focus) edit_arrows(CX, 146, w, cap_height(2), AMBER);
        text(hp_focus ? "F1 TYPE" : "F1 HAPTIC", CX, 190, GREY, 1, CENTER);
    } else if (in.type == MENU_HID_MIDI && snap.row_count > 2) {
        // The channel and the synth, both at scale 2 so they read from arm's length; the one
        // being turned is amber with arrows. The maker's wordmark (1x) sits before the synth.
        char ch[16];
        snprintf(ch, sizeof(ch), "CH %s", snap.rows[1].value);
        const char *sy = in.midi_synth ? in.midi_synth : snap.rows[2].value;
        const Sprite *logo = in.midi_maker ? maker_logo(in.midi_maker) : nullptr;
        bool ch_focus = snap.selected == 1, sy_focus = snap.selected == 2;
        int w = text(ch, CX, 126, ch_focus ? AMBER : WHITE, 2, CENTER);
        if (ch_focus) edit_arrows(CX, 126, w, cap_height(2), AMBER);
        const int lw = logo ? logo->w + 7 : 0;
        const int sc = fit_scale(sy, 180 - lw, 2), tw = text_width(sy, sc), total = lw + tw;
        const int x0 = (int)lroundf(CX - total / 2.0f), y = 148;
        if (logo) sprite(*logo, x0, y + (cap_height(sc) - logo->h) / 2, sy_focus ? AMBER : WHITE);
        text(sy, x0 + lw, y, sy_focus ? AMBER : WHITE, sc);
        if (sy_focus) edit_arrows(CX, y, total, cap_height(sc), AMBER);
        text(snap.selected == 0 ? "F1 CHANNEL" : ch_focus ? "F1 SYNTH" : "F1 TYPE", CX, 190, GREY, 1, CENTER);
    } else if (!snap.dirty) {
        text("IN USE", CX, 134, GREY, 1, CENTER);
    }
    save_hint(170, snap.dirty, in.blink_on);
}

// --- App profile: carousel of app icons ---

#define PROFILE_SPACING 80

void draw_app_profile(const menu_render_snapshot_t &snap, const ProfileInputs &in) {
    header("APP PROFILE");
    for (int j = -2; j <= 2; j++) {
        int idx = in.index + j;
        if (idx < 0 || idx >= in.count) continue; // no wrap: the list has ends
        float x = CX + j * PROFILE_SPACING + in.slide_px;
        if (fabsf(x - CX) > 104) continue;
        const ProfileItem &it = in.items[idx];
        if (fabsf(x - CX) < PROFILE_SPACING / 2) {
            if (it.icon48) image565((int)lroundf(x) - 24, 60, 48, 48, it.icon48);
            else sprite_c(SPR_CUBE_M, x, 84, WHITE, 2);
        } else {
            if (it.icon24) image565((int)lroundf(x) - 12, 72, 24, 24, it.icon24, 0.4f);
            else sprite_c(SPR_CUBE, x, 84, GREY, 2);
        }
    }
    // Arrows only where there's somewhere to go.
    if (in.index > 0) sprite_c(SPR_TRI_L_M, 22, 84, AMBER, 1);
    if (in.index < in.count - 1) sprite_c(SPR_TRI_R_M, 218, 84, AMBER, 1);

    const ProfileItem &cur = in.items[in.index];
    text(cur.name, CX, 118, WHITE, fit_scale(cur.name, 170, 2), CENTER);
    // What F1-F4 will do with this profile.
    char line[40];
    snprintf(line, sizeof(line), "F1 %s  F2 %s", cur.legend[0], cur.legend[1]);
    text(line, CX, 140, GREY, 1, CENTER);
    snprintf(line, sizeof(line), "F3 %s  F4 %s", cur.legend[2], cur.legend[3]);
    text(line, CX, 154, GREY, 1, CENTER);

    if (snap.dirty) save_hint(174, true, in.blink_on);
    else text("IN USE", CX, 174, GREY, 1, CENTER);
    // Position in the list: one dot per profile, the chosen one amber.
    int dots_w = in.count * 8 - 4;
    for (int i = 0; i < in.count; i++) {
        rect(CX - dots_w / 2.0f + i * 8, 194, 4, 4, i == in.index ? AMBER : DARK);
    }
}

// --- Boot mode: two cards ---

void draw_display(const menu_render_snapshot_t &snap, int rotation, bool blink_on) {
    header("DISPLAY");
    text("ROTATION", CX, 54, GREY, 1, CENTER);
    // "This way up": a 4x arrow head on a short shaft.
    sprite_c(SPR_TRI_U, CX, 76, WHITE, 4);
    rect(CX - 2, 82, 4, 12, WHITE);
    char deg[8];
    snprintf(deg, sizeof(deg), "%d", rotation * 90);
    int w = text(deg, CX, 106, WHITE, 3, CENTER);
    frame_box((int)lroundf(CX + w / 2.0f) + 3, 106, 5, 5, WHITE); // degree mark
    edit_arrows(CX, 106, w + 16, cap_height(3), AMBER);
    text("TURN TO ROTATE", CX, 140, GREY, 1, CENTER);
    save_hint(160, snap.dirty, blink_on);
}

void draw_boot_mode(const menu_render_snapshot_t &snap, boot_usb_mode_t selected, bool serial_in_use, bool blink_on) {
    header("BOOT MODE");
    struct Card { int x; const char *label; const Sprite *icon; boot_usb_mode_t mode; };
    const Card cards[2] = {{34, "HID", &SPR_USB, BOOT_USB_MODE_HID}, {126, "SERIAL", &SPR_TERM, BOOT_USB_MODE_SERIAL}};
    for (const Card &c : cards) {
        bool sel = c.mode == selected;
        const int y = 50, w = 80, h = 74;
        if (sel) {
            frame_box(c.x - 2, y - 2, w + 4, h + 4, AMBER);
            frame_box(c.x, y, w, h, AMBER);
        } else {
            frame_box(c.x, y, w, h, DARK);
        }
        sprite_c(*c.icon, c.x + w / 2.0f, y + 22, sel ? WHITE : GREY, 2);
        text(c.label, c.x + w / 2.0f, y + 44, sel ? WHITE : GREY, 1, CENTER);
        bool in_use = (c.mode == BOOT_USB_MODE_SERIAL) == serial_in_use;
        if (in_use) text("IN USE", c.x + w / 2.0f, y + 58, sel ? AMBER : GREY, 1, CENTER);
    }
    text("APPLIES AFTER RESTART", CX, 136, GREY, 1, CENTER);
    text("HOLD F3+F4 AT BOOT", CX, 154, GREY, 1, CENTER);
    text("TO FORCE SERIAL", CX, 167, GREY, 1, CENTER);
    save_hint(188, snap.dirty, blink_on);
}

// --- Device -> Sys Info: four pages of live readings ---

// "0.43A" from mA.
static void fmt_amps(char *buf, size_t n, unsigned ma) {
    snprintf(buf, n, "%u.%02uA", ma / 1000, (ma % 1000) / 10);
}

// Label left in grey, value right-aligned -- the column the glass leaves room for from y 40
// to 200.
static void kv(const char *label, const char *value, float y, uint32_t vc = WHITE) {
    text(label, 52, y, GREY);
    text(value, 188, y, vc, 1, RIGHT);
}

// A thin gauge: `frac` of it filled, an optional amber tick at `peak`.
static void gauge(int x, int y, int w, float frac, uint32_t c, float peak = -1) {
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    rect(x, y, w, 4, DARK);
    rect(x, y, (int)lroundf(w * frac), 4, c);
    if (peak > 0) rect(x + (int)lroundf((w - 1) * (peak > 1 ? 1 : peak)), y - 2, 1, 8, AMBER);
}

static void degree_after(const char *s, float cx, float y, int scale, uint32_t c) {
    int w = text(s, cx, y, c, scale, CENTER);
    int d = scale >= 3 ? 5 : 3; // the DISPLAY screen's degree mark at 3x, a smaller one below
    frame_box((int)lroundf(cx + w / 2.0f) + scale + 1, (int)y, d, d, c);
}

static void sysinfo_power(const sysmon_info_t &in, const pd_status_t &power) {
    bool have = power.source != PD_SRC_READING && power.source != PD_SRC_NO_CHIP && power.ma > 0;
    char buf[48], avail[12];
    text("DRAW, ESTIMATED", CX, 50, GREY, 1, CENTER);
    fmt_amps(buf, sizeof(buf), in.total_ma);
    bool over = have && in.total_ma > power.ma;
    text(buf, CX, 62, over ? AMBER : WHITE, 3, CENTER);
    if (have) {
        float frac = (float)in.total_ma / power.ma;
        gauge(60, 90, 120, frac, frac > 0.8f ? AMBER : WHITE, (float)in.total_peak_ma / power.ma);
    }
    const char *source;
    switch (power.source) {
        case PD_SRC_PD: source = "USB PD"; break;
        case PD_SRC_TYPEC_1A5:
        case PD_SRC_TYPEC_3A0: source = "USB-C"; break;
        case PD_SRC_USB: source = "USB"; break;
        case PD_SRC_NO_CHIP: source = "PD CHIP NOT FOUND"; break;
        default: source = "USB: READING"; break;
    }
    if (have) {
        fmt_amps(avail, sizeof(avail), power.ma);
        snprintf(buf, sizeof(buf), "OF %s %dV %s", avail, (power.mv > 0 ? power.mv : 5000) / 1000, source);
        text(buf, CX, 100, GREY, 1, CENTER);
    } else {
        text(source, CX, 100, GREY, 1, CENTER);
    }
    fmt_amps(buf, sizeof(buf), in.motor_ma);
    kv("MOTOR", buf, 120);
    fmt_amps(buf, sizeof(buf), in.led_ma);
    kv("LEDS", buf, 134);
    fmt_amps(buf, sizeof(buf), in.board_ma);
    kv("BOARD", buf, 148);
    fmt_amps(buf, sizeof(buf), in.total_peak_ma);
    kv("PEAK", buf, 164, AMBER);
}

static void sysinfo_heat(const sysmon_info_t &in) {
    char buf[24];
    text("CHIP", CX, 50, GREY, 1, CENTER);
    if (in.chip_ok) {
        snprintf(buf, sizeof(buf), "%.1f", (double)in.chip_c);
        degree_after(buf, CX, 62, 3, WHITE);
        snprintf(buf, sizeof(buf), "PEAK %.1f", (double)in.chip_peak_c);
        degree_after(buf, CX, 90, 1, GREY);
    } else {
        text("--", CX, 62, WHITE, 3, CENTER);
        text("NO SENSOR", CX, 90, GREY, 1, CENTER);
    }
    rect(60, 106, 120, 1, DARK);
    text("MOTOR COIL", CX, 114, GREY, 1, CENTER);
    fmt_amps(buf, sizeof(buf), in.coil_ma);
    kv("CURRENT", buf, 130);
    fmt_amps(buf, sizeof(buf), in.coil_peak_ma);
    kv("PEAK", buf, 144, AMBER);
    snprintf(buf, sizeof(buf), "%.2fW", (double)in.copper_w);
    kv("HEAT", buf, 158);
}

static void sysinfo_cpu(const sysmon_info_t &in) {
    char buf[24];
    for (int core = 0; core < 2; core++) {
        int y = 50 + core * 26;
        snprintf(buf, sizeof(buf), "CORE %d", core);
        text(buf, 52, y, GREY);
        snprintf(buf, sizeof(buf), "%u%%", in.load[core]);
        text(buf, 188, y, WHITE, 1, RIGHT);
        gauge(52, y + 12, 136, in.load[core] / 100.0f, WHITE, in.load_peak[core] / 100.0f);
    }
    // WORK's average is TOTAL on the LOOP page; here the spike rate takes its row.
    snprintf(buf, sizeof(buf), "%.2fKHZ", (double)in.loop_khz);
    kv("LOOP", buf, 106);
    snprintf(buf, sizeof(buf), "%.0f/S", (double)in.spikes_per_s);
    kv("SPIKES", buf, 120, in.spikes_per_s >= 1 ? AMBER : WHITE);
    snprintf(buf, sizeof(buf), "%.1fUS", (double)in.work_max_us);
    kv("WORK MAX", buf, 134, AMBER);
    snprintf(buf, sizeof(buf), "%.1fUS", (double)in.jitter_max_us);
    kv("JITTER", buf, 148, AMBER);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)in.missed);
    kv("MISSED", buf, 162, in.missed ? AMBER : WHITE);
}

// One control iteration taken apart: average and worst per part, in microseconds.
static void sysinfo_loop(const sysmon_info_t &in) {
    text("US", 52, 54, GREY);
    text("AVG", 148, 54, GREY, 1, RIGHT);
    text("MAX", 188, 54, GREY, 1, RIGHT);
    rect(52, 66, 136, 1, DARK);
    static const char *const names[SYSMON_SEC_COUNT] = {"INPUT", "SENSOR", "FORCE", "MOTOR"};
    char buf[16];
    float y = 74;
    for (int i = 0; i < SYSMON_SEC_COUNT; i++, y += 14) {
        text(names[i], 52, y, GREY);
        snprintf(buf, sizeof(buf), "%.1f", (double)in.sec_avg_us[i]);
        text(buf, 148, y, WHITE, 1, RIGHT);
        snprintf(buf, sizeof(buf), "%.1f", (double)in.sec_max_us[i]);
        text(buf, 188, y, AMBER, 1, RIGHT);
    }
    text("OTHER", 52, y, GREY);
    snprintf(buf, sizeof(buf), "%.1f", (double)in.other_avg_us);
    text(buf, 148, y, WHITE, 1, RIGHT);
    y += 18;
    rect(52, y - 6, 136, 1, DARK);
    text("TOTAL", 52, y, GREY);
    snprintf(buf, sizeof(buf), "%.1f", (double)in.work_avg_us);
    text(buf, 148, y, in.work_avg_us > 80 ? AMBER : WHITE, 1, RIGHT);
    snprintf(buf, sizeof(buf), "%.0f", (double)in.work_max_us);
    text(buf, 188, y, AMBER, 1, RIGHT);
    // Bad sensor frames: should stay 0 (a faster SPI clock that's too fast shows up here).
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)in.sensor_crc_errors);
    kv("SENSOR CRC ERR", buf, y + 20, in.sensor_crc_errors ? AMBER : WHITE);
}

static void sysinfo_system(const sysmon_info_t &in) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%luK", (unsigned long)(in.heap_free / 1024));
    kv("RAM FREE", buf, 64);
    snprintf(buf, sizeof(buf), "%luK", (unsigned long)(in.heap_min / 1024));
    kv("RAM LOW", buf, 80);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)in.hid_drops);
    kv("HID DROPS", buf, 104, in.hid_drops ? AMBER : WHITE);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)in.audio_gaps);
    kv("AUDIO GAPS", buf, 120, in.audio_gaps ? AMBER : WHITE);
    unsigned long s = in.uptime_s;
    snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
    kv("UPTIME", buf, 144);
}

void draw_sysinfo(const menu_render_snapshot_t &snap, const sysmon_info_t &info, const pd_status_t &power) {
    int page = snap.selected < 0 ? 0 : snap.selected;
    header(page < snap.row_count ? snap.rows[page].label : "SYS INFO");
    switch (page) {
        case MENU_SYSINFO_POWER: sysinfo_power(info, power); break;
        case MENU_SYSINFO_HEAT: sysinfo_heat(info); break;
        case MENU_SYSINFO_CPU: sysinfo_cpu(info); break;
        case MENU_SYSINFO_LOOP: sysinfo_loop(info); break;
        default: sysinfo_system(info); break;
    }
    text("F1 RESET PEAKS", CX, 182, GREY, 1, CENTER);
    int dots_w = snap.row_count * 8 - 4;
    for (int i = 0; i < snap.row_count; i++) {
        rect(CX - dots_w / 2.0f + i * 8, 198, 4, 4, i == page ? AMBER : DARK);
    }
}

// --- Device -> Bindings: MAC / PC ---

void draw_bindings(const menu_render_snapshot_t &snap, menu_host_t host, bool blink_on) {
    header("BINDINGS");
    text("COMPUTER", CX, 50, GREY, 1, CENTER);
    struct Card { int x; const char *label; const char *mod; menu_host_t host; };
    const Card cards[2] = {{34, "MAC", "CMD", MENU_HOST_MAC}, {126, "PC", "CTRL", MENU_HOST_PC}};
    for (const Card &c : cards) {
        bool sel = c.host == host;
        const int y = 64, w = 80, h = 62;
        if (sel) {
            frame_box(c.x - 2, y - 2, w + 4, h + 4, AMBER);
            frame_box(c.x, y, w, h, AMBER);
        } else {
            frame_box(c.x, y, w, h, DARK);
        }
        text(c.label, c.x + w / 2.0f, y + 16, sel ? WHITE : GREY, 2, CENTER);
        text(c.mod, c.x + w / 2.0f, y + 42, sel ? AMBER : GREY, 1, CENTER);
    }
    if (host == MENU_HOST_PC) {
        text("CMD SHORTCUTS", CX, 144, GREY, 1, CENTER);
        text("ARE SENT AS CTRL", CX, 157, GREY, 1, CENTER);
    } else {
        text("SHORTCUTS ARE SENT", CX, 144, GREY, 1, CENTER);
        text("AS WRITTEN", CX, 157, GREY, 1, CENTER);
    }
    save_hint(178, snap.dirty, blink_on);
}

// --- Device -> Recalibrate ---

void draw_recalibrate(const menu_render_snapshot_t &snap) {
    header("RECALIBRATE");

    // F1 arms it (filled), a second F1 runs it.
    bool armed = snap.row_count > 0 && strcmp(snap.rows[0].value, MENU_RECAL_ARMED) == 0;
    const int bx = 44, by = 80, bw = 152, bh = 26;
    if (armed) {
        cut(bx, by, bw, bh, AMBER);
    } else {
        frame_box(bx, by, bw, bh, AMBER);
    }
    int sc = fit_scale("RECALIBRATE", bw - 12, 2);
    text("RECALIBRATE", CX, by + (bh - cap_height(sc)) / 2, armed ? BLACK : WHITE, sc, CENTER);
    if (armed) {
        text("HANDS OFF THE KNOB", CX, 124, WHITE, 1, CENTER);
        text("F1 AGAIN TO START", CX, 140, AMBER, 1, CENTER);
        text("F3 CANCEL", CX, 158, GREY, 1, CENTER);
    } else {
        text("F1 RECALIBRATE", CX, 124, GREY, 1, CENTER);
        text("RESTARTS, THEN", CX, 142, GREY, 1, CENTER);
        text("REALIGNS THE MOTOR", CX, 155, GREY, 1, CENTER);
    }
}

// --- SAVED! dialog ---

void draw_saved_toast(const char *msg) {
    // Tall enough to cover the Orbit center value (y 96..111) completely.
    cut(66, 90, 108, 50, BLACK);
    frame_box(66, 90, 108, 50, WHITE);
    frame_box(68, 92, 104, 46, GREY);
    text(msg, CX, 110, AMBER, fit_scale(msg, 96, 2), CENTER);
}

} // namespace ui
