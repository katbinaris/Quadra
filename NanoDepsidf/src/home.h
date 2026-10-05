#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

// HOME: the knob as a remote for Xiaomi lamps on the local network (miIO, UDP 54321). The lamps
// and their tokens come from the computer (`quadra.py home import`, EXT_CMD_HOME, USB only) and
// live in NVS; the knob finds which are on line, lists them, and changes them live.
//
// Three sides:
// - Core 0 (the control loop): turns and key presses go into a small lock-free ring
//   (home_input_*), never blocking; home_at_end() and home_haptic_profile() are single atomic
//   loads.
// - The `home` task (Core 1): owns the lamps, the socket and the crypto; applies the inputs,
//   sends the changes (at most every HOME_SEND_MS per lamp), reads the replies.
// - The display (Core 1): copies a snapshot (home_get_snapshot) and draws it.

#define HOME_MAX_LAMPS 12
#define HOME_NAME_LEN 20 // with the NUL

// What a lamp can change, from its MIoT spec (quadra.py). Power is always there (F2).
#define HOME_CAP_BRIGHT 0x01
#define HOME_CAP_TEMP 0x02
#define HOME_CAP_COLOR 0x04

// How a lamp is spoken to. MIOT: get_properties / set_properties by siid / piid. LEGACY: the
// older Yeelight-style get_prop / set_power / set_bright / set_ct_abx / set_rgb (e.g. the
// yeelink.light.lamp4 desk lamp, which ignores MIoT). The knob switches to the other one by
// itself when a lamp answers hello but not its queries.
enum { HOME_PROTO_MIOT = 0, HOME_PROTO_LEGACY = 1 };

// What the lamp looks like, for its icon (from the model name at import). Unknown: a bulb.
enum { HOME_KIND_BULB = 0, HOME_KIND_DESK = 1, HOME_KIND_DESK_ARM = 2, HOME_KIND_STRIP = 3, HOME_KIND_COUNT };

// MIoT property slots, in home_lamp_cfg_t's siid[] / piid[].
enum { HOME_PROP_ON = 0, HOME_PROP_BRIGHT, HOME_PROP_TEMP, HOME_PROP_COLOR, HOME_PROP_COUNT };

// One lamp as imported. Also the NVS blob's element: never reorder.
typedef struct {
    uint32_t did;       // miIO device id
    uint32_t ip;        // IPv4, network order (the last known address; a broadcast finds moves)
    uint8_t token[16];
    char name[HOME_NAME_LEN];
    uint8_t proto;      // HOME_PROTO_*
    uint8_t caps;       // HOME_CAP_*
    uint16_t ct_min, ct_max; // kelvin
    uint8_t siid[HOME_PROP_COUNT], piid[HOME_PROP_COUNT]; // MIoT; 0 = not there
    uint8_t kind;       // HOME_KIND_*: in what was padding, so the blob kept its size (0 = bulb)
} home_lamp_cfg_t;

// The edit screen's options, in F1 order. Only the ones a lamp has are offered.
typedef enum { HOME_OPT_BRIGHT = 0, HOME_OPT_TEMP, HOME_OPT_COLOR, HOME_OPT_COUNT } home_opt_t;

typedef enum {
    HOME_PHASE_OFF = 0,  // HOME isn't on screen (another mode, or the menu is open)
    HOME_PHASE_NO_WIFI,  // WiFi not connected
    HOME_PHASE_EMPTY,    // nothing imported
    HOME_PHASE_SCAN,     // looking for the lamps (a few seconds)
    HOME_PHASE_LIST,     // the knob picks a lamp
    HOME_PHASE_EDIT,     // the knob changes one of its options
} home_phase_t;

