#pragma once
// App profile: everything APP mode needs to drive one application -- what the knob and
// F1-F4 send, how the knob feels while doing it, and what the screen shows. Pure data: the
// engine (app_mode.c), the menu (PROFILE row) and the screens read it; a profile never
// contains code. Adding an app = one new file in this folder + one line in app_profiles.c.
//
// The same layout is meant to become the uploaded-profile format later (DEVELOPMENT_PLAN.md
// "App profiles"), so it carries a version from the start.
//
// Key codes / modifiers / mouse buttons are TinyUSB's HID constants (class/hid/hid.h:
// HID_KEY_*, KEYBOARD_MODIFIER_*, MOUSE_BUTTON_*). Keys are USB key *positions*, so
// shortcuts assume a US keyboard layout (on German QWERTZ, Z and Y swap).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../haptic_params.h" // src/ has no include dir of its own; paths are relative

#define APP_PROFILE_VERSION 1

typedef enum {
    APP_ACT_NONE = 0,
    // Knob turn -> pointer drag: `buttons` (+ `modifier`) held while the knob moves the
    // pointer on one axis. Starts once the knob has moved a few px, so a held key that never
    // turns sends nothing. On the knob-alone slot it lets go once the knob rests.
    APP_ACT_DRAG,
    // Knob turn -> mouse wheel, one step per detent, with `modifier` held (e.g. Cmd = zoom).
    APP_ACT_WHEEL,
    // Knob turn -> one key tap per detent: `cw` one way, `ccw` the other.
    APP_ACT_KEYS,
    // Key press -> one key tap (`cw`), or the macro `macro` when set. F1-F3 only: F4 is also
    // the menu key, so it has to be a turn action (or NONE).
    APP_ACT_TAP,
    // Hold the key -> the command wheel (the profile's `rings`): turning picks a command, one
    // detent each, and letting go runs it. The first entry of every ring is "cancel". Other
    // F keys tapped while it's held jump to the ring bound to them. F1-F3 only.
    APP_ACT_COMMANDS,
    // Media keys (HID Consumer page). Knob turn -> one consumer usage per detent: `cw.keycode`
    // one way, `ccw.keycode` the other (volume up / down). Key press -> `cw.keycode`, like TAP
    // (F1-F3 only). The common usages fit in 8 bits: volume 0xE9/0xEA, mute 0xE2, play/pause
    // 0xCD, next/prev 0xB5/0xB6, stop 0xB7. The OS handles these itself (macOS shows its volume
    // OSD), so no app focus is needed. `modifier` is held around the usage on a Mac
    // (Shift+Option + volume = quarter steps); BINDINGS = PC drops it.
    APP_ACT_MEDIA,
} app_action_kind_t;

// What the Main Screen's middle shows in APP mode: the live action as large text, or a 3D
// shape that follows the knob (micro-interactions, DEVELOPMENT_PLAN.md "Plasticity").
typedef enum { APP_VISUAL_LABEL = 0, APP_VISUAL_SHAPE } app_visual_t;
typedef enum { APP_SHAPE_CUBE = 0, APP_SHAPE_PYRAMID, APP_SHAPE_OCTA } app_shape_t;
// How the shape is drawn (preview styles R3C / R2C / R4A).
typedef enum { APP_SHAPE_STYLE_SELECTED_FACE = 0, APP_SHAPE_STYLE_CAD_GRIPS, APP_SHAPE_STYLE_THICK } app_shape_style_t;
// What an action does to the shape: zoom through nested copies, turn it, slide it, or flash.
typedef enum { APP_FX_NONE = 0, APP_FX_ZOOM, APP_FX_ORBIT, APP_FX_PAN, APP_FX_FLASH } app_fx_t;

typedef struct {
    uint8_t modifier; // KEYBOARD_MODIFIER_* bits
    uint8_t keycode;  // HID_KEY_*
} app_key_t;

