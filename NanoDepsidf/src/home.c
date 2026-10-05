#include "home.h"
#include "haptic_params.h"
#include "menu.h"
#include "net.h"
#include "tasks_common.h"
#include "ui_state.h"
#include "aes/esp_aes.h"
#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_md5.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "home";

#define MIIO_PORT 54321
#define HOME_SEND_MS 150       // a lamp gets the newest value at most this often while the knob turns
#define REPLY_WAIT_MS 1500     // a change without a reply by then: NO REPLY on screen
#define QUERY_RETRY_MS 700
#define SCAN_MS 2500           // the scan screen, at most
#define SCAN_MIN_MS 900        // and at least, so it reads as a scan
#define RESCAN_AFTER_MS 60000  // coming back to HOME after this long looks again
#define SWEEP_BURST 32         // a subnet sweep's hellos per pass (10 ms): 254 in 80 ms
#define OFFLINE_HELLO_MS 10000 // in the list, a lamp that hasn't answered gets another hello
#define REFRESH_MS 30000       // and one that has is read again (changed from the phone, say)
#define IDLE_CLOSE_MS 60000    // the socket closes this long after HOME was left
#define BRIGHT_STEP 2          // % per detent
#define TEMP_STEP 50           // K per detent
#define HUE_STEP 10            // degrees per detent: one turn of FINE (36) is the whole wheel

// The NVS blob: every imported lamp, tokens included (flash encryption is off, like the WiFi
// password; FIRMWARE.md section 15).
#define NVS_NS "home"
#define NVS_KEY "lamps"
#define STORE_VERSION 1
_Static_assert(sizeof(home_lamp_cfg_t) == 60, "home_lamp_cfg_t is stored in NVS: its size can't change");
typedef struct {
    uint32_t version;
    uint32_t count;
    home_lamp_cfg_t lamps[HOME_MAX_LAMPS];
} home_store_t;

// --- Core 0 -> home task: one byte per input, single producer, single consumer ---
#define EVT_RING 32
enum { EVT_F1 = 11, EVT_F2 = 12, EVT_F3 = 13 }; // turns are -1 / +1
static int8_t s_evt[EVT_RING]; // internal RAM: the control loop writes it while flash may be busy
static _Atomic uint32_t s_evt_head = 0, s_evt_tail = 0;
static _Atomic uint8_t s_end_flags = 0; // bit 0: nothing further back, bit 1: nothing further on
static _Atomic int s_haptic = HAPTIC_PROFILE_COARSE;

static void CONTROL_HOT push_evt(int8_t e) {
    uint32_t h = atomic_load_explicit(&s_evt_head, memory_order_relaxed);
    if (h - atomic_load_explicit(&s_evt_tail, memory_order_acquire) >= EVT_RING) return; // full: dropped
    s_evt[h % EVT_RING] = e;
    atomic_store_explicit(&s_evt_head, h + 1, memory_order_release);
}
void CONTROL_HOT home_input_rotate(int8_t dir) { push_evt(dir > 0 ? 1 : -1); }
void CONTROL_HOT home_input_key(uint8_t key) {
    push_evt(key == UI_BTN_F1 ? EVT_F1 : key == UI_BTN_F2 ? EVT_F2 : EVT_F3);
}
bool CONTROL_HOT home_at_end(int8_t dir) {
    return atomic_load_explicit(&s_end_flags, memory_order_relaxed) & (dir > 0 ? 2 : 1);
}
int CONTROL_HOT home_haptic_profile(void) { return atomic_load_explicit(&s_haptic, memory_order_relaxed); }

static bool take_evt(int8_t *e) {
    uint32_t t = atomic_load_explicit(&s_evt_tail, memory_order_relaxed);
    if (t == atomic_load_explicit(&s_evt_head, memory_order_acquire)) return false;
    *e = s_evt[t % EVT_RING];
    atomic_store_explicit(&s_evt_tail, t + 1, memory_order_release);
    return true;
}

// --- the lamps ---
typedef struct {
    home_lamp_cfg_t cfg;
    uint8_t key[16], iv[16]; // AES-128-CBC: MD5(token), MD5(key + token)
    uint8_t proto;           // in use (cfg.proto is the import's guess)
    // the session
    bool online;
    uint32_t stamp;          // the lamp's clock at its last hello
    int64_t stamp_at;        // and ours then
    int64_t hello_at;        // our last hello to it
    // its state, as read and then as the knob wants it
    bool known, on, color_mode;
    uint8_t bright;
    uint16_t ct, hue;
    // queries and changes in flight
    uint32_t query_id;
    int64_t query_at;
    int query_tries;
    int64_t refresh_at;
    uint8_t dirty;           // 1 << HOME_PROP_*: to send
    int64_t sent_at;
    uint32_t set_id;
    int64_t set_at;          // 0: no change waiting for its reply
    bool failed;
} lamp_t;

EXT_RAM_BSS_ATTR static lamp_t s_lamps[HOME_MAX_LAMPS];
static int s_count = 0;
EXT_RAM_BSS_ATTR static home_store_t s_import; // staged by the usb task
static SemaphoreHandle_t s_cfg_mux;             // s_import's commit vs the home task's reload
static _Atomic bool s_reload = false;
static _Atomic bool s_patch = false; // an edit: names, icons, order -- the lamps' sessions stay
EXT_RAM_BSS_ATTR static lamp_t s_tmp[HOME_MAX_LAMPS]; // the home task's, for a patch
static _Atomic int s_stored_count = 0;

