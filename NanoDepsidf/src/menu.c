#include "menu.h"
#include "clock.h"
#include "config_store.h"
#include "haptic_params.h"
#include "app_profiles/app_profiles.h"
#include "sysmon.h"
#include "host_proto.h"
#include "midi.h"
#include "motor_sound.h"
#include "tasks_common.h"
#include "user_prefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

// --- Item/screen model ---

typedef enum {
    MENU_ITEM_SUBMENU, // enters a child screen
    MENU_ITEM_VALUE,   // enters edit mode; rendered as "label  value"
    MENU_ITEM_ACTION,  // F1 arms, a second F1 runs `action`; turning or F3 disarms
    MENU_ITEM_INFO,    // read-only (a SYS INFO page); F1 runs `action` at once, if any
} menu_item_kind_t;
// (An older ACTION kind was for the "Save" rows; saving is F2 now -- menu_input_save().)

typedef struct menu_screen_s menu_screen_t;

typedef struct {
    const char *label;
    const char *caption;                               // small engineering name, NULL = none
    menu_item_kind_t kind;
    const menu_screen_t *submenu;                      // MENU_ITEM_SUBMENU
    void (*format_value)(char *buf, size_t buf_size);  // MENU_ITEM_VALUE
    void (*on_rotate)(int8_t direction);               // MENU_ITEM_VALUE, called while editing
    bool (*is_enabled)(void);                          // NULL = always enabled
    bool (*is_muted)(void);                            // shown as "--", skipped by the knob; NULL = never
    bool (*at_end)(int8_t direction);                  // non-wrapping value at its end that way
    void (*action)(void);                              // MENU_ITEM_ACTION (confirming F1), MENU_ITEM_INFO (F1)
} menu_item_t;

struct menu_screen_s {
    menu_screen_id_t id;
    const char *title;
    const menu_item_t *items;
    int item_count;
    bool direct_edit;          // turn edits the focused value directly -- see menu.h
    void (*save)(void);        // F2; NULL = nothing to save on this screen
};

// --- Placeholder settings state (step 1 only) ---
// Not persisted, not the real haptic/HID/boot config -- steps 2-7 (DEVELOPMENT_PLAN.md
// Phase 8) replace these with real NVS-backed values shared with control_task.c/usb_task.c/
// main.c.
//
// _Atomic, matching ui_state.h's cross-core convention: on_rotate() (called from
// menu_input_rotate() on Core 0) writes, format_value() (called from
// menu_get_render_snapshot() on Core 1) reads. Word-sized atomics are naturally
// torn-read-free and lock-free on this target -- no mutex needed for these scalars, and
// crucially nothing here can block Core 0's real-time loop (see the state-lock comment
// below for why that matters).

// --- Haptic profiles: live state (haptic_params.h) ---
// One set of values per profile and feel, the feel each profile uses, the profile the
// Haptics screen is showing (edit) and the one the control loop is running (active: the edit
// one while the menu is open, otherwise the mode's or the app input's). All atomics, same
// convention as the rest of this file. control_task.c's loop reads the active profile's
// values every tick (menu_get_haptic_*() below).
#define LD(a) atomic_load_explicit(&(a), memory_order_relaxed)
#define ST(a, v) atomic_store_explicit(&(a), (v), memory_order_relaxed)

static _Atomic int32_t s_hp_edit = HAPTIC_PROFILE_COARSE;
static _Atomic int32_t s_hp_active = HAPTIC_PROFILE_COARSE;
static _Atomic int32_t s_hp_feel[HAPTIC_PROFILE_COUNT];
static _Atomic float s_hp_kp[HAPTIC_PROFILE_COUNT][HAPTIC_TYPE_COUNT];
static _Atomic float s_hp_kd[HAPTIC_PROFILE_COUNT][HAPTIC_TYPE_COUNT];
static _Atomic int32_t s_hp_shape[HAPTIC_PROFILE_COUNT][HAPTIC_TYPE_COUNT];
static _Atomic int32_t s_hp_amp[HAPTIC_PROFILE_COUNT][HAPTIC_TYPE_COUNT];
static _Atomic float s_hp_pitch[HAPTIC_PROFILE_COUNT][HAPTIC_TYPE_COUNT];
static _Atomic int32_t s_hp_click[HAPTIC_PROFILE_COUNT]; // the click's wave (motor_sound.h), per profile
// Each profile's detents per turn, copied to RAM for the control loop (the table is in flash).
static uint8_t s_hp_detents[HAPTIC_PROFILE_COUNT];
// The haptic profile each HID type uses (APP: for inputs that don't name their own). HOME's
// isn't a setting (home.h picks the feel: the list, or a value), and the NVS blob holds the four
// before it -- MODE_HAPTIC_STORED -- so it kept its size.
static _Atomic int32_t s_mode_hp[MENU_HID_TYPE_COUNT];
#define MODE_HAPTIC_STORED ((int)(sizeof(((mode_haptic_cfg_t *)0)->profile) / sizeof(int32_t)))

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline int hp_edit(void) { return (int)LD(s_hp_edit); }
static inline int hp_feel(int p) { return (int)LD(s_hp_feel[p]); }

static void hp_get_tune(int p, int f, haptic_tune_t *t) {
    t->kp = LD(s_hp_kp[p][f]);
    t->kd = LD(s_hp_kd[p][f]);
    t->shape = LD(s_hp_shape[p][f]);
    t->amp = LD(s_hp_amp[p][f]);
    t->pitch = LD(s_hp_pitch[p][f]);
}
// Stores a feel's values, each pulled into the profile's safe range for that feel.
static void hp_set_tune(int p, int f, const haptic_tune_t *t) {
    const haptic_limits_t *l = &HAPTIC_PROFILES[p].lim[f];
    ST(s_hp_kp[p][f], clampf(t->kp, l->kp_min, l->kp_max));
    ST(s_hp_kd[p][f], clampf(t->kd, l->kd_min, l->kd_max));
    ST(s_hp_shape[p][f], clampi(t->shape, HAPTIC_SHAPE_MIN, HAPTIC_SHAPE_MAX));
    ST(s_hp_amp[p][f], clampi(t->amp, HAPTIC_CLICK_AMP_MIN, l->amp_max));
    ST(s_hp_pitch[p][f], clampf(t->pitch, l->pitch_min, l->pitch_max));
}
// The five click shapes in the stored profiles' one spare word (it was the speaker's timbre,
// 0 or 1): four bits each, and flag bits to say they are there at all and which list they
// index. Only the first flag: three bits each, of the first eight shapes. The first two: four
// bits, of the fourteen there were for an afternoon. With HP_CLICK_LIST_10: of the ten after
// that. All four: today's list. The older ones are put through MOTOR_SOUND_SHAPE_FROM_14 and
// _FROM_10.
#define HP_CLICK_STORED 0x40000000
#define HP_CLICK_WIDE 0x20000000
#define HP_CLICK_LIST_10 0x10000000
#define HP_CLICK_LIST_9 0x08000000
_Static_assert(SNDCAL_CLICK_COUNT <= 16, "four bits a profile");
static int32_t hp_click_pack(void) {
    int32_t v = HP_CLICK_STORED | HP_CLICK_WIDE | HP_CLICK_LIST_10 | HP_CLICK_LIST_9;
    for (int p = 0; p < HAPTIC_PROFILE_COUNT; p++) v |= (LD(s_hp_click[p]) & 15) << (4 * p);
    return v;
}
static void hp_click_unpack(int32_t v) {
    if (!(v & HP_CLICK_STORED)) return;
    int bits = v & HP_CLICK_WIDE ? 4 : 3;
    for (int p = 0; p < HAPTIC_PROFILE_COUNT; p++) {
        int c = (v >> (bits * p)) & ((1 << bits) - 1);
        if (!(v & HP_CLICK_LIST_10)) c = c < 14 ? MOTOR_SOUND_SHAPE_FROM_14[c] : MOTOR_SOUND_SHAPE_DEFAULT;
        if (!(v & HP_CLICK_LIST_9)) c = c < 10 ? MOTOR_SOUND_SHAPE_FROM_10[c] : MOTOR_SOUND_SHAPE_DEFAULT;
        ST(s_hp_click[p], c < SNDCAL_CLICK_COUNT ? c : MOTOR_SOUND_SHAPE_DEFAULT);
    }
}
static void hp_factory(int p) {
    ST(s_hp_click[p], HAPTIC_PROFILES[p].click);
    ST(s_hp_feel[p], HAPTIC_PROFILES[p].feel);
    for (int f = 0; f < HAPTIC_TYPE_COUNT; f++) hp_set_tune(p, f, &HAPTIC_PROFILES[p].tune[f]);
}

// haptic_type_t itself now lives in haptic_params.h (Phase 8 step 4) -- control_task.c
// branches on it directly to pick a restoring-force law, so it can no longer be a
// menu.c-private enum the way it was in steps 1-3.
static const char *ph_haptic_type_name(haptic_type_t t) {
    switch (t) {
        case HAPTIC_TYPE_SAW: return "SAW";
        case HAPTIC_TYPE_SINE: return "SINE";
        case HAPTIC_TYPE_VISCOSE: return "VISCOSE";
        default: return "?";
    }
}

// menu_hid_type_t lives in menu.h -- display_task.cpp reads it for the mode icon.
static _Atomic menu_hid_type_t s_ph_hid_type = MENU_HID_APP; // default until something is saved
static const char *ph_hid_type_name(menu_hid_type_t t) {
    switch (t) {
        case MENU_HID_KEYBOARD: return "KEYBOARD";
        case MENU_HID_MOUSE: return "MOUSE";
        case MENU_HID_MIDI: return "MIDI";
        case MENU_HID_APP: return "APP";
        case MENU_HID_HOME: return "HOME";
        default: return "?";
    }
}

static _Atomic int32_t s_ph_midi_channel = 1; // 1-16
static _Atomic int32_t s_ph_midi_synth = 0;   // midi_synth_get() index; NVS stores the id
static _Atomic int32_t s_ph_app_profile = 0;  // app_profiles_get() index; NVS stores the id

// boot_usb_mode_t itself now lives in boot_mode.h (Phase 8 step 6) -- main.c reads it
// directly at startup to decide which USB personality to bring up.
static _Atomic boot_usb_mode_t s_ph_boot_mode = BOOT_USB_MODE_HID; // matches today's real default (normal boot = composite HID+CDC)
// Quarter turns clockwise. Direct screen: turning rotates the screen live, F2 keeps it.
static _Atomic int32_t s_ph_rotation = 0;

static const char *ph_boot_mode_name(boot_usb_mode_t m) {
    return (m == BOOT_USB_MODE_SERIAL) ? "SERIAL" : "HID";
}

// --- Field callbacks ---
// on_rotate() runs on Core 0 (called from menu_input_rotate()); format_value() runs on
// Core 1 (called from menu_get_render_snapshot()). Both sides only ever touch the atomics
// above -- no shared mutable buffers, so there's nothing here for a lock to protect.

