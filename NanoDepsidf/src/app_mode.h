#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "haptic_params.h"
#include "app_profiles/app_profile.h"

// APP mode (HID type "APP"): the knob and F1-F4 drive an application, as described by the
// active app profile (src/app_profiles/, chosen in the menu's PROFILE row). This file is the
// engine; it knows nothing about any particular app.
//
// Slots (app_profile.h): turning with no key held = the KNOB slot; holding F1/F2/F4 and
// turning = that key's slot; a TAP slot fires on press. Holding F4 ~0.7s without turning
// opens the menu in every profile. Output only starts once the knob actually moves, so a
// held key that never turns sends nothing -- that's what lets the long press mean "menu".
// The knob-alone slot starts by itself on the first movement and lets go once the knob
// rests (250ms).
//
// State, not events: the control task (Core 0) only records what the host *should* see --
// held mouse buttons + modifier, pending pointer travel and wheel steps, queued key taps --
// and usb_task.c keeps sending until the host matches. A report lost to a busy endpoint
// therefore can't leave a button or modifier stuck down (the first hardware test did exactly
// that with queued edge events). Buttons are debounced here (raw GPIO bounce fired several
// actions per press).

// One queued key tap. `wait_ticks` = pause after it (10ms ticks) -- a command-search macro
// waits for the search box to open and for results to appear.
typedef struct {
    uint8_t modifier;
    uint8_t keycode;  // HID_KEY_*, or a Consumer usage when `consumer` is set
    uint8_t wait_ticks;
    uint8_t consumer; // 1 = keycode is an 8-bit HID Consumer usage (APP_ACT_MEDIA)
} app_tap_t;

// --- control task side (Core 0) ---

// Every control tick. `active` = APP mode selected and the menu closed; when false this just
// lets go of anything held and tracks the buttons, so no stale press fires on return.
// `held` is the UI_BTN_* mask.
void app_mode_update(bool active, int64_t now_us, uint8_t held);

// Every control tick with the knob's shaft-angle change since the last tick (radians).
void app_mode_motion(float delta_rad, int64_t now_us);

// Every detent crossing while APP mode is active, +1 / -1.
void app_mode_detent(int8_t dir, int64_t now_us);

// The haptic profile (haptic_params.h) of what's live: parameter mode's step, or the live
// input's. Leaves *profile alone if the input has no action or names none.
void app_mode_haptics(int *profile);

// True when a detent in `dir` would run off the end of a list (the command wheel): the
// control loop turns that detent into a haptic wall instead of a step.
bool app_mode_at_end(int8_t dir);

// --- display side ---

// The slot whose action is live right now (APP_SLOT_KNOB when no key is held).
int app_mode_live_slot(void);
// Command wheel: true while open; *ring = index into the profile's rings, *entry 0 = cancel,
// n = the ring's command n-1.
bool app_mode_wheel(int *ring, int *entry);

// Volume keys sent so far, +1 per Volume Increment, -1 per Decrement (any core): those under
// Shift+Option (`fine`) and the rest (`plain`).
void app_mode_volume_steps(int32_t *fine, int32_t *plain);
// Commands run so far (a counter that moves on every run) and which one ran last.
uint32_t app_mode_last_run(int *ring, int *entry);
// Slot taps fired so far (a counter) and which slot fired last -- a key's quick-press `tap`.
uint32_t app_mode_last_tap(int *slot);

// Parameter mode (app_profile.h): after a command with a `param` runs from the wheel.
typedef struct {
    bool active;
    int ring, entry;       // the command
    float value;
    int step;              // -1 free, 0-2 the held F key's step
    int axis;              // 0-2 X / Y / Z (with APP_PARAM_AXES)
    bool plane, uniform;
    bool exact;            // a step was used: the value will be typed in on confirm
    bool field;            // number-field input: step = F1 / alone / F4
    bool typed;            // ...B (type): `value` is the value; A (scroll): value - start = the change
    uint32_t f3_ms;        // F3 held this long (0 = up): past 600ms, release cancels
    uint32_t bump;         // counts end-stop hits (the card nudges)
} app_param_state_t;
bool app_mode_param(app_param_state_t *out);

// --- USB task side ---

// What the host should see held: MOUSE_BUTTON_* mask, KEYBOARD_MODIFIER_* mask, and
// whether pointer travel goes on the y axis instead of x.
void app_mode_wanted(uint8_t *buttons, uint8_t *modifier, bool *axis_y);
int32_t app_mode_take_move_px(void);      // pointer travel accumulated since the last call
void app_mode_return_move_px(int32_t px); // give back what didn't fit in one report
int32_t app_mode_take_wheel_steps(void);  // wheel steps accumulated since the last call
void app_mode_return_wheel_steps(int32_t steps);
bool app_mode_take_tap(app_tap_t *key);   // next queued key tap, if any
bool app_mode_hover(void);                 // pointer travel counts with no button held (parameter mode)
