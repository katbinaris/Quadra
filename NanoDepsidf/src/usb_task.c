#include "usb_task.h"
#include <string.h>
#include "tasks_common.h"
#include "ipc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "class/hid/hid_device.h"
#include "icon_store.h"
#include "app_mode.h"
#include "sysmon.h"
#include "menu.h"
#include "host_link.h"
#include "ext_link.h"
#include "ext_proto.h"
#include "host_proto.h"

static const char *TAG = "usb";

// --- Phase 3: USB HID (keyboard / mouse / gamepad) + CDC-ACM ---
// First working slice: a composite TinyUSB device -- one CDC-ACM serial port (2
// interfaces) plus one HID interface carrying three report IDs (keyboard, mouse,
// gamepad), following the exact pattern of the ESP-IDF reference examples this is adapted
// from (examples/peripherals/usb/device/tusb_hid for the HID half, the upstream TinyUSB
// cdc_msc example for the CDC-composite pattern -- not a legacy_fw port, neither existed
// there). Enumeration and report delivery both confirmed on hardware (HID recognized by
// host, a keyboard self-test 'a' tap was visibly typed) -- the self-test itself has since
// been removed now that it's served its purpose.
//
// First real (non-self-test) mapping: knob rotation -> mouse scroll wheel, one detent
// crossing = one step (`HID_EVENT_MOUSE_WHEEL`, produced in `control_task.c` at the same
// detent edge that drives the haptic click, consumed here). This is the first concrete
// answer to Phase 3's "how much of the mapping engine survives" open decision -- not the
// whole answer; keys and the rest of the mapping surface are still open.
//
// CDC-ACM is not optional here, even though Phase 3's own scope is just HID: this board's
// upload flow (`use_1200bps_touch` in boards/nanofoc_d.json) depends on the *running*
// firmware exposing a USB-CDC serial port so PlatformIO/esptool can open it and toggle
// 1200 baud to trigger a reset into the bootloader. A HID-only descriptor has no such
// port, which would silently break `pio run -t upload` -- falling back to the physical
// BOOT switch every time, which is reserved for brick recovery, not routine reflashing.
// This also fixes the secondary-console conflict flagged in Phase 7's plan entry (the
// built-in USB-Serial-JTAG console can't coexist with TinyUSB on the same physical OTG
// PHY) by giving the running firmware its own CDC console instead, via
// `tinyusb_console_init()` -- redirects stdio (all ESP_LOGx output included) onto this
// port once it's up. Endpoint budget confirmed directly from TinyUSB's dwc2_esp32.h port
// source (ESP32-S3: 7 total endpoint numbers, 5 usable IN) rather than assumed: CDC needs
// 2 IN + 1 OUT, HID's 3 report IDs share a single IN endpoint -- 3 IN + 1 OUT total,
// comfortably inside budget, so nothing had to be dropped from the HID side to fit CDC in.
//
// Keys and richer mappings arrived with APP mode: app_mode.c (Core 0) publishes the wanted
// buttons / modifier / pointer travel / wheel steps / key taps, and app_sync() below brings
// the host in line with it every tick.
//
// Second HID interface: vendor-defined (usage page 0xFF00, "raw HID" style), with its own
// 64-byte interrupt IN+OUT endpoints, carrying host<->device data -- first user is icon
// upload (icon_store.h has the wire protocol). Deliberately a separate interface rather
// than a vendor report ID on the keyboard/mouse interface: full 64-byte payload with no
// report-ID byte, interrupt-OUT throughput instead of SET_REPORT control transfers, and host
// tools can open it by usage page without touching a keyboard interface (which macOS gates
// behind Input Monitoring permission). Endpoint budget: now 4 IN + 2 OUT, still inside the
// S3's 5 usable IN.

enum {
    ITF_NUM_CDC = 0,      // CDC control interface (CDC data is ITF_NUM_CDC + 1, implicit
                          // in TUD_CDC_DESCRIPTOR's own Interface Association Descriptor)
    ITF_NUM_CDC_DATA = 1,
    ITF_NUM_HID = 2,
    ITF_NUM_HID_VENDOR = 3,
    ITF_NUM_TOTAL = 4,
};

