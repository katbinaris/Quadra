// Figma (design). Standard profile: keyboard shortcuts, the wheel, and a command wheel -- no
// plugin (a Figma plugin talking to the knob is later scope, DEVELOPMENT_PLAN.md). macOS
// modifiers, US key positions.
//
// Knob = zoom (Cmd + wheel). F1: tap undo, turn undo/redo. F2: tap zoom to selection, turn
// = depth (Enter selects children, Shift+Enter the parent). F3: hold = command wheel. F4: turn
// = next / previous frame (N / Shift+N); long-press = menu, as in every profile.
//
// The command wheel is built around auto layout and design-system work, not rare settings:
// commands that are buried in menus or need three-key chords. Two go through Figma's Actions
// search (Cmd+K, type, Enter) because they have no shortcut. Shortcuts are from memory --
// check them against Figma's own shortcut panel (Ctrl+Shift+?).
#include "app_profile.h"
#include "icons/app_icons.h"
#include "class/hid/hid.h"

#define CMD KEYBOARD_MODIFIER_LEFTGUI
#define SHIFT KEYBOARD_MODIFIER_LEFTSHIFT
#define OPT KEYBOARD_MODIFIER_LEFTALT

// --- command cards (DEVELOPMENT_PLAN.md "Figma command wheel", preview round 3) ---
// Canvas pane x 4..60; layers rows on the right. Amber = the selection / what changes.

static const app_scene_t SCENE_ADD_AL = {
    .n_frames = 4,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(650, EL_BOX(8, 6, 8, 14, K_W), EL_BOX(26, 38, 8, 14, K_W), EL_BOX(46, 12, 8, 14, K_W),
                 EL_ROW(0, 0, APP_GLYPH_RECT, 14, K_W, APP_ROW_SEL), EL_ROW(1, 0, APP_GLYPH_RECT, 18, K_W, APP_ROW_SEL),
                 EL_ROW(2, 0, APP_GLYPH_RECT, 12, K_W, APP_ROW_SEL)),
        KEYFRAME(80, EL_BOX(10, 12, 8, 14, K_W), EL_BOX(27, 33, 8, 14, K_W), EL_BOX(45, 16, 8, 14, K_W),
                 EL_ROW(0, 0, APP_GLYPH_RECT, 14, K_W, APP_ROW_SEL), EL_ROW(1, 0, APP_GLYPH_RECT, 18, K_W, APP_ROW_SEL),
                 EL_ROW(2, 0, APP_GLYPH_RECT, 12, K_W, APP_ROW_SEL)),
        KEYFRAME(80, EL_FRAME(9, 19, 47, 24, K_D),
                 EL_BOX(12, 18, 8, 14, K_W), EL_BOX(27, 29, 8, 14, K_W), EL_BOX(43, 20, 8, 14, K_W),
                 EL_ROW(0, 0, APP_GLYPH_RECT, 14, K_W, APP_ROW_SEL), EL_ROW(1, 0, APP_GLYPH_RECT, 18, K_W, APP_ROW_SEL),
                 EL_ROW(2, 0, APP_GLYPH_RECT, 12, K_W, APP_ROW_SEL)),
        KEYFRAME(1400, EL_FRAME(9, 19, 47, 24, K_W),
                 EL_BOX(14, 24, 8, 14, K_W), EL_BOX(28, 24, 8, 14, K_W), EL_BOX(42, 24, 8, 14, K_W),
                 EL_BOX(24, 28, 2, 6, K_A), EL_BOX(38, 28, 2, 6, K_A),
                 EL_ROW(0, 0, APP_GLYPH_AUTO, 18, K_A, 0), EL_ROW(1, 1, APP_GLYPH_RECT, 14, K_G, 0),
                 EL_ROW(2, 1, APP_GLYPH_RECT, 18, K_G, 0), EL_ROW(3, 1, APP_GLYPH_RECT, 12, K_G, 0)),
    },
};

