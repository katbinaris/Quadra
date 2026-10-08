#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// This fork's personal settings: the idle-screen text and the LED look (LIGHTS).
//
// LIGHTS follows menu.c's settings model -- live atomics, a saved copy, F2 saves, leaving the
// screen without F2 puts them back -- so menu.c captures/restores them with lights_get/set and
// saves them through lights_save(). The idle text has no on-device editor: the host sets it
// (tools/quadra.py text) and it is stored at once.

void user_prefs_init(void); // once, before the display and LED tasks start (loads NVS)

// --- Idle text: replaces the QUADRA wordmark (loading screen and idle animation) ---
#define USER_TEXT_MAX 12 // characters; printable ASCII, anything else becomes a space
uint32_t user_text_version(void);        // bumps on every change; any core
void user_text_get(char *out, size_t n); // "" = the stock wordmark
bool user_text_set(const char *s);       // live + NVS (a flash write: Core 1 only)

// --- MUSIC's cover: how the now-playing screen shows it (ui_vinyl.hpp) ---
// Cycled by a tap of F4 on the now-playing screen, or set from the host (EXT_CMD_MUSIC). The
// display task changes it; the usb task stores it a moment later (user_prefs_poll).
typedef enum { COVER_FLAT = 0, COVER_RECORD, COVER_SLIDE, COVER_BLEED, COVER_STYLE_COUNT } cover_style_t;
int cover_style_get(void);       // any core
void cover_style_set(int style); // clamped; any core
const char *cover_style_name(int style);
void user_prefs_poll(void); // Core 1 (usb task): stores a changed cover style

// --- LIGHTS: the LED ring and key LEDs ---
typedef enum { LIGHT_SRC_APP = 0, LIGHT_SRC_CUSTOM, LIGHT_SRC_COUNT } light_src_t;
typedef enum {
    LIGHT_FX_GRADIENT = 0, // the stock look: the palette around the ring, drifting while idle
    LIGHT_FX_SOLID,
    LIGHT_FX_BREATHE,
    LIGHT_FX_SPIN,         // a comet chasing round the ring
    LIGHT_FX_RAINBOW,      // the whole hue wheel, turning (ignores the colour)
    LIGHT_FX_OFF,          // dark at rest; the knob spot, flashes and notifications still show
    LIGHT_FX_COUNT
} light_fx_t;

typedef struct {
    int32_t src;   // light_src_t: the active app's colours, or hue/sat below
    int32_t hue;   // 0..359
    int32_t sat;   // 0..100 (0 = white)
    int32_t fx;    // light_fx_t
    int32_t speed; // 1..10 (BREATHE, SPIN, RAINBOW)
    int32_t level; // brightness, % of the stock level, 10..200 (power budget still applies)
} lights_t;

#define LIGHT_HUE_STEP 5
#define LIGHT_SAT_STEP 10
#define LIGHT_LEVEL_STEP 10

void lights_get(lights_t *out);      // any core
void lights_set(const lights_t *in); // clamps every field; any core
bool lights_save(void);              // the live values -> NVS (Core 1)

// Menu field callbacks (menu.c): rotate on Core 0 (CONTROL_HOT), format on Core 1.
void lights_rotate_src(int8_t dir);
void lights_rotate_hue(int8_t dir);
void lights_rotate_sat(int8_t dir);
void lights_rotate_fx(int8_t dir);
void lights_rotate_speed(int8_t dir);
void lights_rotate_level(int8_t dir);
const char *lights_fx_name(int32_t fx);
bool lights_custom(void);      // src == CUSTOM (Core 0 safe: the menu's mute callbacks)
bool lights_fx_animated(void); // the effect uses SPEED

// The custom colour as RGB888 at full value (hue/sat only), for swatches and the LED palette.
uint32_t lights_hsv(float hue_deg, float sat, float val);

