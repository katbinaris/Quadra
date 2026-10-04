#include "profile_json.h"
#include "app_profiles.h"
#include "cJSON.h"
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BIG_ALLOC(n) heap_caps_malloc_prefer((n), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)
#else
#define BIG_ALLOC(n) malloc(n) // host test build (tools/profile_json_test)
#endif

#define ICON48_BYTES (48 * 48 * 2)
#define ICON24_BYTES (24 * 24 * 2)

// Length caps (characters) -- what the screens have room for, with some slack.
#define MAX_NAME 15
#define MAX_LEGEND 7
#define MAX_LABEL 23
#define MAX_TAB 6
#define MAX_PHRASE 60
#define MAX_RINGS 8     // app_mode.c remembers one entry per ring
#define MAX_CMDS 32
#define MAX_FRAMES 16
#define MAX_ELEMENTS 48
#define MAX_MACRO_TEXT 120
#define MAX_WAIT_MS 10000

// --- names for the enums (index = value) ---
static const char *const KIND[] = {"none", "drag", "wheel", "keys", "tap", "commands", "media"};
static const char *const FEEL[] = {"saw", "sine", "viscose"};
static const char *const VISUAL[] = {"label", "shape"};
static const char *const SHAPE[] = {"cube", "pyramid", "octa"};
static const char *const STYLE[] = {"face", "grips", "thick"};
static const char *const FX[] = {"none", "zoom", "orbit", "pan", "flash"};
static const char *const SLOT[] = {"knob", "f1", "f2", "f3", "f4"};
static const char *const CMD_KIND[] = {"keys", "actions", "macro"};
static const char *const PVISUAL[] = {"none", "fillet", "extrude", "offset", "hollow", "move", "rotate", "scale", "chamfer", "slide"};
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
_Static_assert(N(KIND) == APP_ACT_MEDIA + 1, "KIND names");
_Static_assert(N(FEEL) == HAPTIC_TYPE_COUNT, "FEEL names");
_Static_assert(N(SLOT) == APP_SLOT_COUNT, "SLOT names");
_Static_assert(N(PVISUAL) == APP_PV_SLIDE + 1, "PVISUAL names");
_Static_assert(N(CMD_KIND) == APP_CMD_MACRO + 1, "CMD_KIND names");
#define APP_EL_LAST APP_EL_ROLLBACK
#define APP_C_LAST APP_C_AMBER

// --- base64 ---

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const uint8_t *in, size_t n) {
    char *out = malloc(4 * ((n + 2) / 3) + 1), *o = out;
    if (out == NULL) return NULL;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        *o++ = B64[v >> 18 & 63];
        *o++ = B64[v >> 12 & 63];
        *o++ = i + 1 < n ? B64[v >> 6 & 63] : '=';
        *o++ = i + 2 < n ? B64[v & 63] : '=';
    }
    *o = '\0';
    return out;
}

static int b64_val(char c) {
    const char *p = c ? strchr(B64, c) : NULL;
    return p ? (int)(p - B64) : -1;
}

// Decodes exactly `n` bytes into `out`; false if the text is anything else.
static bool b64_decode(const char *s, uint8_t *out, size_t n) {
    size_t len = strlen(s);
    if (len != 4 * ((n + 2) / 3)) return false;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 4) {
        int v[4];
        for (int k = 0; k < 4; k++) v[k] = s[i + k] == '=' ? 0 : b64_val(s[i + k]);
        if (v[0] < 0 || v[1] < 0 || v[2] < 0 || v[3] < 0) return false;
        uint32_t w = (uint32_t)v[0] << 18 | v[1] << 12 | v[2] << 6 | v[3];
        if (o < n) out[o++] = w >> 16;
        if (o < n) out[o++] = (w >> 8) & 0xFF;
        if (o < n) out[o++] = w & 0xFF;
    }
    return true;
}

// ======================================================================================
// Writing
// ======================================================================================

// A float as the shortest decimal that reads back to the same float (0.05f -> 0.05, not
// 0.0500000007).
static double tidy(float f) {
    char buf[24];
    for (int prec = 6; prec <= 9; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, (double)f);
        if (strtof(buf, NULL) == f) break;
    }
    return strtod(buf, NULL);
}

