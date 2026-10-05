#include "midi.h"
#include "tasks_common.h"
#include <string.h>

// The synth profiles: each one's parameters as its maker documents them. Names are upper case
// and short (the screen has ~10 characters at scale 2).
//
// Sources (fetched 2026-10-05):
// - KORG minilogue xd MIDI Implementation, revision 1.01 (2020.2.10), minilogue_xd__MIDIImp.txt
//   from korg.com. Continuous parameters marked *1-4 / *5-4 are 10-bit (CC 63 first); the
//   switches send the values of its notes *2-01..*2-17 and are read back by its zones *6-xx.
// - Roland JU-06A MIDI Implementation Chart, Version 1.00 (Sep. 5, 2019), sound module page.
// - Roland TR-8S MIDI Implementation Chart, Version 1.02 (Feb. 20, 2018).
// Roland's charts list each CC but not a switch's values; the knob sends evenly spaced values
// (2 options: 0 / 127, 3: 0 / 64 / 127, 4: 0 / 43 / 85 / 127), which a synth that splits 0..127
// into equal zones reads as intended. To check on the hardware.

#define CC(g, n, c) {g, n, MIDI_P_CC, c, 0, {0}, {0}, false}
#define CCB(g, n, c) {g, n, MIDI_P_CC, c, 0, {0}, {0}, true}
#define K10(g, n, c) {g, n, MIDI_P_KORG10, c, 0, {0}, {0}, false}
#define K10B(g, n, c) {g, n, MIDI_P_KORG10, c, 0, {0}, {0}, true}
#define SW(g, n, c, cnt, vals, names) {g, n, MIDI_P_CC, c, cnt, vals, names, false}
#define V(...) {__VA_ARGS__}

#define ONOFF V("OFF", "ON")
#define V2 V(0, 127)
#define V3 V(0, 64, 127)
#define V4 V(0, 43, 85, 127)

// --- GENERIC: General MIDI / GM2 controllers, for a DAW's MIDI learn or any synth ---
static const midi_param_t GENERIC[] = {
    CC("CONTROL", "MOD WHEEL", 1),
    CC("CONTROL", "BREATH", 2),
    CC("CONTROL", "PORTA TIME", 5),
    SW("CONTROL", "SUSTAIN", 64, 2, V2, ONOFF),
    CC("MIX", "VOLUME", 7),
    CCB("MIX", "PAN", 10),
    CC("MIX", "EXPRESSION", 11),
    CC("SOUND", "CUTOFF", 74),     // GM2 brightness
    CC("SOUND", "RESONANCE", 71),  // GM2 timbre / harmonic intensity
    CC("SOUND", "ATTACK", 73),
    CC("SOUND", "DECAY", 75),
    CC("SOUND", "RELEASE", 72),
    CC("FX", "REVERB", 91),
    CC("FX", "CHORUS", 93),
    CC("GENERAL", "GP 1", 16),
    CC("GENERAL", "GP 2", 17),
    CC("GENERAL", "GP 3", 18),
    CC("GENERAL", "GP 4", 19),
};

