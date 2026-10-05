#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "haptic_params.h"

// Phase 8 step 2: NVS-backed persistence for the three settings groups exposed by menu.c,
// following foc_calibration.c's existing load/save pattern (one blob per namespace, sanity-
// checked on load so corrupted/partial NVS data is never trusted over a safe default).
// Scaffolding only -- menu.c's placeholder atomics are the only thing wired to this so far
// (loaded once at boot in menu_init(), saved on each screen's "Save" action). Nothing in
// control_task.c/usb_task.c/main.c reads any of this yet -- that real wiring is Phase 8
// steps 3/6/7, tracked separately in DEVELOPMENT_PLAN.md.

// The haptic profiles (haptic_params.h): which feel each one uses and its tuning per feel.
// Size and version are checked on load; menu.c clamps every value into the profile's limits.
#define HAPTIC_PROFILES_CFG_VERSION 1
typedef struct {
    uint32_t version;
    int32_t edit;  // the profile last shown on the Haptics screen
    int32_t sound; // audio_trigger.h's audio_click_timbre_t
    int32_t feel[HAPTIC_PROFILE_COUNT];
    haptic_tune_t tune[HAPTIC_PROFILE_COUNT][HAPTIC_TYPE_COUNT];
} haptic_profiles_cfg_t;

// Which haptic profile each HID type uses, indexed by menu.h's menu_hid_type_t -- the first four
// only: HOME (4) has no setting of its own, and the blob kept its size (menu.c MODE_HAPTIC_STORED).
typedef struct {
    int32_t profile[4];
} mode_haptic_cfg_t;

typedef struct {
    int32_t hid_type;     // menu.h's menu_hid_type_t
    int32_t midi_channel; // 1-16
} hid_cfg_t;

typedef struct {
    int32_t boot_mode; // boot_mode.h's boot_usb_mode_t, stored as a plain int
} boot_cfg_t;

typedef struct {
    int32_t rotation; // quarter turns clockwise, 0-3
} display_cfg_t;

typedef struct {
    int32_t host; // menu.h's menu_host_t: 0 MAC, 1 PC
} bind_cfg_t;

// Each returns false (leaving *out untouched) if the namespace doesn't exist yet (normal on
// first boot) or the stored blob fails a basic sanity check (wrong size, non-finite float,
// enum field out of range) -- callers should keep their own compiled-in default in that case,
// same as foc_calibration_load()'s caller does. Assumes nvs_flash_init() has already been
// called (done once in app_main(), before menu_init()).
bool config_store_load_haptic_profiles(haptic_profiles_cfg_t *out);
bool config_store_load_mode_haptic(mode_haptic_cfg_t *out);
bool config_store_load_hid(hid_cfg_t *out);
bool config_store_load_boot(boot_cfg_t *out);
bool config_store_load_display(display_cfg_t *out);
bool config_store_load_bindings(bind_cfg_t *out);

void config_store_save_haptic_profiles(const haptic_profiles_cfg_t *cfg);
void config_store_save_mode_haptic(const mode_haptic_cfg_t *cfg);
void config_store_save_hid(const hid_cfg_t *cfg);
void config_store_save_boot(const boot_cfg_t *cfg);
void config_store_save_display(const display_cfg_t *cfg);
void config_store_save_bindings(const bind_cfg_t *cfg);

// APP mode's profile, stored by its id string ("figma") next to hid_cfg -- a separate key
// rather than a new hid_cfg field, so blobs saved before profiles existed still load. The
// id (not an index) keeps a saved choice valid when profiles are added or reordered.
bool config_store_load_app_profile(char *id, size_t len);
void config_store_save_app_profile(const char *id);
// MIDI mode's synth profile (midi.h), by its id ("minilogue-xd"), next to the app profile.
bool config_store_load_midi_synth(char *id, size_t len);
void config_store_save_midi_synth(const char *id);