static const app_scene_t SCENE_REMOVE_AL = {
    .n_base = 4,
    .base = EL_LIST(EL_FRAME(9, 19, 47, 24, K_W), EL_ROW(1, 1, APP_GLYPH_RECT, 14, K_G, 0),
                    EL_ROW(2, 1, APP_GLYPH_RECT, 18, K_G, 0), EL_ROW(3, 1, APP_GLYPH_RECT, 12, K_G, 0)),
    .n_frames = 3,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(750, EL_BOX(14, 24, 8, 14, K_W), EL_BOX(28, 24, 8, 14, K_W), EL_BOX(42, 24, 8, 14, K_W),
                 EL_BOX(24, 28, 2, 6, K_G), EL_BOX(38, 28, 2, 6, K_G), EL_ROW(0, 0, APP_GLYPH_AUTO, 18, K_W, APP_ROW_SEL)),
        KEYFRAME(120, EL_BOX(13, 23, 8, 14, K_W), EL_BOX(28, 26, 8, 14, K_W), EL_BOX(42, 22, 8, 14, K_W),
                 EL_BOX(24, 28, 2, 6, K_A), EL_BOX(38, 28, 2, 6, K_A), EL_ROW(0, 0, APP_GLYPH_AUTO, 18, K_W, APP_ROW_SEL)),
        KEYFRAME(1400, EL_BOX(12, 22, 8, 14, K_W), EL_BOX(29, 27, 8, 14, K_W), EL_BOX(43, 21, 8, 14, K_W),
                 EL_ROW(0, 0, APP_GLYPH_FRAME, 18, K_A, 0)),
    },
};

static const app_scene_t SCENE_WRAP = {
    .n_base = 2,
    .base = EL_LIST(EL_BOX(13, 26, 14, 14, K_W), EL_DISC(37, 26, 14, K_W)),
    .n_frames = 3,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(750, EL_SEL(10, 23, 45, 20),
                 EL_ROW(0, 0, APP_GLYPH_RECT, 16, K_W, APP_ROW_SEL), EL_ROW(1, 0, APP_GLYPH_CIRCLE, 12, K_W, APP_ROW_SEL)),
        KEYFRAME(100, EL_FRAME(8, 19, 49, 28, K_G),
                 EL_ROW(0, 0, APP_GLYPH_RECT, 16, K_W, APP_ROW_SEL), EL_ROW(1, 0, APP_GLYPH_CIRCLE, 12, K_W, APP_ROW_SEL)),
        KEYFRAME(1400, EL_FRAME(8, 19, 49, 28, K_A), EL_LABEL(8, 10, APP_GLYPH_FRAME, 14, K_A),
                 EL_ROW(0, 0, APP_GLYPH_FRAME, 14, K_A, 0), EL_ROW(1, 1, APP_GLYPH_RECT, 16, K_G, 0),
                 EL_ROW(2, 1, APP_GLYPH_CIRCLE, 12, K_G, 0)),
    },
};

static const app_scene_t SCENE_GROUP = {
    .n_base = 2,
    .base = EL_LIST(EL_BOX(13, 26, 14, 14, K_W), EL_DISC(37, 26, 14, K_W)),
    .n_frames = 2,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(750, EL_SEL(10, 23, 45, 20),
                 EL_ROW(0, 0, APP_GLYPH_RECT, 16, K_W, APP_ROW_SEL), EL_ROW(1, 0, APP_GLYPH_CIRCLE, 12, K_W, APP_ROW_SEL)),
        KEYFRAME(1400, EL_DASH(9, 22, 47, 22, K_A),
                 EL_ROW(0, 0, APP_GLYPH_GROUP, 14, K_A, 0), EL_ROW(1, 1, APP_GLYPH_RECT, 16, K_G, 0),
                 EL_ROW(2, 1, APP_GLYPH_CIRCLE, 12, K_G, 0)),
    },
};

static const app_scene_t SCENE_COMPONENT = {
    .n_base = 1,
    .base = EL_LIST(EL_BOX(18, 24, 28, 22, K_G)),
    .n_frames = 3,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(750, EL_FRAME(18, 24, 28, 22, K_W), EL_SEL(16, 22, 32, 26), EL_ROW(0, 0, APP_GLYPH_RECT, 18, K_W, APP_ROW_SEL)),
        KEYFRAME(100, EL_FRAME(16, 22, 32, 26, K_A), EL_FRAME(17, 23, 30, 24, K_A), EL_FRAME(18, 24, 28, 22, K_W),
                 EL_ROW(0, 0, APP_GLYPH_RECT, 18, K_W, APP_ROW_SEL)),
        KEYFRAME(1400, EL_FRAME(18, 24, 28, 22, K_A), EL_LABEL(18, 14, APP_GLYPH_COMP, 16, K_A),
                 EL_ROW(0, 0, APP_GLYPH_COMP, 18, K_A, 0)),
    },
};

