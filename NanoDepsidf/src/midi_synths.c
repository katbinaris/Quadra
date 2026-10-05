#include "midi.h"
#include "tasks_common.h"
#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "synths";

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
#define BUILTINS ((int)(sizeof(SYNTHS) / sizeof(SYNTHS[0])))

// --- the list: the built-ins, then the user's own ---
// Each entry is a pointer, swapped whole: a reader that took one keeps a complete synth. One that
// came from JSON is a single PSRAM block (the synth, its parameters, then their strings); when a
// change replaces it, the old block waits in s_dead for a second before it's freed (a screen frame
// or a midi task pass is done with it long before).
#ifndef MIDI_SYNTH_DIR
#define MIDI_SYNTH_DIR "/fs/synths" // LittleFS (profile_store.c mounts it); the host test points it elsewhere
#endif
#define DIR_PATH MIDI_SYNTH_DIR
#define DEAD_MAX 24
#define DEAD_US 1000000

EXT_RAM_BSS_ATTR static const midi_synth_t *_Atomic s_list[MIDI_MAX_SYNTHS];
EXT_RAM_BSS_ATTR static void *s_block[MIDI_MAX_SYNTHS]; // the block behind s_list[i], NULL: a built-in's table
EXT_RAM_BSS_ATTR static _Atomic uint8_t s_flags[MIDI_MAX_SYNTHS];
static _Atomic int s_count = 0; // internal RAM: read on the control loop (menu.c)
static _Atomic uint32_t s_gen = 1;
EXT_RAM_BSS_ATTR static struct {
    void *p;
    int64_t at;
} s_dead[DEAD_MAX];

// IRAM: the menu's SYNTH row asks it at a detent crossing (midi_synth_at_end, menu.c).
int CONTROL_HOT midi_synth_count(void) { return atomic_load_explicit(&s_count, memory_order_relaxed); }

const midi_synth_t *midi_synth_get(int i) {
    int n = midi_synth_count();
    if (i >= n) i = n - 1;
    if (i < 0) i = 0;
    const midi_synth_t *s = atomic_load(&s_list[i]);
    return s ? s : &SYNTHS[0];
}

int midi_synth_find(const char *id) {
    for (int i = 0; i < midi_synth_count(); i++) {
        if (strcmp(midi_synth_get(i)->id, id) == 0) return i;
    }
    return -1;
}

uint8_t midi_synth_flags(int i) { return i >= 0 && i < midi_synth_count() ? atomic_load(&s_flags[i]) : 0; }
uint32_t midi_synth_gen(void) { return atomic_load(&s_gen); }

static void retire(void *block) {
    if (block == NULL) return;
    for (int i = 0; i < DEAD_MAX; i++) {
        if (s_dead[i].p == NULL) {
            s_dead[i].p = block;
            s_dead[i].at = esp_timer_get_time();
            return;
        }
    }
    ESP_LOGW(TAG, "too many changes at once: a replaced synth is kept"); // a leak, not a crash
}

void midi_synths_reap(void) {
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < DEAD_MAX; i++) {
        if (s_dead[i].p != NULL && now - s_dead[i].at >= DEAD_US) {
            heap_caps_free(s_dead[i].p);
            s_dead[i].p = NULL;
        }
    }
}

// Slot `i` now shows `s` (its block, NULL for a built-in table), with `flags`.
static void set_slot(int i, const midi_synth_t *s, void *block, uint8_t flags) {
    void *old = s_block[i];
    s_block[i] = block;
    atomic_store(&s_flags[i], flags);
    atomic_store(&s_list[i], s);
    if (old != block) retire(old);
    atomic_fetch_add(&s_gen, 1);
}

static void remove_slot(int i) {
    int n = midi_synth_count();
    void *old = s_block[i];
    // Shrink first, so nobody indexes past the end while the rest move down.
    atomic_store(&s_count, n - 1);
    for (int j = i; j < n - 1; j++) {
        s_block[j] = s_block[j + 1];
        atomic_store(&s_flags[j], atomic_load(&s_flags[j + 1]));
        atomic_store(&s_list[j], atomic_load(&s_list[j + 1]));
    }
    s_block[n - 1] = NULL;
    atomic_store(&s_list[n - 1], NULL);
    retire(old);
    atomic_fetch_add(&s_gen, 1);
}

