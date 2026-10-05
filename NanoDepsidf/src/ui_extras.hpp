#pragma once
// This fork's screens (LIGHTS, agent notifications), drawn with the Pixel UI toolkit like
// ui_screens.cpp and kept apart from it so upstream changes merge cleanly.

#include <stdint.h>
extern "C" {
#include "menu.h"
#include "home.h"
#include "midi.h"
}

namespace ui {

// LIGHTS: one row per field (MENU_LIGHTS_ROW_*), the focused one on an amber bar. The rim of
// the glass mirrors the LED ring as it is right now (`ring`: 60 RGB triples, clockwise from 12
// o'clock as led_task_snapshot() gives them; nullptr = none). `swatch`: the colour in use.
struct LightsInputs {
    uint32_t swatch;
    const uint8_t (*ring)[3];
    bool blink_on;
};
void draw_lights(const menu_render_snapshot_t &snap, const LightsInputs &in);

// An agent notification (notify.h). Takes the whole screen while the menu is closed.
enum { AGENT_CLAUDE = 0, AGENT_CODEX, AGENT_CURSOR, AGENT_OTHER };
struct NotifyInputs {
    int agent;               // AGENT_*: which mark goes on the badge
    const char *source;      // "CLAUDE CODE"
    uint32_t color;          // the agent's colour (RGB888)
    const char *title;       // "RUN"
    const char *body;        // the command or file, wrapped to three lines
    bool ask;                // an approval (ALLOW / DENY), else an attention item (any key)
    const uint32_t *queue;   // colours of everything waiting, this one first
    int waiting;             // how many (>= 1)
    float hold;              // F1 hold progress 0..1 -- a green arc fills the rim
    uint8_t buttons;         // held keys (UI_BTN_*)
    uint32_t t_ms;           // animation clock
};
void draw_notify(const NotifyInputs &in);

// MUSIC, something playing: drawn over the cover (already in the frame: flat with its lower
// part darkened for the text, or on a record, ui_vinyl.hpp) -- title and artist, a volume ring
// while the knob turns, and a big glyph for a moment after a media key.
enum { NP_GLYPH_NONE = 0, NP_GLYPH_PLAY, NP_GLYPH_PAUSE, NP_GLYPH_PREV, NP_GLYPH_NEXT };
struct NowPlayingInputs {
    const char *title;
    const char *artist;
    bool has_cover;     // false: the icon stands in for it
    const uint8_t *icon48;
    uint32_t accent;    // the cover's colour (RGB888)
    bool playing;
    int volume;         // 0..100, -1 unknown
    float volume_k;     // 0..1: how visible the volume ring is (fades out after turning)
    int glyph;          // NP_GLYPH_*
    float glyph_k;      // 0..1, fading
    int style = 0;               // cover_style_t: where the title and the overlays go
    const char *badge = nullptr; // a word at the top for a moment (the style F4 just picked)
};
void draw_now_playing(const NowPlayingInputs &in);

// CLOCK (clock.h): one zone's time, big; the zone's name and UTC offset above it, the date
// below, a dot per zone, and a seconds ring round the glass (from 12 o'clock, like a dial).
struct ClockInputs {
    bool valid;                  // the time has been set (WiFi or the Mac service)
    int hour, minute, second;    // the zone's wall clock
    int wday, mday, mon;         // tm_wday (0 = Sunday), day of the month, tm_mon (0 = January)
    bool h24, seconds, date;     // the format (CLOCK_*)
    const char *label;           // the zone
    int offset_min;              // its UTC offset
    int zone, zones;             // which one, of how many
    uint32_t accent;
};
void draw_clock(const ClockInputs &in);

// AGENTS: who's running and what each is doing, in the Main Screen's middle.
struct AgentRowView {
    uint32_t color;
    const char *name;
    int state; // agent_board.h agent_state_t
};
void draw_agent_board(const AgentRowView *rows, int n, uint32_t t_ms);

// HOME (home.h): the whole Main Screen -- scanning, the lamps as a carousel, or one lamp's option
// being turned, its scale thin on the rim. The lamp's colours and the Mi badge are HOME's palette
// exceptions (PIXEL_ART.md section 2).
struct HomeInputs {
    const home_snapshot_t *snap;
    uint32_t t_ms;     // the scan animation's clock
    float slide_px;    // the carousel's offset while sliding to `selected`, 0 at rest
    uint8_t buttons;   // held keys (UI_BTN_*)
};
void draw_home(const HomeInputs &in);
// HOME's idle-screen icon: lamp `l` drawn into a 48x48 RGB565 BE image (black = see-through),
// and its three accent colours for the idle effects and LEDs.
void home_idle_icon(const home_lamp_view_t &l, uint8_t *icon48, uint32_t accents[3]);

// MIDI (midi.h): the whole Main Screen -- the synth, the parameter being turned and its value,
// the ports, and F1-F4. With F1 held, the parameter list instead. Palette only.
struct MidiInputs {
    const midi_snapshot_t *snap;
    uint32_t t_ms;   // ms since boot: a program change shows for a moment (snap->prog_ms)
    uint8_t buttons; // held keys (UI_BTN_*)
};
void draw_midi(const MidiInputs &in);

} // namespace ui