// The screen's copy.
EXT_RAM_BSS_ATTR static home_snapshot_t s_snap;
EXT_RAM_BSS_ATTR static home_snapshot_t s_build;
static SemaphoreHandle_t s_snap_mux;
static _Atomic uint32_t s_version = 0;

// home task state
static home_phase_t s_phase = HOME_PHASE_OFF;
static int s_selected = 0;
static int s_last = -1; // the lamp changed last
static home_opt_t s_option = HOME_OPT_BRIGHT;
static int64_t s_scan_t0 = 0, s_left_at = 0;
static int s_scan_hellos = 0;
static uint32_t s_my_ip = 0; // the knob's address (network order)
// The sweep: a hello to every address of a subnet a missing lamp was last seen on (a router
// that hands out new addresses; broadcasts don't cross into another subnet).
static uint32_t s_sweep_net[HOME_MAX_LAMPS]; // x.y.z.0, host order
static int s_sweep_n = 0, s_sweep_pos = 0;
static bool s_ever_scanned = false;
static int s_sock = -1;
static uint32_t s_msg_id = 1;
// Packets: internal RAM, since the AES driver may hand them to DMA.
static uint8_t s_pkt[1024];
static char s_json[640];

// --- helpers ---
static void md5(const void *a, size_t na, const void *b, size_t nb, const void *c, size_t nc, uint8_t out[16]) {
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    if (na) esp_rom_md5_update(&ctx, a, na);
    if (nb) esp_rom_md5_update(&ctx, b, nb);
    if (nc) esp_rom_md5_update(&ctx, c, nc);
    esp_rom_md5_final(out, &ctx);
}

static void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int rgb_hue(uint32_t rgb) {
    float r = ((rgb >> 16) & 0xFF) / 255.0f, g = ((rgb >> 8) & 0xFF) / 255.0f, b = (rgb & 0xFF) / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    if (d <= 0) return 0;
    float h = mx == r ? fmodf((g - b) / d, 6) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
    int deg = (int)lroundf(h * 60);
    return ((deg % 360) + 360) % 360;
}

static bool has(const lamp_t *l, home_opt_t o) {
    return l->cfg.caps & (o == HOME_OPT_BRIGHT ? HOME_CAP_BRIGHT : o == HOME_OPT_TEMP ? HOME_CAP_TEMP : HOME_CAP_COLOR);
}

// --- the socket and the packets ---
static bool sock_open(void) {
    if (s_sock >= 0) return true;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGW(TAG, "no socket (%d)", errno);
        return false;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(s);
        return false;
    }
    s_sock = s;
    return true;
}

static void sock_close(void) {
    if (s_sock >= 0) close(s_sock);
    s_sock = -1;
}

static void send_to(uint32_t ip, const uint8_t *p, size_t n) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(MIIO_PORT), .sin_addr.s_addr = ip};
    sendto(s_sock, p, n, 0, (struct sockaddr *)&a, sizeof(a));
}

// miIO hello: 32 bytes, magic and length, the rest 0xFF. Every miIO device answers with its id
// and clock, token or not.
static void send_hello(uint32_t ip) {
    uint8_t h[32];
    memset(h, 0xFF, sizeof(h));
    put_be16(h, 0x2131);
    put_be16(h + 2, 32);
    send_to(ip, h, sizeof(h));
}

// One request: header, MD5 checksum over header + token + body, body AES-128-CBC with PKCS#7.
static uint32_t send_json(lamp_t *l) {
    size_t n = strlen(s_json);
    size_t pad = 16 - n % 16;
    if (n + pad > sizeof(s_json) || 32 + n + pad > sizeof(s_pkt)) return 0;
    memset(s_json + n, (int)pad, pad);
    n += pad;
    uint8_t *body = s_pkt + 32;
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, l->key, 128);
    uint8_t iv[16];
    memcpy(iv, l->iv, 16);
    esp_aes_crypt_cbc(&ctx, ESP_AES_ENCRYPT, n, iv, (const uint8_t *)s_json, body);
    esp_aes_free(&ctx);
    put_be16(s_pkt, 0x2131);
    put_be16(s_pkt + 2, (uint16_t)(32 + n));
    put_be32(s_pkt + 4, 0);
    put_be32(s_pkt + 8, l->cfg.did);
    put_be32(s_pkt + 12, l->stamp + (uint32_t)((now_ms() - l->stamp_at) / 1000));
    uint8_t sum[16];
    md5(s_pkt, 16, l->cfg.token, 16, body, n, sum);
    memcpy(s_pkt + 16, sum, 16);
    send_to(l->cfg.ip, s_pkt, 32 + n);
    return 1;
}

static uint32_t next_id(void) {
    if (++s_msg_id > 0x7FFFFFF0u) s_msg_id = 1;
    return s_msg_id;
}