// --- KORG minilogue xd ---
// SYNC and RING: the implementation's own tables disagree (*2-08 sends OFF, ON = 0, 127; *6-08
// reads 0~63, 64~127 = ON, OFF). The knob follows what the synth reads (*6-08). To check.
static const midi_param_t MINILOGUE_XD[] = {
    K10B("VCO 1", "PITCH", 34),
    K10("VCO 1", "SHAPE", 36),
    K10("VCO 1", "LEVEL", 39),
    SW("VCO 1", "OCTAVE", 48, 4, V(0, 42, 84, 127), V("16'", "8'", "4'", "2'")),
    SW("VCO 1", "WAVE", 50, 3, V3, V("SQR", "TRI", "SAW")),
    K10B("VCO 2", "PITCH", 35),
    K10("VCO 2", "SHAPE", 37),
    K10("VCO 2", "LEVEL", 40),
    SW("VCO 2", "OCTAVE", 49, 4, V(0, 42, 84, 127), V("16'", "8'", "4'", "2'")),
    SW("VCO 2", "WAVE", 51, 3, V3, V("SQR", "TRI", "SAW")),
    K10("VCO 2", "CROSS MOD", 41),
    SW("VCO 2", "SYNC", 80, 2, V(127, 0), ONOFF),
    SW("VCO 2", "RING", 81, 2, V(127, 0), ONOFF),
    SW("MULTI", "TYPE", 53, 3, V3, V("NOISE", "VPM", "USER")),
    K10("MULTI", "SHAPE", 54),
    K10("MULTI", "SHIFT SHP", 104),
    K10("MULTI", "LEVEL", 33),
    K10("FILTER", "CUTOFF", 43),
    K10("FILTER", "RESONANCE", 44),
    SW("FILTER", "DRIVE", 84, 3, V3, V("0%", "50%", "100%")),
    SW("FILTER", "KEYTRACK", 83, 3, V3, V("0%", "50%", "100%")),
    K10("AMP EG", "ATTACK", 16),
    K10("AMP EG", "DECAY", 17),
    K10("AMP EG", "SUSTAIN", 18),
    K10("AMP EG", "RELEASE", 19),
    K10("EG", "ATTACK", 20),
    K10("EG", "DECAY", 21),
    K10B("EG", "INT", 22),
    SW("EG", "TARGET", 23, 3, V3, V("CUTOFF", "PITCH 2", "PITCH")),
    SW("LFO", "WAVE", 57, 3, V3, V("SQR", "TRI", "SAW")),
    SW("LFO", "MODE", 58, 3, V3, V("1-SHOT", "NORMAL", "BPM")),
    K10("LFO", "RATE", 24),
    K10("LFO", "INT", 26),
    SW("LFO", "TARGET", 56, 3, V3, V("CUTOFF", "SHAPE", "PITCH")),
    K10("VOICE", "MODE DEPTH", 27),
    CC("VOICE", "PORTAMENTO", 5),
    SW("MOD FX", "ON", 92, 2, V2, ONOFF),
    SW("MOD FX", "TYPE", 88, 5, V(0, 38, 64, 84, 127), V("CHORUS", "ENSEMBLE", "PHASER", "FLANGER", "USER")),
    K10("MOD FX", "TIME", 28),
    K10("MOD FX", "DEPTH", 29),
    SW("DELAY", "ON", 93, 2, V2, ONOFF),
    K10("DELAY", "TIME", 105),
    K10("DELAY", "DEPTH", 106),
    K10("DELAY", "MIX", 107),
    SW("REVERB", "ON", 94, 2, V2, ONOFF),
    K10("REVERB", "TIME", 108),
    K10("REVERB", "DEPTH", 109),
    K10("REVERB", "MIX", 110),
};

// --- Roland JU-06A (sound module chart; not the Control Surface Mode page) ---
// Left out: LFO WAVE (29), LFO TRIG (30) and BEND RANGE (87), whose options the chart doesn't
// name.
static const midi_param_t JU_06A[] = {
    CC("LFO", "RATE", 3),
    CC("LFO", "DELAY", 9),
    SW("DCO", "RANGE", 12, 3, V3, V("16'", "8'", "4'")),
    CC("DCO", "LFO", 13),
    CC("DCO", "PWM", 14),
    SW("DCO", "PWM MODE", 15, 3, V3, V("LFO", "MAN", "ENV")),
    SW("DCO", "SQUARE", 16, 2, V2, ONOFF),
    SW("DCO", "SAW", 17, 2, V2, ONOFF),
    CC("DCO", "SUB", 18),
    SW("DCO", "SUB SW", 28, 2, V2, ONOFF),
    CC("DCO", "NOISE", 19),
    CC("HPF", "FREQ", 20),
    CC("VCF", "FREQ", 74),
    CC("VCF", "RES", 71),
    CC("VCF", "ENV", 22),
    SW("VCF", "ENV POL", 21, 2, V2, V("POS", "NEG")),
    CC("VCF", "LFO", 23),
    CC("VCF", "KYBD", 24),
    SW("VCA", "MODE", 25, 2, V2, V("ENV", "GATE")),
    CC("VCA", "LEVEL", 26),
    CC("ENV", "ATTACK", 73),
    CC("ENV", "DECAY", 75),
    CC("ENV", "SUSTAIN", 27),
    CC("ENV", "RELEASE", 72),
    SW("CHORUS", "MODE", 93, 4, V4, V("OFF", "I", "II", "I+II")),
    SW("DELAY", "ON", 89, 2, V2, ONOFF),
    CC("DELAY", "TIME", 82),
    CC("DELAY", "FEEDBACK", 83),
    CC("DELAY", "LEVEL", 91),
    SW("DELAY", "SYNC", 88, 2, V2, ONOFF),
    CC("VOICE", "PORTA", 5),
    SW("VOICE", "PORTA SW", 65, 2, V2, ONOFF),
    SW("VOICE", "MODE", 86, 4, V4, V("POLY", "(POLY)", "SOLO", "UNISON")),
};