// --- JSON ---
// {"id": "minilogue-xd", "maker": "KORG", "name": "MINILOGUE XD", "channel": 1,
//  "programs": {"scheme": "pc" | "korg-bank100", "count": 500},
//  "params": [{"group": "VCO 1", "name": "PITCH", "sends": "cc" | "korg10", "cc": 34,
//              "centred": true, "options": [{"name": "SQR", "value": 0}, ...]}, ...]}

static bool valid_text(const cJSON *v, int max, bool allow_empty) {
    if (!cJSON_IsString(v)) return false;
    size_t n = strlen(v->valuestring);
    if ((n == 0 && !allow_empty) || n > (size_t)max) return false;
    for (const char *c = v->valuestring; *c; c++) {
        if (*c < 0x20 || *c > 0x7E) return false;
    }
    return true;
}

static bool valid_id(const char *id) {
    size_t n = strlen(id);
    if (n == 0 || n > MIDI_ID_MAX) return false;
    for (const char *c = id; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) return false;
    }
    return true;
}

static int int_in(const cJSON *v, int lo, int hi, int dflt, bool *ok) {
    if (v == NULL) return dflt;
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi || v->valuedouble != (int)v->valuedouble) {
        *ok = false;
        return dflt;
    }
    return (int)v->valuedouble;
}

#define FAIL(...)                              \
    do {                                       \
        snprintf(why, why_n, __VA_ARGS__);     \
        return NULL;                           \
    } while (0)