static const app_scene_t SCENE_DETACH = {
    .n_base = 4,
    .base = EL_LIST(EL_BOX(16, 24, 32, 24, K_G), EL_FRAME(16, 24, 32, 24, K_W), EL_DISC(21, 33, 6, K_W),
                    EL_BOX(30, 35, 12, 2, K_W)),
    .n_frames = 3,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(750, EL_LABEL(16, 14, APP_GLYPH_INST, 16, K_W), EL_ROW(0, 0, APP_GLYPH_INST, 18, K_W, APP_ROW_SEL)),
        KEYFRAME(110, EL_LABEL(16, 14, APP_GLYPH_INST, 16, K_W), EL_LINE(12, 10, 9, 7, K_A), EL_LINE(12, 20, 9, 23, K_A),
                 EL_LINE(42, 11, 46, 8, K_A), EL_ROW(0, 0, APP_GLYPH_INST, 18, K_W, APP_ROW_SEL)),
        KEYFRAME(1400, EL_LABEL(16, 14, APP_GLYPH_FRAME, 16, K_A), EL_ROW(0, 0, APP_GLYPH_FRAME, 18, K_A, 0),
                 EL_ROW(1, 1, APP_GLYPH_CIRCLE, 8, K_G, 0), EL_ROW(2, 1, APP_GLYPH_TEXT, 14, K_G, 0)),
    },
};

// The canvas pans from the instance to its main component (camera steps of ~15px).
#define MAIN_AT(c, last)                                                                            \
    EL_BOX(14 - (c), 26, 26, 20, K_G), EL_FRAME(14 - (c), 26, 26, 20, K_W),                         \
    EL_LABEL(14 - (c), 16, APP_GLYPH_INST, 14, K_W), EL_BOX(72 - (c), 26, 26, 20, K_G),              \
    EL_FRAME(72 - (c), 26, 26, 20, (last) ? K_A : K_W), EL_LABEL(72 - (c), 16, APP_GLYPH_COMP, 14, (last) ? K_A : K_W), \
    EL_ROW(0, 0, APP_GLYPH_COMP, 16, (last) ? K_W : K_G, (last) ? APP_ROW_SEL : 0),                  \
    EL_ROW(1, 0, APP_GLYPH_INST, 16, (c) == 0 ? K_W : K_G, (c) == 0 ? APP_ROW_SEL : 0)
static const app_scene_t SCENE_GO_TO_MAIN = {
    .n_frames = 5,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(650, MAIN_AT(0, 0), EL_SEL(12, 24, 30, 24)),
        KEYFRAME(70, MAIN_AT(15, 0)),
        KEYFRAME(70, MAIN_AT(29, 0)),
        KEYFRAME(70, MAIN_AT(44, 0)),
        KEYFRAME(1400, MAIN_AT(58, 1), EL_SEL(12, 24, 30, 24)),
    },
};

static const app_scene_t SCENE_RESET = {
    .n_base = 1,
    .base = EL_LIST(EL_LABEL(16, 14, APP_GLYPH_INST, 16, K_W)),
    .n_frames = 3,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(750, EL_BOX(16, 24, 32, 24, K_A), EL_FRAME(16, 24, 32, 24, K_W),
                 EL_ROW(0, 0, APP_GLYPH_INST, 18, K_W, APP_ROW_SEL | APP_ROW_DOT)),
        KEYFRAME(110, EL_BOX(16, 24, 32, 24, K_G), EL_FRAME(16, 24, 32, 24, K_W), EL_FRAME(14, 22, 36, 28, K_A),
                 EL_ROW(0, 0, APP_GLYPH_INST, 18, K_W, APP_ROW_SEL | APP_ROW_DOT)),
        KEYFRAME(1400, EL_BOX(16, 24, 32, 24, K_G), EL_FRAME(16, 24, 32, 24, K_W),
                 EL_ROW(0, 0, APP_GLYPH_INST, 18, K_W, APP_ROW_SEL)),
    },
};

