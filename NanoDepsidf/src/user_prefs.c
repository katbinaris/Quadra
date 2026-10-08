#include "user_prefs.h"
#include "clock.h"
#include "tasks_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "user_prefs";

#define NS "user_prefs"
#define KEY_TEXT "idle_text"
#define KEY_LIGHTS "lights"
#define KEY_COVER "cover"
#define KEY_SCREEN "screen"
#define LIGHTS_VERSION 1
#define SCREEN_VERSION 1

// --- idle text ---
static char s_text[USER_TEXT_MAX + 1];
static portMUX_TYPE s_text_mux = portMUX_INITIALIZER_UNLOCKED;
static _Atomic uint32_t s_text_version = 1;

// --- LIGHTS: one atomic per field (rotated on Core 0, read by the LED task) ---
static _Atomic int32_t s_src = LIGHT_SRC_APP, s_hue = 30, s_sat = 100, s_fx = LIGHT_FX_GRADIENT,
                       s_speed = 4, s_level = 100;

typedef struct {
    uint32_t version;
    lights_t l;
} lights_blob_t;

// --- MUSIC's cover style ---
static _Atomic int s_cover = COVER_FLAT;
static _Atomic bool s_cover_dirty = false;

// --- SCREEN: one atomic per field, like LIGHTS ---
static _Atomic int32_t s_bright = 80, s_saver = SAVER_AUTO, s_saver_s = 15, s_sleep_on = 0,
                       s_sleep_from = 23 * 60, s_sleep_to = 7 * 60, s_dark_s = 30, s_wake = WAKE_NORMAL;
static _Atomic bool s_sleeping = false;
// DRAM: the rotate callbacks run from IRAM on Core 0.
static const DRAM_ATTR int32_t SAVER_STEPS[] = {5, 10, 15, 30, 60, 120, 300, 600};
static const DRAM_ATTR int32_t DARK_STEPS[] = {0, 10, 30, 60, 120, 300, 600};
#define SAVER_STEP_COUNT (int)(sizeof(SAVER_STEPS) / sizeof(SAVER_STEPS[0]))
#define DARK_STEP_COUNT (int)(sizeof(DARK_STEPS) / sizeof(DARK_STEPS[0]))

typedef struct {
    uint32_t version;
    screen_t s;
} screen_blob_t;

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t CONTROL_HOT wrapi(int32_t v, int32_t n) { v %= n; return v < 0 ? v + n : v; }

static void sanitize(char *dst, const char *src) {
    size_t n = 0;
    for (; src[n] && n < USER_TEXT_MAX; n++) dst[n] = (src[n] >= 0x20 && src[n] <= 0x7E) ? src[n] : ' ';
    while (n > 0 && dst[n - 1] == ' ') n--; // trailing spaces would only shift the centring
    dst[n] = '\0';
}

void user_prefs_init(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return; // nothing saved yet: defaults
    char buf[USER_TEXT_MAX + 1];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, KEY_TEXT, buf, &len) == ESP_OK) sanitize(s_text, buf);
    lights_blob_t b;
    len = sizeof(b);
    if (nvs_get_blob(h, KEY_LIGHTS, &b, &len) == ESP_OK && len == sizeof(b) && b.version == LIGHTS_VERSION) {
        lights_set(&b.l);
    }
    uint8_t cover;
    if (nvs_get_u8(h, KEY_COVER, &cover) == ESP_OK) atomic_store(&s_cover, clampi(cover, 0, COVER_STYLE_COUNT - 1));
    screen_blob_t sb;
    len = sizeof(sb);
    if (nvs_get_blob(h, KEY_SCREEN, &sb, &len) == ESP_OK && len == sizeof(sb) && sb.version == SCREEN_VERSION) {
        screen_set(&sb.s);
    }
    nvs_close(h);
}

static bool nvs_write(const char *key, const void *data, size_t size, bool is_str) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = is_str ? nvs_set_str(h, key, (const char *)data) : nvs_set_blob(h, key, data, size);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    return err == ESP_OK;
}