// --- SCREEN: brightness, the screensaver, and the sleep hours ---
// The same model as LIGHTS: live atomics, F2 saves (screen_save), leaving puts them back; the
// host sets them with EXT_CMD_IDLE. During the sleep hours the screensaver goes dark (backlight
// and every LED off) DARK AFTER seconds after it starts; only touching the knob wakes it, to
// the full or the night (DIM) level. The sleep hours only apply while the knob trusts its
// local time (clock_trusted): otherwise the screen behaves as by day.
typedef enum {
    SAVER_AUTO = 0, // MUSIC while a track plays, else CLOCK when the time is known, else ICON
    SAVER_ICON,     // the jumping icon or word (ui_fx.cpp)
    SAVER_BOUNCE,   // the icon rattling round the rim over stars
    SAVER_CLOCK,
    SAVER_MUSIC,    // the cover, title and artist (falls back like AUTO with nothing playing)
    SAVER_BLANK,    // the backlight off
    SAVER_NEVER,
    SAVER_COUNT
} saver_t;
typedef enum { WAKE_NORMAL = 0, WAKE_DIM, WAKE_COUNT } wake_t;

typedef struct {
    int32_t bright;     // backlight, 10..100 %
    int32_t saver;      // saver_t
    int32_t saver_s;    // seconds idle before the screensaver (one of SCREEN_SAVER_STEPS)
    int32_t sleep_on;   // 0 / 1
    int32_t sleep_from; // minutes of the day the sleep hours start, 0..1439 (15 min steps)
    int32_t sleep_to;   // ... and end; before sleep_from = across midnight
    int32_t dark_s;     // seconds of screensaver before dark, in the sleep hours (SCREEN_DARK_STEPS)
    int32_t wake;       // wake_t: how bright it wakes in the sleep hours
} screen_t;

#define SCREEN_BRIGHT_STEP 10
#define SCREEN_TIME_STEP 15 // minutes
#define SCREEN_DIM_BRIGHT 15 // %: WAKE_DIM's backlight
#define SCREEN_DIM_LEDS 0.3f // and its LED scale

void screen_get(screen_t *out);           // any core
void screen_set(const screen_t *in);      // clamps every field (times to 15 min, delays to a step); any core
bool screen_store(const screen_t *s);     // -> NVS (Core 1); menu.c says which (its saved baseline)
// Which fields each menu screen owns: DISPLAY bright, saver, saver_s; SLEEP the rest.
void screen_copy_part(screen_t *dst, const screen_t *src, bool sleep_part);
bool screen_part_differs(const screen_t *a, const screen_t *b, bool sleep_part);

// The sleep hours apply right now: on, the time trusted, and inside the window. Reads the clock:
// call it about once a second, not per frame (the display task caches it, screen_sleeping()).
bool screen_sleep_now(void);
bool screen_sleeping(void);       // the display task's last answer; any core
void screen_set_sleeping(bool s); // display task
bool screen_dim_now(void);        // ON WAKE = DIM (only meaningful while screen_sleeping())

// Menu field callbacks (menu.c): rotate on Core 0 (CONTROL_HOT), format on Core 1.
void screen_rotate_bright(int8_t dir);
void screen_rotate_saver(int8_t dir);
void screen_rotate_saver_s(int8_t dir);
void screen_rotate_sleep_on(int8_t dir);
void screen_rotate_sleep_from(int8_t dir);
void screen_rotate_sleep_to(int8_t dir);
void screen_rotate_dark_s(int8_t dir);
void screen_rotate_wake(int8_t dir);
const char *screen_saver_name(int32_t saver);
const char *screen_wake_name(int32_t wake);
void screen_format_secs(int32_t s, char *out, size_t n); // "15 S", "2 MIN", "NOW"
void screen_format_time(int32_t min, char *out, size_t n); // "23:00"
bool screen_sleep_enabled(void); // sleep_on (Core 0 safe: the menu's mute callbacks)
