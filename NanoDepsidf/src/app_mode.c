#include "app_mode.h"
#include "tasks_common.h"
#include "app_profiles/app_profiles.h"
#include "menu.h"
#include "ui_state.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "class/hid/hid.h"

#define APP_DRAG_START_PX 3.0f       // movement before a drag engages (ignores a wobble while
                                     // long-pressing F4 for the menu)
#define APP_MOTION_EPS_RAD 0.004f    // travel that counts as "the knob moved" (above sensor noise)
#define APP_IDLE_RELEASE_US 250000   // knob-alone slot: let go once the knob rests this long
#define APP_MENU_HOLD_US 700000      // long-press F4 -> menu
#define APP_DEBOUNCE_US 15000        // a button change counts once it has been stable this long
#define APP_TAP_RING 64              // queued key taps (a full ring drops taps, never releases);
                                     // sized for one command-search macro (open + ~40 chars + Enter)
#define APP_TAP_MAX_US 400000        // a key let go within this, without turning, was a tap
#define APP_CANCEL 0                 // wheel entry 0 of every ring: close without running
#define APP_WHEEL_SHOW_US 250000     // a wheel key that also taps (Plasticity F3 = undo) shows the
                                     // wheel only after this -- a quick tap never flashes it
#define APP_PARAM_CANCEL_US 600000   // parameter mode: F3 held this long = cancel, shorter = confirm
#define APP_PARAM_AXIS_TAP_US 350000 // parameter mode: F1 / F2 / F4 let go within this = axis tap
#define APP_PARAM_TAP_TRAVEL 0.05f   // ...unless the knob turned more than this (rad) meanwhile
#define APP_PARAM_TYPE_PAUSE_US 350000 // number field, B (type): retype once the knob rests this long
#define WANT_HOVER (1u << 17)        // s_want: pointer travel with no button held

static const uint8_t SLOT_KEY[APP_SLOT_COUNT] = {0, UI_BTN_F1, UI_BTN_F2, UI_BTN_F3, UI_BTN_F4};

// Control-task-only state.
static const app_profile_t *s_profile = NULL;
static bool s_active = false;
static uint8_t s_raw_held = 0, s_stable_held = 0, s_prev_held = 0;
static int64_t s_raw_since_us = 0;
static int s_slot = -1;             // live slot, -1 = none
static uint8_t s_slot_key = 0;      // UI_BTN_* that owns it (0 = knob alone)
static bool s_engaged = false;      // the slot has sent something (button/modifier down, a step)
static float s_accum_px = 0.0f;     // drag travel not yet published
static float s_motion_accum = 0.0f; // travel since the last "moved" mark
static int64_t s_slot_start_us = 0;
static int64_t s_last_motion_us = 0;

// Shared with the USB task. The held state is one packed word so buttons, modifier and axis
// always change together: bits 0-7 mouse buttons, 8-15 keyboard modifier, bit 16 = y axis.
static _Atomic uint32_t s_want = 0;
static _Atomic int32_t s_move_px = 0;
static _Atomic int32_t s_wheel_steps = 0;
static app_tap_t s_taps[APP_TAP_RING];
static _Atomic uint32_t s_tap_head = 0, s_tap_tail = 0; // single producer (Core 0), single consumer (USB)
// Shared with the display task.
static _Atomic int s_live_slot = APP_SLOT_KNOB;
// Command wheel: bit 31 open, bits 8-15 ring, 0-7 entry (0 = cancel). Runs: a counter plus
// which command, so the display can echo it.
static _Atomic uint32_t s_wheel = 0;
static _Atomic uint32_t s_run = 0; // bits 16-31 count, 8-15 ring, 0-7 entry

// Command wheel, control-task side.
static bool s_wheel_open = false;
static bool s_wheel_shown = false;  // published to the display (delayed when the key also taps)
static _Atomic uint32_t s_tapped = 0; // bits 8-31 count, 0-7 slot: a slot's `tap` just fired
static uint8_t s_ring = 0, s_entry = 1;
static uint8_t s_ring_entry[8];     // last entry used per ring (opens there: hold-release repeats)
static uint8_t s_swallow_keys = 0;  // F keys used to switch rings: their release does nothing

