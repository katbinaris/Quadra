// Host preview of the Pixel UI: renders the firmware's own ui_screens / ui_fx code (built
// against the stubs in stub/) into one contact sheet, each screen masked to the round panel
// and shown at 2x. Build and run with tools/ui_preview/run.sh.
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "lgfx_config.hpp"
#include "ui_gfx.hpp"
#include "ui_fx.hpp"
#include "ui_screens.hpp"
#include "ui_extras.hpp"
#include "ui_vinyl.hpp"
extern "C" {
#include "user_prefs.h"
}
#include "icons/app_icons.h"
extern "C" {
#include "ui_state.h"
#include "app_profiles/app_profile.h"
extern const app_profile_t app_profile_figma;
extern const app_profile_t app_profile_plasticity;
extern const app_profile_t app_profile_onshape;
extern const app_profile_t app_profile_agents;
}

static LGFX_Sprite g;

struct Tile {
    const char *name;
    std::vector<uint32_t> px;
};
static std::vector<Tile> tiles;

static void keep(const char *name) {
    tiles.push_back({name, std::vector<uint32_t>(g.px, g.px + 240 * 240)});
    g.clear();
}

static menu_render_row_t row(const char *label, const char *caption, const char *value, bool sel) {
    menu_render_row_t r = {};
    snprintf(r.label, sizeof(r.label), "%s", label);
    snprintf(r.caption, sizeof(r.caption), "%s", caption);
    snprintf(r.value, sizeof(r.value), "%s", value);
    r.selected = sel;
    return r;
}

static menu_render_snapshot_t haptic_snap(int selected, bool editing, bool dirty) {
    menu_render_snapshot_t s = {};
    s.open = true;
    s.screen = MENU_SCREEN_HAPTIC;
    s.editing = editing;
    s.dirty = dirty;
    s.selected = selected;
    const char *L[8][3] = {{"STEPS", "PROFILE", "COARSE"}, {"SNAP", "KP", "6.00"}, {"DAMP", "KD", ".010"},
                           {"SHAPE", "RAMP", "40%"}, {"FEEL", "TYPE", "VISCOSE"}, {"AMP", "AMPLITUDE", "80%"},
                           {"PITCH", "CLICK", "1.00X"}, {"CLICK", "WAVE", "SQR 4C"}};
    for (int i = 0; i < 8; i++) s.rows[i] = row(L[i][0], L[i][1], L[i][2], i == selected);
    s.row_count = 8;
    return s;
}