typedef struct {
    app_action_kind_t kind;
    const char *label;   // shown large mid-screen while this action is live ("ZOOM")
    // DRAG
    uint8_t buttons;     // MOUSE_BUTTON_* bits
    uint8_t modifier;    // KEYBOARD_MODIFIER_* held during the drag / wheel
    bool axis_y;         // pointer travel on y instead of x
    float px_per_rad;    // pointer px per radian of knob travel
    // DRAG + WHEEL: +1 / -1, flips which way the knob drives the app
    int8_t sign;
    // KEYS (cw/ccw) and TAP (cw)
    app_key_t cw, ccw;
    // KEYS / WHEEL / DRAG: sent on release when the key was tapped (let go quickly without
    // turning) -- one key, two jobs. keycode 0 = none.
    app_key_t tap;
    // Macros (the profile's `macros`, 1-based, 0 = none): TAP runs `macro` instead of `cw`; a
    // quick tap runs `tap_macro` instead of `tap`.
    uint8_t macro, tap_macro;
    // Feel while this action is live. detents 0 = the Haptics menu's STEPS value.
    haptic_type_t feel;
    uint16_t detents;
    app_fx_t fx;         // APP_VISUAL_SHAPE: what this action does to the shape
} app_action_t;

// Slots: what each input does. KNOB = turning with no key held; F1/F2/F4 = turning while
// that key is held (or a TAP on press); F3 = usually a TAP. Holding F4 ~0.7s without
// turning always opens the menu, in every profile.
typedef enum {
    APP_SLOT_KNOB = 0,
    APP_SLOT_F1,
    APP_SLOT_F2,
    APP_SLOT_F3,
    APP_SLOT_F4,
    APP_SLOT_COUNT,
} app_slot_t;

