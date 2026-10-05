#pragma once

#include <stdint.h>

// Quadra companion protocol (the desktop app, companion/): settings, live state and SYS INFO
// over the vendor HID interface, next to the icon upload (icon_store.h: commands 0x01-0x04,
// replies tagged 0xA0). The app mirrors this file in companion/src/proto.ts -- change both.
//
// Every report is 64 bytes, no report ID. [0] is the command (host -> device) or the reply tag
// (device -> host). Multi-byte fields little-endian, floats IEEE-754 single. Packed layouts
// below are byte offsets; unused bytes are 0.
//
// Fixed little messages for settings and state. Whole profiles travel as JSON text
// (app_profiles/profile_json.h), in 60-byte pieces with a CRC-32 over the whole text (zlib's).

#define HOST_PROTO_VERSION 3
#define HOST_REPORT_SIZE 64

// --- Host -> device ---
enum {
    HOST_CMD_HELLO = 0x10,        // -> HOST_TAG_HELLO
    HOST_CMD_GET_SETTINGS = 0x11, // -> HOST_TAG_SETTINGS
    HOST_CMD_SET = 0x12,          // [1]=HOST_SET_* [4..7]=value (i32, or f32 for the float ones)
                                  //   applied live (clamped like the menu) -> HOST_TAG_SETTINGS
    HOST_CMD_SAVE = 0x13,         // everything that differs from NVS is saved -> HOST_TAG_SETTINGS
    HOST_CMD_REVERT = 0x14,       // back to what NVS holds -> HOST_TAG_SETTINGS
    HOST_CMD_STREAM = 0x15,       // [1]=state rate in Hz (0 = stop, max 50). While on: HOST_TAG_STATE
                                  //   at that rate, HOST_TAG_SYS_A / _B twice a second and
                                  //   HOST_TAG_LEDS ~15 times a second.
    HOST_CMD_PROFILE = 0x16,      // [1]=index -> HOST_TAG_PROFILE
    HOST_CMD_PROFILE_ICON = 0x17, // [1]=index [2..3]=offset -> HOST_TAG_PROFILE_ICON (icon48)
    HOST_CMD_RESET_PEAKS = 0x18,  // SYS INFO peaks and counters start over (F1 there); no reply
    // --- profiles (v2) ---
    HOST_CMD_PROFILE_READ = 0x19, // [1]=index -> HOST_TAG_PROFILE_BEGIN, then HOST_TAG_PROFILE_DATA
                                  //   pieces back to back until the text is complete
    HOST_CMD_UPLOAD_BEGIN = 0x1A, // [1]=HOST_UPLOAD_* flags [4..7]=length [8..11]=CRC-32; no reply
    HOST_CMD_UPLOAD_DATA = 0x1B,  // [1..3]=offset (24-bit) [4..63]=text, 60 bytes (fewer in the
                                  //   last piece, by the length); no reply, in order
    HOST_CMD_UPLOAD_END = 0x1C,   // checks and applies the profile -> HOST_TAG_RESULT
    HOST_CMD_HAPTIC_RESET = 0x1E, // the shown haptic profile back to factory -> HOST_TAG_SETTINGS
    HOST_CMD_PROFILE_OP = 0x1D,   // [1]=index [2]=HOST_OP_* -> HOST_TAG_RESULT
};

// HOST_CMD_UPLOAD_BEGIN [1]
#define HOST_UPLOAD_SAVE 0x01 // store it too (otherwise a live edit only)

// HOST_CMD_PROFILE_OP [2]
enum {
    HOST_OP_SAVE = 1,   // store the live edit
    HOST_OP_REVERT = 2, // drop the live edit (a never-stored new profile goes away)
    HOST_OP_REMOVE = 3, // delete the stored copy: a built-in goes back to its default
};

// Setting ids for HOST_CMD_SET (and the order of HOST_TAG_SETTINGS' dirty bits).
enum {
    HOST_SET_DETENTS = 0,   // i32 detents per turn: shows the nearest stepped haptic profile (older apps)
    HOST_SET_KP = 1,        // f32 (SNAP)
    HOST_SET_KD = 2,        // f32 (DAMP)
    HOST_SET_FEEL = 3,      // i32 haptic_type_t
    HOST_SET_AMP = 4,       // i32 percent
    HOST_SET_PITCH = 5,     // f32 multiplier
    HOST_SET_SOUND = 6,     // i32 audio_click_timbre_t
    HOST_SET_HID_TYPE = 7,  // i32 menu_hid_type_t
    HOST_SET_MIDI_CH = 8,   // i32 1..16
    HOST_SET_PROFILE = 9,   // i32 app profile index
    HOST_SET_BOOT = 10,     // i32 boot_usb_mode_t (SERIAL: the next boot has no HID, no app). USB only
    HOST_SET_ROTATION = 11, // i32 quarter turns 0..3
    HOST_SET_HOST = 12,     // i32 menu_host_t
    HOST_SET_SHAPE = 13,    // i32 percent, HAPTIC_SHAPE_MIN..MAX
    HOST_SET_HAPTIC_PROFILE = 14, // i32 haptic_profile_id_t: the profile the haptic values are of
    HOST_SET_MODE_HAPTIC = 15,    // i32 haptic_profile_id_t: the profile the current HID type uses
    HOST_SET_MIDI_SYNTH = 16,     // i32 MIDI synth profile index (midi.h)
    HOST_SET_COUNT
};