static void w_int(cJSON *o, const char *k, long v) {
    if (v) cJSON_AddNumberToObject(o, k, (double)v);
}
static void w_float(cJSON *o, const char *k, float v) {
    if (v != 0.0f) cJSON_AddNumberToObject(o, k, tidy(v));
}
static void w_bool(cJSON *o, const char *k, bool v) {
    if (v) cJSON_AddTrueToObject(o, k);
}
static void w_str(cJSON *o, const char *k, const char *s) {
    if (s) cJSON_AddStringToObject(o, k, s);
}
static void w_enum(cJSON *o, const char *k, int v, const char *const *names, int n) {
    if (v > 0 && v < n) cJSON_AddStringToObject(o, k, names[v]);
}
static void w_key(cJSON *o, const char *k, app_key_t key) {
    if (key.modifier == 0 && key.keycode == 0) return;
    cJSON *a = cJSON_AddArrayToObject(o, k);
    cJSON_AddItemToArray(a, cJSON_CreateNumber(key.modifier));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(key.keycode));
}
static void w_icon(cJSON *o, const char *k, const uint8_t *px, size_t n) {
    if (px == NULL) return;
    char *s = b64_encode(px, n);
    if (s) cJSON_AddStringToObject(o, k, s);
    free(s);
}

static cJSON *w_elements(const app_el_t *el, int n) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        const app_el_t *e = &el[i];
        const int v[9] = {e->op, e->color, e->x, e->y, e->w, e->h, e->arg, e->d, e->flags};
        cJSON_AddItemToArray(a, cJSON_CreateIntArray(v, 9));
    }
    return a;
}

static cJSON *w_scene(const app_scene_t *s) {
    cJSON *o = cJSON_CreateObject();
    if (s->n_base) cJSON_AddItemToObject(o, "base", w_elements(s->base, s->n_base));
    cJSON *frames = cJSON_AddArrayToObject(o, "frames");
    for (int i = 0; i < s->n_frames; i++) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddNumberToObject(f, "ms", s->frames[i].ms);
        cJSON_AddItemToObject(f, "el", w_elements(s->frames[i].el, s->frames[i].n));
        cJSON_AddItemToArray(frames, f);
    }
    return o;
}

static cJSON *w_param(const app_param_t *p) {
    cJSON *o = cJSON_CreateObject();
    w_str(o, "label", p->label);
    w_str(o, "label_neg", p->label_neg);
    cJSON *steps = cJSON_AddArrayToObject(o, "steps");
    for (int i = 0; i < 3; i++) cJSON_AddItemToArray(steps, cJSON_CreateNumber(tidy(p->steps[i])));
    w_float(o, "free_step", p->free_step);
    w_float(o, "px_per_step", p->px_per_step);
    w_float(o, "start", p->start);
    w_float(o, "min", p->min);
    w_float(o, "max", p->max);
    w_int(o, "decimals", p->decimals);
    w_bool(o, "deg", p->flags & APP_PARAM_DEG);
    w_bool(o, "axes", p->flags & APP_PARAM_AXES);
    w_bool(o, "planes", p->flags & APP_PARAM_PLANES);
    w_bool(o, "uniform", p->flags & APP_PARAM_UNIFORM);
    w_enum(o, "visual", p->visual, PVISUAL, N(PVISUAL));
    w_int(o, "modes", p->modes);
    w_key(o, "enter", p->enter);
    w_int(o, "axis_default", p->axis_default);
    return o;
}

// A macro reference (1-based) by its name.
static void w_macro(cJSON *o, const char *k, const app_profile_t *p, uint8_t ref) {
    if (ref && ref <= p->macro_count) cJSON_AddStringToObject(o, k, p->macros[ref - 1].name);
}

static cJSON *w_macros(const app_profile_t *p) {
    cJSON *a = cJSON_CreateArray();
    for (int m = 0; m < p->macro_count; m++) {
        const app_macro_t *mac = &p->macros[m];
        cJSON *mo = cJSON_CreateObject();
        w_str(mo, "name", mac->name);
        cJSON *steps = cJSON_AddArrayToObject(mo, "steps");
        for (int i = 0; i < mac->count; i++) {
            const app_mstep_t *st = &mac->steps[i];
            cJSON *so = cJSON_CreateObject();
            if (st->kind == APP_MSTEP_KEY) {
                const int v[2] = {st->key.modifier, st->key.keycode};
                cJSON_AddItemToObject(so, "key", cJSON_CreateIntArray(v, 2));
            } else if (st->kind == APP_MSTEP_TEXT) {
                cJSON_AddStringToObject(so, "text", st->text ? st->text : "");
            } else {
                cJSON_AddNumberToObject(so, "wait", st->ms);
            }
            cJSON_AddItemToArray(steps, so);
        }
        cJSON_AddItemToArray(a, mo);
    }
    return a;
}