static void send_query(lamp_t *l) {
    uint32_t id = next_id();
    int n;
    if (l->proto == HOME_PROTO_LEGACY) {
        n = snprintf(s_json, sizeof(s_json), "{\"id\":%lu,\"method\":\"get_prop\",\"params\":[\"power\",\"bright\"%s%s]}",
                     (unsigned long)id, (l->cfg.caps & HOME_CAP_TEMP) ? ",\"ct\"" : "",
                     (l->cfg.caps & HOME_CAP_COLOR) ? ",\"rgb\",\"color_mode\"" : "");
    } else {
        n = snprintf(s_json, sizeof(s_json), "{\"id\":%lu,\"method\":\"get_properties\",\"params\":[", (unsigned long)id);
        bool first = true;
        for (int p = 0; p < HOME_PROP_COUNT; p++) {
            if (!l->cfg.siid[p]) continue;
            n += snprintf(s_json + n, sizeof(s_json) - n, "%s{\"did\":\"%lu\",\"siid\":%u,\"piid\":%u}", first ? "" : ",",
                          (unsigned long)l->cfg.did, l->cfg.siid[p], l->cfg.piid[p]);
            first = false;
        }
        n += snprintf(s_json + n, sizeof(s_json) - n, "]}");
    }
    if (n <= 0 || n >= (int)sizeof(s_json)) return;
    if (send_json(l)) {
        l->query_id = id;
        l->query_at = now_ms();
        l->query_tries++;
    }
}

static void prop_value(const lamp_t *l, int p, char *out, size_t n) {
    switch (p) {
        case HOME_PROP_ON: snprintf(out, n, "%s", l->on ? "true" : "false"); break;
        case HOME_PROP_BRIGHT: snprintf(out, n, "%u", l->bright); break;
        case HOME_PROP_TEMP: snprintf(out, n, "%u", l->ct); break;
        default: snprintf(out, n, "%lu", (unsigned long)home_hue_rgb(l->hue)); break;
    }
}

// The changes waiting for a lamp: MIoT sends them together; the older protocol has one method
// per property, so one goes now and the rest at the next turn of the send interval.
static void send_changes(lamp_t *l) {
    uint32_t id = next_id();
    int n;
    if (l->proto == HOME_PROTO_LEGACY) {
        int p = 0;
        while (p < HOME_PROP_COUNT && !(l->dirty & (1u << p))) p++;
        if (p == HOME_PROP_COUNT) return;
        switch (p) {
            case HOME_PROP_ON:
                n = snprintf(s_json, sizeof(s_json), "{\"id\":%lu,\"method\":\"set_power\",\"params\":[\"%s\",\"smooth\",200]}",
                             (unsigned long)id, l->on ? "on" : "off");
                break;
            case HOME_PROP_BRIGHT:
                n = snprintf(s_json, sizeof(s_json), "{\"id\":%lu,\"method\":\"set_bright\",\"params\":[%u,\"smooth\",200]}",
                             (unsigned long)id, l->bright);
                break;
            case HOME_PROP_TEMP:
                n = snprintf(s_json, sizeof(s_json), "{\"id\":%lu,\"method\":\"set_ct_abx\",\"params\":[%u,\"smooth\",200]}",
                             (unsigned long)id, l->ct);
                break;
            default:
                n = snprintf(s_json, sizeof(s_json), "{\"id\":%lu,\"method\":\"set_rgb\",\"params\":[%lu,\"smooth\",200]}",
                             (unsigned long)id, (unsigned long)home_hue_rgb(l->hue));
                break;
        }
        l->dirty &= ~(1u << p);
    } else {
        n = snprintf(s_json, sizeof(s_json), "{\"id\":%lu,\"method\":\"set_properties\",\"params\":[", (unsigned long)id);
        bool first = true;
        for (int p = 0; p < HOME_PROP_COUNT; p++) {
            if (!(l->dirty & (1u << p)) || !l->cfg.siid[p]) continue;
            char v[16];
            prop_value(l, p, v, sizeof(v));
            n += snprintf(s_json + n, sizeof(s_json) - n, "%s{\"did\":\"%lu\",\"siid\":%u,\"piid\":%u,\"value\":%s}",
                          first ? "" : ",", (unsigned long)l->cfg.did, l->cfg.siid[p], l->cfg.piid[p], v);
            first = false;
        }
        n += snprintf(s_json + n, sizeof(s_json) - n, "]}");
        l->dirty = 0;
        if (first) return;
    }
    if (n <= 0 || n >= (int)sizeof(s_json)) return;
    if (send_json(l)) {
        l->set_id = id;
        l->sent_at = l->set_at = now_ms();
    }
}

// --- replies ---
static lamp_t *by_did(uint32_t did) {
    for (int i = 0; i < s_count; i++) {
        if (s_lamps[i].cfg.did == did) return &s_lamps[i];
    }
    return NULL;
}

static int json_int(const cJSON *v, int dflt) {
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v)) return atoi(v->valuestring);
    return dflt;
}

