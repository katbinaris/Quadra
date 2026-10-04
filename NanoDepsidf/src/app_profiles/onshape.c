// Onshape (browser CAD). A port of the Plasticity profile (DEVELOPMENT_PLAN.md "Onshape
// profile"). Its default mouse (cad.onshape.com/help, "View navigation"): right-drag rotates,
// middle-drag pans, the scroll wheel zooms -- there is no drag-zoom, so the knob zooms one
// notch per click. Keys only land while the Onshape tab has focus.
#include "app_profile.h"
#include "icons/app_icons.h"
#include "class/hid/hid.h"

#define PX_PER_RAD 120.0f // ~750px of pointer travel per knob turn (as Plasticity)

// Wheel up = zoom in (Onshape's default direction). Flip `sign` if the knob zooms the wrong way.
#define ZOOM {                                                                 \
    .kind = APP_ACT_WHEEL, .label = "ZOOM", .sign = 1,                         \
    .feel = HAPTIC_TYPE_SAW, .detents = 24, .fx = APP_FX_ZOOM,                 \
}

#define CTRL KEYBOARD_MODIFIER_LEFTCTRL
#define SHIFT KEYBOARD_MODIFIER_LEFTSHIFT
#define OPT KEYBOARD_MODIFIER_LEFTALT
#define CMD KEYBOARD_MODIFIER_LEFTGUI

// --- Command wheel (preview round 1: https://claude.ai/artifact/FBsPx6XgUQGaXP2emcbitR) ---
// Default keys from Onshape's "Keyboard shortcuts" page; tools without one go through tool search
// (Opt+C, type, Enter). Cards: Plasticity's iso wireframe viewport on the left, Onshape's
// feature list on the right -- the new feature amber above the rollback bar.
#define BOX(f) EL_ISO(32, 30, 16, 16, 12, f)
#define FLASH EL_FRAME(5, 5, 55, 54, K_A)
#define TREE EL_FROW(0, APP_GLYPH_SKETCH, 14, K_G, 0), EL_FROW(1, APP_GLYPH_SOLID, 18, K_G, 0)
#define TREE0 TREE, EL_ROLLBACK(2)
#define TREE_NEW(glyph, len) TREE, EL_FROW(2, glyph, len, K_A, 0), EL_ROLLBACK(3)
#define TREE_ADD(len) TREE_NEW(APP_GLYPH_SOLID, len)
// Editing a sketch: the plane, the sketch selected.
#define TREE_SK EL_FROW(0, APP_GLYPH_SHEET, 10, K_G, 0), EL_FROW(1, APP_GLYPH_SKETCH, 14, K_W, APP_ROW_SEL), EL_ROLLBACK(2)
// Extrude / revolve pick up the sketch.
#define TREE_PICK EL_FROW(0, APP_GLYPH_SKETCH, 14, K_W, APP_ROW_SEL), EL_ROLLBACK(1)
#define TREE_MADE EL_FROW(0, APP_GLYPH_SKETCH, 14, K_G, 0), EL_FROW(1, APP_GLYPH_SOLID, 18, K_A, 0), EL_ROLLBACK(2)
// 2D sketch view: the plane's outline (dashed), a 3x3 vertex.
#define PLANE EL_DASH(8, 8, 48, 48, K_D)
#define VTX(x, y, c) EL_BOX((x) - 1, (y) - 1, 3, 3, c)
#define SCENE(name, ...)                                                           \
    static const app_scene_t name = {                                              \
        .n_frames = (uint8_t)(sizeof((const app_keyframe_t[]){__VA_ARGS__}) / sizeof(app_keyframe_t)), \
        .frames = (const app_keyframe_t[]){__VA_ARGS__},                           \
    }

// MODEL
SCENE(SC_SKETCH,
      KEYFRAME(650, BOX(APP_ISO_FACE), TREE0),
      KEYFRAME(90, BOX(APP_ISO_FACE), FLASH, TREE0),
      KEYFRAME(500, EL_FRAME(16, 16, 32, 32, K_W), TREE_NEW(APP_GLYPH_SKETCH, 12)),
      KEYFRAME(1300, EL_FRAME(16, 16, 32, 32, K_W), EL_FRAME(22, 22, 20, 14, K_A), TREE_NEW(APP_GLYPH_SKETCH, 12)));
