#include "net_link.h"
#include "host_link.h"
#include "net.h"
#include "net_pend.h"
#include "tasks_common.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static const char *TAG = "net_link";

_Static_assert(NET_LINK_SESSIONS == HOST_LINK_COUNT - HOST_LINK_NET, "a host link for each WiFi client");

#define REPORT 64
#define TAG_LEN 16
#define WIRE (REPORT + TAG_LEN)
#define TXQ_DEPTH 64     // stream reports on their way out (the screen fills it, then waits)
#define REPLYQ_DEPTH 16  // replies, sent first
#define TX_BATCH 16      // encrypted and written together
#define HELLO_MS 2000    // a new connection proves the key within this, or it's closed
#define PENDING 4        // handshakes going on at once (net_pend.h: one per peer address)
#define IO_TIMEOUT_S 3   // a write that can't finish means the client is gone

static int s_wake = -1; // eventfd: host_link queued a report, or the key changed
static _Atomic bool s_drop = false;
static portMUX_TYPE s_key_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_key[NET_KEY_BYTES]; // under s_key_mux
static bool s_have_key;

// A client that proved the key: its own host link (HOST_LINK_NET + slot), queues (in PSRAM: the
// tasks that use them never run with the cache off) and counters. `live` and the queues are any
// task's; the rest is the net_link task's.
typedef struct {
    QueueHandle_t txq, replyq;
    _Atomic bool live;
    int fd;
    psa_key_id_t aead;
    uint64_t tx_seq, rx_seq;
    uint8_t rx[WIRE * 4]; // a frame can arrive in pieces
    size_t rx_have;
    int64_t last_rx; // when it last sent a frame: the quietest goes when a third one comes in
} session_t;
static session_t s_ses[NET_LINK_SESSIONS];

static void wake(void) {
    uint64_t one = 1;
    if (s_wake >= 0) write(s_wake, &one, sizeof(one));
}

// --- the key ---

bool net_link_key(uint8_t key[NET_KEY_BYTES], bool fresh) {
    uint8_t k[NET_KEY_BYTES];
    size_t n = sizeof(k);
    nvs_handle_t h;
    if (nvs_open("net", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = !fresh && nvs_get_blob(h, "key", k, &n) == ESP_OK && n == sizeof(k);
    if (!ok) {
        esp_fill_random(k, sizeof(k)); // the hardware RNG: true random with the radio on
        ok = nvs_set_blob(h, "key", k, sizeof(k)) == ESP_OK && nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);
    if (!ok) return false;
    portENTER_CRITICAL(&s_key_mux);
    memcpy(s_key, k, sizeof(k));
    s_have_key = true;
    portEXIT_CRITICAL(&s_key_mux);
    if (fresh) { // whoever paired with the old key is out
        atomic_store(&s_drop, true);
        wake();
    }
    memcpy(key, k, sizeof(k));
    return true;
}

static void load_key(void) {
    nvs_handle_t h;
    size_t n = sizeof(s_key);
    if (nvs_open("net", NVS_READONLY, &h) != ESP_OK) return;
    s_have_key = nvs_get_blob(h, "key", s_key, &n) == ESP_OK && n == sizeof(s_key);
    nvs_close(h);
}

// --- crypto (PSA: the chip's SHA and AES engines) ---

static bool hmac(const uint8_t *key, const uint8_t *msg, size_t len, uint8_t out[32]) {
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&a, 256);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_key_id_t id;
    if (psa_import_key(&a, key, 32, &id) != PSA_SUCCESS) return false;
    size_t n = 0;
    psa_status_t st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, len, out, 32, &n);
    psa_destroy_key(id);
    return st == PSA_SUCCESS && n == 32;
}