// Parameter mode, control-task side (see app_profile.h "Parameter mode").
static const app_param_t *s_param = NULL;
static float s_pvalue = 0.0f;
static bool s_pexact = false;        // a step was used: the value is typed in on confirm
static uint8_t s_paxis = 0;          // 0-2 X / Y / Z
static bool s_pplane = false, s_puniform = false;
static int64_t s_pf3_at = -1;        // F3 down since (-1 = up)
static int64_t s_pkey_at[APP_SLOT_COUNT];
static float s_pkey_travel[APP_SLOT_COUNT];
static float s_paccum_px = 0.0f;
static uint8_t s_pring = 0, s_pentry = 0;
static uint8_t s_paxis_memo[8];      // last constraint per visual: bit 7 set, axis | plane << 2 | uniform << 3
// Number-field input (param_keys.field): B = type instead of scroll (F2 tap, kept across commands).
static bool s_ptype = false;
static bool s_ptype_dirty = false;   // B: the value changed since it was last typed
static int64_t s_pstep_at = 0;       // B: last click
// Published: bit 31 active, 24 number field, 25 typed (B), 16-23 ring, 8-15 entry, 0-1 axis,
// 2 plane, 3 uniform, 4-5 step + 1, 6 exact. Plus the value (x1000), F3 held time and an
// end-stop bump counter.
static _Atomic uint32_t s_pword = 0;
static _Atomic int32_t s_pvalue_milli = 0;
static _Atomic uint32_t s_pf3_ms = 0;
static _Atomic uint32_t s_pbump = 0;

#define WANT(buttons, modifier, axis_y) ((uint32_t)(buttons) | ((uint32_t)(modifier) << 8) | ((axis_y) ? 1u << 16 : 0u))

static const app_action_t *CONTROL_HOT action(int slot) {
    return &s_profile->slot[slot];
}

static void publish_run(uint8_t ring, uint8_t entry) {
    uint32_t n = (atomic_load(&s_run) >> 16) + 1;
    atomic_store(&s_run, (n << 16) | ((uint32_t)ring << 8) | entry);
}

static uint32_t CONTROL_HOT tap_room(void) {
    return APP_TAP_RING - (atomic_load(&s_tap_head) - atomic_load(&s_tap_tail));
}

static void CONTROL_HOT push_tap_wait(app_key_t key, uint8_t wait_ticks) {
    uint32_t head = atomic_load(&s_tap_head);
    if (head - atomic_load(&s_tap_tail) >= APP_TAP_RING) return; // full: drop this tap
    s_taps[head % APP_TAP_RING] = (app_tap_t){key.modifier, key.keycode, wait_ticks, 0};
    atomic_store(&s_tap_head, head + 1);
}

static void CONTROL_HOT push_tap(app_key_t key) {
    push_tap_wait(key, 0);
}

// Volume keys sent, +1 per Volume Increment, -1 per Decrement: under Shift+Option (a quarter
// step on a Mac) and plain (media.c runs the ring ahead of the host's report on these).
#define VOLUME_FINE (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_LEFTALT)
static _Atomic int32_t s_volume_fine = 0, s_volume_plain = 0;

void app_mode_volume_steps(int32_t *fine, int32_t *plain) {
    *fine = atomic_load(&s_volume_fine);
    *plain = atomic_load(&s_volume_plain);
}

// APP_ACT_MEDIA: one Consumer-page usage, under the key's modifier (press + release sent by
// the usb task).
static void CONTROL_HOT push_media(app_key_t key) {
    uint32_t head = atomic_load(&s_tap_head);
    if (head - atomic_load(&s_tap_tail) >= APP_TAP_RING) return; // full: drop this tap
    s_taps[head % APP_TAP_RING] = (app_tap_t){key.modifier, key.keycode, 0, 1};
    atomic_store(&s_tap_head, head + 1);
    int step = key.keycode == HID_USAGE_CONSUMER_VOLUME_INCREMENT ? 1 : key.keycode == HID_USAGE_CONSUMER_VOLUME_DECREMENT ? -1 : 0;
    if (step) atomic_fetch_add((key.modifier & VOLUME_FINE) == VOLUME_FINE ? &s_volume_fine : &s_volume_plain, step);
}

// US key positions for printable ASCII (command-search phrases, typed values, macro text).
// Punctuation: the unshifted character's key, and the shifted one's (e.g. '-' / '_').
static const char US_PLAIN[] = "-=[]\\;',./`";
static const char US_SHIFTED[] = "_+{}|:\"<>?~";
static const uint8_t US_PUNCT_KEY[] = {HID_KEY_MINUS, HID_KEY_EQUAL, HID_KEY_BRACKET_LEFT, HID_KEY_BRACKET_RIGHT,
                                       HID_KEY_BACKSLASH, HID_KEY_SEMICOLON, HID_KEY_APOSTROPHE, HID_KEY_COMMA,
                                       HID_KEY_PERIOD, HID_KEY_SLASH, HID_KEY_GRAVE};
static const char US_DIGIT_SHIFTED[] = ")!@#$%^&*("; // Shift + 0..9