// A styled shape (grey fill, 2px white stroke); its fill + stroke travel as two amber swatches.
static const app_scene_t SCENE_COPY_PROPS = {
    .n_base = 5,
    .base = EL_LIST(EL_BOX(10, 22, 26, 24, K_G), EL_FRAME(10, 22, 26, 24, K_W), EL_FRAME(11, 23, 24, 22, K_W),
                    EL_SEL(8, 20, 30, 28), EL_ROW(0, 0, APP_GLYPH_RECT, 18, K_W, APP_ROW_SEL)),
    .n_frames = 4,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(650, EL_CLIPBOARD(44, 10, K_D)),
        KEYFRAME(90, EL_CLIPBOARD(44, 10, K_D), EL_DISC(30, 24, 5, K_A), EL_FRAME(30, 30, 5, 5, K_A)),
        KEYFRAME(90, EL_CLIPBOARD(44, 10, K_D), EL_DISC(40, 18, 5, K_A), EL_FRAME(40, 24, 5, 5, K_A)),
        KEYFRAME(1400, EL_CLIPBOARD(44, 10, K_W), EL_DISC(48, 15, 5, K_A), EL_FRAME(48, 21, 5, 5, K_A)),
    },
};

static const app_scene_t SCENE_PASTE_PROPS = {
    .n_base = 2,
    .base = EL_LIST(EL_CLIPBOARD(44, 10, K_W), EL_ROW(0, 0, APP_GLYPH_RECT, 18, K_W, APP_ROW_SEL)),
    .n_frames = 5,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(650, EL_FRAME(10, 22, 26, 24, K_D), EL_SEL(8, 20, 30, 28), EL_DISC(48, 15, 5, K_A), EL_FRAME(48, 21, 5, 5, K_A)),
        KEYFRAME(90, EL_FRAME(10, 22, 26, 24, K_D), EL_SEL(8, 20, 30, 28), EL_DISC(38, 21, 5, K_A), EL_FRAME(38, 27, 5, 5, K_A)),
        KEYFRAME(90, EL_FRAME(10, 22, 26, 24, K_D), EL_SEL(8, 20, 30, 28), EL_DISC(27, 27, 5, K_A), EL_FRAME(27, 33, 5, 5, K_A)),
        KEYFRAME(130, EL_FRAME(10, 22, 26, 24, K_A), EL_FRAME(11, 23, 24, 22, K_A), EL_SEL(8, 20, 30, 28)),
        KEYFRAME(1400, EL_BOX(10, 22, 26, 24, K_G), EL_FRAME(10, 22, 26, 24, K_W), EL_FRAME(11, 23, 24, 22, K_W),
                 EL_SEL(8, 20, 30, 28)),
    },
};

static const app_scene_t SCENE_RENAME = {
    .n_base = 2,
    .base = EL_LIST(EL_BOX(14, 26, 36, 22, K_G), EL_FRAME(14, 26, 36, 22, K_W)),
    .n_frames = 6,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(650, EL_LABEL(14, 16, APP_GLYPH_FRAME, 26, K_G), EL_ROW(0, 0, APP_GLYPH_FRAME, 26, K_W, APP_ROW_SEL)),
        KEYFRAME(380, EL_LABEL(14, 16, APP_GLYPH_FRAME, 26, K_G), EL_ROW(0, 0, APP_GLYPH_FRAME, 26, K_W, APP_ROW_SEL | APP_ROW_ALL)),
        KEYFRAME(260, EL_LABEL(14, 16, APP_GLYPH_FRAME, 26, K_G),
                 EL_ROW(0, 0, APP_GLYPH_FRAME, 0, K_W, APP_ROW_SEL | APP_ROW_FIELD | APP_ROW_CURSOR)),
        KEYFRAME(420, EL_LABEL(14, 16, APP_GLYPH_FRAME, 26, K_G),
                 EL_ROW(0, 0, APP_GLYPH_FRAME, 14, K_W, APP_ROW_SEL | APP_ROW_FIELD | APP_ROW_CURSOR)),
        KEYFRAME(260, EL_LABEL(14, 16, APP_GLYPH_FRAME, 26, K_G), EL_ROW(0, 0, APP_GLYPH_FRAME, 14, K_W, APP_ROW_SEL | APP_ROW_FIELD)),
        KEYFRAME(1400, EL_LABEL(14, 16, APP_GLYPH_FRAME, 14, K_A), EL_ROW(0, 0, APP_GLYPH_FRAME, 14, K_W, APP_ROW_SEL)),
    },
};