static cJSON *w_action(const app_profile_t *p, const app_action_t *a) {
    cJSON *o = cJSON_CreateObject();
    w_enum(o, "kind", a->kind, KIND, N(KIND));
    w_str(o, "label", a->label);
    w_int(o, "buttons", a->buttons);
    w_int(o, "modifier", a->modifier);
    w_bool(o, "axis_y", a->axis_y);
    w_float(o, "px_per_rad", a->px_per_rad);
    w_int(o, "sign", a->sign);
    w_key(o, "cw", a->cw);
    w_key(o, "ccw", a->ccw);
    w_key(o, "tap", a->tap);
    w_macro(o, "macro", p, a->macro);
    w_macro(o, "tap_macro", p, a->tap_macro);
    w_enum(o, "feel", a->feel, FEEL, N(FEEL));
    w_int(o, "detents", a->detents);
    w_enum(o, "fx", a->fx, FX, N(FX));
    return o;
}

char *profile_json_write(const app_profile_t *p, size_t *len) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "format", PROFILE_JSON_FORMAT);
    w_str(o, "id", p->id);
    w_str(o, "name", p->name);
    cJSON *legend = cJSON_AddArrayToObject(o, "legend");
    for (int i = 0; i < 4; i++) cJSON_AddItemToArray(legend, cJSON_CreateString(p->legend[i] ? p->legend[i] : ""));
    w_icon(o, "icon48", p->icon48, ICON48_BYTES);
    w_icon(o, "icon24", p->icon24, ICON24_BYTES);
    w_enum(o, "visual", p->visual, VISUAL, N(VISUAL));
    w_enum(o, "shape", p->shape, SHAPE, N(SHAPE));
    w_enum(o, "shape_style", p->shape_style, STYLE, N(STYLE));
    w_bool(o, "shape_stepped", p->shape_stepped);
    if (p->accents[0] | p->accents[1] | p->accents[2]) {
        cJSON *h = cJSON_AddArrayToObject(o, "accents");
        for (int i = 0; i < 3; i++) cJSON_AddItemToArray(h, cJSON_CreateNumber(p->accents[i]));
    }
    if (p->macro_count) cJSON_AddItemToObject(o, "macros", w_macros(p));
    cJSON *slots = cJSON_AddObjectToObject(o, "slots");
    for (int i = 0; i < APP_SLOT_COUNT; i++) {
        const app_action_t *a = &p->slot[i];
        if (a->kind != APP_ACT_NONE || a->tap.keycode || a->tap_macro) cJSON_AddItemToObject(slots, SLOT[i], w_action(p, a));
    }
    if (p->ring_count) {
        cJSON *rings = cJSON_AddArrayToObject(o, "rings");
        for (int r = 0; r < p->ring_count; r++) {
            const app_ring_t *ring = &p->rings[r];
            cJSON *ro = cJSON_CreateObject();
            w_str(ro, "name", ring->name);
            w_str(ro, "tab", ring->tab);
            cJSON_AddStringToObject(ro, "slot", SLOT[ring->slot < APP_SLOT_COUNT ? ring->slot : 0]);
            cJSON *cmds = cJSON_AddArrayToObject(ro, "cmds");
            for (int c = 0; c < ring->count; c++) {
                const app_cmd_t *cmd = &ring->cmds[c];
                cJSON *co = cJSON_CreateObject();
                w_str(co, "name", cmd->name);
                w_enum(co, "kind", cmd->kind, CMD_KIND, N(CMD_KIND));
                w_key(co, "key", cmd->key);
                w_str(co, "phrase", cmd->phrase);
                w_macro(co, "macro", p, cmd->macro);
                if (cmd->scene) cJSON_AddItemToObject(co, "scene", w_scene(cmd->scene));
                if (cmd->param) cJSON_AddItemToObject(co, "param", w_param(cmd->param));
                cJSON_AddItemToArray(cmds, co);
            }
            cJSON_AddItemToArray(rings, ro);
        }
    }
    if (p->search.open.keycode) {
        cJSON *s = cJSON_AddObjectToObject(o, "search");
        w_key(s, "open", p->search.open);
        w_int(s, "open_wait", p->search.open_wait);
        w_int(s, "result_wait", p->search.result_wait);
    }
    const app_param_keys_t *k = &p->param_keys;
    cJSON *pk = cJSON_CreateObject();
    w_key(pk, "numeric", k->numeric);
    w_key(pk, "confirm", k->confirm);
    w_key(pk, "cancel", k->cancel);
    if (k->axis[0].keycode | k->axis[1].keycode | k->axis[2].keycode) {
        cJSON *ax = cJSON_AddArrayToObject(pk, "axis");
        for (int i = 0; i < 3; i++) {
            const int v[2] = {k->axis[i].modifier, k->axis[i].keycode};
            cJSON_AddItemToArray(ax, cJSON_CreateIntArray(v, 2));
        }
    }
    w_key(pk, "uniform", k->uniform);
    w_bool(pk, "field", k->field);
    if (k->step_mod[0] | k->step_mod[1] | k->step_mod[2]) {
        const int v[3] = {k->step_mod[0], k->step_mod[1], k->step_mod[2]};
        cJSON_AddItemToObject(pk, "step_mod", cJSON_CreateIntArray(v, 3));
    }
    w_int(pk, "scroll_sign", k->scroll_sign);
    w_key(pk, "select_all", k->select_all);
    if (pk->child) cJSON_AddItemToObject(o, "param_keys", pk);
    else cJSON_Delete(pk);

    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (text && len) *len = strlen(text);
    return text;
}

