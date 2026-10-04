#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "haptic_params.h"
#include "audio_trigger.h"
#include "boot_mode.h"

// Phase 8: real configuration menu, replacing the Phase 4 mock (three static labels,
// BTN_D toggle only). See DEVELOPMENT_PLAN.md Phase 8 for the full screen hierarchy,
// button roles and 8-step build order -- this file is step 1: the navigation framework +
// data-driven rendering. Placeholder settings values only (menu.c) -- no real haptic/HID/
// boot config is read or written yet, that's steps 2-7.
//
// Owned/driven from Core 0 (control_task.c, where button/knob reads already live per the
// architecture log in DEVELOPMENT_PLAN.md) -- menu_input_*() below must stay cheap and
// bounded, since they run inside that file's hard-real-time 10kHz control loop. All string
// formatting happens lazily on the consumer side instead (menu_get_render_snapshot(),
// called only from display_task.c on Core 1, which has no real-time deadline to violate) --
// see the state-lock comment in menu.c for the hardware incident that made this the design,
// not a stylistic choice.

#define MENU_MAX_VISIBLE_ITEMS 8
#define MENU_LABEL_TEXT_LEN 20
#define MENU_CAPTION_TEXT_LEN 12
#define MENU_VALUE_TEXT_LEN 12
#define MENU_TITLE_LEN 24

// Pixel UI (DEVELOPMENT_PLAN.md "Pixel UI"): every screen has its own layout on the display
// side (text list, Orbit dashboard, HID carousel, Boot mode cards), so the snapshot names the
// screen explicitly instead of the renderer guessing from the title.
typedef enum {
    MENU_SCREEN_NONE = 0, // menu closed -- Main Screen
    MENU_SCREEN_ROOT,
    MENU_SCREEN_HAPTIC,
    MENU_SCREEN_HID,
    MENU_SCREEN_BOOT,
    MENU_SCREEN_APP_PROFILE, // PROFILES = APP -> F1: choose the app profile
    MENU_SCREEN_DISPLAY,     // screen rotation
    MENU_SCREEN_DEVICE,      // a list: SYS INFO, BINDINGS, RECALIBRATE
    MENU_SCREEN_SYSINFO,     // live readings (sysmon.h), one page per row; F1 resets the peaks
    MENU_SCREEN_RECALIBRATE, // forget the motor calibration and restart
    MENU_SCREEN_BINDINGS,    // which computer: MAC or PC (Cmd <-> Ctrl)
    MENU_SCREEN_LIGHTS,      // LED colour, effect, speed, level (user_prefs.h)
} menu_screen_id_t;

// Rows of the LIGHTS screen -- display_task.cpp draws each by index.
enum {
    MENU_LIGHTS_ROW_COLOR = 0,
    MENU_LIGHTS_ROW_HUE,
    MENU_LIGHTS_ROW_SAT,
    MENU_LIGHTS_ROW_EFFECT,
    MENU_LIGHTS_ROW_SPEED,
    MENU_LIGHTS_ROW_LEVEL,
    MENU_LIGHTS_ROW_COUNT,
};

// DEVICE -> BINDINGS: the computer on the other end. Profiles are written with macOS
// shortcuts; on PC, usb_task.c sends Ctrl wherever a profile says Cmd. The values are what
// NVS stores -- never reorder.
typedef enum { MENU_HOST_MAC = 0, MENU_HOST_PC, MENU_HOST_COUNT } menu_host_t;

// SYS INFO's pages -- the knob turns through them. display_task.cpp draws each from sysmon.h.
enum {
    MENU_SYSINFO_POWER = 0,
    MENU_SYSINFO_HEAT,
    MENU_SYSINFO_CPU,
    MENU_SYSINFO_LOOP,   // where one control iteration's time goes
    MENU_SYSINFO_SYSTEM,
    MENU_SYSINFO_PAGE_COUNT,
};

// RECALIBRATE's row: its value is this while armed (F1 pressed once, waiting for the
// confirming F1), "" otherwise.
#define MENU_RECAL_ARMED "ARMED"

// Screen rotation in quarter turns clockwise (0-3), on top of the panel's mounting offset
// (lgfx_config.hpp). For holding the device in any orientation.
#define MENU_DISPLAY_ROTATIONS 4

// Row indices of the Haptic screen -- display_task.cpp keys its per-setting icons and the
// FEEL animation off these.
enum {
    MENU_HAPTIC_ROW_STEPS = 0,
    MENU_HAPTIC_ROW_SNAP,
    MENU_HAPTIC_ROW_DAMP,
    MENU_HAPTIC_ROW_SHAPE,
    MENU_HAPTIC_ROW_FEEL,
    MENU_HAPTIC_ROW_AMP,   // click amplitude; TONE (timbre) is hidden for now, see menu.c
    MENU_HAPTIC_ROW_PITCH,
    MENU_HAPTIC_ROW_COUNT,
};