uint32_t user_text_version(void) { return atomic_load(&s_text_version); }

void user_text_get(char *out, size_t n) {
    if (n == 0) return;
    portENTER_CRITICAL(&s_text_mux);
    strncpy(out, s_text, n - 1);
    portEXIT_CRITICAL(&s_text_mux);
    out[n - 1] = '\0';
}

bool user_text_set(const char *s) {
    char clean[USER_TEXT_MAX + 1];
    sanitize(clean, s);
    portENTER_CRITICAL(&s_text_mux);
    memcpy(s_text, clean, sizeof(clean));
    portEXIT_CRITICAL(&s_text_mux);
    atomic_fetch_add(&s_text_version, 1);
    return nvs_write(KEY_TEXT, clean, 0, true);
}

int cover_style_get(void) { return atomic_load(&s_cover); }

void cover_style_set(int style) {
    int v = clampi(style, 0, COVER_STYLE_COUNT - 1);
    if (atomic_exchange(&s_cover, v) != v) atomic_store(&s_cover_dirty, true);
}

const char *cover_style_name(int style) {
    static const char *const NAMES[COVER_STYLE_COUNT] = {"FLAT", "RECORD", "SLIDE", "BLEED"};
    return style >= 0 && style < COVER_STYLE_COUNT ? NAMES[style] : "?";
}

void user_prefs_poll(void) {
    if (!atomic_exchange(&s_cover_dirty, false)) return;
    uint8_t v = (uint8_t)atomic_load(&s_cover);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_COVER, v);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "saving the cover style failed: %s", esp_err_to_name(err));
}

void lights_get(lights_t *out) {
    out->src = atomic_load(&s_src);
    out->hue = atomic_load(&s_hue);
    out->sat = atomic_load(&s_sat);
    out->fx = atomic_load(&s_fx);
    out->speed = atomic_load(&s_speed);
    out->level = atomic_load(&s_level);
}

void lights_set(const lights_t *in) {
    atomic_store(&s_src, clampi(in->src, 0, LIGHT_SRC_COUNT - 1));
    atomic_store(&s_hue, wrapi(in->hue, 360));
    atomic_store(&s_sat, clampi(in->sat, 0, 100));
    atomic_store(&s_fx, clampi(in->fx, 0, LIGHT_FX_COUNT - 1));
    atomic_store(&s_speed, clampi(in->speed, 1, 10));
    atomic_store(&s_level, clampi(in->level, 10, 200));
}

bool lights_save(void) {
    lights_blob_t b = {.version = LIGHTS_VERSION};
    lights_get(&b.l);
    return nvs_write(KEY_LIGHTS, &b, sizeof(b), false);
}

void CONTROL_HOT lights_rotate_src(int8_t dir) { atomic_store(&s_src, wrapi(atomic_load(&s_src) + dir, LIGHT_SRC_COUNT)); }
void CONTROL_HOT lights_rotate_hue(int8_t dir) { atomic_store(&s_hue, wrapi(atomic_load(&s_hue) + dir * LIGHT_HUE_STEP, 360)); }
void CONTROL_HOT lights_rotate_fx(int8_t dir) { atomic_store(&s_fx, wrapi(atomic_load(&s_fx) + dir, LIGHT_FX_COUNT)); }
void CONTROL_HOT lights_rotate_sat(int8_t dir) {
    int32_t v = atomic_load(&s_sat) + dir * LIGHT_SAT_STEP;
    atomic_store(&s_sat, v < 0 ? 0 : v > 100 ? 100 : v);
}
void CONTROL_HOT lights_rotate_speed(int8_t dir) {
    int32_t v = atomic_load(&s_speed) + dir;
    atomic_store(&s_speed, v < 1 ? 1 : v > 10 ? 10 : v);
}
void CONTROL_HOT lights_rotate_level(int8_t dir) {
    int32_t v = atomic_load(&s_level) + dir * LIGHT_LEVEL_STEP;
    atomic_store(&s_level, v < 10 ? 10 : v > 200 ? 200 : v);
}