static bool ascii_key(char c, app_key_t *k) {
    k->modifier = 0;
    if (c >= 'a' && c <= 'z') k->keycode = HID_KEY_A + (c - 'a');
    else if (c >= 'A' && c <= 'Z') { k->keycode = HID_KEY_A + (c - 'A'); k->modifier = KEYBOARD_MODIFIER_LEFTSHIFT; }
    else if (c >= '1' && c <= '9') k->keycode = HID_KEY_1 + (c - '1');
    else if (c == '0') k->keycode = HID_KEY_0;
    else if (c == ' ') k->keycode = HID_KEY_SPACE;
    else {
        const char *p;
        if (c != '\0' && (p = strchr(US_PLAIN, c)) != NULL) {
            k->keycode = US_PUNCT_KEY[p - US_PLAIN];
        } else if (c != '\0' && (p = strchr(US_SHIFTED, c)) != NULL) {
            k->keycode = US_PUNCT_KEY[p - US_SHIFTED];
            k->modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
        } else if (c != '\0' && (p = strchr(US_DIGIT_SHIFTED, c)) != NULL) {
            int d = (int)(p - US_DIGIT_SHIFTED);
            k->keycode = d == 0 ? HID_KEY_0 : HID_KEY_1 + (d - 1);
            k->modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
        } else {
            return false;
        }
    }
    return true;
}

// --- Macros (app_profile.h) ---
// Fed into the tap ring a little each tick, as room allows, so a macro can be longer than the
// ring. Presses that come while one runs queue up (a few); a profile change or leaving APP
// mode stops them. Control-task only.
#define MACRO_QUEUE 4
static const app_macro_t *s_mrun = NULL;
static uint8_t s_mstep = 0;
static uint16_t s_mpos = 0;      // TEXT: next character; WAIT: ticks already queued
static uint8_t s_mqueue[MACRO_QUEUE], s_mqueued = 0;

static void macro_stop(void) {
    s_mrun = NULL;
    s_mqueued = 0;
}

static void macro_begin(uint8_t ref) {
    s_mrun = &s_profile->macros[ref - 1];
    s_mstep = 0;
    s_mpos = 0;
}

// Runs the profile's macro `ref` (1-based) now, or after the one that's running.
static void macro_start(uint8_t ref) {
    if (s_profile == NULL || ref == 0 || ref > s_profile->macro_count) return;
    if (s_mrun == NULL) macro_begin(ref);
    else if (s_mqueued < MACRO_QUEUE) s_mqueue[s_mqueued++] = ref;
}

static void CONTROL_HOT macro_pump(void) {
    while (s_mrun != NULL && tap_room() > 0) {
        if (s_mstep >= s_mrun->count) {
            s_mrun = NULL;
            if (s_mqueued) {
                uint8_t next = s_mqueue[0];
                memmove(s_mqueue, s_mqueue + 1, --s_mqueued);
                macro_begin(next);
            }
            continue;
        }
        const app_mstep_t *st = &s_mrun->steps[s_mstep];
        bool done = true;
        if (st->kind == APP_MSTEP_KEY) {
            push_tap(st->key);
        } else if (st->kind == APP_MSTEP_TEXT) {
            char c = st->text ? st->text[s_mpos] : '\0';
            if (c != '\0') {
                app_key_t k;
                if (ascii_key(c, &k)) push_tap(k);
                s_mpos++;
                done = false;
            }
        } else {
            // A wait = an empty tap that pauses after itself, 2.55s at most per tap.
            uint32_t ticks = (st->ms + 9u) / 10u;
            if (s_mpos < ticks) {
                uint32_t n = ticks - s_mpos > 255 ? 255 : ticks - s_mpos;
                push_tap_wait((app_key_t){0, 0}, (uint8_t)n);
                s_mpos += (uint16_t)n;
                done = false;
            }
        }
        if (done) {
            s_mstep++;
            s_mpos = 0;
        }
    }
}

// ACTIONS: open the app's command search, type the phrase, wait for the results, Enter. All
// or nothing -- a macro cut short by a full ring would type half a phrase.
static void push_search(const app_search_t *s, const char *phrase) {
    uint32_t need = 2 + (uint32_t)strlen(phrase);
    if (tap_room() < need) return;
    push_tap_wait(s->open, s->open_wait);
    for (const char *c = phrase; *c; c++) {
        app_key_t k;
        if (ascii_key(*c, &k)) push_tap_wait(k, c[1] ? 0 : s->result_wait);
    }
    push_tap((app_key_t){0, HID_KEY_ENTER});
}

static int ring_count(void) {
    int n = s_profile->ring_count;
    return n > (int)sizeof(s_ring_entry) ? (int)sizeof(s_ring_entry) : n;
}

static void CONTROL_HOT publish_wheel(void) {
    atomic_store(&s_wheel, (s_wheel_open && s_wheel_shown ? 1u << 31 : 0u) | ((uint32_t)s_ring << 8) | s_entry);
}

// A key's quick tap: a key or a macro.
static bool has_tap(const app_action_t *a) {
    return a->tap.keycode != 0 || a->tap_macro != 0;
}

