#include "midi.h"
#include "board_pins.h"
#include "haptic_params.h"
#include "menu.h"
#include "tasks_common.h"
#include "ui_state.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "midi";

#define MIDI_UART UART_NUM_1 // UART0's pins (43 / 44) are the jacks'; the console is on USB
#define MIDI_BAUD 31250
#define UART_RX_BUF 256      // the driver wants more than the 128-byte FIFO
#define HOLD_MS 400          // F1 held this long shows the list (a turn while held shows it at once)
// Turning speed -> step (ms between detents): slow is fine, a flick covers the range.
#define TURN_FINE_MS 150
#define TURN_NORMAL_MS 60
#define TURN_FAST_MS 25

// --- Core 0 -> midi task: one word per input, single producer, single consumer ---
// Bits 0-7: the event (a turn is -1 / +1). Bits 8-31: when, in units of 1.024 ms (a shift, not a
// 64-bit division, on the control loop), wrapping every ~4.8 h; only differences are used.
#define EVT_RING 64
enum { EVT_F1_DOWN = 11, EVT_F1_UP = 12, EVT_F2 = 13, EVT_F3 = 14 };
static uint32_t s_evt[EVT_RING]; // internal RAM: the control loop writes it while flash may be busy
static _Atomic uint32_t s_evt_head = 0, s_evt_tail = 0;
static _Atomic uint8_t s_end_flags = 0; // bit 0: nothing further back, bit 1: nothing further on
static _Atomic int s_haptic = HAPTIC_PROFILE_FINE;

static void CONTROL_HOT push_evt(int8_t e) {
    uint32_t h = atomic_load_explicit(&s_evt_head, memory_order_relaxed);
    if (h - atomic_load_explicit(&s_evt_tail, memory_order_acquire) >= EVT_RING) return; // full: dropped
    uint32_t t = (uint32_t)(esp_timer_get_time() >> 10);
    s_evt[h % EVT_RING] = t << 8 | (uint8_t)e;
    atomic_store_explicit(&s_evt_head, h + 1, memory_order_release);
}
void CONTROL_HOT midi_input_rotate(int8_t dir) { push_evt(dir > 0 ? 1 : -1); }
void CONTROL_HOT midi_input_key(uint8_t key, bool down) {
    if (key == UI_BTN_F1) push_evt(down ? EVT_F1_DOWN : EVT_F1_UP);
    else if (down) push_evt(key == UI_BTN_F2 ? EVT_F2 : EVT_F3);
}
bool CONTROL_HOT midi_at_end(int8_t dir) {
    return atomic_load_explicit(&s_end_flags, memory_order_relaxed) & (dir > 0 ? 2 : 1);
}
int CONTROL_HOT midi_haptic_profile(void) { return atomic_load_explicit(&s_haptic, memory_order_relaxed); }

static bool take_evt(uint32_t *e) {
    uint32_t t = atomic_load_explicit(&s_evt_tail, memory_order_relaxed);
    if (t == atomic_load_explicit(&s_evt_head, memory_order_acquire)) return false;
    *e = s_evt[t % EVT_RING];
    atomic_store_explicit(&s_evt_tail, t + 1, memory_order_release);
    return true;
}

// --- midi task state ---
static int s_synth = -1;   // the profile the values below belong to
static const midi_synth_t *s_synth_ptr = NULL; // and its data: an edit from the companion replaces it
static _Atomic int s_goto = -1; // the companion's "show on the knob": a parameter to move to
static int s_param = 0;
EXT_RAM_BSS_ATTR static int16_t s_val[MIDI_MAX_PARAMS]; // -1: unknown
static bool s_f1_down = false, s_browsed = false;
static uint32_t s_f1_at = 0;      // 1.024 ms units, like the events
static uint32_t s_last_turn = 0;
static int s_prog = -1;
static uint32_t s_prog_ms = 0;
static uint8_t s_channel = 1;
static bool s_uart = false;
static uint32_t s_tx = 0, s_rx = 0;
static int s_lsb = -1;             // KORG10: the low 3 bits from the last CC 63, -1: none since

EXT_RAM_BSS_ATTR static midi_snapshot_t s_snap;
static SemaphoreHandle_t s_snap_mux;
static _Atomic uint32_t s_version = 0;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static const midi_synth_t *synth(void) { return midi_synth_get(s_synth); }

static void forget_values(void) {
    for (int i = 0; i < MIDI_MAX_PARAMS; i++) s_val[i] = -1;
    s_lsb = -1;
}

// --- the ports ---
static void uart_start(void) {
    const uart_config_t cfg = {
        .baud_rate = MIDI_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(MIDI_UART, UART_RX_BUF, 0, 0, NULL, 0) != ESP_OK
        || uart_param_config(MIDI_UART, &cfg) != ESP_OK
        || uart_set_pin(MIDI_UART, PIN_SERIAL2_TX, PIN_SERIAL2_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "TRS port not started");
        return;
    }
    gpio_pullup_en(PIN_SERIAL2_RX); // nothing plugged into MIDI IN: idle, not noise
    s_uart = true;
    ESP_LOGI(TAG, "TRS port up (TX GPIO %d, RX GPIO %d, %d baud)", PIN_SERIAL2_TX, PIN_SERIAL2_RX, MIDI_BAUD);
}

