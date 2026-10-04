#pragma once
// An app's three accent colours (RGB888, dark to bright), shared by the idle screen and the
// LEDs so both show the same colours: the profile's own `accents` when it names them,
// otherwise the three most common colours of its 48x48 icon. Pure function, no state.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// icon48: RGB565 big-endian 48x48 (black = transparent), may be NULL; heat: 3 colours or NULL
// (all zero also counts as none). Neither: AMBER x3.
void app_accents(const uint8_t *icon48, const uint32_t *heat, uint32_t out[3]);

#ifdef __cplusplus
}
#endif