// What the screen shows of a lamp.
typedef struct {
    char name[HOME_NAME_LEN];
    uint32_t did;
    uint8_t caps;
    uint8_t kind;  // HOME_KIND_*
    bool online;   // answered (hello, then its state)
    bool known;    // its state has been read at least once
    bool on;
    bool failed;   // the last change got no reply
    uint8_t bright;          // 1-100
    uint16_t ct, ct_min, ct_max;
    uint16_t hue;            // 0-359, COLOR's value (full saturation)
    uint32_t rgb;            // the colour it shows, RGB888: its colour, or its white as RGB
    uint32_t ip;             // where it answers (network order)
    uint8_t proto;           // HOME_PROTO_* it answers
} home_lamp_view_t;

typedef struct {
    uint32_t version; // bumps on every change the screen should show
    home_phase_t phase;
    int count;
    int selected;
    home_opt_t option;     // HOME_PHASE_EDIT: the option being turned
    int found;             // HOME_PHASE_SCAN: lamps that answered so far
    int last;              // the lamp changed last (the idle screen shows it), -1: none yet
    home_lamp_view_t lamps[HOME_MAX_LAMPS];
} home_snapshot_t;

// The white a colour temperature gives, as RGB888 (an approximation of the black-body curve), and
// the full-saturation colour at a hue. Shared by home.c and the screens (inline: the host
// preview draws the screens without home.c).
static inline uint32_t home_kelvin_rgb(int k) {
    float t = k / 100.0f, r, g, b;
    if (t <= 66) {
        r = 255;
        g = 99.47f * logf(t) - 161.12f;
        b = t <= 19 ? 0 : 138.52f * logf(t - 10) - 305.04f;
    } else {
        r = 329.70f * powf(t - 60, -0.1332f);
        g = 288.12f * powf(t - 60, -0.0755f);
        b = 255;
    }
    int R = r < 0 ? 0 : r > 255 ? 255 : (int)r, G = g < 0 ? 0 : g > 255 ? 255 : (int)g, B = b < 0 ? 0 : b > 255 ? 255 : (int)b;
    return (uint32_t)R << 16 | (uint32_t)G << 8 | (uint32_t)B;
}
static inline uint32_t home_hue_rgb(int hue) {
    hue = ((hue % 360) + 360) % 360;
    int x = (int)(255 * (1 - fabsf(fmodf(hue / 60.0f, 2) - 1)));
    int r = 0, g = 0, b = 0;
    switch (hue / 60) {
        case 0: r = 255, g = x; break;
        case 1: r = x, g = 255; break;
        case 2: g = 255, b = x; break;
        case 3: g = x, b = 255; break;
        case 4: r = x, b = 255; break;
        default: r = 255, b = x; break;
    }
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

// app_main: loads the lamps from NVS and starts the home task (after net_start()).
void home_start(void);

// --- Core 0 (CONTROL_HOT; never block) ---
void home_input_rotate(int8_t dir); // one detent
void home_input_key(uint8_t key);   // UI_BTN_F1 / F2 / F3 pressed
bool home_at_end(int8_t dir);       // the list or the value is at its end that way: a wall
int home_haptic_profile(void);      // HAPTIC_PROFILE_*: COARSE in the list, FINE while editing

// --- Display (Core 1) ---
void home_get_snapshot(home_snapshot_t *out);
uint32_t home_version(void);

// --- usb task (ext_link.c): the import. Staged one lamp at a time, then stored together. ---
void home_import_begin(void);
bool home_import_lamp(int slot, const home_lamp_cfg_t *lamp);
bool home_import_commit(int count); // NVS; the home task picks the new list up and looks again
int home_lamp_count(void);
// The companion's edits (EXT_HOME_EDIT; usb task): the lamp in `slot`, if it is still the one with
// device id `did`. NAME: `name`; KIND: `value` = HOME_KIND_*; MOVE: `value` = its new slot;
// REMOVE. Stored at once (NVS); the knob keeps talking to the lamps (no new scan).
enum { HOME_EDIT_NAME = 1, HOME_EDIT_KIND, HOME_EDIT_MOVE, HOME_EDIT_REMOVE };
bool home_edit(int slot, uint32_t did, int what, int value, const char *name);