static bool same(const uint8_t *a, const uint8_t *b, size_t n) { // constant time
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

static void nonce(uint8_t n[12], char dir, uint64_t seq) {
    n[0] = (uint8_t)dir;
    memcpy(n + 1, &seq, 8);
    n[9] = n[10] = n[11] = 0;
}

// --- the connection ---

static bool send_all(int s, const uint8_t *buf, size_t n) {
    for (size_t done = 0; done < n;) {
        int r = send(s, buf + done, n - done, 0);
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}

// Connections that haven't proved the key yet (the handshake, net_link.h). The loop that serves
// the client steps each as its bytes arrive, and each has HELLO_MS in all: a peer that stalls or
// drips a byte at a time never holds up the client that's in, and the handshakes go on side by
// side -- a peer that floods only ever replaces its own (net_pend_pick).
static net_pend_t s_pend[PENDING];

// A line a second at most: a flood of strangers mustn't flood the console too.
static void warn_dropped(const char *why) {
    static int64_t s_next;
    static unsigned s_quiet;
    int64_t now = esp_timer_get_time();
    if (now < s_next) {
        s_quiet++;
        return;
    }
    if (s_quiet) ESP_LOGW(TAG, "a connection %s: closed (and %u more since)", why, s_quiet);
    else ESP_LOGW(TAG, "a connection %s: closed", why);
    s_quiet = 0;
    s_next = now + 1000000;
}

static void drop_pending(net_pend_t *p, const char *why) {
    if (p->fd < 0) return;
    close(p->fd);
    p->fd = -1;
    memset(p->k, 0, sizeof(p->k));
    if (why) warn_dropped(why);
}

static void drop_all_pending(void) {
    for (int i = 0; i < PENDING; i++) drop_pending(&s_pend[i], NULL);
}

static bool any_live(void) {
    for (int i = 0; i < NET_LINK_SESSIONS; i++)
        if (s_ses[i].fd >= 0) return true;
    return false;
}

static void close_session(int i) {
    session_t *c = &s_ses[i];
    if (c->fd < 0) return;
    atomic_store(&c->live, false);
    close(c->fd);
    c->fd = -1;
    if (c->aead) {
        psa_destroy_key(c->aead);
        c->aead = 0;
    }
    host_link_stop_link((host_link_t)(HOST_LINK_NET + i)); // its streams stop, its replies go
    if (!any_live()) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "companion gone (slot %d)", i);
}

static void close_all(void) {
    for (int i = 0; i < NET_LINK_SESSIONS; i++) close_session(i);
}

static void accept_pending(int lsock) {
    struct sockaddr_in from = {0};
    socklen_t len = sizeof(from);
    int c = accept(lsock, (struct sockaddr *)&from, &len);
    if (c < 0) { // out of sockets or memory: it stays in the backlog, so don't spin on it
        vTaskDelay(1);
        return;
    }
    net_pend_t *p = &s_pend[net_pend_pick(s_pend, PENDING, from.sin_addr.s_addr)];
    drop_pending(p, NULL);
    *p = (net_pend_t){.fd = c, .addr = from.sin_addr.s_addr, .since = esp_timer_get_time()};
}

// A pending connection proved the key: it takes a free slot, else the one that's been quiet the
// longest (a live companion polls every second, so that's one that left without closing). Not by
// address: the app and the Mac service come from the same computer.
static void promote(net_pend_t *p) {
    int c = p->fd;
    p->fd = -1;
    int slot = 0;
    for (int i = 0; i < NET_LINK_SESSIONS; i++) {
        if (s_ses[i].fd < 0) {
            slot = i;
            break;
        }
        if (s_ses[i].last_rx < s_ses[slot].last_rx) slot = i;
    }
    close_session(slot);
    session_t *ses = &s_ses[slot];
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    bool ok = psa_import_key(&a, p->k, sizeof(p->k), &ses->aead) == PSA_SUCCESS;
    memset(p->k, 0, sizeof(p->k));
    if (!ok) {
        close(c);
        return;
    }
    // A write that can't finish means the client is gone. A client that vanished (asleep, out of
    // range) without closing: found within ~25 s.
    struct timeval tv = {.tv_sec = IO_TIMEOUT_S};
    int one = 1, idle = 10, intvl = 5, cnt = 3;
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(c, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(c, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    xQueueReset(ses->txq);
    xQueueReset(ses->replyq);
    ses->fd = c;
    ses->tx_seq = ses->rx_seq = 0;
    ses->rx_have = 0;
    ses->last_rx = esp_timer_get_time();
    esp_wifi_set_ps(WIFI_PS_NONE); // a companion is in: answer in milliseconds, not at the next beacon
    atomic_store(&ses->live, true);
    ESP_LOGI(TAG, "companion in (slot %d)", slot);
}

// A pending connection's next bytes: its hello (then our half goes out, in one non-blocking
// write -- 36 bytes on a fresh connection always fit), then its proof.
static void step_pending(net_pend_t *p) {
    size_t want = p->replied ? 16 : 20;
    int r = recv(p->fd, p->buf + p->have, want - p->have, MSG_DONTWAIT);
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        drop_pending(p, "that went away mid-handshake");
        return;
    }
    if (r < 0 || (p->have += (size_t)r) < want) return;
    p->have = 0;
    uint8_t proof[32];
    if (p->replied) {
        if (hmac(p->k, (const uint8_t *)"client", 6, proof) && same(proof, p->buf, 16)) promote(p);
        else drop_pending(p, "that couldn't prove the key");
        return;
    }
    uint8_t key[NET_KEY_BYTES], msg[46], reply[36];
    portENTER_CRITICAL(&s_key_mux);
    bool have = s_have_key;
    memcpy(key, s_key, sizeof(key));
    portEXIT_CRITICAL(&s_key_mux);
    bool ok = have && memcmp(p->buf, "QDR1", 4) == 0;
    if (ok) {
        memcpy(msg, "quadra session", 14);
        memcpy(msg + 14, p->buf + 4, 16);
        esp_fill_random(msg + 30, 16);
        memcpy(reply, "QDR1", 4);
        memcpy(reply + 4, msg + 30, 16);
        ok = hmac(key, msg, sizeof(msg), p->k) && hmac(p->k, (const uint8_t *)"knob", 4, proof);
        memcpy(reply + 20, proof, 16);
    }
    memset(key, 0, sizeof(key));
    if (!ok || send(p->fd, reply, sizeof(reply), MSG_DONTWAIT) != (int)sizeof(reply)) {
        drop_pending(p, have ? "that isn't a companion" : "before pairing");
        return;
    }
    p->replied = true;
}

// Everything waiting, replies first, encrypted, TX_BATCH reports a write, until both queues are
// empty (so a report queued after this returns wakes the task). false: the client is gone.
static bool flush_tx(session_t *c) {
    static uint8_t buf[WIRE * TX_BATCH];
    uint8_t plain[REPORT], n12[12];
    for (;;) {
        size_t n = 0, out = 0;
        while (n < TX_BATCH && (xQueueReceive(c->replyq, plain, 0) == pdTRUE || xQueueReceive(c->txq, plain, 0) == pdTRUE)) {
            nonce(n12, 'K', c->tx_seq++);
            if (psa_aead_encrypt(c->aead, PSA_ALG_GCM, n12, sizeof(n12), NULL, 0, plain, REPORT, buf + n * WIRE, WIRE, &out)
                != PSA_SUCCESS || out != WIRE) return false;
            n++;
        }
        if (n == 0) return true;
        if (!send_all(c->fd, buf, n * WIRE)) return false;
    }
}

// What the client sent: whole frames, decrypted, to host_link. false: a bad frame, or gone.
static bool read_rx(int slot) {
    session_t *c = &s_ses[slot];
    uint8_t *buf = c->rx;
    size_t have = c->rx_have;
    int r = recv(c->fd, buf + have, sizeof(c->rx) - have, MSG_DONTWAIT);
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) return false;
    if (r > 0) have += (size_t)r;
    size_t at = 0;
    while (have - at >= WIRE) {
        uint8_t plain[REPORT], n12[12];
        size_t out = 0;
        if (atomic_load(&s_drop)) return false; // rekeyed: not one more frame of this session
        nonce(n12, 'C', c->rx_seq++);
        if (psa_aead_decrypt(c->aead, PSA_ALG_GCM, n12, sizeof(n12), NULL, 0, buf + at, WIRE, plain, REPORT, &out)
            != PSA_SUCCESS || out != REPORT) {
            ESP_LOGW(TAG, "a frame that didn't verify: closed");
            return false;
        }
        c->last_rx = esp_timer_get_time();
        host_link_receive((host_link_t)(HOST_LINK_NET + slot), plain, REPORT);
        at += WIRE;
    }
    memmove(buf, buf + at, have - at);
    c->rx_have = have - at;
    return true;
}

static int listen_on(void) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(NET_LINK_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, PENDING) != 0) {
        close(s);
        return -1;
    }
    return s;
}

