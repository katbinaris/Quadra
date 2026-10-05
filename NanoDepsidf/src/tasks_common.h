#pragma once

#include "esp_attr.h"

// Code the control loop runs every tick, placed in IRAM. From flash it goes through the cache
// Core 1 shares (display, USB, LittleFS, cJSON), and every miss there is a stall on Core 0 --
// SYS INFO's spikes. The per-tick path and the detent crossing (click, menu step, APP-mode
// step: left in flash, one crossing cost ~90 us of FORCE); a key press stays in flash. The IDF side of the tick (FreeRTOS, SPI master, MCPWM, GPIO) is placed by
// sdkconfig.defaults.
#define CONTROL_HOT IRAM_ATTR

// Core split decided in DEVELOPMENT_PLAN.md: Core 0 is exclusive to the control loop
// (FOC + sensor + key read + mapping, once those phases land) to keep it deterministic.
// Core 1 carries everything DMA-offloaded/tolerant: USB, I2S, display, LED.
#define CORE_CONTROL 0
#define CORE_IO      1

// PSRAM stacks: internal RAM is short since the 115 KB frame sprite moved there (display_task.cpp
// frame_init), so the tasks that can, keep their stacks in PSRAM (xTaskCreatePinnedToCoreWithCaps,
// MALLOC_CAP_SPIRAM): LED, sysmon, home and midi. A task qualifies only if it never writes flash (NVS,
// LittleFS -- a write turns the PSRAM cache off, and a task whose stack lives there can't run),
// never hands a stack buffer to DMA, and never ends itself (vTaskDeleteWithCaps from another
// task only; the one-shot PD task frees its internal stack by ending). Not the control task, USB (profile saves), menu_save,
// net / net_link (NVS, crypto), I2S (audio DMA) or the display (LCD DMA, frame rate).

// Priorities within Core 1. Core 0's control task sits above all of them since it's the
// one task that must never be starved.
//
// I2S and display were originally USB > I2S > LVGL > LED, on the reasoning that "a late
// HID report or audio underrun is user-perceptible in a way a late screen redraw or LED
// frame isn't" -- true back when the display only showed a passive, slowly-changing number
// (Phase 4). No longer true once Phase 8 added an animated, interactively-scrolled menu:
// a stuttery scroll animation is very perceptible. Found on hardware: `i2s_task.c`'s
// while(1) loop has no explicit yield -- it calls `i2s_channel_write(..., portMAX_DELAY)`
// back to back continuously, even synthesizing silence between clicks. ESP-IDF's FreeRTOS
// is strictly priority-preemptive across different priorities (no time-slicing except
// between tasks of the SAME priority), so at PRIO_I2S > PRIO_DISPLAY, the display task can
// only run during the brief windows where I2S actually blocks on its DMA queue -- and
// deepening that queue (done separately, to fix an audio-glitch-during-menu-redraw bug)
// makes those windows rarer, not more frequent. Equalized I2S and DISPLAY so FreeRTOS
// time-slices between them instead of I2S being able to fully starve display.
//
// TinyUSB's own device task (the stack that actually moves reports onto the bus) is created
// by esp_tinyusb at priority 5 on Core 1 unless told otherwise -- *below* the display. Found
// on hardware with Plasticity's shape animation: while the shape moves, display_task yields
// frame to frame (taskYIELD only hands over to equal-or-higher priority), so the TinyUSB
// task never ran and HID reports only went out once the animation settled. It now sits just
// under usb_task, above everything else on Core 1.
#define PRIO_CONTROL 20
#define PRIO_USB     12
#define PRIO_TINYUSB 11
#define PRIO_I2S     9
#define PRIO_DISPLAY 9
#define PRIO_LED     10 // above the display: PACE_FAST only yields, which would starve it;
                           // it sleeps between its 30 fps frames, so it starves nothing
#define PRIO_PD      10 // one-shot USB power read at boot (pd_status.c); above the display for
                        // the same reason, and it mostly waits on I2C
#define PRIO_SYSMON  10 // SYS INFO sampling (sysmon.c), twice a second; same reason again
#define PRIO_STORE   10 // F2's NVS save (menu.c), off Core 0; mostly asleep, same reason again
#define PRIO_NET     5  // WiFi housekeeping (net.c): reconnects, RSSI; the radio's own work is in
                        // its driver's task and lwIP's, both pinned to Core 1 (sdkconfig)
#define PRIO_NET_LINK 10 // the companion over WiFi (net_link.c): its socket, crypto, reports. Above
                         // the display, same reason as the LEDs: at 8 a departed client went
                         // unnoticed for seconds and the next one's handshake timed out. It
                         // sleeps in select() between frames
#define PRIO_MIDI    10 // MIDI mode (midi.c): a knob turn to the synth within one 10 ms pass; above
                        // the display for the same reason as the LEDs, and it sleeps between passes

// Control loop rate. Was a 1kHz placeholder through all of Phase 2's bench-validation work.
// RAISED to 10kHz once for haptic-feel tuning and immediately REVERTED: at the time, IDLE0
// (which the task watchdog monitors by default) got starved because the control task
// pinned to Core 0 at top priority left it no headroom, tripping the watchdog within
// seconds. RAISED AGAIN now that that's actually fixed at the root: Core 0 is
// architecturally dedicated exclusively to this one real-time task (see
// DEVELOPMENT_PLAN.md), so intentionally saturating it is correct, not a bug -- disabled
// IDLE0-specific watchdog monitoring instead (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n in
// sdkconfig.defaults; IDLE1/Core 1 is still monitored normally). All duration-based
// *_ITERS constants in control_task.c go through MS_TO_ITERS()/ITERS_TO_SEC() so real-world
// seconds are preserved regardless of this value.
#define CONTROL_LOOP_PERIOD_US 100
