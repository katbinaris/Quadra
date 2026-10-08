#include "host_link.h"
#include "host_proto.h"
#include "ext_link.h"
#include "screen_stream.h"
#include "net_link.h"
#include "icon_store.h"
#include "menu.h"
#include "sysmon.h"
#include "pd_status.h"
#include "ui_state.h"
#include "app_mode.h"
#include "led_task.h"
#include "midi.h"
#include "app_profiles/app_profiles.h"
#include "app_profiles/profile_json.h"
#include "esp_heap_caps.h"
#include "class/hid/hid_device.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "host";

// Two links: USB (the vendor HID interface; reports arrive in the TinyUSB task) and the
// network (net_link.c; its task). Commands are handled one at a time (s_rx_lock) and each reply
// goes back on the link its command came on, so the Mac service on USB and a companion on WiFi
// (or two companions) work side by side. Each link has its own live stream; the screen, one
// encoder, goes to the link that asked for it last, and back to the other when that one stops.
#define REPLY_QUEUE_DEPTH 16 // the companion's once-a-second poll asks up to 8 things at once
#define NET_BATCH 32 // reports handed to the network per usb-task pass (USB: one, then completions)
static QueueHandle_t s_replies[HOST_LINK_COUNT];
static SemaphoreHandle_t s_rx_lock;
static volatile uint8_t s_instance; // the vendor HID's TinyUSB instance: 1, or 0 in the MIDI personality

static _Atomic int s_screen_link = HOST_LINK_USB; // the screen's (host_link_screen; s_tx_lock)
// Each link's host, by generation: host_link_stop_link moves it on (with s_tx_lock held), so work
// that finishes after its host went away -- a download, a stored upload, an ack -- isn't handed to
// the next one on that link.
static _Atomic uint32_t s_gen[HOST_LINK_COUNT];

// --- Profile transfers ---
// Download, one per link: HOST_CMD_PROFILE_READ asks, the usb task serializes the profile and
// the pieces go out back to back -- on USB each finished IN report sends the next one straight
// from TinyUSB's completion callback, so a 40KB profile takes well under a second instead of
// one piece per usb-task pass. s_tx_lock keeps the senders from building the same piece.
static SemaphoreHandle_t s_tx_lock;
typedef struct {
    _Atomic int req;          // the index asked for, -1: none
    char *out;                // the JSON being sent, NULL: none (the rest: s_tx_lock)
    uint32_t len, off;
    bool began;
    uint8_t index;
} download_t;
static download_t s_dl[HOST_LINK_COUNT];
// Upload: BEGIN / DATA land in the TinyUSB task (copy only); END hands the text to the usb
// task, which parses, applies and stores it.
static char *s_in = NULL;
static uint32_t s_in_len, s_in_crc, s_in_got;
static uint8_t s_in_flags;
static bool s_in_bad;
static host_link_t s_in_link;
static uint32_t s_in_gen;
static _Atomic bool s_in_done = false;
// HOST_CMD_PROFILE_OP, done in the usb task (file I/O): bit 31 pending, 16-23 link, 8-15 index,
// 0-7 op.
static _Atomic uint32_t s_op = 0;
static uint32_t s_op_gen; // set before s_op

// A link's live stream (HOST_CMD_STREAM): STATE at its rate, SYS twice a second, and the LEDs --
// a snapshot every LED_FRAME_US, sent as LED_PARTS reports back to back (on USB from the
// completion callback, like a profile download). The rest is s_tx_lock's.
#define LED_FRAME_US 66000
#define LED_PER_REPORT 20
#define LED_PARTS ((LED_VIEW_COUNT + LED_PER_REPORT - 1) / LED_PER_REPORT)
typedef struct {
    _Atomic uint8_t hz;          // 0: off
    _Atomic uint8_t screen_fps;  // what it asked of the screen (EXT_CMD_SCREEN)
    int64_t next_state, next_sys, next_leds;
    uint16_t seq;
    bool sys_b_pending;
    uint8_t sys_b[HOST_REPORT_SIZE];
    uint8_t leds[LED_VIEW_COUNT][3];
    int led_part; // LED_PARTS = nothing to send
} stream_t;
static stream_t s_st[HOST_LINK_COUNT];