SCENE(SC_EXTRUDE,
      KEYFRAME(600, EL_ISO(32, 40, 16, 16, 0, APP_ISO_FACE), TREE_PICK),
      KEYFRAME(80, EL_ISO(32, 38, 16, 16, 5, APP_ISO_FACE), TREE_PICK),
      KEYFRAME(80, EL_ISO(32, 36, 16, 16, 10, APP_ISO_FACE), TREE_PICK),
      KEYFRAME(1300, EL_ISO(32, 35, 16, 16, 14, APP_ISO_FACE), TREE_MADE));
// A profile beside a dashed axis sweeps round into a cylinder.
#define AXIS(c) EL_DLINE(32, 8, 32, 56, c)
#define PROFILE EL_FRAME(34, 20, 10, 24, K_W)
SCENE(SC_REVOLVE,
      KEYFRAME(650, AXIS(K_A), PROFILE, TREE_PICK),
      KEYFRAME(90, AXIS(K_A), PROFILE, EL_ELLIPSE(32, 44, 12, 0, 31, K_A, 0), TREE_PICK),
      KEYFRAME(1300, AXIS(K_D), EL_ELLIPSE(32, 20, 12, 0, 63, K_W, 0), EL_ELLIPSE(32, 44, 12, 0, 31, K_W, 0),
               EL_ELLIPSE(32, 44, 12, 31, 63, K_D, APP_ARC_DOTTED), EL_LINE(20, 20, 20, 44, K_W),
               EL_LINE(44, 20, 44, 44, K_W), TREE_MADE));
SCENE(SC_FILLET,
      KEYFRAME(650, BOX(APP_ISO_EDGE), TREE0),
      KEYFRAME(90, EL_ISO_FILLET(32, 30, 16, 16, 12, 3, 0), TREE_ADD(14)),
      KEYFRAME(1300, EL_ISO_FILLET(32, 30, 16, 16, 12, 6, 0), TREE_ADD(14)));
SCENE(SC_CHAMFER,
      KEYFRAME(650, BOX(APP_ISO_EDGE), TREE0),
      KEYFRAME(90, EL_ISO_FILLET(32, 30, 16, 16, 12, 3 | APP_ISO_ARG_CHAMFER, 0), TREE_ADD(16)),
      KEYFRAME(1300, EL_ISO_FILLET(32, 30, 16, 16, 12, 6 | APP_ISO_ARG_CHAMFER, 0), TREE_ADD(16)));
SCENE(SC_SHELL,
      KEYFRAME(650, BOX(APP_ISO_FACE), TREE0),
      KEYFRAME(1300, EL_ISO_FILLET(32, 30, 16, 16, 12, 3, APP_ISO_HOLLOW), TREE_ADD(12)));

// MODIFY
SCENE(SC_BOOLEAN,
      KEYFRAME(650, EL_ISO(30, 38, 20, 20, 8, 0), EL_ISO(30, 20, 10, 10, 8, APP_ISO_SOLID), TREE0),
      KEYFRAME(90, EL_ISO(30, 38, 20, 20, 8, 0), EL_ISO(30, 25, 10, 10, 8, APP_ISO_SOLID), TREE0),
      KEYFRAME(1300, EL_ISO(30, 38, 20, 20, 8, APP_ISO_SOLID), EL_ISO(30, 30, 10, 10, 8, APP_ISO_SOLID | APP_ISO_NO_BOTTOM),
               TREE_ADD(16)));
SCENE(SC_SPLIT,
      KEYFRAME(650, BOX(0), EL_DLINE(20, 14, 46, 46, K_A), TREE0),
      KEYFRAME(1300, EL_ISO(28, 32, 8, 16, 12, 0), EL_ISO(40, 30, 8, 16, 12, APP_ISO_SOLID), TREE_ADD(10)));