// --- Command wheel ---
// Each command has a small illustration: a 120x64 card with a mini canvas (left, x 4..60)
// and a mini layers panel (right), animated through keyframes. It is *described*, not stored
// as pixels: a few shapes per keyframe, drawn crisp at runtime (ui_cards.cpp), so a command
// costs a few hundred bytes instead of ~15KB of frames. Canvas elements are clipped to the
// canvas pane, so moving them past its edge (a pan) is fine.
typedef enum {
    APP_EL_BOX = 0,  // filled rect, corners cut (x, y, w, h)
    APP_EL_FRAME,    // 1px outline, corners cut
    APP_EL_DASH,     // dashed 1px outline (a group)
    APP_EL_DISC,     // filled circle, top-left (x, y), diameter w
    APP_EL_SEL,      // selection: 1px box + 3x3 corner handles
    APP_EL_LABEL,    // canvas label above a frame: glyph `arg` + a name bar `w` long
    APP_EL_LINE,     // (x, y) -> (w, h)
    APP_EL_CLIPBOARD,// 13x17 clipboard at (x, y)
    APP_EL_PLAY,     // play button: disc + black triangle, diameter w
    APP_EL_ROW,      // layers row: y = row index, x = depth, glyph `arg`, name bar `w`, flags `h`,
                     // `d` = extra px down (rows under a mode strip)
    // Isometric 2:1 box, drawn as a wireframe (Plasticity): (x, y) = the (0,0,0) corner, w = a
    // (along x, right-down), h = b (along y, left-down), d = height, arg = fillet radius on
    // the front vertical edge, flags = APP_ISO_*.
    APP_EL_ISO,
    APP_EL_MODES,    // Plasticity's selection-mode strip (point / edge / face / solid), arg = active bits
    // Arc: centre (x, y), radius w, from arg/10 to d/10 rad. An iso ellipse (r x r/2, 0..pi =
    // the front half) with a head at the end unless flags say otherwise (APP_ARC_*).
    APP_EL_ARC,
    APP_EL_NUM,      // the number `arg` in the UI font, centred on x, top at y
    APP_EL_ROLLBACK, // Onshape's rollback bar under feature-list row y - 1 (rows as APP_EL_ROW, `d` offset)
} app_el_op_t;
// APP_EL_ARC flags
#define APP_ARC_ROUND 0x01   // a circle (r x r, 0..pi = the upper half), not an iso ellipse
#define APP_ARC_NO_HEAD 0x02
#define APP_ARC_DOTTED 0x04
// APP_EL_ISO flags
#define APP_ISO_SOLID 0x01   // whole body selected (amber edges)
#define APP_ISO_FACE 0x02    // top face selected / changed (amber dots + edges)
#define APP_ISO_EDGE 0x04    // front vertical edge selected
#define APP_ISO_POINTS 0x08  // corner points selected
#define APP_ISO_GHOST 0x10   // dashed dark outline (where something was / will be)
#define APP_ISO_NO_BOTTOM 0x20 // no bottom edges (sits merged on another body)
#define APP_ISO_GRIPS 0x40   // scale grips on the corners
#define APP_ISO_HOLLOW 0x80  // shelled: an inset rim on the top face
// APP_EL_ISO arg bits above the radius (0-63)
#define APP_ISO_ARG_CUT 0x40     // the x = a face is a section cut (amber dots)
#define APP_ISO_ARG_CHAMFER 0x80 // the rounded edge is a chamfer
// APP_EL_LINE arg
#define APP_LINE_HEAD 0x01   // 3x3 head at the end (an arrow)
#define APP_LINE_DASHED 0x02
typedef enum { APP_C_BLACK = 0, APP_C_DARK, APP_C_GREY, APP_C_WHITE, APP_C_AMBER } app_el_color_t;
typedef enum {
    APP_GLYPH_RECT = 0, APP_GLYPH_CIRCLE, APP_GLYPH_FRAME, APP_GLYPH_AUTO, APP_GLYPH_GROUP,
    APP_GLYPH_COMP, APP_GLYPH_INST, APP_GLYPH_TEXT,
    APP_GLYPH_SOLID, APP_GLYPH_SHEET, // Plasticity outliner (Onshape: feature, plane)
    APP_GLYPH_SKETCH,                 // Onshape feature list: a sketch
} app_glyph_t;
// APP_EL_ROW flags
#define APP_ROW_SEL 0x01      // selected (dark band)
#define APP_ROW_DOT 0x02      // override dot after the name
#define APP_ROW_FIELD 0x04    // inline rename field
#define APP_ROW_ALL 0x08      // name selected (amber, text black)
#define APP_ROW_CURSOR 0x10   // text cursor after the name

typedef struct {
    uint8_t op;    // app_el_op_t
    uint8_t color; // app_el_color_t
    int8_t x, y;
    uint8_t w, h;
    uint8_t arg;
    uint8_t d;     // ISO height, ARC end, ROW offset
    uint8_t flags; // ISO: APP_ISO_*
} app_el_t;

typedef struct {
    uint16_t ms;          // how long this keyframe shows
    uint8_t n;
    const app_el_t *el;
} app_keyframe_t;

typedef struct {
    uint8_t n_base;
    const app_el_t *base; // drawn under every keyframe (may be NULL)
    uint8_t n_frames;
    const app_keyframe_t *frames;
} app_scene_t;