// A slot's quick-press `tap` (the display flashes on it, e.g. Plasticity's undo).
static void fire_tap(int slot, const app_action_t *a) {
    if (a->tap_macro) macro_start(a->tap_macro);
    else push_tap(a->tap);
    uint32_t n = (atomic_load(&s_tapped) >> 8) + 1;
    atomic_store(&s_tapped, (n << 8) | (uint32_t)slot);
}

static void wheel_open(const app_action_t *a) {
    if (ring_count() == 0) return;
    if (s_ring >= ring_count()) s_ring = 0;
    s_entry = s_ring_entry[s_ring] ? s_ring_entry[s_ring] : 1;
    s_wheel_open = true;
    s_wheel_shown = !has_tap(a); // a key that also taps waits APP_WHEEL_SHOW_US
    publish_wheel();
}

// --- parameter mode ---

static bool CONTROL_HOT param_field(void) {
    return s_profile->param_keys.field;
}

// B (type) is on and the value is ours to hold; A (scroll) only knows the change.
static bool param_typed(void) {
    return param_field() && s_ptype;
}

// The held F key's step: F4 > F2 > F1, -1 = free. Number field: F1 / alone / F4 (F2 = A/B).
static int CONTROL_HOT param_step(void) {
    uint8_t h = s_stable_held;
    if (s_param && param_field()) return (h & UI_BTN_F4) ? 2 : (h & UI_BTN_F1) ? 0 : 1;
    return (h & UI_BTN_F4) ? 2 : (h & UI_BTN_F2) ? 1 : (h & UI_BTN_F1) ? 0 : -1;
}

// Parameter mode's haptic profile: the finer the step, the finer the profile, so each step
// has its own feel and spacing (one click = one step, always). Free still clicks: a
// continuous value jittered in its last digit with sensor noise (hardware).
//   Plasticity: free FINE, F1 MEDIUM, F2 COARSE, F4 WIDE
//   number field (Onshape): F1 FINE, alone MEDIUM, F4 COARSE
static int CONTROL_HOT param_haptic(void) {
    int rank = param_field() ? param_step() : param_step() + 1; // 0 = the finest step
    return HAPTIC_PROFILE_FINE - rank; // FINE, MEDIUM, COARSE, WIDE are in id order, coarse first
}

static void CONTROL_HOT param_publish(void) {
    uint32_t w = 0;
    if (s_param) {
        w = 1u << 31 | (uint32_t)s_pring << 16 | (uint32_t)s_pentry << 8 | s_paxis | (s_pplane ? 4u : 0u)
          | (s_puniform ? 8u : 0u) | (uint32_t)(param_step() + 1) << 4 | (s_pexact ? 64u : 0u)
          | (param_field() ? 1u << 24 : 0u) | (param_typed() ? 1u << 25 : 0u);
    }
    atomic_store(&s_pvalue_milli, (int32_t)lroundf(s_pvalue * 1000.0f));
    atomic_store(&s_pword, w);
}

// The constraint key for the current axis state (X, Shift + X for its plane, S uniform).
static app_key_t param_constraint_key(void) {
    const app_param_keys_t *k = &s_profile->param_keys;
    if (s_puniform) return k->uniform;
    app_key_t key = k->axis[s_paxis];
    if (s_pplane) key.modifier |= KEYBOARD_MODIFIER_LEFTSHIFT;
    return key;
}

static void CONTROL_HOT param_set(float v) {
    if (param_field() && !s_ptype) { // A: the field's real value is unknown -- no limits
        s_pvalue = v;
        return;
    }
    if (v < s_param->min || v > s_param->max) atomic_fetch_add(&s_pbump, 1);
    s_pvalue = v < s_param->min ? s_param->min : v > s_param->max ? s_param->max : v;
    if (fabsf(s_pvalue) < 1e-6f) s_pvalue = 0.0f;
}

static void param_start(const app_param_t *p, uint8_t ring, uint8_t entry) {
    s_param = p;
    s_pvalue = p->start;
    s_pexact = false;
    s_pf3_at = -1;
    s_paccum_px = 0.0f;
    s_pring = ring;
    s_pentry = entry;
    memset(s_pkey_at, 0, sizeof(s_pkey_at));
    memset(s_pkey_travel, 0, sizeof(s_pkey_travel));
    if (p->enter.keycode) push_tap(p->enter);
    if (p->flags & APP_PARAM_AXES) {
        uint8_t m = s_paxis_memo[p->visual & 7];
        if (m & 0x80) {
            s_paxis = m & 3;
            s_pplane = m & 4;
            s_puniform = m & 8;
        } else {
            s_paxis = p->axis_default == APP_AXIS_UNIFORM ? 0 : p->axis_default;
            s_pplane = false;
            s_puniform = p->axis_default == APP_AXIS_UNIFORM;
        }
        push_tap(param_constraint_key());
    }
    s_ptype_dirty = false;
    // Pointer: the knob alone moves it and the app's handle follows. Number field: nothing
    // held until a step needs its modifier (param_keys()).
    atomic_store(&s_want, param_field() ? 0 : WANT_HOVER);
    param_publish();
}