// CRC-32 (IEEE, reflected, zlib.crc32), as icon_store.c.
static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static void put_u16(uint8_t *b, uint16_t v) { memcpy(b, &v, 2); }
static void put_u32(uint8_t *b, uint32_t v) { memcpy(b, &v, 4); }
static void put_i32(uint8_t *b, int32_t v) { memcpy(b, &v, 4); }
static void put_f32(uint8_t *b, float v) { memcpy(b, &v, 4); }
// NUL-padded into an n-byte field (the reply buffer is zeroed, so the last byte stays 0).
static void put_str(uint8_t *b, const char *s, size_t n) {
    size_t len = s ? strnlen(s, n - 1) : 0;
    memcpy(b, s, len);
}

static void queue_reply(host_link_t link, const uint8_t *r) {
    if (xQueueSend(s_replies[link], r, 0) != pdTRUE && link != HOST_LINK_USB)
        ESP_LOGW(TAG, "reply dropped (queue full)"); // USB with no host: nobody to tell
}

uint32_t host_link_gen(host_link_t link) {
    return atomic_load(&s_gen[link]);
}

void host_link_queue_to(host_link_t link, uint32_t gen, const uint8_t *report) {
    xSemaphoreTake(s_tx_lock, portMAX_DELAY); // not between stop_link's new generation and its reset
    if (atomic_load(&s_gen[link]) == gen) queue_reply(link, report);
    xSemaphoreGive(s_tx_lock);
}

void host_link_queue(const uint8_t *report) {
    queue_reply(HOST_LINK_USB, report);
}

static bool send_reply_locked(host_link_t l);

bool host_link_send_event(host_link_t link, uint32_t gen, const uint8_t *report) {
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    bool ok = atomic_load(&s_gen[link]) == gen && xQueueSend(s_replies[link], report, 0) == pdTRUE;
    if (ok) send_reply_locked(link);
    xSemaphoreGive(s_tx_lock);
    return ok;
}

void host_link_queue_all(const uint8_t *report) {
    queue_reply(HOST_LINK_USB, report);
    for (int l = HOST_LINK_NET; l < HOST_LINK_COUNT; l++)
        if (net_link_connected(l - HOST_LINK_NET)) host_link_queue_to((host_link_t)l, host_link_gen((host_link_t)l), report);
}


static void build_settings(uint8_t *r) {
    menu_remote_settings_t s;
    menu_remote_get(&s);
    r[0] = HOST_TAG_SETTINGS;
    put_u16(r + 1, (uint16_t)s.dirty);
    r[3] = (uint8_t)(s.dirty >> 16);
    put_i32(r + 4, s.detents);
    put_f32(r + 8, s.kp);
    put_f32(r + 12, s.kd);
    r[16] = (uint8_t)s.feel;
    r[17] = (uint8_t)s.amp;
    put_f32(r + 18, s.pitch);
    r[22] = 0x80 | (uint8_t)s.click; // 0x80: this firmware has a wave here (it was the timbre)
    r[23] = (uint8_t)s.hid_type;
    r[24] = (uint8_t)s.midi_channel;
    r[25] = (uint8_t)s.profile;
    r[26] = (uint8_t)s.boot_mode;
    r[27] = (uint8_t)s.rotation;
    r[28] = (uint8_t)s.host;
    r[29] = (uint8_t)s.shape;
    r[30] = (uint8_t)s.haptic_profile;
    r[31] = (uint8_t)s.feels;
    r[32] = (uint8_t)s.amp_max;
    r[33] = (uint8_t)s.mode_haptic;
    r[34] = (uint8_t)s.midi_synth;
    r[35] = (uint8_t)midi_synth_count();
    put_f32(r + 36, s.kp_min);
    put_f32(r + 40, s.kp_max);
    put_f32(r + 44, s.kd_min);
    put_f32(r + 48, s.kd_max);
    put_f32(r + 52, s.pitch_min);
    put_f32(r + 56, s.pitch_max);
}