// The Haptics screen edits the selected profile (STEPS) in its current feel. Every value is
// kept inside that profile's limits for that feel.
static void fmt_detents(char *buf, size_t n) {
    snprintf(buf, n, "%s", HAPTIC_PROFILES[hp_edit()].name);
}
static void CONTROL_HOT rotate_detents(int8_t dir) {
    ST(s_hp_edit, clampi(hp_edit() + dir, 0, HAPTIC_PROFILE_COUNT - 1));
}

static void fmt_kp(char *buf, size_t n) {
    int p = hp_edit();
    snprintf(buf, n, "%.2f", (double)LD(s_hp_kp[p][hp_feel(p)]));
}
static void CONTROL_HOT rotate_kp(int8_t dir) {
    int p = hp_edit(), f = hp_feel(p);
    const haptic_limits_t *l = &HAPTIC_PROFILES[p].lim[f];
    ST(s_hp_kp[p][f], clampf(LD(s_hp_kp[p][f]) + dir * 0.05f, l->kp_min, l->kp_max));
}

static void fmt_kd(char *buf, size_t n) {
    // ".010" rather than "0.010" -- the leading zero costs a character on the Orbit ring
    int p = hp_edit();
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%.3f", (double)LD(s_hp_kd[p][hp_feel(p)]));
    snprintf(buf, n, "%s", (tmp[0] == '0') ? tmp + 1 : tmp);
}
static void CONTROL_HOT rotate_kd(int8_t dir) {
    int p = hp_edit(), f = hp_feel(p);
    const haptic_limits_t *l = &HAPTIC_PROFILES[p].lim[f];
    ST(s_hp_kd[p][f], clampf(LD(s_hp_kd[p][f]) + dir * 0.005f, l->kd_min, l->kd_max));
}

static void fmt_shape(char *buf, size_t n) {
    int p = hp_edit();
    snprintf(buf, n, "%ld%%", (long)LD(s_hp_shape[p][hp_feel(p)]));
}
static void CONTROL_HOT rotate_shape(int8_t dir) {
    int p = hp_edit(), f = hp_feel(p);
    ST(s_hp_shape[p][f], clampi(LD(s_hp_shape[p][f]) + dir * HAPTIC_SHAPE_STEP, HAPTIC_SHAPE_MIN, HAPTIC_SHAPE_MAX));
}

static void fmt_haptic_type(char *buf, size_t n) {
    snprintf(buf, n, "%s", ph_haptic_type_name((haptic_type_t)hp_feel(hp_edit())));
}
// The next feel this profile allows, that way round. The feel's own values come with it.
static void CONTROL_HOT rotate_haptic_type(int8_t dir) {
    int p = hp_edit(), f = hp_feel(p);
    for (int i = 0; i < HAPTIC_TYPE_COUNT; i++) {
        f = (f + dir + HAPTIC_TYPE_COUNT) % HAPTIC_TYPE_COUNT;
        if (HAPTIC_PROFILES[p].feels & (1u << f)) break;
    }
    ST(s_hp_feel[p], f);
}

static void fmt_pitch(char *buf, size_t n) {
    int p = hp_edit();
    snprintf(buf, n, "%.2fX", (double)LD(s_hp_pitch[p][hp_feel(p)]));
}
static void CONTROL_HOT rotate_pitch(int8_t dir) {
    int p = hp_edit(), f = hp_feel(p);
    const haptic_limits_t *l = &HAPTIC_PROFILES[p].lim[f];
    ST(s_hp_pitch[p][f], clampf(LD(s_hp_pitch[p][f]) + dir * 0.05f, l->pitch_min, l->pitch_max));
}

static void fmt_amp(char *buf, size_t n) {
    int p = hp_edit();
    snprintf(buf, n, "%ld%%", (long)LD(s_hp_amp[p][hp_feel(p)]));
}
static void CONTROL_HOT rotate_amp(int8_t dir) {
    int p = hp_edit(), f = hp_feel(p);
    ST(s_hp_amp[p][f], clampi(LD(s_hp_amp[p][f]) + dir * HAPTIC_CLICK_AMP_STEP, HAPTIC_CLICK_AMP_MIN, HAPTIC_PROFILES[p].lim[f].amp_max));
}

// Muted rows: shown as "--" and skipped by the knob. SNAP means nothing in VISCOSE, SHAPE
// only bends SAW, and FEEL has nothing to choose in a one-feel profile (SMOOTH).
static bool CONTROL_HOT snap_muted(void) {
    return hp_feel(hp_edit()) == HAPTIC_TYPE_VISCOSE;
}
static bool CONTROL_HOT shape_muted(void) {
    return hp_feel(hp_edit()) != HAPTIC_TYPE_SAW;
}
static bool CONTROL_HOT feel_muted(void) {
    uint8_t m = HAPTIC_PROFILES[hp_edit()].feels;
    return (m & (m - 1)) == 0;
}

static void action_save_haptic(void) {
    haptic_profiles_cfg_t cfg = {
        .version = HAPTIC_PROFILES_CFG_VERSION,
        .edit = hp_edit(),
        .click = hp_click_pack(),
    };
    for (int p = 0; p < HAPTIC_PROFILE_COUNT; p++) {
        cfg.feel[p] = hp_feel(p);
        for (int f = 0; f < HAPTIC_TYPE_COUNT; f++) hp_get_tune(p, f, &cfg.tune[p][f]);
    }
    config_store_save_haptic_profiles(&cfg);
}

static void fmt_hid_type(char *buf, size_t n) {
    snprintf(buf, n, "%s", ph_hid_type_name(atomic_load_explicit(&s_ph_hid_type, memory_order_relaxed)));
}
static void CONTROL_HOT rotate_hid_type(int8_t dir) {
    int pos = menu_hid_type_pos(atomic_load_explicit(&s_ph_hid_type, memory_order_relaxed)) + dir;
    atomic_store_explicit(&s_ph_hid_type, menu_hid_type_at(pos), memory_order_relaxed);
}
// KEYBOARD and MOUSE: which haptic profile the knob uses in that mode.
static bool mode_haptic_enabled(void) {
    menu_hid_type_t t = atomic_load_explicit(&s_ph_hid_type, memory_order_relaxed);
    return t == MENU_HID_KEYBOARD || t == MENU_HID_MOUSE;
}
static void fmt_mode_haptic(char *buf, size_t n) {
    snprintf(buf, n, "%s", HAPTIC_PROFILES[LD(s_mode_hp[atomic_load_explicit(&s_ph_hid_type, memory_order_relaxed)])].name);
}
static void CONTROL_HOT rotate_mode_haptic(int8_t dir) {
    menu_hid_type_t t = atomic_load_explicit(&s_ph_hid_type, memory_order_relaxed);
    ST(s_mode_hp[t], clampi(LD(s_mode_hp[t]) + dir, 0, HAPTIC_PROFILE_COUNT - 1));
}
static bool midi_mapping_enabled(void) {
    return atomic_load_explicit(&s_ph_hid_type, memory_order_relaxed) == MENU_HID_MIDI;
}

static void fmt_midi_mapping(char *buf, size_t n) {
    snprintf(buf, n, "%02ld", (long)atomic_load_explicit(&s_ph_midi_channel, memory_order_relaxed));
}
static void CONTROL_HOT rotate_midi_mapping(int8_t dir) {
    int32_t v = atomic_load_explicit(&s_ph_midi_channel, memory_order_relaxed) + dir;
    if (v < 1) v = 1;
    if (v > 16) v = 16;
    atomic_store_explicit(&s_ph_midi_channel, v, memory_order_relaxed);
}
// MIDI: which synth profile (midi_synths.c) the knob plays. A list with ends, like PROFILE.
static void fmt_midi_synth(char *buf, size_t n) {
    snprintf(buf, n, "%s", midi_synth_get(atomic_load_explicit(&s_ph_midi_synth, memory_order_relaxed))->name);
}
static bool CONTROL_HOT midi_synth_at_end(int8_t dir) {
    int v = (int)atomic_load_explicit(&s_ph_midi_synth, memory_order_relaxed);
    return dir > 0 ? v >= midi_synth_count() - 1 : v <= 0;
}
static void CONTROL_HOT rotate_midi_synth(int8_t dir) {
    int v = (int)atomic_load_explicit(&s_ph_midi_synth, memory_order_relaxed) + dir;
    atomic_store_explicit(&s_ph_midi_synth, clampi(v, 0, midi_synth_count() - 1), memory_order_relaxed);
}

static bool app_profile_enabled(void) {
    return atomic_load_explicit(&s_ph_hid_type, memory_order_relaxed) == MENU_HID_APP;
}
static void fmt_app_profile(char *buf, size_t n) {
    snprintf(buf, n, "%s", app_profiles_get(atomic_load_explicit(&s_ph_app_profile, memory_order_relaxed))->name);
}
// The profile list stops at both ends instead of wrapping: with only a few apps, a
// wrapping carousel would show the same app on both sides.
static bool CONTROL_HOT app_profile_at_end(int8_t dir) {
    int v = (int)atomic_load_explicit(&s_ph_app_profile, memory_order_relaxed);
    return dir > 0 ? v >= app_profiles_count() - 1 : v <= 0;
}
static void CONTROL_HOT rotate_app_profile(int8_t dir) {
    int v = (int)atomic_load_explicit(&s_ph_app_profile, memory_order_relaxed) + dir;
    if (v < 0) v = 0;
    if (v >= app_profiles_count()) v = app_profiles_count() - 1;
    atomic_store_explicit(&s_ph_app_profile, v, memory_order_relaxed);
}

static void action_save_hid(void) {
    hid_cfg_t cfg = {
        .hid_type = (int32_t)atomic_load_explicit(&s_ph_hid_type, memory_order_relaxed),
        .midi_channel = atomic_load_explicit(&s_ph_midi_channel, memory_order_relaxed),
    };
    config_store_save_hid(&cfg);
    config_store_save_app_profile(app_profiles_get(atomic_load_explicit(&s_ph_app_profile, memory_order_relaxed))->id);
    config_store_save_midi_synth(midi_synth_get(atomic_load_explicit(&s_ph_midi_synth, memory_order_relaxed))->id);
    mode_haptic_cfg_t mcfg;
    for (int i = 0; i < MODE_HAPTIC_STORED; i++) mcfg.profile[i] = LD(s_mode_hp[i]);
    config_store_save_mode_haptic(&mcfg);
}

static void fmt_boot_mode(char *buf, size_t n) {
    snprintf(buf, n, "%s", ph_boot_mode_name(atomic_load_explicit(&s_ph_boot_mode, memory_order_relaxed)));
}
static void CONTROL_HOT rotate_boot_mode(int8_t dir) {
    int v = ((int)atomic_load_explicit(&s_ph_boot_mode, memory_order_relaxed) + dir) % BOOT_USB_MODE_COUNT;
    if (v < 0) v += BOOT_USB_MODE_COUNT;
    atomic_store_explicit(&s_ph_boot_mode, (boot_usb_mode_t)v, memory_order_relaxed);
}
static void fmt_rotation(char *buf, size_t n) {
    snprintf(buf, n, "%ld", (long)atomic_load_explicit(&s_ph_rotation, memory_order_relaxed) * 90);
}
static void CONTROL_HOT rotate_rotation(int8_t dir) {
    int32_t v = (atomic_load_explicit(&s_ph_rotation, memory_order_relaxed) + dir) % MENU_DISPLAY_ROTATIONS;
    if (v < 0) v += MENU_DISPLAY_ROTATIONS;
    atomic_store_explicit(&s_ph_rotation, v, memory_order_relaxed);
}
static void save_screen_part(bool sleep_part);
static void action_save_display(void) {
    display_cfg_t cfg = {.rotation = atomic_load_explicit(&s_ph_rotation, memory_order_relaxed)};
    config_store_save_display(&cfg);
    save_screen_part(false);
}

