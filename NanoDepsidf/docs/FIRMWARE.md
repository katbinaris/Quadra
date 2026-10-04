# Quadra firmware: how it works

This document explains the firmware in `NanoDepsidf/`: what runs where, how the knob's feel is
produced, how data moves between the parts, and what to keep in mind when changing it. It is
for someone who will read or change the code. For what the device does from the user's side,
see the [README](../../README.md).

All paths below are relative to `NanoDepsidf/`.

## Contents

1. [Overview](#1-overview)
2. [Boot sequence](#2-boot-sequence)
3. [Tasks and cores](#3-tasks-and-cores)
4. [The control loop](#4-the-control-loop)
5. [Timing and code placement](#5-timing-and-code-placement)
6. [How the parts talk to each other](#6-how-the-parts-talk-to-each-other)
7. [Menu and settings](#7-menu-and-settings)
8. [APP mode and profiles](#8-app-mode-and-profiles)
9. [USB](#9-usb)
10. [The companion protocol](#10-the-companion-protocol)
11. [Audio](#11-audio)
12. [Display](#12-display)
13. [LEDs](#13-leds)
14. [SYS INFO](#14-sys-info)
15. [Flash layout and storage](#15-flash-layout-and-storage)
16. [Building, and things that bite](#16-building-and-things-that-bite)
17. [Rules for changing the control loop](#17-rules-for-changing-the-control-loop)

---

## 1. Overview

Quadra is an ESP32-S3 (two cores at 240 MHz, 4 MB flash, 2 MB PSRAM) driving a small brushless
motor under a knob. The firmware reads the knob's angle 10,000 times a second and commands a
motor voltage that makes the knob feel like detents, a smooth bump, a viscous drag or an end
stop. The same loop turns knob travel and four keys into USB input for the computer.

The firmware is ESP-IDF 6.1, built with PlatformIO. It is C throughout, except the display
layer, which is C++ because of LovyanGFX.

One design rule shapes everything else: **Core 0 runs only the control loop.** Everything that
can wait a few milliseconds (USB, audio, display, LEDs, storage, the companion link) runs on
Core 1.

```
            Core 0                                   Core 1
  ┌──────────────────────────┐        ┌───────────────────────────────────────┐
  │ control task, 10 kHz     │        │ usb        HID state sync, companion   │
  │  keys → menu / APP mode  │ atomics│ TinyUSB    the USB stack               │
  │  encoder → FOC → PWM     │ ─────► │ i2s        click synthesis             │
  │  detents, clicks         │ queues │ display    frames to the round LCD     │
  │                          │        │ led        ring + key LEDs             │
  │                          │ ◄───── │ menu_save  NVS commits                 │
  │                          │ atomics│ sysmon     SYS INFO sampling           │
  └──────────────────────────┘        └───────────────────────────────────────┘
```

## 2. Boot sequence

`src/main.c`, `app_main()`, runs on Core 0:

1. Logs the reset reason, then initialises NVS (erasing and retrying if the partition needs it).
2. Creates the queues and shared state: `ipc_init()`, `audio_trigger_init()`, `ui_state_init()`.
3. `app_profiles_init()` mounts the profile store and loads stored profiles. It runs before
   `menu_init()`, which looks the saved profile up by its id.
4. `menu_init()` restores the saved settings from NVS and starts the `menu_save` task.
5. `icon_store_init()`.
6. **USB mode select.** For about 2 seconds it checks whether F3 + F4 are held. If they are,
   or if the saved BOOT MODE is SERIAL, TinyUSB is not installed for this boot. The chip then
   stays in its built-in USB serial/JTAG mode, which the uploader can always reach. The key
   combination always wins over the saved setting, so a saved setting can never lock the board
   out.
7. Starts the tasks: control (Core 0), then usb (unless in serial mode), i2s, display, led, pd
   and sysmon (Core 1).

The control task then waits 3 seconds, initialises the encoder and motor driver, and loads the
motor calibration from NVS. If there is none, it calibrates first (section 4.2). After that it
runs the haptic loop for as long as the device is on.

Holding **F1 during boot** arms a bench diagnostic instead of the haptic loop: F1 alone runs a
closed-loop position test, F1 + F2 also forces a new calibration, F1 + F3 runs an open-loop
spin, and F1 + F4 measures the pole-pair count. These are bring-up tools and each has a hard
time limit.

## 3. Tasks and cores

Priorities and core assignments are in `src/tasks_common.h`.

| Task | Core | Priority | Source | Job |
|---|---|---|---|---|
| `control` | 0 | 20 | `control_task.c` | The 10 kHz loop: keys, menu input, APP-mode engine, encoder, haptics, FOC, PWM |
| `usb` | 1 | 12 | `usb_task.c`, `host_link.c` | Brings the host in line with the wanted HID state; companion replies and streams |
| TinyUSB | 1 | 11 | esp_tinyusb | The USB stack; receives vendor reports |
| `led` | 1 | 10 | `led_task.c` | LED ring and key LEDs, 30 fps |
| `sysmon` | 1 | 10 | `sysmon.c` | SYS INFO sampling twice a second |
| `menu_save` | 1 | 10 | `menu.c` | Runs F2's NVS commit; asleep otherwise |
| `pd` | 1 | 10 | `pd_status.c` | Reads the USB-C power contract once at boot, then exits |
| `i2s` | 1 | 9 | `i2s_task.c` | Click and chime synthesis |
| `display` | 1 | 9 | `display_task.cpp` | Draws frames and pushes them to the LCD |
| `net_link` | 1 | 10 | `net_link.c` | The companion over WiFi: socket, handshake, AES-GCM (section 10.2) |
| `net` | 1 | 5 | `net.c` | WiFi: connecting, reconnecting with a growing pause, RSSI; starts SNTP and mDNS |

Why the priorities are what they are:

- `i2s` and `display` share a priority so FreeRTOS time-slices between them. The i2s task never
  sleeps on its own, so at a higher priority it would starve the display.
- TinyUSB sits above the display. The display yields between frames during animations, and a
  yield only hands over to equal or higher priority, so a lower TinyUSB task would not run
  until the animation ended.
- `led`, `sysmon`, `menu_save`, `pd` and `net_link` sit above the display for the same
  reason. They sleep most of the time, so they starve nothing.
- WiFi stays off Core 0. The radio's task, lwIP and mDNS are pinned to Core 1, and
  `esp_wifi_init()` runs from a Core 1 task so the radio's interrupt lands there. ESP-IDF's
  event task is fixed on Core 0 at the control task's priority, so the WiFi event handlers
  only note the event and wake `net`.
- The esp_timer task and interrupt run on Core 1 (`CONFIG_ESP_TIMER_TASK_AFFINITY_CPU1`,
  `..._ISR_AFFINITY_CPU1`). In modem sleep the WiFi driver uses esp_timer at every beacon; on
  Core 0 that cost the control loop 36 missed ticks in 20 s.

The idle task on Core 0 is not watched by the task watchdog
(`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n`), because the control task is meant to keep that
core busy.

## 4. The control loop

### 4.1 Pacing

A hardware timer (gptimer, 1 MHz count, alarm every `CONTROL_LOOP_PERIOD_US` = 100 µs) raises
an interrupt on Core 0. The interrupt handler does one thing: it notifies the control task.
The task wakes, does one iteration, and blocks again until the next notification.

If an iteration runs past the next alarm, the notification count is above 1 when the task
next wakes. SYS INFO reports that as **missed** ticks.

All durations in the loop are written as real time and converted with `MS_TO_ITERS()`, and
filters are written as time constants divided by the loop period. Changing the loop rate
therefore does not change what a constant means.

### 4.2 Calibration

`foc_calibration.c` finds two things the FOC maths needs: the electrical angle offset between
the encoder and the motor's windings, and whether the encoder counts in the same direction as
the motor's electrical rotation.

1. **Align.** It ramps a direct-axis voltage at electrical angle 0. That pulls the rotor to a
   known electrical position without producing continuous torque. It reads the encoder there.
2. **Step.** It moves the commanded angle by 45 electrical degrees and reads the encoder
   again. The sign of the movement gives the direction. If the rotor barely moved, calibration
   fails and the motor stays disabled.

The result is stored in NVS (`foc_cal`) and reused on every boot. DEVICE → RECALIBRATE erases
it and restarts.

### 4.3 One iteration

In the normal haptic mode, one iteration has four timed sections. SYS INFO shows each one's
average and maximum.

| Section | What it does | Typical |
|---|---|---|
| INPUT | Reads the four keys, publishes the held mask, runs the APP-mode engine (`app_mode_update`), handles menu key presses | 3.4 µs |
| SENSOR | One SPI transaction to the MT6701 encoder at 10 MHz; checks the CRC | 13.5 µs |
| FORCE | Finds the nearest detent, computes velocity, applies the haptic law, handles a detent crossing | 4.1 µs |
| MOTOR | Inverse Park and inverse Clarke, then three PWM compare updates | 3.9 µs |

A whole iteration averages about 26 µs of its 100 µs budget.

### 4.4 From angle to motor voltage

The encoder gives a 14-bit mechanical angle. The electrical angle is

```
elec = direction × mech × pole_pairs − electrical_offset        (pole_pairs = 7)
```

The haptic law (section 4.5) produces one number, `vq`: the quadrature-axis voltage. Positive
`vq` is torque in one direction, negative in the other. The direct-axis voltage is always 0.
`foc_math.c` rotates `(0, vq)` by the electrical angle (inverse Park) and splits it into three
phase voltages (inverse Clarke). `motor_driver.c` turns each phase voltage into a PWM duty
around 50%, clamped away from 0% and 100%.

This is **voltage-mode FOC**: there is no current sensing. Current is limited indirectly by
capping `vq` (`motor_config.h`: 0.756 A × 2.645 Ω ≈ 2.0 V) and by slew-limiting it. The motor
runs from USB 5 V, and the PWM carrier is 32 kHz (MCPWM, centre-aligned).

### 4.5 Haptics

Each tick:

1. **Nearest detent, with hysteresis.** The knob's travel since boot is divided into
   `num_detents` equal steps per turn. The loop stays with the detent it last committed to
   until the knob passes the midpoint by an extra 15% of the spacing. Without that margin,
   sensor noise at a midpoint would flip the choice back and forth.
2. **Velocity.** The angle difference from the last tick, low-pass filtered with a 6.7 ms time
   constant.
3. **The law.** The active haptic profile's feel (below) selects one:
   - **SAW:** `vq = kp × gain × error − kd × velocity`, a spring toward the detent plus damping.
     `gain = 1 − shape + shape × u²`, where `u` runs from 0 at the detent to 1 at the midpoint
     to the next one. With SHAPE at 0 the gain is 1 (a straight line). Higher values soften
     the centre and steepen the rise near the midpoint; the force at the midpoint is the same.
   - **SINE:** `vq = −kp × sin(num_detents × position) − kd × velocity`, a smooth bump.
   - **VISCOSE:** `vq = −kd × velocity`, damping only, no detents.
4. **Coasting.** Above 30 rad/s (a fast flick), SAW and SINE apply no torque, so the knob spins
   freely on momentum. VISCOSE keeps damping at any speed.
5. **Clamp and slew limit.**

`kp`, `kd`, `shape`, the feel and the detent count are those of the **active haptic
profile**. There are five (`HAPTIC_PROFILES` in `haptic_params.h`): WIDE 8, COARSE 12,
MEDIUM 24, FINE 36 and SMOOTH (VISCOSE only). Each has factory values and limits per feel;
`menu.c` holds the live values (atomics, one set per profile and feel) and clamps every
change into the profile's limits for that feel. Each tick the loop names the active profile
(`menu_haptic_set_active`): the one the Haptics screen shows while the menu is open,
otherwise the HID type's own; in APP mode the live input's (`app_mode_haptics`: VISCOSE maps
to SMOOTH, a detent count to the nearest stepped profile). Parameter mode picks a profile
per step, finest first: free FINE, then MEDIUM, COARSE, WIDE (a number field: FINE, MEDIUM,
COARSE). Per-tick reads come from RAM; the table itself is in flash and is read only
when a setting changes.

Factory values (tuned on hardware, 2026-10-02). The stepped profiles offer SAW and SINE;
SMOOTH is VISCOSE only. A stepped profile's other feel starts from the same numbers. The
limits are still the full ranges (SNAP 0-20, DAMP 0-0.15) and are to be narrowed per profile
and feel.

| Profile | Detents | Feel | SNAP | DAMP | SHAPE | AMP | PITCH |
|---|---|---|---|---|---|---|---|
| WIDE | 8 | SAW | 6.00 | 0.005 | 25% | 100% | 0.85x |
| COARSE | 12 | SINE | 2.00 | 0.035 | – | 100% | 0.90x |
| MEDIUM | 24 | SAW | 4.00 | 0.115 | 55% | 90% | 1.20x |
| FINE | 36 | SAW | 1.50 | 0.150 | 80% | 70% | 1.95x |
| SMOOTH | – (24 virtual) | VISCOSE | – | 0.150 | – | 15% | 1.85x |

**A detent crossing** is the moment the committed detent index changes. It triggers:

- the **click pulse** (SAW only): a 4 ms, 100 Hz burst driven above the voltage cap so it
  clips, then a 20 ms decaying tail. It is added on top of the spring and is not slew-limited.
  It does not fire while coasting, because kicks during a free spin would add energy to it.
- the **audio click**: one call to `audio_trigger_click()`, at the active profile's AMP and
  PITCH. VISCOSE clicks too, on its virtual steps, but its AMP is 0 by default and at most
  20%.
- the **meaning** of the step: a menu move if the menu is open, an APP-mode step in APP mode,
  otherwise one mouse-wheel step queued for the USB task.

**End stops.** When the next detent would run past the end of a list or a value range, the
loop refuses the crossing and keeps the old detent. It then adds a stiffer spring that starts
at the switch point, so the wall comes in without a jump. SINE needs more care, because its
force is a wave that would push on towards the next step past the midpoint: towards an end,
the wave is followed only up to its peak (a quarter step out), and from there the push back
holds that peak and stiffens. The LEDs flash once per push.

**Direction.** `KNOB_DIRECTION` in `control_task.c` decides which way counts as forward for
everything a turn means. The motor and haptic maths stay in sensor coordinates.

**Safety stops.** A failed encoder read, or a position error larger than one whole detent
spacing (which signals a control bug, not normal use), disables the motor driver.

## 5. Timing and code placement

### 5.1 Why placement matters

Most firmware code runs from flash through a cache that both cores share. When Core 1 is busy
(display, USB, JSON parsing, the filesystem), it pushes Core 0's code out of that cache. Core 0
then stalls on reloads. With the whole loop in flash, this produced 700 to 1500 iterations a
second above 60 µs.

The loop's code therefore runs from **IRAM**: internal RAM that holds code and needs no cache.

| What | How it is placed |
|---|---|
| Our own per-tick and detent-crossing functions | The `CONTROL_HOT` attribute (`tasks_common.h`), which is `IRAM_ATTR` |
| FreeRTOS, SPI master transmit, MCPWM compare, GPIO | `sdkconfig`: `CONFIG_FREERTOS_IN_IRAM`, `CONFIG_SPI_MASTER_IN_IRAM`, `CONFIG_MCPWM_CTRL_FUNC_IN_IRAM`, `CONFIG_GPIO_CTRL_FUNC_IN_IRAM` |
| The C library's `sinf`, `cosf`, `roundf` | The linker fragment `control_hot.lf` |

Two different mechanisms are needed for a reason. PlatformIO compiles `src/` itself and links
those objects directly, outside ESP-IDF's linker-script generator, so a linker fragment cannot
reach our own code. The attribute can. The C library is the opposite: we cannot add an
attribute to it, but the generator does map it.

Code that runs only on a key press stays in flash on purpose.

IRAM and the heap come out of the same internal RAM. IRAM code is about 94 KB; every kilobyte
moved there is a kilobyte less heap. SYS INFO's SYSTEM page shows the free heap and its lowest
point.

The biggest single user of internal RAM is the screen's frame sprite (115 KB), kept there for
frame rate. What can live in the 2 MB of PSRAM does:
- Statics marked `EXT_RAM_BSS_ATTR` (icon_store's buffers, the idle word's blocks), and with
  that option ESP-IDF also puts the WiFi driver's and lwIP's statics there.
- The stacks of the LED and sysmon tasks (`tasks_common.h`, "PSRAM stacks", says which tasks
  may).
- mbedTLS / PSA allocations, and any plain `malloc()` of 4 KB or more.
- Our own large buffers ask for PSRAM by name (`heap_caps_malloc`).

The WiFi radio's buffers must stay internal (DMA), so they are kept few (`sdkconfig.defaults`).
The console prints each heap and every task's unused stack 10 s and 60 s after boot (`sys`
lines, `memory_report()` in sysmon.c).

### 5.2 Measured timing

On hardware, with peaks reset and the knob in use (2026-10-02):

| | Value |
|---|---|
| Loop rate | 10.00 kHz |
| Iteration, average / maximum | 25.7 / 42 µs |
| Jitter (worst deviation of the period from 100 µs) | 5 µs |
| Spikes (iterations over 60 µs) | 0 per second |
| Missed ticks | 0 |

### 5.3 Flash writes

While the flash chip erases or programs, the cache is switched off for **both cores**, and
Core 0 is parked until the write finishes. The control loop cannot run during that time, no
matter which core started the write. The motor keeps its last PWM duty.

Flash is written by:

- an F2 save or the companion's Save to knob (NVS);
- a companion profile save (LittleFS);
- calibration (once, or on RECALIBRATE).

F2's NVS commit runs in the `menu_save` task on Core 1, so NVS's own work no longer happens
inside a control tick; it used to cost one 11 ms iteration. The stall during the actual chip
write remains. The loop's worst case during a flash write has not been measured since the
timing work. ESP-IDF's `CONFIG_SPI_FLASH_AUTO_SUSPEND` would remove the stall, but it only
works with certain flash chips and has not been tried.

Logging can stall a task too: a console write waits on the USB serial port. The control loop
has no periodic logging for that reason, and after boot the console is set to warnings and
errors only (`SYSMON_QUIET_AFTER_BOOT` in `sysmon.c`).

## 6. How the parts talk to each other

The control task never blocks on another task. Almost everything it shares is a single atomic
word, written on one side and read on the other.

| Data | Mechanism | Writer → reader |
|---|---|---|
| Settings, haptic profiles (feel and tuning per profile and feel), the active profile | Atomics in `menu.c` | menu, companion, control → control, i2s, display |
| Menu navigation state | Small struct under a spinlock (`s_state_mux`) | control → display |
| Knob angle, detent, held keys, click and wall counters, screensaver | Atomics in `ui_state.c` | control ↔ display, led, companion |
| Audio clicks | Counter plus a small ring (`audio_trigger.c`) | control → i2s |
| Mouse-wheel steps (non-APP modes) | FreeRTOS queue, 32 deep, never blocks (`ipc.c`) | control → usb |
| APP mode: wanted buttons, modifier, pointer travel, wheel steps | Atomics in `app_mode.c` | control → usb |
| APP mode: key taps | Single-producer, single-consumer ring of 64 | control → usb |
| Loop timing | Plain statics on Core 0, handed over every 1000 ticks under a spinlock | control → sysmon |
| F2 save request | FreeRTOS queue, 4 deep, never blocks | control → menu_save |
| Active profile | Atomic pointer per registry slot | usb → control |

**State, not events.** In APP mode the control task does not queue "press" and "release". It
publishes what the host should currently see, and the usb task keeps sending reports until
the host matches. A report lost to a busy endpoint can then never leave a key or a mouse
button stuck down.

A click counter rather than a flag means two detents inside one audio poll still play as two
clicks.

## 7. Menu and settings

`menu.c` holds the menu's structure as data: screens, items, and for each value a formatter
and a rotate callback. The control task calls `menu_input_*()` for key presses and knob steps.
The display task takes a snapshot under the spinlock and does all string formatting outside it.

Every setting has three copies:

- the **live** value, an atomic. Turning the knob on a field changes it at once.
- the **saved** value (`s_saved`), what NVS holds. The difference between live and saved is the
  "unsaved" cue on screen.
- the **undo** value, captured when an edit starts, so F3 can cancel.

F2 hands the save to the `menu_save` task. `config_store.c` writes one blob per settings group,
each in its own NVS namespace: `hprof_cfg` (the haptic profiles: each one's feel and its
tuning per feel, versioned), `hmode_cfg` (the haptic profile per HID type), `hid_cfg`,
`boot_cfg`, `disp_cfg`, `bind_cfg`. On load, a blob with the wrong size or an out-of-range
value is rejected and the compiled-in default is kept; haptic values are also clamped into
their profile's limits. (`haptic_cfg`, the single global tuning from before haptic
profiles, is no longer read.)

On the Haptics screen a row that doesn't apply in the profile's current feel is **muted**:
it shows `--`, the knob skips it and it can't be edited (SNAP in VISCOSE, SHAPE outside
SAW, FEEL in SMOOTH). F2 there saves on release; held for 1.5 s it puts the shown profile
back to its factory values (live, unsaved) and the screen shows FACTORY. The active profile is stored by its id string, not its index, so adding or
reordering profiles does not change what a saved setting points at.

## 8. APP mode and profiles

### 8.1 The engine

`app_mode.c` runs inside the control task. It knows nothing about any particular application;
everything comes from the active profile.

A profile has five **slots**: the knob alone, and the knob while F1, F2, F3 or F4 is held.
Each slot has an action:

| Action | A turn does |
|---|---|
| DRAG | Holds mouse buttons and a modifier, and moves the pointer along one axis |
| WHEEL | Sends scroll steps, optionally with a modifier |
| KEYS | Sends one shortcut per detent, one for each direction |
| TAP | Nothing on a turn; the key press itself sends a shortcut or runs a macro |
| COMMANDS | Opens the command wheel; a turn picks an entry, releasing the key runs it |
| MEDIA | Sends one HID Consumer usage per detent (volume, play / pause, next, previous, mute), or one on a key press. MUSIC's volume knob uses it with Shift + Option, a quarter step on a Mac |

Each slot also names the haptic profile while it is live, through its feel and detent count
(VISCOSE: SMOOTH; a count: the nearest stepped profile; neither: the HID type's profile). A key can carry a quick-tap
action as well: pressed and released within 400 ms without turning, it sends a shortcut.
Holding F4 for 0.7 s without turning opens the menu.

Keys are debounced in the engine (15 ms stable). The knob-alone slot starts when the knob
moves more than sensor noise and lets go after 250 ms at rest.

**Parameter mode** is entered by a command that has a parameter spec. The knob then steps a
value, the keys choose an axis or a step size, and the engine sends the keystrokes that put
the application into value entry. Each step size has its own haptic profile, finest first
(section 4.5).

**Macros** are lists of key, text and wait steps. The engine feeds them into the tap ring a
little each tick as room allows, so a macro can be longer than the ring.

### 8.2 The registry

`app_profiles/app_profiles.c` keeps up to 16 profiles:

- the **built-ins**, one C file each in `app_profiles/`;
- **stored** profiles, JSON files on the device. A stored file with a built-in's id overrides
  it; any other id is a profile of its own.
- a **live edit** per profile from the companion: in use at once, but not stored until saved.

Each profile is checked once before use (`app_profiles_valid`). A profile that fails is
replaced by an empty template, so a bad profile cannot crash the engine.

The control loop reads the active profile through an atomic pointer every tick. Changes come
only from the usb task. A replaced profile stays allocated for a few seconds
(`app_profiles_reap`) so a task still holding the old pointer finishes safely.

### 8.3 Storage and JSON

`profile_store.c` keeps profiles as `/profiles/<id>.json` on a LittleFS partition. A write goes
to a temporary file that is then renamed, so a reset mid-write never leaves half a profile.

`app_profiles/profile_json.c` converts between JSON text and the `app_profile_t` structure. A
parsed profile owns its memory: the structure itself (with the slots the control loop reads
every tick) is in internal RAM, and the larger parts (rings, macros, text, icons) are in PSRAM.

`tools/profile_json_test/run.sh` builds this code on the host and checks that every built-in
survives a round trip unchanged and that bad input is refused with a reason.

## 9. USB

`usb_task.c` installs one composite TinyUSB device:

| Interface | Endpoints | Purpose |
|---|---|---|
| CDC-ACM | EP1 IN, EP2 IN/OUT | Serial console, and the 1200-baud reset the uploader uses |
| HID | EP3 IN | Keyboard, mouse, gamepad and Consumer (media keys) as four report IDs; polled every 10 ms |
| Vendor HID | EP4 IN/OUT | 64-byte raw reports for the companion and icon upload |

The vendor interface is separate from the keyboard interface so host tools can open it without
the operating system's keyboard-access permission.

The usb task waits on the wheel queue with a one-tick timeout, so it wakes at least every
10 ms. Each pass it:

1. serves the companion link (`host_link_poll`);
2. sends a queued mouse-wheel step, if any;
3. runs `app_sync`: compares what the host currently holds (buttons, modifier) with what the
   engine wants, and sends reports until they match; then pointer travel, wheel steps and
   queued key taps.

If a report cannot be sent, the travel or steps are handed back to the engine and retried on
the next pass.

DEVICE → BINDINGS (MAC / PC) is applied here: on PC, Cmd is sent as Ctrl.

## 10. The companion protocol

`host_proto.h` defines it; `host_link.c` implements it; the desktop app mirrors it in
`companion/src/proto.ts`. **Change both together.**

Every message is one 64-byte report on the vendor interface. Byte 0 is the command (host to
device) or the reply tag (device to host). Fields are little-endian.

| Group | Commands |
|---|---|
| Handshake | `HELLO` returns the protocol version, profile count, boot mode and firmware version |
| Settings | `GET_SETTINGS`, `SET` (applied live, clamped like the menu), `SAVE`, `REVERT`, `HAPTIC_RESET` |
| Live state | `STREAM` at up to 50 Hz: knob angle, detent, keys, menu screen; SYS INFO twice a second; LED colours about 15 times a second |
| Profiles | `PROFILE` (summary), `PROFILE_ICON`, `PROFILE_READ` (the JSON, in 60-byte pieces), `UPLOAD_BEGIN` / `DATA` / `END`, `PROFILE_OP` (save, revert, remove) |
| Other | `RESET_PEAKS` |

The protocol version is **3**. The haptic values in `SETTINGS` (KP, KD, SHAPE, FEEL, AMP,
PITCH) are those of one haptic profile, the one the Haptics screen shows, in its current
feel; the reply also carries that profile's id, the feels it allows and its limits, and the
HID type's own haptic profile. `SET HAPTIC_PROFILE` chooses which profile the values are of,
`SET MODE_HAPTIC` sets the current HID type's profile, and `HAPTIC_RESET` puts the shown
profile back to factory. `SET DETENTS`, from older apps, shows the nearest stepped profile.

Whole profiles travel as JSON text with a CRC-32 over the complete text. Uploads must arrive
in order; anything else fails the transfer rather than applying a damaged profile.

Incoming reports arrive in the TinyUSB task. Replies are queued and sent from the usb task,
together with the streams, so two senders never race for the one IN endpoint. A profile
download is the exception: each piece is sent from TinyUSB's "report sent" callback, one per
1 ms host poll.

Icon upload (`icon_store.c`, commands 0x01 to 0x04) shares the interface. Uploaded icons are
held in RAM and lost on restart.

### 10.1 Extensions

`ext_proto.h` and `ext_link.c` add commands 0x20 to 0x2F (replies and events 0xC0 to 0xCF),
mirrored in `proto.ts` as `ExtCmd` / `ExtTag`. `EXT_CMD_HELLO` returns the extensions version,
and the companion shows a feature only from the version that has it:

| Version | Adds |
|---|---|
| 1–3 | `REBOOT` (also into SERIAL, for buttonless flashing), `TEXT` (the idle word), `LIGHTS`, `PREFS`, `NOTIFY` (agent requests and their answers), `COVER` (a 240×240 JPEG, from one link at a time), `TRACK` (title, artist, colours, volume), `AGENTS` (the dashboard). The companion's Look page needs any of these |
| 4 | `NET`: WiFi setup and status |
| 5 | `TIME` (from the Mac service), `CLOCK` (format and zones) |
| 6 | `SCREEN` (the live screen: changed 16×16 tiles, RLE when shorter) and `INPUT` (keys and turns from a host). The companion used them as a live remote until its redesign (2026-10); it asks for neither now |
| 7 | `NET KEY`: the WiFi pairing key |
| 8 | `MUSIC`: the now-playing cover style (`PREFS` reports it, and how many styles there are) |
| 9 | `PD`: the USB-PD chip's NVM, read and checked, or written to 5 V 3 A only (USB only, `quadra.py pd`). Also two WiFi clients at once, and `COVER` over WiFi |
| 10 | `NET CONTROLS` and `EXT_TAG_HID`: with no USB host, the HID reports go to the WiFi client that asked (the companion app types and scrolls for the knob) |

Work that touches NVS or decodes images runs in the usb task (`ext_link_poll`), not in
TinyUSB's callback. Keys from `INPUT` are OR-ed into the real ones in the control loop and let
go 600 ms after the last refresh, so a lost link can't leave one held. The one exception is
approving an agent request (`notify.c`): only the physical F1 counts there.

### 10.2 Over WiFi

`net_link.c` serves the same 64-byte reports over TCP port 3333 (mDNS `_quadra._tcp`) to two
clients at once, the companion app and the Mac service. A newly authenticated client takes a
free slot, or else the slot of the one that has been quiet the longest (a live client sends
something every second or two, so that's one that left without closing). Not by address: both
come from the same computer.

- **Pairing** is over USB only. The knob makes a random 256-bit key (`NET KEY`) and the
  companion keeps it.
- **Handshake:** both sides prove the key with HMAC-SHA256 over two fresh nonces. Every report
  after that is AES-256-GCM with a per-direction counter, and a frame that doesn't verify
  ends the connection. The format is in `net_link.h`.
- **Strangers:** handshakes run side by side, one slot per peer address, with a 2 s
  deadline. A peer that stalls or floods never holds up the companion that's in
  (`net_pend.h`, `tools/net_pend_test`).
- **USB only:** the WiFi network and password, the key, SERIAL boot, and icon uploads
  (`tools/send_icon.py`). The cover comes over either; its acknowledgement goes to the link
  that began it.

**The controls over WiFi:** with no USB host, `usb_task.c` sends the reports USB would have
carried (keyboard, mouse, media keys; the whole state each time) to the WiFi client that asked
with `NET CONTROLS` -- the companion app, which posts them as macOS events
(`companion/src-tauri/src/input.rs`, Accessibility permission). Everything upstream (APP mode,
BINDINGS, macros) is the same. Leaving that client lets go of whatever it held; the app also
lets go when the link drops or goes quiet for 1.5 s with something held.

`host_link.c` serves the links side by side (USB and one per WiFi client): replies go back on
the link that asked, ahead of the streams. An agent approval's answer goes to every link, the
way every program that opens the vendor HID interface sees every report.

The extensions, WiFi, MUSIC, AGENTS, CLOCK, LIGHTS and the idle word were contributed by
[@Dviros](https://github.com/Dviros) in
[pull request #17](https://github.com/katbinaris/NanoD_RatchetH1/pull/17).

## 11. Audio

`i2s_task.c` synthesises every sound; there are no audio files. It writes 64-sample chunks at
44.1 kHz to a MAX98357A amplifier and checks for a new click on every sample.

- **Detent click:** one of two timbres (a short pitched "wood" tock, or a tick with a low
  thud), at the active haptic profile's PITCH and AMP.
- **Button thump** and the **startup chime**.

Oscillators read a 256-entry sine table rather than calling `sinf`. SYS INFO counts **audio
gaps**: times the driver ran out of fresh samples.

## 12. Display

`display_task.cpp` decides *when* to draw; `ui_screens.cpp`, `ui_cards.cpp`, `ui_shape.cpp` and
`ui_fx.cpp` decide *what*; `ui_gfx.cpp` has the pixel primitives and the font.

One 240×240 RGB565 sprite in internal RAM is redrawn completely and pushed over SPI with
LovyanGFX: about 1 ms to draw and 12 ms to push. Frames are produced only when needed:

- on any visible change (menu snapshot, held keys, view change);
- at about 30 fps while something loops (the idle screen, the loading screen);
- back to back during short transitions (the iris wipe between views, list scrolls).

Otherwise the task polls every 30 ms.

**MUSIC's record** (`ui_vinyl.cpp`, the RECORD, SLIDE and BLEED cover styles in
`user_prefs.h`) is the one screen drawn per pixel instead of from rectangles. Its buffers take
about 130 KB of PSRAM the first time a record style is shown, and its per-pixel tables about
6 KB of internal RAM. Each layout's grooves, lit by a fixed light, are drawn once into a cache
at 4 bits a pixel (an index into the 10 greys), and the label and sleeve are averaged from the
cover once per cover. A frame expands the cache, redraws only the pixels the light can reach
(the groove grain turns through them), places about 80 dust specks, and rotates the label
nearest neighbour by stepping through it in fixed point along each row. PSRAM is quad SPI with
a 32 KB cache, so it is read only in order. While the record moves, it is drawn every third
10 ms tick (about 33 fps; a pace between tick multiples would alternate 30 and 40 ms frames),
the volume ring and key glyphs included, and a host's live screen (`SCREEN`) gets 5 of those
frames a second. Once the record has stopped, the screen is drawn only on a change. The console
shows the frame rate and the draw and push times every 2 s while it moves. The
motion is a function of the time since the last play or pause, like every other animation. A
tap of F4 on the now-playing screen (no turn, released within 0.6 s, before the menu's long
press) picks the next style; the usb task stores it (`user_prefs_poll`). The idle screen starts after 5 seconds without input;
the first key press only wakes the screen and is not passed on.

The same UI code compiles on the host: `tools/ui_preview/run.sh` renders every screen to a PNG
without hardware. How the pixel art is drawn, and the rules for changing it, are in
[PIXEL_ART.md](PIXEL_ART.md).

## 13. LEDs

`led_task.c` drives two WS2811 strips over RMT at 30 fps: 60 LEDs around the knob and 8 under
the keys. Colours follow the active profile (`app_colors.c`).

- At rest: a dim gradient.
- While turning: a bright spot follows the knob and pulses on each click.
- Command wheel open: one segment per entry.
- End stop: a short white flash.

Brightness is 20% at the standard level; LIGHTS → LEVEL scales it from 10% to 200% of that
(`user_prefs.c`, which also holds the colour and the effect at rest). The whole frame is then
scaled so the estimated draw stays under the LED budget: what the USB port offers (`pd_status`) less 400 mA for the board and the motor,
between 60 and 250 mA. That is 100 mA on a plain 500 mA port and 250 mA from 1.5 A up. With
WiFi on, the radio's ~100 mA comes off it. A strip is sent only when its data changed.

Other ring states: an agent request breathes in the agent's colour, with one arc per waiting
agent; MUSIC shows the volume as an arc while the knob turns; CLOCK can sweep the seconds.

## 14. SYS INFO

`sysmon.c` collects the numbers behind DEVICE → SYS INFO and the companion's System info page.

- **Loop timing.** The control task adds each iteration's cycle counts to plain statics and
  hands the sums over every 1000 ticks. Reported: rate, average and maximum work, jitter,
  missed ticks, spikes per second, and the four sections.
- **Core load.** Whatever the idle task on each core did not get.
- **Power.** An estimate, not a measurement: motor current from `vq` and the phase resistance,
  LED current from the colours sent, and a fixed figure for the rest of the board.
- **Heat.** The chip's temperature sensor.
- **System.** Free heap and its minimum, dropped HID wheel events, audio gaps, encoder CRC
  errors, uptime.

Maximums are held until reset: F1 on a SYS INFO page, or `RESET_PEAKS` from the companion.

**To measure a change to the loop:** flash, reset the peaks, use the knob in the way the
change affects, then read the LOOP page. One event sets a maximum, so compare sessions that
did the same things.

## 15. Flash layout and storage

`boards/nano_partitions.csv`:

| Partition | Size | Use |
|---|---|---|
| `nvs` | 20 KB | Settings and motor calibration |
| `otadata` | 8 KB | OTA slot selection |
| `app0`, `app1` | 1.625 MiB each | Firmware (two OTA slots) |
| `spiffs` | 640 KiB | LittleFS: stored profiles |
| `coredump` | 64 KB | Crash dumps |

The firmware image is about 1.29 MB (76% of a slot), most of the growth being WiFi. NVS also
holds the namespaces `user_prefs` (idle word, LIGHTS, cover style), `clock` and `net` (WiFi network,
password and pairing key, in plain text: flash encryption is off).

The slots grew from 1.25 MB, and the profile store moved and shrank from 1.4 MB, when WiFi
came in. A device flashed before then needs the new table; `tools/quadra.py flash
--partitions` writes it and blanks the new profile store.

## 16. Building, and things that bite

```sh
cd NanoDepsidf
pio run                                   # build (platform pinned: platformio/espressif32@7.1.3)
pio run -t upload --upload-port <port>    # flash
tools/profile_json_test/run.sh            # host test for profile JSON
tools/ui_preview/run.sh out.png           # render every screen on the host
```

- **`sdkconfig.defaults` does not override a saved value.** The per-environment file
  `sdkconfig.esp32-s3-devkitm-1` wins for any option it already lists, including ones listed as
  "not set". Change an option in both files.
- **Editing `control_hot.lf` needs a fresh linker script.** PlatformIO does not regenerate it
  when only a fragment changes. Delete `.pio/build/<env>/sections.ld` or do a clean build, then
  check the symbol's address (section 17).
- **`src/` is globbed** (`src/CMakeLists.txt`). A new source file needs no build change. For
  the same reason, keep non-source files out of `src/`.
- **`src/app_profiles/*.c` also compiles on the host** for the JSON test. Do not include
  ESP-IDF-only headers there without an `ESP_PLATFORM` guard.
- **`FREERTOS_HZ` is 100.** `pdMS_TO_TICKS(1)` is 0, so a 1 ms delay is no delay. Use whole
  ticks.
- **PSRAM runs at 80 MHz.** If a board fails to boot or fails its PSRAM check, set
  `CONFIG_SPIRAM_SPEED_40M=y` in both sdkconfig files.
- **If an upload cannot find the device,** hold F3 + F4 while powering on (section 2).

## 17. Rules for changing the control loop

1. **Nothing in the loop may block.** No flash or NVS access, no logging on a normal path, no
   mutex another task can hold, no queue send with a timeout. Hand slow work to a Core 1 task,
   as `menu_input_save()` does.
2. **Share data through atomics** or, for a few words that must change together, a short
   spinlock section. Format strings and do other slow work outside the lock.
3. **Mark new per-tick or detent-crossing functions `CONTROL_HOT`,** including the static
   helpers they call. For a C library function, add its object to `control_hot.lf`.
4. **Check the placement.** IRAM addresses start with `0x4037` or `0x4038`; flash code starts
   with `0x42`:

   ```sh
   ~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp-elf-nm \
       .pio/build/esp32-s3-devkitm-1/firmware.elf | grep my_function
   ```

5. **Write durations as real time** (`MS_TO_ITERS()`, time constants), never as bare iteration
   counts.
6. **Measure on hardware** with SYS INFO before and after (section 14).