// Authoring helpers for scene data (profile files). Colors: K_D dark, K_G grey, K_W white,
// K_A amber, K_B black.
#define K_B APP_C_BLACK
#define K_D APP_C_DARK
#define K_G APP_C_GREY
#define K_W APP_C_WHITE
#define K_A APP_C_AMBER
#define EL_BOX(x, y, w, h, c) {APP_EL_BOX, c, x, y, w, h, 0, 0, 0}
#define EL_FRAME(x, y, w, h, c) {APP_EL_FRAME, c, x, y, w, h, 0, 0, 0}
#define EL_DASH(x, y, w, h, c) {APP_EL_DASH, c, x, y, w, h, 0, 0, 0}
#define EL_DISC(x, y, d, c) {APP_EL_DISC, c, x, y, d, d, 0, 0, 0}
#define EL_SEL(x, y, w, h) {APP_EL_SEL, K_A, x, y, w, h, 0, 0, 0}
#define EL_LABEL(x, y, glyph, len, c) {APP_EL_LABEL, c, x, y, len, 0, glyph, 0, 0}
#define EL_LINE(x0, y0, x1, y1, c) {APP_EL_LINE, c, x0, y0, x1, y1, 0, 0, 0}
#define EL_CLIPBOARD(x, y, c) {APP_EL_CLIPBOARD, c, x, y, 13, 17, 0, 0, 0}
#define EL_PLAY(x, y, d) {APP_EL_PLAY, K_A, x, y, d, d, 0, 0, 0}
#define EL_ROW(i, depth, glyph, len, c, flags) {APP_EL_ROW, c, depth, i, len, flags, glyph, 0, 0}
#define EL_ISO(x, y, a, b, h, flags) {APP_EL_ISO, K_W, x, y, a, b, 0, h, flags}
#define EL_ISO_FILLET(x, y, a, b, h, r, flags) {APP_EL_ISO, K_W, x, y, a, b, r, h, flags}
#define EL_MODES(bits) {APP_EL_MODES, K_A, 0, 0, 0, 0, bits, 0, 0}
#define EL_ARC(x, y, r, t0, t1, c) {APP_EL_ARC, c, x, y, r, 0, t0, t1, 0}
#define EL_ELLIPSE(x, y, r, t0, t1, c, flags) {APP_EL_ARC, c, x, y, r, 0, t0, t1, (APP_ARC_NO_HEAD | (flags))}
#define EL_CIRCLE(x, y, r, c) {APP_EL_ARC, c, x, y, r, 0, 0, 63, (APP_ARC_ROUND | APP_ARC_NO_HEAD)}
#define EL_CARC(x, y, r, t0, t1, c) {APP_EL_ARC, c, x, y, r, 0, t0, t1, (APP_ARC_ROUND | APP_ARC_NO_HEAD)}
#define EL_NUM(x, y, n, c) {APP_EL_NUM, c, x, y, 0, 0, n, 0, 0}
// Onshape's feature list: rows from the top of the right pane, the rollback bar under row i - 1.
#define EL_FROW(i, glyph, len, c, flags) {APP_EL_ROW, c, 0, i, len, flags, glyph, 2, 0}
#define EL_ROLLBACK(i) {APP_EL_ROLLBACK, K_G, 0, i, 0, 0, 0, 2, 0}
#define EL_ARROW(x0, y0, x1, y1, c) {APP_EL_LINE, c, x0, y0, x1, y1, APP_LINE_HEAD, 0, 0}
#define EL_DLINE(x0, y0, x1, y1, c) {APP_EL_LINE, c, x0, y0, x1, y1, APP_LINE_DASHED, 0, 0}
// Outliner row under the mode strip.
#define EL_PROW(i, glyph, len, c, flags) {APP_EL_ROW, c, 0, i, len, flags, glyph, 14, 0}
#define EL_LIST(...) (const app_el_t[]){__VA_ARGS__}
#define EL_COUNT(...) (uint8_t)(sizeof((const app_el_t[]){__VA_ARGS__}) / sizeof(app_el_t))
#define KEYFRAME(ms, ...) {ms, EL_COUNT(__VA_ARGS__), EL_LIST(__VA_ARGS__)}

typedef enum { APP_CMD_KEYS = 0, APP_CMD_ACTIONS, APP_CMD_MACRO } app_cmd_kind_t;

// --- Macros ---
// A named list of steps the knob types by itself: key taps, text and pauses. Stored with the
// profile, run by the device (no host software needed). Keys are US positions like everywhere
// else; text is plain printable ASCII, typed as US keys.
typedef enum { APP_MSTEP_KEY = 0, APP_MSTEP_TEXT, APP_MSTEP_WAIT } app_mstep_kind_t;
typedef struct {
    uint8_t kind;     // app_mstep_kind_t
    app_key_t key;    // KEY
    uint16_t ms;      // WAIT
    const char *text; // TEXT
} app_mstep_t;
typedef struct {
    const char *name; // "SIGNATURE"
    uint8_t count;
    const app_mstep_t *steps;
} app_macro_t;
#define APP_MACROS_MAX 16
#define APP_MACRO_STEPS_MAX 64

