#pragma once
#include <stdint.h>
// The test moves the clock by hand (midi_synths_reap frees what's a second old).
extern int64_t g_test_now_us;
static inline int64_t esp_timer_get_time(void) { return g_test_now_us; }