// DISPLAY's BRIGHT, SAVER, AFTER and the SLEEP screen (user_prefs.h screen_t). Direct screens
// like LIGHTS: turning changes it live (the backlight follows at once), F2 keeps it. On SLEEP,
// the rows after SLEEP only mean something with it on -- muted otherwise.
static void fmt_bright(char *buf, size_t n) {
    screen_t s;
    screen_get(&s);
    snprintf(buf, n, "%ld%%", (long)s.bright);
}
static void fmt_saver(char *buf, size_t n) {
    screen_t s;
    screen_get(&s);
    snprintf(buf, n, "%s", screen_saver_name(s.saver));
}
static void fmt_saver_s(char *buf, size_t n) {
    screen_t s;
    screen_get(&s);
    screen_format_secs(s.saver_s, buf, n);
}
static void fmt_sleep_on(char *buf, size_t n) {
    // On, but the knob doesn't trust its local time (clock.h): the hours wait for it.
    snprintf(buf, n, "%s", !screen_sleep_enabled() ? "OFF" : clock_trusted() ? "ON" : "NO TIME");
}
static void fmt_sleep_from(char *buf, size_t n) {
    screen_t s;
    screen_get(&s);
    screen_format_time(s.sleep_from, buf, n);
}
static void fmt_sleep_to(char *buf, size_t n) {
    screen_t s;
    screen_get(&s);
    screen_format_time(s.sleep_to, buf, n);
}
static void fmt_dark_s(char *buf, size_t n) {
    screen_t s;
    screen_get(&s);
    screen_format_secs(s.dark_s, buf, n);
}
static void fmt_wake(char *buf, size_t n) {
    screen_t s;
    screen_get(&s);
    snprintf(buf, n, "%s", screen_wake_name(s.wake));
}
static bool CONTROL_HOT sleep_rows_muted(void) { return !screen_sleep_enabled(); }
static void action_save_sleep(void) { save_screen_part(true); }

// DEVICE -> BINDINGS. Direct screen: turning switches it live (the next key report already
// uses it), F2 keeps it.
static _Atomic menu_host_t s_ph_host = MENU_HOST_MAC;
static void fmt_host(char *buf, size_t n) {
    snprintf(buf, n, "%s", atomic_load_explicit(&s_ph_host, memory_order_relaxed) == MENU_HOST_PC ? "PC" : "MAC");
}
static void CONTROL_HOT rotate_host(int8_t dir) {
    int v = ((int)atomic_load_explicit(&s_ph_host, memory_order_relaxed) + dir) % MENU_HOST_COUNT;
    if (v < 0) v += MENU_HOST_COUNT;
    atomic_store_explicit(&s_ph_host, (menu_host_t)v, memory_order_relaxed);
}
static void action_save_bindings(void) {
    bind_cfg_t cfg = {.host = (int32_t)atomic_load_explicit(&s_ph_host, memory_order_relaxed)};
    config_store_save_bindings(&cfg);
}

static void action_save_boot(void) {
    boot_cfg_t cfg = {
        .boot_mode = (int32_t)atomic_load_explicit(&s_ph_boot_mode, memory_order_relaxed),
    };
    config_store_save_boot(&cfg);
}

// LIGHTS (user_prefs.h). Direct screen: turning changes the LEDs live, F2 keeps the look,
// leaving without F2 puts it back. HUE / SAT only mean something with COLOR = CUSTOM, SPEED
// only for the moving effects -- muted otherwise.
static void fmt_light_src(char *buf, size_t n) {
    snprintf(buf, n, "%s", lights_custom() ? "CUSTOM" : "APP");
}
static void fmt_light_hue(char *buf, size_t n) {
    lights_t l;
    lights_get(&l);
    snprintf(buf, n, "%ld", (long)l.hue);
}
static void fmt_light_sat(char *buf, size_t n) {
    lights_t l;
    lights_get(&l);
    snprintf(buf, n, "%ld%%", (long)l.sat);
}
static void fmt_light_fx(char *buf, size_t n) {
    lights_t l;
    lights_get(&l);
    snprintf(buf, n, "%s", lights_fx_name(l.fx));
}
static void fmt_light_speed(char *buf, size_t n) {
    lights_t l;
    lights_get(&l);
    snprintf(buf, n, "%ld", (long)l.speed);
}
static void fmt_light_level(char *buf, size_t n) {
    lights_t l;
    lights_get(&l);
    snprintf(buf, n, "%ld%%", (long)l.level);
}
static bool CONTROL_HOT light_color_muted(void) { return !lights_custom(); }
static bool CONTROL_HOT light_speed_muted(void) { return !lights_fx_animated(); }
static void action_save_lights(void) { lights_save(); }

// --- Screens ---
// Labels are the Pixel UI's display names (DEVELOPMENT_PLAN.md "Pixel UI"); captions keep the
// engineering names visible in small type. Detents are "STEPS" for now -- they're due for a
// proper rename later.

// The click's wave (motor_sound.h) for the profile shown, like its AMP and PITCH.
static void fmt_click_short(char *buf, size_t n) {
    snprintf(buf, n, "%s", MOTOR_SOUND_SHAPE_SHORT[LD(s_hp_click[hp_edit()])]);
}
static void rotate_click_shape(int8_t dir) {
    int p = hp_edit();
    int v = ((int)LD(s_hp_click[p]) + dir) % SNDCAL_CLICK_COUNT;
    if (v < 0) v += SNDCAL_CLICK_COUNT;
    ST(s_hp_click[p], v);
}

static const menu_item_t s_haptic_items[MENU_HAPTIC_ROW_COUNT] = {
    [MENU_HAPTIC_ROW_STEPS] = { .label = "STEPS", .caption = "PROFILE", .kind = MENU_ITEM_VALUE, .format_value = fmt_detents,     .on_rotate = rotate_detents },
    [MENU_HAPTIC_ROW_SNAP]  = { .label = "SNAP",  .caption = "KP",      .kind = MENU_ITEM_VALUE, .format_value = fmt_kp,          .on_rotate = rotate_kp, .is_muted = snap_muted },
    [MENU_HAPTIC_ROW_DAMP]  = { .label = "DAMP",  .caption = "KD",      .kind = MENU_ITEM_VALUE, .format_value = fmt_kd,          .on_rotate = rotate_kd },
    [MENU_HAPTIC_ROW_SHAPE] = { .label = "SHAPE", .caption = "RAMP",    .kind = MENU_ITEM_VALUE, .format_value = fmt_shape,       .on_rotate = rotate_shape, .is_muted = shape_muted },
    [MENU_HAPTIC_ROW_FEEL]  = { .label = "FEEL",  .caption = "TYPE",    .kind = MENU_ITEM_VALUE, .format_value = fmt_haptic_type, .on_rotate = rotate_haptic_type, .is_muted = feel_muted },
    [MENU_HAPTIC_ROW_AMP]   = { .label = "AMP",   .caption = "AMPLITUDE", .kind = MENU_ITEM_VALUE, .format_value = fmt_amp,       .on_rotate = rotate_amp },
    [MENU_HAPTIC_ROW_PITCH] = { .label = "PITCH", .caption = "CLICK",   .kind = MENU_ITEM_VALUE, .format_value = fmt_pitch,       .on_rotate = rotate_pitch },
    [MENU_HAPTIC_ROW_CLICK] = { .label = "CLICK", .caption = "WAVE",    .kind = MENU_ITEM_VALUE, .format_value = fmt_click_short, .on_rotate = rotate_click_shape },
};
static const menu_screen_t s_haptic_screen = {
    MENU_SCREEN_HAPTIC, "Haptic Configurator", s_haptic_items, MENU_HAPTIC_ROW_COUNT, false, action_save_haptic
};

// APP -> F1 opens a screen of its own for choosing the app profile (a carousel of app
// icons), rather than an inline row -- it scales past a handful of apps.
static const menu_item_t s_app_profile_items[] = {
    { .label = "PROFILE", .kind = MENU_ITEM_VALUE, .format_value = fmt_app_profile, .on_rotate = rotate_app_profile, .at_end = app_profile_at_end },
};
static const menu_screen_t s_app_profile_screen = {
    MENU_SCREEN_APP_PROFILE, "App Profile", s_app_profile_items, 1, true, action_save_hid
};

static const menu_item_t s_hid_items[] = {
    { .label = "PROFILES", .kind = MENU_ITEM_VALUE,   .format_value = fmt_hid_type,     .on_rotate = rotate_hid_type },
    { .label = "HAPTIC",   .kind = MENU_ITEM_VALUE,   .format_value = fmt_mode_haptic,  .on_rotate = rotate_mode_haptic,  .is_enabled = mode_haptic_enabled },
    { .label = "CHANNEL",  .kind = MENU_ITEM_VALUE,   .format_value = fmt_midi_mapping, .on_rotate = rotate_midi_mapping, .is_enabled = midi_mapping_enabled },
    { .label = "SYNTH",    .kind = MENU_ITEM_VALUE,   .format_value = fmt_midi_synth,   .on_rotate = rotate_midi_synth,   .is_enabled = midi_mapping_enabled, .at_end = midi_synth_at_end },
    { .label = "PROFILE",  .kind = MENU_ITEM_SUBMENU, .submenu = &s_app_profile_screen, .format_value = fmt_app_profile, .is_enabled = app_profile_enabled },
};
static const menu_screen_t s_hid_screen = {
    MENU_SCREEN_HID, "Profiles", s_hid_items, sizeof(s_hid_items) / sizeof(s_hid_items[0]), true, action_save_hid
};

static const menu_item_t s_boot_items[] = {
    { .label = "USB MODE", .kind = MENU_ITEM_VALUE, .format_value = fmt_boot_mode, .on_rotate = rotate_boot_mode },
};
static const menu_screen_t s_boot_screen = {
    MENU_SCREEN_BOOT, "Boot USB Mode", s_boot_items, sizeof(s_boot_items) / sizeof(s_boot_items[0]), true, action_save_boot
};

static const menu_item_t s_display_items[MENU_DISPLAY_ROW_COUNT] = {
    [MENU_DISPLAY_ROW_ROTATION] = { .label = "ROTATION", .kind = MENU_ITEM_VALUE, .format_value = fmt_rotation, .on_rotate = rotate_rotation },
    [MENU_DISPLAY_ROW_BRIGHT]   = { .label = "BRIGHT",   .kind = MENU_ITEM_VALUE, .format_value = fmt_bright,   .on_rotate = screen_rotate_bright },
    [MENU_DISPLAY_ROW_SAVER]    = { .label = "SAVER",    .kind = MENU_ITEM_VALUE, .format_value = fmt_saver,    .on_rotate = screen_rotate_saver },
    [MENU_DISPLAY_ROW_AFTER]    = { .label = "AFTER",    .kind = MENU_ITEM_VALUE, .format_value = fmt_saver_s,  .on_rotate = screen_rotate_saver_s },
};
static const menu_screen_t s_display_screen = {
    MENU_SCREEN_DISPLAY, "Display", s_display_items, MENU_DISPLAY_ROW_COUNT, true, action_save_display
};

