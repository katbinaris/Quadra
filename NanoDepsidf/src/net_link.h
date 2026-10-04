#pragma once

#include <stdbool.h>
#include <stdint.h>

// The companion protocol over WiFi: the same 64-byte reports as the vendor HID interface, on a
// TCP connection (port NET_LINK_PORT, advertised over mDNS as _quadra._tcp), up to
// NET_LINK_SESSIONS clients at once (the companion app and the Mac service), each its own host
// link (HOST_LINK_NET + slot). A newly authenticated client takes a free slot, else the one that
// has been quiet the longest.
//
// Pairing happens over USB only: the knob makes a random 256-bit key (EXT_CMD_NET KEY) and the
// companion keeps it. A connection then opens with
//   client: "QDR1" | client nonce (16)
//   knob:   "QDR1" | knob nonce (16) | knob proof (16)
//   client: client proof (16)
// with K = HMAC-SHA256(key, "quadra session" | client nonce | knob nonce), knob proof =
// HMAC-SHA256(K, "knob")[0..16), client proof = HMAC-SHA256(K, "client")[0..16). Every report
// after that is AES-256-GCM under K -- 64 bytes of ciphertext and a 16-byte tag, the nonce
// being 'K' (from the knob) or 'C' (from the client), then a per-direction counter (64-bit,
// little-endian), then three zero bytes. So nothing on the network can read a report, forge
// one, or replay it; a frame that doesn't verify ends the connection.

#define NET_LINK_PORT 3333
#define NET_KEY_BYTES 32
#define NET_LINK_SESSIONS 2 // host_link.h has a link for each

void net_link_start(void); // app_main, after net_start(): serves while WiFi is connected
// The pairing key, made on first use (usb task, EXT_CMD_NET KEY). `fresh`: replace it -- every
// paired companion has to pair again. false if NVS failed.
bool net_link_key(uint8_t key[NET_KEY_BYTES], bool fresh);
// host_link, for the client in `slot`: room for another stream report on the way to it, and
// handing it one; and a reply, which goes out ahead of the streams (a queue of its own), so a
// request isn't answered seconds late behind a screen frame. false: no room (or no client in
// that slot) -- try again later.
bool net_link_ready(int slot);
bool net_link_send(int slot, const uint8_t *report);
bool net_link_reply(int slot, const uint8_t *report);
bool net_link_connected(int slot); // a client is in that slot
