#include "ext_link.h"
#include "ext_proto.h"
#include "host_link.h"
#include "host_proto.h"
#include "menu.h"
#include "notify.h"
#include "media.h"
#include "agent_board.h"
#include "net.h"
#include "net_link.h"
#include "pd_status.h"
#include "home.h"
#include "midi.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "clock.h"
#include "screen_stream.h"
#include "tasks_common.h"
#include "user_prefs.h"
#include "ui_state.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "ext";

// The one-boot serial request. RTC_NOINIT memory survives esp_restart() but not a power cycle
// or the EN button, and the reset reason is checked too, so stale contents can't trigger it.
#define SERIAL_BOOT_MAGIC 0x5E71A1B0u
static RTC_NOINIT_ATTR uint32_t s_serial_boot;

// A requested restart: the control task does it (motor off first, like RECALIBRATE) once the
// ack has had time to reach the host; the usb task steps in if the control loop isn't running.
#define REBOOT_DELAY_TICKS 25     // x 10ms
#define REBOOT_FALLBACK_TICKS 150 // x 10ms
static _Atomic bool s_reboot_pending = false;
static TickType_t s_reboot_at;

// Requests that write NVS run in the usb task (ext_link_poll), not the task they arrive in,
// the way host_link.c hands a profile save over; the reply goes back on the request's link.
static char s_text_req[USER_TEXT_MAX + 1];
static _Atomic bool s_text_pending = false;
static lights_t s_lights_req;
static bool s_lights_save;
static _Atomic bool s_lights_pending = false;
static _Atomic bool s_cover_end_pending = false;
static host_link_t s_text_link, s_lights_link;
static uint32_t s_text_gen, s_lights_gen; // host_link_gen() of the link that asked
// The cover being received: one transfer at a time (media.h), on the link that began it -- the
// Mac service, over USB or WiFi. Its pieces from any other link are ignored.
static _Atomic int s_cover_link = HOST_LINK_USB;
static uint32_t s_cover_gen;
static bool s_key_fresh;
static _Atomic bool s_key_pending = false; // EXT_NET_KEY (USB)
static bool s_pd_write;
static _Atomic bool s_pd_pending = false; // EXT_CMD_PD (USB): I2C and NVM, in the usb task
static int s_home_count;
static _Atomic bool s_home_pending = false; // EXT_HOME_COMMIT (USB): NVS, in the usb task
// EXT_HOME_EDIT: NVS too, in the usb task; acked on the link that asked.
EXT_RAM_BSS_ATTR static struct {
    int slot, what, value;
    uint32_t did;
    char name[HOME_NAME_LEN];
    host_link_t link;
    uint32_t gen;
} s_edit;
static _Atomic bool s_edit_pending = false;

// EXT_CMD_SYNTH. READ: the JSON of one synth, written in the usb task and kept for the pieces
// asked after it (s_syn_mux: the handlers read it in the TinyUSB / net tasks). A new version of the
// list (midi_synth_gen) or another synth makes the next offset-0 request write it again.
static SemaphoreHandle_t s_syn_mux;
static char *s_syn_json;
static uint32_t s_syn_len, s_syn_crc, s_syn_gen;
static int s_syn_index = -1;
EXT_RAM_BSS_ATTR static struct {
    int index;
    host_link_t link;
    uint32_t gen;
} s_syn_read;
static _Atomic bool s_syn_read_pending = false;
// PUT: one upload at a time, into PSRAM; END hands it to the usb task.
static char *s_put;
static uint32_t s_put_len, s_put_crc, s_put_got;
static bool s_put_save, s_put_bad;
static host_link_t s_put_link;
static uint32_t s_put_gen;
static _Atomic bool s_put_pending = false;
// OP: bit 31 pending, 16-23 link, 8-15 index, 0-7 op.
static _Atomic uint32_t s_syn_op = 0;
static uint32_t s_syn_op_gen;
// WiFi setup, staged until APPLY (EXT_CMD_NET). The password is wiped once it's stored.
static char s_net_ssid[NET_SSID_MAX + 1], s_net_pass[64];
static bool s_net_have_ssid, s_net_have_pass, s_net_on;
static _Atomic bool s_net_apply_pending = false;

// The companion's hands (EXT_CMD_INPUT): keys held until a deadline it keeps moving while they're
// down (a lost link can't leave one stuck), and detents to play out at the control loop's pace.
#define VKEYS_HOLD_TICKS pdMS_TO_TICKS(600)
#define VTURN_BACKLOG 60 // a fling, not a queue of minutes
static _Atomic uint8_t s_vkeys = 0;
static _Atomic uint32_t s_vkeys_until = 0;
static _Atomic int32_t s_vturns = 0;
static _Atomic int s_input_link = HOST_LINK_USB; // whose hands they are
// The WiFi client the knob's controls go to with no USB host (EXT_NET_CONTROLS): -1 none. The
// generation is stored before the link, and read after it.
static _Atomic int s_controls_link = -1;
static _Atomic uint32_t s_controls_gen;