static const menu_item_t s_sleep_items[MENU_SLEEP_ROW_COUNT] = {
    [MENU_SLEEP_ROW_ON]   = { .label = "SLEEP",   .kind = MENU_ITEM_VALUE, .format_value = fmt_sleep_on,   .on_rotate = screen_rotate_sleep_on },
    [MENU_SLEEP_ROW_FROM] = { .label = "FROM",    .kind = MENU_ITEM_VALUE, .format_value = fmt_sleep_from, .on_rotate = screen_rotate_sleep_from, .is_muted = sleep_rows_muted },
    [MENU_SLEEP_ROW_TO]   = { .label = "TO",      .kind = MENU_ITEM_VALUE, .format_value = fmt_sleep_to,   .on_rotate = screen_rotate_sleep_to,   .is_muted = sleep_rows_muted },
    [MENU_SLEEP_ROW_DARK] = { .label = "DARK IN", .kind = MENU_ITEM_VALUE, .format_value = fmt_dark_s,     .on_rotate = screen_rotate_dark_s,     .is_muted = sleep_rows_muted },
    [MENU_SLEEP_ROW_WAKE] = { .label = "WAKE",    .kind = MENU_ITEM_VALUE, .format_value = fmt_wake,       .on_rotate = screen_rotate_wake,       .is_muted = sleep_rows_muted },
};
static const menu_screen_t s_sleep_screen = {
    MENU_SCREEN_SLEEP, "Sleep", s_sleep_items, MENU_SLEEP_ROW_COUNT, true, action_save_sleep
};

static const menu_item_t s_lights_items[MENU_LIGHTS_ROW_COUNT] = {
    [MENU_LIGHTS_ROW_COLOR]  = { .label = "COLOR",  .kind = MENU_ITEM_VALUE, .format_value = fmt_light_src,   .on_rotate = lights_rotate_src },
    [MENU_LIGHTS_ROW_HUE]    = { .label = "HUE",    .kind = MENU_ITEM_VALUE, .format_value = fmt_light_hue,   .on_rotate = lights_rotate_hue,   .is_muted = light_color_muted },
    [MENU_LIGHTS_ROW_SAT]    = { .label = "SAT",    .kind = MENU_ITEM_VALUE, .format_value = fmt_light_sat,   .on_rotate = lights_rotate_sat,   .is_muted = light_color_muted },
    [MENU_LIGHTS_ROW_EFFECT] = { .label = "EFFECT", .kind = MENU_ITEM_VALUE, .format_value = fmt_light_fx,    .on_rotate = lights_rotate_fx },
    [MENU_LIGHTS_ROW_SPEED]  = { .label = "SPEED",  .kind = MENU_ITEM_VALUE, .format_value = fmt_light_speed, .on_rotate = lights_rotate_speed, .is_muted = light_speed_muted },
    [MENU_LIGHTS_ROW_LEVEL]  = { .label = "LEVEL",  .kind = MENU_ITEM_VALUE, .format_value = fmt_light_level, .on_rotate = lights_rotate_level },
};
static const menu_screen_t s_lights_screen = {
    MENU_SCREEN_LIGHTS, "Lights", s_lights_items, MENU_LIGHTS_ROW_COUNT, true, action_save_lights
};

// SYS INFO: read-only pages drawn from sysmon.h (and pd_status.h for the USB contract);
// turning moves between them, F1 starts the peaks and counters over.
static const menu_item_t s_sysinfo_items[MENU_SYSINFO_PAGE_COUNT] = {
    [MENU_SYSINFO_POWER]  = { .label = "POWER",  .kind = MENU_ITEM_INFO, .action = sysmon_reset_peaks },
    [MENU_SYSINFO_HEAT]   = { .label = "HEAT",   .kind = MENU_ITEM_INFO, .action = sysmon_reset_peaks },
    [MENU_SYSINFO_CPU]    = { .label = "CPU",    .kind = MENU_ITEM_INFO, .action = sysmon_reset_peaks },
    [MENU_SYSINFO_LOOP]   = { .label = "LOOP",   .kind = MENU_ITEM_INFO, .action = sysmon_reset_peaks },
    [MENU_SYSINFO_SYSTEM] = { .label = "SYSTEM", .kind = MENU_ITEM_INFO, .action = sysmon_reset_peaks },
};
static const menu_screen_t s_sysinfo_screen = {
    MENU_SCREEN_SYSINFO, "Sys Info", s_sysinfo_items, MENU_SYSINFO_PAGE_COUNT, false, NULL
};

// RECALIBRATE: its action only raises a flag -- control_task.c owns the motor, so it does the
// work there (see menu_take_recalibrate_request()).
static _Atomic bool s_recal_request = false;
static void action_recalibrate(void) {
    atomic_store_explicit(&s_recal_request, true, memory_order_relaxed);
}
static const menu_item_t s_recal_items[] = {
    { .label = "RECALIBRATE", .kind = MENU_ITEM_ACTION, .action = action_recalibrate },
};
static const menu_screen_t s_recal_screen = {
    MENU_SCREEN_RECALIBRATE, "Recalibrate", s_recal_items, 1, false, NULL
};

static const menu_item_t s_bindings_items[] = {
    { .label = "COMPUTER", .kind = MENU_ITEM_VALUE, .format_value = fmt_host, .on_rotate = rotate_host },
};
static const menu_screen_t s_bindings_screen = {
    MENU_SCREEN_BINDINGS, "Bindings", s_bindings_items, 1, true, action_save_bindings
};

// DEVICE -> CLICK: the axis the motor's sounds play on (motor_sound.h); the wave is each haptic
// profile's own (HAPTICS -> CLICK). A direct screen: turning switches it live -- and clicks, so
// it is heard as it is picked.
static void fmt_click_axis(char *buf, size_t n) {
    snprintf(buf, n, "%s", motor_sound_axis() ? "Q" : "D");
}
static void rotate_click_axis(int8_t dir) {
    (void)dir;
    motor_sound_set_axis(!motor_sound_axis());
}
static void action_save_click(void) {
    motor_sound_save();
}
static const menu_item_t s_click_items[] = {
    { .label = "AXIS",  .kind = MENU_ITEM_VALUE, .format_value = fmt_click_axis,  .on_rotate = rotate_click_axis },
};
static const menu_screen_t s_click_screen = {
    MENU_SCREEN_CLICK, "Click", s_click_items, 1, true, action_save_click
};

// SOUND CAL: F1 starts it. motor_sound.c runs it from there and takes F1-F3 while it does
// (control_task.c); F4 or leaving the screen stops it.
static const menu_item_t s_sound_cal_items[] = {
    { .label = "SOUND CAL", .kind = MENU_ITEM_INFO, .action = motor_sound_cal_start },
};
static const menu_screen_t s_sound_cal_screen = {
    MENU_SCREEN_SOUND_CAL, "Sound Cal", s_sound_cal_items, 1, false, NULL
};

// DEVICE: a list like the top level.
static const menu_item_t s_device_items[] = {
    { .label = "SYS INFO",    .kind = MENU_ITEM_SUBMENU, .submenu = &s_sysinfo_screen },
    { .label = "BINDINGS",    .kind = MENU_ITEM_SUBMENU, .submenu = &s_bindings_screen },
    { .label = "RECALIBRATE", .kind = MENU_ITEM_SUBMENU, .submenu = &s_recal_screen },
    { .label = "CLICK",       .kind = MENU_ITEM_SUBMENU, .submenu = &s_click_screen },
    { .label = "SOUND CAL",   .kind = MENU_ITEM_SUBMENU, .submenu = &s_sound_cal_screen },
};
static const menu_screen_t s_device_screen = {
    MENU_SCREEN_DEVICE, "Device", s_device_items, sizeof(s_device_items) / sizeof(s_device_items[0]), false, NULL
};

static const menu_item_t s_root_items[] = {
    { .label = "PROFILES",  .kind = MENU_ITEM_SUBMENU, .submenu = &s_hid_screen },
    { .label = "HAPTICS",   .kind = MENU_ITEM_SUBMENU, .submenu = &s_haptic_screen },
    { .label = "DISPLAY",   .kind = MENU_ITEM_SUBMENU, .submenu = &s_display_screen },
    { .label = "SLEEP",     .kind = MENU_ITEM_SUBMENU, .submenu = &s_sleep_screen },
    { .label = "LIGHTS",    .kind = MENU_ITEM_SUBMENU, .submenu = &s_lights_screen },
    { .label = "BOOT MODE", .kind = MENU_ITEM_SUBMENU, .submenu = &s_boot_screen },
    { .label = "DEVICE",    .kind = MENU_ITEM_SUBMENU, .submenu = &s_device_screen },
};
static const menu_screen_t s_root_screen = {
    MENU_SCREEN_ROOT, "", s_root_items, sizeof(s_root_items) / sizeof(s_root_items[0]), false, NULL
};

// --- Navigation state ---
// A stack rather than separate "MenuRoot"/"Section" state: depth 0 = closed, depth 1 =
// top-level screen, depth 2+ = however deep a submenu goes. F3 (back) pops one frame; F1
// (select) on a MENU_ITEM_SUBMENU pushes one.
//
// **Real-time note, learned the hard way on hardware**: menu_input_*() below runs on Core 0
// inside control_task.c's hard-real-time 10kHz loop (one call per button edge / per detent
// crossing). The first version of this file recomputed a fully-formatted render snapshot
// (snprintf across up to 8 rows) inline on every such call, guarded by a FreeRTOS mutex
// shared with the display task. At slow rotation that's rarely a problem, but fast CW
// rotation drives many detent crossings per second -- each one doing variable-cost
// string-formatting work (and a mutex take that can block if Core 1 happens to hold it)
// inside a loop whose slew-rate-limiter math assumes a fixed 100us dt per iteration.
// Repeated budget overruns clustering at high spin rates manifested on hardware as the
// knob "overshooting and spinning by itself" -- the same class of scheduler-starvation bug
// as Phase 2a's per-tick-logging watchdog incident, just triggered by string formatting
// instead of UART logging. Fixed by only ever touching a few bounded ints/pointers here,
// under a spinlock (portMUX, non-blocking, unlike a mutex) -- all string formatting moved
// to menu_get_render_snapshot(), called only from Core 1's relaxed ~30ms display poll,
// which has no real-time constraint to violate.

#define MENU_MAX_DEPTH 4

typedef struct {
    const menu_screen_t *screen;
    int selected_index;
} menu_stack_frame_t;

static menu_stack_frame_t s_stack[MENU_MAX_DEPTH];
static int s_stack_depth = 0; // 0 = closed
static bool s_editing = false;
static bool s_armed = false; // a MENU_ITEM_ACTION waiting for its confirming F1
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static bool CONTROL_HOT item_enabled(const menu_screen_t *screen, int index) {
    const menu_item_t *it = &screen->items[index];
    return it->is_enabled == NULL || it->is_enabled();
}