static void result_reply(uint8_t *r, uint8_t cmd, uint8_t res, int index, bool removed, const char *why) {
    memset(r, 0, HOST_REPORT_SIZE);
    r[0] = HOST_TAG_RESULT;
    r[1] = cmd;
    r[2] = res;
    r[3] = (uint8_t)(index < 0 ? 0 : index);
    r[4] = (uint8_t)app_profiles_count();
    r[5] = removed;
    if (why) put_str(r + 8, why, HOST_REPORT_SIZE - 8);
}

static void error_reply(uint8_t *r, uint8_t cmd, uint8_t err) {
    r[0] = HOST_TAG_ERROR;
    r[1] = cmd;
    r[2] = err;
}

// One command from `link` -> at most one reply in `r`. Returns false when there's nothing to send.
static bool handle(host_link_t link, const uint8_t *in, uint8_t *r) {
    uint8_t cmd = in[0];
    switch (cmd) {
        case HOST_CMD_HELLO: {
            const esp_app_desc_t *app = esp_app_get_description();
            r[0] = HOST_TAG_HELLO;
            r[1] = HOST_PROTO_VERSION;
            r[2] = (uint8_t)app_profiles_count();
            r[3] = ui_state_get_usb_serial_active() ? 1 : 0;
            put_str(r + 4, app->version, 32);
            put_str(r + 36, app->date, 16);
            return true;
        }
        case HOST_CMD_GET_SETTINGS:
            build_settings(r);
            return true;
        case HOST_CMD_SET: {
            int32_t iv;
            float fv;
            memcpy(&iv, in + 4, 4);
            memcpy(&fv, in + 4, 4);
            // SERIAL boot mode has no HID and no WiFi: a choice for someone with the knob on a cable.
            if ((link != HOST_LINK_USB && in[1] == HOST_SET_BOOT) || !menu_remote_set(in[1], iv, fv)) {
                error_reply(r, cmd, HOST_ERR_BAD_PARAM);
                return true;
            }
            build_settings(r);
            return true;
        }
        case HOST_CMD_SAVE:
            menu_remote_save();
            build_settings(r);
            return true;
        case HOST_CMD_HAPTIC_RESET:
            menu_remote_reset_haptic();
            build_settings(r);
            return true;
        case HOST_CMD_REVERT:
            menu_remote_revert();
            build_settings(r);
            return true;
        case HOST_CMD_RESET_PEAKS:
            sysmon_reset_peaks();
            return false;
        case HOST_CMD_STREAM:
            atomic_store(&s_st[link].hz, in[1] > 50 ? 50 : in[1]);
            return false;
        case HOST_CMD_PROFILE: {
            if (in[1] >= app_profiles_count()) {
                error_reply(r, cmd, HOST_ERR_BAD_PARAM);
                return true;
            }
            const app_profile_t *p = app_profiles_get(in[1]);
            r[0] = HOST_TAG_PROFILE;
            r[1] = in[1];
            r[2] = (uint8_t)app_profiles_count();
            r[3] = (p->icon48 != NULL) | (uint8_t)(app_profiles_flags(in[1]) << 1);
            put_str(r + 4, p->id, 12);
            put_str(r + 16, p->name, 16);
            for (int i = 0; i < 4; i++) put_str(r + 32 + i * 8, p->legend[i], 8);
            return true;
        }
        case HOST_CMD_PROFILE_ICON: {
            uint16_t off;
            memcpy(&off, in + 2, 2);
            const app_profile_t *p = in[1] < app_profiles_count() ? app_profiles_get(in[1]) : NULL;
            if (p == NULL || p->icon48 == NULL || off >= ICON_BYTES) {
                error_reply(r, cmd, HOST_ERR_BAD_PARAM);
                return true;
            }
            uint16_t len = ICON_BYTES - off < HOST_ICON_CHUNK ? ICON_BYTES - off : HOST_ICON_CHUNK;
            r[0] = HOST_TAG_PROFILE_ICON;
            r[1] = in[1];
            put_u16(r + 2, off);
            r[4] = (uint8_t)len;
            memcpy(r + 8, p->icon48 + off, len);
            return true;
        }
        case HOST_CMD_PROFILE_READ:
            if (in[1] >= app_profiles_count()) {
                error_reply(r, cmd, HOST_ERR_BAD_PARAM);
                return true;
            }
            atomic_store(&s_dl[link].req, in[1]); // replaces one of this link's still going
            return false;
        case HOST_CMD_UPLOAD_BEGIN: {
            uint32_t len, crc;
            memcpy(&len, in + 4, 4);
            memcpy(&crc, in + 8, 4);
            // One upload at a time: the other link's, under way, is left alone.
            if (atomic_load(&s_in_done) || (s_in != NULL && s_in_link != link)) {
                result_reply(r, cmd, HOST_RES_BUSY, 0, false, NULL);
                return true;
            }
            free(s_in);
            s_in = NULL;
            if (len == 0 || len > HOST_PROFILE_MAX_BYTES ||
                (s_in = heap_caps_malloc_prefer(len + 1, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)) == NULL) {
                result_reply(r, cmd, HOST_RES_FULL, 0, false, "too big");
                return true;
            }
            s_in_len = len;
            s_in_crc = crc;
            s_in_got = 0;
            s_in_flags = in[1];
            s_in_bad = false;
            s_in_link = link;
            s_in_gen = atomic_load(&s_gen[link]);
            return false;
        }
        case HOST_CMD_UPLOAD_DATA: {
            uint32_t off = in[1] | in[2] << 8 | (uint32_t)in[3] << 16;
            if (s_in == NULL || s_in_link != link) return false; // not its upload
            if (atomic_load(&s_in_done) || off != s_in_got) {
                s_in_bad = true; // reported at END
                return false;
            }
            uint32_t n = s_in_len - off < HOST_TEXT_CHUNK ? s_in_len - off : HOST_TEXT_CHUNK;
            memcpy(s_in + off, in + 4, n);
            s_in_got += n;
            return false;
        }
        case HOST_CMD_UPLOAD_END:
            if (s_in == NULL || s_in_link != link) {
                result_reply(r, cmd, HOST_RES_TRANSFER, 0, false, "no upload");
                return true;
            }
            atomic_store(&s_in_done, true); // the usb task takes it from here
            return false;
        case HOST_CMD_PROFILE_OP:
            if (atomic_load(&s_op)) {
                result_reply(r, cmd, HOST_RES_BUSY, 0, false, NULL);
                return true;
            }
            s_op_gen = atomic_load(&s_gen[link]);
            atomic_store(&s_op, 0x80000000u | (uint32_t)link << 16 | (uint32_t)in[1] << 8 | in[2]);
            return false;
        default:
            error_reply(r, cmd, HOST_ERR_UNKNOWN_CMD);
            return true;
    }
}

