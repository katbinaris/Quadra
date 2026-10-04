// AGENTS: Claude Code, Codex and Cursor from the knob. Approvals don't live here -- they come
// up over any profile (notify.h, tools/agents/) -- this is the everyday driving: scroll the
// output, stop a turn, switch modes, walk the prompt history, and a wheel of each tool's
// commands. Keys verified against Claude Code 2.1.287, Codex CLI 0.160 and Cursor 3.19
// (2026-10).
//
// Macros type with US key positions (HID sends positions, not characters): with a non-Latin
// input source active they type garbage, so none of them press Enter -- what got typed is on
// screen, and a tap on F1 sends it.
#include "app_profile.h"
#include "icons/app_icons.h"
#include "class/hid/hid.h"

#define CMD KEYBOARD_MODIFIER_LEFTGUI
#define CTRL KEYBOARD_MODIFIER_LEFTCTRL
#define SHIFT KEYBOARD_MODIFIER_LEFTSHIFT
#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

// No scenes: the wheel draws these commands from themselves -- a little terminal typing a
// macro's text, or a shortcut's chord on big keycaps (ui_cards.cpp).

// --- macros (1-based, in this order) ---
#define TYPE(s) {APP_MSTEP_TEXT, {0, 0}, 0, s}
#define KEY(m, k) {APP_MSTEP_KEY, {m, k}, 0, NULL}
enum {
    M_CLEAR = 1, M_COMPACT, M_MODEL, M_REVIEW, M_PLAN, M_CONTEXT, M_USAGE, M_CONTINUE, M_REWIND,
    M_NEW, M_PERMISSIONS, M_DIFF, M_STATUS,
};
static const app_mstep_t S_CLEAR[] = {TYPE("/clear")};
static const app_mstep_t S_COMPACT[] = {TYPE("/compact")};
static const app_mstep_t S_MODEL[] = {TYPE("/model")};
static const app_mstep_t S_REVIEW[] = {TYPE("/review")};
static const app_mstep_t S_PLAN[] = {TYPE("/plan")};
static const app_mstep_t S_CONTEXT[] = {TYPE("/context")};
static const app_mstep_t S_USAGE[] = {TYPE("/usage")};
static const app_mstep_t S_CONTINUE[] = {TYPE("continue")};
static const app_mstep_t S_REWIND[] = {KEY(0, HID_KEY_ESCAPE), KEY(0, HID_KEY_ESCAPE)}; // on an empty prompt
static const app_mstep_t S_NEW[] = {TYPE("/new")};
static const app_mstep_t S_PERMISSIONS[] = {TYPE("/permissions")};
static const app_mstep_t S_DIFF[] = {TYPE("/diff")};
static const app_mstep_t S_STATUS[] = {TYPE("/status")};
static const app_macro_t MACROS[] = {
    {"CLEAR", N(S_CLEAR), S_CLEAR},       {"COMPACT", N(S_COMPACT), S_COMPACT},
    {"MODEL", N(S_MODEL), S_MODEL},       {"REVIEW", N(S_REVIEW), S_REVIEW},
    {"PLAN", N(S_PLAN), S_PLAN},          {"CONTEXT", N(S_CONTEXT), S_CONTEXT},
    {"USAGE", N(S_USAGE), S_USAGE},       {"CONTINUE", N(S_CONTINUE), S_CONTINUE},
    {"REWIND", N(S_REWIND), S_REWIND},    {"NEW", N(S_NEW), S_NEW},
    {"PERMISSIONS", N(S_PERMISSIONS), S_PERMISSIONS}, {"DIFF", N(S_DIFF), S_DIFF},
    {"STATUS", N(S_STATUS), S_STATUS},
};

