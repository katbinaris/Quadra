#pragma once

// Extensions to the companion protocol (host_proto.h) for this fork. Same framing: 64-byte
// reports on the vendor HID interface (or over WiFi, net_link.h), no report ID, little-endian. Kept in their own command
// range (0x20-0x2F, replies and events 0xC0-0xCF) so upstream can grow 0x10-0x1F freely. That
// range is full: new commands go in 0x30-0x3F (same handler, ext_link.c).
// Host side: tools/quadra.py, tools/agents/.

#define EXT_PROTO_VERSION 12 // 4: EXT_CMD_NET; 5: EXT_CMD_TIME / _CLOCK; 6: _SCREEN / _INPUT;
                             // 7: the companion over WiFi (net_link.h), EXT_NET_KEY; 8: EXT_CMD_MUSIC;
                             // 9: EXT_CMD_PD, two WiFi clients, the cover over WiFi;
                             // 10: EXT_NET_CONTROLS / EXT_TAG_HID (the controls over WiFi);
                             // 11: EXT_CMD_HOME / EXT_TAG_HOME (HOME's lamps);
                             // 12: EXT_HOME_EDIT, the lamp's icon / address in EXT_TAG_HOME,
                             //     EXT_CMD_SYNTH / EXT_TAG_SYNTH (MIDI's synth profiles)

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
                           //   CONTROLS (WiFi only): [2]=1 this client types and scrolls for the
                           //     knob while no USB host has it (EXT_TAG_HID), 0 not -> EXT_TAG_ACK.
                           //     One client at a time: the last to ask
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
    EXT_CMD_PD = 0x2F,     // the USB-PD chip's NVM (pd_status.h pd_nvm_5v), USB only. [1]=0: read and
                           //   check, 1: write 5 V 3 A for good -> EXT_TAG_PD
    EXT_CMD_HOME = 0x30,   // HOME's lamps (home.h). [1]=EXT_HOME_*; BEGIN, LAMP, COMMIT over USB only (tokens):
                           //   BEGIN: a new list starts -> EXT_TAG_ACK
                           //   LAMP: [2]=slot [3..6]=miIO device id [7..10]=IPv4 (a.b.c.d)
                           //     [11..26]=token [27]=HOME_PROTO_* [28]=HOME_CAP_* [29..30]=lowest
                           //     and [31..32]=highest colour temperature, K [33..36]=MIoT siid of
                           //     on, brightness, colour temperature, colour (0 = none) [37..40]=their
                           //     piid [41..60]=name, NUL-padded [61]=HOME_KIND_* (its icon)
                           //     -> EXT_TAG_ACK
                           //   COMMIT: [2]=count -- stores the list (NVS), the knob looks for it
                           //     -> EXT_TAG_ACK (EXT_ST_STORAGE: not stored)
                           //   STATUS: [2]=slot -> EXT_TAG_HOME
                           //   EDIT (v12): [2]=slot [3]=EXT_HOME_EDIT_* [4..7]=its device id (the
                           //     lamp the companion means; another there now: BAD_PARAM)
                           //     [8..]=NAME: the name, NUL-padded (19) / KIND: HOME_KIND_* /
                           //     MOVE: the slot it goes to / REMOVE: nothing.
                           //     Stored at once -> EXT_TAG_ACK (EXT_ST_STORAGE: not stored)
    EXT_CMD_SYNTH = 0x31,  // MIDI's synth profiles (midi.h), v12. [1]=EXT_SYNTH_*:
                           //   LIST: [2]=index -> EXT_TAG_SYNTH LIST
                           //   READ: [2]=index [4..7]=offset -> EXT_TAG_SYNTH READ: its JSON from
                           //     there (EXT_SYNTH_CHUNK bytes). Ask for offset 0 first and wait:
                           //     the knob writes the JSON then; the rest can be asked for together
                           //   PUT_BEGIN: [2]=EXT_SYNTH_SAVE or 0 [4..7]=length [8..11]=CRC-32;
                           //     PUT_DATA: [2..4]=offset (24-bit) [5]=n (<= 56) [8..]=bytes;
                           //     PUT_END -> EXT_TAG_SYNTH RESULT. Live at once (replaces the
                           //     one with its id, else added); SAVE stores it too
                           //   OP: [2]=index [3]=MIDI_SYNTH_OP_* (SAVE, REVERT, REMOVE)
                           //     -> EXT_TAG_SYNTH RESULT
                           //   STATUS -> EXT_TAG_SYNTH STATUS
                           //   GOTO: [2]=parameter: the knob moves to it (the synth in use)
};
enum { EXT_INPUT_KEYS = 1, EXT_INPUT_TURN = 2 };
enum { EXT_CLOCK_FORMAT = 1, EXT_CLOCK_ZONE = 2, EXT_CLOCK_GET = 3 };
enum { EXT_NET_SSID = 1, EXT_NET_PASS_A = 2, EXT_NET_PASS_B = 3, EXT_NET_APPLY = 4, EXT_NET_STATUS = 5, EXT_NET_KEY = 6,
       EXT_NET_CONTROLS = 7 };