// ======================================================================================
// Reading
// ======================================================================================

// Everything a parsed profile points at lives in chunks owned by the profile: freed in one go.
// The struct itself stays in internal RAM (the control loop reads its slots every tick); the
// rest -- strings, rings, scenes, icons -- goes to PSRAM.
typedef struct chunk {
    struct chunk *next;
    size_t used, cap;
    _Alignas(8) uint8_t data[];
} chunk_t;

typedef struct {
    app_profile_t p; // first: an app_profile_t * is an owned_t *
    chunk_t *chunks;
} owned_t;

typedef struct {
    owned_t *own;
    char *err;
    size_t err_len;
    bool failed;
} rd_t;

static void fail(rd_t *r, const char *fmt, ...) {
    if (r->failed) return; // keep the first reason
    r->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->err, r->err_len, fmt, ap);
    va_end(ap);
}

static void *alloc(rd_t *r, size_t n) {
    if (n == 0) n = 1;
    n = (n + 7) & ~(size_t)7;
    chunk_t *c = r->own->chunks;
    if (c == NULL || c->cap - c->used < n) {
        size_t cap = n > 4096 ? n : 4096;
        c = BIG_ALLOC(sizeof(chunk_t) + cap);
        if (c == NULL) {
            fail(r, "out of memory");
            return NULL;
        }
        c->used = 0;
        c->cap = cap;
        c->next = r->own->chunks;
        r->own->chunks = c;
    }
    void *p = c->data + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

static const cJSON *get(const cJSON *o, const char *k) {
    return cJSON_GetObjectItemCaseSensitive(o, k);
}

static long r_int(rd_t *r, const cJSON *o, const char *k, long lo, long hi) {
    const cJSON *v = get(o, k);
    if (v == NULL) return 0;
    if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble) || v->valuedouble < lo || v->valuedouble > hi) {
        fail(r, "%s: a whole number %ld..%ld", k, lo, hi);
        return 0;
    }
    return (long)v->valuedouble;
}

static float r_float(rd_t *r, const cJSON *o, const char *k) {
    const cJSON *v = get(o, k);
    if (v == NULL) return 0.0f;
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) {
        fail(r, "%s: a number", k);
        return 0.0f;
    }
    return (float)v->valuedouble;
}

static bool r_bool(rd_t *r, const cJSON *o, const char *k) {
    const cJSON *v = get(o, k);
    if (v == NULL) return false;
    if (!cJSON_IsBool(v)) fail(r, "%s: true or false", k);
    return cJSON_IsTrue(v);
}

// A copy in the profile's memory; NULL when missing (or `required` -> an error).
static const char *r_str(rd_t *r, const cJSON *o, const char *k, size_t max, bool required) {
    const cJSON *v = get(o, k);
    if (v == NULL) {
        if (required) fail(r, "%s: missing", k);
        return NULL;
    }
    if (!cJSON_IsString(v)) {
        fail(r, "%s: text", k);
        return NULL;
    }
    size_t n = strlen(v->valuestring);
    if (n > max || (required && n == 0)) {
        fail(r, "%s: 1..%u characters", k, (unsigned)max);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)v->valuestring[i] < 0x20 || (unsigned char)v->valuestring[i] > 0x7E) {
            fail(r, "%s: plain ASCII only", k);
            return NULL;
        }
    }
    char *s = alloc(r, n + 1);
    if (s) memcpy(s, v->valuestring, n + 1);
    return s;
}

static int r_enum(rd_t *r, const cJSON *o, const char *k, const char *const *names, int n) {
    const cJSON *v = get(o, k);
    if (v == NULL) return 0;
    if (cJSON_IsString(v)) {
        for (int i = 0; i < n; i++) {
            if (strcmp(v->valuestring, names[i]) == 0) return i;
        }
    }
    fail(r, "%s: unknown value", k);
    return 0;
}