static void send(const uint8_t *m, int n) {
    if (tud_midi_mounted()) tud_midi_stream_write(0, m, (uint32_t)n);
    if (s_uart) uart_write_bytes(MIDI_UART, m, (size_t)n);
    s_tx++;
}

static void send_cc(uint8_t cc, uint8_t v) {
    const uint8_t m[3] = {(uint8_t)(0xB0 | (s_channel - 1)), cc, (uint8_t)(v & 0x7F)};
    send(m, 3);
}

static void send_param(const midi_param_t *p, int v) {
    if (p->n_opts > 1) {
        send_cc(p->cc, p->opt_value[v]);
    } else if (p->kind == MIDI_P_KORG10) {
        send_cc(63, (uint8_t)(v & 7)); // the low 3 bits first, then the upper 7 (minilogue xd *5-4)
        send_cc(p->cc, (uint8_t)(v >> 3));
    } else {
        send_cc(p->cc, (uint8_t)v);
    }
}

static void send_program(int n) {
    const midi_synth_t *s = synth();
    if (s->prog_scheme == MIDI_PROG_KORG_BANK100) {
        send_cc(0, 0);
        send_cc(32, (uint8_t)(n / 100));
        n %= 100;
    }
    const uint8_t m[2] = {(uint8_t)(0xC0 | (s_channel - 1)), (uint8_t)n};
    send(m, 2);
}

// --- what comes back: a synth that sends its knobs (or a DAW's echo) keeps the values true ---
static void received_cc(uint8_t cc, uint8_t v) {
    if (cc == 63) {
        s_lsb = v & 7;
        return;
    }
    const midi_synth_t *s = synth();
    for (int i = 0; i < s->n_params; i++) {
        const midi_param_t *p = &s->params[i];
        if (p->cc != cc) continue;
        if (p->n_opts > 1) { // the option whose value is nearest
            int best = 0;
            for (int o = 1; o < p->n_opts; o++) {
                if (abs((int)p->opt_value[o] - v) < abs((int)p->opt_value[best] - v)) best = o;
            }
            s_val[i] = (int16_t)best;
        } else if (p->kind == MIDI_P_KORG10) {
            s_val[i] = (int16_t)(v << 3 | (s_lsb >= 0 ? s_lsb : 0));
        } else {
            s_val[i] = v;
        }
    }
    s_lsb = -1;
}

static void received(const uint8_t *m, int n) {
    if (n < 2 || (m[0] & 0x0F) != s_channel - 1) return;
    s_rx++;
    switch (m[0] & 0xF0) {
        case 0xB0:
            if (n >= 3) received_cc(m[1], m[2]);
            break;
        case 0xC0: // the synth changed its program: its values are new
            if (synth()->prog_scheme == MIDI_PROG_PC) s_prog = m[1];
            forget_values();
            break;
    }
}

// The TRS input's bytes, with running status; system messages are skipped.
static void parse_byte(uint8_t b) {
    static uint8_t msg[3], status = 0;
    static int have = 0, want = 0;
    static bool sysex = false;
    if (b >= 0xF8) return; // real time, anywhere
    if (b & 0x80) {
        sysex = b == 0xF0;
        if (b >= 0xF0) { status = 0; return; }
        status = b;
        have = 0;
        want = (b & 0xE0) == 0xC0 ? 1 : 2; // Cn / Dn: one data byte
        return;
    }
    if (sysex || status == 0) return;
    msg[0] = status;
    msg[1 + have++] = b;
    if (have == want) {
        received(msg, 1 + want);
        have = 0;
    }
}

static void receive_all(void) {
    uint8_t p[4];
    while (tud_midi_mounted() && tud_midi_packet_read(p)) {
        uint8_t cin = p[0] & 0x0F;
        if (cin == 0x0B) received(p + 1, 3);
        else if (cin == 0x0C) received(p + 1, 2);
    }
    if (s_uart) {
        uint8_t buf[64];
        int n;
        while ((n = uart_read_bytes(MIDI_UART, buf, sizeof(buf), 0)) > 0) {
            for (int i = 0; i < n; i++) parse_byte(buf[i]);
        }
    }
}

// --- inputs ---
static void apply_turn(int dir, uint32_t t) {
    const midi_synth_t *s = synth();
    if (s_f1_down) { // picking a parameter
        s_param = clampi(s_param + dir, 0, s->n_params - 1);
        s_browsed = true;
        return;
    }
    const midi_param_t *p = &s->params[s_param];
    const int max = midi_param_max(p);
    int v = s_val[s_param];
    if (p->n_opts > 1) {
        v = clampi((v < 0 ? 0 : v) + dir, 0, max);
    } else {
        const uint32_t dt = (t - s_last_turn) & 0xFFFFFF;
        const int unit = p->kind == MIDI_P_KORG10 ? 8 : 1; // one 7-bit step
        const int step = dt > TURN_FINE_MS ? (p->kind == MIDI_P_KORG10 ? 2 : 1)
                       : dt > TURN_NORMAL_MS ? unit : dt > TURN_FAST_MS ? unit * 3 : unit * 6;
        // Not known yet (a new synth or program): start from the middle.
        v = clampi((v < 0 ? (max + 1) / 2 : v) + dir * step, 0, max);
    }
    s_last_turn = t;
    if (v == s_val[s_param]) return;
    s_val[s_param] = (int16_t)v;
    send_param(p, v);
}

