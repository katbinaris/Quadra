#pragma once

// Extensions to the companion protocol (host_proto.h) for this fork. Same framing: 64-byte
// reports on the vendor HID interface (or over WiFi, net_link.h), no report ID, little-endian. Kept in their own command
// range (0x20-0x2F, replies and events 0xC0-0xCF) so upstream can grow 0x10-0x1F freely.
// Host side: tools/quadra.py, tools/agents/.

#define EXT_PROTO_VERSION 8 // 4: EXT_CMD_NET; 5: EXT_CMD_TIME / _CLOCK; 6: _SCREEN / _INPUT;
                            // 7: the companion over WiFi (net_link.h), EXT_NET_KEY; 8: EXT_CMD_MUSIC

// --- Host -> device ---
enum {
    EXT_CMD_HELLO = 0x20,  // -> EXT_TAG_HELLO
    EXT_CMD_REBOOT = 0x21, // [1]=EXT_REBOOT_* (SERIAL: USB only) -> EXT_TAG_ACK, then the device
                           //   restarts
    EXT_CMD_TEXT = 0x22,   // [1]=0 (the idle text) [2..17]=text, NUL-padded, <= USER_TEXT_MAX
                           //   (user_prefs.h); "" = the stock wordmark. Stored. -> EXT_TAG_ACK
    EXT_CMD_LIGHTS = 0x23, // [1]=EXT_LIGHTS_* flags [2]=src [3]=fx [4..5]=hue [6]=sat [7]=speed
                           //   [8..9]=level (user_prefs.h lights_t); 0xFF / 0xFFFF = keep that
                           //   field. Live at once, like turning the knob. -> EXT_TAG_PREFS
    EXT_CMD_PREFS = 0x24,  // -> EXT_TAG_PREFS
    EXT_CMD_NOTIFY = 0x25, // [1]=EXT_NOTIFY_* [2..3]=id; POST: [4]=source [5]=kind (notify.h),
                           //   | EXT_NOTIFY_NUDGE [6..21]=title [22..60]=body, NUL-padded
                           //   [61..63]=colour RGB (0,0,0 = the source's own). No reply.
    EXT_CMD_COVER = 0x26,  // now-playing cover, a JPEG (media.h), from one link at a time, in order:
                           //   [1]=EXT_COVER_BEGIN [4..7]=length [8..11]=CRC-32 -> EXT_TAG_ACK
                           //   [1]=EXT_COVER_DATA [2..4]=offset (24-bit) [5]=n (<= 58) [6..]=bytes
                           //   [1]=EXT_COVER_END -> EXT_TAG_ACK (EXT_ST_BAD_PARAM: rejected)
    EXT_CMD_TRACK = 0x27,  // [1]=EXT_TRACK_* flags [2]=volume % (0xFF unknown) [3..11]=3 cover
                           //   colours RGB [12..35]=title [36..59]=artist, NUL-padded. No reply.
    EXT_CMD_AGENTS = 0x28, // the AGENTS dashboard (agent_board.h): [1]=rows (<= 4), then 14 bytes
                           //   each from [2]: source, agent_state_t, name (12). No reply.
    EXT_CMD_NET = 0x29,    // WiFi (net.h). [1]=EXT_NET_*; all but STATUS over USB only:
                           //   SSID: [2..33] the network's name, NUL-padded
                           //   PASS_A / PASS_B: [2..33] the password's first / second 32 bytes
                           //   APPLY: [2]=1 on / 0 off -- stores what was sent (the stored SSID /
                           //     password stay when none was) and (re)connects -> EXT_TAG_NET
                           //   STATUS -> EXT_TAG_NET. The password is never sent back.
                           //   KEY: [2]=1 a new one (every paired companion pairs again), 0 the
                           //     current one (made on first use) -> EXT_TAG_KEY
    EXT_CMD_TIME = 0x2A,   // the Mac service, on connecting and every few minutes: [1..6] UTC time
                           //   in ms (48-bit) [7..18] LOCAL's label [19..63] its POSIX TZ rule
                           //   (clock.h), NUL-padded. No reply.
    EXT_CMD_CLOCK = 0x2B,  // the CLOCK app. [1]=EXT_CLOCK_*:
                           //   FORMAT: [2]=CLOCK_* flags -> EXT_TAG_CLOCK (slot 0)
                           //   ZONE: [2]=slot 1-4 [3..14]=label ("" = none) [15..60]=POSIX TZ rule
                           //     -> EXT_TAG_CLOCK (that slot); EXT_TAG_ACK BAD_PARAM: not a rule
                           //     the knob can follow
                           //   GET: [2]=slot 0-4 -> EXT_TAG_CLOCK
    EXT_CMD_SCREEN = 0x2C, // [1]=frames a second (0 = stop, <= 30): the screen, live, as
                           //   EXT_TAG_SCREEN reports (screen_stream.h); a start sends it whole first
    EXT_CMD_INPUT = 0x2D,  // the companion's hands on the knob. [1]=EXT_INPUT_*:
                           //   KEYS: [2]=keys held (UI_BTN_*, F1 = 0x01 .. F4 = 0x08), for 600 ms
                           //     unless sent again (the companion repeats it while a key is down)
                           //   TURN: [2]=detents (int8, + = clockwise), as if the knob had turned
    EXT_CMD_MUSIC = 0x2E,  // [1]=the cover style (user_prefs.h cover_style_t), 0xFF = keep. Stored.
                           //   -> EXT_TAG_PREFS
};
enum { EXT_INPUT_KEYS = 1, EXT_INPUT_TURN = 2 };
enum { EXT_CLOCK_FORMAT = 1, EXT_CLOCK_ZONE = 2, EXT_CLOCK_GET = 3 };
enum { EXT_NET_SSID = 1, EXT_NET_PASS_A = 2, EXT_NET_PASS_B = 3, EXT_NET_APPLY = 4, EXT_NET_STATUS = 5, EXT_NET_KEY = 6 };
enum { EXT_COVER_BEGIN = 1, EXT_COVER_DATA = 2, EXT_COVER_END = 3 };
#define EXT_COVER_CHUNK 58
#define EXT_TRACK_PLAYING 0x01
#define EXT_TRACK_NONE 0x80 // nothing playing: back to the normal MUSIC screen
#define EXT_NOTIFY_NUDGE 0x80 // POST [5]: input is needed -- the knob taps gently every few seconds

