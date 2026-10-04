// Host test for app_profiles/profile_json.c: every built-in profile goes to JSON and back and
// must come out the same -- same text on a second write, same struct field by field. Also
// checks that broken input is refused with a reason. Run: tools/profile_json_test/run.sh
#include "app_profiles/app_profiles.h"
#include "app_profiles/profile_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const app_profile_t app_profile_music, app_profile_agents, app_profile_clock, app_profile_plasticity, app_profile_figma, app_profile_onshape, app_profile_blender,
    app_profile_autocad;

// Stubs for the store (the registry's boot load isn't under test here).
bool profile_store_init(void) { return false; }
void profile_store_list(void (*fn)(const char *, void *), void *ctx) { (void)fn; (void)ctx; }
char *profile_store_read(const char *id, size_t *len) { (void)id; (void)len; return NULL; }
bool profile_store_write(const char *id, const char *t, size_t l) { (void)id; (void)t; (void)l; return true; }
bool profile_store_delete(const char *id) { (void)id; return true; }

static int s_fail = 0;
#define CHECK(c, ...)                        \
    do {                                     \
        if (!(c)) {                          \
            printf("  FAIL: " __VA_ARGS__);  \
            printf("\n");                    \
            s_fail++;                        \
        }                                    \
    } while (0)

static bool str_eq(const char *a, const char *b) {
    if (a == NULL || b == NULL) return a == b;
    return strcmp(a, b) == 0;
}
static bool key_eq(app_key_t a, app_key_t b) { return a.modifier == b.modifier && a.keycode == b.keycode; }
static bool els_eq(const app_el_t *a, const app_el_t *b, int n) { return n == 0 || memcmp(a, b, n * sizeof(app_el_t)) == 0; }

static void compare(const app_profile_t *a, const app_profile_t *b) {
    CHECK(str_eq(a->id, b->id) && str_eq(a->name, b->name), "id / name");
    for (int i = 0; i < 4; i++) CHECK(str_eq(a->legend[i], b->legend[i]), "legend %d", i);
    CHECK((a->icon48 == NULL) == (b->icon48 == NULL) && (!a->icon48 || memcmp(a->icon48, b->icon48, 4608) == 0), "icon48");
    CHECK((a->icon24 == NULL) == (b->icon24 == NULL) && (!a->icon24 || memcmp(a->icon24, b->icon24, 1152) == 0), "icon24");
    CHECK(a->visual == b->visual && a->shape == b->shape && a->shape_style == b->shape_style && a->shape_stepped == b->shape_stepped, "visual");
    CHECK(memcmp(a->accents, b->accents, sizeof(a->accents)) == 0, "accents");
    for (int i = 0; i < APP_SLOT_COUNT; i++) {
        const app_action_t *x = &a->slot[i], *y = &b->slot[i];
        CHECK(x->kind == y->kind && str_eq(x->label, y->label) && x->buttons == y->buttons && x->modifier == y->modifier
                  && x->axis_y == y->axis_y && x->px_per_rad == y->px_per_rad && x->sign == y->sign && key_eq(x->cw, y->cw)
                  && key_eq(x->ccw, y->ccw) && key_eq(x->tap, y->tap) && x->feel == y->feel && x->detents == y->detents
                  && x->fx == y->fx,
              "slot %d", i);
    }
    CHECK(a->ring_count == b->ring_count, "ring count");
    for (int r = 0; r < a->ring_count && r < b->ring_count; r++) {
        const app_ring_t *x = &a->rings[r], *y = &b->rings[r];
        CHECK(str_eq(x->name, y->name) && str_eq(x->tab, y->tab) && x->slot == y->slot && x->count == y->count, "ring %d", r);
        for (int c = 0; c < x->count && c < y->count; c++) {
            const app_cmd_t *p = &x->cmds[c], *q = &y->cmds[c];
            CHECK(str_eq(p->name, q->name) && p->kind == q->kind && key_eq(p->key, q->key) && str_eq(p->phrase, q->phrase),
                  "ring %d cmd %d", r, c);
            CHECK((p->scene == NULL) == (q->scene == NULL), "ring %d cmd %d scene", r, c);
            if (p->scene && q->scene) {
                CHECK(p->scene->n_base == q->scene->n_base && els_eq(p->scene->base, q->scene->base, p->scene->n_base)
                          && p->scene->n_frames == q->scene->n_frames,
                      "ring %d cmd %d scene base", r, c);
                for (int f = 0; f < p->scene->n_frames && f < q->scene->n_frames; f++) {
                    const app_keyframe_t *m = &p->scene->frames[f], *n = &q->scene->frames[f];
                    CHECK(m->ms == n->ms && m->n == n->n && els_eq(m->el, n->el, m->n), "ring %d cmd %d frame %d", r, c, f);
                }
            }
            CHECK((p->param == NULL) == (q->param == NULL), "ring %d cmd %d param", r, c);
            if (p->param && q->param) {
                const app_param_t *m = p->param, *n = q->param;
                CHECK(str_eq(m->label, n->label) && str_eq(m->label_neg, n->label_neg)
                          && memcmp(m->steps, n->steps, sizeof(m->steps)) == 0 && m->free_step == n->free_step
                          && m->px_per_step == n->px_per_step && m->start == n->start && m->min == n->min && m->max == n->max
                          && m->decimals == n->decimals && m->flags == n->flags && m->visual == n->visual
                          && m->modes == n->modes && key_eq(m->enter, n->enter) && m->axis_default == n->axis_default,
                      "ring %d cmd %d param fields", r, c);
            }
        }
    }
    CHECK(key_eq(a->search.open, b->search.open) && a->search.open_wait == b->search.open_wait
              && a->search.result_wait == b->search.result_wait,
          "search");
    const app_param_keys_t *k = &a->param_keys, *l = &b->param_keys;
    CHECK(key_eq(k->numeric, l->numeric) && key_eq(k->confirm, l->confirm) && key_eq(k->cancel, l->cancel)
              && key_eq(k->axis[0], l->axis[0]) && key_eq(k->axis[1], l->axis[1]) && key_eq(k->axis[2], l->axis[2])
              && key_eq(k->uniform, l->uniform) && k->field == l->field && memcmp(k->step_mod, l->step_mod, 3) == 0
              && k->scroll_sign == l->scroll_sign && key_eq(k->select_all, l->select_all),
          "param_keys");
}