void host_link_set_instance(uint8_t vendor_instance) { s_instance = vendor_instance; }

void host_link_init(uint8_t vendor_instance) {
    s_instance = vendor_instance;
    for (int l = 0; l < HOST_LINK_COUNT; l++) {
        // WiFi's in PSRAM (internal RAM is short; no task that uses them runs with the cache off)
        s_replies[l] = l == HOST_LINK_USB ? xQueueCreate(REPLY_QUEUE_DEPTH, HOST_REPORT_SIZE)
                                          : xQueueCreateWithCaps(REPLY_QUEUE_DEPTH, HOST_REPORT_SIZE, MALLOC_CAP_SPIRAM);
        s_st[l].led_part = LED_PARTS;
        s_dl[l].req = -1;
    }
    s_tx_lock = xSemaphoreCreateMutex();
    s_rx_lock = xSemaphoreCreateMutex();
}

static void send_now(host_link_t link);

void host_link_receive(host_link_t link, const uint8_t *report, uint16_t len) {
    if (s_rx_lock == NULL) return; // not up yet
    uint8_t in[HOST_REPORT_SIZE] = {0};
    uint8_t r[HOST_REPORT_SIZE] = {0};
    memcpy(in, report, len < sizeof(in) ? len : sizeof(in));
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    bool reply = false;
    if (in[0] >= 0x10 && in[0] <= 0x1F) reply = handle(link, in, r);
    else if (in[0] >= 0x20 && in[0] <= 0x3F) reply = ext_link_handle(link, in, r);
    else if (link == HOST_LINK_USB) reply = icon_store_handle_report(in, sizeof(in), r); // tools/send_icon.py
    if (reply) {
        queue_reply(link, r);
        send_now(link);
    }
    xSemaphoreGive(s_rx_lock);
}