static void read_state(lamp_t *l, const cJSON *result) {
    if (l->proto == HOME_PROTO_LEGACY) {
        // In the order asked: power, bright, [ct], [rgb, color_mode]
        int i = 0;
        const cJSON *v = cJSON_GetArrayItem(result, i++);
        l->on = cJSON_IsString(v) && strcmp(v->valuestring, "on") == 0;
        l->bright = (uint8_t)clampi(json_int(cJSON_GetArrayItem(result, i++), l->bright), 1, 100);
        if (l->cfg.caps & HOME_CAP_TEMP) l->ct = (uint16_t)json_int(cJSON_GetArrayItem(result, i++), l->ct);
        if (l->cfg.caps & HOME_CAP_COLOR) {
            uint32_t rgb = (uint32_t)json_int(cJSON_GetArrayItem(result, i++), 0);
            if (rgb) l->hue = (uint16_t)rgb_hue(rgb);
            l->color_mode = json_int(cJSON_GetArrayItem(result, i++), 2) == 1;
        }
    } else {
        const cJSON *it;
        cJSON_ArrayForEach(it, result) {
            int siid = json_int(cJSON_GetObjectItem(it, "siid"), 0), piid = json_int(cJSON_GetObjectItem(it, "piid"), 0);
            if (json_int(cJSON_GetObjectItem(it, "code"), -1) != 0) continue;
            const cJSON *v = cJSON_GetObjectItem(it, "value");
            for (int p = 0; p < HOME_PROP_COUNT; p++) {
                if (l->cfg.siid[p] != siid || l->cfg.piid[p] != piid) continue;
                switch (p) {
                    case HOME_PROP_ON: l->on = cJSON_IsTrue(v); break;
                    case HOME_PROP_BRIGHT: l->bright = (uint8_t)clampi(json_int(v, l->bright), 1, 100); break;
                    case HOME_PROP_TEMP: l->ct = (uint16_t)json_int(v, l->ct); break;
                    default: {
                        uint32_t rgb = (uint32_t)json_int(v, 0);
                        if (rgb) l->hue = (uint16_t)rgb_hue(rgb);
                        break;
                    }
                }
            }
        }
        // MIoT doesn't say whether a lamp with both is showing its colour or its white: a lamp
        // with no white is always colour.
        l->color_mode = !(l->cfg.caps & HOME_CAP_TEMP);
    }
    if (l->cfg.ct_max > l->cfg.ct_min) l->ct = (uint16_t)clampi(l->ct, l->cfg.ct_min, l->cfg.ct_max);
    l->known = true;
    l->refresh_at = now_ms();
}

static void handle_packet(const uint8_t *p, int n, uint32_t from) {
    if (n < 32 || p[0] != 0x21 || p[1] != 0x31) return;
    lamp_t *l = by_did(be32(p + 8));
    if (l == NULL) return; // a miIO device that wasn't imported
    if (n == 32) { // hello
        if (!l->online) ESP_LOGI(TAG, "%s answered (%s)", l->cfg.name, l->proto == HOME_PROTO_LEGACY ? "legacy" : "miot");
        l->online = true;
        l->stamp = be32(p + 12);
        l->stamp_at = now_ms();
        if (from != l->cfg.ip) {
            ESP_LOGI(TAG, "%s moved to a new address", l->cfg.name);
            l->cfg.ip = from; // stored with the next edit or import
            xSemaphoreTake(s_cfg_mux, portMAX_DELAY);
            for (uint32_t i = 0; i < s_import.count; i++) {
                if (s_import.lamps[i].did == l->cfg.did) s_import.lamps[i].ip = from;
            }
            xSemaphoreGive(s_cfg_mux);
        }
        return;
    }
    uint8_t sum[16];
    md5(p, 16, l->cfg.token, 16, p + 32, n - 32, sum);
    if (memcmp(sum, p + 16, 16) != 0 || (n - 32) % 16 != 0 || n - 32 >= (int)sizeof(s_json)) return;
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, l->key, 128);
    uint8_t iv[16];
    memcpy(iv, l->iv, 16);
    int len = n - 32;
    esp_aes_crypt_cbc(&ctx, ESP_AES_DECRYPT, len, iv, p + 32, (uint8_t *)s_json);
    esp_aes_free(&ctx);
    int pad = (uint8_t)s_json[len - 1];
    if (pad < 1 || pad > 16) return;
    len -= pad;
    while (len > 0 && s_json[len - 1] == '\0') len--; // some firmware ends the JSON with a NUL
    cJSON *j = cJSON_ParseWithLength(s_json, len);
    if (j == NULL) return;
    uint32_t id = (uint32_t)json_int(cJSON_GetObjectItem(j, "id"), 0);
    const cJSON *result = cJSON_GetObjectItem(j, "result");
    bool ok = result != NULL && cJSON_GetObjectItem(j, "error") == NULL;
    l->online = true;
    if (id == l->query_id && l->query_id) {
        l->query_id = 0;
        // A refresh doesn't overwrite a change still on its way.
        if (ok && cJSON_IsArray(result) && (!l->known || (!l->dirty && l->set_at == 0))) read_state(l, result);
        else if (!ok) ESP_LOGW(TAG, "%s: the query was refused", l->cfg.name);
    } else if (id == l->set_id && l->set_id) {
        l->set_id = 0;
        l->set_at = 0;
        l->failed = !ok;
        if (!ok) ESP_LOGW(TAG, "%s: the change was refused", l->cfg.name);
    }
    cJSON_Delete(j);
}

static void receive_all(void) {
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, s_pkt, sizeof(s_pkt), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) return;
        handle_packet(s_pkt, n, from.sin_addr.s_addr);
    }
}

// --- the lamps: load, scan, inputs ---
static void load_lamps(const home_store_t *st) {
    s_count = (int)st->count;
    for (int i = 0; i < s_count; i++) {
        lamp_t *l = &s_lamps[i];
        memset(l, 0, sizeof(*l));
        l->cfg = st->lamps[i];
        l->cfg.name[HOME_NAME_LEN - 1] = '\0';
        md5(l->cfg.token, 16, NULL, 0, NULL, 0, l->key);
        md5(l->key, 16, l->cfg.token, 16, NULL, 0, l->iv);
        l->proto = l->cfg.proto == HOME_PROTO_LEGACY ? HOME_PROTO_LEGACY : HOME_PROTO_MIOT;
        l->bright = 50;
        l->ct = l->cfg.ct_max > l->cfg.ct_min ? (uint16_t)((l->cfg.ct_min + l->cfg.ct_max) / 2) : 4000;
    }
    s_selected = 0;
    s_last = -1;
    s_ever_scanned = false;
}