static bool CONTROL_HOT item_muted(const menu_screen_t *screen, int index) {
    const menu_item_t *it = &screen->items[index];
    return it->is_muted != NULL && it->is_muted();
}

static int first_enabled_index(const menu_screen_t *screen) {
    for (int i = 0; i < screen->item_count; i++) {
        if (item_enabled(screen, i)) return i;
    }
    return -1; // no enabled items -- shouldn't happen with any screen defined above
}

// Steps the selection by +-1, skipping disabled items, wrapping at both ends. Bounded by
// item_count iterations (at most 6 today) so this stays cheap and deterministic even
// though it's called from Core 0's real-time loop.
static int CONTROL_HOT step_index(const menu_screen_t *screen, int current, int8_t dir) {
    if (screen->item_count <= 0) return current;
    int idx = current;
    for (int i = 0; i < screen->item_count; i++) {
        idx = (idx + dir) % screen->item_count;
        if (idx < 0) idx += screen->item_count;
        if (item_enabled(screen, idx) && !item_muted(screen, idx)) return idx;
    }
    return current;
}

// --- Settings snapshots (Pixel UI) ---
// One plain copy of every setting atomic. Used three ways:
//   - s_saved: what NVS holds -- compared against the live atomics for the "unsaved" (dirty)
//     cue, and restored from when a direct screen (HID / Boot mode) is left without saving.
//   - s_undo: taken when a Haptic edit starts, restored by F3 (cancel).
// All capture/restore work is a handful of atomic loads/stores -- bounded and cheap, safe on
// Core 0's real-time loop. s_saved is read from Core 1 (snapshot) and written from Core 0
// (save), so it's only ever copied under s_state_mux.

typedef struct {
    int32_t hp_edit;
    int32_t hp_feel[HAPTIC_PROFILE_COUNT];
    haptic_tune_t hp_tune[HAPTIC_PROFILE_COUNT][HAPTIC_TYPE_COUNT];
    int32_t mode_hp[MENU_HID_TYPE_COUNT];
    menu_hid_type_t hid_type;
    int32_t midi_channel;
    int32_t midi_synth;
    int32_t app_profile;
    boot_usb_mode_t boot_mode;
    int32_t rotation;
    menu_host_t host;
    lights_t lights;
    screen_t screen;
    int32_t hp_click[HAPTIC_PROFILE_COUNT];
    int click_axis;
} settings_t;

static settings_t s_saved;
static settings_t s_undo;
static _Atomic uint32_t s_save_count = 0;
static _Atomic uint32_t s_reset_count = 0; // a haptic profile went back to factory

static void settings_capture(settings_t *s) {
    s->hp_edit = hp_edit();
    for (int p = 0; p < HAPTIC_PROFILE_COUNT; p++) {
        s->hp_feel[p] = hp_feel(p);
        for (int f = 0; f < HAPTIC_TYPE_COUNT; f++) hp_get_tune(p, f, &s->hp_tune[p][f]);
    }
    for (int i = 0; i < MENU_HID_TYPE_COUNT; i++) s->mode_hp[i] = LD(s_mode_hp[i]);
    s->hid_type = atomic_load_explicit(&s_ph_hid_type, memory_order_relaxed);
    s->midi_channel = atomic_load_explicit(&s_ph_midi_channel, memory_order_relaxed);
    s->midi_synth = atomic_load_explicit(&s_ph_midi_synth, memory_order_relaxed);
    s->app_profile = atomic_load_explicit(&s_ph_app_profile, memory_order_relaxed);
    s->boot_mode = atomic_load_explicit(&s_ph_boot_mode, memory_order_relaxed);
    s->rotation = atomic_load_explicit(&s_ph_rotation, memory_order_relaxed);
    s->host = atomic_load_explicit(&s_ph_host, memory_order_relaxed);
    lights_get(&s->lights);
    screen_get(&s->screen);
    for (int p = 0; p < HAPTIC_PROFILE_COUNT; p++) s->hp_click[p] = LD(s_hp_click[p]);
    s->click_axis = motor_sound_axis();
}

static void settings_restore(const settings_t *s) {
    ST(s_hp_edit, s->hp_edit);
    for (int p = 0; p < HAPTIC_PROFILE_COUNT; p++) {
        ST(s_hp_feel[p], s->hp_feel[p]);
        for (int f = 0; f < HAPTIC_TYPE_COUNT; f++) hp_set_tune(p, f, &s->hp_tune[p][f]);
    }
    for (int i = 0; i < MENU_HID_TYPE_COUNT; i++) ST(s_mode_hp[i], s->mode_hp[i]);
    atomic_store_explicit(&s_ph_hid_type, s->hid_type, memory_order_relaxed);
    atomic_store_explicit(&s_ph_midi_channel, s->midi_channel, memory_order_relaxed);
    atomic_store_explicit(&s_ph_midi_synth, s->midi_synth, memory_order_relaxed);
    atomic_store_explicit(&s_ph_app_profile, s->app_profile, memory_order_relaxed);
    atomic_store_explicit(&s_ph_boot_mode, s->boot_mode, memory_order_relaxed);
    atomic_store_explicit(&s_ph_rotation, s->rotation, memory_order_relaxed);
    atomic_store_explicit(&s_ph_host, s->host, memory_order_relaxed);
    lights_set(&s->lights);
    screen_set(&s->screen);
    for (int p = 0; p < HAPTIC_PROFILE_COUNT; p++) ST(s_hp_click[p], s->hp_click[p]);
    motor_sound_set_axis(s->click_axis);
}

// Copies only the fields a screen owns (what that screen's save writes to NVS).
static void settings_copy_group(settings_t *dst, const settings_t *src, menu_screen_id_t group) {
    switch (group) {
        case MENU_SCREEN_HAPTIC:
            dst->hp_edit = src->hp_edit;
            memcpy(dst->hp_feel, src->hp_feel, sizeof(dst->hp_feel));
            memcpy(dst->hp_tune, src->hp_tune, sizeof(dst->hp_tune));
            memcpy(dst->hp_click, src->hp_click, sizeof(dst->hp_click));
            break;
        case MENU_SCREEN_HID:
            dst->hid_type = src->hid_type;
            dst->midi_channel = src->midi_channel;
            dst->midi_synth = src->midi_synth;
            memcpy(dst->mode_hp, src->mode_hp, sizeof(dst->mode_hp));
            dst->app_profile = src->app_profile;
            break;
        case MENU_SCREEN_APP_PROFILE:
            dst->app_profile = src->app_profile;
            break;
        case MENU_SCREEN_BOOT:
            dst->boot_mode = src->boot_mode;
            break;
        case MENU_SCREEN_DISPLAY:
            dst->rotation = src->rotation;
            screen_copy_part(&dst->screen, &src->screen, false);
            break;
        case MENU_SCREEN_SLEEP:
            screen_copy_part(&dst->screen, &src->screen, true);
            break;
        case MENU_SCREEN_BINDINGS:
            dst->host = src->host;
            break;
        case MENU_SCREEN_LIGHTS:
            dst->lights = src->lights;
            break;
        case MENU_SCREEN_CLICK:
            dst->click_axis = src->click_axis;
            break;
        default:
            break;
    }
}

static bool settings_group_differs(const settings_t *a, const settings_t *b, menu_screen_id_t group) {
    switch (group) {
        case MENU_SCREEN_HAPTIC:
            // Which profile the screen shows (hp_edit) isn't a change to save.
            return memcmp(a->hp_feel, b->hp_feel, sizeof(a->hp_feel)) != 0
                || memcmp(a->hp_tune, b->hp_tune, sizeof(a->hp_tune)) != 0
                || memcmp(a->hp_click, b->hp_click, sizeof(a->hp_click)) != 0;
        case MENU_SCREEN_HID:
            return a->hid_type != b->hid_type || a->midi_channel != b->midi_channel || a->midi_synth != b->midi_synth
                || a->app_profile != b->app_profile || memcmp(a->mode_hp, b->mode_hp, sizeof(a->mode_hp)) != 0;
        case MENU_SCREEN_APP_PROFILE:
            return a->app_profile != b->app_profile;
        case MENU_SCREEN_BOOT:
            return a->boot_mode != b->boot_mode;
        case MENU_SCREEN_DISPLAY:
            return a->rotation != b->rotation || screen_part_differs(&a->screen, &b->screen, false);
        case MENU_SCREEN_SLEEP:
            return screen_part_differs(&a->screen, &b->screen, true);
        case MENU_SCREEN_BINDINGS:
            return a->host != b->host;
        case MENU_SCREEN_LIGHTS:
            return memcmp(&a->lights, &b->lights, sizeof(a->lights)) != 0;
        case MENU_SCREEN_CLICK:
            return a->click_axis != b->click_axis;
        default:
            return false;
    }
}

// What F2 on a screen saves (and what its "F2 SAVE" hint tracks). The profile screen saves
// the whole HID group -- choosing a profile there commits the APP choice with it -- while its
// F3 only reverts the profile itself (settings_copy_group by the screen's own id).
static menu_screen_id_t save_group(menu_screen_id_t id) {
    return id == MENU_SCREEN_APP_PROFILE ? MENU_SCREEN_HID : id;
}

// Puts a direct screen's fields back to their saved values (leaving without F2). Core 0,
// caller holds s_state_mux (s_saved is read here).
static void revert_group_locked(menu_screen_id_t group) {
    static settings_t cur; // Core 0 only, under the spinlock; static: settings_t is ~400 B now
    settings_capture(&cur);
    settings_copy_group(&cur, &s_saved, group);
    settings_restore(&cur);
}

// One NVS blob holds both screens' fields: write this screen's live part over the other's saved
// one, so saving DISPLAY never stores an unsaved SLEEP change (or the other way round). Core 1.
static void save_screen_part(bool sleep_part) {
    screen_t live, out;
    screen_get(&live);
    portENTER_CRITICAL(&s_state_mux);
    out = s_saved.screen;
    portEXIT_CRITICAL(&s_state_mux);
    screen_copy_part(&out, &live, sleep_part);
    screen_store(&out);
}

// F2's NVS commit, on Core 1. It used to run inline in menu_input_save(), i.e. inside a
// control-loop tick on Core 0: SYS INFO showed ~11 ms of INPUT for one save. Any flash write
// still briefly stalls both cores (the cache is off while the chip programs), but NVS's own
// page search, hashing and bookkeeping no longer run on Core 0.
#define SAVE_QUEUE_LEN 4
typedef struct {
    void (*save)(void);
    menu_screen_id_t group;
} save_job_t;
static QueueHandle_t s_save_queue;

static void save_task_fn(void *arg) {
    save_job_t job;
    while (1) {
        xQueueReceive(s_save_queue, &job, portMAX_DELAY);
        static settings_t cur; // this task only
        settings_capture(&cur);
        job.save();
        portENTER_CRITICAL(&s_state_mux);
        settings_copy_group(&s_saved, &cur, job.group);
        portEXIT_CRITICAL(&s_state_mux);
        atomic_fetch_add_explicit(&s_save_count, 1, memory_order_relaxed);
        motor_sound_jingle(MOTOR_SOUND_JINGLE_SAVE);
    }
}