enum {
    EXT_REBOOT_NORMAL = 0,
    // The next boot only comes up as the chip's USB-Serial-JTAG port (no HID), like holding
    // F3+F4 at power-on: esptool can then reset it into the ROM loader and flash it, and its
    // reset after flashing boots normally again. One boot only -- nothing is stored.
    EXT_REBOOT_SERIAL = 1,
};

#define EXT_LIGHTS_SAVE 0x01 // also store them (otherwise live only, shown as unsaved)

enum { EXT_NOTIFY_POST = 1, EXT_NOTIFY_CLEAR = 2, EXT_NOTIFY_CLEAR_ALL = 3 };

// --- Device -> host ---
enum {
    EXT_TAG_HELLO = 0xC0,  // [1]=EXT_PROTO_VERSION
    EXT_TAG_ACK = 0xC1,    // [1]=command [2]=EXT_ST_*
    EXT_TAG_PREFS = 0xC2,  // [1]=src [2]=fx [3..4]=hue [5]=sat [6]=speed [7..8]=level
                           // [9]=1: the lights differ from what's saved [10]=MUSIC's cover style
                           // [11]=how many cover styles there are (v8; 0 before) [16..31]=idle text
    EXT_TAG_NOTIFY = 0xC3, // unsolicited: [1]=notify_decision_t [2..3]=id
    EXT_TAG_NET = 0xC4,    // [1]=net_state_t [2]=RSSI dBm (int8) [3..6]=IPv4 [7]=1: the clock is set
                           // (SNTP) [8]=1: on [9..40]=SSID [41..63]=host name (<host>.local)
    EXT_TAG_CLOCK = 0xC5,  // [1]=CLOCK_* flags [2]=1: the time is set [3]=slot [4..5]=its UTC offset
                           // now, minutes (int16) [6..17]=label [18..63]=POSIX TZ rule
    EXT_TAG_SCREEN = 0xC6, // [1]=frame number [2]=1: a frame's first report, 2: its last
                           // [3]=bytes [4..63]=the stream (screen_stream.h)
    EXT_TAG_KEY = 0xC7,    // [1..32]=the WiFi pairing key (net_link.h) [33..34]=its TCP port
};

enum {
    EXT_ST_OK = 0,
    EXT_ST_BAD_PARAM = 1,
    EXT_ST_UNKNOWN = 2,
    EXT_ST_STORAGE = 3, // the NVS write failed
    EXT_ST_USB_ONLY = 4, // not over WiFi
};