uint8_t CONTROL_HOT ext_virtual_keys(void) {
    uint8_t k = atomic_load_explicit(&s_vkeys, memory_order_relaxed);
    if (k && (int32_t)(xTaskGetTickCount() - atomic_load_explicit(&s_vkeys_until, memory_order_relaxed)) >= 0) return 0;
    return k;
}

int8_t CONTROL_HOT ext_take_virtual_turn(void) {
    int32_t v = atomic_load_explicit(&s_vturns, memory_order_relaxed);
    if (v == 0) return 0;
    int8_t d = v > 0 ? 1 : -1;
    atomic_fetch_sub_explicit(&s_vturns, d, memory_order_relaxed);
    return d;
}

void ext_link_stop(host_link_t link) {
    int was = (int)link;
    atomic_compare_exchange_strong(&s_controls_link, &was, -1);
    if (atomic_load(&s_input_link) != (int)link) return;
    atomic_store(&s_vkeys, 0);
    atomic_store(&s_vturns, 0);
}

bool ext_controls_link(host_link_t *link, uint32_t *gen) {
    int l = atomic_load(&s_controls_link);
    if (l < 0) return false;
    *link = (host_link_t)l;
    *gen = atomic_load(&s_controls_gen);
    return true;
}

void ext_link_serial_boot(void) {
    if (atomic_load(&s_reboot_pending)) return;
    s_serial_boot = SERIAL_BOOT_MAGIC;
    s_reboot_at = xTaskGetTickCount() + REBOOT_DELAY_TICKS;
    atomic_store(&s_reboot_pending, true);
}

bool ext_take_serial_boot(void) {
    bool requested = s_serial_boot == SERIAL_BOOT_MAGIC && esp_reset_reason() == ESP_RST_SW;
    s_serial_boot = 0;
    return requested;
}

bool CONTROL_HOT ext_restart_due(void) {
    return atomic_load_explicit(&s_reboot_pending, memory_order_relaxed)
        && (int32_t)(xTaskGetTickCount() - s_reboot_at) >= 0;
}

static void put_u16(uint8_t *b, uint16_t v) { memcpy(b, &v, 2); }

static void build_prefs(uint8_t *r) {
    lights_t l;
    lights_get(&l);
    r[0] = EXT_TAG_PREFS;
    r[1] = (uint8_t)l.src;
    r[2] = (uint8_t)l.fx;
    put_u16(r + 3, (uint16_t)l.hue);
    r[5] = (uint8_t)l.sat;
    r[6] = (uint8_t)l.speed;
    put_u16(r + 7, (uint16_t)l.level);
    r[9] = menu_lights_dirty();
    r[10] = (uint8_t)cover_style_get();
    r[11] = COVER_STYLE_COUNT;
    user_text_get((char *)r + 16, USER_TEXT_MAX + 1);
}

static void ack(uint8_t *r, uint8_t cmd, uint8_t status) {
    r[0] = EXT_TAG_ACK;
    r[1] = cmd;
    r[2] = status;
}

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void build_clock(uint8_t *r, int slot) {
    char label[CLOCK_LABEL_MAX + 1], tz[CLOCK_TZ_MAX + 1];
    clock_slot(slot, label, tz);
    struct tm tm;
    int off = 0;
    r[0] = EXT_TAG_CLOCK;
    r[1] = clock_flags();
    r[2] = clock_now(slot, &tm, NULL, &off);
    r[3] = (uint8_t)slot;
    put_u16(r + 4, (uint16_t)(int16_t)off);
    memcpy(r + 6, label, strlen(label));
    memcpy(r + 18, tz, strlen(tz));
}

static void build_net(uint8_t *r) {
    net_status_t st;
    net_status(&st);
    r[0] = EXT_TAG_NET;
    r[1] = (uint8_t)st.state;
    r[2] = (uint8_t)st.rssi;
    memcpy(r + 3, &st.ip, 4); // network order: a.b.c.d
    r[7] = st.time_set;
    r[8] = st.enabled;
    memcpy(r + 9, st.ssid, strnlen(st.ssid, NET_SSID_MAX));
    memcpy(r + 41, st.host, strnlen(st.host, NET_HOST_MAX));
}

static uint32_t rd_u32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
static void put_u32(uint8_t *b, uint32_t v) { memcpy(b, &v, 4); }

// CRC-32 (IEEE, reflected, zlib.crc32), as host_link.c.
static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