int main() {
    ui::bind(&g);
    ui::fx_init();

    // Hand-drawn sprites: the row string must be exactly w*h chars.
    const ui::Sprite *all[] = {&ui::SPR_USB, &ui::SPR_SPK, &ui::SPR_KBD, &ui::SPR_MOUSE, &ui::SPR_NOTE, &ui::SPR_TERM, &ui::SPR_CUBE, &ui::SPR_CUBE_M,
                               &ui::SPR_TRI_L, &ui::SPR_TRI_R, &ui::SPR_STEPS, &ui::SPR_SNAP, &ui::SPR_DAMP,
                               &ui::SPR_PITCH, &ui::SPR_USB_M, &ui::SPR_SPK_M, &ui::SPR_KBD_M, &ui::SPR_MOUSE_M,
                               &ui::SPR_NOTE_M, &ui::SPR_TERM_M, &ui::SPR_TRI_L_M, &ui::SPR_TRI_R_M,
                               &ui::SPR_STEPS_M, &ui::SPR_SNAP_M, &ui::SPR_DAMP_M, &ui::SPR_SHAPE_M, &ui::SPR_PITCH_M,
                               &ui::SPR_BULB, &ui::SPR_BULB_LIT, &ui::SPR_BULB_M, &ui::SPR_BULB_LIT_M,
                               &ui::SPR_LOGO_KORG, &ui::SPR_LOGO_ROLAND};
    for (const ui::Sprite *s : all) {
        if (strlen(s->rows) != (size_t)s->w * s->h) {
            fprintf(stderr, "sprite %dx%d has %zu chars\n", s->w, s->h, strlen(s->rows));
            return 1;
        }
    }

    for (uint32_t e : {300u, 800u, 1400u, 2600u}) {
        ui::fx_boot(e);
        static char n[32];
        keep(strdup((snprintf(n, sizeof(n), "boot %ums", e), n)));
    }

    ui::draw_main({false, 6, MENU_HID_MOUSE, HAPTIC_TYPE_SINE, 0, nullptr});
    keep("main mouse");
    ui::draw_main({true, 3, MENU_HID_KEYBOARD, HAPTIC_TYPE_VISCOSE, UI_BTN_F4, nullptr});
    keep("main keyboard, F4 held");
    ui::AppView figma = {"FIGMA", app_icon_figma_24, {"UNDO", "DEPTH", "WHEEL", "FRAME"}, "ZOOM", "KNOB"};
    ui::draw_main({false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, 0, nullptr, &figma});
    keep("main APP figma");
    ui::AppView figma_f1 = figma;
    figma_f1.action = "UNDO/REDO";
    figma_f1.action_via = "F1 + KNOB";
    ui::draw_main({false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, UI_BTN_F1, nullptr, &figma_f1});
    keep("main APP figma, F1 held");
    // Plasticity's micro-interaction: an isometric pyramid in each scene.
    ui::ShapeView sv = {ui::SHAPE_PYRAMID, ui::STYLE_THICK, ui::SCENE_ZOOM, (float)M_PI / 4, 0.375f, 0, false};
    ui::AppView plast = {"PLASTICITY", app_icon_plasticity_24, {"ZOOM", "ORBIT", "WHEEL", "PAN"}, "ZOOM", "KNOB", "KNOB", &sv, false};
    ui::draw_main({false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, 0, nullptr, &plast});
    keep("plasticity ZOOM");
    sv.scene = ui::SCENE_ORBIT;
    plast.action = "ORBIT";
    plast.action_key = "F2";
    ui::draw_main({false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, UI_BTN_F2, nullptr, &plast});
    keep("plasticity ORBIT at rest (45 deg)");
    sv.yaw = (float)M_PI / 4 + 2 * (float)M_PI / 32 * 3; // a stepped pose
    ui::draw_main({false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, UI_BTN_F2, nullptr, &plast});
    keep("plasticity ORBIT turning");
    sv.yaw = (float)M_PI / 4;
    sv.scene = ui::SCENE_PAN;
    sv.pan = 20;
    plast.action = "PAN";
    plast.action_key = "F4";
    ui::draw_main({false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, UI_BTN_F4, nullptr, &plast});
    keep("plasticity PAN");
    // Uploaded icon: raw RGB565 BE written by run.sh from tools/icons/figma_pixel_48.png.
    if (FILE *f = fopen(getenv("ICON_RAW") ? getenv("ICON_RAW") : "", "rb")) {
        static uint8_t icon[48 * 48 * 2];
        size_t n = fread(icon, 1, sizeof(icon), f);
        fclose(f);
        if (n == sizeof(icon)) {
            ui::draw_main({false, 6, MENU_HID_MOUSE, HAPTIC_TYPE_SINE, 0, icon});
            keep("main + uploaded icon");
        }
    }

    menu_render_snapshot_t root = {};
    root.open = true;
    root.screen = MENU_SCREEN_ROOT;
    root.selected = 0;
    root.rows[0] = row("PROFILES", "", "", true);
    root.rows[1] = row("HAPTICS", "", "", false);
    root.rows[2] = row("DISPLAY", "", "", false);
    root.rows[3] = row("LIGHTS", "", "", false);
    root.rows[4] = row("BOOT MODE", "", "", false);
    root.rows[5] = row("DEVICE", "", "", false);
    root.row_count = 6;
    ui::draw_menu_list(root, 0);
    keep("menu");

    ui::draw_orbit(haptic_snap(MENU_HAPTIC_ROW_FEEL, true, true), {HAPTIC_TYPE_SINE, 700, -1, 1, true, 12, 6});
    keep("orbit FEEL editing");
    ui::set_saw_shape(0.9f);
    ui::draw_orbit(haptic_snap(MENU_HAPTIC_ROW_SHAPE, true, false), {HAPTIC_TYPE_SAW, 0, -1, 1, true, 12, 6});
    ui::set_saw_shape(0.0f);
    keep("orbit SHAPE editing");
    ui::draw_orbit(haptic_snap(MENU_HAPTIC_ROW_AMP, true, true), {HAPTIC_TYPE_SAW, 0, -1, 1, true, 12, 6});
    keep("orbit AMP editing");
    ui::draw_orbit(haptic_snap(MENU_HAPTIC_ROW_STEPS, true, false), {HAPTIC_TYPE_SAW, 900, -1, 1, true, 12, 6});
    keep("orbit STEPS editing");
    // CLICK: each of the shapes, the dot part way along.
    for (int k = 0; k < SNDCAL_CLICK_COUNT; k++) {
        menu_render_snapshot_t m = haptic_snap(MENU_HAPTIC_ROW_CLICK, true, k == 0);
        snprintf(m.rows[MENU_HAPTIC_ROW_CLICK].value, sizeof(m.rows[0].value), "%s", MOTOR_SOUND_SHAPE_SHORT[k]);
        ui::draw_orbit(m, {HAPTIC_TYPE_SAW, 250, -1, 1, true, 12, k});
        char n[32];
        snprintf(n, sizeof(n), "orbit CLICK %s", MOTOR_SOUND_SHAPE_NAME[k]);
        keep(strdup(n));
    }
    {
        // SMOOTH: VISCOSE only -- SNAP, SHAPE and FEEL are muted.
        menu_render_snapshot_t m = haptic_snap(MENU_HAPTIC_ROW_STEPS, true, false);
        snprintf(m.rows[MENU_HAPTIC_ROW_STEPS].value, sizeof(m.rows[0].value), "SMOOTH");
        for (int r : {MENU_HAPTIC_ROW_SNAP, MENU_HAPTIC_ROW_SHAPE}) {
            m.rows[r].muted = true;
            snprintf(m.rows[r].value, sizeof(m.rows[r].value), "--");
        }
        m.rows[MENU_HAPTIC_ROW_FEEL].muted = true;
        snprintf(m.rows[MENU_HAPTIC_ROW_AMP].value, sizeof(m.rows[0].value), "0%%");
        ui::draw_orbit(m, {HAPTIC_TYPE_VISCOSE, 900, -1, 1, true, 0, 6});
        keep("orbit SMOOTH");
        ui::draw_saved_toast("FACTORY");
        keep("orbit FACTORY toast");
    }
    ui::draw_orbit(haptic_snap(MENU_HAPTIC_ROW_STEPS, false, false), {HAPTIC_TYPE_SAW, 0, -1, 1, true, 12, 6});
    ui::draw_saved_toast();
    keep("orbit + SAVED!");

    menu_render_snapshot_t hid = {};
    hid.open = true;
    hid.screen = MENU_SCREEN_HID;
    hid.selected = 0;
    hid.rows[0] = row("PROFILES", "", "KEYBOARD", true);
    hid.rows[1] = row("HAPTIC", "", "COARSE", false); // KEYBOARD / MOUSE: the mode's haptic profile
    hid.row_count = 2;
    ui::draw_hid(hid, {MENU_HID_KEYBOARD, 0, true});
    keep("hid keyboard");
    hid.selected = 1;
    hid.rows[0] = row("PROFILES", "", "MOUSE", false);
    hid.rows[1] = row("HAPTIC", "", "MEDIUM", true);
    ui::draw_hid(hid, {MENU_HID_MOUSE, 0, true});
    keep("hid mouse, HAPTIC");
    hid.selected = 0;
    hid.row_count = 1;
    hid.rows[0] = row("PROFILES", "", "APP", true);
    ui::draw_hid(hid, {MENU_HID_APP, 0, true, "FIGMA", app_icon_figma_24});
    keep("hid APP");
    hid.rows[0] = row("PROFILES", "", "HOME", true);
    ui::draw_hid(hid, {MENU_HID_HOME, 0, true});
    keep("hid HOME");

    // HOME (home.h): the user's lamps, made up here.
    {
        static home_snapshot_t hs;
        auto lamp = [](const char *name, uint8_t kind, uint8_t caps, bool online, bool on, int bright, int ct, int lo, int hi, int hue,
                       uint32_t rgb) {
            home_lamp_view_t l = {};
            snprintf(l.name, sizeof(l.name), "%s", name);
            l.kind = kind;
            l.caps = caps;
            l.online = online;
            l.known = online;
            l.on = on;
            l.bright = (uint8_t)bright;
            l.ct = (uint16_t)ct;
            l.ct_min = (uint16_t)lo;
            l.ct_max = (uint16_t)hi;
            l.hue = (uint16_t)hue;
            l.rgb = rgb;
            return l;
        };
        hs.count = 4;
        hs.lamps[0] = lamp("LED DESK LAMP 2", HOME_KIND_DESK, HOME_CAP_BRIGHT | HOME_CAP_TEMP, true, true, 64, 2700, 2700, 5100, 0, 0xFFA757);
        hs.lamps[1] = lamp("SMART LIGHTSTRIP", HOME_KIND_STRIP, HOME_CAP_BRIGHT | HOME_CAP_COLOR, true, true, 76, 0, 0, 0, 200, 0x00AAFF);
        hs.lamps[2] = lamp("DESK LAMP", HOME_KIND_DESK_ARM, HOME_CAP_BRIGHT | HOME_CAP_TEMP, true, false, 1, 2600, 2600, 5000, 0, 0xFFA04F);
        hs.lamps[3] = lamp("SMART LED LEFT", HOME_KIND_BULB, HOME_CAP_BRIGHT | HOME_CAP_TEMP | HOME_CAP_COLOR, false, false, 50, 4000,
                           1700, 6500, 0, 0xFFD1A3);
        hs.phase = HOME_PHASE_SCAN;
        hs.found = 2;
        ui::draw_home({&hs, 700, 0, 0});
        keep("home: scanning");
        hs.phase = HOME_PHASE_LIST;
        hs.selected = 1;
        ui::draw_home({&hs, 0, 0, 0});
        keep("home: lightstrip, on");
        hs.selected = 0;
        ui::draw_home({&hs, 0, 0, 0});
        keep("home: desk lamp 2, on");
        hs.selected = 2;
        ui::draw_home({&hs, 0, 0, UI_BTN_F2});
        keep("home: desk lamp off, F2 held");
        hs.selected = 3;
        ui::draw_home({&hs, 0, 0, 0});
        keep("home: bulb offline");
        hs.selected = 1;
        ui::draw_home({&hs, 0, 30, 0});
        keep("home: sliding");
        hs.phase = HOME_PHASE_EDIT;
        hs.selected = 0;
        hs.option = HOME_OPT_BRIGHT;
        ui::draw_home({&hs, 0, 0, 0});
        keep("home: edit BRIGHT");
        hs.option = HOME_OPT_TEMP;
        hs.lamps[0].ct = 4300;
        hs.lamps[0].rgb = 0xFFDCC0;
        ui::draw_home({&hs, 0, 0, UI_BTN_F1});
        keep("home: edit TEMP, F1 held");
        hs.selected = 1;
        hs.option = HOME_OPT_COLOR;
        ui::draw_home({&hs, 0, 0, 0});
        keep("home: edit COLOR");
        hs.lamps[1].failed = true;
        hs.option = HOME_OPT_BRIGHT;
        ui::draw_home({&hs, 0, 0, 0});
        keep("home: edit, no reply");
        hs.phase = HOME_PHASE_EMPTY;
        ui::draw_home({&hs, 0, 0, 0});
        keep("home: no lamps");
        hs.phase = HOME_PHASE_NO_WIFI;
        ui::draw_home({&hs, 0, 0, 0});
        keep("home: no wifi");
        // The idle screen in HOME: the lamp changed last, as the jumping icon.
        static uint8_t icon[48 * 48 * 2];
        uint32_t acc[3];
        ui::home_idle_icon(hs.lamps[1], icon, acc);
        ui::fx_attract(0, icon, acc, 1, ui::ATTRACT_JUMP);
        for (uint32_t t = 33; t < 2300; t += 33) ui::fx_attract(t, icon, acc, 1, ui::ATTRACT_JUMP), g.clear();
        ui::fx_attract(2300, icon, acc, 1, ui::ATTRACT_JUMP);
        keep("home: idle, lightstrip");
    }
    // The idle screen in MIDI: the synth maker's logo jumps (KORG for the minilogue xd, Roland).
    for (const ui::Sprite *m : {&ui::SPR_LOGO_KORG, &ui::SPR_LOGO_ROLAND}) {
        ui::fx_attract(0, nullptr, nullptr, 1, ui::ATTRACT_JUMP, m);
        for (uint32_t t = 33; t < 2300; t += 33) ui::fx_attract(t, nullptr, nullptr, 1, ui::ATTRACT_JUMP, m), g.clear();
        ui::fx_attract(2300, nullptr, nullptr, 1, ui::ATTRACT_JUMP, m);
        keep(m == &ui::SPR_LOGO_KORG ? "midi: idle, KORG" : "midi: idle, Roland");
    }

    // The built-ins in the firmware's order (app_profiles.c).
    static const ui::ProfileItem profiles[8] = {
        {"MUSIC", app_icon_music_24, app_icon_music_48, {"PLAY", "PREV", "NEXT", "MENU"}},
        {"AGENTS", app_icon_agents_24, app_icon_agents_48, {"ENTER", "ESC", "MODE", "MENU"}},
        {"CLOCK", app_icon_clock_24, app_icon_clock_48, {"12/24", "SEC", "DATE", "MENU"}},
        {"PLASTICITY", app_icon_plasticity_24, app_icon_plasticity_48, {"ZOOM", "ORBIT", "WHEEL", "PAN"}},
        {"FIGMA", app_icon_figma_24, app_icon_figma_48, {"UNDO", "DEPTH", "WHEEL", "FRAME"}},
        {"ONSHAPE", app_icon_onshape_24, app_icon_onshape_48, {"ZOOM", "ORBIT", "WHEEL", "PAN"}},
        {"BLENDER", app_icon_blender_24, app_icon_blender_48, {"-", "-", "-", "MENU"}}, // the empty template
        {"AUTOCAD", app_icon_autocad_24, app_icon_autocad_48, {"-", "-", "-", "MENU"}},
    };
    menu_render_snapshot_t prof = {};
    prof.open = true;
    prof.screen = MENU_SCREEN_APP_PROFILE;
    prof.rows[0] = row("PROFILE", "", "FIGMA", true);
    prof.row_count = 1;
    ui::draw_app_profile(prof, {profiles, 8, 3, 0, true});
    keep("profile PLASTICITY, saved");
    prof.dirty = true;
    ui::draw_app_profile(prof, {profiles, 8, 4, 0, true});
    keep("profile FIGMA, unsaved");
    ui::draw_app_profile(prof, {profiles, 8, 5, 0, true});
    keep("profile ONSHAPE, unsaved");
    ui::draw_app_profile(prof, {profiles, 8, 6, 0, true});
    keep("profile BLENDER (empty template), unsaved");
    ui::AppView empty = {"BLENDER", app_icon_blender_24, {"-", "-", "-", "MENU"}, "SCROLL", "KNOB"};
    ui::draw_main({false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, 0, nullptr, &empty});
    keep("main APP blender (empty template)");
    ui::draw_app_profile(prof, {profiles, 8, 7, 0, true});
    keep("profile AUTOCAD (empty template), unsaved");
    prof.dirty = false;
    prof.rows[0] = row("PROFILE", "", "MUSIC", true);
    ui::draw_app_profile(prof, {profiles, 8, 0, 0, true});
    keep("profile MUSIC, saved");
    hid.selected = 0;
    hid.dirty = false;
    hid.rows[0] = row("PROFILES", "", "MIDI", true);
    hid.rows[1] = row("CHANNEL", "", "01", false);
    hid.rows[2] = row("SYNTH", "", "MINILOGUE X", false);
    hid.row_count = 3;
    ui::draw_hid(hid, {MENU_HID_MIDI, 0, true, nullptr, nullptr, "MINILOGUE XD", "KORG"});
    keep("hid MIDI");
    hid.selected = 1;
    hid.dirty = true;
    hid.rows[0].selected = false;
    hid.rows[1] = row("CHANNEL", "", "01", true);
    ui::draw_hid(hid, {MENU_HID_MIDI, 0, true, nullptr, nullptr, "MINILOGUE XD", "KORG"});
    keep("hid MIDI channel");
    hid.selected = 2;
    hid.rows[1].selected = false;
    hid.rows[2] = row("SYNTH", "", "TR-8S", true);
    ui::draw_hid(hid, {MENU_HID_MIDI, 0, true, nullptr, nullptr, "TR-8S", "ROLAND"});
    keep("hid MIDI synth");

    // MIDI (midi.h): the Main Screen with the built-in synth profiles (midi_synths.c).
    {
        midi_snapshot_t ms = {};
        ms.active = true;
        ms.synth = 1; // minilogue xd
        ms.param = 17; // FILTER CUTOFF (10-bit)
        ms.value = 712;
        ms.channel = 1;
        ms.usb = true;
        ms.trs = true;
        ms.prog = -1;
        ui::draw_midi({&ms, 100000, 0});
        keep("midi: minilogue xd cutoff");
        ms.param = 4; // VCO 1 WAVE
        ms.value = 2;
        ui::draw_midi({&ms, 100000, 0});
        keep("midi: switch (VCO 1 WAVE)");
        ms.param = 17;
        ms.value = -1;
        ms.usb = false;
        ui::draw_midi({&ms, 100000, 0});
        keep("midi: value unknown, TRS only");
        ms.prog = 41;
        ms.prog_ms = 99500;
        ui::draw_midi({&ms, 100000, UI_BTN_F3});
        keep("midi: program sent, F3 held");
        ms.prog = -1;
        ms.browsing = true;
        ms.param = 18;
        ui::draw_midi({&ms, 100000, UI_BTN_F1});
        keep("midi: F1 held, picking");
        ms.browsing = false;
        ms.synth = 3; // TR-8S
        ms.channel = 10;
        ms.usb = true;
        ms.param = 0; // BD TUNE, centred
        ms.value = 52;
        ui::draw_midi({&ms, 100000, 0});
        keep("midi: tr-8s BD tune");
    }

    menu_render_snapshot_t boot = {};
    boot.open = true;
    boot.screen = MENU_SCREEN_BOOT;
    boot.selected = 0;
    boot.dirty = true;
    boot.rows[0] = row("USB MODE", "", "SERIAL", true);
    boot.row_count = 1;
    ui::draw_boot_mode(boot, BOOT_USB_MODE_SERIAL, false, true);
    keep("boot mode");

    // Command wheel (Figma): each card on its resting (last) keyframe, one mid-animation,
    // cancel, a slide in flight, and the Main Screen echo after a run.
    for (const app_profile_t *pp : {&app_profile_figma, &app_profile_plasticity, &app_profile_onshape}) {
        const app_profile_t &p = *pp;
        auto rest_ms = [](const app_scene_t *s) {
            uint32_t t = 0;
            for (int i = 0; i + 1 < s->n_frames; i++) t += s->frames[i].ms;
            return t;
        };
        auto wheel = [&](int ring, int entry, uint32_t t, float slide, const app_scene_t *prev) {
            static char key[8];
            const app_ring_t &r = p.rings[ring];
            ui::WheelView v = {};
            v.ring_name = r.name;
            v.ring_count = p.ring_count;
            for (int i = 0; i < p.ring_count; i++) v.ring_tabs[i] = p.rings[i].tab;
            v.ring = ring;
            v.count = r.count + 1;
            v.entry = entry;
            const app_cmd_t *c = entry ? &r.cmds[entry - 1] : nullptr;
            v.name = c ? c->name : "CANCEL";
            v.scene = c ? c->scene : nullptr;
            if (c) {
                const app_key_t &kk = c->kind == APP_CMD_ACTIONS ? p.search.open : c->key;
                v.search = c->kind == APP_CMD_ACTIONS;
                v.modifier = kk.modifier;
                uint8_t k = kk.keycode;
                if (k >= 0x04 && k <= 0x1D) snprintf(key, sizeof(key), "%c", 'A' + k - 0x04);
                else if (k >= 0x1E && k <= 0x26) snprintf(key, sizeof(key), "%c", '1' + k - 0x1E);
                else if (k >= 0x59 && k <= 0x61) snprintf(key, sizeof(key), "NUM%d", k - 0x59 + 1);
                else if (k == 0x2B) snprintf(key, sizeof(key), "TAB");
                else if (k == 0x38) snprintf(key, sizeof(key), "/");
                else if (k == 0x37) snprintf(key, sizeof(key), ".");
                else snprintf(key, sizeof(key), "?");
                v.key = key;
            }
            v.slide = slide;
            v.slide_dir = 1;
            v.prev = prev;
            v.prev_valid = slide < 1;
            v.t_ms = c && t == UINT32_MAX ? rest_ms(c->scene) : t;
            ui::MainInputs in = {false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, UI_BTN_F3, nullptr, nullptr, &v};
            ui::draw_main(in);
        };
        for (int ring = 0; ring < p.ring_count; ring++)
            for (int e = 1; e <= p.rings[ring].count; e++) {
                wheel(ring, e, UINT32_MAX, 1, nullptr);
                keep(p.rings[ring].cmds[e - 1].name);
            }
        wheel(0, 1, 0, 1, nullptr);
        keep("ADD AUTO LAYOUT, start");
        wheel(0, 0, 0, 1, nullptr);
        keep("wheel cancel");
        wheel(0, 3, UINT32_MAX, 0.4f, p.rings[0].cmds[1].scene);
        keep("wheel sliding");
        ui::AppView echo = {"FIGMA", app_icon_figma_24, {"UNDO", "DEPTH", "WHEEL", "FRAME"}, "ZOOM", "KNOB"};
        echo.echo = true;
        echo.echo_scene = p.rings[0].cmds[2].scene;
        echo.echo_name = p.rings[0].cmds[2].name;
        echo.echo_ms = rest_ms(echo.echo_scene);
        ui::draw_main({false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, 0, nullptr, &echo});
        keep("echo after run");
    }

    // Parameter mode (Plasticity): the value dial for a few commands / states.
    {
        struct T { const char *name, *label; float v; int dec; bool deg; uint8_t vis, modes; bool axes; uint8_t bits; int axis, step; float f3; float s0, s1, s2; };
        const T cases[] = {
            {"FILLET", "CHAMFER", -0.85f, 2, false, APP_PV_FILLET, 2, false, 0, 0, 1, -1, 0.05f, 0.1f, 1},
            {"FILLET", "FILLET", 1.25f, 2, false, APP_PV_FILLET, 2, false, 0, 0, -1, -1, 0.05f, 0.1f, 1},
            {"EXTRUDE", "DISTANCE", 4.5f, 2, false, APP_PV_EXTRUDE, 4, false, 0, 0, 2, -1, 0.05f, 0.1f, 1},
            {"HOLLOW", "THICKNESS", 1.5f, 2, false, APP_PV_HOLLOW, 8, false, 0, 0, -1, -1, 0.05f, 0.1f, 1},
            {"MOVE", "DISTANCE", 6.0f, 2, false, APP_PV_MOVE, 8, true, 1, 0, -1, -1, 0.05f, 0.1f, 1},
            {"MOVE", "DISTANCE", 6.0f, 2, false, APP_PV_MOVE, 8, true, 5, 1, -1, -1, 0.05f, 0.1f, 1},
            {"ROTATE", "ANGLE", 35, 0, true, APP_PV_ROTATE, 8, true, 1, 0, -1, -1, 1, 5, 15},
            {"SCALE", "FACTOR", 1.4f, 2, false, APP_PV_SCALE, 8, true, 7, 0, 0, 0.7f, 0.05f, 0.1f, 1},
        };
        // Onshape (number field): A scroll shows the signed change, B type the value.
        struct F { const char *name, *label; float v, drawn; uint8_t vis; bool typed; int step; float f3; };
        const F fcases[] = {
            {"FILLET", "RADIUS", 0.3f, 1.3f, APP_PV_FILLET, false, 1, -1},
            {"EXTRUDE", "DEPTH", 26.0f, 26.0f, APP_PV_EXTRUDE, true, 2, -1},
            {"CHAMFER", "DISTANCE", -0.05f, 0.95f, APP_PV_CHAMFER, false, 0, 0.5f},
            {"TRANSFORM", "DISTANCE", 8.0f, 8.0f, APP_PV_SLIDE, true, 1, -1},
        };
        for (const F &c : fcases) {
            ui::ParamView v = {};
            v.name = c.name; v.label = c.label; v.value = c.v; v.drawn = c.drawn; v.decimals = 2;
            v.steps[0] = 0.01f; v.steps[1] = 0.1f; v.steps[2] = 1; v.step = c.step; v.visual = c.vis;
            v.field = true; v.typed = c.typed; v.f3 = c.f3;
            ui::MainInputs in = {false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, 0, nullptr, nullptr, nullptr, &v};
            ui::draw_main(in);
            keep(c.name);
        }
        for (const T &c : cases) {
            ui::ParamView v = {};
            v.name = c.name; v.label = c.label; v.value = c.v; v.decimals = c.dec; v.degrees = c.deg;
            v.steps[0] = c.s0; v.steps[1] = c.s1; v.steps[2] = c.s2; v.step = c.step; v.visual = c.vis;
            v.modes = c.modes; v.axes = c.axes; v.axis_bits = c.bits; v.axis = c.axis; v.f3 = c.f3;
            ui::MainInputs in = {false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, 0, nullptr, nullptr, nullptr, &v};
            ui::draw_main(in);
            keep(c.name);
        }
    }

    menu_render_snapshot_t disp = {};
    disp.open = true;
    disp.screen = MENU_SCREEN_DISPLAY;
    disp.dirty = true;
    disp.row_count = 1;
    ui::draw_display(disp, 1, true);
    keep("display rotation 90, unsaved");

    menu_render_snapshot_t dev = {};
    dev.open = true;
    dev.screen = MENU_SCREEN_DEVICE;
    dev.selected = 0;
    dev.rows[0] = row("SYS INFO", "", "", true);
    dev.rows[1] = row("BINDINGS", "", "", false);
    dev.rows[2] = row("RECALIBRATE", "", "", false);
    dev.rows[3] = row("CLICK", "", "", false);
    dev.rows[4] = row("SOUND CAL", "", "", false);
    dev.row_count = 5;
    ui::draw_menu_list(dev, 0);
    keep("device list");

    menu_render_snapshot_t bind = {};
    bind.open = true;
    bind.screen = MENU_SCREEN_BINDINGS;
    bind.row_count = 1;
    bind.selected = 0;
    bind.rows[0] = row("COMPUTER", "", "MAC", true);
    ui::draw_bindings(bind, MENU_HOST_MAC, true);
    keep("bindings MAC");
    bind.dirty = true;
    ui::draw_bindings(bind, MENU_HOST_PC, true);
    keep("bindings PC, unsaved");

    // SYS INFO with plausible numbers: haptics running, LEDs at rest, a missed tick and a
    // dropped report so the amber cases show.
    sysmon_info_t si = {};
    si.motor_ma = 62; si.led_ma = 71; si.board_ma = 150; si.total_ma = 283; si.total_peak_ma = 512;
    si.chip_ok = true; si.chip_c = 44.6f; si.chip_peak_c = 47.2f;
    si.coil_ma = 180; si.coil_peak_ma = 640; si.copper_w = 0.13f;
    si.load[0] = 88; si.load[1] = 41; si.load_peak[0] = 93; si.load_peak[1] = 77;
    si.loop_khz = 10.0f; si.work_avg_us = 38.4f; si.work_max_us = 212.7f; si.jitter_max_us = 131.2f; si.missed = 3;
    si.heap_free = 186 * 1024; si.heap_min = 151 * 1024; si.hid_drops = 1; si.uptime_s = 5025;
    menu_render_snapshot_t sys = {};
    sys.open = true;
    sys.screen = MENU_SCREEN_SYSINFO;
    sys.rows[MENU_SYSINFO_POWER] = row("POWER", "", "", false);
    sys.rows[MENU_SYSINFO_HEAT] = row("HEAT", "", "", false);
    sys.rows[MENU_SYSINFO_CPU] = row("CPU", "", "", false);
    sys.rows[MENU_SYSINFO_LOOP] = row("LOOP", "", "", false);
    sys.rows[MENU_SYSINFO_SYSTEM] = row("SYSTEM", "", "", false);
    sys.row_count = MENU_SYSINFO_PAGE_COUNT;
    const float avg[SYSMON_SEC_COUNT] = {6.2f, 41.8f, 18.4f, 4.1f}, mx[SYSMON_SEC_COUNT] = {88.0f, 63.5f, 402.7f, 9.8f};
    for (int i = 0; i < SYSMON_SEC_COUNT; i++) { si.sec_avg_us[i] = avg[i]; si.sec_max_us[i] = mx[i]; }
    si.work_avg_us = 95.0f; si.other_avg_us = 24.5f;
    const char *sys_names[] = {"sys info POWER", "sys info HEAT", "sys info CPU", "sys info LOOP", "sys info SYSTEM"};
    for (int p = 0; p < MENU_SYSINFO_PAGE_COUNT; p++) {
        sys.selected = p;
        ui::draw_sysinfo(sys, si, {PD_SRC_PD, 3000, 5000});
        keep(sys_names[p]);
    }
    sys.selected = MENU_SYSINFO_POWER;
    ui::draw_sysinfo(sys, si, {PD_SRC_USB, 500, 5000});
    keep("sys info POWER, 500mA USB");

    menu_render_snapshot_t rc = {};
    rc.open = true;
    rc.screen = MENU_SCREEN_RECALIBRATE;
    rc.row_count = 1;
    rc.rows[0] = row("RECALIBRATE", "", "", true);
    ui::draw_recalibrate(rc);
    keep("recalibrate");
    snprintf(rc.rows[0].value, sizeof(rc.rows[0].value), "%s", MENU_RECAL_ARMED);
    ui::draw_recalibrate(rc);
    keep("recalibrate, armed");

    menu_render_snapshot_t clk = {};
    clk.open = true;
    clk.screen = MENU_SCREEN_CLICK;
    clk.row_count = 1;
    clk.selected = 0;
    clk.rows[0] = row("AXIS", "", "Q", true);
    clk.dirty = true;
    ui::draw_click(clk, true);
    keep("click, unsaved");

    // SOUND CAL: before any calibration, mid-sweep, a tone being asked, a click shape, the
    // result, and the stop when the knob turns.
    sndcal_view_t cal = {};
    cal.best = SNDCAL_NOT_HEARD;
    cal.click_hz = 3500;
    ui::draw_sound_cal(cal);
    keep("sound cal, not calibrated");
    cal.stage = SNDCAL_SWEEP; cal.step = 6; cal.freq_hz = 1149; cal.playing = 1;
    ui::draw_sound_cal(cal);
    keep("sound cal, sweep");
    cal.stage = SNDCAL_AXIS_ASK; cal.playing = 0;
    ui::draw_sound_cal(cal);
    keep("sound cal, which axis");
    const uint8_t cal_part[SNDCAL_TONE_COUNT] = {SNDCAL_NOT_HEARD, 4, 4, 3, 3, 2, 3, 2, 1, 2, 0, 0, 0, 0, 0, 0};
    memcpy(cal.heard, cal_part, sizeof(cal_part));
    cal.stage = SNDCAL_TONES; cal.step = 10; cal.level = 1; cal.freq_hz = 2000;
    ui::draw_sound_cal(cal);
    keep("sound cal, tone asked");
    const uint8_t cal_all[SNDCAL_TONE_COUNT] = {SNDCAL_NOT_HEARD, 4, 4, 3, 3, 2, 3, 2, 1, 2, 2, 3, 3, 4, 4, SNDCAL_NOT_HEARD};
    memcpy(cal.heard, cal_all, sizeof(cal_all));
    cal.best = 8; cal.axis = 1; cal.freq_hz = 1516;
    cal.stage = SNDCAL_DONE; cal.playing = 0; cal.calibrated = 1; cal.click_hz = 1516; cal.click_axis = 1;
    ui::draw_sound_cal(cal);
    keep("sound cal, stored");
    cal.stage = SNDCAL_MOVED;
    ui::draw_sound_cal(cal);
    keep("sound cal, knob moved");

    // Idle screen: each routine pinned, at a few telling moments. Each call restarts the
    // routine (time goes back between them), so frames are independent.
    struct Idle { int routine; const char *name; uint32_t ms; };
    const Idle idles[] = {
        {ui::ATTRACT_JUMP, "idle JUMP big jump", 2300}, {ui::ATTRACT_JUMP, "idle JUMP landing", 2720},
        {ui::ATTRACT_JUMP, "idle JUMP spin", 6900}, {ui::ATTRACT_JUMP, "idle JUMP sparkles", 8200},
        {ui::ATTRACT_BOUNCE, "idle BOUNCE travel", 700}, {ui::ATTRACT_BOUNCE, "idle BOUNCE rim hit", 1420},
    };
    for (const Idle &d : idles) {
        ui::fx_attract(0, app_icon_onshape_48, app_profile_onshape.accents, 1, d.routine);
        for (uint32_t t = 33; t < d.ms; t += 33) ui::fx_attract(t, app_icon_onshape_48, app_profile_onshape.accents, 1, d.routine), g.clear();
        ui::fx_attract(d.ms, app_icon_onshape_48, app_profile_onshape.accents, 1, d.routine);
        keep(d.name);
    }
    for (int r = 0; r < ui::ATTRACT_ROUTINES; r++) {
        uint32_t ms = 900;
        for (uint32_t t = 0; t < ms; t += 33) ui::fx_attract(t, nullptr, nullptr, 2, r), g.clear();
        ui::fx_attract(ms, nullptr, nullptr, 2, r);
        keep("idle QUADRA");
    }

    // --- this fork: the user's idle word, LIGHTS, agent notifications, the AGENTS wheel ---
    for (const char *word : {"HELLO", "MAKE IT NICE"}) {
        ui::fx_set_word(word);
        ui::fx_boot(2600);
        keep(strdup(word));
        for (uint32_t t = 0; t < 5950; t += 33) ui::fx_attract(t, nullptr, nullptr, 5, ui::ATTRACT_JUMP), g.clear();
        ui::fx_attract(5950, nullptr, nullptr, 5, ui::ATTRACT_JUMP); // landing at +34 px, squashed: the widest reach
        keep("idle word, side hop");
    }
    ui::fx_set_word(nullptr);
    for (const uint8_t *ic : {app_icon_music_48, app_icon_agents_48}) {
        for (uint32_t t = 0; t < 2720; t += 33) ui::fx_attract(t, ic, nullptr, 7, ui::ATTRACT_JUMP), g.clear();
        ui::fx_attract(2720, ic, nullptr, 7, ui::ATTRACT_JUMP);
        keep("idle: new icon, landing");
    }
    {
        // A ring frame as led_task_snapshot() gives it: a blue comet (SPIN), or the app gradient.
        static uint8_t comet[60][3], grad[60][3];
        for (int i = 0; i < 60; i++) {
            float d = fmodf(42 - i + 120, 60), k = d < 20 ? 1 - d / 20 : 0;
            float lv = 0.06f + 0.94f * k * k;
            comet[i][0] = (uint8_t)(10 * lv); comet[i][1] = (uint8_t)(34 * lv); comet[i][2] = (uint8_t)(51 * lv);
            float u = i / 60.0f;
            grad[i][0] = (uint8_t)(15 + 5 * u); grad[i][1] = (uint8_t)(9 + 4 * u); grad[i][2] = (uint8_t)(2);
        }
        const char *LV[MENU_LIGHTS_ROW_COUNT][2] = {{"COLOR", "CUSTOM"}, {"HUE", "200"}, {"SAT", "80%"},
                                                    {"EFFECT", "SPIN"}, {"SPEED", "4"}, {"LEVEL", "100%"}};
        for (int sel : {0, 3}) {
            menu_render_snapshot_t s = {};
            s.open = true;
            s.screen = MENU_SCREEN_LIGHTS;
            s.dirty = sel == 3;
            s.selected = sel;
            s.row_count = MENU_LIGHTS_ROW_COUNT;
            for (int i = 0; i < MENU_LIGHTS_ROW_COUNT; i++) s.rows[i] = row(LV[i][0], "", LV[i][1], i == sel);
            if (sel == 0) { // COLOR = APP: hue and sat are muted
                snprintf(s.rows[0].value, sizeof(s.rows[0].value), "APP");
                snprintf(s.rows[3].value, sizeof(s.rows[3].value), "GRADIENT");
                for (int i : {1, 2, 4}) s.rows[i].muted = true, snprintf(s.rows[i].value, sizeof(s.rows[i].value), "--");
            }
            ui::draw_lights(s, {sel == 0 ? 0xFFB030u : 0x33AAFFu, sel == 0 ? grad : comet, true});
            keep(sel == 0 ? "lights: APP colour" : "lights: editing EFFECT");
        }
    }
    static const uint32_t one_claude[1] = {0xE8825F}, three[3] = {0xE8825F, 0x4F9DFF, 0xB98CFF};
    ui::draw_notify({ui::AGENT_CLAUDE, "CLAUDE CODE", 0xE8825F, "RUN", "rm -rf node_modules && npm install --force", true,
                     three, 3, 0, 0, 1200});
    keep("notify: 3 agents waiting");
    ui::draw_notify({ui::AGENT_CLAUDE, "CLAUDE CODE", 0xE8825F, "EDIT", "agents.c", true, one_claude, 1, 0.62f, UI_BTN_F1, 2400});
    keep("notify: F1 held 62%");
    static const uint32_t codex1[1] = {0x4F9DFF};
    ui::draw_notify({ui::AGENT_CODEX, "CODEX", 0x4F9DFF, "PATCH", "ext_link.c, notify.c, menu.c", true, codex1, 1, 0, 0, 600});
    keep("notify: Codex asks");
    static const uint32_t cursor1[1] = {0xB98CFF};
    ui::draw_notify({ui::AGENT_CURSOR, "CURSOR", 0xB98CFF, "YOUR TURN", "Cursor finished the refactor", false, cursor1, 1, 0, 0, 1800});
    keep("notify: Cursor, info");
    {
        const app_profile_t &p = app_profile_agents;
        static char key[12];
        for (int ring = 0; ring < p.ring_count; ring++) {
            for (int e : {1, 3, 10}) {
                if (e > p.rings[ring].count) continue;
                const app_cmd_t &c = p.rings[ring].cmds[e - 1];
                ui::WheelView v = {};
                v.ring_name = p.rings[ring].name;
                v.ring_count = p.ring_count;
                for (int i = 0; i < p.ring_count; i++) v.ring_tabs[i] = p.rings[i].tab;
                v.ring = ring;
                v.count = p.rings[ring].count + 1;
                v.entry = e;
                v.name = c.name;
                v.scene = c.scene;
                v.modifier = c.key.modifier;
                uint8_t k = c.key.keycode;
                if (k >= 0x04 && k <= 0x1D) snprintf(key, sizeof(key), "%c", 'A' + k - 4);
                else snprintf(key, sizeof(key), "%s", k == 0x28 ? "ENTER" : k == 0x2A ? "BKSP" : k == 0x37 ? "." : "?");
                v.key = key;
                v.chord = c.kind == APP_CMD_KEYS && c.scene == nullptr;
                if (c.kind == APP_CMD_MACRO) {
                    const app_macro_t &m = p.macros[c.macro - 1];
                    if (m.steps[0].kind == APP_MSTEP_TEXT) {
                        v.typed = m.steps[0].text;
                    } else {
                        v.chord = true;
                        v.modifier = m.steps[0].key.modifier;
                        snprintf(key, sizeof(key), "ESC");
                        v.chord_repeat = m.count;
                    }
                }
                v.slide = 1;
                v.slide_dir = 1;
                v.t_ms = 2600;
                ui::MainInputs in = {false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, UI_BTN_F1, nullptr, nullptr, &v};
                ui::draw_main(in);
                keep(c.name);
            }
        }
    }

    {
        // MUSIC now playing: a synthetic cover (COVER_RAW, 240x240 RGB565 BE, band already darkened).
        static uint8_t cover[240 * 240 * 2];
        FILE *f = getenv("COVER_RAW") ? fopen(getenv("COVER_RAW"), "rb") : nullptr;
        bool have = f && fread(cover, 1, sizeof(cover), f) == sizeof(cover);
        if (f) fclose(f);
        struct NP { const char *name; bool playing; int vol; float vk; int glyph; float gk; };
        const NP nps[] = {{"np: playing", true, 42, 0, 0, 0}, {"np: volume ring", true, 42, 1, 0, 0},
                          {"np: paused + glyph", false, 42, 0, ui::NP_GLYPH_PLAY, 1}};
        for (const NP &n : nps) {
            if (have) ui::image565(0, 0, 240, 240, cover);
            ui::draw_now_playing({"MIDNIGHT CITY", "M83", have, app_icon_music_48, 0xFF8C3C, n.playing, n.vol, n.vk,
                                  n.glyph, n.gk});
            keep(n.name);
        }
        ui::draw_now_playing({"NO COVER YET", "ARTIST", false, app_icon_music_48, 0xFF8C3C, true, -1, 0, 0, 0});
        keep("np: no cover");
        // The cover on a record (ui_vinyl.hpp): each style playing, BLEED at rest, RECORD with the
        // volume ring, and SLIDE just after F4 picked it.
        static uint16_t plain[240 * 240], polar[ui::VINYL_POLAR_PX], label[ui::VINYL_LABEL_PX],
            sleeve[ui::VINYL_SLEEVE_PX];
        static uint8_t base[ui::VINYL_BASE_BYTES], tables[ui::VINYL_TABLES_BYTES];
        FILE *fp = getenv("COVER_PLAIN_RAW") ? fopen(getenv("COVER_PLAIN_RAW"), "rb") : nullptr;
        bool have_plain = fp && fread(plain, 1, sizeof(plain), fp) == sizeof(plain);
        if (fp) fclose(fp);
        if (have_plain) {
            ui::vinyl_bind({polar, base, label, sleeve, tables});
            struct VT { const char *name; int style; float theta, out; bool playing; int vol; float vk; const char *badge; };
            const VT vts[] = {{"np: record", COVER_RECORD, 0.6f, 1, true, 42, 0, nullptr},
                              {"np: record, volume", COVER_RECORD, 0.6f, 1, true, 42, 1, nullptr},
                              {"np: slide", COVER_SLIDE, 0.6f, 1, true, 42, 0, nullptr},
                              {"np: slide, picked", COVER_SLIDE, 0.6f, 1, true, 42, 0, "SLIDE"},
                              {"np: bleed", COVER_BLEED, 0.6f, 1, true, 42, 0, nullptr},
                              {"np: bleed, paused", COVER_BLEED, 0.6f, 0, false, 42, 0, nullptr}};
            for (const VT &v : vts) {
                ui::vinyl_prepare(v.style, plain);
                ui::vinyl_draw(v.style, v.theta, v.out);
                ui::NowPlayingInputs np = {"MIDNIGHT CITY", "M83", true, app_icon_music_48, 0xFF8C3C, v.playing, v.vol, v.vk, 0, 0};
                np.style = v.style;
                np.badge = v.badge;
                ui::draw_now_playing(np);
                keep(v.name);
            }
        }
        // CLOCK: a zone besides LOCAL, 24 h with seconds and the date; then LOCAL, 12 h, plain.
        ui::draw_clock({true, 14, 7, 42, 6, 3, 9, true, true, true, "TOKYO", 9 * 60, 2, 4, 0xFFB030});
        keep("clock: zone, 24h");
        ui::draw_clock({true, 9, 41, 0, 6, 3, 9, false, false, false, "LOCAL", 2 * 60, 0, 4, 0xFFB030});
        keep("clock: local, 12h");
        // AGENTS dashboard over the APP main screen (middle cleared).
        const app_profile_t &ap = app_profile_agents;
        ui::AppView av = {ap.name, ap.icon24, {ap.legend[0], ap.legend[1], ap.legend[2], ap.legend[3]}, "", "", "KNOB"};
        ui::draw_main({false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, 0, nullptr, &av});
        const ui::AgentRowView rows[] = {{0xE8825F, "binaris", 1}, {0x4F9DFF, "api-server", 2}, {0xB98CFF, "web", 3}};
        ui::draw_agent_board(rows, 3, 900);
        keep("agents: board");
        ui::draw_main({false, 6, MENU_HID_APP, HAPTIC_TYPE_SAW, 0, nullptr, &av});
        ui::draw_agent_board(rows, 0, 0);
        keep("agents: none");
    }

    // Contact sheet: 4 per row, 2x, round mask, 8px gutters. Binary PPM on stdout; names on stderr.
    const int cols = 4, sc = 2, cell = 240 * sc + 16;
    int rows = (int)(tiles.size() + cols - 1) / cols;
    int W = cols * cell, H = rows * cell;
    std::vector<uint8_t> img(W * H * 3, 24);
    for (size_t t = 0; t < tiles.size(); t++) {
        int ox = (int)(t % cols) * cell + 8, oy = (int)(t / cols) * cell + 8;
        for (int y = 0; y < 240 * sc; y++)
            for (int x = 0; x < 240 * sc; x++) {
                float dx = x / (float)sc - 119.5f, dy = y / (float)sc - 119.5f;
                uint32_t c = (dx * dx + dy * dy <= 120 * 120) ? tiles[t].px[(y / sc) * 240 + x / sc] : 0x181818;
                uint8_t *p = &img[((oy + y) * W + ox + x) * 3];
                p[0] = c >> 16; p[1] = c >> 8; p[2] = c;
            }
        fprintf(stderr, "%zu: %s\n", t, tiles[t].name);
    }
    printf("P6\n%d %d\n255\n", W, H);
    fwrite(img.data(), 1, img.size(), stdout);
    return 0;
}