static void apply_key(int e, uint32_t t) {
    const midi_synth_t *s = synth();
    switch (e) {
        case EVT_F1_DOWN:
            s_f1_down = true;
            s_browsed = false;
            s_f1_at = t;
            break;
        case EVT_F1_UP:
            if (s_f1_down && !s_browsed) s_param = (s_param + 1) % s->n_params; // a tap: the next one
            s_f1_down = false;
            break;
        case EVT_F2:
        case EVT_F3: {
            int n = s_prog < 0 ? 0 : s_prog + (e == EVT_F3 ? 1 : -1);
            n = clampi(n, 0, s->prog_count - 1);
            if (n == s_prog) break;
            s_prog = n;
            s_prog_ms = now_ms();
            forget_values();
            send_program(n);
            break;
        }
    }
}

static void publish(bool active) {
    const midi_synth_t *s = synth();
    const midi_param_t *p = &s->params[s_param];
    const uint32_t t = (uint32_t)(esp_timer_get_time() >> 10);
    midi_snapshot_t b = {
        .active = active,
        .synth = s_synth,
        .param = s_param,
        .value = s_val[s_param],
        .browsing = s_f1_down && (s_browsed || ((t - s_f1_at) & 0xFFFFFF) >= HOLD_MS),
        .prog = s_prog,
        .prog_ms = s_prog_ms,
        .channel = s_channel,
        .usb = tud_midi_mounted(),
        .trs = s_uart,
        .tx = s_tx,
        .rx = s_rx,
    };
    // The control loop's walls and feel.
    uint8_t ends = 0;
    int haptic = HAPTIC_PROFILE_FINE;
    if (s_f1_down) {
        ends = (s_param <= 0 ? 1 : 0) | (s_param >= s->n_params - 1 ? 2 : 0);
        haptic = HAPTIC_PROFILE_COARSE;
    } else {
        if (b.value >= 0) ends = (b.value <= 0 ? 1 : 0) | (b.value >= midi_param_max(p) ? 2 : 0);
        if (p->n_opts > 1) haptic = HAPTIC_PROFILE_WIDE; // a switch: one option per click
    }
    atomic_store(&s_end_flags, active ? ends : 0);
    atomic_store(&s_haptic, haptic);
    b.version = s_snap.version;
    if (memcmp(&b, &s_snap, sizeof(b)) == 0) return;
    b.version = s_snap.version + 1;
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    s_snap = b;
    xSemaphoreGive(s_snap_mux);
    atomic_store(&s_version, b.version);
}

static void midi_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(1); // 10 ms
        const bool active = menu_get_hid_type() == MENU_HID_MIDI && !menu_is_open();
        const int synth_now = menu_get_midi_synth();
        if (synth_now != s_synth) { // another profile (the menu, or the companion)
            s_synth = synth_now;
            s_param = 0;
            s_prog = -1;
            s_f1_down = false;
            forget_values();
        }
        const midi_synth_t *sp = midi_synth_get(s_synth);
        if (sp != s_synth_ptr) { // edited (or reverted) in the companion: its parameters may differ
            s_synth_ptr = sp;
            s_param = clampi(s_param, 0, sp->n_params - 1);
            forget_values();
        }
        int g = atomic_exchange(&s_goto, -1);
        if (g >= 0) s_param = clampi(g, 0, sp->n_params - 1);
        s_channel = (uint8_t)clampi(menu_get_midi_channel(), 1, 16);
        if (active && !s_uart) uart_start(); // once MIDI is first used; the pins stay the jacks'
        receive_all();
        uint32_t e;
        while (take_evt(&e)) {
            if (!active) continue; // the menu took the keys
            const int8_t ev = (int8_t)(e & 0xFF);
            const uint32_t t = e >> 8;
            if (ev == 1 || ev == -1) apply_turn(ev, t);
            else apply_key(ev, t);
        }
        if (!active) s_f1_down = false;
        publish(active);
    }
}

// --- the screen ---
void midi_get_snapshot(midi_snapshot_t *out) {
    if (s_snap_mux == NULL) { // before midi_start()
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_snap_mux, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_snap_mux);
}

uint32_t midi_version(void) { return atomic_load(&s_version); }

void midi_goto(int param) { atomic_store(&s_goto, param); }

void midi_start(void) {
    s_snap_mux = xSemaphoreCreateMutex();
    forget_values();
    // PSRAM stack: it never writes flash (the profile choice is the menu's save), and the UART
    // driver copies what it sends into its FIFO.
    xTaskCreatePinnedToCoreWithCaps(midi_task, "midi", 3584, NULL, PRIO_MIDI, NULL, CORE_IO, MALLOC_CAP_SPIRAM);
}
