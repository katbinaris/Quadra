// Host test for midi_synths.c: every built-in synth goes to JSON and back and must come out the
// same; an upload is live, SAVE stores it, REVERT and REMOVE go back; a stored synth comes back at
// the next boot; broken input is refused with a reason. Run: tools/midi_synth_test/run.sh
#include "midi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int64_t g_test_now_us = 0;

static int s_fail = 0;
#define CHECK(c, ...)                        \
    do {                                     \
        if (!(c)) {                          \
            printf("  FAIL: " __VA_ARGS__);  \
            printf("\n");                    \
            s_fail++;                        \
        }                                    \
    } while (0)

static bool same_synth(const midi_synth_t *a, const midi_synth_t *b) {
    if (strcmp(a->id, b->id) || strcmp(a->maker, b->maker) || strcmp(a->name, b->name) || a->channel != b->channel
        || a->prog_scheme != b->prog_scheme || a->prog_count != b->prog_count || a->n_params != b->n_params)
        return false;
    for (int i = 0; i < a->n_params; i++) {
        const midi_param_t *p = &a->params[i], *q = &b->params[i];
        if (strcmp(p->group, q->group) || strcmp(p->name, q->name) || p->cc != q->cc || p->n_opts != q->n_opts
            || p->bipolar != q->bipolar)
            return false;
        if (p->n_opts <= 1 && p->kind != q->kind) return false; // a switch's kind doesn't matter
        for (int o = 0; o < p->n_opts; o++) {
            if (p->opt_value[o] != q->opt_value[o] || strcmp(p->opt_name[o], q->opt_name[o])) return false;
        }
    }
    return true;
}

static void later(void) {
    g_test_now_us += 2000000;
    midi_synths_reap();
}