// --- Roland TR-8S ---
#define TR_INST(n, tune, decay, level, ctrl) \
    CCB(n, "TUNE", tune), CC(n, "DECAY", decay), CC(n, "LEVEL", level), CC(n, "CTRL", ctrl)
static const midi_param_t TR_8S[] = {
    TR_INST("BD", 20, 23, 24, 96),
    TR_INST("SD", 25, 28, 29, 97),
    TR_INST("LT", 46, 47, 48, 102),
    TR_INST("MT", 49, 50, 51, 103),
    TR_INST("HT", 52, 53, 54, 104),
    TR_INST("RS", 55, 56, 57, 105),
    TR_INST("HC", 58, 59, 60, 106),
    TR_INST("CH", 61, 62, 63, 107),
    TR_INST("OH", 80, 81, 82, 108),
    TR_INST("CC", 83, 84, 85, 109),
    TR_INST("RC", 86, 87, 88, 110),
    CC("MASTER", "ACCENT", 71),
    CCB("MASTER", "SHUFFLE", 9),
    CC("MASTER", "EXT IN", 12),
    SW("MASTER", "AUTO FILL", 14, 2, V2, ONOFF),
    CC("FX", "REVERB", 91),
    CC("FX", "DELAY LVL", 16),
    CC("FX", "DELAY TIME", 17),
    CC("FX", "DELAY FB", 18),
    SW("FX", "MASTER FX", 15, 2, V2, ONOFF),
    CC("FX", "FX CTRL", 19),
};

#define N(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))
_Static_assert(sizeof(GENERIC) / sizeof(GENERIC[0]) <= MIDI_MAX_PARAMS, "GENERIC");
_Static_assert(sizeof(MINILOGUE_XD) / sizeof(MINILOGUE_XD[0]) <= MIDI_MAX_PARAMS, "MINILOGUE_XD");
_Static_assert(sizeof(JU_06A) / sizeof(JU_06A[0]) <= MIDI_MAX_PARAMS, "JU_06A");
_Static_assert(sizeof(TR_8S) / sizeof(TR_8S[0]) <= MIDI_MAX_PARAMS, "TR_8S");

static const midi_synth_t SYNTHS[] = {
    {"generic", "MIDI", "GENERIC", 0, MIDI_PROG_PC, 128, N(GENERIC), GENERIC},
    {"minilogue-xd", "KORG", "MINILOGUE XD", 1, MIDI_PROG_KORG_BANK100, 500, N(MINILOGUE_XD), MINILOGUE_XD},
    {"ju-06a", "ROLAND", "JU-06A", 1, MIDI_PROG_PC, 64, N(JU_06A), JU_06A},
    {"tr-8s", "ROLAND", "TR-8S", 10, MIDI_PROG_PC, 128, N(TR_8S), TR_8S},
};

// IRAM: the menu's SYNTH row asks it at a detent crossing (midi_synth_at_end, menu.c).
int CONTROL_HOT midi_synth_count(void) { return (int)(sizeof(SYNTHS) / sizeof(SYNTHS[0])); }

const midi_synth_t *midi_synth_get(int i) {
    if (i < 0) i = 0;
    if (i >= midi_synth_count()) i = midi_synth_count() - 1;
    return &SYNTHS[i];
}

int midi_synth_find(const char *id) {
    for (int i = 0; i < midi_synth_count(); i++) {
        if (strcmp(SYNTHS[i].id, id) == 0) return i;
    }
    return -1;
}
