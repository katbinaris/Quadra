#include "config_store.h"
#include "haptic_params.h"
#include "audio_trigger.h"
#include "boot_mode.h"
#include "esp_log.h"
#include "nvs.h"
#include <math.h>

static const char *TAG = "config_store";

#define NS_HAPTIC "hprof_cfg" // "haptic_cfg" held the single global tuning before haptic profiles
#define NS_MODE_HAPTIC "hmode_cfg"
#define NS_HID    "hid_cfg"
#define NS_BOOT   "boot_cfg"
#define NS_DISPLAY "disp_cfg"
#define NS_BINDINGS "bind_cfg"
#define KEY       "cfg" // one blob per namespace, same shape as foc_calibration.c's "foc_cal"/"cal"

// HAPTIC_TYPE_COUNT (haptic_params.h), AUDIO_TIMBRE_COUNT (audio_trigger.h), and now
// BOOT_USB_MODE_COUNT (boot_mode.h) all come from their own real, shared enums (Phase 8
// steps 4/5/6). ph_hid_type_t still doesn't have one of its own (stays menu.c-private), so
// HID_TYPE_COUNT below is still kept in sync by hand. A mismatch there would only make the
// load-time range check too loose or too strict, not silently corrupt anything: menu.c's own
// rotate_*() callbacks already clamp every value they ever produce, so this check only
// guards against corrupted flash.
#define HID_TYPE_COUNT 5 // KEYBOARD, MOUSE, MIDI, APP, HOME
#define HOST_COUNT 2     // menu.h's menu_host_t: MAC, PC

static bool nvs_load_blob(const char *ns, void *out, size_t size) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return false; // namespace doesn't exist yet -- normal on first-ever boot
    }
    size_t len = size;
    esp_err_t err = nvs_get_blob(h, KEY, out, &len);
    nvs_close(h);
    return (err == ESP_OK && len == size);
}

static void nvs_save_blob(const char *ns, const void *data, size_t size, const char *label) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open (write, %s) failed: %s", label, esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, KEY, data, size);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save %s to NVS: %s", label, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "%s saved to NVS", label);
    }
}

bool config_store_load_haptic_profiles(haptic_profiles_cfg_t *out) {
    haptic_profiles_cfg_t stored;
    if (!nvs_load_blob(NS_HAPTIC, &stored, sizeof(stored)) || stored.version != HAPTIC_PROFILES_CFG_VERSION) {
        return false;
    }
    *out = stored;
    ESP_LOGI(TAG, "loaded haptic profiles from NVS");
    return true;
}

void config_store_save_haptic_profiles(const haptic_profiles_cfg_t *cfg) {
    nvs_save_blob(NS_HAPTIC, cfg, sizeof(*cfg), "haptic profiles");
}

bool config_store_load_mode_haptic(mode_haptic_cfg_t *out) {
    mode_haptic_cfg_t stored;
    if (!nvs_load_blob(NS_MODE_HAPTIC, &stored, sizeof(stored))) {
        return false;
    }
    for (int i = 0; i < HID_TYPE_COUNT; i++) {
        if (stored.profile[i] < 0 || stored.profile[i] >= HAPTIC_PROFILE_COUNT) return false;
    }
    *out = stored;
    return true;
}

void config_store_save_mode_haptic(const mode_haptic_cfg_t *cfg) {
    nvs_save_blob(NS_MODE_HAPTIC, cfg, sizeof(*cfg), "mode haptic profiles");
}

bool config_store_load_hid(hid_cfg_t *out) {
    hid_cfg_t stored;
    if (!nvs_load_blob(NS_HID, &stored, sizeof(stored))) {
        return false;
    }
    if (stored.hid_type < 0 || stored.hid_type >= HID_TYPE_COUNT ||
        stored.midi_channel < 1 || stored.midi_channel > 16) {
        ESP_LOGW(TAG, "stored hid_cfg failed sanity check, ignoring");
        return false;
    }
    *out = stored;
    ESP_LOGI(TAG, "loaded hid_cfg from NVS: hid_type=%ld midi_channel=%ld",
             (long)stored.hid_type, (long)stored.midi_channel);
    return true;
}

void config_store_save_hid(const hid_cfg_t *cfg) {
    nvs_save_blob(NS_HID, cfg, sizeof(*cfg), "hid_cfg");
}

#define KEY_APP_PROFILE "app"

bool config_store_load_app_profile(char *id, size_t len) {
    nvs_handle_t h;
    if (nvs_open(NS_HID, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = len;
    esp_err_t err = nvs_get_str(h, KEY_APP_PROFILE, id, &n);
    nvs_close(h);
    if (err != ESP_OK) return false;
    ESP_LOGI(TAG, "loaded app profile from NVS: %s", id);
    return true;
}

void config_store_save_app_profile(const char *id) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_HID, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_APP_PROFILE, id);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save app profile to NVS: %s", esp_err_to_name(err));
    }
}

bool config_store_load_boot(boot_cfg_t *out) {
    boot_cfg_t stored;
    if (!nvs_load_blob(NS_BOOT, &stored, sizeof(stored))) {
        return false;
    }
    if (stored.boot_mode < 0 || stored.boot_mode >= BOOT_USB_MODE_COUNT) {
        ESP_LOGW(TAG, "stored boot_cfg failed sanity check, ignoring");
        return false;
    }
    *out = stored;
    ESP_LOGI(TAG, "loaded boot_cfg from NVS: boot_mode=%ld", (long)stored.boot_mode);
    return true;
}

void config_store_save_boot(const boot_cfg_t *cfg) {
    nvs_save_blob(NS_BOOT, cfg, sizeof(*cfg), "boot_cfg");
}

bool config_store_load_display(display_cfg_t *out) {
    display_cfg_t stored;
    if (!nvs_load_blob(NS_DISPLAY, &stored, sizeof(stored))) {
        return false;
    }
    if (stored.rotation < 0 || stored.rotation > 3) {
        ESP_LOGW(TAG, "stored disp_cfg failed sanity check, ignoring");
        return false;
    }
    *out = stored;
    ESP_LOGI(TAG, "loaded disp_cfg from NVS: rotation=%ld", (long)stored.rotation);
    return true;
}

void config_store_save_display(const display_cfg_t *cfg) {
    nvs_save_blob(NS_DISPLAY, cfg, sizeof(*cfg), "disp_cfg");
}

bool config_store_load_bindings(bind_cfg_t *out) {
    bind_cfg_t stored;
    if (!nvs_load_blob(NS_BINDINGS, &stored, sizeof(stored))) {
        return false;
    }
    if (stored.host < 0 || stored.host >= HOST_COUNT) {
        ESP_LOGW(TAG, "stored bind_cfg failed sanity check, ignoring");
        return false;
    }
    *out = stored;
    ESP_LOGI(TAG, "loaded bind_cfg from NVS: host=%ld", (long)stored.host);
    return true;
}

void config_store_save_bindings(const bind_cfg_t *cfg) {
    nvs_save_blob(NS_BINDINGS, cfg, sizeof(*cfg), "bind_cfg");
}