// One of HOME's lamps as the knob sees it now (never its token).
EXT_RAM_BSS_ATTR static home_snapshot_t s_home_snap;
static void build_home(uint8_t *r, int slot) {
    home_get_snapshot(&s_home_snap);
    r[0] = EXT_TAG_HOME;
    r[1] = (uint8_t)home_lamp_count();
    r[2] = (uint8_t)slot;
    if (slot >= s_home_snap.count) return;
    const home_lamp_view_t *l = &s_home_snap.lamps[slot];
    memcpy(r + 3, &l->did, 4);
    r[7] = (l->online ? EXT_HOME_ONLINE : 0) | (l->known ? EXT_HOME_KNOWN : 0) | (l->on ? EXT_HOME_ON : 0)
         | (l->failed ? EXT_HOME_FAILED : 0);
    r[8] = l->bright;
    put_u16(r + 9, l->ct);
    r[11] = (uint8_t)(l->rgb >> 16);
    r[12] = (uint8_t)(l->rgb >> 8);
    r[13] = (uint8_t)l->rgb;
    r[14] = l->caps;
    memcpy(r + 15, l->name, strnlen(l->name, HOME_NAME_LEN - 1));
    r[35] = l->kind;
    memcpy(r + 36, &l->ip, 4); // network order: a.b.c.d
    r[40] = l->proto;
}

static void synth_list(uint8_t *r, int i) {
    r[0] = EXT_TAG_SYNTH;
    r[1] = EXT_SYNTH_LIST;
    r[2] = (uint8_t)i;
    r[3] = (uint8_t)midi_synth_count();
    if (i < 0 || i >= midi_synth_count()) return;
    const midi_synth_t *sy = midi_synth_get(i);
    r[4] = midi_synth_flags(i);
    r[5] = sy->n_params;
    r[6] = sy->channel;
    r[7] = sy->prog_scheme;
    put_u16(r + 8, sy->prog_count);
    memcpy(r + 10, sy->id, strnlen(sy->id, MIDI_ID_MAX));
    memcpy(r + 34, sy->maker, strnlen(sy->maker, MIDI_MAKER_MAX));
    memcpy(r + 46, sy->name, strnlen(sy->name, MIDI_NAME_MAX));
}

// A piece of the JSON written last. Caller holds s_syn_mux.
static void synth_piece_locked(uint8_t *r, uint32_t off) {
    r[0] = EXT_TAG_SYNTH;
    r[1] = EXT_SYNTH_READ;
    r[2] = (uint8_t)s_syn_index;
    put_u32(r + 4, s_syn_len);
    put_u32(r + 8, s_syn_crc);
    put_u32(r + 12, off);
    if (s_syn_json == NULL || off >= s_syn_len) return;
    uint32_t n = s_syn_len - off < EXT_SYNTH_CHUNK ? s_syn_len - off : EXT_SYNTH_CHUNK;
    r[3] = (uint8_t)n;
    memcpy(r + 16, s_syn_json + off, n);
}

static void synth_result(uint8_t *r, uint8_t what, int err, int index, bool removed, const char *why) {
    r[0] = EXT_TAG_SYNTH;
    r[1] = EXT_SYNTH_RESULT;
    r[2] = what;
    r[3] = (uint8_t)err;
    r[4] = (uint8_t)index;
    r[5] = removed;
    if (why) memcpy(r + 8, why, strnlen(why, HOST_REPORT_SIZE - 9));
}

static void synth_status(uint8_t *r) {
    midi_snapshot_t m;
    midi_get_snapshot(&m);
    r[0] = EXT_TAG_SYNTH;
    r[1] = EXT_SYNTH_STATUS;
    r[2] = m.active;
    r[3] = (uint8_t)m.synth;
    r[4] = (uint8_t)m.param;
    put_u16(r + 5, (uint16_t)(int16_t)m.value);
    r[7] = m.browsing;
    put_u16(r + 8, (uint16_t)(int16_t)m.prog);
    r[10] = m.channel;
    r[11] = m.usb;
    r[12] = m.trs;
    put_u32(r + 13, m.tx);
    put_u32(r + 17, m.rx);
}