// Checks `root` and builds the synth in one PSRAM block. NULL: not valid (why says what).
static midi_synth_t *from_json(const cJSON *root, char *why, size_t why_n) {
    if (!cJSON_IsObject(root)) FAIL("not a JSON object");
    const cJSON *id = cJSON_GetObjectItem(root, "id"), *maker = cJSON_GetObjectItem(root, "maker"),
                *name = cJSON_GetObjectItem(root, "name"), *params = cJSON_GetObjectItem(root, "params"),
                *progs = cJSON_GetObjectItem(root, "programs");
    if (!cJSON_IsString(id) || !valid_id(id->valuestring)) FAIL("id: 1-%d of a-z, 0-9, -", MIDI_ID_MAX);
    if (!valid_text(maker, MIDI_MAKER_MAX, true)) FAIL("maker: up to %d characters", MIDI_MAKER_MAX);
    if (!valid_text(name, MIDI_NAME_MAX, false)) FAIL("name: 1-%d characters", MIDI_NAME_MAX);
    int n = cJSON_GetArraySize(params);
    if (!cJSON_IsArray(params) || n < 1 || n > MIDI_MAX_PARAMS) FAIL("params: 1-%d of them", MIDI_MAX_PARAMS);
    bool ok = true;
    int channel = int_in(cJSON_GetObjectItem(root, "channel"), 0, 16, 0, &ok);
    int scheme = MIDI_PROG_PC, count = 128;
    if (progs != NULL) {
        const cJSON *sc = cJSON_GetObjectItem(progs, "scheme");
        if (cJSON_IsString(sc) && strcmp(sc->valuestring, "korg-bank100") == 0) scheme = MIDI_PROG_KORG_BANK100;
        else if (sc != NULL && !(cJSON_IsString(sc) && strcmp(sc->valuestring, "pc") == 0)) FAIL("programs.scheme: pc or korg-bank100");
        count = int_in(cJSON_GetObjectItem(progs, "count"), 1, scheme == MIDI_PROG_KORG_BANK100 ? 12800 : 128, count, &ok);
    }
    if (!ok) FAIL("channel 0-16, programs.count in range");

    // Measure: the strings' room.
    size_t text = strlen(id->valuestring) + strlen(maker->valuestring) + strlen(name->valuestring) + 3;
    for (int i = 0; i < n; i++) {
        const cJSON *p = cJSON_GetArrayItem(params, i);
        const cJSON *g = cJSON_GetObjectItem(p, "group"), *pn = cJSON_GetObjectItem(p, "name"),
                    *sends = cJSON_GetObjectItem(p, "sends"), *opts = cJSON_GetObjectItem(p, "options");
        if (!cJSON_IsObject(p)) FAIL("param %d: not an object", i + 1);
        if (!valid_text(g, MIDI_LABEL_MAX, true)) FAIL("param %d: group up to %d characters", i + 1, MIDI_LABEL_MAX);
        if (!valid_text(pn, MIDI_LABEL_MAX, false)) FAIL("param %d: name 1-%d characters", i + 1, MIDI_LABEL_MAX);
        if (sends != NULL && !(cJSON_IsString(sends) && (strcmp(sends->valuestring, "cc") == 0 || strcmp(sends->valuestring, "korg10") == 0)))
            FAIL("param %d: sends cc or korg10", i + 1);
        int_in(cJSON_GetObjectItem(p, "cc"), 0, 127, 0, &ok);
        if (!ok || cJSON_GetObjectItem(p, "cc") == NULL) FAIL("param %d: cc 0-127", i + 1);
        text += strlen(g->valuestring) + strlen(pn->valuestring) + 2;
        if (opts != NULL) {
            int k = cJSON_GetArraySize(opts);
            if (!cJSON_IsArray(opts) || k < 2 || k > MIDI_MAX_OPTS) FAIL("param %d: 2-%d options", i + 1, MIDI_MAX_OPTS);
            for (int o = 0; o < k; o++) {
                const cJSON *opt = cJSON_GetArrayItem(opts, o), *on = cJSON_GetObjectItem(opt, "name");
                if (!valid_text(on, MIDI_OPT_MAX, false)) FAIL("param %d option %d: name 1-%d characters", i + 1, o + 1, MIDI_OPT_MAX);
                int_in(cJSON_GetObjectItem(opt, "value"), 0, 127, 0, &ok);
                if (!ok || cJSON_GetObjectItem(opt, "value") == NULL) FAIL("param %d option %d: value 0-127", i + 1, o + 1);
                text += strlen(on->valuestring) + 1;
            }
        }
    }

    size_t size = sizeof(midi_synth_t) + (size_t)n * sizeof(midi_param_t) + text;
    uint8_t *block = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
    if (block == NULL) FAIL("out of memory");
    midi_synth_t *s = (midi_synth_t *)block;
    midi_param_t *ps = (midi_param_t *)(block + sizeof(midi_synth_t));
    char *at = (char *)(ps + n);
    // Copies `src` into the block's string room.
    #define PUT(dst, src)                \
        do {                             \
            size_t _l = strlen(src) + 1; \
            memcpy(at, (src), _l);       \
            (dst) = at;                  \
            at += _l;                    \
        } while (0)
    PUT(s->id, id->valuestring);
    PUT(s->maker, maker->valuestring);
    PUT(s->name, name->valuestring);
    s->channel = (uint8_t)channel;
    s->prog_scheme = (uint8_t)scheme;
    s->prog_count = (uint16_t)count;
    s->n_params = (uint8_t)n;
    s->params = ps;
    for (int i = 0; i < n; i++) {
        const cJSON *p = cJSON_GetArrayItem(params, i), *opts = cJSON_GetObjectItem(p, "options"),
                    *sends = cJSON_GetObjectItem(p, "sends");
        midi_param_t *q = &ps[i];
        PUT(q->group, cJSON_GetObjectItem(p, "group")->valuestring);
        PUT(q->name, cJSON_GetObjectItem(p, "name")->valuestring);
        q->cc = (uint8_t)cJSON_GetObjectItem(p, "cc")->valuedouble;
        q->kind = sends && strcmp(sends->valuestring, "korg10") == 0 ? MIDI_P_KORG10 : MIDI_P_CC;
        q->bipolar = cJSON_IsTrue(cJSON_GetObjectItem(p, "centred"));
        if (opts != NULL) {
            q->n_opts = (uint8_t)cJSON_GetArraySize(opts);
            q->kind = MIDI_P_CC; // a switch sends one CC value per option
            for (int o = 0; o < q->n_opts; o++) {
                const cJSON *opt = cJSON_GetArrayItem(opts, o);
                PUT(q->opt_name[o], cJSON_GetObjectItem(opt, "name")->valuestring);
                q->opt_value[o] = (uint8_t)cJSON_GetObjectItem(opt, "value")->valuedouble;
            }
        }
    }
    #undef PUT
    return s;
}
#undef FAIL