// TinyUSB numbers HID instances in configuration-descriptor order.
enum {
    HID_INSTANCE_INPUT = 0,  // keyboard / mouse / gamepad
    HID_INSTANCE_VENDOR = 1, // raw 64-byte host<->device data
};

enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_MOUSE = 2,
    REPORT_ID_GAMEPAD = 3,
    REPORT_ID_CONSUMER = 4, // media keys (volume, play/pause) -- APP_ACT_MEDIA
};

#define EPNUM_CDC_NOTIF 0x81 // EP1 IN
#define EPNUM_CDC_OUT   0x02 // EP2 OUT
#define EPNUM_CDC_IN    0x82 // EP2 IN
#define EPNUM_HID_IN    0x83 // EP3 IN
#define EPNUM_VENDOR_OUT 0x04 // EP4 OUT
#define EPNUM_VENDOR_IN  0x84 // EP4 IN

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
    TUD_HID_REPORT_DESC_GAMEPAD(HID_REPORT_ID(REPORT_ID_GAMEPAD)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER)),
};

// Usage page 0xFF00 / usage 0x01, one 64-byte input + one 64-byte output report, no report
// ID -- what host tools match on (tools/send_icon.py).
static const uint8_t s_vendor_report_descriptor[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(ICON_HID_REPORT_SIZE),
};

static const char *s_usb_string_descriptor[7] = {
    (char[]){0x09, 0x04}, // 0: supported language -- English (0x0409)
    "Kafi Devices",       // 1: Manufacturer
    "Quadra",             // 2: Product (tools/send_icon.py matches on this)
    "QUADRA-DEV",         // 3: Serial -- placeholder; a real per-device ID (e.g. from
                           //    efuse MAC) is follow-on work, not needed for this slice
    "Quadra Console",      // 4: CDC interface name
    "Quadra HID",          // 5: HID interface name
    "Quadra Data",         // 6: vendor HID interface name
};

// device descriptor deliberately left NULL in tinyusb_config_t below: esp_tinyusb's own
// default device descriptor already sets bDeviceClass/SubClass/Protocol to
// MISC/COMMON/IAD whenever CFG_TUD_CDC > 0 (confirmed directly in its
// usb_descriptors.c) -- exactly what a CDC-composite device needs, so there's no need to
// hand-roll a custom device descriptor just to get those three fields right.
static const uint8_t s_usb_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500), // mA: the USB 2.0 maximum (LEDs)
    // Interface number, string index, EP notification address & size, EP data (out, in) & size
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 16, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    // Interface number, string index, boot protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 5, false, sizeof(s_hid_report_descriptor), EPNUM_HID_IN, 16, 10),
    // Interface number, string index, boot protocol, report descriptor len, EP Out & In address, size & polling interval
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID_VENDOR, 6, HID_ITF_PROTOCOL_NONE, sizeof(s_vendor_report_descriptor),
                             EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, ICON_HID_REPORT_SIZE, 1),
};

// --- Required TinyUSB HID callbacks (no weak default -- must be defined) ---

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    return (instance == HID_INSTANCE_VENDOR) ? s_vendor_report_descriptor : s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

// Runs in the TinyUSB task. For the vendor interface, TinyUSB re-arms the OUT endpoint right
// after this returns, so handling must stay synchronous and short -- icon_store only copies
// the chunk into its staging buffer (and CRCs ~4.6KB once, at END).
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
    (void)report_id; (void)report_type;
    if (instance != HID_INSTANCE_VENDOR) {
        return; // keyboard LED output reports etc. -- unused
    }
    // The companion app's commands and the icon upload share this interface; host_link sorts
    // them and queues any reply for the usb task, which also sends the live stream.
    host_link_handle_report(buffer, bufsize);
}

// Runs in the TinyUSB task after an IN report reached the host. A profile download sends its
// next piece from here, so the pieces go out once per host poll (1ms) rather than once per
// usb-task pass.
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)report; (void)len;
    if (instance == HID_INSTANCE_VENDOR) host_link_report_sent();
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    ESP_LOGI(TAG, "USB suspended");
}

void tud_resume_cb(void) {
    ESP_LOGI(TAG, "USB resumed");
}