// Messy shapes -> the plugin window runs -> the shapes come out tidied.
#define PLUGIN_MESS EL_BOX(9, 12, 12, 9, K_W), EL_BOX(30, 34, 16, 11, K_W), EL_BOX(44, 14, 10, 13, K_W)
static const app_scene_t SCENE_PLUGIN = {
    .n_base = 3,
    .base = EL_LIST(EL_ROW(0, 0, APP_GLYPH_RECT, 14, K_G, 0), EL_ROW(1, 0, APP_GLYPH_RECT, 18, K_G, 0),
                    EL_ROW(2, 0, APP_GLYPH_RECT, 12, K_G, 0)),
    .n_frames = 5,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(650, PLUGIN_MESS),
        KEYFRAME(80, PLUGIN_MESS, EL_BOX(20, 20, 26, 22, K_B), EL_FRAME(20, 20, 26, 22, K_W), EL_BOX(21, 21, 24, 5, K_D)),
        KEYFRAME(600, PLUGIN_MESS, EL_BOX(12, 12, 42, 36, K_B), EL_FRAME(12, 12, 42, 36, K_W), EL_BOX(13, 13, 40, 5, K_D),
                 EL_PLAY(25, 23, 16)),
        KEYFRAME(90, EL_BOX(10, 19, 12, 11, K_W), EL_BOX(28, 30, 14, 12, K_W), EL_BOX(43, 20, 11, 13, K_W)),
        KEYFRAME(1400, EL_BOX(10, 26, 12, 12, K_W), EL_BOX(26, 26, 12, 12, K_W), EL_BOX(42, 26, 12, 12, K_W),
                 EL_LINE(16, 19, 16, 23, K_A), EL_LINE(14, 21, 18, 21, K_A), EL_LINE(32, 19, 32, 23, K_A),
                 EL_LINE(30, 21, 34, 21, K_A), EL_LINE(48, 19, 48, 23, K_A), EL_LINE(46, 21, 50, 21, K_A)),
    },
};

// ALIGN: three layers pull to a shared edge / centre line (amber guide). Horizontal aligns use
// bars stacked vertically, vertical aligns bars side by side.
#define ALIGN_ROWS EL_ROW(0, 0, APP_GLYPH_RECT, 14, K_W, APP_ROW_SEL), EL_ROW(1, 0, APP_GLYPH_RECT, 18, K_W, APP_ROW_SEL), \
                   EL_ROW(2, 0, APP_GLYPH_RECT, 12, K_W, APP_ROW_SEL)
#define H_BARS(x0, x1, x2) EL_BOX(x0, 12, 30, 8, K_W), EL_BOX(x1, 26, 18, 8, K_W), EL_BOX(x2, 40, 24, 8, K_W)
#define V_BARS(y0, y1, y2) EL_BOX(12, y0, 8, 30, K_W), EL_BOX(27, y1, 8, 18, K_W), EL_BOX(42, y2, 8, 24, K_W)
#define ALIGN_SCENE(NAME, START, MID, END, GUIDE)                                                   \
    static const app_scene_t NAME = {                                                               \
        .n_base = 3, .base = EL_LIST(ALIGN_ROWS), .n_frames = 3,                                    \
        .frames = (const app_keyframe_t[]){KEYFRAME(700, START), KEYFRAME(90, MID), KEYFRAME(1300, GUIDE, END)}, \
    }
ALIGN_SCENE(SCENE_ALIGN_LEFT, H_BARS(16, 32, 22), H_BARS(12, 20, 15), H_BARS(10, 10, 10), EL_BOX(8, 8, 1, 44, K_A));
ALIGN_SCENE(SCENE_ALIGN_HCENTER, H_BARS(10, 36, 14), H_BARS(14, 28, 17), H_BARS(17, 23, 20), EL_BOX(32, 8, 1, 44, K_A));
ALIGN_SCENE(SCENE_ALIGN_RIGHT, H_BARS(16, 22, 12), H_BARS(21, 30, 22), H_BARS(24, 36, 30), EL_BOX(55, 8, 1, 44, K_A));
ALIGN_SCENE(SCENE_ALIGN_TOP, V_BARS(18, 34, 22), V_BARS(13, 20, 15), V_BARS(10, 10, 10), EL_BOX(8, 8, 48, 1, K_A));
ALIGN_SCENE(SCENE_ALIGN_VCENTER, V_BARS(10, 36, 14), V_BARS(14, 28, 17), V_BARS(17, 23, 20), EL_BOX(8, 32, 48, 1, K_A));
ALIGN_SCENE(SCENE_ALIGN_BOTTOM, V_BARS(18, 22, 12), V_BARS(22, 30, 22), V_BARS(24, 36, 30), EL_BOX(8, 55, 48, 1, K_A));

