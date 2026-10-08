#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// The CLOCK app's time. The system clock is set from the internet (SNTP, net.c) or by the Mac
// service over USB (EXT_CMD_TIME); until then it isn't valid (the chip has no clock battery).
// Time zones are POSIX TZ rules (daylight saving included), in 5 slots: 0 = LOCAL, the
// computer's, sent with the time; 1-4 the ones the user picks (EXT_CMD_CLOCK). The format and
// the zones live in NVS.

#define CLOCK_SLOTS 5
#define CLOCK_LABEL_MAX 12
#define CLOCK_TZ_MAX 45 // the longest rule in tzdata is 44 (Pacific/Chatham)

enum {
    CLOCK_24H = 0x01,
    CLOCK_SECONDS = 0x02,     // HH:MM:SS, and the screen's seconds ring
    CLOCK_DATE = 0x04,
    CLOCK_LED_SECONDS = 0x08, // the LED ring sweeps the seconds
    CLOCK_BOARD = 0x10,       // the screen is the airline board (every zone), not the flap clock
};
#define CLOCK_FLAGS_DEFAULT (CLOCK_24H | CLOCK_SECONDS | CLOCK_DATE | CLOCK_LED_SECONDS)

void clock_init(void);  // app_main, after nvs_flash_init()
bool clock_valid(void); // the time has been set
void clock_set_utc_ms(int64_t ms);
void clock_note_sync(void); // SNTP set the time (clock_set_utc_ms notes its own)
// The local wall clock can be relied on: set, synced in the last 24 h (no battery-backed clock
// keeps it honest), and LOCAL's zone sent by a host at some point (SNTP alone only knows UTC).
// The sleep hours (user_prefs.h) only apply while this holds.
bool clock_trusted(void);
uint8_t clock_flags(void);
void clock_set_flags(uint8_t flags); // stored from clock_poll()
// Slot `slot` (0-4): its label and rule ("" = empty). LOCAL's rule is "" until the host sends it.
void clock_slot(int slot, char label[CLOCK_LABEL_MAX + 1], char tz[CLOCK_TZ_MAX + 1]);
bool clock_set_slot(int slot, const char *label, const char *tz); // stored from clock_poll()
// The zones to show, in slot order (empty slots skipped): how many, and the n-th one's slot.
int clock_zone_count(void);
int clock_zone_slot(int n);
// The time now in slot `slot`. false while the time isn't set. *offset_min: its UTC offset.
bool clock_now(int slot, struct tm *out, int *ms, int *offset_min);
uint32_t clock_version(void); // bumps on a change of format or zones
void clock_poll(void);        // usb task: writes what changed to NVS