// APP: an application profile (src/app_profiles/) -- F1-F4 become app controls and
// long-press F4 opens the menu. The enum values are what NVS stores, so never reorder them;
// the order people see (APP first) is MENU_HID_ORDER below.
// HOME: the knob as a remote for the lamps on the network (home.h); F1-F3 are its keys, F4
// opens the menu.
typedef enum { MENU_HID_KEYBOARD = 0, MENU_HID_MOUSE, MENU_HID_MIDI, MENU_HID_APP, MENU_HID_HOME, MENU_HID_TYPE_COUNT } menu_hid_type_t;

// Display / rotation order of the HID types. Header-inline so the screens (and the host UI
// preview, which doesn't link menu.c) can use it.
static const menu_hid_type_t MENU_HID_ORDER[MENU_HID_TYPE_COUNT] = {
    MENU_HID_APP, MENU_HID_HOME, MENU_HID_KEYBOARD, MENU_HID_MOUSE, MENU_HID_MIDI,
};
static inline int menu_hid_type_pos(menu_hid_type_t t) {
    for (int i = 0; i < MENU_HID_TYPE_COUNT; i++) {
        if (MENU_HID_ORDER[i] == t) return i;
    }
    return 0;
}
static inline menu_hid_type_t menu_hid_type_at(int pos) {
    pos %= MENU_HID_TYPE_COUNT;
    if (pos < 0) pos += MENU_HID_TYPE_COUNT;
    return MENU_HID_ORDER[pos];
}

// One rendered row. `caption` is the small engineering name shown under the friendly label
// (e.g. label "SNAP", caption "KP"); "" where there is none. `value` is "" for a submenu item
// that has nothing to show. Disabled items (MIDI channel when HID type != MIDI) are never
// included here at all -- skipped during navigation -- so the renderer never needs a
// disabled/greyed style.
typedef struct {
    char label[MENU_LABEL_TEXT_LEN];
    char caption[MENU_CAPTION_TEXT_LEN];
    char value[MENU_VALUE_TEXT_LEN];
    bool selected;
    bool muted; // not usable in the current feel: value is "--", the knob skips it
} menu_render_row_t;

typedef struct {
    bool open;    // false = menu closed entirely; display_task.cpp shows the Main Screen instead
    bool editing; // true = the selected row's value is being live-adjusted by knob rotation
    bool dirty;   // the current screen has changes F2 hasn't saved yet
    menu_screen_id_t screen;
    int selected;        // index into rows[] of the selected row, -1 if none
    uint32_t save_count; // bumps on every successful F2 save -- the display's SAVED! cue
    uint32_t reset_count; // bumps when a haptic profile goes back to factory -- the FACTORY cue
    char title[MENU_TITLE_LEN]; // "" at the top-level screen (no title row there)
    int row_count;
    menu_render_row_t rows[MENU_MAX_VISIBLE_ITEMS];
} menu_render_snapshot_t;

void menu_init(void);

// Producer side (Core 0) -- call on each button's press edge / each detent-crossing tick.
//
// Two kinds of settings screen:
//   - Haptic (Orbit): turn moves focus; F1 enters edit, turn changes the value live, F1
//     confirms; F3 cancels the edit and restores the value from before it. Unsaved changes
//     stay live after leaving the screen (tune by feel, save when it's right).
//   - HID / Boot mode ("direct"): turn changes the focused value immediately; F1 moves focus
//     to the next field (HID type <-> MIDI channel). Leaving with F3 or F4 discards unsaved
//     changes -- these aren't live-tunable, only a saved choice means anything.
void menu_input_toggle_open(void);        // F4: closed->open, or open at any depth->closed
void menu_input_back(void);               // F3: cancel edit, else back one level, else close
void menu_input_select(void);             // F1: enter submenu / enter or confirm edit / next field
void menu_input_save(void);               // F2: save the current settings screen (if changed)
void menu_input_reset_haptic(void);       // F2 held 1.5 s on Haptics: the shown profile back to factory
void menu_input_rotate(int8_t direction); // knob tick, +1/-1: navigate list, or adjust value while editing

bool menu_is_open(void);
menu_screen_id_t menu_current_screen(void); // MENU_SCREEN_NONE when closed; cheap, any core

// DEVICE -> RECALIBRATE confirmed: true once, then false again. control_task.c polls it and
// does the work (motor off, forget the calibration, restart -- the next boot recalibrates).
bool menu_take_recalibrate_request(void);