// --- Device -> host ---
enum {
    HOST_TAG_HELLO = 0xB0,
    // [1]=HOST_PROTO_VERSION [2]=profile count [3]=boot mode running (0 HID, 1 SERIAL)
    // [4..35]=firmware version, NUL-terminated [36..51]=build date, NUL-terminated
    HOST_TAG_SETTINGS = 0xB1,
    // [1..3]=dirty bits (1 << HOST_SET_*: live differs from NVS), 24 bits ([3] was 0 before
    // HOST_SET_MIDI_SYNTH)
    // [4..7]=detents i32 [8..11]=kp f32 [12..15]=kd f32 [16]=feel [17]=amp % [18..21]=pitch f32
    // [22]=sound [23]=hid type [24]=midi ch [25]=profile [26]=boot [27]=rotation [28]=host
    // [29]=shape % (0 from firmware before SHAPE existed)
    // Haptic profiles (protocol 3): KP, KD, SHAPE, FEEL, AMP and PITCH are the values of one
    // haptic profile in its current feel, and are clamped to that profile's limits.
    // [30]=haptic profile shown [31]=feels it allows (1 << haptic_type_t) [32]=amp max %
    // [33]=the HID type's haptic profile [36..39]=kp min [40..43]=kp max [44..47]=kd min
    // [48..51]=kd max [52..55]=pitch min [56..59]=pitch max (f32)
    // [34]=MIDI synth profile [35]=how many there are (0 from firmware before MIDI synths)
    HOST_TAG_PROFILE = 0xB2,
    // [1]=index [2]=count [3]=flags: bit0 has icon48, bit1 built-in, bit2 stored, bit3 live
    // edit (APP_PROFILE_* << 1) [4..15]=id [16..31]=name [32..63]=legend: 4 x 8 bytes
    // (all NUL-padded strings)
    HOST_TAG_PROFILE_ICON = 0xB3,
    // [1]=index [2..3]=offset [4]=len (<= 56) [8..63]=bytes of icon48 (48x48 RGB565 BE, 4608 B)
    HOST_TAG_LEDS = 0xB4,
    // [1]=first LED [2]=count (<= 20) [4..63]=count x RGB. LEDs 0-59: the ring, clockwise from
    // 12 o'clock in the screen's frame; 60-67: the keys, two each (F1 first). While streaming,
    // all 68 go out ~15 times a second, in four reports (led_task.h). As sent to the strips:
    // dim (power-limited to 20%).
    HOST_TAG_STATE = 0xB5,
    // [1..2]=sequence [4..7]=knob angle i32, 1e-4 rad, continuous [8..11]=detent i32
    // [12]=buttons held (bit0 F1..bit3 F4) [13]=menu screen (menu_screen_id_t, 0 = closed)
    // [14]=screensaver on [15]=APP live slot [16..19]=clicks i32 [20..23]=end-stop hits i32
    HOST_TAG_SYS_A = 0xB6,
    // power + heat: [4..5]=motor mA [6..7]=led mA [8..9]=board mA [10..11]=total mA
    // [12..13]=total peak mA [14]=chip ok [16..19]=chip C f32 [20..23]=chip peak C f32
    // [24..25]=coil mA [26..27]=coil peak mA [28..31]=copper W f32
    // [32]=USB source (pd_source_t) [34..35]=USB mA [36..37]=USB mV
    HOST_TAG_SYS_B = 0xB7,
    // cpu + system: [4]=load core 0 % [5]=load core 1 % [6]=peak 0 [7]=peak 1
    // [8..11]=loop kHz f32 [12..15]=work avg us f32 [16..19]=work max us f32
    // [20..23]=jitter max us f32 [24..27]=missed u32 [28..31]=spikes/s f32
    // [32..35]=heap free u32 [36..39]=heap min u32 [40..43]=hid drops u32
    // [44..47]=audio gaps u32 [48..51]=uptime s u32 [52..55]=sensor CRC errors u32
    HOST_TAG_PROFILE_BEGIN = 0xB8,
    // [1]=index [4..7]=length [8..11]=CRC-32 of the JSON text
    HOST_TAG_PROFILE_DATA = 0xB9,
    // [1..3]=offset (24-bit) [4..63]=text, 60 bytes (fewer in the last piece)
    HOST_TAG_RESULT = 0xBA,
    // [1]=command [2]=HOST_RES_* [3]=profile index (after the change) [4]=profile count
    // [5]=1: the profile at [3] was removed (the ones after it moved up)
    // [8..63]=what's wrong, NUL-terminated (HOST_RES_INVALID)
    HOST_TAG_ERROR = 0xBF,
    // [1]=command [2]=HOST_ERR_*
};

enum {
    HOST_ERR_UNKNOWN_CMD = 1,
    HOST_ERR_BAD_PARAM = 2,
};

enum {
    HOST_RES_OK = 0,
    HOST_RES_INVALID = 1,   // the JSON isn't a usable profile (reason in [8..])
    HOST_RES_TRANSFER = 2,  // pieces missing / out of order, or the CRC doesn't match
    HOST_RES_FULL = 3,      // no room for another profile
    HOST_RES_STORAGE = 4,   // writing / deleting the file failed
    HOST_RES_BAD_INDEX = 5,
    HOST_RES_BUSY = 6,      // another transfer is still going
};

#define HOST_ICON_CHUNK 56
#define HOST_TEXT_CHUNK 60
#define HOST_PROFILE_MAX_BYTES (96 * 1024)