static app_key_t key_from(rd_t *r, const cJSON *v, const char *k) {
    app_key_t key = {0, 0};
    if (v == NULL) return key;
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) != 2) {
        fail(r, "%s: [modifier, keycode]", k);
        return key;
    }
    const cJSON *m = cJSON_GetArrayItem(v, 0), *c = cJSON_GetArrayItem(v, 1);
    if (!cJSON_IsNumber(m) || !cJSON_IsNumber(c) || m->valueint < 0 || m->valueint > 255 || c->valueint < 0 || c->valueint > 255) {
        fail(r, "%s: [modifier, keycode] 0..255", k);
        return key;
    }
    key.modifier = (uint8_t)m->valueint;
    key.keycode = (uint8_t)c->valueint;
    return key;
}

static app_key_t r_key(rd_t *r, const cJSON *o, const char *k) {
    return key_from(r, get(o, k), k);
}

static const uint8_t *r_icon(rd_t *r, const cJSON *o, const char *k, size_t n) {
    const cJSON *v = get(o, k);
    if (v == NULL) return NULL;
    uint8_t *px = alloc(r, n);
    if (px == NULL) return NULL;
    if (!cJSON_IsString(v) || !b64_decode(v->valuestring, px, n)) fail(r, "%s: base64 of %u bytes", k, (unsigned)n);
    return px;
}

static int r_array(rd_t *r, const cJSON *v, const char *k, int max) {
    if (v == NULL) return 0;
    int n = cJSON_IsArray(v) ? cJSON_GetArraySize(v) : -1;
    if (n < 0 || n > max) {
        fail(r, "%s: a list of up to %d", k, max);
        return 0;
    }
    return n;
}

static const app_el_t *r_elements(rd_t *r, const cJSON *v, const char *k, uint8_t *count) {
    int n = r_array(r, v, k, MAX_ELEMENTS);
    *count = 0;
    if (n == 0) return NULL;
    app_el_t *els = alloc(r, n * sizeof(app_el_t));
    if (els == NULL) return NULL;
    static const int16_t LO[9] = {0, 0, -128, -128, 0, 0, 0, 0, 0};
    static const int16_t HI[9] = {APP_EL_LAST, APP_C_LAST, 127, 127, 255, 255, 255, 255, 255};
    for (int i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(v, i);
        int f[9];
        if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) != 9) {
            fail(r, "%s: elements are 9 numbers", k);
            return NULL;
        }
        for (int j = 0; j < 9; j++) {
            const cJSON *x = cJSON_GetArrayItem(e, j);
            if (!cJSON_IsNumber(x) || x->valueint < LO[j] || x->valueint > HI[j]) {
                fail(r, "%s: element %d, field %d out of range", k, i, j);
                return NULL;
            }
            f[j] = x->valueint;
        }
        els[i] = (app_el_t){(uint8_t)f[0], (uint8_t)f[1], (int8_t)f[2], (int8_t)f[3], (uint8_t)f[4],
                            (uint8_t)f[5], (uint8_t)f[6], (uint8_t)f[7], (uint8_t)f[8]};
    }
    *count = (uint8_t)n;
    return els;
}

static const app_scene_t *r_scene(rd_t *r, const cJSON *o) {
    if (o == NULL) return NULL;
    app_scene_t *s = alloc(r, sizeof(*s));
    if (s == NULL) return NULL;
    s->base = r_elements(r, get(o, "base"), "base", &s->n_base);
    const cJSON *frames = get(o, "frames");
    int n = r_array(r, frames, "frames", MAX_FRAMES);
    if (n) {
        app_keyframe_t *f = alloc(r, n * sizeof(*f));
        if (f == NULL) return NULL;
        for (int i = 0; i < n; i++) {
            const cJSON *fo = cJSON_GetArrayItem(frames, i);
            f[i].ms = (uint16_t)r_int(r, fo, "ms", 0, 60000);
            f[i].el = r_elements(r, get(fo, "el"), "el", &f[i].n);
        }
        s->frames = f;
        s->n_frames = (uint8_t)n;
    }
    return s;
}