SCENE(SC_TRANSFORM,
      KEYFRAME(650, EL_ISO(24, 28, 12, 12, 10, APP_ISO_SOLID), TREE0),
      KEYFRAME(90, EL_ISO(24, 28, 12, 12, 10, APP_ISO_GHOST), EL_ISO(31, 31, 12, 12, 10, APP_ISO_SOLID), TREE0),
      KEYFRAME(1300, EL_ISO(24, 28, 12, 12, 10, APP_ISO_GHOST), EL_ISO(38, 34, 12, 12, 10, APP_ISO_SOLID),
               EL_ARROW(28, 42, 42, 49, K_A), TREE_ADD(20)));
SCENE(SC_PATTERN,
      KEYFRAME(650, EL_ISO(16, 22, 8, 8, 8, APP_ISO_SOLID), TREE0),
      KEYFRAME(90, EL_ISO(16, 22, 8, 8, 8, APP_ISO_SOLID), EL_ISO(30, 29, 8, 8, 8, APP_ISO_GHOST),
               EL_ISO(44, 36, 8, 8, 8, APP_ISO_GHOST), TREE0),
      KEYFRAME(1300, EL_ISO(16, 22, 8, 8, 8, 0), EL_ISO(30, 29, 8, 8, 8, APP_ISO_SOLID), EL_ISO(44, 36, 8, 8, 8, APP_ISO_SOLID),
               TREE_ADD(16)));
SCENE(SC_MIRROR,
      KEYFRAME(650, EL_ISO(22, 26, 10, 12, 12, APP_ISO_SOLID), EL_DLINE(33, 10, 33, 54, K_A), TREE0),
      KEYFRAME(1300, EL_ISO(22, 26, 10, 12, 12, 0), EL_DLINE(33, 10, 33, 54, K_A), EL_ISO(46, 26, 12, 10, 12, APP_ISO_SOLID),
               TREE_ADD(14)));
SCENE(SC_MOVE_FACE,
      KEYFRAME(650, BOX(APP_ISO_FACE), TREE0),
      KEYFRAME(80, EL_ISO(32, 30, 16, 16, 15, APP_ISO_FACE), TREE0),
      KEYFRAME(1300, EL_ISO(32, 30, 16, 16, 18, APP_ISO_FACE), EL_ARROW(52, 26, 52, 14, K_A), TREE_ADD(18)));

// SKETCH: flat 2D views; the new entity amber, placed vertices white.
SCENE(SC_LINE,
      KEYFRAME(500, PLANE, VTX(16, 46, K_A), TREE_SK),
      KEYFRAME(90, PLANE, EL_LINE(16, 46, 30, 34, K_A), VTX(16, 46, K_W), TREE_SK),
      KEYFRAME(1300, PLANE, EL_LINE(16, 46, 46, 20, K_A), VTX(16, 46, K_W), VTX(46, 20, K_W), TREE_SK));
SCENE(SC_RECT,
      KEYFRAME(500, PLANE, VTX(16, 18, K_A), TREE_SK),
      KEYFRAME(90, PLANE, EL_FRAME(16, 18, 16, 12, K_A), VTX(16, 18, K_W), TREE_SK),
      KEYFRAME(1300, PLANE, EL_FRAME(16, 18, 32, 26, K_A), VTX(16, 18, K_W), VTX(47, 43, K_W), TREE_SK));
SCENE(SC_CIRCLE,
      KEYFRAME(500, PLANE, VTX(32, 32, K_A), TREE_SK),
      KEYFRAME(90, PLANE, EL_CIRCLE(32, 32, 8, K_A), VTX(32, 32, K_W), TREE_SK),
      KEYFRAME(1300, PLANE, EL_CIRCLE(32, 32, 16, K_A), VTX(32, 32, K_W), TREE_SK));
SCENE(SC_ARC,
      KEYFRAME(500, PLANE, VTX(14, 40, K_A), VTX(50, 40, K_A), TREE_SK),
      KEYFRAME(1300, PLANE, EL_CARC(32, 44, 18, 2, 29, K_A), VTX(14, 40, K_W), VTX(50, 40, K_W), TREE_SK));