// An edit stored (home_edit): the lamps in the new order, with their new names and icons, each
// keeping its session (online, its state); the selection stays on the same lamp.
static void patch_lamps(void) {
    uint32_t sel = s_selected < s_count ? s_lamps[s_selected].cfg.did : 0;
    uint32_t last = s_last >= 0 && s_last < s_count ? s_lamps[s_last].cfg.did : 0;
    int n = (int)s_import.count;
    for (int i = 0; i < n; i++) {
        const home_lamp_cfg_t *c = &s_import.lamps[i];
        lamp_t *old = by_did(c->did);
        if (old != NULL) {
            s_tmp[i] = *old;
            memcpy(s_tmp[i].cfg.name, c->name, HOME_NAME_LEN);
            s_tmp[i].cfg.kind = c->kind;
        } else { // not on the knob before (an edit can't add one, but be safe)
            memset(&s_tmp[i], 0, sizeof(s_tmp[i]));
            s_tmp[i].cfg = *c;
            md5(c->token, 16, NULL, 0, NULL, 0, s_tmp[i].key);
            md5(s_tmp[i].key, 16, c->token, 16, NULL, 0, s_tmp[i].iv);
            s_tmp[i].proto = c->proto == HOME_PROTO_LEGACY ? HOME_PROTO_LEGACY : HOME_PROTO_MIOT;
            s_tmp[i].bright = 50;
            s_tmp[i].ct = 4000;
        }
    }
    memcpy(s_lamps, s_tmp, sizeof(lamp_t) * (size_t)n);
    s_count = n;
    s_selected = 0;
    s_last = -1;
    for (int i = 0; i < n; i++) {
        if (s_lamps[i].cfg.did == sel) s_selected = i;
        if (s_lamps[i].cfg.did == last) s_last = i;
    }
    if (s_count == 0) s_phase = HOME_PHASE_EMPTY;
    else if (s_phase == HOME_PHASE_EDIT && s_lamps[s_selected].cfg.did != sel) s_phase = HOME_PHASE_LIST;
    memset(s_tmp, 0, sizeof(s_tmp)); // the tokens
}

static void start_scan(void) {
    s_phase = HOME_PHASE_SCAN;
    s_scan_t0 = now_ms();
    s_scan_hellos = 0;
    for (int i = 0; i < s_count; i++) {
        lamp_t *l = &s_lamps[i];
        l->online = false;
        l->known = false;
        l->query_id = 0;
        l->query_tries = 0;
        l->failed = false;
    }
    s_ever_scanned = true;
}

static int found_count(void) {
    int n = 0;
    for (int i = 0; i < s_count; i++) n += s_lamps[i].online;
    return n;
}

// The next option the lamp has after `from` (F1), or the first one when from < 0. -1: none.
static int next_option(const lamp_t *l, int from) {
    for (int k = 1; k <= HOME_OPT_COUNT; k++) {
        int o = ((from < 0 ? -1 : from) + k) % HOME_OPT_COUNT;
        if (has(l, (home_opt_t)o)) return o;
    }
    return -1;
}

static void mark(lamp_t *l, int prop) {
    s_last = (int)(l - s_lamps);
    l->dirty |= 1u << prop;
    if (prop != HOME_PROP_ON && !l->on) { // turning a value up on a lamp that's off switches it on
        l->on = true;
        l->dirty |= 1u << HOME_PROP_ON;
    }
}

static void apply_turn(int dir) {
    if (s_phase == HOME_PHASE_LIST) {
        s_selected = clampi(s_selected + dir, 0, s_count - 1);
        return;
    }
    if (s_phase != HOME_PHASE_EDIT) return;
    lamp_t *l = &s_lamps[s_selected];
    switch (s_option) {
        case HOME_OPT_BRIGHT: {
            int v = clampi((l->bright <= 1 && dir > 0 ? 0 : l->bright) + dir * BRIGHT_STEP, 1, 100);
            if (v != l->bright) l->bright = (uint8_t)v, mark(l, HOME_PROP_BRIGHT);
            break;
        }
        case HOME_OPT_TEMP: {
            int v = (l->ct / TEMP_STEP) * TEMP_STEP + dir * TEMP_STEP;
            v = clampi(v, l->cfg.ct_min, l->cfg.ct_max);
            if (v != l->ct || l->color_mode) l->ct = (uint16_t)v, l->color_mode = false, mark(l, HOME_PROP_TEMP);
            break;
        }
        default:
            l->hue = (uint16_t)((((l->hue / HUE_STEP) * HUE_STEP + dir * HUE_STEP) % 360 + 360) % 360);
            l->color_mode = true;
            mark(l, HOME_PROP_COLOR);
            break;
    }
}

static void plan_sweep(const lamp_t *only);

