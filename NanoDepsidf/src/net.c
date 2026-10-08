#include "net.h"
#include "net_link.h"
#include "clock.h"
#include "tasks_common.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "net";

#define NVS_NS "net"
// 11 dBm, not the radio's 20: the legacy firmware found full power's supply spikes upset the
// LED strips' RMT timing. Plenty for a desk.
#define TX_POWER_QDBM 44
#define RADIO_MA 100.0f // average while on (modem sleep); TX peaks are short
static const uint8_t BACKOFF_S[] = {1, 2, 5, 10, 30};

// What wakes net_task (notification bits).
#define EV_APPLY (1u << 0)   // the setup changed
#define EV_STARTED (1u << 1) // the radio is up
#define EV_DROPPED (1u << 2) // disconnected (s_reason says why)
#define EV_GOT_IP (1u << 3)

static TaskHandle_t s_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static net_status_t s_st;              // under s_mux
static char s_pass[NET_PASS_MAX + 1];  // under s_mux
static _Atomic uint16_t s_reason;
static _Atomic bool s_radio_on = false, s_time_set = false;
static _Atomic bool s_leaving = false; // the next disconnect is our own (a new setup)
static esp_netif_t *s_netif;

// --- the event task (core 0, the control task's priority): note it, wake net_task ---

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    uint32_t ev = 0;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ev = EV_STARTED;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&s_reason, ((const wifi_event_sta_disconnected_t *)data)->reason);
        ev = EV_DROPPED;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        uint32_t ip = ((const ip_event_got_ip_t *)data)->ip_info.ip.addr;
        portENTER_CRITICAL(&s_mux);
        s_st.ip = ip;
        portEXIT_CRITICAL(&s_mux);
        ev = EV_GOT_IP;
    }
    if (ev) xTaskNotify(s_task, ev, eSetBits);
}

static void on_time(struct timeval *tv) {
    atomic_store(&s_time_set, true); // lwIP's task (core 1)
    clock_note_sync();
}

// --- net_task (core 1) ---

static void set_state(net_state_t st) {
    portENTER_CRITICAL(&s_mux);
    s_st.state = st;
    if (st != NET_CONNECTED) {
        s_st.ip = 0;
        s_st.rssi = 0;
    }
    portEXIT_CRITICAL(&s_mux);
}