static bool handle_synth(host_link_t link, const uint8_t *in, uint8_t *r) {
    switch (in[1]) {
        case EXT_SYNTH_LIST:
            synth_list(r, in[2]);
            return true;
        case EXT_SYNTH_STATUS:
            synth_status(r);
            return true;
        case EXT_SYNTH_GOTO:
            midi_goto(in[2]);
            ack(r, in[0], EXT_ST_OK);
            return true;
        case EXT_SYNTH_READ: {
            uint32_t off = rd_u32(in + 4);
            if (s_syn_mux == NULL) { // the usb task's first pass makes it
                ack(r, in[0], EXT_ST_BAD_PARAM);
                return true;
            }
            xSemaphoreTake(s_syn_mux, portMAX_DELAY);
            bool have = s_syn_json != NULL && s_syn_index == in[2] && s_syn_gen == midi_synth_gen();
            if (have && off > 0) {
                synth_piece_locked(r, off);
                xSemaphoreGive(s_syn_mux);
                return true;
            }
            xSemaphoreGive(s_syn_mux);
            if (off > 0 || atomic_load(&s_syn_read_pending)) { // offset 0 first, one at a time
                ack(r, in[0], EXT_ST_BAD_PARAM);
                return true;
            }
            s_syn_read.index = in[2];
            s_syn_read.link = link;
            s_syn_read.gen = host_link_gen(link);
            atomic_store(&s_syn_read_pending, true); // ext_link_poll writes it and answers
            return false;
        }
        case EXT_SYNTH_PUT_BEGIN: {
            if (atomic_load(&s_put_pending) || (s_put != NULL && s_put_link != link)) {
                synth_result(r, EXT_SYNTH_PUT_END, MIDI_SYNTH_ERR_STORAGE, 0, false, "busy");
                return true;
            }
            heap_caps_free(s_put);
            s_put = NULL;
            uint32_t len = rd_u32(in + 4);
            if (len == 0 || len > MIDI_JSON_MAX || (s_put = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM)) == NULL) {
                synth_result(r, EXT_SYNTH_PUT_END, MIDI_SYNTH_ERR_INVALID, 0, false, "too big");
                return true;
            }
            s_put_len = len;
            s_put_crc = rd_u32(in + 8);
            s_put_got = 0;
            s_put_save = in[2] & EXT_SYNTH_SAVE;
            s_put_bad = false;
            s_put_link = link;
            s_put_gen = host_link_gen(link);
            return false;
        }
        case EXT_SYNTH_PUT_DATA: {
            if (s_put == NULL || s_put_link != link || atomic_load(&s_put_pending)) return false;
            uint32_t off = in[2] | in[3] << 8 | (uint32_t)in[4] << 16, n = in[5];
            if (off != s_put_got || n > EXT_SYNTH_PUT_CHUNK || off + n > s_put_len) {
                s_put_bad = true; // reported at END
                return false;
            }
            memcpy(s_put + off, in + 8, n);
            s_put_got += n;
            return false;
        }
        case EXT_SYNTH_PUT_END:
            if (s_put == NULL || s_put_link != link) {
                synth_result(r, EXT_SYNTH_PUT_END, MIDI_SYNTH_ERR_INVALID, 0, false, "no upload");
                return true;
            }
            atomic_store(&s_put_pending, true); // the usb task parses and stores it
            return false;
        case EXT_SYNTH_OP:
            if (atomic_load(&s_syn_op)) {
                synth_result(r, EXT_SYNTH_OP, MIDI_SYNTH_ERR_STORAGE, in[2], false, "busy");
                return true;
            }
            s_syn_op_gen = host_link_gen(link);
            atomic_store(&s_syn_op, 0x80000000u | (uint32_t)link << 16 | (uint32_t)in[2] << 8 | in[3]);
            return false;
        default:
            ack(r, in[0], EXT_ST_BAD_PARAM);
            return true;
    }
}