// --- report sending ---
// The host polls the HID endpoint every 10ms (bInterval above), so a report can only go out
// once the previous one has been collected. Waits are whole ticks: at CONFIG_FREERTOS_HZ=100
// pdMS_TO_TICKS(1) is 0, which made the first version of this wait a no-op and silently
// dropped back-to-back reports (stuck Option / middle button in the APP-mode test).
#define HID_SEND_TRIES 5 // x 1 tick (10ms) -- well past one host poll

// Where the reports go: the USB host when one has the knob, else the WiFi client that asked for
// the controls (EXT_NET_CONTROLS -- the companion app types and scrolls for the knob, ext_proto.h
// EXT_TAG_HID), else nowhere. Picked every pass of the usb task; everything upstream (app_sync,
// BINDINGS, the macros' pauses) is the same for both.
typedef enum { OUT_NONE, OUT_USB, OUT_NET } hid_out_t;
static hid_out_t s_out = OUT_NONE;
static host_link_t s_out_link;
static uint32_t s_out_gen;

static hid_out_t pick_out(void) {
    if (tud_mounted()) return OUT_USB;
    if (ext_controls_link(&s_out_link, &s_out_gen)) return OUT_NET;
    return OUT_NONE;
}

// One report to the WiFi client, retried like USB's while its queue is full.
static bool send_net(uint8_t kind, const uint8_t *body, size_t n) {
    uint8_t r[HOST_REPORT_SIZE] = {EXT_TAG_HID, kind};
    memcpy(r + 2, body, n);
    for (int i = 0; i < HID_SEND_TRIES; i++) {
        if (host_link_send_event(s_out_link, s_out_gen, r)) return true;
        vTaskDelay(1);
    }
    ESP_LOGW(TAG, "report to the WiFi client dropped");
    sysmon_note_hid_drop();
    return false;
}

static bool send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    if (s_out == OUT_NET) return send_net(EXT_HID_MOUSE, (const uint8_t[]){buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel}, 4);
    for (int i = 0; i < HID_SEND_TRIES; i++) {
        if (tud_hid_n_ready(HID_INSTANCE_INPUT) && tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, dx, dy, wheel, 0)) {
            return true;
        }
        vTaskDelay(1);
    }
    ESP_LOGW(TAG, "mouse report dropped (endpoint busy)");
    sysmon_note_hid_drop();
    return false;
}

// DEVICE -> BINDINGS. Profiles are written with macOS shortcuts; on a PC the same shortcut is
// Ctrl where the Mac has Cmd (Option is Alt on both, and a profile's own Ctrl stays Ctrl).
// Done here, on every keyboard report, so no profile needs a PC copy and a held modifier (Cmd
// + wheel zoom, a drag) is translated the same as a tap. Everything upstream (app_mode, the
// sent-state tracking below) stays in Mac terms.
static uint8_t host_modifier(uint8_t m) {
    if (menu_get_host() != MENU_HOST_PC) return m;
    uint8_t gui = m & (KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);
    m &= (uint8_t)~gui;
    if (gui & KEYBOARD_MODIFIER_LEFTGUI) m |= KEYBOARD_MODIFIER_LEFTCTRL;
    if (gui & KEYBOARD_MODIFIER_RIGHTGUI) m |= KEYBOARD_MODIFIER_RIGHTCTRL;
    return m;
}

static bool send_keys(uint8_t modifier, uint8_t keycode) {
    uint8_t keys[6] = {keycode, 0, 0, 0, 0, 0};
    modifier = host_modifier(modifier);
    if (s_out == OUT_NET) return send_net(EXT_HID_KEYBOARD, (const uint8_t[]){modifier, keycode, 0, 0, 0, 0, 0}, 7);
    for (int i = 0; i < HID_SEND_TRIES; i++) {
        if (tud_hid_n_ready(HID_INSTANCE_INPUT)
            && tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, keycode ? keys : NULL)) {
            return true;
        }
        vTaskDelay(1);
    }
    ESP_LOGW(TAG, "keyboard report dropped (endpoint busy)");
    sysmon_note_hid_drop();
    return false;
}