static const app_param_t *r_param(rd_t *r, const cJSON *o) {
    if (o == NULL) return NULL;
    app_param_t *p = alloc(r, sizeof(*p));
    if (p == NULL) return NULL;
    p->label = r_str(r, o, "label", MAX_LABEL, true);
    p->label_neg = r_str(r, o, "label_neg", MAX_LABEL, false);
    const cJSON *steps = get(o, "steps");
    if (steps && (!cJSON_IsArray(steps) || cJSON_GetArraySize(steps) != 3)) fail(r, "steps: 3 numbers");
    for (int i = 0; steps && i < 3 && !r->failed; i++) {
        const cJSON *x = cJSON_GetArrayItem(steps, i);
        if (!cJSON_IsNumber(x)) fail(r, "steps: 3 numbers");
        else p->steps[i] = (float)x->valuedouble;
    }
    p->free_step = r_float(r, o, "free_step");
    p->px_per_step = r_float(r, o, "px_per_step");
    p->start = r_float(r, o, "start");
    p->min = r_float(r, o, "min");
    p->max = r_float(r, o, "max");
    if (p->min > p->max) fail(r, "param: min above max");
    p->decimals = (uint8_t)r_int(r, o, "decimals", 0, 4);
    p->flags = (r_bool(r, o, "deg") ? APP_PARAM_DEG : 0) | (r_bool(r, o, "axes") ? APP_PARAM_AXES : 0)
             | (r_bool(r, o, "planes") ? APP_PARAM_PLANES : 0) | (r_bool(r, o, "uniform") ? APP_PARAM_UNIFORM : 0);
    p->visual = (uint8_t)r_enum(r, o, "visual", PVISUAL, N(PVISUAL));
    p->modes = (uint8_t)r_int(r, o, "modes", 0, 15);
    p->enter = r_key(r, o, "enter");
    p->axis_default = (uint8_t)r_int(r, o, "axis_default", 0, APP_AXIS_UNIFORM);
    return p;
}

// A macro reference: its name -> 1-based index (macros are read first).
static uint8_t r_macro(rd_t *r, const cJSON *o, const char *k, const app_profile_t *p) {
    const cJSON *v = get(o, k);
    if (v == NULL) return 0;
    if (cJSON_IsString(v)) {
        for (int m = 0; m < p->macro_count; m++) {
            if (strcmp(p->macros[m].name, v->valuestring) == 0) return (uint8_t)(m + 1);
        }
    }
    fail(r, "%s: no macro called that", k);
    return 0;
}

static bool printable(const char *s) {
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
    }
    return true;
}

static void r_macros(rd_t *r, const cJSON *v, app_profile_t *p) {
    int n = r_array(r, v, "macros", APP_MACROS_MAX);
    if (n == 0) return;
    app_macro_t *ms = alloc(r, n * sizeof(*ms));
    for (int m = 0; ms && m < n && !r->failed; m++) {
        const cJSON *mo = cJSON_GetArrayItem(v, m);
        ms[m].name = r_str(r, mo, "name", MAX_NAME, true);
        for (int k = 0; k < m && ms[m].name; k++) {
            if (strcmp(ms[k].name, ms[m].name) == 0) fail(r, "macros: two called %s", ms[m].name);
        }
        const cJSON *steps = get(mo, "steps");
        int ns = r_array(r, steps, "steps", APP_MACRO_STEPS_MAX);
        app_mstep_t *st = ns ? alloc(r, ns * sizeof(*st)) : NULL;
        for (int i = 0; st && i < ns && !r->failed; i++) {
            const cJSON *so = cJSON_GetArrayItem(steps, i);
            const cJSON *text = get(so, "text"), *wait = get(so, "wait");
            if (get(so, "key")) {
                st[i].kind = APP_MSTEP_KEY;
                st[i].key = r_key(r, so, "key");
            } else if (text) {
                st[i].kind = APP_MSTEP_TEXT;
                if (!cJSON_IsString(text) || strlen(text->valuestring) > MAX_MACRO_TEXT || !printable(text->valuestring)) {
                    fail(r, "text: up to %d plain ASCII characters", MAX_MACRO_TEXT);
                } else {
                    st[i].text = r_str(r, so, "text", MAX_MACRO_TEXT, false);
                }
            } else if (wait) {
                st[i].kind = APP_MSTEP_WAIT;
                st[i].ms = (uint16_t)r_int(r, so, "wait", 0, MAX_WAIT_MS);
            } else {
                fail(r, "steps: each is a key, a text or a wait");
            }
        }
        ms[m].steps = st;
        ms[m].count = (uint8_t)ns;
    }
    p->macros = ms;
    p->macro_count = (uint8_t)n;
}

