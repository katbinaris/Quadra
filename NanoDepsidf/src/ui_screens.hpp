#pragma once
// Pixel UI screens (DEVELOPMENT_PLAN.md "Pixel UI"), ported from the approved mockups. Each
// draws one full screen into the bound sprite from plain inputs -- no state of their own;
// display_task.cpp owns timing, transitions and the animation state they're handed.

#include <stdint.h>
#include "haptic_params.h"
#include "ui_shape.hpp"
#include "ui_cards.hpp"
extern "C" {
#include "menu.h"
#include "audio_trigger.h"
#include "boot_mode.h"
#include "pd_status.h"
#include "sysmon.h"
}

namespace ui {

// APP mode's view of the active app profile (display_task.cpp fills it from
// app_profiles/; the screens stay free of engine types).
struct AppView {
    const char *name;         // "FIGMA"
    const uint8_t *icon24;    // 24x24 RGB565 BE status-bar icon, or nullptr
    const char *legend[4];    // under the F1-F4 keycaps
    const char *action;       // what the knob does right now ("ZOOM", "UNDO/REDO")
    const char *action_via;   // how: "KNOB" or "F1 + KNOB"
    const char *action_key;   // short form under a shape: "KNOB", "F2"
    const ShapeView *shape;   // profile shows a 3D shape instead of the label, or nullptr
    bool flash;               // a tap action just fired (the shape's action line goes amber)
    // A command-wheel command just ran: its card replays in the middle with its name.
    bool echo;
    const app_scene_t *echo_scene; // may be nullptr (name only)
    const char *echo_name;
    uint32_t echo_ms;              // since it ran
};

struct MainInputs {
    bool usb_serial;          // this boot's USB personality (status strip)
    audio_click_timbre_t tone;
    menu_hid_type_t mode;     // knob's job: keyboard / mouse / MIDI
    haptic_type_t feel;
    uint8_t buttons_held;     // UI_BTN_* -- lit keycaps
    const uint8_t *icon;      // HID-uploaded 48x48 RGB565 (big-endian) icon, or nullptr
    const AppView *app;       // APP mode: the active profile, or nullptr
    const WheelView *wheel;   // APP mode, command wheel open: it takes the whole screen
    const ParamView *param;   // APP mode, parameter mode: likewise
};
void draw_main(const MainInputs &in);

// Top-level menu: the hardware-confirmed scrolling list, text only.
void draw_menu_list(const menu_render_snapshot_t &snap, float scroll_px);

struct OrbitInputs {
    haptic_type_t feel;
    uint32_t t_ms;     // FEEL animation clock
    int morph_from;    // previous feel type while morphing, -1 otherwise
    float morph_blend; // 0..1
    bool blink_on;     // "F2 SAVE" blink phase
    int steps;         // detents per turn, for the STEPS dial
};
void draw_orbit(const menu_render_snapshot_t &snap, const OrbitInputs &in);

struct HidInputs {
    menu_hid_type_t type;
    float slide_px;    // carousel offset while sliding to `type`, 0 at rest
    bool blink_on;
    const char *profile_name;     // APP: the PROFILE row's profile
    const uint8_t *profile_icon;  // its 24x24 icon, or nullptr
    const char *midi_synth;       // MIDI: the SYNTH row's synth, in full (the row's value is cut short)
    const char *midi_maker;       // and its maker ("KORG"), for the wordmark
};
void draw_hid(const menu_render_snapshot_t &snap, const HidInputs &in);

// APP -> F1: choose the app profile. A non-wrapping carousel: the chosen app's 48px icon in
// the middle, its neighbours' 24px icons dimmed at the sides, the name, and a preview of what
// F1-F4 will do.
struct ProfileItem {
    const char *name;
    const uint8_t *icon24, *icon48; // RGB565 BE, either may be nullptr
    const char *legend[4];
};
struct ProfileInputs {
    const ProfileItem *items;
    int count;
    int index;         // the chosen profile
    float slide_px;    // carousel offset while sliding to `index`, 0 at rest
    bool blink_on;
};
void draw_app_profile(const menu_render_snapshot_t &snap, const ProfileInputs &in);

void draw_boot_mode(const menu_render_snapshot_t &snap, boot_usb_mode_t selected, bool serial_in_use, bool blink_on);

// DISPLAY: screen rotation. The screen turns live while this is adjusted, so an arrow marks
// which way is up now; the value is the angle in degrees.
void draw_display(const menu_render_snapshot_t &snap, int rotation, bool blink_on);

// DEVICE -> SYS INFO: one page per snapshot row (MENU_SYSINFO_*), the selected one shown, dots
// for the others. `power` is the USB contract the STUSB4500 negotiated (read at boot).
void draw_sysinfo(const menu_render_snapshot_t &snap, const sysmon_info_t &info, const pd_status_t &power);

// DEVICE -> BINDINGS: which computer (MAC / PC), two cards; turning switches it live.
void draw_bindings(const menu_render_snapshot_t &snap, menu_host_t host, bool blink_on);

// DEVICE -> RECALIBRATE: takes a second F1 to run (its row value is MENU_RECAL_ARMED in between).
void draw_recalibrate(const menu_render_snapshot_t &snap);

void draw_saved_toast(const char *msg = "SAVED!");

} // namespace ui