void host_link_handle_report(const uint8_t *report, uint16_t len) {
    host_link_receive(HOST_LINK_USB, report, len);
}

static void build_state(uint8_t *r, uint16_t seq) {
    r[0] = HOST_TAG_STATE;
    put_u16(r + 1, seq);
    put_i32(r + 4, ui_state_get_knob_angle());
    put_i32(r + 8, ui_state_get_detent());
    r[12] = ui_state_get_buttons();
    r[13] = (uint8_t)menu_current_screen();
    r[14] = ui_state_get_saver();
    r[15] = (uint8_t)app_mode_live_slot();
    put_i32(r + 16, (int32_t)ui_state_get_clicks());
    put_i32(r + 20, (int32_t)ui_state_get_walls());
}

static void build_sys(uint8_t *a, uint8_t *b) {
    sysmon_info_t s;
    sysmon_get(&s);
    pd_status_t pd = pd_status_get();
    a[0] = HOST_TAG_SYS_A;
    put_u16(a + 4, s.motor_ma);
    put_u16(a + 6, s.led_ma);
    put_u16(a + 8, s.board_ma);
    put_u16(a + 10, s.total_ma);
    put_u16(a + 12, s.total_peak_ma);
    a[14] = s.chip_ok;
    put_f32(a + 16, s.chip_c);
    put_f32(a + 20, s.chip_peak_c);
    put_u16(a + 24, s.coil_ma);
    put_u16(a + 26, s.coil_peak_ma);
    put_f32(a + 28, s.copper_w);
    a[32] = (uint8_t)pd.source;
    put_u16(a + 34, pd.ma);
    put_u16(a + 36, pd.mv);

    b[0] = HOST_TAG_SYS_B;
    b[4] = s.load[0];
    b[5] = s.load[1];
    b[6] = s.load_peak[0];
    b[7] = s.load_peak[1];
    put_f32(b + 8, s.loop_khz);
    put_f32(b + 12, s.work_avg_us);
    put_f32(b + 16, s.work_max_us);
    put_f32(b + 20, s.jitter_max_us);
    put_u32(b + 24, s.missed);
    put_f32(b + 28, s.spikes_per_s);
    put_u32(b + 32, s.heap_free);
    put_u32(b + 36, s.heap_min);
    put_u32(b + 40, s.hid_drops);
    put_u32(b + 44, 0); // was the I2S audio gaps
    put_u32(b + 48, s.uptime_s);
    put_u32(b + 52, s.sensor_crc_errors);
}

// --- Sending: on USB (the vendor IN endpoint) or the network (net_link.c) ---

static bool link_ready(host_link_t l) {
    return l == HOST_LINK_USB ? tud_hid_n_ready(s_instance) : net_link_ready(l - HOST_LINK_NET);
}

static bool link_send(host_link_t l, const uint8_t *r) {
    return l == HOST_LINK_USB ? tud_hid_n_report(s_instance, 0, r, HOST_REPORT_SIZE) : net_link_send(l - HOST_LINK_NET, r);
}

// The next piece of link `l`'s profile download, if it has room. Caller holds s_tx_lock.
static bool send_piece_locked(host_link_t l) {
    download_t *d = &s_dl[l];
    if (d->out == NULL || !link_ready(l)) return false;
    uint8_t r[HOST_REPORT_SIZE] = {0};
    uint32_t n = 0;
    if (!d->began) {
        r[0] = HOST_TAG_PROFILE_BEGIN;
        r[1] = d->index;
        put_u32(r + 4, d->len);
        put_u32(r + 8, crc32_ieee((const uint8_t *)d->out, d->len));
    } else {
        n = d->len - d->off < HOST_TEXT_CHUNK ? d->len - d->off : HOST_TEXT_CHUNK;
        r[0] = HOST_TAG_PROFILE_DATA;
        r[1] = d->off & 0xFF;
        r[2] = (d->off >> 8) & 0xFF;
        r[3] = (d->off >> 16) & 0xFF;
        memcpy(r + 4, d->out + d->off, n);
    }
    if (!link_send(l, r)) return false;
    if (!d->began) d->began = true;
    else d->off += n;
    if (d->began && d->off >= d->len) {
        free(d->out);
        d->out = NULL;
    }
    return true;
}