static void r_action(rd_t *r, const cJSON *o, app_action_t *a, const app_profile_t *p) {
    if (o == NULL) return;
    a->kind = (app_action_kind_t)r_enum(r, o, "kind", KIND, N(KIND));
    a->label = r_str(r, o, "label", MAX_LABEL, false);
    a->buttons = (uint8_t)r_int(r, o, "buttons", 0, 31);
    a->modifier = (uint8_t)r_int(r, o, "modifier", 0, 255);
    a->axis_y = r_bool(r, o, "axis_y");
    a->px_per_rad = r_float(r, o, "px_per_rad");
    if (a->px_per_rad < 0.0f || a->px_per_rad > 2000.0f) fail(r, "px_per_rad: 0..2000");
    a->sign = (int8_t)r_int(r, o, "sign", -1, 1);
    a->cw = r_key(r, o, "cw");
    a->ccw = r_key(r, o, "ccw");
    a->tap = r_key(r, o, "tap");
    a->macro = r_macro(r, o, "macro", p);
    a->tap_macro = r_macro(r, o, "tap_macro", p);
    a->feel = (haptic_type_t)r_enum(r, o, "feel", FEEL, N(FEEL));
    a->detents = (uint16_t)r_int(r, o, "detents", 0, HAPTIC_NUM_DETENTS_MAX);
    a->fx = (app_fx_t)r_enum(r, o, "fx", FX, N(FX));
}