#define DIM_LINE EL_LINE(14, 40, 50, 40, K_W), VTX(14, 40, K_W), VTX(50, 40, K_W)
SCENE(SC_DIMENSION,
      KEYFRAME(500, PLANE, DIM_LINE, TREE_SK),
      KEYFRAME(1300, PLANE, DIM_LINE, EL_LINE(14, 36, 14, 22, K_A), EL_LINE(50, 36, 50, 22, K_A),
               EL_LINE(14, 26, 50, 26, K_A), EL_NUM(32, 14, 20, K_A), TREE_SK));
SCENE(SC_TRIM,
      KEYFRAME(500, PLANE, EL_LINE(12, 32, 52, 32, K_W), EL_LINE(32, 12, 32, 52, K_W), TREE_SK),
      KEYFRAME(120, PLANE, EL_LINE(12, 32, 52, 32, K_W), EL_LINE(32, 12, 32, 31, K_W), EL_DLINE(32, 33, 32, 52, K_A), TREE_SK),
      KEYFRAME(1300, PLANE, EL_LINE(12, 32, 52, 32, K_W), EL_LINE(32, 12, 32, 32, K_W), VTX(32, 32, K_W), TREE_SK));
SCENE(SC_CONSTRUCTION,
      KEYFRAME(650, PLANE, EL_LINE(14, 44, 50, 20, K_W), VTX(14, 44, K_W), VTX(50, 20, K_W), TREE_SK),
      KEYFRAME(1300, PLANE, EL_DLINE(14, 44, 50, 20, K_A), VTX(14, 44, K_W), VTX(50, 20, K_W), TREE_SK));

// VIEW: the model, a flash as the camera snaps, then the new view.
#define VIEW_SCENE(name, before, ...) \
    SCENE(name, KEYFRAME(650, before, TREE0), KEYFRAME(90, before, FLASH, TREE0), KEYFRAME(1300, __VA_ARGS__, TREE0))
VIEW_SCENE(SC_FRONT, BOX(0), EL_FRAME(16, 20, 32, 24, K_W));
VIEW_SCENE(SC_RIGHT, BOX(0), EL_FRAME(20, 20, 24, 24, K_W));
VIEW_SCENE(SC_TOP, BOX(0), EL_FRAME(16, 16, 32, 32, K_W));
VIEW_SCENE(SC_ISO, EL_FRAME(16, 20, 32, 24, K_W), BOX(0));
VIEW_SCENE(SC_NORMAL, BOX(APP_ISO_FACE), EL_FRAME(16, 16, 32, 32, K_A));
VIEW_SCENE(SC_FIT, EL_ISO(46, 42, 6, 6, 5, 0), EL_ISO(32, 22, 20, 20, 16, 0));
SCENE(SC_SECTION,
      KEYFRAME(650, BOX(0), TREE0),
      KEYFRAME(90, BOX(0), EL_DLINE(28, 12, 28, 58, K_A), TREE0),
      KEYFRAME(1300, EL_ISO_FILLET(32, 30, 8, 16, 12, APP_ISO_ARG_CUT, 0), TREE0));