// The next LED report of link `l`'s current frame. Caller holds s_tx_lock.
static bool send_led_part_locked(host_link_t l) {
    stream_t *st = &s_st[l];
    if (st->led_part >= LED_PARTS || !link_ready(l)) return false;
    int first = st->led_part * LED_PER_REPORT;
    int n = LED_VIEW_COUNT - first < LED_PER_REPORT ? LED_VIEW_COUNT - first : LED_PER_REPORT;
    uint8_t r[HOST_REPORT_SIZE] = {0};
    r[0] = HOST_TAG_LEDS;
    r[1] = (uint8_t)first;
    r[2] = (uint8_t)n;
    memcpy(r + 4, st->leds[first], (size_t)n * 3);
    if (!link_send(l, r)) return false;
    st->led_part++;
    return true;
}

// The next report of the screen frame on its way (EXT_CMD_SCREEN). Caller holds s_tx_lock.
static bool send_screen_locked(host_link_t l) {
    uint8_t r[HOST_REPORT_SIZE];
    if (!link_ready(l) || !screen_stream_next(r) || !link_send(l, r)) return false;
    screen_stream_sent();
    return true;
}

// The screen and the LED frames take turns, so a big screen frame (a new cover) doesn't stop
// the LEDs on the companion's picture. Caller holds s_tx_lock.
static bool send_screen_or_leds_locked(host_link_t l) {
    static bool s_screen_turn = true;
    bool screen = atomic_load(&s_screen_link) == (int)l;
    bool led = atomic_load(&s_st[l].hz) != 0 && s_st[l].led_part < LED_PARTS;
    bool sent = false;
    if (screen && (s_screen_turn || !led)) sent = send_screen_locked(l);
    if (!sent && led) sent = send_led_part_locked(l);
    s_screen_turn = !s_screen_turn;
    return sent;
}

// The oldest queued reply for `l`, if its link has room. Caller holds s_tx_lock, which keeps
// USB's two senders (the usb task, the completion callback) from reordering them.
static bool send_reply_locked(host_link_t l) {
    uint8_t r[HOST_REPORT_SIZE];
    if (xQueuePeek(s_replies[l], r, 0) != pdTRUE) return false;
    bool sent = l == HOST_LINK_USB ? link_ready(l) && link_send(l, r) : net_link_reply(l - HOST_LINK_NET, r);
    if (sent) xQueueReceive(s_replies[l], r, 0);
    return sent;
}

// One report of the live stream (or the screen) for link `l`, if one is due. Caller holds
// s_tx_lock.
static bool stream_one_locked(host_link_t l) {
    stream_t *st = &s_st[l];
    uint8_t hz = atomic_load(&st->hz);
    int64_t now = esp_timer_get_time();
    if (hz && st->sys_b_pending) { // SYS_B right after SYS_A
        if (!link_ready(l) || !link_send(l, st->sys_b)) return false;
        st->sys_b_pending = false;
        return true;
    }
    if (hz && now >= st->next_sys && link_ready(l)) {
        uint8_t a[HOST_REPORT_SIZE] = {0};
        memset(st->sys_b, 0, sizeof(st->sys_b));
        build_sys(a, st->sys_b);
        if (!link_send(l, a)) return false;
        st->sys_b_pending = true;
        st->next_sys = now + 500000;
        return true;
    }
    if (hz && now >= st->next_state && link_ready(l)) {
        uint8_t r[HOST_REPORT_SIZE] = {0};
        build_state(r, st->seq);
        if (!link_send(l, r)) return false;
        st->seq++;
        st->next_state = now + 1000000 / hz;
        return true;
    }
    // LEDs: a new frame once the last one is out; its reports and the screen's take turns.
    if (hz && st->led_part >= LED_PARTS && now >= st->next_leds) {
        led_task_snapshot(st->leds);
        st->led_part = 0;
        st->next_leds = now + LED_FRAME_US;
    }
    return send_screen_or_leds_locked(l);
}