static cJSON *to_json(const midi_synth_t *s) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    cJSON_AddStringToObject(root, "id", s->id);
    cJSON_AddStringToObject(root, "maker", s->maker);
    cJSON_AddStringToObject(root, "name", s->name);
    cJSON_AddNumberToObject(root, "channel", s->channel);
    cJSON *progs = cJSON_AddObjectToObject(root, "programs");
    cJSON_AddStringToObject(progs, "scheme", s->prog_scheme == MIDI_PROG_KORG_BANK100 ? "korg-bank100" : "pc");
    cJSON_AddNumberToObject(progs, "count", s->prog_count);
    cJSON *params = cJSON_AddArrayToObject(root, "params");
    for (int i = 0; i < s->n_params; i++) {
        const midi_param_t *p = &s->params[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "group", p->group);
        cJSON_AddStringToObject(o, "name", p->name);
        cJSON_AddStringToObject(o, "sends", p->kind == MIDI_P_KORG10 ? "korg10" : "cc");
        cJSON_AddNumberToObject(o, "cc", p->cc);
        if (p->bipolar) cJSON_AddTrueToObject(o, "centred");
        if (p->n_opts > 1) {
            cJSON *opts = cJSON_AddArrayToObject(o, "options");
            for (int k = 0; k < p->n_opts; k++) {
                cJSON *x = cJSON_CreateObject();
                cJSON_AddStringToObject(x, "name", p->opt_name[k]);
                cJSON_AddNumberToObject(x, "value", p->opt_value[k]);
                cJSON_AddItemToArray(opts, x);
            }
        }
        cJSON_AddItemToArray(params, o);
    }
    return root;
}

char *midi_synth_json(int i, size_t *len) {
    if (i < 0 || i >= midi_synth_count()) return NULL;
    cJSON *root = to_json(midi_synth_get(i));
    char *text = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (text && len) *len = strlen(text);
    return text;
}

// --- files ---
static void path_for(char *out, size_t n, const char *id, const char *ext) { snprintf(out, n, DIR_PATH "/%s%s", id, ext); }

static midi_synth_t *load_file(const char *id) {
    char path[96], why[64];
    path_for(path, sizeof(path), id, ".json");
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    midi_synth_t *s = NULL;
    char *buf = size > 0 && size <= MIDI_JSON_MAX ? heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM) : NULL;
    if (buf && fread(buf, 1, size, f) == (size_t)size) {
        buf[size] = '\0';
        cJSON *root = cJSON_ParseWithLength(buf, size);
        s = from_json(root, why, sizeof(why));
        cJSON_Delete(root);
        if (s == NULL) ESP_LOGW(TAG, "%s: %s", path, why);
        else if (strcmp(s->id, id) != 0) {
            ESP_LOGW(TAG, "%s: its id is %s -- skipped", path, s->id);
            heap_caps_free(s);
            s = NULL;
        }
    }
    heap_caps_free(buf);
    fclose(f);
    return s;
}

static bool write_file(const midi_synth_t *s) {
    cJSON *root = to_json(s);
    char *text = root ? cJSON_Print(root) : NULL; // indented: it's meant to be read and shared
    cJSON_Delete(root);
    if (text == NULL) return false;
    char path[96], tmp[96];
    path_for(path, sizeof(path), s->id, ".json");
    path_for(tmp, sizeof(tmp), s->id, ".tmp");
    size_t len = strlen(text);
    FILE *f = fopen(tmp, "wb");
    bool ok = f != NULL && fwrite(text, 1, len, f) == len;
    if (f) ok = (fclose(f) == 0) && ok;
    cJSON_free(text);
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    ESP_LOGI(TAG, "%s: %s", path, ok ? "saved" : "NOT saved");
    return ok;
}

static bool delete_file(const char *id) {
    char path[96];
    path_for(path, sizeof(path), id, ".json");
    return remove(path) == 0;
}

static int builtin_index(const char *id) {
    for (int i = 0; i < BUILTINS; i++) {
        if (strcmp(SYNTHS[i].id, id) == 0) return i;
    }
    return -1;
}

