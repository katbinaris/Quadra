#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// MIDI: the knob as a controller for a synth. It sends on two ports at once: USB MIDI (class
// compliant, in the MIDI USB personality, usb_task.c) and the board's TRS jacks (UART1 on GPIO
// 43 / 44, 31250 baud). A synth profile (midi_synths.c) lists the synth's parameters from its
// maker's MIDI implementation; the knob turns one at a time, F1 moves on (hold F1 and turn to
// pick one from the list), F2 / F3 step the synth's program down / up.
//
// Three sides, like HOME (home.h):
// - Core 0 (the control loop): turns and keys go into a small lock-free ring (midi_input_*),
//   never blocking; midi_at_end() and midi_haptic_profile() are single atomic loads.
// - The `midi` task (Core 1): applies the inputs, sends the messages, reads what comes back (a
//   synth that sends its own knob moves updates the value shown).
// - The display and the LEDs (Core 1): copy a snapshot (midi_get_snapshot) and draw it.

#define MIDI_MAX_PARAMS 64
#define MIDI_MAX_OPTS 8

typedef enum {
    MIDI_P_CC = 0,  // one 7-bit control change: 0..127
    MIDI_P_KORG10,  // KORG's 10-bit CC: CC 63 carries the low 3 bits first, then the CC the upper
                    // 7 (minilogue xd MIDI Implementation, note *1-4 / *5-4): 0..1023
} midi_param_kind_t;

typedef enum {
    MIDI_PROG_PC = 0,       // program change 0..prog_count-1
    MIDI_PROG_KORG_BANK100, // bank select MSB 0 + LSB n / 100, then program change n % 100
} midi_prog_scheme_t;

typedef struct {
    const char *group; // what the page's caption shows: "FILTER", "BD"
    const char *name;  // "CUTOFF", "TUNE"
    uint8_t kind;      // midi_param_kind_t
    uint8_t cc;
    // A switch: n_opts > 1, the value sent for each option and its name. 0: a continuous value.
    uint8_t n_opts;
    uint8_t opt_value[MIDI_MAX_OPTS];
    const char *opt_name[MIDI_MAX_OPTS];
    bool bipolar; // centred (pan, tune): the screen and the ring mark the middle
} midi_param_t;

typedef struct {
    const char *id;    // what NVS stores ("minilogue-xd")
    const char *maker; // "KORG"
    const char *name;  // "MINILOGUE XD"
    uint8_t channel;   // the synth's factory channel, shown as a hint (0: none)
    uint8_t prog_scheme;
    uint16_t prog_count;
    uint8_t n_params;
    const midi_param_t *params;
} midi_synth_t;

// --- the synth profiles (midi_synths.c) ---
// The built-ins (compiled in) come first, in a fixed order; then the user's own, from LittleFS
// (/fs/synths/<id>.json). A built-in with a stored copy of the same id shows that copy. The
// companion edits them like app profiles: an upload is live at once, SAVE stores it, REVERT goes
// back to what's stored (or built in). The list is only changed from the usb task; everyone else
// reads it: midi_synth_get()'s pointer stays good for a second after a change replaces it.
#define MIDI_MAX_SYNTHS 16
#define MIDI_ID_MAX 23    // [a-z0-9-]
#define MIDI_MAKER_MAX 11
#define MIDI_NAME_MAX 15  // the synth's, on the knob's header
#define MIDI_LABEL_MAX 11 // a parameter's group and name
#define MIDI_OPT_MAX 9    // an option's name
#define MIDI_JSON_MAX (16 * 1024)

#define MIDI_SYNTH_BUILTIN 0x01 // compiled in
#define MIDI_SYNTH_STORED 0x02  // a file in LittleFS (for a built-in: a changed copy)
#define MIDI_SYNTH_LIVE 0x04    // edited, differs from what's stored (or built in)

typedef enum {
    MIDI_SYNTH_OK = 0,
    MIDI_SYNTH_ERR_INVALID, // not a synth profile (why says what)
    MIDI_SYNTH_ERR_FULL,    // MIDI_MAX_SYNTHS already
    MIDI_SYNTH_ERR_STORAGE,
    MIDI_SYNTH_ERR_INDEX,
} midi_synth_err_t;
enum { MIDI_SYNTH_OP_SAVE = 1, MIDI_SYNTH_OP_REVERT = 2, MIDI_SYNTH_OP_REMOVE = 3 };

void midi_synths_init(void); // app_main: after LittleFS is mounted (app_profiles_init), before menu_init
int midi_synth_count(void);  // CONTROL_HOT
const midi_synth_t *midi_synth_get(int i); // clamped
int midi_synth_find(const char *id);       // -1: not there
uint8_t midi_synth_flags(int i);           // MIDI_SYNTH_*
uint32_t midi_synth_gen(void);             // moves on with every change to the list or a synth
// usb task only:
char *midi_synth_json(int i, size_t *len); // the profile as JSON (malloc'd, PSRAM); NULL: no such
// Live at once (replacing the one with its id, else added); `save` stores it too.
midi_synth_err_t midi_synth_put(const char *json, size_t len, bool save, int *index, char *why, size_t why_n);
midi_synth_err_t midi_synth_op(int index, int op, bool *removed);
void midi_synths_reap(void); // frees what a change replaced, once nobody can still be reading it

// A continuous parameter's range: 0..127, or 0..1023 for KORG10.
static inline int midi_param_max(const midi_param_t *p) {
    return p->n_opts > 1 ? p->n_opts - 1 : p->kind == MIDI_P_KORG10 ? 1023 : 127;
}

// --- what the screen and the LEDs show ---
typedef struct {
    uint32_t version; // bumps on every change the screen should show
    bool active;      // MIDI mode, menu closed
    int synth;
    int param;
    int value;        // -1: unknown (not turned or received since the synth or program changed)
    bool browsing;    // F1 held: the knob picks the parameter
    int prog;         // the program sent last (0-based), -1: none yet
    uint32_t prog_ms; // when (ms since boot): the screen shows it for a moment
    uint8_t channel;  // 1..16
    bool usb;         // USB MIDI is up (the MIDI personality, a host has it open)
    bool trs;         // the TRS port is up
    uint32_t tx, rx;  // messages sent / received, for the activity dots
} midi_snapshot_t;

// app_main: starts the midi task (after menu_init()).
void midi_start(void);

// --- Core 0 (CONTROL_HOT; never block) ---
void midi_input_rotate(int8_t dir);     // one detent
void midi_input_key(uint8_t key, bool down); // UI_BTN_F1 down / up, F2 / F3 down
bool midi_at_end(int8_t dir);           // the value, or the list, is at its end that way: a wall
int midi_haptic_profile(void);          // HAPTIC_PROFILE_*: the parameter's, COARSE while picking

// --- display / LEDs (Core 1) ---
void midi_get_snapshot(midi_snapshot_t *out);
uint32_t midi_version(void);

// The companion: the knob moves to parameter `param` of the synth in use (as if picked with F1).
void midi_goto(int param);