// B: select the field's text and type the value over it.
static void param_type_value(void) {
    const app_param_keys_t *k = &s_profile->param_keys;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.*f", s_param->decimals, (double)s_pvalue);
    if (tap_room() < strlen(buf) + 1) return; // retried on the next tick
    push_tap_wait(k->select_all, 2);
    for (const char *c = buf; *c; c++) {
        app_key_t key;
        if (ascii_key(*c, &key)) push_tap(key);
    }
    s_ptype_dirty = false;
}

// Tap F1 / F2 / F4 = X / Y / Z; the active one again = its plane, then (scale) uniform.
static void param_axis_tap(uint8_t axis) {
    uint8_t f = s_param->flags;
    if (s_puniform || s_paxis != axis) {
        s_paxis = axis;
        s_pplane = s_puniform = false;
    } else if (!s_pplane && (f & APP_PARAM_PLANES)) {
        s_pplane = true;
    } else if (f & APP_PARAM_UNIFORM) {
        s_pplane = false;
        s_puniform = true;
    } else {
        s_pplane = false;
    }
    s_paxis_memo[s_param->visual & 7] = 0x80 | s_paxis | (s_pplane ? 4 : 0) | (s_puniform ? 8 : 0);
    push_tap(param_constraint_key());
    param_publish();
}

// Confirm: an exact value is typed in (numeric entry, the digits, confirm); a free one is just
// confirmed where the handle is. Cancel: the cancel key.
static void param_end(bool ok) {
    if (s_param == NULL) return;
    const app_param_keys_t *k = &s_profile->param_keys;
    if (ok && param_typed() && s_ptype_dirty) param_type_value();
    if (ok && s_pexact && k->numeric.keycode) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.*f", s_param->decimals, (double)s_pvalue);
        if (tap_room() >= strlen(buf) + 3) {
            push_tap_wait(k->numeric, 5);
            for (const char *c = buf; *c; c++) {
                app_key_t key;
                if (ascii_key(*c, &key)) push_tap(key);
            }
        }
    }
    push_tap(ok ? k->confirm : k->cancel);
    if (ok) publish_run(s_pring, s_pentry);
    s_param = NULL;
    atomic_store(&s_want, 0);
    atomic_store(&s_move_px, 0);
    atomic_store(&s_pf3_ms, 0);
    param_publish();
}

// Every tick while parameter mode is on: it owns F1-F4 (no slots, no menu long-press).
static void param_keys(int64_t now_us, uint8_t pressed, uint8_t released) {
    if (pressed & UI_BTN_F3) s_pf3_at = now_us;
    if ((released & UI_BTN_F3) && s_pf3_at >= 0) {
        bool ok = now_us - s_pf3_at < APP_PARAM_CANCEL_US;
        s_pf3_at = -1;
        param_end(ok);
        return;
    }
    atomic_store(&s_pf3_ms, s_pf3_at >= 0 ? (uint32_t)((now_us - s_pf3_at) / 1000) + 1 : 0);
    static const uint8_t AXIS_SLOT[3] = {APP_SLOT_F1, APP_SLOT_F2, APP_SLOT_F4};
    for (uint8_t a = 0; a < 3; a++) {
        int slot = AXIS_SLOT[a];
        uint8_t key = SLOT_KEY[slot];
        if (pressed & key) {
            s_pkey_at[slot] = now_us;
            s_pkey_travel[slot] = 0.0f;
        }
        bool tapped = (released & key) && s_pkey_travel[slot] < APP_PARAM_TAP_TRAVEL
                   && now_us - s_pkey_at[slot] < APP_PARAM_AXIS_TAP_US;
        if (tapped && param_field() && slot == APP_SLOT_F2) {
            s_ptype = !s_ptype; // A <-> B
            s_ptype_dirty = false;
        } else if (tapped && (s_param->flags & APP_PARAM_AXES)) {
            param_axis_tap(a);
        }
    }
    if (param_field()) {
        // A: hold the step's modifier so the wheel notches land as that step. B: nothing held;
        // retype once the knob rests.
        uint8_t mod = s_ptype ? 0 : s_profile->param_keys.step_mod[param_step()];
        atomic_store(&s_want, WANT(0, mod, false));
        if (s_ptype && s_ptype_dirty && now_us - s_pstep_at >= APP_PARAM_TYPE_PAUSE_US) param_type_value();
    }
    param_publish();
}