void midi_synths_init(void) {
    for (int i = 0; i < BUILTINS; i++) {
        atomic_store(&s_list[i], &SYNTHS[i]);
        atomic_store(&s_flags[i], MIDI_SYNTH_BUILTIN);
    }
    atomic_store(&s_count, BUILTINS);
    mkdir(DIR_PATH, 0775); // EEXIST after the first boot; fails quietly with no LittleFS
    DIR *d = opendir(DIR_PATH);
    if (d == NULL) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char id[MIDI_ID_MAX + 1];
        size_t len = strlen(e->d_name);
        if (len <= 5 || len - 5 > MIDI_ID_MAX || strcmp(e->d_name + len - 5, ".json") != 0) continue;
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        midi_synth_t *s = load_file(id);
        if (s == NULL) continue;
        int b = builtin_index(id);
        if (b >= 0) {
            set_slot(b, s, s, MIDI_SYNTH_BUILTIN | MIDI_SYNTH_STORED);
        } else if (midi_synth_count() < MIDI_MAX_SYNTHS) {
            int n = midi_synth_count();
            set_slot(n, s, s, MIDI_SYNTH_STORED);
            atomic_store(&s_count, n + 1);
        } else {
            heap_caps_free(s);
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "%d synth profiles (%d built in)", midi_synth_count(), BUILTINS);
}

// --- the companion's edits (usb task) ---
midi_synth_err_t midi_synth_put(const char *json, size_t len, bool save, int *index, char *why, size_t why_n) {
    why[0] = '\0';
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        snprintf(why, why_n, "not JSON");
        return MIDI_SYNTH_ERR_INVALID;
    }
    midi_synth_t *s = from_json(root, why, why_n);
    cJSON_Delete(root);
    if (s == NULL) return MIDI_SYNTH_ERR_INVALID;
    int i = midi_synth_find(s->id);
    uint8_t flags;
    if (i >= 0) {
        flags = (midi_synth_flags(i) & (MIDI_SYNTH_BUILTIN | MIDI_SYNTH_STORED)) | MIDI_SYNTH_LIVE;
        set_slot(i, s, s, flags);
    } else {
        i = midi_synth_count();
        if (i >= MIDI_MAX_SYNTHS) {
            heap_caps_free(s);
            snprintf(why, why_n, "%d synths already", MIDI_MAX_SYNTHS);
            return MIDI_SYNTH_ERR_FULL;
        }
        set_slot(i, s, s, MIDI_SYNTH_LIVE);
        atomic_store(&s_count, i + 1);
    }
    *index = i;
    if (save) return midi_synth_op(i, MIDI_SYNTH_OP_SAVE, NULL);
    return MIDI_SYNTH_OK;
}

midi_synth_err_t midi_synth_op(int index, int op, bool *removed) {
    if (removed) *removed = false;
    if (index < 0 || index >= midi_synth_count()) return MIDI_SYNTH_ERR_INDEX;
    const midi_synth_t *s = midi_synth_get(index);
    uint8_t flags = midi_synth_flags(index);
    int b = builtin_index(s->id);
    switch (op) {
        case MIDI_SYNTH_OP_SAVE:
            if (!write_file(s)) return MIDI_SYNTH_ERR_STORAGE;
            atomic_store(&s_flags[index], (uint8_t)((flags | MIDI_SYNTH_STORED) & ~MIDI_SYNTH_LIVE));
            atomic_fetch_add(&s_gen, 1);
            return MIDI_SYNTH_OK;
        case MIDI_SYNTH_OP_REVERT:
            if (flags & MIDI_SYNTH_STORED) {
                midi_synth_t *f = load_file(s->id);
                if (f == NULL) return MIDI_SYNTH_ERR_STORAGE;
                set_slot(index, f, f, (uint8_t)(flags & ~MIDI_SYNTH_LIVE));
            } else if (b >= 0) {
                set_slot(index, &SYNTHS[b], NULL, MIDI_SYNTH_BUILTIN);
            } else {
                remove_slot(index);
                if (removed) *removed = true;
            }
            return MIDI_SYNTH_OK;
        case MIDI_SYNTH_OP_REMOVE:
            if ((flags & MIDI_SYNTH_STORED) && !delete_file(s->id)) return MIDI_SYNTH_ERR_STORAGE;
            if (b >= 0) {
                set_slot(index, &SYNTHS[b], NULL, MIDI_SYNTH_BUILTIN); // a built-in: back to the original
            } else {
                remove_slot(index);
                if (removed) *removed = true;
            }
            return MIDI_SYNTH_OK;
        default:
            return MIDI_SYNTH_ERR_INDEX;
    }
}