bool ext_link_handle(host_link_t link, const uint8_t *in, uint8_t *r) {
    switch (in[0]) {
        case EXT_CMD_HELLO:
            r[0] = EXT_TAG_HELLO;
            r[1] = EXT_PROTO_VERSION;
            return true;
        case EXT_CMD_REBOOT:
            if (in[1] > EXT_REBOOT_SERIAL) {
                ack(r, in[0], EXT_ST_BAD_PARAM);
                return true;
            }
            if (in[1] == EXT_REBOOT_SERIAL && link != HOST_LINK_USB) { // a boot with no HID: for a USB host
                ack(r, in[0], EXT_ST_USB_ONLY);
                return true;
            }
            s_serial_boot = in[1] == EXT_REBOOT_SERIAL ? SERIAL_BOOT_MAGIC : 0;
            s_reboot_at = xTaskGetTickCount() + REBOOT_DELAY_TICKS;
            atomic_store(&s_reboot_pending, true);
            ack(r, in[0], EXT_ST_OK);
            return true;
        case EXT_CMD_TEXT:
            if (in[1] != 0 || atomic_load(&s_text_pending)) {
                ack(r, in[0], EXT_ST_BAD_PARAM);
                return true;
            }
            memcpy(s_text_req, in + 2, USER_TEXT_MAX);
            s_text_req[USER_TEXT_MAX] = '\0';
            s_text_link = link;
            s_text_gen = host_link_gen(link);
            atomic_store(&s_text_pending, true); // acked from ext_link_poll once stored
            return false;
        case EXT_CMD_LIGHTS: {
            if (atomic_load(&s_lights_pending)) {
                ack(r, in[0], EXT_ST_BAD_PARAM);
                return true;
            }
            lights_t l;
            lights_get(&l);
            if (in[2] != 0xFF) l.src = in[2];
            if (in[3] != 0xFF) l.fx = in[3];
            if (rd_u16(in + 4) != 0xFFFF) l.hue = rd_u16(in + 4);
            if (in[6] != 0xFF) l.sat = in[6];
            if (in[7] != 0xFF) l.speed = in[7];
            if (rd_u16(in + 8) != 0xFFFF) l.level = rd_u16(in + 8);
            s_lights_req = l;
            s_lights_save = in[1] & EXT_LIGHTS_SAVE;
            s_lights_link = link;
            s_lights_gen = host_link_gen(link);
            atomic_store(&s_lights_pending, true);
            return false;
        }
        case EXT_CMD_PREFS:
            build_prefs(r);
            return true;
        case EXT_CMD_MUSIC:
            if (in[1] != 0xFF) {
                if (in[1] >= COVER_STYLE_COUNT) {
                    ack(r, in[0], EXT_ST_BAD_PARAM);
                    return true;
                }
                cover_style_set(in[1]); // stored from ext_link_poll
            }
            build_prefs(r);
            return true;
        case EXT_CMD_COVER:
            if (in[1] == EXT_COVER_BEGIN) { // a new transfer, from whichever link (it replaces one going)
                uint32_t len, crc;
                memcpy(&len, in + 4, 4);
                memcpy(&crc, in + 8, 4);
                atomic_store(&s_cover_link, link);
                s_cover_gen = host_link_gen(link);
                ack(r, in[0], media_cover_begin(len, crc) ? EXT_ST_OK : EXT_ST_BAD_PARAM);
                return true;
            }
            if (atomic_load(&s_cover_link) != (int)link) return false; // not this link's transfer
            if (in[1] == EXT_COVER_DATA) {
                uint32_t off = in[2] | in[3] << 8 | (uint32_t)in[4] << 16;
                media_cover_data(off, in + 6, in[5] <= EXT_COVER_CHUNK ? in[5] : 0); // a failure shows at END
                return false;
            }
            if (in[1] == EXT_COVER_END) {
                atomic_store(&s_cover_end_pending, true); // the CRC runs in the usb task
                return false;
            }
            ack(r, in[0], EXT_ST_BAD_PARAM);
            return true;
        case EXT_CMD_TRACK:
            if (in[1] & EXT_TRACK_NONE) {
                media_clear();
            } else {
                media_track_t t = {.playing = in[1] & EXT_TRACK_PLAYING, .volume = in[2] <= 100 ? (int8_t)in[2] : -1};
                for (int i = 0; i < 3; i++) t.palette[i] = (uint32_t)in[3 + i * 3] << 16 | (uint32_t)in[4 + i * 3] << 8 | in[5 + i * 3];
                memcpy(t.title, in + 12, MEDIA_TEXT_MAX);
                memcpy(t.artist, in + 36, MEDIA_TEXT_MAX);
                media_set_track(&t);
            }
            return false;
        case EXT_CMD_AGENTS: {
            agent_row_t rows[AGENT_BOARD_MAX] = {0};
            int n = in[1] <= AGENT_BOARD_MAX ? in[1] : AGENT_BOARD_MAX;
            for (int i = 0; i < n; i++) {
                const uint8_t *p = in + 2 + i * 14;
                rows[i].source = p[0] < NOTIFY_SRC_COUNT ? p[0] : NOTIFY_SRC_OTHER;
                rows[i].state = p[1] < AGENT_STATE_COUNT ? p[1] : AGENT_IDLE;
                memcpy(rows[i].name, p + 2, AGENT_NAME_MAX);
            }
            agent_board_set(rows, n);
            return false;
        }
        case EXT_CMD_NOTIFY: {
            uint16_t id = rd_u16(in + 2);
            uint8_t kind = in[5] & 0x7F;
            if (in[1] == EXT_NOTIFY_POST && in[4] < NOTIFY_SRC_COUNT && kind < NOTIFY_KIND_COUNT) {
                notify_item_t it = {
                    .id = id, .source = in[4], .kind = kind,
                    .flags = (in[5] & EXT_NOTIFY_NUDGE) ? NOTIFY_FLAG_NUDGE : 0,
                    .color = (uint32_t)in[61] << 16 | (uint32_t)in[62] << 8 | in[63],
                };
                memcpy(it.title, in + 6, NOTIFY_TITLE_MAX);
                memcpy(it.body, in + 22, NOTIFY_BODY_MAX);
                notify_post(&it);
            } else if (in[1] == EXT_NOTIFY_CLEAR) {
                notify_clear(id);
            } else if (in[1] == EXT_NOTIFY_CLEAR_ALL) {
                notify_clear_all();
            }
            return false;
        }
        case EXT_CMD_TIME: {
            int64_t ms = 0;
            memcpy(&ms, in + 1, 6); // 48-bit, little-endian
            char label[CLOCK_LABEL_MAX + 1] = {0}, tz[CLOCK_TZ_MAX + 1] = {0};
            memcpy(label, in + 7, CLOCK_LABEL_MAX);
            memcpy(tz, in + 19, CLOCK_TZ_MAX);
            clock_set_utc_ms(ms);
            if (label[0]) clock_set_slot(0, label, tz);
            return false;
        }
        case EXT_CMD_CLOCK:
            if (in[1] == EXT_CLOCK_FORMAT) {
                clock_set_flags(in[2]);
                build_clock(r, 0);
            } else if (in[1] == EXT_CLOCK_ZONE && in[2] >= 1 && in[2] < CLOCK_SLOTS) {
                char label[CLOCK_LABEL_MAX + 1] = {0}, tz[CLOCK_TZ_MAX + 2] = {0};
                memcpy(label, in + 3, CLOCK_LABEL_MAX);
                memcpy(tz, in + 15, CLOCK_TZ_MAX + 1); // a full field has no NUL: too long, refused
                if (clock_set_slot(in[2], label, tz)) build_clock(r, in[2]);
                else ack(r, in[0], EXT_ST_BAD_PARAM);
            } else if (in[1] == EXT_CLOCK_GET && in[2] < CLOCK_SLOTS) {
                build_clock(r, in[2]);
            } else {
                ack(r, in[0], EXT_ST_BAD_PARAM);
            }
            return true;
        case EXT_CMD_SCREEN:
            host_link_screen(link, in[1]);
            return false;
        case EXT_CMD_INPUT:
            if (atomic_exchange(&s_input_link, link) != (int)link) { // another link's hands let go
                atomic_store(&s_vkeys, 0);
                atomic_store(&s_vturns, 0);
            }
            if (in[1] == EXT_INPUT_KEYS) {
                atomic_store(&s_vkeys_until, xTaskGetTickCount() + VKEYS_HOLD_TICKS);
                atomic_store(&s_vkeys, in[2] & (UI_BTN_F1 | UI_BTN_F2 | UI_BTN_F3 | UI_BTN_F4));
            } else if (in[1] == EXT_INPUT_TURN) {
                int32_t now = atomic_load(&s_vturns), d = (int8_t)in[2];
                if ((d > 0 && now < VTURN_BACKLOG) || (d < 0 && now > -VTURN_BACKLOG)) atomic_fetch_add(&s_vturns, d);
            }
            return false;
        case EXT_CMD_PD:
            if (link != HOST_LINK_USB) { // writes a chip's NVM: someone with the knob in hand
                ack(r, in[0], EXT_ST_USB_ONLY);
                return true;
            }
            s_pd_write = in[1] == 1;
            atomic_store(&s_pd_pending, true);
            return false;
        case EXT_CMD_SYNTH:
            return handle_synth(link, in, r);
        case EXT_CMD_HOME:
            if (in[1] == EXT_HOME_STATUS) {
                build_home(r, in[2]);
                return true;
            }
            if (in[1] == EXT_HOME_EDIT) { // no tokens: over WiFi too
                if (atomic_load(&s_edit_pending)) {
                    ack(r, in[0], EXT_ST_BAD_PARAM);
                    return true;
                }
                s_edit.slot = in[2];
                s_edit.what = in[3];
                s_edit.did = rd_u32(in + 4);
                s_edit.value = in[8];
                memset(s_edit.name, 0, sizeof(s_edit.name));
                memcpy(s_edit.name, in + 8, HOME_NAME_LEN - 1);
                s_edit.link = link;
                s_edit.gen = host_link_gen(link);
                atomic_store(&s_edit_pending, true); // NVS: ext_link_poll
                return false;
            }
            if (link != HOST_LINK_USB) { // the lamps' tokens: someone with the knob on a cable
                ack(r, in[0], EXT_ST_USB_ONLY);
                return true;
            }
            switch (in[1]) {
                case EXT_HOME_BEGIN:
                    home_import_begin();
                    ack(r, in[0], EXT_ST_OK);
                    return true;
                case EXT_HOME_LAMP: {
                    home_lamp_cfg_t l = {0};
                    l.did = rd_u32(in + 3);
                    memcpy(&l.ip, in + 7, 4); // a.b.c.d: network order as it stands
                    memcpy(l.token, in + 11, 16);
                    l.proto = in[27];
                    l.caps = in[28];
                    l.ct_min = rd_u16(in + 29);
                    l.ct_max = rd_u16(in + 31);
                    memcpy(l.siid, in + 33, HOME_PROP_COUNT);
                    memcpy(l.piid, in + 37, HOME_PROP_COUNT);
                    memcpy(l.name, in + 41, HOME_NAME_LEN - 1);
                    l.kind = in[61];
                    bool ok = home_import_lamp(in[2], &l);
                    memset(&l, 0, sizeof(l));
                    ack(r, in[0], ok ? EXT_ST_OK : EXT_ST_BAD_PARAM);
                    return true;
                }
                case EXT_HOME_COMMIT:
                    if (in[2] > HOME_MAX_LAMPS) {
                        ack(r, in[0], EXT_ST_BAD_PARAM);
                        return true;
                    }
                    s_home_count = in[2];
                    atomic_store(&s_home_pending, true); // NVS: ext_link_poll
                    return false;
                default:
                    ack(r, in[0], EXT_ST_BAD_PARAM);
                    return true;
            }
        case EXT_CMD_NET:
            // The network's name and password, the radio, the pairing key: someone with the knob
            // on a cable. Over WiFi they could only cut the link they came over, or hand it on.
            if (in[1] == EXT_NET_CONTROLS) { // WiFi only: on USB the controls are the cable's anyway
                if (link == HOST_LINK_USB) {
                    ack(r, in[0], EXT_ST_BAD_PARAM);
                    return true;
                }
                if (in[2] == 1) {
                    atomic_store(&s_controls_gen, host_link_gen(link));
                    atomic_store(&s_controls_link, link);
                } else {
                    int was = (int)link;
                    atomic_compare_exchange_strong(&s_controls_link, &was, -1);
                }
                ack(r, in[0], EXT_ST_OK);
                return true;
            }
            if (in[1] != EXT_NET_STATUS && link != HOST_LINK_USB) {
                ack(r, in[0], EXT_ST_USB_ONLY);
                return true;
            }
            if ((atomic_load(&s_net_apply_pending) || atomic_load(&s_key_pending)) && in[1] != EXT_NET_STATUS) {
                ack(r, in[0], EXT_ST_BAD_PARAM);
                return true;
            }
            switch (in[1]) {
                case EXT_NET_SSID:
                    memcpy(s_net_ssid, in + 2, NET_SSID_MAX);
                    s_net_ssid[NET_SSID_MAX] = '\0';
                    s_net_have_ssid = true;
                    return false;
                case EXT_NET_PASS_A:
                    memset(s_net_pass, 0, sizeof(s_net_pass));
                    memcpy(s_net_pass, in + 2, 32);
                    s_net_have_pass = true;
                    return false;
                case EXT_NET_PASS_B:
                    memcpy(s_net_pass + 32, in + 2, 31); // 63 characters at most, NUL-ended
                    return false;
                case EXT_NET_APPLY:
                    s_net_on = in[2] == 1;
                    atomic_store(&s_net_apply_pending, true); // stored in ext_link_poll
                    return false;
                case EXT_NET_STATUS:
                    build_net(r);
                    return true;
                case EXT_NET_KEY:
                    s_key_fresh = in[2] == 1;
                    atomic_store(&s_key_pending, true); // NVS: ext_link_poll
                    return false;
                default:
                    ack(r, in[0], EXT_ST_BAD_PARAM);
                    return true;
            }
        default:
            ack(r, in[0], EXT_ST_UNKNOWN);
            return true;
    }
}