static void apply_key(int8_t e) {
    lamp_t *l = s_count > 0 ? &s_lamps[s_selected] : NULL;
    switch (e) {
        case EVT_F1:
            if (s_phase == HOME_PHASE_LIST && l && l->online && l->known) {
                int o = next_option(l, -1);
                if (o >= 0) s_phase = HOME_PHASE_EDIT, s_option = (home_opt_t)o;
            } else if (s_phase == HOME_PHASE_LIST && l && !l->online) {
                send_hello(l->cfg.ip); // one more try, and round its subnet in case it moved
                plan_sweep(l);
                l->hello_at = now_ms();
            } else if (s_phase == HOME_PHASE_EDIT && l) {
                int o = next_option(l, s_option);
                if (o >= 0) s_option = (home_opt_t)o;
            }
            break;
        case EVT_F2: // power, in the list and while editing
            if ((s_phase == HOME_PHASE_LIST || s_phase == HOME_PHASE_EDIT) && l && l->online && l->known) {
                l->on = !l->on;
                l->dirty |= 1u << HOME_PROP_ON;
                s_last = s_selected;
                l->sent_at = 0; // a key press goes at once
            }
            break;
        case EVT_F3:
            if (s_phase == HOME_PHASE_EDIT) s_phase = HOME_PHASE_LIST;
            else if (s_phase == HOME_PHASE_LIST) start_scan();
            break;
    }
}

// Sweeps the subnets the missing lamps were last seen on (not the knob's own: the broadcast covers
// that one, and 254 unicasts there would each need an ARP lookup).
static void plan_sweep(const lamp_t *only) {
    s_sweep_n = s_sweep_pos = 0;
    const uint32_t mine = ntohl(s_my_ip) & 0xFFFFFF00u;
    for (int i = 0; i < s_count; i++) {
        const lamp_t *l = &s_lamps[i];
        if (l->online || (only && l != only)) continue;
        uint32_t net = ntohl(l->cfg.ip) & 0xFFFFFF00u;
        bool dup = net == mine || net == 0;
        for (int k = 0; k < s_sweep_n && !dup; k++) dup = s_sweep_net[k] == net;
        if (!dup) s_sweep_net[s_sweep_n++] = net;
    }
}

static void sweep_tick(void) {
    for (int sent = 0; sent < SWEEP_BURST && s_sweep_pos < s_sweep_n * 254; sent++, s_sweep_pos++) {
        send_hello(htonl(s_sweep_net[s_sweep_pos / 254] | (uint32_t)(s_sweep_pos % 254 + 1)));
    }
}

// --- one pass of the home task while HOME is on screen ---
static void scan_tick(int64_t now) {
    // Hellos at 0, 0.5 and 1.2 s: a broadcast for the lamps on this subnet, and one to each
    // lamp's own address for the ones behind a router.
    static const int at[] = {0, 500, 1200};
    if (s_scan_hellos < 3 && now - s_scan_t0 >= at[s_scan_hellos]) {
        send_hello(htonl(INADDR_BROADCAST));
        for (int i = 0; i < s_count; i++) {
            if (!s_lamps[i].online) send_hello(s_lamps[i].cfg.ip);
        }
        s_scan_hellos++;
        if (s_scan_hellos == 2) plan_sweep(NULL); // after the first answers: whoever's still missing
    }
    bool all = s_count > 0;
    for (int i = 0; i < s_count; i++) all &= s_lamps[i].online && s_lamps[i].known;
    if (now - s_scan_t0 >= SCAN_MS || (all && now - s_scan_t0 >= SCAN_MIN_MS)) {
        s_phase = HOME_PHASE_LIST;
        if (s_selected >= s_count) s_selected = 0;
        ESP_LOGI(TAG, "scan done: %d of %d lamps answered", found_count(), s_count);
    }
}

static void lamp_tick(lamp_t *l, int64_t now) {
    if (l->online && !l->known && (l->query_id == 0 || now - l->query_at >= QUERY_RETRY_MS)) {
        if (l->query_tries >= 2 && l->query_tries % 2 == 0) {
            // Answers hello, not the queries: try the other protocol.
            l->proto = l->proto == HOME_PROTO_MIOT ? HOME_PROTO_LEGACY : HOME_PROTO_MIOT;
            ESP_LOGI(TAG, "%s: trying the %s protocol", l->cfg.name, l->proto == HOME_PROTO_LEGACY ? "legacy" : "miot");
        }
        if (l->query_tries < 6) send_query(l);
    }
    if (l->dirty && l->online && now - l->sent_at >= HOME_SEND_MS && l->set_at == 0) send_changes(l);
    else if (l->dirty && l->online && l->set_at && now - l->set_at >= HOME_SEND_MS * 2) send_changes(l); // don't wait forever on a lost reply
    if (l->set_at && now - l->set_at >= REPLY_WAIT_MS) {
        l->set_at = 0;
        l->set_id = 0;
        l->failed = true;
        send_hello(l->cfg.ip); // perhaps it restarted (a new clock) or moved
    }
    if (l->online && l->known && l->query_id && now - l->query_at >= REPLY_WAIT_MS * 2) {
        l->query_id = 0; // a refresh went unanswered: unplugged, say
        l->online = false;
        l->hello_at = now;
    }
    if (l->online && l->known && s_phase == HOME_PHASE_LIST && !l->dirty && l->set_at == 0
        && now - l->refresh_at >= REFRESH_MS) {
        l->refresh_at = now;
        l->query_tries = 0;
        send_query(l);
    }
    if (!l->online && s_phase == HOME_PHASE_LIST && now - l->hello_at >= OFFLINE_HELLO_MS) {
        send_hello(l->cfg.ip);
        l->hello_at = now;
    }
}