bool profile_json_id_ok(const char *id) {
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n > PROFILE_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static void parse(rd_t *r, const cJSON *o) {
    app_profile_t *p = &r->own->p;
    if (!cJSON_IsObject(o)) {
        fail(r, "not a JSON object");
        return;
    }
    long format = r_int(r, o, "format", 0, 1000);
    if (format != PROFILE_JSON_FORMAT) fail(r, "format %ld: this firmware reads %d", format, PROFILE_JSON_FORMAT);
    p->version = APP_PROFILE_VERSION;
    p->id = r_str(r, o, "id", PROFILE_ID_MAX, true);
    if (p->id && !profile_json_id_ok(p->id)) fail(r, "id: a-z, 0-9, _ and - only");
    p->name = r_str(r, o, "name", MAX_NAME, true);

    const cJSON *legend = get(o, "legend");
    if (!cJSON_IsArray(legend) || cJSON_GetArraySize(legend) != 4) fail(r, "legend: 4 texts");
    for (int i = 0; i < 4 && !r->failed; i++) {
        const cJSON *s = cJSON_GetArrayItem(legend, i);
        if (!cJSON_IsString(s) || strlen(s->valuestring) > MAX_LEGEND) {
            fail(r, "legend: up to %d characters each", MAX_LEGEND);
            break;
        }
        char *c = alloc(r, strlen(s->valuestring) + 1);
        if (c) strcpy(c, s->valuestring);
        p->legend[i] = c;
    }
    p->icon48 = r_icon(r, o, "icon48", ICON48_BYTES);
    p->icon24 = r_icon(r, o, "icon24", ICON24_BYTES);
    p->visual = (app_visual_t)r_enum(r, o, "visual", VISUAL, N(VISUAL));
    p->shape = (app_shape_t)r_enum(r, o, "shape", SHAPE, N(SHAPE));
    p->shape_style = (app_shape_style_t)r_enum(r, o, "shape_style", STYLE, N(STYLE));
    p->shape_stepped = r_bool(r, o, "shape_stepped");
    const cJSON *heat = get(o, "accents");
    if (heat == NULL) heat = get(o, "plasma"); // its name before 2026-10-04 (profiles stored then)
    if (heat) {
        if (!cJSON_IsArray(heat) || cJSON_GetArraySize(heat) != 3) fail(r, "accents: 3 colours");
        for (int i = 0; i < 3 && !r->failed; i++) {
            const cJSON *c = cJSON_GetArrayItem(heat, i);
            if (!cJSON_IsNumber(c) || c->valuedouble < 0 || c->valuedouble > 0xFFFFFF) fail(r, "accents: 0xRRGGBB numbers");
            else p->accents[i] = (uint32_t)c->valuedouble;
        }
    }

    r_macros(r, get(o, "macros"), p); // before anything refers to them
    const cJSON *slots = get(o, "slots");
    if (slots && !cJSON_IsObject(slots)) fail(r, "slots: an object");
    for (int i = 0; slots && i < APP_SLOT_COUNT; i++) r_action(r, get(slots, SLOT[i]), &p->slot[i], p);

    const cJSON *rings = get(o, "rings");
    int nr = r_array(r, rings, "rings", MAX_RINGS);
    if (nr) {
        app_ring_t *rs = alloc(r, nr * sizeof(*rs));
        for (int i = 0; rs && i < nr && !r->failed; i++) {
            const cJSON *ro = cJSON_GetArrayItem(rings, i);
            rs[i].name = r_str(r, ro, "name", MAX_LABEL, true);
            rs[i].tab = r_str(r, ro, "tab", MAX_TAB, true);
            rs[i].slot = (uint8_t)r_enum(r, ro, "slot", SLOT, N(SLOT));
            const cJSON *cmds = get(ro, "cmds");
            int nc = r_array(r, cmds, "cmds", MAX_CMDS);
            if (nc == 0) fail(r, "rings: each needs at least one command");
            app_cmd_t *cs = nc ? alloc(r, nc * sizeof(*cs)) : NULL;
            for (int c = 0; cs && c < nc && !r->failed; c++) {
                const cJSON *co = cJSON_GetArrayItem(cmds, c);
                cs[c].name = r_str(r, co, "name", MAX_LABEL, true);
                cs[c].kind = (app_cmd_kind_t)r_enum(r, co, "kind", CMD_KIND, N(CMD_KIND));
                cs[c].key = r_key(r, co, "key");
                cs[c].phrase = r_str(r, co, "phrase", MAX_PHRASE, false);
                cs[c].macro = r_macro(r, co, "macro", p);
                if (cs[c].kind == APP_CMD_MACRO && cs[c].macro == 0) fail(r, "%s: pick a macro", cs[c].name ? cs[c].name : "command");
                cs[c].scene = r_scene(r, get(co, "scene"));
                cs[c].param = r_param(r, get(co, "param"));
            }
            rs[i].cmds = cs;
            rs[i].count = (uint8_t)nc;
        }
        p->rings = rs;
        p->ring_count = (uint8_t)nr;
    }

    const cJSON *search = get(o, "search");
    if (search) {
        p->search.open = r_key(r, search, "open");
        p->search.open_wait = (uint8_t)r_int(r, search, "open_wait", 0, 255);
        p->search.result_wait = (uint8_t)r_int(r, search, "result_wait", 0, 255);
    }
    const cJSON *pk = get(o, "param_keys");
    if (pk) {
        app_param_keys_t *k = &p->param_keys;
        k->numeric = r_key(r, pk, "numeric");
        k->confirm = r_key(r, pk, "confirm");
        k->cancel = r_key(r, pk, "cancel");
        const cJSON *ax = get(pk, "axis");
        if (ax && (!cJSON_IsArray(ax) || cJSON_GetArraySize(ax) != 3)) fail(r, "axis: 3 keys");
        for (int i = 0; ax && i < 3 && !r->failed; i++) k->axis[i] = key_from(r, cJSON_GetArrayItem(ax, i), "axis");
        k->uniform = r_key(r, pk, "uniform");
        k->field = r_bool(r, pk, "field");
        const cJSON *sm = get(pk, "step_mod");
        if (sm && (!cJSON_IsArray(sm) || cJSON_GetArraySize(sm) != 3)) fail(r, "step_mod: 3 modifiers");
        for (int i = 0; sm && i < 3 && !r->failed; i++) {
            const cJSON *x = cJSON_GetArrayItem(sm, i);
            if (!cJSON_IsNumber(x) || x->valueint < 0 || x->valueint > 255) fail(r, "step_mod: 0..255");
            else k->step_mod[i] = (uint8_t)x->valueint;
        }
        k->scroll_sign = (int8_t)r_int(r, pk, "scroll_sign", -1, 1);
        k->select_all = r_key(r, pk, "select_all");
    }
}

app_profile_t *profile_json_read(const char *text, size_t len, char *err, size_t err_len) {
    char dummy[1];
    if (err == NULL || err_len == 0) {
        err = dummy;
        err_len = sizeof(dummy);
    }
    err[0] = '\0';
    owned_t *own = calloc(1, sizeof(*own));
    if (own == NULL) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }
    rd_t r = {own, err, err_len, false};
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (root == NULL) {
        fail(&r, "not valid JSON (near byte %d)", (int)(cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() - text : 0));
    } else {
        parse(&r, root);
        cJSON_Delete(root);
    }
    if (!r.failed && !app_profiles_valid(&own->p)) fail(&r, "fails the profile checks (wheel, search key, params)");
    if (r.failed) {
        profile_json_free(&own->p);
        return NULL;
    }
    return &own->p;
}

void profile_json_free(app_profile_t *p) {
    if (p == NULL) return;
    owned_t *own = (owned_t *)p;
    for (chunk_t *c = own->chunks, *next; c; c = next) {
        next = c->next;
        free(c);
    }
    free(own);
}
