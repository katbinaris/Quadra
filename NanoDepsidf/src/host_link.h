#pragma once

#include <stdbool.h>
#include <stdint.h>

// The desktop app's link (host_proto.h has the wire format; the icon upload shares it,
// icon_store.h): over the vendor HID interface (usb_task.c) and, once paired, over WiFi
// (net_link.c), up to two clients at once (the companion app and the Mac service). Each command's
// reply goes back on the link it came on; the live stream and the screen go to the link that
// last asked for them.

// HOST_LINK_NET + slot: WiFi client `slot` (net_link.h, NET_LINK_SESSIONS of them).
typedef enum { HOST_LINK_USB = 0, HOST_LINK_NET = 1, HOST_LINK_NET2 = 2, HOST_LINK_COUNT } host_link_t;

void host_link_init(uint8_t vendor_instance);
// The USB personality changed (usb_task.c): MIDI has no keyboard interface before the vendor one.
void host_link_set_instance(uint8_t vendor_instance);

// One report from the host on `link` (TinyUSB task / net_link task). Builds the reply (if any)
// and queues it for that link.
void host_link_receive(host_link_t link, const uint8_t *report, uint16_t len);
void host_link_handle_report(const uint8_t *report, uint16_t len); // USB's

// usb task, every pass: profile work (JSON, files), then each link's queued replies, a profile
// download or the live stream.
void host_link_poll(void);

// A link's host, by generation (it moves on when that host goes away): a deferred reply carries
// the one it's for, and host_link_queue_to drops it if that host is gone. host_link_queue: USB's
// host now (events). In order with the link's replies. Core 1.
uint32_t host_link_gen(host_link_t link);
void host_link_queue_to(host_link_t link, uint32_t gen, const uint8_t *report);
void host_link_queue(const uint8_t *report);
// Every link's host now -- USB and each WiFi client -- the way every program that opens the
// vendor HID interface sees every report: events whose owner may have reconnected (an agent
// approval's answer).
void host_link_queue_all(const uint8_t *report);
// usb task: a report for `link`'s host `gen` (the controls over WiFi), on its way at once. false:
// that host is gone, or its queue is full -- try again in a tick.
bool host_link_send_event(host_link_t link, uint32_t gen, const uint8_t *report);

// TinyUSB task: a vendor IN report went out -- sends USB's next one.
void host_link_report_sent(void);

// EXT_CMD_SCREEN from `link`: fps 0 stops it (if that link has it).
void host_link_screen(host_link_t link, uint8_t fps);

// A link's host went away: its streams stop, its replies and a download for it are dropped.
void host_link_stop_link(host_link_t link);
void host_link_stop(void); // USB unmounted