// F3 let go: run the chosen command (entry 0 = cancel).
static void wheel_close(void) {
    if (!s_wheel_open) return;
    s_wheel_open = false;
    const app_ring_t *r = &s_profile->rings[s_ring];
    if (s_entry != APP_CANCEL && s_entry <= r->count) {
        const app_cmd_t *c = &r->cmds[s_entry - 1];
        if (c->kind == APP_CMD_ACTIONS && c->phrase) push_search(&s_profile->search, c->phrase);
        else if (c->kind == APP_CMD_MACRO) macro_start(c->macro);
        else push_tap(c->key);
        s_ring_entry[s_ring] = s_entry;
        if (c->param) param_start(c->param, s_ring, s_entry); // the echo comes on confirm
        else publish_run(s_ring, s_entry);
    }
    publish_wheel();
}

static void CONTROL_HOT end_slot(void) {
    s_wheel_open = false;
    publish_wheel();
    atomic_store(&s_want, 0);
    atomic_store(&s_move_px, 0);
    atomic_store(&s_wheel_steps, 0);
    s_slot = -1;
    s_slot_key = 0;
    s_engaged = false;
    s_accum_px = 0.0f;
}

static void CONTROL_HOT begin_slot(int slot, int64_t now_us) {
    if (s_slot >= 0) end_slot();
    s_slot = slot;
    s_slot_key = SLOT_KEY[slot];
    s_engaged = false;
    s_accum_px = 0.0f;
    s_slot_start_us = now_us;
    s_last_motion_us = now_us;
}

void CONTROL_HOT app_mode_update(bool active, int64_t now_us, uint8_t held) {
    if (held != s_raw_held) {
        s_raw_held = held;
        s_raw_since_us = now_us;
    }
    if (s_raw_held != s_stable_held && now_us - s_raw_since_us >= APP_DEBOUNCE_US) {
        s_stable_held = s_raw_held;
    }
    held = s_stable_held;
    uint8_t pressed = held & ~s_prev_held;
    uint8_t released = s_prev_held & ~held;
    s_prev_held = held;

    const app_profile_t *p = app_profiles_get(menu_get_app_profile());
    if (p != s_profile) {
        if (s_param) param_end(false);
        end_slot();
        macro_stop(); // it points into the old profile
        s_profile = p;
    }
    s_active = active;
    s_swallow_keys &= held; // forget keys once they're up
    if (active) macro_pump();
    else macro_stop();
    if (!active) {
        if (s_param) param_end(false); // the menu opened / mode changed: leave the app's command
        if (s_slot >= 0 || atomic_load(&s_want)) end_slot();
        atomic_store(&s_live_slot, APP_SLOT_KNOB);
        return;
    }
    if (s_param) {
        param_keys(now_us, pressed, released);
        atomic_store(&s_live_slot, APP_SLOT_KNOB);
        return;
    }

    // Command wheel open: the other F keys jump between rings and do nothing else.
    if (s_wheel_open) {
        for (int slot = APP_SLOT_F1; slot < APP_SLOT_COUNT; slot++) {
            uint8_t k = SLOT_KEY[slot];
            if (!(pressed & k) || k == s_slot_key) continue;
            // The next ring bound to this key after the current one (wrapping), so a key
            // that owns several rings steps through them.
            int n = ring_count();
            for (int step = 1; step <= n; step++) {
                int i = (s_ring + step) % n;
                if (s_profile->rings[i].slot != slot) continue;
                s_ring = (uint8_t)i;
                s_entry = s_ring_entry[i] ? s_ring_entry[i] : 1;
                s_engaged = true; // using the wheel: its key's release is no longer a tap
                s_wheel_shown = true;
                publish_wheel();
                break;
            }
        }
        s_swallow_keys |= pressed & ~s_slot_key;
        pressed &= s_slot_key;
    }
    pressed &= ~s_swallow_keys;

    for (int slot = APP_SLOT_F1; slot < APP_SLOT_COUNT; slot++) {
        if (!(pressed & SLOT_KEY[slot])) continue;
        const app_action_t *a = action(slot);
        if (a->kind == APP_ACT_TAP && slot != APP_SLOT_F4) {
            if (a->macro) macro_start(a->macro);
            else push_tap(a->cw);
        } else if (a->kind == APP_ACT_MEDIA && slot != APP_SLOT_F4) {
            push_media(a->cw); // fires on press, like TAP
        } else if (s_slot < 0 || s_slot_key == 0) {
            // A key takes over from the knob-alone slot. F4 always starts a slot (even with
            // no action) so its long press can open the menu; so does a key with only a
            // quick tap, so its release can fire it.
            if (a->kind != APP_ACT_NONE || slot == APP_SLOT_F4 || has_tap(a)) begin_slot(slot, now_us);
            if (a->kind == APP_ACT_COMMANDS && slot != APP_SLOT_F4) wheel_open(a);
        }
    }
    if (s_slot_key != 0 && !(held & s_slot_key)) {
        // Let go: the wheel runs its command; a quick press that never turned is a tap.
        const app_action_t *a = action(s_slot);
        bool tap = has_tap(a) && !s_engaged && now_us - s_slot_start_us < APP_TAP_MAX_US;
        if (s_wheel_open && !s_wheel_shown) tap = has_tap(a); // released before the wheel showed
        else if (s_wheel_open) tap = false;
        if (tap) fire_tap(s_slot, a);
        else wheel_close();
        end_slot();
    }
    // The wheel shows once held long enough, or at once when it's being used.
    if (s_wheel_open && !s_wheel_shown && (s_engaged || now_us - s_slot_start_us >= APP_WHEEL_SHOW_US)) {
        s_wheel_shown = true;
        publish_wheel();
    }
    if (s_slot == APP_SLOT_KNOB && now_us - s_last_motion_us >= APP_IDLE_RELEASE_US) {
        end_slot();
    }
    // F4 held with the knob still -> it was a long press for the menu.
    if (s_slot == APP_SLOT_F4 && !s_engaged && now_us - s_slot_start_us >= APP_MENU_HOLD_US) {
        end_slot();
        menu_input_toggle_open();
    }
    atomic_store(&s_live_slot, s_slot >= 0 ? s_slot : APP_SLOT_KNOB);
}