// Consumer page (media keys): the report is one 16-bit usage; 0 releases it.
static bool send_consumer(uint16_t usage) {
    if (s_out == OUT_NET) return send_net(EXT_HID_CONSUMER, (const uint8_t *)&usage, 2);
    for (int i = 0; i < HID_SEND_TRIES; i++) {
        if (tud_hid_n_ready(HID_INSTANCE_INPUT)
            && tud_hid_n_report(HID_INSTANCE_INPUT, REPORT_ID_CONSUMER, &usage, sizeof(usage))) {
            return true;
        }
        vTaskDelay(1);
    }
    ESP_LOGW(TAG, "consumer report dropped (endpoint busy)");
    sysmon_note_hid_drop();
    return false;
}

// APP mode (app_mode.h): bring the host in line with the wanted state. What the host has is
// tracked in *sent_buttons / *sent_modifier and only updated on a report that went out, so a
// failed send is simply retried next pass. Order matters for chords (Ctrl + middle-drag,
// Cmd + wheel): a modifier goes down before the button or wheel, and comes up after.
static void app_sync(uint8_t *sent_buttons, uint8_t *sent_modifier) {
    static bool keys_dirty = false;  // a key tap's report may still be held on the host
    static bool media_dirty = false; // a media key's release may not have reached the host
    // A tap's pause (a macro waiting on the host's UI) holds back the taps after it, not this
    // task: pointer, wheel and the companion link keep running meanwhile.
    static TickType_t taps_resume = 0;
    // The modifier this pass's media taps hold. It stays down across back-to-back media taps (a
    // fast volume turn) and comes up once the queue is empty: two reports per step, not four.
    uint8_t media_mod = 0;
    app_tap_t tap;
    while ((int32_t)(xTaskGetTickCount() - taps_resume) >= 0 && app_mode_take_tap(&tap)) {
        // One tap = press with the tap's own modifier, then back to what a slot holds. An empty
        // tap (a macro's pause) only waits.
        if (tap.consumer) {
            // On a Mac, Shift+Option + volume moves in quarter steps. On a PC a modifier on a
            // media key does nothing useful and can trip OS hotkeys (Alt+Shift switches layout).
            uint8_t mod = menu_get_host() == MENU_HOST_PC ? 0 : tap.modifier;
            if (mod != media_mod && send_keys(*sent_modifier | mod, 0)) {
                media_mod = mod;
                keys_dirty = true; // let go below, after the last media tap
            }
            if (send_consumer(tap.keycode)) media_dirty = true;
            if (send_consumer(0)) media_dirty = false;
        } else if (tap.keycode || tap.modifier) {
            if (send_keys(tap.modifier, tap.keycode)) keys_dirty = true;
            if (send_keys(*sent_modifier, 0)) keys_dirty = false;
            media_mod = 0; // that release let go of a media modifier too
        }
        if (tap.wait_ticks) taps_resume = xTaskGetTickCount() + tap.wait_ticks;
    }
    if (media_dirty && send_consumer(0)) media_dirty = false;
    if (media_mod != 0 && send_keys(*sent_modifier, 0)) keys_dirty = false;
    uint8_t want_buttons, want_modifier;
    bool axis_y;
    app_mode_wanted(&want_buttons, &want_modifier, &axis_y);

    // Modifier added -> before the buttons / wheel.
    if (want_modifier & ~*sent_modifier) {
        if (send_keys(want_modifier, 0)) *sent_modifier = want_modifier;
    }
    // Wheel steps only go out once the host has the slot's modifier (Cmd + wheel is zoom;
    // the wheel alone would scroll).
    int32_t wheel = app_mode_take_wheel_steps();
    if (wheel != 0) {
        if (*sent_modifier != want_modifier) {
            app_mode_return_wheel_steps(wheel);
        } else {
            int32_t w = wheel > 127 ? 127 : wheel < -127 ? -127 : wheel;
            if (w != wheel) app_mode_return_wheel_steps(wheel - w);
            if (!send_mouse(*sent_buttons, 0, 0, (int8_t)w)) app_mode_return_wheel_steps(w);
        }
    }
    int32_t move = app_mode_take_move_px();
    if (move != 0 && want_buttons == 0 && !app_mode_hover()) move = 0; // travel only counts during a drag (or parameter mode)
    if (move > 127 || move < -127) {
        int32_t clipped = move > 0 ? 127 : -127;
        app_mode_return_move_px(move - clipped);
        move = clipped;
    }
    if (want_buttons != *sent_buttons || move != 0) {
        int8_t dx = axis_y ? 0 : (int8_t)move, dy = axis_y ? (int8_t)move : 0;
        if (send_mouse(want_buttons, dx, dy, 0)) {
            *sent_buttons = want_buttons;
        } else if (move != 0) {
            app_mode_return_move_px(move);
        }
    }
    // Modifier removed (or a tap's release failed) -> once the buttons are in sync.
    if ((*sent_modifier != want_modifier || keys_dirty) && *sent_buttons == want_buttons) {
        if (send_keys(want_modifier, 0)) {
            *sent_modifier = want_modifier;
            keys_dirty = false;
        }
    }
}