static void refuse(const char *what, const char *json) {
    char err[80];
    app_profile_t *p = profile_json_read(json, strlen(json), err, sizeof(err));
    CHECK(p == NULL, "%s: accepted", what);
    if (p) profile_json_free(p);
    else printf("  refused %-16s \"%s\"\n", what, err);
}

// --parse <file>: what the firmware makes of a profile file (the companion's uploads).
static int parse_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return printf("%s: can't open\n", path), 2;
    static char buf[128 * 1024];
    size_t len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    char err[80];
    app_profile_t *p = profile_json_read(buf, len, err, sizeof(err));
    if (p == NULL) return printf("%s: REFUSED: %s\n", path, err), 1;
    printf("%s: ok, %s, %d rings, %d macros\n", path, p->name, p->ring_count, p->macro_count);
    for (int m = 0; m < p->macro_count; m++) printf("  macro %d %s: %d steps\n", m + 1, p->macros[m].name, p->macros[m].count);
    for (int i = 0; i < APP_SLOT_COUNT; i++) {
        if (p->slot[i].macro || p->slot[i].tap_macro) printf("  slot %d: macro %d, tap_macro %d\n", i, p->slot[i].macro, p->slot[i].tap_macro);
    }
    profile_json_free(p);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[1], "--parse") == 0) {
        int rc = 0;
        for (int i = 2; i < argc; i++) rc |= parse_file(argv[i]);
        return rc;
    }
    const app_profile_t *all[] = {&app_profile_music, &app_profile_agents, &app_profile_clock, &app_profile_plasticity, &app_profile_figma, &app_profile_onshape, &app_profile_blender,
                                  &app_profile_autocad};
    const char *dump = argc > 1 ? argv[1] : NULL; // a folder: each built-in's JSON goes there
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        size_t len = 0;
        char *text = profile_json_write(all[i], &len);
        char err[80];
        app_profile_t *back = profile_json_read(text, len, err, sizeof(err));
        printf("%-11s %6zu bytes  %s\n", all[i]->id, len, back ? "parsed" : err);
        CHECK(back != NULL, "%s: %s", all[i]->id, err);
        if (back) {
            compare(all[i], back);
            size_t len2 = 0;
            char *again = profile_json_write(back, &len2);
            CHECK(len == len2 && strcmp(text, again) == 0, "second write differs");
            free(again);
            profile_json_free(back);
        }
        if (dump) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s.json", dump, all[i]->id);
            FILE *f = fopen(path, "w");
            if (f) {
                fwrite(text, 1, len, f);
                fclose(f);
            }
        }
        free(text);
    }
    refuse("not json", "{\"format\":1,");
    refuse("wrong format", "{\"format\":9,\"id\":\"x\",\"name\":\"X\",\"legend\":[\"\",\"\",\"\",\"\"]}");
    refuse("bad id", "{\"format\":1,\"id\":\"Bad Id\",\"name\":\"X\",\"legend\":[\"\",\"\",\"\",\"\"]}");
    refuse("no legend", "{\"format\":1,\"id\":\"x\",\"name\":\"X\"}");
    refuse("unknown kind", "{\"format\":1,\"id\":\"x\",\"name\":\"X\",\"legend\":[\"\",\"\",\"\",\"\"],\"slots\":{\"knob\":{\"kind\":\"fly\"}}}");
    refuse("wheel, no rings", "{\"format\":1,\"id\":\"x\",\"name\":\"X\",\"legend\":[\"\",\"\",\"\",\"\"],\"slots\":{\"f3\":{\"kind\":\"commands\"}}}");
    refuse("bad element", "{\"format\":1,\"id\":\"x\",\"name\":\"X\",\"legend\":[\"\",\"\",\"\",\"\"],\"rings\":[{\"name\":\"A\",\"tab\":\"A\",\"cmds\":[{\"name\":\"C\",\"scene\":{\"frames\":[{\"ms\":1,\"el\":[[99,0,0,0,0,0,0,0,0]]}]}}]}]}");
    refuse("short icon", "{\"format\":1,\"id\":\"x\",\"name\":\"X\",\"legend\":[\"\",\"\",\"\",\"\"],\"icon48\":\"AAAA\"}");
    const char *minimal = "{\"format\":1,\"id\":\"mine\",\"name\":\"MINE\",\"legend\":[\"A\",\"B\",\"C\",\"MENU\"],"
                          "\"slots\":{\"knob\":{\"kind\":\"wheel\",\"sign\":1}}}";
    app_profile_t *p = profile_json_read(minimal, strlen(minimal), NULL, 0);
    CHECK(p && p->slot[0].kind == APP_ACT_WHEEL && p->slot[0].sign == 1 && p->icon48 == NULL, "minimal profile");
    profile_json_free(p);

    // Macros: steps of each kind, referred to by name from a TAP, a quick tap and a command;
    // written back the same.
    const char *macros =
        "{\"format\":1,\"id\":\"mac\",\"name\":\"MAC\",\"legend\":[\"A\",\"B\",\"C\",\"MENU\"],"
        "\"macros\":[{\"name\":\"HI\",\"steps\":[{\"key\":[8,4]},{\"wait\":250},{\"text\":\"Hello, {World}!\"}]},"
        "{\"name\":\"BYE\",\"steps\":[{\"text\":\"bye\"}]}],"
        "\"slots\":{\"f1\":{\"kind\":\"tap\",\"macro\":\"BYE\"},\"f2\":{\"kind\":\"wheel\",\"sign\":1,\"tap_macro\":\"HI\"},"
        "\"f3\":{\"kind\":\"commands\"}},"
        "\"rings\":[{\"name\":\"R\",\"tab\":\"R\",\"slot\":\"f1\",\"cmds\":[{\"name\":\"SAY HI\",\"kind\":\"macro\",\"macro\":\"HI\"}]}]}";
    char err[80];
    p = profile_json_read(macros, strlen(macros), err, sizeof(err));
    CHECK(p != NULL, "macros: %s", err);
    if (p) {
        CHECK(p->macro_count == 2 && p->macros[0].count == 3 && p->macros[0].steps[1].kind == APP_MSTEP_WAIT
                  && p->macros[0].steps[1].ms == 250 && strcmp(p->macros[0].steps[2].text, "Hello, {World}!") == 0,
              "macro steps");
        CHECK(p->slot[APP_SLOT_F1].macro == 2 && p->slot[APP_SLOT_F2].tap_macro == 1, "macro refs on keys");
        CHECK(p->rings[0].cmds[0].kind == APP_CMD_MACRO && p->rings[0].cmds[0].macro == 1, "macro ref on a command");
        size_t len = 0;
        char *text = profile_json_write(p, &len);
        app_profile_t *q = profile_json_read(text, len, err, sizeof(err));
        CHECK(q && q->slot[APP_SLOT_F1].macro == 2 && q->rings[0].cmds[0].macro == 1, "macros round trip");
        size_t len2 = 0;
        char *again = q ? profile_json_write(q, &len2) : NULL;
        CHECK(again && strcmp(text, again) == 0, "macros: second write differs");
        free(text);
        free(again);
        profile_json_free(q);
        profile_json_free(p);
    }
    refuse("unknown macro", "{\"format\":1,\"id\":\"x\",\"name\":\"X\",\"legend\":[\"\",\"\",\"\",\"\"],\"slots\":{\"f1\":{\"kind\":\"tap\",\"macro\":\"NOPE\"}}}");
    refuse("two same names", "{\"format\":1,\"id\":\"x\",\"name\":\"X\",\"legend\":[\"\",\"\",\"\",\"\"],\"macros\":[{\"name\":\"A\",\"steps\":[]},{\"name\":\"A\",\"steps\":[]}]}");
    refuse("non-ascii text", "{\"format\":1,\"id\":\"x\",\"name\":\"X\",\"legend\":[\"\",\"\",\"\",\"\"],\"macros\":[{\"name\":\"A\",\"steps\":[{\"text\":\"\u00e9\"}]}]}");
    refuse("long wait", "{\"format\":1,\"id\":\"x\",\"name\":\"X\",\"legend\":[\"\",\"\",\"\",\"\"],\"macros\":[{\"name\":\"A\",\"steps\":[{\"wait\":99999}]}]}");
    refuse("macro cmd, none", "{\"format\":1,\"id\":\"x\",\"name\":\"X\",\"legend\":[\"\",\"\",\"\",\"\"],\"rings\":[{\"name\":\"R\",\"tab\":\"R\",\"cmds\":[{\"name\":\"C\",\"kind\":\"macro\"}]}]}");

    printf(s_fail ? "%d FAILED\n" : "all good\n", s_fail);
    return s_fail != 0;
}
