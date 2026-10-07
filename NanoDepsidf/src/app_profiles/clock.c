// CLOCK: the time, in up to five zones (clock.h): LOCAL -- the computer's -- and four more.
// Turning the knob steps through the zones; F1 switches 12 / 24 hours, F2 the seconds, F3 the
// date. The display task does all of that (display_task.cpp): nothing goes to the computer.
// The zones and the format are also set from the companion app (LOOK) or `quadra.py clock`.
#include "app_profile.h"
#include "icons/app_icons.h"

const app_profile_t app_profile_clock = {
    .version = APP_PROFILE_VERSION,
    .id = "clock",
    .name = "CLOCK",
    .icon24 = app_icon_clock_24,
    .icon48 = app_icon_clock_48,
    .legend = {"12/24", "SEC", "DATE", "MENU"},
    // No actions: the knob and the keys are read by the display task while CLOCK is up, and
    // nothing goes to the computer. F4: a tap = the other screen (the flap clock / the board),
    // long press = menu, as in every profile.
};
