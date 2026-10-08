#include "clock.h"
#include "tzrule.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "clock";

#define NVS_NS "clock"
#define VALID_AFTER 1704067200 // 2024-01-01: the chip starts at 1970
#define TRUST_US (24LL * 3600 * 1000000) // a sync this recent: the crystal hasn't drifted far

typedef struct {
    char label[CLOCK_LABEL_MAX + 1];
    char tz[CLOCK_TZ_MAX + 1];
} slot_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static slot_t s_slots[CLOCK_SLOTS]; // under s_mux
static tzrule_t s_rules[CLOCK_SLOTS]; // parsed from s_slots, under s_mux
static _Atomic uint8_t s_flags = CLOCK_FLAGS_DEFAULT;
static _Atomic uint32_t s_version = 1;
static _Atomic uint32_t s_dirty = 0; // bit i: slot i; DIRTY_FLAGS: the format
static _Atomic int64_t s_synced_us = -1; // esp_timer when the time was last set, -1 never
static _Atomic bool s_local_known = false; // LOCAL's zone came from a host (now or stored)
#define DIRTY_FLAGS (1u << 31)

static bool printable(const char *s, size_t max) {
    size_t n = strnlen(s, max + 1);
    if (n > max) return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < 0x20 || s[i] > 0x7E) return false;
    return true;
}

void clock_init(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t f;
        if (nvs_get_u8(h, "flags", &f) == ESP_OK) atomic_store(&s_flags, f);
        for (int i = 0; i < CLOCK_SLOTS; i++) {
            char key[4] = {'s', (char)('0' + i), 0};
            slot_t s;
            size_t n = sizeof(s);
            if (nvs_get_blob(h, key, &s, &n) == ESP_OK && n == sizeof(s)) {
                s.label[CLOCK_LABEL_MAX] = s.tz[CLOCK_TZ_MAX] = '\0';
                s_slots[i] = s;
            }
        }
        nvs_close(h);
    }
    atomic_store(&s_local_known, s_slots[0].label[0]);
    if (!s_slots[0].label[0]) { // LOCAL until the host says which zone that is
        strcpy(s_slots[0].label, "UTC");
        strcpy(s_slots[0].tz, "UTC0");
    }
    for (int i = 0; i < CLOCK_SLOTS; i++) tzrule_parse(s_slots[i].tz, &s_rules[i]);
}

bool clock_valid(void) {
    return time(NULL) >= VALID_AFTER;
}

void clock_set_utc_ms(int64_t ms) {
    if (ms / 1000 < VALID_AFTER) return;
    struct timeval tv = {.tv_sec = (time_t)(ms / 1000), .tv_usec = (suseconds_t)(ms % 1000) * 1000};
    settimeofday(&tv, NULL);
    clock_note_sync();
}

void clock_note_sync(void) {
    atomic_store(&s_synced_us, esp_timer_get_time());
}

bool clock_trusted(void) {
    int64_t at = atomic_load(&s_synced_us);
    return clock_valid() && atomic_load(&s_local_known) && at >= 0 && esp_timer_get_time() - at < TRUST_US;
}

uint8_t clock_flags(void) {
    return atomic_load(&s_flags);
}

void clock_set_flags(uint8_t flags) {
    if (atomic_exchange(&s_flags, flags) == flags) return;
    atomic_fetch_or(&s_dirty, DIRTY_FLAGS);
    atomic_fetch_add(&s_version, 1);
}

void clock_slot(int slot, char label[CLOCK_LABEL_MAX + 1], char tz[CLOCK_TZ_MAX + 1]) {
    label[0] = tz[0] = '\0';
    if (slot < 0 || slot >= CLOCK_SLOTS) return;
    portENTER_CRITICAL(&s_mux);
    memcpy(label, s_slots[slot].label, CLOCK_LABEL_MAX + 1);
    memcpy(tz, s_slots[slot].tz, CLOCK_TZ_MAX + 1);
    portEXIT_CRITICAL(&s_mux);
}

bool clock_set_slot(int slot, const char *label, const char *tz) {
    if (slot < 0 || slot >= CLOCK_SLOTS || !printable(label, CLOCK_LABEL_MAX) || !printable(tz, CLOCK_TZ_MAX)) return false;
    if (slot == 0 && !label[0]) return false; // LOCAL always shows
    slot_t s = {0};
    tzrule_t r = {0};
    if (label[0]) { // "" = an empty slot, no rule kept
        if (!tzrule_parse(tz, &r)) return false; // a rule the knob can't follow
        strcpy(s.label, label);
        strcpy(s.tz, tz);
    }
    portENTER_CRITICAL(&s_mux);
    bool same = memcmp(&s_slots[slot], &s, sizeof(s)) == 0;
    if (!same) {
        s_slots[slot] = s;
        s_rules[slot] = r;
    }
    portEXIT_CRITICAL(&s_mux);
    if (slot == 0) atomic_store(&s_local_known, true);
    if (!same) { // the host sends LOCAL every few minutes: only a change is stored
        atomic_fetch_or(&s_dirty, 1u << slot);
        atomic_fetch_add(&s_version, 1);
    }
    return true;
}

int clock_zone_count(void) {
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CLOCK_SLOTS; i++) n += s_slots[i].label[0] != '\0';
    portEXIT_CRITICAL(&s_mux);
    return n;
}

int clock_zone_slot(int n) {
    int slot = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CLOCK_SLOTS; i++) {
        if (!s_slots[i].label[0]) continue;
        if (n-- == 0) {
            slot = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return slot;
}

bool clock_now(int slot, struct tm *out, int *ms, int *offset_min) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < VALID_AFTER || slot < 0 || slot >= CLOCK_SLOTS) return false;
    portENTER_CRITICAL(&s_mux);
    tzrule_t r = s_rules[slot];
    portEXIT_CRITICAL(&s_mux);
    int32_t off = tzrule_offset(&r, tv.tv_sec);
    time_t local = tv.tv_sec + off;
    gmtime_r(&local, out); // the zone's wall clock: UTC shifted by its offset
    if (ms) *ms = (int)(tv.tv_usec / 1000);
    if (offset_min) *offset_min = (int)(off / 60);
    return true;
}

uint32_t clock_version(void) {
    return atomic_load(&s_version);
}

void clock_poll(void) {
    uint32_t dirty = atomic_exchange(&s_dirty, 0);
    if (!dirty) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        atomic_fetch_or(&s_dirty, dirty); // again next time
        return;
    }
    bool ok = true;
    if (dirty & DIRTY_FLAGS) ok &= nvs_set_u8(h, "flags", atomic_load(&s_flags)) == ESP_OK;
    for (int i = 0; i < CLOCK_SLOTS; i++) {
        if (!(dirty & (1u << i))) continue;
        slot_t s;
        portENTER_CRITICAL(&s_mux);
        s = s_slots[i];
        portEXIT_CRITICAL(&s_mux);
        char key[4] = {'s', (char)('0' + i), 0};
        ok &= nvs_set_blob(h, key, &s, sizeof(s)) == ESP_OK;
    }
    ok &= nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) ESP_LOGW(TAG, "couldn't store the clock's settings");
}