enum { EXT_COVER_BEGIN = 1, EXT_COVER_DATA = 2, EXT_COVER_END = 3 };
enum { EXT_HOME_BEGIN = 1, EXT_HOME_LAMP = 2, EXT_HOME_COMMIT = 3, EXT_HOME_STATUS = 4, EXT_HOME_EDIT = 5 };
enum { EXT_HOME_EDIT_NAME = 1, EXT_HOME_EDIT_KIND = 2, EXT_HOME_EDIT_MOVE = 3, EXT_HOME_EDIT_REMOVE = 4 };
enum { EXT_SYNTH_LIST = 1, EXT_SYNTH_READ = 2, EXT_SYNTH_RESULT = 3, EXT_SYNTH_STATUS = 4, EXT_SYNTH_PUT_BEGIN = 5,
       EXT_SYNTH_PUT_DATA = 6, EXT_SYNTH_PUT_END = 7, EXT_SYNTH_OP = 8, EXT_SYNTH_GOTO = 10 };
#define EXT_SYNTH_SAVE 0x01
#define EXT_SYNTH_CHUNK 48  // READ's bytes per report
#define EXT_SYNTH_PUT_CHUNK 56
#define EXT_HOME_ONLINE 0x01 // EXT_TAG_HOME [7]: answered this session
#define EXT_HOME_KNOWN 0x02  // its state has been read
#define EXT_HOME_ON 0x04
#define EXT_HOME_FAILED 0x08 // the last change got no reply
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
    EXT_TAG_PD = 0xC8,     // [1]=pd_nvm_result_t [2]=sink PDOs in the NVM before [3]=after
                           //   [4..43]=the NVM as read before (5 sectors x 8 bytes)
    EXT_TAG_HOME = 0xCA,   // [1]=lamps stored [2]=slot [3..6]=device id [7]=EXT_HOME_* flags
                           // [8]=brightness % [9..10]=colour temperature K [11..13]=the colour it
                           // shows, RGB [14]=HOME_CAP_* [15..34]=name [35]=HOME_KIND_* (v12)
                           // [36..39]=its IPv4, a.b.c.d (v12) [40]=HOME_PROTO_* it answers (v12).
                           // A slot past the list: [3..]=0
    EXT_TAG_SYNTH = 0xCB,  // [1]=EXT_SYNTH_*:
                           //   LIST: [2]=index [3]=how many [4]=MIDI_SYNTH_* flags [5]=parameters
                           //     [6]=factory channel [7]=midi_prog_scheme_t [8..9]=programs
                           //     [10..33]=id [34..45]=maker [46..61]=name. Past the end: [4..]=0
                           //   READ: [2]=index [3]=bytes here [4..7]=the JSON's length [8..11]=its
                           //     CRC-32 [12..15]=offset [16..63]=bytes. [3]=0, length 0: no such
                           //   RESULT: [2]=EXT_SYNTH_PUT_END or _OP [3]=MIDI_SYNTH_ERR_* (0 OK)
                           //     [4]=index [5]=1: it was removed [8..63]=why, NUL-padded
                           //   STATUS: [2]=1: MIDI mode, menu closed [3]=synth [4]=parameter
                           //     [5..6]=its value (int16, -1 unknown) [7]=1: F1 held, picking
                           //     [8..9]=program sent last (int16, -1 none) [10]=channel [11]=1: USB
                           //     MIDI up [12]=1: TRS up [13..16]=messages sent [17..20]=received
    EXT_TAG_HID = 0xC9,    // to the EXT_NET_CONTROLS client: the HID report USB would have carried,
                           //   the whole state each time (like HID). [1]=EXT_HID_*:
                           //   KEYBOARD: [2]=modifiers (HID bits) [3..8]=keys held (HID usages)
                           //   MOUSE: [2]=buttons (1 left, 2 right, 4 middle) [3]=dx [4]=dy
                           //     [5]=wheel (int8; dx, dy, wheel are this report's motion, + = up)
                           //   CONSUMER: [2..3]=usage held (media keys; 0 = released)
};
enum { EXT_HID_KEYBOARD = 1, EXT_HID_MOUSE = 2, EXT_HID_CONSUMER = 3 };

enum {
    EXT_ST_OK = 0,
    EXT_ST_BAD_PARAM = 1,
    EXT_ST_UNKNOWN = 2,
    EXT_ST_STORAGE = 3, // the NVS write failed
    EXT_ST_USB_ONLY = 4, // not over WiFi
};