// SOUND CAL stored the axis itself (motor_sound.c, Core 1): that is the saved one now, or
// DEVICE -> CLICK would show it as an unsaved change and put the old one back on leaving.
void menu_click_saved(void) {
    static settings_t cur; // the sndcal task only
    settings_capture(&cur);
    portENTER_CRITICAL(&s_state_mux);
    settings_copy_group(&s_saved, &cur, MENU_SCREEN_CLICK);
    portEXIT_CRITICAL(&s_state_mux);
}

void menu_init(void) {
    user_prefs_init(); // LIGHTS joins the saved baseline captured at the end of this function

    portENTER_CRITICAL(&s_state_mux);
    s_stack_depth = 0;
    s_editing = false;
    portEXIT_CRITICAL(&s_state_mux);

    // Phase 8 step 2: restore persisted settings over the compiled-in defaults above, if a
    // valid save exists (see config_store.c). No lock needed here -- menu_init() runs once
    // from app_main() before control_task_start()/display_task_start(), i.e. before any other
    // reader or writer of these atomics exists yet.
    // Haptic profiles: factory values first, then whatever was saved, each value pulled into
    // its profile's limits (hp_set_tune) so a stale or damaged save can't leave the safe range.
    for (int p = 0; p < HAPTIC_PROFILE_COUNT; p++) {
        s_hp_detents[p] = HAPTIC_PROFILES[p].detents;
        hp_factory(p);
    }
    for (int i = 0; i < MENU_HID_TYPE_COUNT; i++) ST(s_mode_hp[i], HAPTIC_PROFILE_COARSE);
    static haptic_profiles_cfg_t hcfg; // static: too big for app_main's stack to carry lightly
    // Profiles stored before each had its own click: all start from the one the device had.
    // A knob with nothing stored: each profile's factory click.
    for (int p = 0; p < HAPTIC_PROFILE_COUNT; p++) {
        int legacy = motor_sound_legacy_shape();
        ST(s_hp_click[p], legacy >= 0 ? legacy : HAPTIC_PROFILES[p].click);
    }
    if (config_store_load_haptic_profiles(&hcfg)) {
        ST(s_hp_edit, clampi(hcfg.edit, 0, HAPTIC_PROFILE_COUNT - 1));
        hp_click_unpack(hcfg.click);
        for (int p = 0; p < HAPTIC_PROFILE_COUNT; p++) {
            int feel = hcfg.feel[p];
            if (feel >= 0 && feel < HAPTIC_TYPE_COUNT && (HAPTIC_PROFILES[p].feels & (1u << feel))) ST(s_hp_feel[p], feel);
            for (int f = 0; f < HAPTIC_TYPE_COUNT; f++) {
                const haptic_tune_t *t = &hcfg.tune[p][f];
                if (isfinite(t->kp) && isfinite(t->kd) && isfinite(t->pitch)) hp_set_tune(p, f, t);
            }
        }
    }
    mode_haptic_cfg_t mcfg;
    if (config_store_load_mode_haptic(&mcfg)) {
        for (int i = 0; i < MODE_HAPTIC_STORED; i++) ST(s_mode_hp[i], mcfg.profile[i]);
    }
    ST(s_hp_active, hp_edit());
    hid_cfg_t icfg;
    if (config_store_load_hid(&icfg)) {
        atomic_store_explicit(&s_ph_hid_type, (menu_hid_type_t)icfg.hid_type, memory_order_relaxed);
        atomic_store_explicit(&s_ph_midi_channel, icfg.midi_channel, memory_order_relaxed);
    }
    char app_id[16];
    if (config_store_load_app_profile(app_id, sizeof(app_id))) {
        int idx = app_profiles_find(app_id);
        if (idx >= 0) atomic_store_explicit(&s_ph_app_profile, idx, memory_order_relaxed);
    }
    char synth_id[24];
    if (config_store_load_midi_synth(synth_id, sizeof(synth_id))) {
        int idx = midi_synth_find(synth_id);
        if (idx >= 0) atomic_store_explicit(&s_ph_midi_synth, idx, memory_order_relaxed);
    }
    boot_cfg_t bcfg;
    if (config_store_load_boot(&bcfg)) {
        atomic_store_explicit(&s_ph_boot_mode, (boot_usb_mode_t)bcfg.boot_mode, memory_order_relaxed);
    }
    display_cfg_t dcfg;
    if (config_store_load_display(&dcfg)) {
        atomic_store_explicit(&s_ph_rotation, dcfg.rotation, memory_order_relaxed);
    }
    bind_cfg_t bind;
    if (config_store_load_bindings(&bind)) {
        atomic_store_explicit(&s_ph_host, (menu_host_t)bind.host, memory_order_relaxed);
    }

    s_save_queue = xQueueCreate(SAVE_QUEUE_LEN, sizeof(save_job_t));
    xTaskCreatePinnedToCore(save_task_fn, "menu_save", 4096, NULL, PRIO_STORE, NULL, CORE_IO);

    // Whatever is live now is, by definition, what's saved (or the defaults, if nothing was)
    // -- the baseline for the dirty cue.
    settings_capture(&s_saved);
}


void menu_input_toggle_open(void) {
    portENTER_CRITICAL(&s_state_mux);
    if (s_stack_depth == 0) {
        int idx = first_enabled_index(&s_root_screen);
        s_stack[0].screen = &s_root_screen;
        s_stack[0].selected_index = (idx < 0) ? 0 : idx;
        s_stack_depth = 1;
    } else {
        // Closing from a direct screen discards its unsaved choice; a Haptic edit in
        // progress is kept as-is (live, unsaved), same as confirming it.
        for (int i = s_stack_depth - 1; i >= 0; i--) {
            if (s_stack[i].screen->direct_edit) {
                revert_group_locked(s_stack[i].screen->id);
            }
        }
        s_stack_depth = 0;
        s_editing = false;
    }
    s_armed = false;
    portEXIT_CRITICAL(&s_state_mux);
}

void menu_input_back(void) {
    portENTER_CRITICAL(&s_state_mux);
    if (s_stack_depth == 0) {
        // no-op
    } else if (s_armed) {
        s_armed = false; // cancel the action, stay on the screen
        motor_sound_jingle(MOTOR_SOUND_JINGLE_CANCEL);
    } else if (s_editing) {
        // Cancel: put back what the value was when the edit started.
        settings_restore(&s_undo);
        s_editing = false;
        motor_sound_jingle(MOTOR_SOUND_JINGLE_CANCEL);
    } else {
        const menu_screen_t *top = s_stack[s_stack_depth - 1].screen;
        if (top->direct_edit) {
            revert_group_locked(top->id);
        }
        s_stack_depth = (s_stack_depth > 1) ? s_stack_depth - 1 : 0; // at the top level, back = close
    }
    portEXIT_CRITICAL(&s_state_mux);
}

void menu_input_select(void) {
    portENTER_CRITICAL(&s_state_mux);
    if (s_stack_depth == 0) {
        portEXIT_CRITICAL(&s_state_mux);
        return;
    }
    menu_stack_frame_t *top = &s_stack[s_stack_depth - 1];
    const menu_item_t *it = &top->screen->items[top->selected_index];
    void (*action)(void) = NULL;

    if (it->kind == MENU_ITEM_ACTION) {
        if (s_armed) action = it->action; // the confirming press
        s_armed = !s_armed;
    } else if (it->kind == MENU_ITEM_INFO) {
        action = it->action;
    } else if (top->screen->direct_edit) {
        // Direct screens: F1 moves to the next field (HID type <-> MIDI channel) -- or, when
        // that field is a submenu (APP -> PROFILE), opens it; focus stays on this screen's
        // own field.
        int next = step_index(top->screen, top->selected_index, 1);
        const menu_item_t *nit = &top->screen->items[next];
        if (nit->kind == MENU_ITEM_SUBMENU) {
            if (s_stack_depth < MENU_MAX_DEPTH && nit->submenu != NULL) {
                int idx = first_enabled_index(nit->submenu);
                s_stack[s_stack_depth].screen = nit->submenu;
                s_stack[s_stack_depth].selected_index = (idx < 0) ? 0 : idx;
                s_stack_depth++;
            }
        } else {
            top->selected_index = next;
        }
    } else if (s_editing) {
        s_editing = false; // confirm -- the value is already live
    } else if (it->kind == MENU_ITEM_SUBMENU) {
        if (s_stack_depth < MENU_MAX_DEPTH && it->submenu != NULL) {
            int idx = first_enabled_index(it->submenu);
            s_stack[s_stack_depth].screen = it->submenu;
            s_stack[s_stack_depth].selected_index = (idx < 0) ? 0 : idx;
            s_stack_depth++;
        }
    } else if (it->kind == MENU_ITEM_VALUE && !item_muted(top->screen, top->selected_index)) {
        settings_capture(&s_undo);
        s_editing = true;
    }
    portEXIT_CRITICAL(&s_state_mux);
    if (action != NULL) action(); // outside the spinlock, like F2's save
}

void menu_input_save(void) {
    void (*save)(void) = NULL;
    menu_screen_id_t group = MENU_SCREEN_NONE;

    portENTER_CRITICAL(&s_state_mux);
    if (s_stack_depth > 0) {
        const menu_screen_t *screen = s_stack[s_stack_depth - 1].screen;
        static settings_t cur; // Core 0 only, under the spinlock
        settings_capture(&cur);
        if (screen->save != NULL && settings_group_differs(&cur, &s_saved, save_group(screen->id))) {
            save = screen->save;
            group = save_group(screen->id);
        }
        s_editing = false; // saving also confirms an edit in progress
    }
    portEXIT_CRITICAL(&s_state_mux);

    // The NVS commit (a few ms of flash erase/write) is handed to save_task_fn() on Core 1 --
    // this runs inside a control-loop tick. Skipped entirely when nothing changed, so an idle
    // F2 costs no flash wear. A full queue (F2 hammered) drops the press; the dirty cue stays.
    if (save != NULL) {
        save_job_t job = {save, group};
        xQueueSend(s_save_queue, &job, 0);
    }
}

void CONTROL_HOT menu_input_rotate(int8_t direction) {
    portENTER_CRITICAL(&s_state_mux);
    if (s_stack_depth == 0 || direction == 0) {
        portEXIT_CRITICAL(&s_state_mux);
        return;
    }
    menu_stack_frame_t *top = &s_stack[s_stack_depth - 1];
    s_armed = false; // turning away cancels a pending action
    if (s_editing || top->screen->direct_edit) {
        const menu_item_t *it = &top->screen->items[top->selected_index];
        if (it->on_rotate != NULL && !item_muted(top->screen, top->selected_index)) {
            it->on_rotate(direction); // atomic store(s) only -- see field callbacks above
        }
        // The edited row can mute itself' neighbours but never itself, except from outside
        // (the companion changing FEEL): then the focus moves on.
        if (!top->screen->direct_edit && item_muted(top->screen, top->selected_index)) {
            s_editing = false;
            top->selected_index = step_index(top->screen, top->selected_index, direction);
        }
    } else {
        top->selected_index = step_index(top->screen, top->selected_index, direction);
    }
    portEXIT_CRITICAL(&s_state_mux);
}

