#include "ui_state.h"
#include "tasks_common.h"
#include <stdatomic.h>

static _Atomic int32_t s_detent = 0;
static _Atomic uint8_t s_buttons = 0;
static _Atomic int32_t s_knob_angle = 0;
static _Atomic bool s_usb_serial_active = false;
static _Atomic uint8_t s_screensaver = UI_SAVER_OFF;
static _Atomic uint32_t s_clicks = 0, s_walls = 0;
static _Atomic int32_t s_turns = 0;

void CONTROL_HOT ui_state_note_click(void) { atomic_fetch_add_explicit(&s_clicks, 1, memory_order_relaxed); }
void CONTROL_HOT ui_state_note_wall(void) { atomic_fetch_add_explicit(&s_walls, 1, memory_order_relaxed); }
uint32_t ui_state_get_clicks(void) { return atomic_load_explicit(&s_clicks, memory_order_relaxed); }
void CONTROL_HOT ui_state_note_turn(int8_t dir) { atomic_fetch_add_explicit(&s_turns, dir, memory_order_relaxed); }
int32_t ui_state_get_turns(void) { return atomic_load_explicit(&s_turns, memory_order_relaxed); }
uint32_t ui_state_get_walls(void) { return atomic_load_explicit(&s_walls, memory_order_relaxed); }

void ui_state_init(void) {
    atomic_store_explicit(&s_detent, 0, memory_order_relaxed);
    atomic_store_explicit(&s_buttons, 0, memory_order_relaxed);
    atomic_store_explicit(&s_usb_serial_active, false, memory_order_relaxed);
    atomic_store_explicit(&s_screensaver, UI_SAVER_OFF, memory_order_relaxed);
}

void CONTROL_HOT ui_state_set_detent(int32_t wrapped_index) {
    atomic_store_explicit(&s_detent, wrapped_index, memory_order_relaxed);
}

int32_t ui_state_get_detent(void) {
    return atomic_load_explicit(&s_detent, memory_order_relaxed);
}

void CONTROL_HOT ui_state_set_knob_angle(int32_t angle_1e4_rad) {
    atomic_store_explicit(&s_knob_angle, angle_1e4_rad, memory_order_relaxed);
}

int32_t ui_state_get_knob_angle(void) {
    return atomic_load_explicit(&s_knob_angle, memory_order_relaxed);
}

void CONTROL_HOT ui_state_set_buttons(uint8_t held_mask) {
    atomic_store_explicit(&s_buttons, held_mask, memory_order_relaxed);
}

uint8_t CONTROL_HOT ui_state_get_buttons(void) {
    return atomic_load_explicit(&s_buttons, memory_order_relaxed);
}

void ui_state_set_usb_serial_active(bool serial) {
    atomic_store_explicit(&s_usb_serial_active, serial, memory_order_relaxed);
}

bool ui_state_get_usb_serial_active(void) {
    return atomic_load_explicit(&s_usb_serial_active, memory_order_relaxed);
}

void ui_state_set_screensaver(ui_saver_t state) {
    atomic_store_explicit(&s_screensaver, (uint8_t)state, memory_order_relaxed);
}

bool CONTROL_HOT ui_state_get_screensaver(void) {
    return atomic_load_explicit(&s_screensaver, memory_order_relaxed) != UI_SAVER_OFF;
}

ui_saver_t CONTROL_HOT ui_state_get_saver(void) {
    return (ui_saver_t)atomic_load_explicit(&s_screensaver, memory_order_relaxed);
}
