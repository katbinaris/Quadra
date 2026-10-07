#pragma once

#include <stdbool.h>
#include <stdint.h>

// DEVICE -> SYS INFO: what the board can tell about itself while running -- the numbers behind
// the integration test (DEVELOPMENT_PLAN.md Phase 10): core load, control-loop timing, dropped
// HID reports, audio gaps, memory, heat and power.
//
// Measured: chip temperature (the ESP32-S3's internal sensor -- the die, not the motor), core
// load (FreeRTOS run-time stats), the control loop's own timing, the counters.
// **Estimated**: power. The board has no current sense, so the draw is modelled -- motor from the
// commanded q-axis voltage and the winding resistance, LEDs from the pixel values (led_task.c's
// own estimate), everything else a fixed baseline (SYSMON_BOARD_MA, sysmon.c).
//
// Plain C types only: the host UI preview (tools/ui_preview) draws from this struct too.

#define SYSMON_SPIKE_US 60 // a control iteration this long counts as a spike (normal is ~30)

// Parts of one haptic control iteration, timed separately (SYS INFO's LOOP page).
typedef enum {
    SYSMON_SEC_INPUT = 0, // buttons, menu input, APP-mode engine
    SYSMON_SEC_SENSOR,    // MT6701 angle read over SPI
    SYSMON_SEC_FORCE,     // haptic force, detent edges, clicks, knob -> menu / HID
    SYSMON_SEC_MOTOR,     // inverse Park/Clarke + PWM compare update
    SYSMON_SEC_COUNT,
} sysmon_section_t;

typedef struct {
    uint32_t version; // bumps on every refresh (~2 Hz) -- the display redraws on a change

    // Power from the USB supply, mA (estimates)
    uint16_t motor_ma, led_ma, board_ma, total_ma;
    uint16_t total_peak_ma; // highest 0.5 s total since the last reset

    // Heat
    bool chip_ok;             // the temperature sensor is up
    float chip_c, chip_peak_c;
    uint16_t coil_ma;         // motor phase current (q axis), RMS over the last 0.5 s
    uint16_t coil_peak_ma;    // highest 100 ms RMS since the last reset
    float copper_w;           // resistive heat in the windings, last 0.5 s

    // CPU
    uint8_t load[2];          // % busy per core, last 0.5 s
    uint8_t load_peak[2];
    float loop_khz;           // measured control-loop rate
    float work_avg_us;        // compute time of one control iteration, last 0.5 s
    float work_max_us;        // longest iteration since the last reset
    float jitter_max_us;      // worst deviation of the loop period from its nominal 100 us
    uint32_t missed;          // control ticks skipped since the last reset (an iteration overran)
    float spikes_per_s;       // iterations over SYSMON_SPIKE_US, last 0.5 s -- the rate names the
                              // source (100/s: the FreeRTOS tick, 30/s: the LED frame, ...)
    float sec_avg_us[SYSMON_SEC_COUNT]; // per part, per iteration, last 0.5 s
    float sec_max_us[SYSMON_SEC_COUNT]; // longest per part since the last reset
    float other_avg_us;       // the rest of an iteration (work_avg_us minus the parts)
    uint32_t sensor_crc_errors; // MT6701 frames with a bad CRC since the last reset

    // System
    uint32_t heap_free, heap_min; // internal RAM, bytes
    uint32_t hid_drops;           // HID reports that never reached the host since the last reset
    uint32_t uptime_s;
} sysmon_info_t;

// Starts the Core 1 task that samples everything twice a second (and logs a line every 5 s).
void sysmon_start(void);

// Any Core 1 task: the latest refresh.
void sysmon_get(sysmon_info_t *out);

// SYS INFO's F1: peaks and counters start over (the counters show "since reset"). Any core.
void sysmon_reset_peaks(void);

// --- producers ---

// control_task.c, every tick (Core 0): cycles spent on the last iteration, cycles since the
// previous wake-up, the notification count ulTaskNotifyTake() returned (above 1 = ticks were
// missed), and the q- and d-axis voltages currently applied to the motor.
void sysmon_control_tick(uint32_t work_cycles, uint32_t period_cycles, uint32_t notified, float vq, float vd);

// control_task.c (Core 0): cycles one part of the current iteration took.
void sysmon_control_section(sysmon_section_t sec, uint32_t cycles);

// A HID report that was dropped (queue full, or the endpoint stayed busy). Any core.
void sysmon_note_hid_drop(void);

// led_task.c, each frame: its estimated LED current, mA.
void sysmon_set_led_ma(float ma);