// --- Parameter mode ---
// After a command with a `param` runs from the wheel, the knob sets its value until F3
// confirms (tap) or cancels (hold): free = fine clicks, each moving the pointer so the app's own
// handle follows; F1 / F2 / F4 held = exact steps, typed in on confirm. With APP_PARAM_AXES, tapping F1 / F2 /
// F4 constrains to X / Y / Z (again: the plane, then uniform). The card follows the value
// through one of the renderer's parametric visuals.
//
// Profiles with `param_keys.field` (Onshape) drive the dialog's number field instead of a
// handle: every knob click is one step -- steps[0] with F1 held, steps[1] alone, steps[2] with
// F4 -- sent one of two ways, switched by tapping F2 (remembered):
//   A, scroll: a wheel notch over the field the user points at, with `step_mod[i]` held (Onshape:
//      Ctrl 0.01, none 0.1, Shift 1.0). The device can't read the field: it shows the change.
//   B, type: the device holds the value (from `start`) and retypes it -- `select_all`, digits --
//      once the knob rests. It shows the value.
typedef enum {
    APP_PV_NONE = 0, APP_PV_FILLET, APP_PV_EXTRUDE, APP_PV_OFFSET, APP_PV_HOLLOW,
    APP_PV_MOVE, APP_PV_ROTATE, APP_PV_SCALE,
    APP_PV_CHAMFER, // an edge chamfer growing (no sign flip)
    APP_PV_SLIDE,   // a body sliding along one direction, no axis choice
} app_param_visual_t;
#define APP_PARAM_DEG 0x01     // an angle (degree mark, steps in degrees)
#define APP_PARAM_AXES 0x02    // X / Y / Z constraint by tapping F1 / F2 / F4
#define APP_PARAM_PLANES 0x04  // ...and planes (Shift + X / Y / Z)
#define APP_PARAM_UNIFORM 0x08 // ...and uniform (the `uniform` key)
#define APP_AXIS_UNIFORM 3     // axis_default: start uniform

typedef struct {
    const char *label;      // "DISTANCE"
    const char *label_neg;  // shown below zero ("CHAMFER" for a fillet), NULL = label
    float steps[3];         // F1 / F2 / F4 held: exact step sizes
    float free_step;        // knob alone: value change per fine click (the on-screen estimate)
    float px_per_step;      // knob alone: pointer travel per fine click (what the app follows)
    float start, min, max;
    uint8_t decimals;
    uint8_t flags;          // APP_PARAM_*
    uint8_t visual;         // app_param_visual_t
    uint8_t modes;          // selection-mode strip bits on the card
    app_key_t enter;        // sent right after the command (e.g. D = distance), 0 = none
    uint8_t axis_default;   // 0-2 = X / Y / Z, APP_AXIS_UNIFORM
} app_param_t;

// How a profile's parameter mode talks to the app.
typedef struct {
    app_key_t numeric;      // opens exact entry (Plasticity: Tab)
    app_key_t confirm;      // Enter
    app_key_t cancel;       // Esc
    app_key_t axis[3];      // X / Y / Z constraint keys (the plane = the same + Shift)
    app_key_t uniform;      // S
    // Number-field input (see "Parameter mode" above); false = the pointer drives a handle.
    bool field;
    uint8_t step_mod[3];    // A: modifier held for each step's scroll notch
    int8_t scroll_sign;     // A: +1 = wheel up raises the value
    app_key_t select_all;   // B: before the digits (Cmd+A)
} app_param_keys_t;

