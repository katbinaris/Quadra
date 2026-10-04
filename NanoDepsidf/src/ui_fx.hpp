#pragma once
// Pixel UI effects: the loading screen and the attract animations. All time-driven and
// stateless per frame (a pure function of elapsed ms), drawn through ui_gfx.

#include <stdint.h>

namespace ui {

void fx_init(); // one-time tables (logo particles, the jump's choreography)

// The word the loading screen assembles and the idle screen animates: the user's own
// (user_prefs.h, up to 12 characters) or nullptr / "" for the stock QUADRA. With an app icon
// up, a user's word takes turns with it, one routine each. Call from the drawing task.
void fx_set_word(const char *text);

// Loading screen "Big bang": a spark swells and bursts, the logo's own pixels scatter, then
// fly home and lock in; the maker line types in and a block bar fills.
constexpr uint32_t BOOT_ANIM_MS = 3400;
void fx_boot(uint32_t elapsed_ms);

// Attract (idle) animation, arcade attract mode: the active profile's 48x48 icon -- or, with
// none, the QUADRA wordmark -- in a routine (JUMP; BOUNCE is kept but off). `t_ms` counts from
// the start of the idle session; `seed` picks the random sequence of routines (a new seed or
// time going back starts a new one). `heat`: 3 accent colours (RGB888), nullptr = sampled from
// the icon, AMBER without one. `only` >= 0 pins one routine (the host preview).
enum { ATTRACT_JUMP = 0, ATTRACT_BOUNCE, ATTRACT_ROUTINES };
void fx_attract(uint32_t t_ms, const uint8_t *icon48 = nullptr, const uint32_t *heat = nullptr, uint32_t seed = 0,
                int only = -1);

} // namespace ui