static void publish(void) {
    home_snapshot_t *b = &s_build;
    memset(b, 0, sizeof(*b));
    b->phase = s_phase;
    b->count = s_count;
    b->selected = s_selected;
    b->option = s_option;
    b->found = found_count();
    b->last = s_last;
    for (int i = 0; i < s_count; i++) {
        const lamp_t *l = &s_lamps[i];
        home_lamp_view_t *v = &b->lamps[i];
        memcpy(v->name, l->cfg.name, HOME_NAME_LEN);
        v->did = l->cfg.did;
        v->caps = l->cfg.caps;
        v->kind = l->cfg.kind < HOME_KIND_COUNT ? l->cfg.kind : HOME_KIND_BULB;
        v->online = l->online;
        v->known = l->known;
        v->on = l->on;
        v->failed = l->failed;
        v->bright = l->bright;
        v->ct = l->ct;
        v->ct_min = l->cfg.ct_min;
        v->ct_max = l->cfg.ct_max;
        v->hue = l->hue;
        bool colour = (l->cfg.caps & HOME_CAP_COLOR) && (l->color_mode || !(l->cfg.caps & HOME_CAP_TEMP));
        v->rgb = colour ? home_hue_rgb(l->hue) : (l->cfg.caps & HOME_CAP_TEMP) ? home_kelvin_rgb(l->ct) : home_kelvin_rgb(4000);
        v->ip = l->cfg.ip;
        v->proto = l->proto;
    }
    // The control loop's walls and feel.
    uint8_t ends = 0;
    if (s_phase == HOME_PHASE_LIST) {
        ends = (s_selected <= 0 ? 1 : 0) | (s_selected >= s_count - 1 ? 2 : 0);
    } else if (s_phase == HOME_PHASE_EDIT) {
        const lamp_t *l = &s_lamps[s_selected];
        if (s_option == HOME_OPT_BRIGHT) ends = (l->bright <= 1 ? 1 : 0) | (l->bright >= 100 ? 2 : 0);
        else if (s_option == HOME_OPT_TEMP) ends = (l->ct <= l->cfg.ct_min ? 1 : 0) | (l->ct >= l->cfg.ct_max ? 2 : 0);
    }
    atomic_store(&s_end_flags, ends);
    atomic_store(&s_haptic, s_phase == HOME_PHASE_EDIT ? HAPTIC_PROFILE_FINE : HAPTIC_PROFILE_COARSE);
    b->version = s_snap.version;
    if (memcmp(b, &s_snap, sizeof(*b)) == 0) return;
    b->version = s_snap.version + 1;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    memcpy(&s_snap, b, sizeof(*b));
    xSemaphoreGive(s_snap_mux);
    atomic_store(&s_version, b->version);
}

static void home_task(void *arg) {
    (void)arg;
    bool was_active = false;
    for (;;) {
        vTaskDelay(1); // 10 ms
        int64_t now = now_ms();
        if (atomic_exchange(&s_reload, false)) {
            xSemaphoreTake(s_cfg_mux, portMAX_DELAY);
            load_lamps(&s_import);
            xSemaphoreGive(s_cfg_mux);
            ESP_LOGI(TAG, "%d lamps imported", s_count);
            was_active = false; // look for the new list at once
            publish();
        }
        if (atomic_exchange(&s_patch, false)) {
            xSemaphoreTake(s_cfg_mux, portMAX_DELAY);
            patch_lamps();
            xSemaphoreGive(s_cfg_mux);
            publish();
        }
        bool active = menu_get_hid_type() == MENU_HID_HOME && !menu_is_open();
        int8_t e;
        if (!active) {
            while (take_evt(&e)) {}
            if (was_active) s_left_at = now;
            was_active = false;
            if (s_phase == HOME_PHASE_EDIT) s_phase = HOME_PHASE_LIST; // back from the menu: the list
            if (s_sock >= 0 && now - s_left_at >= IDLE_CLOSE_MS) sock_close();
            continue;
        }
        net_status_t ns;
        net_status(&ns);
        if (ns.state != NET_CONNECTED) {
            while (take_evt(&e)) {}
            s_phase = HOME_PHASE_NO_WIFI;
            was_active = false;
            sock_close();
            publish();
            continue;
        }
        if (s_count == 0) {
            while (take_evt(&e)) {}
            s_phase = HOME_PHASE_EMPTY;
            was_active = true;
            publish();
            continue;
        }
        if (!sock_open()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!was_active) {
            if (!s_ever_scanned || now - s_left_at >= RESCAN_AFTER_MS || s_phase == HOME_PHASE_NO_WIFI
                || s_phase == HOME_PHASE_EMPTY || s_phase == HOME_PHASE_OFF) {
                start_scan();
            }
            was_active = true;
        }
        s_my_ip = ns.ip;
        receive_all();
        sweep_tick();
        while (take_evt(&e)) {
            if (e == 1 || e == -1) apply_turn(e);
            else apply_key(e);
        }
        if (s_phase == HOME_PHASE_SCAN) scan_tick(now);
        for (int i = 0; i < s_count; i++) lamp_tick(&s_lamps[i], now);
        publish();
    }
}