// The knob moved: make sure a slot is live (the knob-alone slot starts itself).
static bool CONTROL_HOT knob_slot_ready(int64_t now_us) {
    if (!s_active || s_profile == NULL) return false;
    if (s_slot < 0) {
        if (s_stable_held != 0) return false; // a key is down but owns no turn action
        if (action(APP_SLOT_KNOB)->kind == APP_ACT_NONE) return false;
        begin_slot(APP_SLOT_KNOB, now_us);
    }
    return true;
}

void CONTROL_HOT app_mode_motion(float delta_rad, int64_t now_us) {
    if (!s_active || s_profile == NULL) return;
    if (s_param) {
        for (int slot = 0; slot < APP_SLOT_COUNT; slot++) {
            if (s_stable_held & SLOT_KEY[slot]) s_pkey_travel[slot] += fabsf(delta_rad);
        }
        return; // value and pointer move per click: app_mode_detent()
    }
    s_motion_accum += delta_rad;
    bool moved = fabsf(s_motion_accum) >= APP_MOTION_EPS_RAD;
    if (moved) s_motion_accum = 0.0f;
    if (s_slot < 0 && !moved) return; // sensor noise never starts the knob-alone slot
    if (!knob_slot_ready(now_us)) return;
    if (moved) s_last_motion_us = now_us;

    const app_action_t *a = action(s_slot);
    if (a->kind != APP_ACT_DRAG) return; // steps come from app_mode_detent()
    s_accum_px += delta_rad * a->px_per_rad * a->sign;
    if (!s_engaged) {
        if (fabsf(s_accum_px) < APP_DRAG_START_PX) return;
        s_engaged = true;
        atomic_store(&s_want, WANT(a->buttons, a->modifier, a->axis_y));
    }
    float px = truncf(s_accum_px);
    if (px != 0.0f) {
        s_accum_px -= px;
        atomic_fetch_add(&s_move_px, (int32_t)px);
    }
}

void CONTROL_HOT app_mode_detent(int8_t dir, int64_t now_us) {
    if (s_param && s_active) {
        int i = param_step();
        if (param_field()) {
            // One click = one step: A a wheel notch (the modifier is already held, see
            // param_keys()), B a new value to retype.
            float st = s_param->steps[i];
            param_set(roundf((s_pvalue + dir * st) * 1000.0f) / 1000.0f);
            if (s_ptype) {
                s_ptype_dirty = true;
                s_pstep_at = now_us;
            } else {
                atomic_store(&s_want, WANT(0, s_profile->param_keys.step_mod[i], false));
                atomic_fetch_add(&s_wheel_steps, dir * s_profile->param_keys.scroll_sign);
            }
            param_publish();
            return;
        }
        if (i < 0) { // free: one fine click = one small increment + a fixed bit of pointer travel
            param_set(s_pvalue + dir * s_param->free_step);
            s_paccum_px += dir * s_param->px_per_step;
            float px = truncf(s_paccum_px);
            if (px != 0.0f) {
                s_paccum_px -= px;
                atomic_fetch_add(&s_move_px, (int32_t)px);
            }
            param_publish();
            return;
        }
        float st = s_param->steps[i];
        s_pexact = true;
        param_set(roundf(s_pvalue / st) * st + dir * st);
        param_publish();
        return;
    }
    if (!knob_slot_ready(now_us)) return;
    s_last_motion_us = now_us;
    const app_action_t *a = action(s_slot);
    switch (a->kind) {
        case APP_ACT_COMMANDS: {
            // One detent per entry; the ends just stop (cancel first, last command last).
            if (!s_wheel_open) break;
            if (!s_wheel_shown) { // the first detent reveals the wheel; it doesn't move yet
                s_engaged = s_wheel_shown = true;
                publish_wheel();
                break;
            }
            int last = s_profile->rings[s_ring].count;
            int e = s_entry + dir;
            if (e >= 0 && e <= last) {
                s_entry = (uint8_t)e;
                publish_wheel();
            }
            s_engaged = true;
            break;
        }
        case APP_ACT_WHEEL:
            if (!s_engaged) {
                s_engaged = true;
                atomic_store(&s_want, WANT(0, a->modifier, false));
            }
            atomic_fetch_add(&s_wheel_steps, dir * a->sign);
            break;
        case APP_ACT_KEYS:
            s_engaged = true;
            push_tap(dir > 0 ? a->cw : a->ccw);
            break;
        case APP_ACT_MEDIA:
            s_engaged = true;
            push_media(dir > 0 ? a->cw : a->ccw);
            break;
        default:
            break; // drags follow app_mode_motion(); taps fire on press
    }
}

