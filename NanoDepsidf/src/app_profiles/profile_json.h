#pragma once
// App profiles as JSON: the stored / uploaded form of app_profile_t (app_profile.h). The
// companion app edits this format (companion/src/profile.ts mirrors it -- change both).
//
// Field names follow the C struct; enums are lowercase names ("drag", "viscose"). Keys are
// [modifier, keycode] (TinyUSB HID values). Scene elements are arrays of the nine app_el_t
// bytes [op, color, x, y, w, h, arg, d, flags]. Icons are base64 RGB565 big-endian. A field
// that's missing reads as 0 / false / none, and a field that's 0 is left out, so a built-in
// profile makes the same struct after a round trip.
//
//   {"format": 1, "id": "figma", "name": "FIGMA", "legend": ["UNDO", "DEPTH", "CMDS", "FRAME"],
//    "icon48": "...", "icon24": "...", "visual": "shape", "shape": "pyramid",
//    "shape_style": "thick", "shape_stepped": true, "accents": [16711680, 65280, 255],
//    "slots": {"knob": {"kind": "wheel", "label": "ZOOM", "modifier": 8, "sign": 1,
//                       "feel": "saw", "detents": 24}, "f1": {...}, ...},
//    "rings": [{"name": "LAYOUT", "tab": "LAYOUT", "slot": "f1",
//               "cmds": [{"name": "ADD AUTO LAYOUT", "key": [2, 4], "scene": {...}}]}],
//    "search": {"open": [8, 14], "open_wait": 20, "result_wait": 30},
//    "param_keys": {...},
//    "macros": [{"name": "SIGN OFF", "steps": [{"key": [8, 5]}, {"wait": 200},
//                                              {"text": "Best, K"}]}]}
//
// Macros are referred to by name: "macro" (a TAP action, or a wheel command with
// "kind": "macro") and "tap_macro" (a key's quick tap).

#include <stdbool.h>
#include <stddef.h>
#include "app_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_JSON_FORMAT 1
#define PROFILE_ID_MAX 11 // [a-z0-9_-], also the file name

// The profile as JSON text, malloc'd (free() it), or NULL (out of memory).
char *profile_json_write(const app_profile_t *p, size_t *len);

// Parses and checks JSON text into a profile that owns all its memory, or NULL with a short
// reason in `err`. Free with profile_json_free().
app_profile_t *profile_json_read(const char *text, size_t len, char *err, size_t err_len);
void profile_json_free(app_profile_t *p);

// True for a valid profile id.
bool profile_json_id_ok(const char *id);

#ifdef __cplusplus
}
#endif