// The next report for link `l`: a queued reply, else a piece of a profile download it asked
// for (which has the link to itself meanwhile), else its streams. false: nothing went.
static bool send_next(host_link_t l) {
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    bool sent = send_reply_locked(l) || (s_dl[l].out != NULL ? send_piece_locked(l) : stream_one_locked(l));
    xSemaphoreGive(s_tx_lock);
    return sent;
}

// A reply just queued, out now if its link has room: a request/reply exchange (the companion
// loads a profile's icon ~80 of them in a row) then costs a host poll, not a usb-task tick or
// a whole screen frame.
static void send_now(host_link_t link) {
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    send_reply_locked(link);
    xSemaphoreGive(s_tx_lock);
}

// TinyUSB task: a vendor IN report went out -- USB's next one, back to back. The live stream's
// timed reports wait for the usb task; the rest (replies, a download, LEDs, the screen) chain.
void host_link_report_sent(void) {
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if (!send_reply_locked(HOST_LINK_USB)) {
        if (s_dl[HOST_LINK_USB].out != NULL) send_piece_locked(HOST_LINK_USB);
        else send_screen_or_leds_locked(HOST_LINK_USB);
    }
    xSemaphoreGive(s_tx_lock);
}

// usb task: the slow half of the profile commands (JSON, files).
static void profile_work(void) {
    uint8_t r[HOST_REPORT_SIZE];
    for (int l = 0; l < HOST_LINK_COUNT; l++) {
        download_t *d = &s_dl[l];
        uint32_t gen = atomic_load(&s_gen[l]); // before taking the request: see stop_link
        int want = atomic_exchange(&d->req, -1);
        if (want < 0) continue;
        size_t len = 0;
        char *text = profile_json_write(app_profiles_get(want), &len);
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        if (atomic_load(&s_gen[l]) == gen) { // its host is still the one that asked
            if (text == NULL) {
                memset(r, 0, sizeof(r));
                error_reply(r, HOST_CMD_PROFILE_READ, HOST_ERR_BAD_PARAM);
                queue_reply((host_link_t)l, r);
            } else {
                free(d->out); // a new request replaces one still going
                d->out = text;
                d->len = len;
                d->off = 0;
                d->began = false;
                d->index = (uint8_t)want;
                text = NULL;
            }
        }
        xSemaphoreGive(s_tx_lock);
        free(text);
    }

    if (atomic_load(&s_in_done)) {
        char err[HOST_REPORT_SIZE - 8];
        int index = 0;
        bool removed = false;
        uint8_t res = HOST_RES_OK;
        if (s_in_bad || s_in_got != s_in_len || crc32_ieee((const uint8_t *)s_in, s_in_len) != s_in_crc) {
            res = HOST_RES_TRANSFER;
            snprintf(err, sizeof(err), "got %u of %u bytes%s", (unsigned)s_in_got, (unsigned)s_in_len,
                     s_in_bad ? ", out of order" : "");
        } else {
            s_in[s_in_len] = '\0';
            app_profile_t *p = profile_json_read(s_in, s_in_len, err, sizeof(err));
            if (p == NULL) {
                res = HOST_RES_INVALID;
            } else {
                app_profiles_err_t e = app_profiles_put_live(p, &index);
                if (e == APP_PROFILES_OK && (s_in_flags & HOST_UPLOAD_SAVE)) e = app_profiles_save(index);
                res = e == APP_PROFILES_OK ? HOST_RES_OK : e == APP_PROFILES_ERR_FULL ? HOST_RES_FULL : HOST_RES_STORAGE;
                snprintf(err, sizeof(err), "%s", res == HOST_RES_OK ? "" : "not stored");
            }
        }
        if (res != HOST_RES_OK) ESP_LOGW(TAG, "profile upload: %u (%s)", res, err);
        free(s_in);
        s_in = NULL;
        atomic_store(&s_in_done, false);
        result_reply(r, HOST_CMD_UPLOAD_END, res, index, removed, err);
        host_link_queue_to(s_in_link, s_in_gen, r);
    }

    uint32_t op = atomic_load(&s_op);
    if (op) {
        int index = (op >> 8) & 0xFF;
        bool removed = false;
        app_profiles_err_t e = APP_PROFILES_ERR_INDEX;
        switch (op & 0xFF) {
            case HOST_OP_SAVE: e = app_profiles_save(index); break;
            case HOST_OP_REVERT: e = app_profiles_revert(index, &removed); break;
            case HOST_OP_REMOVE: e = app_profiles_remove(index, &removed); break;
        }
        if (removed) menu_profile_removed(index);
        result_reply(r, HOST_CMD_PROFILE_OP,
                     e == APP_PROFILES_OK ? HOST_RES_OK : e == APP_PROFILES_ERR_STORAGE ? HOST_RES_STORAGE : HOST_RES_BAD_INDEX,
                     index, removed, NULL);
        host_link_queue_to((host_link_t)((op >> 16) & 0xFF), s_op_gen, r);
        atomic_store(&s_op, 0);
    }
    app_profiles_reap();
}