// --- the screen ---
void home_get_snapshot(home_snapshot_t *out) {
    if (s_snap_mux == NULL) { // before home_start()
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    memcpy(out, &s_snap, sizeof(*out));
    xSemaphoreGive(s_snap_mux);
    if (menu_get_hid_type() != MENU_HID_HOME || menu_is_open()) out->phase = HOME_PHASE_OFF;
}

uint32_t home_version(void) { return atomic_load(&s_version); }

// --- the import (usb task) ---
void home_import_begin(void) {
    if (s_cfg_mux == NULL) return;
    xSemaphoreTake(s_cfg_mux, portMAX_DELAY);
    memset(&s_import, 0, sizeof(s_import));
    xSemaphoreGive(s_cfg_mux);
}

bool home_import_lamp(int slot, const home_lamp_cfg_t *lamp) {
    if (slot < 0 || slot >= HOME_MAX_LAMPS || s_cfg_mux == NULL) return false;
    xSemaphoreTake(s_cfg_mux, portMAX_DELAY);
    s_import.lamps[slot] = *lamp;
    s_import.lamps[slot].name[HOME_NAME_LEN - 1] = '\0';
    xSemaphoreGive(s_cfg_mux);
    return true;
}

// s_import to NVS. Caller holds s_cfg_mux.
static bool store_locked(void) {
    // NVS writes from internal RAM: s_import lives in PSRAM.
    home_store_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (st == NULL) return false;
    s_import.version = STORE_VERSION;
    memcpy(st, &s_import, sizeof(*st));
    nvs_handle_t h;
    bool ok = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(h, NVS_KEY, st, sizeof(*st)) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    memset(st, 0, sizeof(*st)); // the tokens
    heap_caps_free(st);
    return ok;
}

bool home_import_commit(int count) {
    if (count < 0 || count > HOME_MAX_LAMPS || s_cfg_mux == NULL) return false;
    xSemaphoreTake(s_cfg_mux, portMAX_DELAY);
    s_import.count = (uint32_t)count;
    bool ok = store_locked();
    xSemaphoreGive(s_cfg_mux);
    if (ok) {
        atomic_store(&s_stored_count, count);
        atomic_store(&s_reload, true);
    }
    ESP_LOGI(TAG, "import of %d lamps %s", count, ok ? "stored" : "NOT stored");
    return ok;
}

bool home_edit(int slot, uint32_t did, int what, int value, const char *name) {
    if (s_cfg_mux == NULL) return false;
    xSemaphoreTake(s_cfg_mux, portMAX_DELAY);
    int n = (int)s_import.count;
    bool ok = slot >= 0 && slot < n && s_import.lamps[slot].did == did;
    if (ok) {
        home_lamp_cfg_t *l = &s_import.lamps[slot];
        switch (what) {
            case HOME_EDIT_NAME:
                ok = name != NULL && name[0] != '\0';
                if (ok) {
                    memset(l->name, 0, HOME_NAME_LEN);
                    strncpy(l->name, name, HOME_NAME_LEN - 1);
                }
                break;
            case HOME_EDIT_KIND:
                ok = value >= 0 && value < HOME_KIND_COUNT;
                if (ok) l->kind = (uint8_t)value;
                break;
            case HOME_EDIT_MOVE: {
                ok = value >= 0 && value < n;
                if (!ok || value == slot) break;
                home_lamp_cfg_t moved = *l;
                if (value < slot) memmove(&s_import.lamps[value + 1], &s_import.lamps[value], sizeof(moved) * (size_t)(slot - value));
                else memmove(&s_import.lamps[slot], &s_import.lamps[slot + 1], sizeof(moved) * (size_t)(value - slot));
                s_import.lamps[value] = moved;
                memset(&moved, 0, sizeof(moved));
                break;
            }
            case HOME_EDIT_REMOVE:
                memmove(&s_import.lamps[slot], &s_import.lamps[slot + 1], sizeof(*l) * (size_t)(n - 1 - slot));
                memset(&s_import.lamps[n - 1], 0, sizeof(*l));
                s_import.count = (uint32_t)(n - 1);
                break;
            default:
                ok = false;
        }
    }
    if (ok) ok = store_locked();
    int count = (int)s_import.count;
    xSemaphoreGive(s_cfg_mux);
    if (ok) {
        atomic_store(&s_stored_count, count);
        atomic_store(&s_patch, true);
    }
    ESP_LOGI(TAG, "edit %d of lamp %d %s", what, slot, ok ? "stored" : "refused");
    return ok;
}

int home_lamp_count(void) { return atomic_load(&s_stored_count); }

void home_start(void) {
    s_cfg_mux = xSemaphoreCreateMutex();
    s_snap_mux = xSemaphoreCreateMutex();
    home_store_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (st != NULL) {
        nvs_handle_t h;
        size_t n = sizeof(*st);
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_blob(h, NVS_KEY, st, &n) == ESP_OK && n == sizeof(*st) && st->version == STORE_VERSION
                && st->count <= HOME_MAX_LAMPS) {
                memcpy(&s_import, st, sizeof(*st));
                atomic_store(&s_stored_count, (int)st->count);
                load_lamps(&s_import);
                ESP_LOGI(TAG, "%d lamps", s_count);
            }
            nvs_close(h);
        }
        memset(st, 0, sizeof(*st));
        heap_caps_free(st);
    }
    s_msg_id = (esp_random() & 0xFFFF) + 1;
    publish(); // the lamps' names for EXT_HOME_STATUS before HOME is first opened
    // PSRAM stack: it never writes flash (the import's NVS write is the usb task's), and its
    // packets for the AES driver are in internal RAM (s_pkt, s_json).
    xTaskCreatePinnedToCoreWithCaps(home_task, "home", 5120, NULL, PRIO_NET, NULL, CORE_IO, MALLOC_CAP_SPIRAM);
}