static void net_link_task(void *arg) {
    int lsock = -1;
    for (;;) {
        net_status_t st;
        net_status(&st);
        if (st.state != NET_CONNECTED) { // no WiFi: nothing to serve
            close_all();
            drop_all_pending();
            if (lsock >= 0) {
                close(lsock);
                lsock = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (lsock < 0 && (lsock = listen_on()) < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (atomic_exchange(&s_drop, false)) { // a new key: whoever holds the old one is out
            close_all();
            drop_all_pending();
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lsock, &rd);
        FD_SET(s_wake, &rd);
        int top = lsock > s_wake ? lsock : s_wake;
        int conn[NET_LINK_SESSIONS], pend[PENDING];
        for (int i = 0; i < NET_LINK_SESSIONS; i++) {
            if ((conn[i] = s_ses[i].fd) < 0) continue;
            FD_SET(conn[i], &rd);
            if (conn[i] > top) top = conn[i];
        }
        int64_t now = esp_timer_get_time(), wake_at = now + 1000000; // WiFi's state, between events
        for (int i = 0; i < PENDING; i++) {
            net_pend_t *p = &s_pend[i];
            int64_t deadline = p->since + HELLO_MS * 1000LL;
            if (p->fd >= 0 && now >= deadline) drop_pending(p, "too slow to prove the key");
            if ((pend[i] = p->fd) < 0) continue;
            FD_SET(p->fd, &rd);
            if (p->fd > top) top = p->fd;
            if (deadline < wake_at) wake_at = deadline;
        }
        struct timeval tv = {.tv_sec = (wake_at - now) / 1000000, .tv_usec = (suseconds_t)((wake_at - now) % 1000000)};
        int n = select(top + 1, &rd, NULL, NULL, &tv);
        if (n < 0) { // shouldn't happen; never spin on it
            ESP_LOGW(TAG, "select: errno %d", errno);
            close_all();
            drop_all_pending();
            close(lsock);
            lsock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (n > 0 && FD_ISSET(s_wake, &rd)) {
            uint64_t v;
            read(s_wake, &v, sizeof(v));
        }
        if (atomic_exchange(&s_drop, false)) { // rekeyed while this waited: not one more frame
            close_all();
            drop_all_pending();
        }
        for (int i = 0; n > 0 && i < NET_LINK_SESSIONS; i++)
            if (conn[i] >= 0 && conn[i] == s_ses[i].fd && FD_ISSET(conn[i], &rd) && !read_rx(i)) close_session(i);
        for (int i = 0; n > 0 && i < PENDING; i++)
            if (pend[i] >= 0 && pend[i] == s_pend[i].fd && FD_ISSET(pend[i], &rd)) step_pending(&s_pend[i]);
        if (n > 0 && FD_ISSET(lsock, &rd)) accept_pending(lsock);
        for (int i = 0; i < NET_LINK_SESSIONS; i++)
            if (s_ses[i].fd >= 0 && !flush_tx(&s_ses[i])) close_session(i);
    }
}

void net_link_start(void) {
    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "no crypto: no companion over WiFi");
        return;
    }
    esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    esp_err_t e = esp_vfs_eventfd_register(&cfg);
    if ((e != ESP_OK && e != ESP_ERR_INVALID_STATE) || (s_wake = eventfd(0, 0)) < 0) {
        ESP_LOGE(TAG, "no eventfd: no companion over WiFi");
        return;
    }
    load_key();
    for (int i = 0; i < PENDING; i++) s_pend[i].fd = -1;
    for (int i = 0; i < NET_LINK_SESSIONS; i++) {
        s_ses[i].fd = -1;
        s_ses[i].txq = xQueueCreateWithCaps(TXQ_DEPTH, REPORT, MALLOC_CAP_SPIRAM);
        s_ses[i].replyq = xQueueCreateWithCaps(REPLYQ_DEPTH, REPORT, MALLOC_CAP_SPIRAM);
        if (s_ses[i].txq == NULL || s_ses[i].replyq == NULL) {
            ESP_LOGE(TAG, "no memory for the queues: no companion over WiFi");
            return;
        }
    }
    xTaskCreatePinnedToCore(net_link_task, "net_link", 6144, NULL, PRIO_NET_LINK, NULL, CORE_IO);
}

bool net_link_ready(int slot) {
    session_t *c = &s_ses[slot];
    return c->txq != NULL && atomic_load(&c->live) && uxQueueSpacesAvailable(c->txq) > 0;
}

bool net_link_send(int slot, const uint8_t *report) {
    session_t *c = &s_ses[slot];
    if (!atomic_load(&c->live) || xQueueSend(c->txq, report, 0) != pdTRUE) return false;
    if (uxQueueMessagesWaiting(c->txq) == 1) wake(); // it was empty: the task may be asleep
    return true;
}

bool net_link_reply(int slot, const uint8_t *report) {
    session_t *c = &s_ses[slot];
    if (c->replyq == NULL || !atomic_load(&c->live) || xQueueSend(c->replyq, report, 0) != pdTRUE) return false;
    wake();
    return true;
}

bool net_link_connected(int slot) {
    return atomic_load(&s_ses[slot].live);
}