// Tidy up: a loose 2x2 arrangement snaps to an even grid, with amber spacing marks.
static const app_scene_t SCENE_TIDY = {
    .n_base = 4,
    .base = EL_LIST(EL_ROW(0, 0, APP_GLYPH_RECT, 14, K_W, APP_ROW_SEL), EL_ROW(1, 0, APP_GLYPH_RECT, 18, K_W, APP_ROW_SEL),
                    EL_ROW(2, 0, APP_GLYPH_RECT, 12, K_W, APP_ROW_SEL), EL_ROW(3, 0, APP_GLYPH_RECT, 16, K_W, APP_ROW_SEL)),
    .n_frames = 3,
    .frames = (const app_keyframe_t[]){
        KEYFRAME(700, EL_BOX(10, 9, 16, 14, K_W), EL_BOX(33, 13, 16, 14, K_W), EL_BOX(13, 33, 16, 14, K_W), EL_BOX(38, 37, 16, 14, K_W)),
        KEYFRAME(90, EL_BOX(12, 11, 16, 14, K_W), EL_BOX(34, 12, 16, 14, K_W), EL_BOX(13, 34, 16, 14, K_W), EL_BOX(36, 36, 16, 14, K_W)),
        KEYFRAME(1300, EL_BOX(13, 12, 16, 14, K_W), EL_BOX(35, 12, 16, 14, K_W), EL_BOX(13, 34, 16, 14, K_W), EL_BOX(35, 34, 16, 14, K_W),
                 EL_BOX(30, 17, 4, 2, K_A), EL_BOX(30, 39, 4, 2, K_A), EL_BOX(20, 27, 2, 6, K_A), EL_BOX(42, 27, 2, 6, K_A)),
    },
};

static const app_cmd_t ALIGN[] = {
    {"ALIGN LEFT", APP_CMD_KEYS, {OPT, HID_KEY_A}, NULL, &SCENE_ALIGN_LEFT, NULL, 0},
    {"ALIGN CENTER", APP_CMD_KEYS, {OPT, HID_KEY_H}, NULL, &SCENE_ALIGN_HCENTER, NULL, 0},
    {"ALIGN RIGHT", APP_CMD_KEYS, {OPT, HID_KEY_D}, NULL, &SCENE_ALIGN_RIGHT, NULL, 0},
    {"ALIGN TOP", APP_CMD_KEYS, {OPT, HID_KEY_W}, NULL, &SCENE_ALIGN_TOP, NULL, 0},
    {"ALIGN MIDDLE", APP_CMD_KEYS, {OPT, HID_KEY_V}, NULL, &SCENE_ALIGN_VCENTER, NULL, 0},
    {"ALIGN BOTTOM", APP_CMD_KEYS, {OPT, HID_KEY_S}, NULL, &SCENE_ALIGN_BOTTOM, NULL, 0},
    {"TIDY UP", APP_CMD_KEYS, {KEYBOARD_MODIFIER_LEFTCTRL | OPT, HID_KEY_T}, NULL, &SCENE_TIDY, NULL, 0},
};