static void refused(const char *json, const char *what) {
    char why[64];
    int index = -1, n = midi_synth_count();
    midi_synth_err_t e = midi_synth_put(json, strlen(json), false, &index, why, sizeof(why));
    CHECK(e == MIDI_SYNTH_ERR_INVALID && why[0] && midi_synth_count() == n, "%s not refused", what);
    printf("  refused %-22s \"%s\"\n", what, why);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/midi_synth_test_fs";
    mkdir(dir, 0775);
    midi_synths_init();
    const int builtins = midi_synth_count();
    CHECK(builtins == 4, "4 built-ins, got %d", builtins);
    if (argc > 2) { // each built-in's JSON, for the companion's demo knob
        for (int i = 0; i < builtins; i++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%d.json", argv[2], i);
            char *json = midi_synth_json(i, NULL);
            FILE *f = fopen(path, "w");
            if (f && json) fputs(json, f);
            if (f) fclose(f);
            free(json);
        }
    }

    // Every built-in: to JSON and back, live, the same; saved, reverted, removed.
    for (int i = 0; i < builtins; i++) {
        const midi_synth_t *orig = midi_synth_get(i);
        CHECK(midi_synth_flags(i) == MIDI_SYNTH_BUILTIN, "%s: flags", orig->id);
        size_t len = 0;
        char *json = midi_synth_json(i, &len);
        CHECK(json != NULL && len == strlen(json), "%s: JSON", orig->id);
        char why[64];
        int index = -1;
        CHECK(midi_synth_put(json, len, false, &index, why, sizeof(why)) == MIDI_SYNTH_OK && index == i, "%s: put (%s)", orig->id, why);
        const midi_synth_t *live = midi_synth_get(i);
        CHECK(live != orig && same_synth(live, orig), "%s: comes back the same", orig->id);
        CHECK(midi_synth_flags(i) == (MIDI_SYNTH_BUILTIN | MIDI_SYNTH_LIVE), "%s: live", orig->id);
        size_t len2 = 0;
        char *json2 = midi_synth_json(i, &len2);
        CHECK(json2 && strcmp(json, json2) == 0, "%s: same JSON a second time", orig->id);
        free(json);
        free(json2);
        CHECK(midi_synth_op(i, MIDI_SYNTH_OP_SAVE, NULL) == MIDI_SYNTH_OK, "%s: save", orig->id);
        CHECK(midi_synth_flags(i) == (MIDI_SYNTH_BUILTIN | MIDI_SYNTH_STORED), "%s: stored", orig->id);
        CHECK(midi_synth_op(i, MIDI_SYNTH_OP_REVERT, NULL) == MIDI_SYNTH_OK && same_synth(midi_synth_get(i), orig), "%s: revert", orig->id);
        bool removed = true;
        CHECK(midi_synth_op(i, MIDI_SYNTH_OP_REMOVE, &removed) == MIDI_SYNTH_OK && !removed, "%s: remove", orig->id);
        CHECK(midi_synth_get(i) == orig && midi_synth_flags(i) == MIDI_SYNTH_BUILTIN, "%s: the built-in again", orig->id);
        later();
        printf("  %-14s %2d parameters, %5zu bytes of JSON\n", orig->id, orig->n_params, len);
    }

    // A synth of one's own: added, saved, there after a boot, then removed.
    const char *mine = "{\"id\":\"my-synth\",\"maker\":\"\",\"name\":\"MY SYNTH\",\"channel\":3,"
                       "\"programs\":{\"scheme\":\"pc\",\"count\":32},\"params\":["
                       "{\"group\":\"FILTER\",\"name\":\"CUTOFF\",\"cc\":74},"
                       "{\"group\":\"OSC\",\"name\":\"WAVE\",\"cc\":70,\"options\":[{\"name\":\"SAW\",\"value\":0},{\"name\":\"SQR\",\"value\":127}]},"
                       "{\"group\":\"\",\"name\":\"PAN\",\"sends\":\"korg10\",\"cc\":10,\"centred\":true}]}";
    char why[64];
    int index = -1;
    CHECK(midi_synth_put(mine, strlen(mine), true, &index, why, sizeof(why)) == MIDI_SYNTH_OK && index == builtins, "mine: put (%s)", why);
    CHECK(midi_synth_count() == builtins + 1 && midi_synth_flags(index) == MIDI_SYNTH_STORED, "mine: stored");
    CHECK(midi_synth_find("my-synth") == index, "mine: found by id");
    const midi_synth_t *m = midi_synth_get(index);
    CHECK(m->params[1].n_opts == 2 && m->params[1].opt_value[1] == 127 && m->params[2].kind == MIDI_P_KORG10 && m->params[2].bipolar,
          "mine: its parameters");
    midi_synths_init(); // a boot
    later();
    CHECK(midi_synth_count() == builtins + 1 && midi_synth_find("my-synth") == builtins, "mine: back after a boot");
    CHECK(midi_synth_flags(builtins) == MIDI_SYNTH_STORED, "mine: stored after a boot");
    // An edit, live, then reverted to the stored one.
    const char *edit = "{\"id\":\"my-synth\",\"maker\":\"\",\"name\":\"EDITED\",\"params\":[{\"group\":\"\",\"name\":\"X\",\"cc\":1}]}";
    CHECK(midi_synth_put(edit, strlen(edit), false, &index, why, sizeof(why)) == MIDI_SYNTH_OK && index == builtins, "mine: edit");
    CHECK(strcmp(midi_synth_get(index)->name, "EDITED") == 0 && (midi_synth_flags(index) & MIDI_SYNTH_LIVE), "mine: edit is live");
    CHECK(midi_synth_op(index, MIDI_SYNTH_OP_REVERT, NULL) == MIDI_SYNTH_OK && strcmp(midi_synth_get(index)->name, "MY SYNTH") == 0,
          "mine: revert to stored");
    bool removed = false;
    CHECK(midi_synth_op(index, MIDI_SYNTH_OP_REMOVE, &removed) == MIDI_SYNTH_OK && removed, "mine: remove");
    CHECK(midi_synth_count() == builtins && midi_synth_find("my-synth") < 0, "mine: gone");
    // A new one never saved: REVERT removes it.
    CHECK(midi_synth_put(edit, strlen(edit), false, &index, why, sizeof(why)) == MIDI_SYNTH_OK, "unsaved: put");
    CHECK(midi_synth_op(index, MIDI_SYNTH_OP_REVERT, &removed) == MIDI_SYNTH_OK && removed && midi_synth_count() == builtins,
          "unsaved: revert removes it");
    later();

    // The list's end.
    for (int i = midi_synth_count(); i < MIDI_MAX_SYNTHS; i++) {
        char j[128];
        snprintf(j, sizeof(j), "{\"id\":\"s%d\",\"maker\":\"\",\"name\":\"S\",\"params\":[{\"group\":\"\",\"name\":\"X\",\"cc\":1}]}", i);
        CHECK(midi_synth_put(j, strlen(j), false, &index, why, sizeof(why)) == MIDI_SYNTH_OK, "fill %d", i);
    }
    const char *one_more = "{\"id\":\"one-more\",\"maker\":\"\",\"name\":\"S\",\"params\":[{\"group\":\"\",\"name\":\"X\",\"cc\":1}]}";
    CHECK(midi_synth_put(one_more, strlen(one_more), false, &index, why, sizeof(why)) == MIDI_SYNTH_ERR_FULL, "full");
    while (midi_synth_count() > builtins) midi_synth_op(builtins, MIDI_SYNTH_OP_REVERT, NULL);
    later();

    // Broken input.
    refused("[1,2]", "not an object");
    refused("{\"id\":\"Bad Id\",\"maker\":\"\",\"name\":\"S\",\"params\":[{\"name\":\"X\",\"cc\":1}]}", "bad id");
    refused("{\"id\":\"a\",\"maker\":\"\",\"name\":\"\",\"params\":[{\"name\":\"X\",\"cc\":1}]}", "empty name");
    refused("{\"id\":\"a\",\"maker\":\"\",\"name\":\"S\",\"params\":[]}", "no params");
    refused("{\"id\":\"a\",\"maker\":\"\",\"name\":\"S\",\"params\":[{\"group\":\"\",\"name\":\"X\"}]}", "no cc");
    refused("{\"id\":\"a\",\"maker\":\"\",\"name\":\"S\",\"params\":[{\"group\":\"\",\"name\":\"X\",\"cc\":128}]}", "cc 128");
    refused("{\"id\":\"a\",\"maker\":\"\",\"name\":\"S\",\"params\":[{\"group\":\"\",\"name\":\"WAY TOO LONG NAME\",\"cc\":1}]}", "long name");
    refused("{\"id\":\"a\",\"maker\":\"\",\"name\":\"S\",\"params\":[{\"group\":\"\",\"name\":\"X\",\"cc\":1,\"options\":[{\"name\":\"A\",\"value\":0}]}]}",
            "one option");
    refused("{\"id\":\"a\",\"maker\":\"\",\"name\":\"S\",\"params\":[{\"group\":\"\",\"name\":\"X\",\"cc\":1,\"options\":[{\"name\":\"A\",\"value\":0},{\"name\":\"B\",\"value\":200}]}]}",
            "option value 200");
    refused("{\"id\":\"a\",\"maker\":\"\",\"name\":\"S\",\"channel\":17,\"params\":[{\"group\":\"\",\"name\":\"X\",\"cc\":1}]}", "channel 17");
    refused("{\"id\":\"a\",\"maker\":\"\",\"name\":\"S\",\"params\":[{\"group\":\"\",\"name\":\"X\",\"cc\":1,\"sends\":\"nrpn\"}]}", "sends nrpn");
    refused("{\"id\":\"a\",", "not JSON");
    CHECK(midi_synth_count() == builtins, "nothing added by broken input");

    later();
    later();
    printf(s_fail ? "%d FAILED\n" : "all good\n", s_fail);
    return s_fail ? 1 : 0;
}