bool CONTROL_HOT lights_custom(void) { return atomic_load(&s_src) == LIGHT_SRC_CUSTOM; }
bool CONTROL_HOT lights_fx_animated(void) {
    int32_t fx = atomic_load(&s_fx);
    return fx == LIGHT_FX_BREATHE || fx == LIGHT_FX_SPIN || fx == LIGHT_FX_RAINBOW;
}

const char *lights_fx_name(int32_t fx) {
    static const char *const NAMES[LIGHT_FX_COUNT] = {"GRADIENT", "SOLID", "BREATHE", "SPIN", "RAINBOW", "OFF"};
    return fx >= 0 && fx < LIGHT_FX_COUNT ? NAMES[fx] : "?";
}

// --- SCREEN ---

static int32_t step_index_of(const int32_t *steps, int n, int32_t v) { // the step nearest v
    int best = 0;
    for (int i = 1; i < n; i++)
        if (abs(steps[i] - v) < abs(steps[best] - v)) best = i;
    return best;
}

void screen_get(screen_t *out) {
    out->bright = atomic_load(&s_bright);
    out->saver = atomic_load(&s_saver);
    out->saver_s = atomic_load(&s_saver_s);
    out->sleep_on = atomic_load(&s_sleep_on);
    out->sleep_from = atomic_load(&s_sleep_from);
    out->sleep_to = atomic_load(&s_sleep_to);
    out->dark_s = atomic_load(&s_dark_s);
    out->wake = atomic_load(&s_wake);
}

static int32_t clamp_time(int32_t min) {
    min = wrapi(min, 24 * 60);
    return min - min % SCREEN_TIME_STEP;
}

void screen_set(const screen_t *in) {
    atomic_store(&s_bright, clampi(in->bright, 10, 100));
    atomic_store(&s_saver, clampi(in->saver, 0, SAVER_COUNT - 1));
    atomic_store(&s_saver_s, SAVER_STEPS[step_index_of(SAVER_STEPS, SAVER_STEP_COUNT, in->saver_s)]);
    atomic_store(&s_sleep_on, in->sleep_on ? 1 : 0);
    atomic_store(&s_sleep_from, clamp_time(in->sleep_from));
    atomic_store(&s_sleep_to, clamp_time(in->sleep_to));
    atomic_store(&s_dark_s, DARK_STEPS[step_index_of(DARK_STEPS, DARK_STEP_COUNT, in->dark_s)]);
    atomic_store(&s_wake, clampi(in->wake, 0, WAKE_COUNT - 1));
}

bool screen_store(const screen_t *s) {
    screen_blob_t b = {.version = SCREEN_VERSION, .s = *s};
    return nvs_write(KEY_SCREEN, &b, sizeof(b), false);
}

void screen_copy_part(screen_t *dst, const screen_t *src, bool sleep_part) {
    if (sleep_part) {
        dst->sleep_on = src->sleep_on;
        dst->sleep_from = src->sleep_from;
        dst->sleep_to = src->sleep_to;
        dst->dark_s = src->dark_s;
        dst->wake = src->wake;
    } else {
        dst->bright = src->bright;
        dst->saver = src->saver;
        dst->saver_s = src->saver_s;
    }
}

bool screen_part_differs(const screen_t *a, const screen_t *b, bool sleep_part) {
    if (sleep_part) {
        return a->sleep_on != b->sleep_on || a->sleep_from != b->sleep_from || a->sleep_to != b->sleep_to
            || a->dark_s != b->dark_s || a->wake != b->wake;
    }
    return a->bright != b->bright || a->saver != b->saver || a->saver_s != b->saver_s;
}

bool screen_sleep_now(void) {
    if (!atomic_load(&s_sleep_on) || !clock_trusted()) return false;
    struct tm tm;
    if (!clock_now(0, &tm, NULL, NULL)) return false;
    int32_t m = tm.tm_hour * 60 + tm.tm_min, from = atomic_load(&s_sleep_from), to = atomic_load(&s_sleep_to);
    if (from == to) return false; // no window
    return from < to ? (m >= from && m < to) : (m >= from || m < to); // across midnight
}