static const app_cmd_t STRUCTURE[] = {
    {"ADD AUTO LAYOUT", APP_CMD_KEYS, {SHIFT, HID_KEY_A}, NULL, &SCENE_ADD_AL, NULL, 0},
    {"REMOVE AUTO LAYOUT", APP_CMD_KEYS, {OPT | SHIFT, HID_KEY_A}, NULL, &SCENE_REMOVE_AL, NULL, 0},
    {"WRAP IN FRAME", APP_CMD_KEYS, {OPT | CMD, HID_KEY_G}, NULL, &SCENE_WRAP, NULL, 0},
    {"GROUP", APP_CMD_KEYS, {CMD, HID_KEY_G}, NULL, &SCENE_GROUP, NULL, 0},
};
static const app_cmd_t COMPONENTS[] = {
    {"CREATE COMPONENT", APP_CMD_KEYS, {OPT | CMD, HID_KEY_K}, NULL, &SCENE_COMPONENT, NULL, 0},
    {"DETACH INSTANCE", APP_CMD_KEYS, {OPT | CMD, HID_KEY_B}, NULL, &SCENE_DETACH, NULL, 0},
    {"GO TO MAIN", APP_CMD_ACTIONS, {0, 0}, "go to main component", &SCENE_GO_TO_MAIN, NULL, 0},
    {"RESET INSTANCE", APP_CMD_ACTIONS, {0, 0}, "reset all changes", &SCENE_RESET, NULL, 0},
};
static const app_cmd_t UTILITY[] = {
    {"COPY PROPERTIES", APP_CMD_KEYS, {OPT | CMD, HID_KEY_C}, NULL, &SCENE_COPY_PROPS, NULL, 0},
    {"PASTE PROPERTIES", APP_CMD_KEYS, {OPT | CMD, HID_KEY_V}, NULL, &SCENE_PASTE_PROPS, NULL, 0},
    {"RENAME", APP_CMD_KEYS, {CMD, HID_KEY_R}, NULL, &SCENE_RENAME, NULL, 0},
    {"RUN LAST PLUGIN", APP_CMD_KEYS, {OPT | CMD, HID_KEY_P}, NULL, &SCENE_PLUGIN, NULL, 0},
};
// F1 steps between the two layout rings (STRUCTURE <-> ALIGN).
static const app_ring_t RINGS[] = {
    {"STRUCTURE", "BUILD", APP_SLOT_F1, 4, STRUCTURE},
    {"ALIGN", "ALIGN", APP_SLOT_F1, 7, ALIGN},
    {"COMPONENTS", "COMP", APP_SLOT_F2, 4, COMPONENTS},
    {"UTILITY", "UTIL", APP_SLOT_F4, 4, UTILITY},
};

const app_profile_t app_profile_figma = {
    .version = APP_PROFILE_VERSION,
    .id = "figma",
    .name = "FIGMA",
    .icon24 = app_icon_figma_24,
    .icon48 = app_icon_figma_48,
    .legend = {"UNDO", "DEPTH", "WHEEL", "FRAME"},
    // Figma's purple -> blue -> green: neighbouring hues, brighter toward the icon (red
    // against blue clashed on hardware).
    .accents = {0xA259FF, 0x1ABCFE, 0x0ACF83},
    .slot = {
        // Wheel up with Cmd = zoom in. Flip `sign` if the knob zooms the wrong way.
        [APP_SLOT_KNOB] = {
            .kind = APP_ACT_WHEEL, .label = "ZOOM", .modifier = CMD, .sign = 1,
            .feel = HAPTIC_TYPE_SAW, .detents = 16,
        },
        [APP_SLOT_F1] = {
            .kind = APP_ACT_KEYS, .label = "UNDO/REDO",
            .cw = {CMD | SHIFT, HID_KEY_Z}, .ccw = {CMD, HID_KEY_Z}, .tap = {CMD, HID_KEY_Z},
            .feel = HAPTIC_TYPE_SAW, .detents = 12,
        },
        // Walk the layer tree: clockwise into the children, counter-clockwise up to the parent.
        [APP_SLOT_F2] = {
            .kind = APP_ACT_KEYS, .label = "DEPTH",
            .cw = {0, HID_KEY_ENTER}, .ccw = {SHIFT, HID_KEY_ENTER}, .tap = {SHIFT, HID_KEY_2},
            .feel = HAPTIC_TYPE_SAW, .detents = 12,
        },
        [APP_SLOT_F3] = {
            .kind = APP_ACT_COMMANDS, .label = "COMMANDS",
            .feel = HAPTIC_TYPE_SAW, .detents = 12,
        },
        [APP_SLOT_F4] = {
            .kind = APP_ACT_KEYS, .label = "FRAMES",
            .cw = {0, HID_KEY_N}, .ccw = {SHIFT, HID_KEY_N},
            .feel = HAPTIC_TYPE_SAW, .detents = 12,
        },
    },
    .ring_count = 4,
    .rings = RINGS,
    .search = {{CMD, HID_KEY_K}, 25, 40},
};
