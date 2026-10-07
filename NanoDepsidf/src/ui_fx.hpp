#pragma once
// Pixel UI effects: the loading screen and the attract animations. All time-driven and
// stateless per frame (a pure function of elapsed ms), drawn through ui_gfx.

#include <stdint.h>

namespace ui {

struct Sprite; // ui_gfx.hpp

void fx_init(); // one-time tables (the idle word, the jump's choreography)

// The word the idle screen animates: the user's own (user_prefs.h, up to 12 characters) or
// nullptr / "" for the stock QUADRA. With an app icon up, a user's word takes turns with it,
// one routine each. Call from the drawing task.
void fx_set_word(const char *text);

// Loading screen, in the idle screen's manner: two 48 px tiles glued back to back (a white one
// with a black Q, a red one with Espressif's mark) drop in, hop with half a turn and
// spin-jump; at the top they split with a flash, circle each other once and come down side by
// side, and POWERED BY ESP32-S3 types in below. The caller plays a knock when they land and
// the startup chime just after.
constexpr uint32_t BOOT_ANIM_MS = 3900;
constexpr uint32_t BOOT_SPLIT_MS = 1700;
constexpr uint32_t BOOT_LAND_MS = 2750;
constexpr uint32_t BOOT_CHIME_MS = 3050;
void fx_boot(uint32_t elapsed_ms);

// Attract (idle) animation, arcade attract mode: the active profile's 48x48 icon -- or, with
// none, the QUADRA wordmark -- in a routine (JUMP; BOUNCE is kept but off). `t_ms` counts from
// the start of the idle session; `seed` picks the random sequence of routines (a new seed or
// time going back starts a new one). `heat`: 3 accent colours (RGB888), nullptr = sampled from
// the icon, AMBER without one. `only` >= 0 pins one routine (the host preview). `mark`: a 1-bit
// logo shown instead of the icon (MIDI: the synth maker's), in white with AMBER effects.
enum { ATTRACT_JUMP = 0, ATTRACT_BOUNCE, ATTRACT_ROUTINES };
void fx_attract(uint32_t t_ms, const uint8_t *icon48 = nullptr, const uint32_t *heat = nullptr, uint32_t seed = 0,
                int only = -1, const Sprite *mark = nullptr);

} // namespace ui