typedef struct {
    const char *name;         // on screen, caps ("WRAP IN FRAME")
    app_cmd_kind_t kind;
    app_key_t key;            // KEYS: the shortcut
    const char *phrase;       // ACTIONS: typed into the app's command search (lowercase ASCII)
    const app_scene_t *scene; // the card, or NULL
    const app_param_t *param; // parameter mode after it runs, or NULL
    uint8_t macro;            // MACRO: the profile's macro, 1-based
} app_cmd_t;

typedef struct {
    const char *name;         // "STRUCTURE"
    const char *tab;          // short name for the ring tabs, <= 6 chars ("BUILD")
    // app_slot_t of the F key that jumps here while the wheel is open. Several rings may share
    // a key: tapping it again steps through them (F1: STRUCTURE -> ALIGN -> STRUCTURE).
    uint8_t slot;
    uint8_t count;
    const app_cmd_t *cmds;    // count entries; "cancel" is added in front by the engine
} app_ring_t;

// ACTIONS: how to reach the app's command search, and how long to wait for it (10ms ticks).
typedef struct {
    app_key_t open;           // Figma: Cmd+K
    uint8_t open_wait;        // after opening, before typing
    uint8_t result_wait;      // after typing, before Enter
} app_search_t;

typedef struct {
    uint16_t version;          // APP_PROFILE_VERSION
    const char *id;            // stable id, stored in NVS ("figma")
    const char *name;          // shown in the status bar and the PROFILE row ("FIGMA")
    const uint8_t *icon24;     // 24x24 RGB565 BE status-bar icon (app_icons.h), or NULL
    const uint8_t *icon48;     // 48x48 RGB565 BE icon for the PROFILE screen, or NULL
    const char *legend[4];     // under the F1-F4 keycaps, <= 5 chars
    app_visual_t visual;       // Main Screen middle (LABEL unless set)
    app_shape_t shape;         // APP_VISUAL_SHAPE: which shape
    app_shape_style_t shape_style; // ...and how it's drawn
    bool shape_stepped;        // show only clean poses: 32 per turn, zoom in 1/8 doublings, pan in 2px
    // The app's three accent colours (RGB888, dark to bright) for the idle screen and the LEDs.
    // All 0 = sampled from icon48 (app_colors.h).
    uint32_t accents[3];
    app_action_t slot[APP_SLOT_COUNT];
    // Command wheel (a slot with APP_ACT_COMMANDS)
    uint8_t ring_count;
    const app_ring_t *rings;
    app_search_t search;
    app_param_keys_t param_keys;
    // Macros, referred to (1-based) by actions and wheel commands.
    uint8_t macro_count;
    const app_macro_t *macros;
} app_profile_t;

// --- Empty template ---
// A profile with nothing app-specific: the knob scrolls (as outside APP mode), F1-F3 do
// nothing, holding F4 still opens the menu. A new app starts as one line
//     const app_profile_t app_profile_blender =
//         APP_PROFILE_EMPTY_ICON("blender", "BLENDER", app_icon_blender_24, app_icon_blender_48);
// and is filled in later (APP_PROFILE_EMPTY: the placeholder icon). `app_profile_empty` (empty.c) is the same template, used in place of
// a registry entry that is missing or fails the checks in app_profiles.c. Needs
// "icons/app_icons.h" (placeholder icon) and "class/hid/hid.h".
#define APP_PROFILE_EMPTY(id_, name_) APP_PROFILE_EMPTY_ICON(id_, name_, app_icon_empty_24, app_icon_empty_48)
#define APP_PROFILE_EMPTY_ICON(id_, name_, icon24_, icon48_) {                 \
    .version = APP_PROFILE_VERSION,                                           \
    .id = id_,                                                                \
    .name = name_,                                                            \
    .icon24 = icon24_,                                                        \
    .icon48 = icon48_,                                                        \
    .legend = {"-", "-", "-", "MENU"},                                        \
    .slot = {                                                                 \
        [APP_SLOT_KNOB] = {                                                   \
            .kind = APP_ACT_WHEEL, .label = "SCROLL", .sign = 1,              \
            .feel = HAPTIC_TYPE_SAW,                                          \
        },                                                                    \
    },                                                                        \
}