bool CONTROL_HOT menu_at_end(int8_t direction) {
    bool end = false;
    portENTER_CRITICAL(&s_state_mux);
    if (s_stack_depth > 0) {
        const menu_stack_frame_t *top = &s_stack[s_stack_depth - 1];
        const menu_item_t *it = &top->screen->items[top->selected_index];
        if ((s_editing || top->screen->direct_edit) && it->at_end != NULL) end = it->at_end(direction);
    }
    portEXIT_CRITICAL(&s_state_mux);
    return end;
}

bool CONTROL_HOT menu_take_recalibrate_request(void) {
    // Cheap load first: this runs every tick of control_task.c's loop.
    if (!atomic_load_explicit(&s_recal_request, memory_order_relaxed)) return false;
    return atomic_exchange_explicit(&s_recal_request, false, memory_order_relaxed);
}

menu_screen_id_t menu_current_screen(void) {
    portENTER_CRITICAL(&s_state_mux);
    menu_screen_id_t id = s_stack_depth > 0 ? s_stack[s_stack_depth - 1].screen->id : MENU_SCREEN_NONE;
    portEXIT_CRITICAL(&s_state_mux);
    return id;
}

bool CONTROL_HOT menu_is_open(void) {
    portENTER_CRITICAL(&s_state_mux);
    bool open = (s_stack_depth > 0);
    portEXIT_CRITICAL(&s_state_mux);
    return open;
}

// Core 1 only. Takes a quick, bounded copy of the raw navigation state under the same
// spinlock Core 0 uses (held only long enough to copy a few words), then does all string
// formatting outside any lock -- safe to be as slow as it likes here, this task has no
// real-time deadline to violate.
void menu_get_render_snapshot(menu_render_snapshot_t *out) {
    menu_stack_frame_t top_copy = { 0 };
    int depth_copy;
    bool editing_copy, armed_copy;
    static settings_t saved_copy, cur; // the display task only

    portENTER_CRITICAL(&s_state_mux);
    depth_copy = s_stack_depth;
    editing_copy = s_editing;
    armed_copy = s_armed;
    if (depth_copy > 0) {
        top_copy = s_stack[depth_copy - 1];
    }
    saved_copy = s_saved;
    portEXIT_CRITICAL(&s_state_mux);

    menu_render_snapshot_t snap = { 0 };
    snap.open = (depth_copy > 0);
    snap.editing = editing_copy;
    snap.selected = -1;
    snap.screen = MENU_SCREEN_NONE;
    snap.save_count = atomic_load_explicit(&s_save_count, memory_order_relaxed);
    snap.reset_count = atomic_load_explicit(&s_reset_count, memory_order_relaxed);

    if (snap.open) {
        const menu_screen_t *screen = top_copy.screen;
        snap.screen = screen->id;
        snprintf(snap.title, sizeof(snap.title), "%s", screen->title);

        settings_capture(&cur);
        snap.dirty = settings_group_differs(&cur, &saved_copy, save_group(screen->id));

        int row = 0;
        for (int i = 0; i < screen->item_count && row < MENU_MAX_VISIBLE_ITEMS; i++) {
            if (!item_enabled(screen, i)) {
                continue; // disabled items are skipped entirely, not shown greyed-out
            }
            const menu_item_t *it = &screen->items[i];
            menu_render_row_t *r = &snap.rows[row];
            snprintf(r->label, sizeof(r->label), "%s", it->label);
            snprintf(r->caption, sizeof(r->caption), "%s", it->caption ? it->caption : "");
            r->muted = item_muted(screen, i);
            if (r->muted && it->is_muted != feel_muted) { // a one-feel profile still names its feel
                snprintf(r->value, sizeof(r->value), "--");
            } else if (it->kind == MENU_ITEM_VALUE && it->format_value != NULL) {
                it->format_value(r->value, sizeof(r->value));
            } else if (it->kind == MENU_ITEM_ACTION) {
                bool armed = armed_copy && i == top_copy.selected_index;
                snprintf(r->value, sizeof(r->value), "%s", armed ? MENU_RECAL_ARMED : "");
            } else {
                r->value[0] = '\0'; // submenu rows have nothing to show on the right
            }
            r->selected = (i == top_copy.selected_index);
            if (r->selected) {
                snap.selected = row;
            }
            row++;
        }
        snap.row_count = row;
    }

    *out = snap;
}

// Phase 8 step 3 -- see menu.h's declaration comment. Called from Core 0 (control_task.c's
// real-time haptic loop) on the same atomics rotate_kp()/rotate_kd()/rotate_detents() above
// already write from Core 0's menu_input_rotate(), and format_value() already reads from
// Core 1's menu_get_render_snapshot() -- one more atomic reader needs no new synchronization,
// same lock-free convention this file already relies on for all three.
// All of these describe the ACTIVE profile -- the one the control loop is running.
uint32_t CONTROL_HOT menu_get_haptic_num_detents(void) {
    return s_hp_detents[LD(s_hp_active)];
}

float CONTROL_HOT menu_get_haptic_kp(void) {
    int p = LD(s_hp_active);
    return LD(s_hp_kp[p][hp_feel(p)]);
}

float CONTROL_HOT menu_get_haptic_kd(void) {
    int p = LD(s_hp_active);
    return LD(s_hp_kd[p][hp_feel(p)]);
}

float CONTROL_HOT menu_get_haptic_shape(void) {
    int p = LD(s_hp_active);
    return (float)LD(s_hp_shape[p][hp_feel(p)]) * 0.01f;
}

haptic_type_t CONTROL_HOT menu_get_haptic_type(void) {
    return (haptic_type_t)hp_feel(LD(s_hp_active));
}

void CONTROL_HOT menu_haptic_set_active(int profile) {
    if (profile < 0 || profile >= HAPTIC_PROFILE_COUNT) profile = HAPTIC_PROFILE_COARSE;
    ST(s_hp_active, profile);
}

int CONTROL_HOT menu_haptic_profile(void) {
    if (menu_is_open()) {
        // The menu has a feel of its own, MEDIUM as saved, so it is the same whatever mode
        // it was opened from. One exception, to tune by feel: the Haptics screen runs on
        // the profile it shows, around the ring and while a value is edited. Not while
        // STEPS is edited: turning there picks the profile, and taking each on as it passed
        // changed the step size under the hand (COARSE to FINE overshot). The one picked
        // takes over when the edit is confirmed.
        int p = HAPTIC_PROFILE_MEDIUM;
        portENTER_CRITICAL(&s_state_mux);
        if (s_stack_depth > 0) {
            const menu_stack_frame_t *top = &s_stack[s_stack_depth - 1];
            if (top->screen->id == MENU_SCREEN_HAPTIC && !(s_editing && top->selected_index == MENU_HAPTIC_ROW_STEPS)) p = hp_edit();
        }
        portEXIT_CRITICAL(&s_state_mux);
        return p;
    }
    return (int)LD(s_mode_hp[atomic_load_explicit(&s_ph_hid_type, memory_order_relaxed)]);
}

// haptic_profile_for() from RAM, for the control loop (the table itself is in flash).
int CONTROL_HOT menu_haptic_for(haptic_type_t feel, unsigned detents) {
    if (feel == HAPTIC_TYPE_VISCOSE) return HAPTIC_PROFILE_SMOOTH;
    if (detents == 0) return -1;
    int best = 0;
    unsigned best_d = ~0u;
    for (int i = 0; i < HAPTIC_PROFILE_STEPPED_COUNT; i++) {
        unsigned p = s_hp_detents[i];
        unsigned dist = p > detents ? p - detents : detents - p;
        if (dist < best_d) {
            best_d = dist;
            best = i;
        }
    }
    return best;
}

int menu_haptic_edit_profile(void) {
    return hp_edit();
}

int CONTROL_HOT menu_get_click_shape(void) {
    return (int)LD(s_hp_click[LD(s_hp_active)]);
}

int menu_get_edit_click_shape(void) {
    return (int)LD(s_hp_click[hp_edit()]);
}

float CONTROL_HOT menu_get_haptic_pitch(void) {
    int p = LD(s_hp_active);
    return LD(s_hp_pitch[p][hp_feel(p)]);
}

float CONTROL_HOT menu_get_click_amplitude(void) {
    int p = LD(s_hp_active);
    return LD(s_hp_amp[p][hp_feel(p)]) / 100.0f;
}

// F2 held on the Haptics screen: the shown profile back to its factory feel and values.
// Live, like any edit; a normal F2 then saves it. Core 0.
void menu_input_reset_haptic(void) {
    bool on_haptic;
    portENTER_CRITICAL(&s_state_mux);
    on_haptic = s_stack_depth > 0 && s_stack[s_stack_depth - 1].screen->id == MENU_SCREEN_HAPTIC;
    if (on_haptic) s_editing = false;
    portEXIT_CRITICAL(&s_state_mux);
    if (!on_haptic) return;
    hp_factory(hp_edit());
    atomic_fetch_add_explicit(&s_reset_count, 1, memory_order_relaxed);
}

void menu_remote_reset_haptic(void) {
    hp_factory(hp_edit());
    atomic_fetch_add_explicit(&s_reset_count, 1, memory_order_relaxed);
}

boot_usb_mode_t menu_get_boot_mode(void) {
    return atomic_load_explicit(&s_ph_boot_mode, memory_order_relaxed);
}

int32_t CONTROL_HOT menu_get_app_profile(void) {
    return atomic_load_explicit(&s_ph_app_profile, memory_order_relaxed);
}

static int32_t after_removal(int32_t v, int index) {
    return v > index ? v - 1 : v == index ? 0 : v;
}

void menu_profile_removed(int index) {
    atomic_store(&s_ph_app_profile, after_removal(atomic_load(&s_ph_app_profile), index));
    portENTER_CRITICAL(&s_state_mux);
    s_saved.app_profile = after_removal(s_saved.app_profile, index);
    s_undo.app_profile = after_removal(s_undo.app_profile, index);
    portEXIT_CRITICAL(&s_state_mux);
}

void menu_midi_synth_removed(int index) {
    atomic_store(&s_ph_midi_synth, after_removal(atomic_load(&s_ph_midi_synth), index));
    portENTER_CRITICAL(&s_state_mux);
    s_saved.midi_synth = after_removal(s_saved.midi_synth, index);
    s_undo.midi_synth = after_removal(s_undo.midi_synth, index);
    portEXIT_CRITICAL(&s_state_mux);
}

int32_t menu_get_display_rotation(void) {
    return atomic_load_explicit(&s_ph_rotation, memory_order_relaxed);
}

menu_host_t menu_get_host(void) {
    return atomic_load_explicit(&s_ph_host, memory_order_relaxed);
}

menu_hid_type_t CONTROL_HOT menu_get_hid_type(void) {
    return atomic_load_explicit(&s_ph_hid_type, memory_order_relaxed);
}

int32_t menu_get_midi_channel(void) { return atomic_load_explicit(&s_ph_midi_channel, memory_order_relaxed); }
int32_t menu_get_midi_synth(void) { return atomic_load_explicit(&s_ph_midi_synth, memory_order_relaxed); }