// True when turning `direction` would push a non-wrapping value past its end (the PROFILE
// list): control_task.c makes that detent a haptic wall instead of a step. Core 0.
bool menu_at_end(int8_t direction);

// Consumer side (Core 1). Thread-safe full-struct copy.
void menu_get_render_snapshot(menu_render_snapshot_t *out);

// Phase 8 step 3: live haptic settings, adjustable via the Haptic Configurator screen and
// read directly by control_task.c's real-time haptic loop -- replaces the retired
// button-combo live-tuning path. Safe to call from Core 0's real-time loop: each is a single
// atomic load, no lock, same convention as the rest of this file. num_detents is always in
// [HAPTIC_NUM_DETENTS_MIN, HAPTIC_NUM_DETENTS_MAX] (haptic_params.h) -- never 0.
// Since haptic profiles (haptic_params.h) these describe the ACTIVE profile: the control loop
// names it every tick with menu_haptic_set_active() -- menu_haptic_profile() (the one the
// Haptics screen shows while the menu is open, else the HID type's), or an app input's own.
int menu_haptic_profile(void);
void menu_haptic_set_active(int profile);
int menu_haptic_edit_profile(void); // the profile the Haptics screen shows; any core
// An app input's (feel, detents) as a haptic profile: VISCOSE -> SMOOTH, a count -> the
// nearest stepped profile, neither -> -1 (the mode's own). Core 0, every tick in APP mode.
int menu_haptic_for(haptic_type_t feel, unsigned detents);
uint32_t menu_get_haptic_num_detents(void);
float menu_get_haptic_kp(void);
float menu_get_haptic_kd(void);
float menu_get_haptic_shape(void); // 0..0.9, the Haptics SHAPE setting
haptic_type_t menu_get_haptic_type(void);

// Phase 8 step 5: live click timbre, adjustable via the Haptic Configurator's "Haptic Sound"
// field and read directly by i2s_task.c (Core 1) when a new detent click starts. Same
// lock-free atomic-load convention as the getters above, just consumed by a different task.
audio_click_timbre_t menu_get_haptic_sound(void);
float menu_get_haptic_pitch(void);
float menu_get_click_amplitude(void); // 0..1, the Haptics AMP setting

// Phase 8 step 6: live boot USB mode, read once by main.c at startup (before any task
// starts, so no cross-core-timing concern) -- loaded from NVS in menu_init() same as
// everything else above, combined there with the BTN_C+BTN_D hold failsafe (which always
// takes priority regardless of this saved setting).
boot_usb_mode_t menu_get_boot_mode(void);

// Pixel UI: read by display_task.cpp for the Main Screen mode icon and the HID carousel.
menu_hid_type_t menu_get_hid_type(void);
// APP mode's profile: an index into app_profiles_get() (app_profiles/app_profiles.h).
int32_t menu_get_app_profile(void);
// The app profile at `index` was removed and the ones after it moved up one: keeps the live,
// saved and undo choices on the same profiles (the removed one falls back to the first).
void menu_profile_removed(int index);
// Screen rotation, 0-3 quarter turns (live while the DISPLAY screen is being turned).
int32_t menu_get_display_rotation(void);
// DEVICE -> BINDINGS (live while the screen is being turned, like rotation). Any core.
menu_host_t menu_get_host(void);

// --- Companion app (host_link.c, Core 1) ---
// The same settings the menu edits, set from the desktop app: a set is live at once (clamped
// exactly like turning the knob), and shows as unsaved on the device's own screens until
// saved -- from the app or with F2.
typedef struct {
    uint16_t dirty; // 1 << HOST_SET_* (host_proto.h) for each value that differs from NVS
    int32_t detents;
    float kp, kd;
    int32_t feel, amp;
    float pitch;
    int32_t sound, hid_type, midi_channel, profile, boot_mode, rotation, host;
    int32_t shape;
    // The haptic values above are those of `haptic_profile` (the one the Haptics screen
    // shows) in its feel; these are its limits there, and the feels it allows.
    int32_t haptic_profile, feels, amp_max, mode_haptic;
    float kp_min, kp_max, kd_min, kd_max, pitch_min, pitch_max;
} menu_remote_settings_t;

void menu_remote_get(menu_remote_settings_t *out);
bool menu_remote_set(int id, int32_t ival, float fval); // false: unknown id
void menu_remote_save(void);   // NVS for every group that differs (a few ms of flash writes)
void menu_remote_reset_haptic(void); // the shown haptic profile back to factory (live, unsaved)
void menu_remote_revert(void); // every group back to what NVS holds
// LIGHTS alone (ext_link.c): whether the live look differs from NVS, and saving just that.
bool menu_lights_dirty(void);
void menu_remote_save_lights(void);