static void usb_task_fn(void *arg) {
    ESP_LOGI(TAG, "usb task started on core %d, prio %d", xPortGetCoreID(), uxTaskPriorityGet(NULL));

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = NULL; // see comment above s_usb_configuration_descriptor
    // Above the display, or animations starve the stack (tasks_common.h PRIO_TINYUSB).
    tusb_cfg.task = TINYUSB_TASK_CUSTOM(4096, PRIO_TINYUSB, CORE_IO);
    tusb_cfg.descriptor.full_speed_config = s_usb_configuration_descriptor;
    tusb_cfg.descriptor.string = s_usb_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_usb_string_descriptor) / sizeof(s_usb_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_usb_configuration_descriptor; // S3 OTG is
                                                                             // FS-only, kept
                                                                             // for portability
#endif
    host_link_init(HID_INSTANCE_VENDOR); // before the host can send anything
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));
    // Redirects stdio (all ESP_LOGx output included) onto this CDC port -- restores a
    // usable serial console over the same USB-C cable now that the built-in
    // USB-Serial-JTAG console can no longer share the OTG PHY with TinyUSB (see header
    // comment). Primary UART0 console is untouched by this and keeps working independently
    // via its own separate pins.
    ESP_ERROR_CHECK(tinyusb_console_init(TINYUSB_CDC_ACM_0));

    ESP_LOGI(TAG, "USB composite device installed (CDC console + HID keyboard+mouse+gamepad + vendor HID)");

    // Blocks on the queue itself (not a fixed-interval poll) so a scroll event reaches
    // the host with minimal added latency -- the timeout just bounds how long this task
    // can sit idle, it isn't a polling period.
    //
    // Wakes at least once a tick (10ms, = the host's poll interval) to sync APP mode's
    // wanted state; wheel steps arrive through the queue as before and carry whatever mouse
    // buttons the host currently has held.
    hid_report_msg_t msg;
    uint8_t sent_buttons = 0, sent_modifier = 0;
    menu_host_t host = menu_get_host();
    while (1) {
        bool got = xQueueReceive(g_hid_report_queue, &msg, 1) == pdTRUE;
        if (!tud_mounted()) host_link_stop(); // USB's share (cheap); the companion over WiFi carries on
        hid_out_t out = pick_out();
        if (out != s_out) {
            // Leaving the WiFi client (a cable came in, or another client asked): let go of what
            // it holds first. A fresh USB enumeration, or a new client, starts with nothing held.
            if (s_out == OUT_NET && (sent_buttons || sent_modifier)) {
                send_keys(0, 0);
                send_mouse(0, 0, 0, 0);
            }
            ESP_LOGI(TAG, "controls: %s", out == OUT_USB ? "USB" : out == OUT_NET ? "WiFi" : "none");
            sent_buttons = 0;
            sent_modifier = 0;
            s_out = out;
        }
        host_link_poll();
        if (out == OUT_NONE) continue;
        // BINDINGS switched while a modifier may be down: the host holds it under the old
        // mapping (Cmd vs Ctrl), so let go of everything; app_sync presses what's wanted again.
        if (menu_get_host() != host) {
            host = menu_get_host();
            if (sent_modifier != 0 && send_keys(0, 0)) sent_modifier = 0;
        }
        if (got && msg.type == HID_EVENT_MOUSE_WHEEL) {
            send_mouse(sent_buttons, 0, 0, msg.wheel_delta);
        }
        app_sync(&sent_buttons, &sent_modifier);
    }
}

void usb_task_start(void) {
    xTaskCreatePinnedToCore(usb_task_fn, "usb", 4096, NULL, PRIO_USB, NULL, CORE_IO);
}