// Parameter mode: Onshape's number fields step 0.1 per scroll notch, 0.01 with Ctrl, 1.0 with
// Shift ("Numeric fields"). steps = F1 / knob alone / F4. `start` is only used by B (type):
// Onshape's defaults are to check (metric documents assumed).
#define STEPS_FIELD {0.01f, 0.10f, 1.00f}
static const app_param_t P_EXTRUDE = {"DEPTH", NULL, STEPS_FIELD, 0, 0, 25.0f, 0.0f, 1000.0f, 2, 0, APP_PV_EXTRUDE, 0, {0, 0}, 0};
static const app_param_t P_FILLET = {"RADIUS", NULL, STEPS_FIELD, 0, 0, 1.0f, 0.0f, 100.0f, 2, 0, APP_PV_FILLET, 0, {0, 0}, 0};
static const app_param_t P_CHAMFER = {"DISTANCE", NULL, STEPS_FIELD, 0, 0, 1.0f, 0.0f, 100.0f, 2, 0, APP_PV_CHAMFER, 0, {0, 0}, 0};
static const app_param_t P_SHELL = {"THICKNESS", NULL, STEPS_FIELD, 0, 0, 1.0f, 0.01f, 100.0f, 2, 0, APP_PV_HOLLOW, 0, {0, 0}, 0};
static const app_param_t P_MOVE_FACE = {"DISTANCE", NULL, STEPS_FIELD, 0, 0, 0.0f, -1000.0f, 1000.0f, 2, 0, APP_PV_OFFSET, 0, {0, 0}, 0};
static const app_param_t P_TRANSFORM = {"DISTANCE", NULL, STEPS_FIELD, 0, 0, 0.0f, -1000.0f, 1000.0f, 2, 0, APP_PV_SLIDE, 0, {0, 0}, 0};

static const app_cmd_t MODEL[] = {
    {"SKETCH", APP_CMD_KEYS, {SHIFT, HID_KEY_S}, NULL, &SC_SKETCH, NULL, 0},
    {"EXTRUDE", APP_CMD_KEYS, {SHIFT, HID_KEY_E}, NULL, &SC_EXTRUDE, &P_EXTRUDE, 0},
    {"REVOLVE", APP_CMD_KEYS, {SHIFT, HID_KEY_W}, NULL, &SC_REVOLVE, NULL, 0},
    {"FILLET", APP_CMD_KEYS, {SHIFT, HID_KEY_F}, NULL, &SC_FILLET, &P_FILLET, 0},
    {"CHAMFER", APP_CMD_ACTIONS, {0, 0}, "chamfer", &SC_CHAMFER, &P_CHAMFER, 0},
    {"SHELL", APP_CMD_ACTIONS, {0, 0}, "shell", &SC_SHELL, &P_SHELL, 0},
};
static const app_cmd_t MODIFY[] = {
    {"BOOLEAN", APP_CMD_ACTIONS, {0, 0}, "boolean", &SC_BOOLEAN, NULL, 0},
    {"SPLIT", APP_CMD_ACTIONS, {0, 0}, "split", &SC_SPLIT, NULL, 0},
    {"TRANSFORM", APP_CMD_ACTIONS, {0, 0}, "transform", &SC_TRANSFORM, &P_TRANSFORM, 0},
    {"PATTERN", APP_CMD_ACTIONS, {0, 0}, "linear pattern", &SC_PATTERN, NULL, 0},
    {"MIRROR", APP_CMD_ACTIONS, {0, 0}, "mirror", &SC_MIRROR, NULL, 0},
    {"MOVE FACE", APP_CMD_ACTIONS, {0, 0}, "move face", &SC_MOVE_FACE, &P_MOVE_FACE, 0},
};
// Single-letter sketch tools: only active while a sketch is open.
static const app_cmd_t SKETCH[] = {
    {"LINE", APP_CMD_KEYS, {0, HID_KEY_L}, NULL, &SC_LINE, NULL, 0},
    {"RECTANGLE", APP_CMD_KEYS, {0, HID_KEY_G}, NULL, &SC_RECT, NULL, 0},
    {"CIRCLE", APP_CMD_KEYS, {0, HID_KEY_C}, NULL, &SC_CIRCLE, NULL, 0},
    {"ARC", APP_CMD_KEYS, {0, HID_KEY_A}, NULL, &SC_ARC, NULL, 0},
    {"DIMENSION", APP_CMD_KEYS, {0, HID_KEY_D}, NULL, &SC_DIMENSION, NULL, 0},
    {"TRIM", APP_CMD_KEYS, {0, HID_KEY_M}, NULL, &SC_TRIM, NULL, 0},
    {"CONSTRUCTION", APP_CMD_KEYS, {0, HID_KEY_Q}, NULL, &SC_CONSTRUCTION, NULL, 0},
};
static const app_cmd_t VIEW[] = {
    {"FRONT", APP_CMD_KEYS, {SHIFT, HID_KEY_1}, NULL, &SC_FRONT, NULL, 0},
    {"RIGHT", APP_CMD_KEYS, {SHIFT, HID_KEY_4}, NULL, &SC_RIGHT, NULL, 0},
    {"TOP", APP_CMD_KEYS, {SHIFT, HID_KEY_5}, NULL, &SC_TOP, NULL, 0},
    {"ISOMETRIC", APP_CMD_KEYS, {SHIFT, HID_KEY_7}, NULL, &SC_ISO, NULL, 0},
    {"NORMAL TO", APP_CMD_KEYS, {0, HID_KEY_N}, NULL, &SC_NORMAL, NULL, 0},
    {"ZOOM TO FIT", APP_CMD_KEYS, {0, HID_KEY_F}, NULL, &SC_FIT, NULL, 0},
    {"SECTION", APP_CMD_KEYS, {SHIFT, HID_KEY_X}, NULL, &SC_SECTION, NULL, 0},
};
// F1 steps MODEL <-> MODIFY, F2 SKETCH, F4 VIEW.
static const app_ring_t RINGS[] = {
    {"MODEL", "MODEL", APP_SLOT_F1, 6, MODEL},
    {"MODIFY", "MODIFY", APP_SLOT_F1, 6, MODIFY},
    {"SKETCH", "SKETCH", APP_SLOT_F2, 7, SKETCH},
    {"VIEW", "VIEW", APP_SLOT_F4, 7, VIEW},
};