void host_link_poll(void) {
    profile_work();
    ext_link_poll();
    // USB: one report a pass; its completion callback sends the rest back to back. The network
    // has no such callback: a batch a pass, into net_link's queue.
    send_next(HOST_LINK_USB);
    for (int l = HOST_LINK_NET; l < HOST_LINK_COUNT; l++)
        for (int i = 0; i < NET_BATCH && send_next((host_link_t)l); i++) {}
}

// The screen to link `to` at `fps` (0: off). A new owner starts from a whole frame: what the
// last one has means nothing to it. Caller holds s_tx_lock (not between a screen report and its
// _sent()).
static void screen_to_locked(host_link_t to, uint8_t fps) {
    if (atomic_load(&s_screen_link) != (int)to) {
        screen_stream_stop();
        atomic_store(&s_screen_link, to);
    }
    screen_stream_set(fps);
}

// The screen's next owner once `link` lets go: another link that still wants it, if any.
static void screen_release_locked(host_link_t link) {
    if (atomic_load(&s_screen_link) != (int)link) return;
    for (int l = 0; l < HOST_LINK_COUNT; l++) {
        uint8_t fps = atomic_load(&s_st[l].screen_fps);
        if (l != (int)link && fps) {
            screen_to_locked((host_link_t)l, fps);
            return;
        }
    }
    screen_stream_stop();
}

void host_link_screen(host_link_t link, uint8_t fps) {
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    atomic_store(&s_st[link].screen_fps, fps);
    if (fps) screen_to_locked(link, fps);
    else screen_release_locked(link);
    xSemaphoreGive(s_tx_lock);
}

// The locks are let go of before the next is taken; where they nest, the order is always rx, then
// tx (host_link_receive).
void host_link_stop_link(host_link_t link) {
    if (s_tx_lock == NULL) return; // not up yet
    atomic_store(&s_st[link].hz, 0);
    ext_link_stop(link);
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    atomic_fetch_add(&s_gen[link], 1); // what it asked for and is still being done is dropped
    xQueueReset(s_replies[link]);
    if (atomic_load(&s_st[link].screen_fps)) {
        atomic_store(&s_st[link].screen_fps, 0);
        screen_release_locked(link);
    }
    atomic_store(&s_dl[link].req, -1);
    free(s_dl[link].out);
    s_dl[link].out = NULL;
    s_st[link].led_part = LED_PARTS;
    s_st[link].sys_b_pending = false;
    xSemaphoreGive(s_tx_lock);
    // Its upload, cut off mid-way, frees the way for the other link's (one being stored finishes).
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    if (s_in != NULL && s_in_link == link && !atomic_load(&s_in_done)) {
        free(s_in);
        s_in = NULL;
    }
    xSemaphoreGive(s_rx_lock);
}

void host_link_stop(void) {
    host_link_stop_link(HOST_LINK_USB);
}