uint32_t app_mode_last_tap(int *slot) {
    uint32_t t = atomic_load(&s_tapped);
    *slot = t & 0xFF;
    return t >> 8;
}

bool CONTROL_HOT app_mode_at_end(int8_t dir) {
    if (s_active && s_param) {
        if (param_field() && !s_ptype) return false; // A: the real value is unknown
        return dir > 0 ? s_pvalue >= s_param->max : s_pvalue <= s_param->min;
    }
    if (!s_active || !s_wheel_open || !s_wheel_shown || s_profile == NULL) return false;
    return dir > 0 ? s_entry >= s_profile->rings[s_ring].count : s_entry == 0;
}

void CONTROL_HOT app_mode_haptics(int *profile) {
    if (s_profile == NULL) return;
    if (s_param) {
        *profile = param_haptic();
        return;
    }
    const app_action_t *a = action(s_slot >= 0 ? s_slot : APP_SLOT_KNOB);
    if (a->kind == APP_ACT_NONE || a->kind == APP_ACT_TAP) return;
    // The input's (feel, detents) names a haptic profile: VISCOSE = SMOOTH, a count = the
    // nearest spacing, neither = the mode's own.
    int p = menu_haptic_for(a->feel, a->detents);
    if (p >= 0) *profile = p;
}

int app_mode_live_slot(void) {
    return atomic_load(&s_live_slot);
}

void app_mode_wanted(uint8_t *buttons, uint8_t *modifier, bool *axis_y) {
    uint32_t w = atomic_load(&s_want);
    *buttons = (uint8_t)w;
    *modifier = (uint8_t)(w >> 8);
    *axis_y = (w >> 16) & 1;
}

int32_t app_mode_take_move_px(void) {
    return atomic_exchange(&s_move_px, 0);
}

void app_mode_return_move_px(int32_t px) {
    atomic_fetch_add(&s_move_px, px);
}

int32_t app_mode_take_wheel_steps(void) {
    return atomic_exchange(&s_wheel_steps, 0);
}

void app_mode_return_wheel_steps(int32_t steps) {
    atomic_fetch_add(&s_wheel_steps, steps);
}

bool app_mode_wheel(int *ring, int *entry) {
    uint32_t w = atomic_load(&s_wheel);
    *ring = (w >> 8) & 0xFF;
    *entry = w & 0xFF;
    return (w >> 31) != 0;
}

uint32_t app_mode_last_run(int *ring, int *entry) {
    uint32_t r = atomic_load(&s_run);
    *ring = (r >> 8) & 0xFF;
    *entry = r & 0xFF;
    return r >> 16;
}

bool app_mode_hover(void) {
    return (atomic_load(&s_want) & WANT_HOVER) != 0;
}

bool app_mode_param(app_param_state_t *s) {
    uint32_t w = atomic_load(&s_pword);
    s->active = (w >> 31) != 0;
    s->ring = (w >> 16) & 0xFF;
    s->entry = (w >> 8) & 0xFF;
    s->axis = w & 3;
    s->plane = (w & 4) != 0;
    s->uniform = (w & 8) != 0;
    s->step = (int)((w >> 4) & 3) - 1;
    s->exact = (w & 64) != 0;
    s->field = (w >> 24) & 1;
    s->typed = (w >> 25) & 1;
    s->value = atomic_load(&s_pvalue_milli) / 1000.0f;
    s->f3_ms = atomic_load(&s_pf3_ms);
    s->bump = atomic_load(&s_pbump);
    return s->active;
}

bool app_mode_take_tap(app_tap_t *key) {
    uint32_t tail = atomic_load(&s_tap_tail);
    if (tail == atomic_load(&s_tap_head)) return false;
    *key = s_taps[tail % APP_TAP_RING];
    atomic_store(&s_tap_tail, tail + 1);
    return true;
}