#define TRY(x)                                                          \
    do {                                                                \
        esp_err_t e_ = (x);                                             \
        if (e_ != ESP_OK) {                                             \
            ESP_LOGE(TAG, "%s: %s -- WiFi stays off", #x, esp_err_to_name(e_)); \
            return false;                                               \
        }                                                               \
    } while (0)

// The stack, once, on the first switch-on: nothing of WiFi is allocated before that. A
// failure leaves WiFi off rather than aborting (abort at boot would be a reboot loop).
static bool bring_up(void) {
    TRY(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) TRY(e);
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) return false;
    esp_netif_set_hostname(s_netif, s_st.host);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0; // the setup lives in our own NVS namespace
    TRY(esp_wifi_init(&cfg)); // from this core-1 task: the radio's interrupt lands on core 1
    TRY(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    TRY(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
    TRY(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    return true;
}

static bool apply(void) {
    static bool up = false;
    net_status_t st;
    char pass[NET_PASS_MAX + 1];
    portENTER_CRITICAL(&s_mux);
    st = s_st;
    memcpy(pass, s_pass, sizeof(pass));
    portEXIT_CRITICAL(&s_mux);

    if (!st.enabled || !st.ssid[0]) {
        if (atomic_exchange(&s_radio_on, false)) esp_wifi_stop();
        set_state(NET_OFF);
        return true;
    }
    if (!up && !(up = bring_up())) {
        set_state(NET_OFF);
        return false;
    }
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, st.ssid, strnlen(st.ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, pass, strnlen(pass, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN; // WPA3 passes too
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    if (atomic_load(&s_radio_on) && esp_wifi_disconnect() == ESP_OK) atomic_store(&s_leaving, true);
    TRY(esp_wifi_set_config(WIFI_IF_STA, &wc));
    set_state(NET_CONNECTING);
    if (!atomic_load(&s_radio_on)) {
        TRY(esp_wifi_start()); // -> WIFI_EVENT_STA_START -> connect
        atomic_store(&s_radio_on, true);
        esp_wifi_set_max_tx_power(TX_POWER_QDBM);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else {
        esp_wifi_connect();
    }
    ESP_LOGI(TAG, "connecting to \"%s\"", st.ssid);
    return true;
}

static void services(void) {
    static bool on = false;
    if (on) return;
    on = true;
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sc.sync_cb = on_time;
    if (esp_netif_sntp_init(&sc) != ESP_OK) ESP_LOGW(TAG, "SNTP didn't start");
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_st.host);
        mdns_instance_name_set("Quadra");
        mdns_service_add(NULL, "_quadra", "_tcp", NET_LINK_PORT, NULL, 0); // the companion finds it
    } else {
        ESP_LOGW(TAG, "mDNS didn't start");
    }
}

static void net_task(void *arg) {
    unsigned fails = 0;
    TickType_t retry_at = 0;
    bool retry = false;
    for (;;) {
        uint32_t ev = 0;
        TickType_t wait = pdMS_TO_TICKS(2000);
        if (retry) {
            int32_t left = (int32_t)(retry_at - xTaskGetTickCount());
            wait = left > 0 ? (TickType_t)left : 0;
        }
        xTaskNotifyWait(0, UINT32_MAX, &ev, wait);

        if (ev & EV_APPLY) {
            retry = false;
            fails = 0;
            apply();
        }
        if (ev & EV_STARTED) esp_wifi_connect();
        if (ev & EV_DROPPED) {
            uint16_t why = atomic_load(&s_reason);
            bool ours = atomic_exchange(&s_leaving, false);
            if (!ours && atomic_load(&s_radio_on)) {
                bool bad_pass = why == WIFI_REASON_AUTH_FAIL || why == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                             || why == WIFI_REASON_HANDSHAKE_TIMEOUT || why == WIFI_REASON_MIC_FAILURE;
                bool absent = why == WIFI_REASON_NO_AP_FOUND || (why >= WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
                                                                  && why <= WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD);
                set_state(absent ? NET_NO_SSID : bad_pass ? NET_BAD_PASS : NET_CONNECTING);
                unsigned s = BACKOFF_S[fails < sizeof(BACKOFF_S) ? fails : sizeof(BACKOFF_S) - 1];
                fails++;
                retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(s * 1000);
                retry = true;
                ESP_LOGI(TAG, "disconnected (reason %u), again in %us", why, s);
            }
        }
        if (ev & EV_GOT_IP) {
            fails = 0;
            retry = false;
            portENTER_CRITICAL(&s_mux);
            s_st.state = NET_CONNECTED;
            uint32_t ip = s_st.ip;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "connected: " IPSTR " (%s.local)", IP2STR((esp_ip4_addr_t *)&ip), s_st.host);
            services();
        }
        if (retry && (int32_t)(xTaskGetTickCount() - retry_at) >= 0) {
            retry = false;
            if (atomic_load(&s_radio_on)) esp_wifi_connect();
        }
        if (atomic_load(&s_radio_on)) {
            int rssi = 0;
            if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
                portENTER_CRITICAL(&s_mux);
                s_st.rssi = (int8_t)rssi;
                portEXIT_CRITICAL(&s_mux);
            }
        }
    }
}

// --- the rest of the firmware ---

static void load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return; // never set up
    size_t n = sizeof(s_st.ssid);
    if (nvs_get_str(h, "ssid", s_st.ssid, &n) != ESP_OK) s_st.ssid[0] = '\0';
    n = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) s_pass[0] = '\0';
    uint8_t on = 0;
    nvs_get_u8(h, "on", &on);
    s_st.enabled = on;
    nvs_close(h);
}

void net_start(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_st.host, sizeof(s_st.host), "quadra-%02x%02x", mac[4], mac[5]);
    load();
    xTaskCreatePinnedToCore(net_task, "net", 4096, NULL, PRIO_NET, &s_task, CORE_IO);
    if (s_st.enabled && s_st.ssid[0]) xTaskNotify(s_task, EV_APPLY, eSetBits);
}

void net_status(net_status_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
    out->time_set = atomic_load(&s_time_set);
}

bool net_configure(const char *ssid, const char *pass, bool enabled) {
    if ((ssid && strlen(ssid) > NET_SSID_MAX) || (pass && strlen(pass) > NET_PASS_MAX)) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (!ssid || nvs_set_str(h, "ssid", ssid) == ESP_OK) && (!pass || nvs_set_str(h, "pass", pass) == ESP_OK)
           && nvs_set_u8(h, "on", enabled) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) return false;
    portENTER_CRITICAL(&s_mux);
    if (ssid) strcpy(s_st.ssid, ssid);
    if (pass) strcpy(s_pass, pass);
    s_st.enabled = enabled;
    s_st.state = enabled && s_st.ssid[0] ? NET_CONNECTING : NET_OFF; // what the reply says: net_task follows
    if (s_st.state != NET_CONNECTED) s_st.ip = 0;
    portEXIT_CRITICAL(&s_mux);
    if (s_task) xTaskNotify(s_task, EV_APPLY, eSetBits);
    return true;
}

float net_current_ma(void) {
    return atomic_load(&s_radio_on) ? RADIO_MA : 0.0f;
}