// --- Companion app (host_link.c, Core 1) ---
// Same atomics, same clamps as the rotate_*() callbacks; s_saved only under s_state_mux, like
// everywhere else in this file.

void menu_remote_get(menu_remote_settings_t *out) {
    static settings_t cur, saved; // Core 1 only (host_link.c); static keeps them off its stack
    settings_capture(&cur);
    portENTER_CRITICAL(&s_state_mux);
    saved = s_saved;
    portEXIT_CRITICAL(&s_state_mux);

    // The haptic values are those of the profile the Haptics screen shows, in its feel.
    int p = cur.hp_edit, f = cur.hp_feel[p];
    const haptic_tune_t *t = &cur.hp_tune[p][f], *st = &saved.hp_tune[p][f];
    const haptic_limits_t *l = &HAPTIC_PROFILES[p].lim[f];
    out->haptic_profile = p;
    out->detents = HAPTIC_PROFILES[p].detents;
    out->feels = HAPTIC_PROFILES[p].feels;
    out->kp = t->kp;
    out->kd = t->kd;
    out->shape = t->shape;
    out->feel = f;
    out->amp = t->amp;
    out->pitch = t->pitch;
    out->click = cur.hp_click[p];
    out->kp_min = l->kp_min;
    out->kp_max = l->kp_max;
    out->kd_min = l->kd_min;
    out->kd_max = l->kd_max;
    out->amp_max = l->amp_max;
    out->pitch_min = l->pitch_min;
    out->pitch_max = l->pitch_max;
    out->hid_type = cur.hid_type;
    out->mode_haptic = cur.mode_hp[cur.hid_type];
    out->midi_channel = cur.midi_channel;
    out->midi_synth = cur.midi_synth;
    out->profile = cur.app_profile;
    out->boot_mode = cur.boot_mode;
    out->rotation = cur.rotation;
    out->host = cur.host;

    uint32_t d = 0;
    if (t->kp != st->kp) d |= 1u << HOST_SET_KP;
    if (t->kd != st->kd) d |= 1u << HOST_SET_KD;
    if (t->shape != st->shape) d |= 1u << HOST_SET_SHAPE;
    if (f != saved.hp_feel[p]) d |= 1u << HOST_SET_FEEL;
    if (t->amp != st->amp) d |= 1u << HOST_SET_AMP;
    if (t->pitch != st->pitch) d |= 1u << HOST_SET_PITCH;
    if (cur.hp_click[p] != saved.hp_click[p]) d |= 1u << HOST_SET_CLICK;
    // Unsaved changes in the other profiles (or this one's other feels) show on the profile.
    for (int q = 0; q < HAPTIC_PROFILE_COUNT; q++) {
        for (int g = 0; g < HAPTIC_TYPE_COUNT; g++) {
            if ((q != p || g != f) && memcmp(&cur.hp_tune[q][g], &saved.hp_tune[q][g], sizeof(haptic_tune_t)) != 0) {
                d |= 1u << HOST_SET_HAPTIC_PROFILE;
            }
        }
        if (q != p && (cur.hp_feel[q] != saved.hp_feel[q] || cur.hp_click[q] != saved.hp_click[q])) d |= 1u << HOST_SET_HAPTIC_PROFILE;
    }
    if (cur.hid_type != saved.hid_type) d |= 1u << HOST_SET_HID_TYPE;
    if (memcmp(cur.mode_hp, saved.mode_hp, sizeof(cur.mode_hp)) != 0) d |= 1u << HOST_SET_MODE_HAPTIC;
    if (cur.midi_channel != saved.midi_channel) d |= 1u << HOST_SET_MIDI_CH;
    if (cur.midi_synth != saved.midi_synth) d |= 1u << HOST_SET_MIDI_SYNTH;
    if (cur.app_profile != saved.app_profile) d |= 1u << HOST_SET_PROFILE;
    if (cur.boot_mode != saved.boot_mode) d |= 1u << HOST_SET_BOOT;
    if (cur.rotation != saved.rotation) d |= 1u << HOST_SET_ROTATION;
    if (cur.host != saved.host) d |= 1u << HOST_SET_HOST;
    out->dirty = d;
}

bool menu_remote_set(int id, int32_t ival, float fval) {
    int p = hp_edit(), f = hp_feel(p);
    const haptic_limits_t *l = &HAPTIC_PROFILES[p].lim[f];
    switch (id) {
        case HOST_SET_DETENTS: // from before haptic profiles: a count picks the nearest stepped one
            ST(s_hp_edit, haptic_profile_nearest(ival < 0 ? 0 : (unsigned)ival));
            break;
        case HOST_SET_HAPTIC_PROFILE: ST(s_hp_edit, clampi(ival, 0, HAPTIC_PROFILE_COUNT - 1)); break;
        case HOST_SET_KP: ST(s_hp_kp[p][f], clampf(fval, l->kp_min, l->kp_max)); break;
        case HOST_SET_KD: ST(s_hp_kd[p][f], clampf(fval, l->kd_min, l->kd_max)); break;
        case HOST_SET_SHAPE: ST(s_hp_shape[p][f], clampi(ival, HAPTIC_SHAPE_MIN, HAPTIC_SHAPE_MAX)); break;
        case HOST_SET_FEEL:
            if (ival < 0 || ival >= HAPTIC_TYPE_COUNT || !(HAPTIC_PROFILES[p].feels & (1u << ival))) return false;
            ST(s_hp_feel[p], ival);
            break;
        case HOST_SET_AMP: ST(s_hp_amp[p][f], clampi(ival, HAPTIC_CLICK_AMP_MIN, l->amp_max)); break;
        case HOST_SET_PITCH: ST(s_hp_pitch[p][f], clampf(fval, l->pitch_min, l->pitch_max)); break;
        case HOST_SET_MODE_HAPTIC:
            ST(s_mode_hp[atomic_load(&s_ph_hid_type)], clampi(ival, 0, HAPTIC_PROFILE_COUNT - 1));
            break;
        case HOST_SET_CLICK: ST(s_hp_click[p], clampi(ival, 0, SNDCAL_CLICK_COUNT - 1)); break;
        case HOST_SET_HID_TYPE:
            atomic_store(&s_ph_hid_type, (menu_hid_type_t)clampi(ival, 0, MENU_HID_TYPE_COUNT - 1));
            break;
        case HOST_SET_MIDI_CH: atomic_store(&s_ph_midi_channel, clampi(ival, 1, 16)); break;
        case HOST_SET_MIDI_SYNTH: atomic_store(&s_ph_midi_synth, clampi(ival, 0, midi_synth_count() - 1)); break;
        case HOST_SET_PROFILE: atomic_store(&s_ph_app_profile, clampi(ival, 0, app_profiles_count() - 1)); break;
        case HOST_SET_BOOT:
            atomic_store(&s_ph_boot_mode, (boot_usb_mode_t)clampi(ival, 0, BOOT_USB_MODE_COUNT - 1));
            break;
        case HOST_SET_ROTATION: atomic_store(&s_ph_rotation, clampi(ival, 0, MENU_DISPLAY_ROTATIONS - 1)); break;
        case HOST_SET_HOST: atomic_store(&s_ph_host, (menu_host_t)clampi(ival, 0, MENU_HOST_COUNT - 1)); break;
        default: return false;
    }
    return true;
}

// Each group with its NVS writer -- what F2 on that group's screen saves.
static const struct {
    menu_screen_id_t group;
    void (*save)(void);
} s_remote_groups[] = {
    {MENU_SCREEN_HAPTIC, action_save_haptic},   {MENU_SCREEN_HID, action_save_hid},
    {MENU_SCREEN_BOOT, action_save_boot},       {MENU_SCREEN_DISPLAY, action_save_display},
    {MENU_SCREEN_BINDINGS, action_save_bindings}, {MENU_SCREEN_LIGHTS, action_save_lights},
    {MENU_SCREEN_CLICK, action_save_click},     {MENU_SCREEN_SLEEP, action_save_sleep},
};

void menu_remote_save(void) {
    bool any = false;
    for (size_t i = 0; i < sizeof(s_remote_groups) / sizeof(s_remote_groups[0]); i++) {
        static settings_t cur, saved; // Core 1 only
        settings_capture(&cur);
        portENTER_CRITICAL(&s_state_mux);
        saved = s_saved;
        portEXIT_CRITICAL(&s_state_mux);
        if (!settings_group_differs(&cur, &saved, s_remote_groups[i].group)) continue;
        s_remote_groups[i].save(); // NVS, outside the spinlock
        portENTER_CRITICAL(&s_state_mux);
        settings_copy_group(&s_saved, &cur, s_remote_groups[i].group);
        portEXIT_CRITICAL(&s_state_mux);
        any = true;
    }
    if (any) {
        atomic_fetch_add_explicit(&s_save_count, 1, memory_order_relaxed); // the SAVED! toast
        motor_sound_jingle(MOTOR_SOUND_JINGLE_SAVE);
    }
}

void menu_remote_revert(void) {
    static settings_t saved; // Core 1 only
    portENTER_CRITICAL(&s_state_mux);
    saved = s_saved;
    s_editing = false;
    portEXIT_CRITICAL(&s_state_mux);
    settings_restore(&saved);
}

bool menu_lights_dirty(void) {
    lights_t cur;
    lights_get(&cur);
    portENTER_CRITICAL(&s_state_mux);
    bool dirty = memcmp(&cur, &s_saved.lights, sizeof(cur)) != 0;
    portEXIT_CRITICAL(&s_state_mux);
    return dirty;
}

void menu_remote_save_lights(void) {
    lights_t cur;
    lights_get(&cur);
    if (!menu_lights_dirty()) return;
    if (!lights_save()) { // NVS failed: still unsaved, and the screen keeps saying so
        motor_sound_jingle(MOTOR_SOUND_JINGLE_CANCEL);
        return;
    }
    portENTER_CRITICAL(&s_state_mux);
    s_saved.lights = cur;
    portEXIT_CRITICAL(&s_state_mux);
    atomic_fetch_add_explicit(&s_save_count, 1, memory_order_relaxed);
    motor_sound_jingle(MOTOR_SOUND_JINGLE_SAVE);
}

bool menu_screen_dirty(void) {
    screen_t cur;
    screen_get(&cur);
    portENTER_CRITICAL(&s_state_mux);
    bool dirty = memcmp(&cur, &s_saved.screen, sizeof(cur)) != 0;
    portEXIT_CRITICAL(&s_state_mux);
    return dirty;
}

void menu_remote_save_screen(void) {
    screen_t cur;
    screen_get(&cur);
    if (!menu_screen_dirty()) return;
    if (!screen_store(&cur)) { // NVS failed: still unsaved
        motor_sound_jingle(MOTOR_SOUND_JINGLE_CANCEL);
        return;
    }
    portENTER_CRITICAL(&s_state_mux);
    s_saved.screen = cur;
    portEXIT_CRITICAL(&s_state_mux);
    atomic_fetch_add_explicit(&s_save_count, 1, memory_order_relaxed);
    motor_sound_jingle(MOTOR_SOUND_JINGLE_SAVE);
}