const app_profile_t app_profile_onshape = {
    .version = APP_PROFILE_VERSION,
    .id = "onshape",
    .name = "ONSHAPE",
    .icon24 = app_icon_onshape_24,
    .icon48 = app_icon_onshape_48,
    .legend = {"ZOOM", "ORBIT", "WHEEL", "PAN"},
    // The Plasticity demo shape, as a cube: Onshape's mark is a hexagon, which is how an
    // isometric cube reads.
    .visual = APP_VISUAL_SHAPE,
    .shape = APP_SHAPE_CUBE,
    .shape_style = APP_SHAPE_STYLE_THICK,
    .shape_stepped = true,
    // The icon is only green + white, too few colours to sample: teal -> Onshape green -> lime.
    .accents = {0x0F9D8A, 0x64BC4F, 0xB5E35A},
    .slot = {
        [APP_SLOT_KNOB] = ZOOM,
        [APP_SLOT_F1] = ZOOM,
        [APP_SLOT_F2] = {
            .kind = APP_ACT_DRAG, .label = "ORBIT",
            .buttons = MOUSE_BUTTON_RIGHT, .px_per_rad = PX_PER_RAD, .sign = 1,
            .feel = HAPTIC_TYPE_VISCOSE, .fx = APP_FX_ORBIT,
        },
        // Hold = the command wheel; a quick tap is Undo (Cmd+Z; Ctrl on Windows).
        [APP_SLOT_F3] = {
            .kind = APP_ACT_COMMANDS, .label = "UNDO",
            .tap = {CMD, HID_KEY_Z}, .fx = APP_FX_FLASH,
            .feel = HAPTIC_TYPE_SAW, .detents = 12,
        },
        [APP_SLOT_F4] = {
            .kind = APP_ACT_DRAG, .label = "PAN",
            .buttons = MOUSE_BUTTON_MIDDLE, .px_per_rad = PX_PER_RAD, .sign = 1,
            .feel = HAPTIC_TYPE_VISCOSE, .fx = APP_FX_PAN,
        },
    },
    .ring_count = 4,
    .rings = RINGS,
    .search = {{OPT, HID_KEY_C}, 25, 40}, // tool search: Opt+C, type, Enter
    .param_keys = {
        .confirm = {0, HID_KEY_ENTER}, .cancel = {0, HID_KEY_ESCAPE},
        .field = true, .step_mod = {CTRL, 0, SHIFT}, .scroll_sign = 1, .select_all = {CMD, HID_KEY_A},
    },
};