// The usb task's half of EXT_CMD_SYNTH: the JSON (cJSON, files).
static void synth_poll(uint8_t *r) {
    if (s_syn_mux == NULL) s_syn_mux = xSemaphoreCreateMutex();
    if (atomic_load(&s_syn_read_pending)) {
        size_t len = 0;
        uint32_t gen = midi_synth_gen();
        char *text = midi_synth_json(s_syn_read.index, &len);
        xSemaphoreTake(s_syn_mux, portMAX_DELAY);
        cJSON_free(s_syn_json);
        s_syn_json = text;
        s_syn_len = text ? (uint32_t)len : 0;
        s_syn_crc = text ? crc32_ieee((const uint8_t *)text, len) : 0;
        s_syn_index = s_syn_read.index;
        s_syn_gen = gen;
        memset(r, 0, HOST_REPORT_SIZE);
        synth_piece_locked(r, 0);
        xSemaphoreGive(s_syn_mux);
        host_link_queue_to(s_syn_read.link, s_syn_read.gen, r);
        atomic_store(&s_syn_read_pending, false);
    }
    if (atomic_load(&s_put_pending)) {
        char why[HOST_REPORT_SIZE - 8] = "";
        int index = 0, err;
        if (s_put_bad || s_put_got != s_put_len || crc32_ieee((const uint8_t *)s_put, s_put_len) != s_put_crc) {
            err = MIDI_SYNTH_ERR_INVALID;
            snprintf(why, sizeof(why), "transfer: got %u of %u bytes", (unsigned)s_put_got, (unsigned)s_put_len);
        } else {
            s_put[s_put_len] = '\0';
            err = midi_synth_put(s_put, s_put_len, s_put_save, &index, why, sizeof(why));
        }
        if (err) ESP_LOGW(TAG, "synth upload: %d (%s)", err, why);
        heap_caps_free(s_put);
        s_put = NULL;
        memset(r, 0, HOST_REPORT_SIZE);
        synth_result(r, EXT_SYNTH_PUT_END, err, index, false, why);
        host_link_queue_to(s_put_link, s_put_gen, r);
        atomic_store(&s_put_pending, false);
    }
    uint32_t op = atomic_load(&s_syn_op);
    if (op) {
        int index = (op >> 8) & 0xFF;
        bool removed = false;
        int err = midi_synth_op(index, op & 0xFF, &removed);
        if (removed) menu_midi_synth_removed(index);
        memset(r, 0, HOST_REPORT_SIZE);
        synth_result(r, EXT_SYNTH_OP, err, index, removed, NULL);
        host_link_queue_to((host_link_t)((op >> 16) & 0xFF), s_syn_op_gen, r);
        atomic_store(&s_syn_op, 0);
    }
    midi_synths_reap();
}