// --- the wheel: hold F1; while it's open F2 jumps to Codex, F3 to Cursor ---
#define MACRO_CMD(name, m) {name, APP_CMD_MACRO, {0, 0}, NULL, NULL, NULL, m}
#define KEYS_CMD(name, mod, key) {name, APP_CMD_KEYS, {mod, key}, NULL, NULL, NULL, 0}
static const app_cmd_t CLAUDE[] = {
    MACRO_CMD("CLEAR", M_CLEAR),       MACRO_CMD("COMPACT", M_COMPACT), MACRO_CMD("REWIND", M_REWIND),
    MACRO_CMD("PLAN", M_PLAN),         MACRO_CMD("MODEL", M_MODEL),     MACRO_CMD("CONTEXT", M_CONTEXT),
    MACRO_CMD("USAGE", M_USAGE),       MACRO_CMD("REVIEW", M_REVIEW),   MACRO_CMD("CONTINUE", M_CONTINUE),
    KEYS_CMD("TRANSCRIPT", CTRL, HID_KEY_O), KEYS_CMD("BACKGROUND", CTRL, HID_KEY_B),
};
static const app_cmd_t CODEX[] = {
    MACRO_CMD("NEW", M_NEW),           MACRO_CMD("COMPACT", M_COMPACT), MACRO_CMD("MODEL", M_MODEL),
    MACRO_CMD("PERMISSIONS", M_PERMISSIONS), MACRO_CMD("REVIEW", M_REVIEW), MACRO_CMD("DIFF", M_DIFF),
    MACRO_CMD("STATUS", M_STATUS),     MACRO_CMD("PLAN", M_PLAN),       KEYS_CMD("TRANSCRIPT", CTRL, HID_KEY_T),
};
// Cursor's chords mean different things by context (Cmd+Enter also sends a queued message,
// Cmd+Shift+Backspace also stops the agent) -- named for what they do with changes waiting.
static const app_cmd_t CURSOR[] = {
    KEYS_CMD("AGENT PANE", CMD, HID_KEY_I),      KEYS_CMD("NEW CHAT", CMD, HID_KEY_R),
    KEYS_CMD("MODES", CMD, HID_KEY_PERIOD),      KEYS_CMD("ACCEPT ALL", CMD, HID_KEY_ENTER),
    KEYS_CMD("REJECT ALL", CMD | SHIFT, HID_KEY_BACKSPACE), KEYS_CMD("INLINE EDIT", CMD, HID_KEY_K),
};
static const app_ring_t RINGS[] = {
    {"CLAUDE CODE", "CLAUDE", APP_SLOT_F1, N(CLAUDE), CLAUDE},
    {"CODEX", "CODEX", APP_SLOT_F2, N(CODEX), CODEX},
    {"CURSOR", "CURSOR", APP_SLOT_F3, N(CURSOR), CURSOR},
};

const app_profile_t app_profile_agents = {
    .version = APP_PROFILE_VERSION,
    .id = "agents",
    .name = "AGENTS",
    .icon24 = app_icon_agents_24,
    .icon48 = app_icon_agents_48,
    .legend = {"ENTER", "ESC", "MODE", "MENU"},
    .accents = {0x3A1A12, 0xE8825F, 0xFFD2BF},
    .slot = {
        [APP_SLOT_KNOB] = {
            .kind = APP_ACT_WHEEL, .label = "SCROLL", .sign = 1,
            .feel = HAPTIC_TYPE_SAW,
        },
        // Tap: Enter -- sends what a macro typed, or takes the highlighted answer of a prompt.
        // Hold: the command wheel.
        [APP_SLOT_F1] = {
            .kind = APP_ACT_COMMANDS, .label = "COMMANDS", .tap = {0, HID_KEY_ENTER},
            .feel = HAPTIC_TYPE_SAW, .detents = 12,
        },
        // Tap: Esc -- stops the turn (and declines a prompt). Hold + turn: the prompt history.
        [APP_SLOT_F2] = {
            .kind = APP_ACT_KEYS, .label = "HISTORY",
            .cw = {0, HID_KEY_ARROW_DOWN}, .ccw = {0, HID_KEY_ARROW_UP}, .tap = {0, HID_KEY_ESCAPE},
            .feel = HAPTIC_TYPE_SAW, .detents = 12,
        },
        // Shift+Tab: Claude Code's permission modes, Codex's Plan toggle, Cursor's modes.
        [APP_SLOT_F3] = {.kind = APP_ACT_TAP, .label = "MODE", .cw = {SHIFT, HID_KEY_TAB}},
    },
    .ring_count = N(RINGS),
    .rings = RINGS,
    .macro_count = N(MACROS),
    .macros = MACROS,
};