bool screen_sleeping(void) { return atomic_load(&s_sleeping); }
void screen_set_sleeping(bool s) { atomic_store(&s_sleeping, s); }
bool screen_dim_now(void) { return atomic_load(&s_wake) == WAKE_DIM; }

static int32_t CONTROL_HOT step_rotate(const int32_t *steps, int n, int32_t v, int8_t dir) {
    int i = 0;
    while (i < n - 1 && steps[i] < v) i++;
    i += dir;
    return steps[i < 0 ? 0 : i >= n ? n - 1 : i];
}

void CONTROL_HOT screen_rotate_bright(int8_t dir) {
    int32_t v = atomic_load(&s_bright) + dir * SCREEN_BRIGHT_STEP;
    atomic_store(&s_bright, v < 10 ? 10 : v > 100 ? 100 : v);
}
void CONTROL_HOT screen_rotate_saver(int8_t dir) { atomic_store(&s_saver, wrapi(atomic_load(&s_saver) + dir, SAVER_COUNT)); }
void CONTROL_HOT screen_rotate_saver_s(int8_t dir) {
    atomic_store(&s_saver_s, step_rotate(SAVER_STEPS, SAVER_STEP_COUNT, atomic_load(&s_saver_s), dir));
}
void CONTROL_HOT screen_rotate_sleep_on(int8_t dir) { (void)dir; atomic_store(&s_sleep_on, !atomic_load(&s_sleep_on)); }
void CONTROL_HOT screen_rotate_sleep_from(int8_t dir) {
    atomic_store(&s_sleep_from, wrapi(atomic_load(&s_sleep_from) + dir * SCREEN_TIME_STEP, 24 * 60));
}
void CONTROL_HOT screen_rotate_sleep_to(int8_t dir) {
    atomic_store(&s_sleep_to, wrapi(atomic_load(&s_sleep_to) + dir * SCREEN_TIME_STEP, 24 * 60));
}
void CONTROL_HOT screen_rotate_dark_s(int8_t dir) {
    atomic_store(&s_dark_s, step_rotate(DARK_STEPS, DARK_STEP_COUNT, atomic_load(&s_dark_s), dir));
}
void CONTROL_HOT screen_rotate_wake(int8_t dir) { atomic_store(&s_wake, wrapi(atomic_load(&s_wake) + dir, WAKE_COUNT)); }
bool CONTROL_HOT screen_sleep_enabled(void) { return atomic_load(&s_sleep_on) != 0; }

const char *screen_saver_name(int32_t saver) {
    static const char *const NAMES[SAVER_COUNT] = {"AUTO", "ICON", "BOUNCE", "CLOCK", "MUSIC", "BLANK", "NEVER"};
    return saver >= 0 && saver < SAVER_COUNT ? NAMES[saver] : "?";
}

const char *screen_wake_name(int32_t wake) { return wake == WAKE_DIM ? "DIM" : "NORMAL"; }

void screen_format_secs(int32_t s, char *out, size_t n) {
    if (s <= 0) snprintf(out, n, "NOW");
    else if (s < 60) snprintf(out, n, "%ld S", (long)s);
    else snprintf(out, n, "%ld MIN", (long)(s / 60));
}

void screen_format_time(int32_t min, char *out, size_t n) {
    snprintf(out, n, "%02ld:%02ld", (long)(min / 60), (long)(min % 60));
}

uint32_t lights_hsv(float h, float s, float v) {
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c, r, g, b;
    switch ((int)(h / 60.0f)) {
        case 0: r = c; g = x; b = 0; break;
        case 1: r = x; g = c; b = 0; break;
        case 2: r = 0; g = c; b = x; break;
        case 3: r = 0; g = x; b = c; break;
        case 4: r = x; g = 0; b = c; break;
        default: r = c; g = 0; b = x; break;
    }
    return (uint32_t)lroundf((r + m) * 255) << 16 | (uint32_t)lroundf((g + m) * 255) << 8 | (uint32_t)lroundf((b + m) * 255);
}