void ext_link_poll(void) {
    uint8_t r[HOST_REPORT_SIZE];
    if (atomic_load(&s_text_pending)) {
        bool ok = user_text_set(s_text_req);
        memset(r, 0, sizeof(r));
        ack(r, EXT_CMD_TEXT, ok ? EXT_ST_OK : EXT_ST_STORAGE);
        host_link_queue_to(s_text_link, s_text_gen, r);
        atomic_store(&s_text_pending, false);
    }
    if (atomic_load(&s_lights_pending)) {
        lights_set(&s_lights_req);
        if (s_lights_save) menu_remote_save_lights();
        memset(r, 0, sizeof(r));
        build_prefs(r);
        host_link_queue_to(s_lights_link, s_lights_gen, r);
        atomic_store(&s_lights_pending, false);
    }
    if (atomic_load(&s_cover_end_pending)) {
        bool ok = media_cover_end();
        memset(r, 0, sizeof(r));
        ack(r, EXT_CMD_COVER, ok ? EXT_ST_OK : EXT_ST_BAD_PARAM);
        host_link_queue_to((host_link_t)atomic_load(&s_cover_link), s_cover_gen, r); // the link that sent it
        atomic_store(&s_cover_end_pending, false);
    }
    if (atomic_load(&s_net_apply_pending)) {
        bool ok = net_configure(s_net_have_ssid ? s_net_ssid : NULL, s_net_have_pass ? s_net_pass : NULL, s_net_on);
        memset(s_net_pass, 0, sizeof(s_net_pass));
        s_net_have_ssid = s_net_have_pass = false;
        memset(r, 0, sizeof(r));
        if (ok) build_net(r);
        else ack(r, EXT_CMD_NET, EXT_ST_STORAGE);
        host_link_queue(r);
        atomic_store(&s_net_apply_pending, false);
    }
    if (atomic_load(&s_key_pending)) {
        uint8_t key[NET_KEY_BYTES];
        memset(r, 0, sizeof(r));
        if (net_link_key(key, s_key_fresh)) {
            r[0] = EXT_TAG_KEY;
            memcpy(r + 1, key, sizeof(key));
            put_u16(r + 33, NET_LINK_PORT);
            memset(key, 0, sizeof(key));
        } else {
            ack(r, EXT_CMD_NET, EXT_ST_STORAGE);
        }
        host_link_queue(r);
        memset(r, 0, sizeof(r));
        atomic_store(&s_key_pending, false);
    }
    if (atomic_load(&s_pd_pending)) {
        uint8_t before = 0, after = 0;
        memset(r, 0, sizeof(r));
        r[0] = EXT_TAG_PD;
        r[1] = (uint8_t)pd_nvm_5v(s_pd_write, r + 4, &before, &after);
        r[2] = before;
        r[3] = after;
        host_link_queue(r);
        atomic_store(&s_pd_pending, false);
    }
    if (atomic_load(&s_home_pending)) {
        bool ok = home_import_commit(s_home_count);
        memset(r, 0, sizeof(r));
        ack(r, EXT_CMD_HOME, ok ? EXT_ST_OK : EXT_ST_STORAGE);
        host_link_queue(r);
        atomic_store(&s_home_pending, false);
    }
    if (atomic_load(&s_edit_pending)) {
        static const int WHAT[] = {0, HOME_EDIT_NAME, HOME_EDIT_KIND, HOME_EDIT_MOVE, HOME_EDIT_REMOVE};
        int what = s_edit.what >= 1 && s_edit.what <= 4 ? WHAT[s_edit.what] : 0;
        bool ok = what && home_edit(s_edit.slot, s_edit.did, what, s_edit.value, s_edit.name);
        memset(r, 0, sizeof(r));
        ack(r, EXT_CMD_HOME, ok ? EXT_ST_OK : what ? EXT_ST_STORAGE : EXT_ST_BAD_PARAM);
        host_link_queue_to(s_edit.link, s_edit.gen, r);
        atomic_store(&s_edit_pending, false);
    }
    synth_poll(r);
    clock_poll();      // stores a changed format / zone
    user_prefs_poll(); // and MUSIC's cover style
    uint16_t id;
    uint8_t decision;
    while (notify_take_event(&id, &decision)) {
        memset(r, 0, sizeof(r));
        r[0] = EXT_TAG_NOTIFY;
        r[1] = decision;
        put_u16(r + 2, id);
        host_link_queue_all(r); // the Mac service, on whichever link (it ignores ids it didn't post)
    }
    if (atomic_load(&s_reboot_pending) && (int32_t)(xTaskGetTickCount() - s_reboot_at) >= REBOOT_FALLBACK_TICKS) {
        ESP_LOGW(TAG, "restart requested by the host -- the control loop didn't take it, restarting from here");
        esp_restart();
    }
}
