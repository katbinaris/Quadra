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
8. [APP mode and profiles](#8-app-mode-and-profiles) (and [HOME](#84-home-mode-xiaomi-lamps), [MIDI](#85-midi-mode))
9. [USB](#9-usb)
10. [The companion protocol](#10-the-companion-protocol)
11. [Sound](#11-sound)
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
can wait a few milliseconds (USB, display, LEDs, storage, the companion link) runs on
Core 1.

```
            Core 0                                   Core 1
  ┌──────────────────────────┐        ┌───────────────────────────────────────┐
  │ control task, 10 kHz     │        │ usb        HID state sync, companion   │
  │  keys → menu / APP mode  │ atomics│ TinyUSB    the USB stack               │
  │  encoder → FOC → PWM     │ ─────► │ display    frames to the round LCD     │
  │  detents, clicks, sound  │ queues │ led        ring + key LEDs             │
  │                          │ ◄───── │ menu_save  NVS commits                 │
  │                          │ atomics│ sysmon     SYS INFO sampling           │
  └──────────────────────────┘        └───────────────────────────────────────┘
```

## 2. Boot sequence

`src/main.c`, `app_main()`, runs on Core 0:

1. Logs the reset reason, then initialises NVS (erasing and retrying if the partition needs it).
2. Creates the queues and shared state: `ipc_init()`, `motor_sound_init()` (which also starts the `sndcal` task), `ui_state_init()`.
3. `app_profiles_init()` mounts the profile store and loads stored profiles. It runs before
   `menu_init()`, which looks the saved profile up by its id.
4. `menu_init()` restores the saved settings from NVS and starts the `menu_save` task.
5. `icon_store_init()`.
6. **USB mode select.** For about 2 seconds it checks whether F3 + F4 are held. If they are,
   or if the saved BOOT MODE is SERIAL, TinyUSB is not installed for this boot. The chip then
   stays in its built-in USB serial/JTAG mode, which the uploader can always reach. The key
   combination always wins over the saved setting, so a saved setting can never lock the board
   out.
7. Starts the tasks: control (Core 0), then usb (unless in serial mode), display, led, pd
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
| `sndcal` | 1 | 10 | `motor_sound.c` | Runs DEVICE → SOUND CAL (section 11); polls every 50 ms otherwise |
| `pd` | 1 | 10 | `pd_status.c` | Reads the USB-C power contract once at boot, then exits |
| `display` | 1 | 9 | `display_task.cpp` | Draws frames and pushes them to the LCD |
| `net_link` | 1 | 10 | `net_link.c` | The companion over WiFi: socket, handshake, AES-GCM (section 10.2) |
| `net` | 1 | 5 | `net.c` | WiFi: connecting, reconnecting with a growing pause, RSSI; starts SNTP and mDNS |
| `home` | 1 | 5 | `home.c` | HOME (section 8.4): the lamps over miIO; wakes every 10 ms. Stack in PSRAM |
| `midi` | 1 | 10 | `midi.c` | MIDI (section 8.5): turns and keys into messages on USB MIDI and the TRS jacks, and what comes back; wakes every 10 ms. Stack in PSRAM |

Why the priorities are what they are:

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
| SENSOR | One 24-bit read of the MT6701 encoder at 10 MHz, on the SPI2 registers directly; checks the CRC | About 5 µs (2026-10-07; one read alone 3.8 µs). Through the driver it was 13.5 µs |
| FORCE | Finds the nearest detent, computes velocity, applies the haptic law, handles a detent crossing | 4.1 µs |
| MOTOR | Inverse Park and inverse Clarke, then three PWM compare updates | 3.9 µs |

A whole iteration averages about 26 µs of its 100 µs budget.

**The sensor read skips the SPI driver.** The 24 bits take 2.4 µs on the wire; the rest of the
driver's 13.5 µs was setting the same transaction up again every tick. The sensor is alone on
SPI2 and holds the bus, so `mt6701.c` lets the driver program the registers once and then
starts each read itself: clear the done flag, start, wait, take the data word. At boot, eight
direct reads must pass the CRC and agree with a driver read taken just before each; if not, or
if a direct read ever times out, it stays with the driver. Ten seconds after boot, next to the
memory report, two `mt6701:` lines say which it is, why, and what one read of each kind took.

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
(`menu_haptic_set_active`): MEDIUM while the menu is open, so the menu feels the same from
every mode (the Haptics screen runs on the profile it shows, except while STEPS is being
edited, which stays on MEDIUM so picking a profile is even steps);
otherwise the HID type's own; in APP mode the live input's (`app_mode_haptics`: VISCOSE maps
to SMOOTH, a detent count to the nearest stepped profile). Parameter mode picks a profile
per step, finest first: free FINE, then MEDIUM, COARSE, WIDE (a number field: FINE, MEDIUM,
COARSE). Per-tick reads come from RAM; the table itself is in flash and is read only
when a setting changes.

Factory values (tuned on hardware, read back from a knob 2026-10-07). The stepped profiles
offer SAW and SINE, each with its own values; SMOOTH is VISCOSE only. SNAP goes up to 4 in SAW
and up to 2 in SINE, in every stepped profile; a stored value over that is pulled down when it
is loaded. DAMP still has the full range (0-0.15).

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
- the **audible click**, at the active profile's AMP and PITCH: `motor_sound_click()`, a burst
  on the motor (section 11). VISCOSE clicks too, on its virtual steps, but its AMP is 0 by default and at most
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
| Settings, haptic profiles (feel and tuning per profile and feel), the active profile | Atomics in `menu.c` | menu, companion, control → control, display |
| Menu navigation state | Small struct under a spinlock (`s_state_mux`) | control → display |
| Knob angle, detent, held keys, click and wall counters, screensaver | Atomics in `ui_state.c` | control ↔ display, led, companion |
| The click in use (frequency, shape, axis) | Atomics in `motor_sound.c` | menu, sndcal → control |
| Mouse-wheel steps (non-APP modes) | FreeRTOS queue, 32 deep, never blocks (`ipc.c`) | control → usb |
| APP mode: wanted buttons, modifier, pointer travel, wheel steps | Atomics in `app_mode.c` | control → usb |
| APP mode: key taps | Single-producer, single-consumer ring of 64 | control → usb |
| Loop timing | Plain statics on Core 0, handed over every 1000 ticks under a spinlock | control → sysmon |
| F2 save request | FreeRTOS queue, 4 deep, never blocks | control → menu_save |
| HOME: turns and F1-F3 | Single-producer, single-consumer ring of 32 bytes (`home.c`) | control → home |
| HOME: end stops and the feel | Atomics in `home.c` | home → control |
| Active profile | Atomic pointer per registry slot | usb → control |

**State, not events.** In APP mode the control task does not queue "press" and "release". It
publishes what the host should currently see, and the usb task keeps sending reports until
the host matches. A report lost to a busy endpoint can then never leave a key or a mouse
button stuck down.

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
tuning per feel, versioned), `hmode_cfg` (the haptic profile per HID type -- the first four;
HOME has none, so the blob kept its size), `hid_cfg` (with the APP profile and MIDI's synth
next to it as strings, by id: `app`, `midi_synth`), `boot_cfg`, `disp_cfg`, `bind_cfg`, and
`snd_cal` (SOUND CAL's result, written by its own task; section 11). On load, a blob with the wrong size or an out-of-range
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

### 8.4 HOME mode (Xiaomi lamps)

HOME (`MENU_HID_HOME`, value 4, second in the PROFILES carousel) makes the knob a remote for
Xiaomi lamps on the local network. `home.c` speaks miIO to them directly; no cloud, no computer.

**Three sides.** The control loop only pushes turns and F1-F3 presses into a 32-byte lock-free
ring (`home_input_*`, `CONTROL_HOT`, internal RAM) and reads two atomics: the end-stop flags
(`home_at_end`: the ends of the list, brightness 1 / 100, the colour-temperature range; hue
wraps) and the haptic profile (`home_haptic_profile`: COARSE in the list, FINE while a value
turns). The `home` task owns everything else and publishes a snapshot (`home_get_snapshot`,
under a mutex) that the display copies once a tick and draws (`ui_extras.cpp` `draw_home`).

**miIO.** UDP port 54321. A 32-byte hello (all 0xFF after the magic) makes any miIO device
answer with its id and clock. A request is a 32-byte header (magic 0x2131, length, device id,
the device's clock plus our elapsed seconds, MD5 over header + token + body) and a JSON body
under AES-128-CBC with PKCS#7, key MD5(token), IV MD5(key + token). The AES is the chip's
(`esp_aes_*`) and MD5 is ROM's (`esp_rom_md5_*`); the packet buffers are internal RAM because
the AES driver may DMA them. Two dialects:
- **MIoT:** `get_properties` / `set_properties` by siid / piid, which come from each model's
  spec at import time, so the firmware has no table of models.
- **Legacy:** `get_prop` / `set_power` / `set_bright` / `set_ct_abx` / `set_rgb`, one property
  per request (the Yeelight-made `yeelink.light.lamp4` answers nothing else).

A lamp that answers hello but not two queries is tried in the other dialect.

**Finding the lamps.** On entering HOME (or F3, or after a minute away): hellos at 0, 0.5 and
1.2 s, each a broadcast plus one to every lamp's stored address (a broadcast doesn't cross a
router). After the first round, the subnets of the lamps still missing (not the knob's own: the
broadcast covers it, and 254 unicasts there would each need an ARP lookup) are swept: a hello to
each of their 254 addresses, 32 per 10 ms pass. F1 on an offline lamp sweeps its subnet too. A
hello reply is matched by device id, and one from a new address updates it for the session (the
import, which sweeps the same way, stores it). In the list, lamps that haven't answered
get a hello every 10 s, and the others are read again every 30 s; a read that gets no answer
marks the lamp offline.

**Changes.** A turn changes the wanted value at once (on screen); each lamp is sent its newest
values at most every 150 ms. A change without a reply in 1.5 s shows NO REPLY and sends a new
hello (the lamp may have restarted with a new clock). A refresh never overwrites a change still
on its way. Turning a value on a lamp that is off switches it on.

**Import.** The companion's Lamps › Import (or `quadra.py home import`; USB only: the tokens)
sends `EXT_CMD_HOME` BEGIN, one LAMP per lamp (id, address, token, dialect, capabilities,
colour-temperature range, siid / piid, name, icon) and COMMIT. The usb task stores the list in
NVS (`home` / `lamps`, one blob of up to 12, written from internal RAM) and the home task picks
it up and scans. `STATUS` (any link) reports what the knob sees of a lamp, never its token: from
extensions v12 also its icon, the address it answers on and its dialect.

**Edits** (v12, `EXT_HOME_EDIT`, any link: no tokens involved): rename, change the icon, move
or remove the lamp in a slot, if it is still the one with the device id the companion names.
`home_edit` changes the stored list and writes it at once (in the usb task, the same blob), then
the home task rebuilds its table in the new order keeping each lamp's session (online, its
state, its address), so nothing is scanned again and the selection stays on the same lamp. A
lamp's new address, found during a session, goes into the stored list with the next edit or
import.

**The screens, the LEDs, the idle screen.** `ui_extras.cpp` draws each lamp from shapes after
the real device (`HOME_KIND_*`: the import sends one per lamp from its model name; a lamp stored
before that has 0, a bulb, until it is imported again), the Mi badge in the header, and while a
lamp changes the value. The scale is the LED ring's (`led_task.c`): brightness fills it
clockwise from 12 o'clock, ceil(b × 60 / 100) LEDs so 1 % is one, at the resting level in the
list and dim to full while turned; the white scale runs over all 60 with the chosen one
brightest; the hue wheel has 6° per LED with the colour at 12 o'clock. A lamp that's off leaves the ring dark. The idle screen's icon is the lamp changed last
(`home_snapshot_t.last`), drawn once per change into a 48×48 image (`ui::home_idle_icon` through
`ui::target`, 2 × 4.6 KB of PSRAM) and handed to `fx_attract` like an app's icon: the same cost
as APP mode's idle screen.

**Memory.** About 2 KB of internal RAM (packet buffers, the ring), the 5 KB stack and the lamp
table in PSRAM, one UDP socket (the socket closes a minute after HOME is left), about 16 KB of
flash.

### 8.5 MIDI mode

MIDI (`MENU_HID_MIDI`, value 2) makes the knob a controller for a synth. `midi_synths.c` holds the
synth profiles as tables, one row per parameter, transcribed from each maker's MIDI document
(the sources are named in the file): GENERIC (General MIDI / GM2 controllers), KORG minilogue xd,
Roland JU-06A and TR-8S. The menu picks one (PROFILES → MIDI → SYNTH, NVS `hid` / `midi_synth`,
stored by id like the app profile) and the channel (`hid_cfg.midi_channel`).

**The list of synths** (from extensions v12) is the four built-ins, then up to 12 of the
user's own, at most 16 (`MIDI_MAX_SYNTHS`). The companion's Synths page edits them like app
profiles: an upload (`EXT_CMD_SYNTH` PUT, JSON) is live at once, replacing the synth with its id
or adding one; SAVE writes it to LittleFS (`/fs/synths/<id>.json`, indented, to be read and
shared); REVERT goes back to the file, or to the built-in table, and a new synth never saved
goes away; REMOVE deletes the file (a built-in shows its table again). At boot
`midi_synths_init()` loads the files: one with a built-in's id replaces that built-in (flags
BUILTIN | STORED, "Built-in, changed" in the app). The JSON:

```json
{"id": "minilogue-xd", "maker": "KORG", "name": "MINILOGUE XD", "channel": 1,
 "programs": {"scheme": "korg-bank100", "count": 500},
 "params": [{"group": "VCO 1", "name": "PITCH", "sends": "korg10", "cc": 34, "centred": true},
            {"group": "VCO 1", "name": "WAVE", "sends": "cc", "cc": 50,
             "options": [{"name": "SQR", "value": 0}, {"name": "TRI", "value": 64}, {"name": "SAW", "value": 127}]}]}
```

Limits (`midi.h`): id 1-23 of a-z, 0-9 and -; name 15 characters, maker 11, a parameter's group
and name 11, an option's name 9; 1-64 parameters, 2-8 options, values 0-127, channel 0
(none) to 16. Anything else is refused with a reason (`midi_synth_put`'s `why`), which the app
shows. Each synth from JSON is one PSRAM block (the synth, its parameters, their strings,
about 6 KB at 64 parameters). The list is an array of pointers swapped whole, so a reader
(the display, the LEDs, the midi task, the menu) always sees a complete synth; a replaced block
is freed a second later (`midi_synths_reap`, from the usb task). `midi_synth_count()` stays in
IRAM (the menu's SYNTH row asks it at a detent crossing) and reads one atomic in internal RAM.
The midi task notices its synth's block changed and starts its values over. A removed synth
moves the menu's live, saved and undo choices like a removed app profile
(`menu_midi_synth_removed`). Host test: `tools/midi_synth_test/run.sh` (every built-in to JSON
and back, save / revert / remove, a boot, refused input).

**Three sides, like HOME.** The control loop pushes turns and F1 down / up / F2 / F3 into a
64-word lock-free ring (`midi_input_*`, `CONTROL_HOT`, internal RAM; each word carries the
event and a 1.024 ms timestamp, `esp_timer_get_time() >> 10`, so the loop does no 64-bit
division) and reads two atomics: the end-stop flags (`midi_at_end`: the value's 0 and maximum,
the first and last option of a switch, the ends of the list while F1 is held) and the haptic
profile (`midi_haptic_profile`: FINE for a continuous value, WIDE for a switch, one option per
click; COARSE while picking). The `midi` task owns the rest and publishes `midi_snapshot_t`.

**A parameter** is a CC (0..127), a KORG 10-bit CC (0..1023: CC 63 carries the low 3 bits,
then the parameter's CC the upper 7, as the minilogue xd's implementation note *1-4 / *5-4
says), or a switch: up to 8 options, each with the value it sends. The step per detent follows
the turning speed (time between detents): over 150 ms the finest (1, or 2 of 1023), then one
7-bit step, then 3, then 6 for a flick. A value is unknown (-1, `--` on screen) until it is
turned or received; the first turn starts from the middle. F1 tapped: the next parameter
(wrapping); F1 held 400 ms, or turned while held: the list, and the knob picks; let go to use
it. F2 / F3: program change down / up (`MIDI_PROG_PC`, or `MIDI_PROG_KORG_BANK100`: bank select
MSB 0 and LSB n / 100, then program n % 100, for the minilogue xd's 500). A program change
makes every value unknown.

**Ports.** Every message goes to both: USB MIDI (`tud_midi_stream_write`, when the MIDI
personality is mounted, section 9) and the TRS jacks (UART1 on GPIO 43 TX / 44 RX at 31250
baud, 8N1, RX pulled up; started the first time MIDI mode is used, then the pins stay the
jacks'). UART0, the console's default, shares those pins: the boot ROM's and bootloader's log
still goes out there at 115200 before the UART is taken, which a synth reads as noise. What
comes in on either port (USB MIDI packets, or TRS bytes through a running-status parser that
skips system messages) updates the values: a CC on the knob's channel sets every parameter
with that CC (a switch to the option whose value is nearest; KORG 10-bit with the last CC 63);
a program change makes them unknown.

**The screen and the LEDs.** `ui_extras.cpp` `draw_midi`: the synth, the parameter's group and
name, the value (a bipolar one as -/+ around the middle, a switch as its option's name), a
31-block meter or one box per option, the channel and the ports, or `PROG nnn` for 1.5 s after
a program change; with F1 held, five rows of the list. The ring (`led_task.c`) fills clockwise
from 12 o'clock with the value, from 12 o'clock either way for a bipolar one, one segment per
option for a switch, and with F1 held a dim dot per parameter and the current one bright. The
idle screen jumps the synth maker's wordmark (`ui::maker_logo`, a 1-bit sprite, handed to
`fx_attract` as its `mark` and drawn at 3×) instead of an icon; GENERIC has none and keeps the
QUADRA wordmark.

**Memory.** About 1.2 KB of internal RAM (the ring, TinyUSB's MIDI FIFOs and buffers, the RAM
descriptors, the snapshots), plus about 0.6 KB of heap for the UART driver once the jacks
start; the 3.5 KB stack in PSRAM; about 37 KB of flash, the four synth tables included.
Measured: static internal RAM 64,480 → 65,728 bytes, flash 1,324,839 → 1,361,411 bytes. The
editable list (v12) added 80 bytes of internal RAM (the count; the rest of the list is in
PSRAM) and about 10 KB of flash.

## 9. USB

`usb_task.c` installs one composite TinyUSB device, in one of two personalities:

| Interface | Endpoints | Purpose |
|---|---|---|
| CDC-ACM | EP1 IN, EP2 IN/OUT | Serial console, and the 1200-baud reset the uploader uses |
| HID (not in MIDI) | EP3 IN | Keyboard, mouse, gamepad and Consumer (media keys) as four report IDs; polled every 10 ms |
| USB MIDI (MIDI only) | EP3 IN/OUT | Class-compliant MIDI 1.0: Audio Control + MIDI Streaming, one cable, "Quadra MIDI" |
| Vendor HID | EP4 IN/OUT | 64-byte raw reports for the companion and icon upload |

The vendor interface is separate from the keyboard interface so host tools can open it without
the operating system's keyboard-access permission.

**Two personalities.** The S3's USB controller has 5 IN endpoints active at once, and EP0 is one
of them (TinyUSB's `dwc2_esp32.h` `ep_in_count`; `dcd_dwc2.c` allocates EP0 IN from the same
count). The HID personality uses all 5, so MIDI can't be added to it: in MIDI mode the device is
CDC + vendor HID + USB MIDI instead (product ID 0x400D; the HID one keeps 0x4009, so a host never
applies one's cached interfaces to the other). esp_tinyusb answers every GET_DESCRIPTOR from the
pointers it was given at install, so the device and configuration descriptors live in RAM and
are rewritten between `tud_disconnect()` and `tud_connect()` (300 ms apart). The usb task does
that when MIDI is picked or left, once the menu is closed (the carousel passes MIDI on its way)
and the choice has held for 500 ms (the companion sets the mode live); the boot comes up in the
saved mode's personality. TinyUSB numbers HID instances in the order they open, so the vendor
interface is instance 1 in HID and 0 in MIDI (`host_link_set_instance`). In MIDI no keyboard or
mouse report is sent over USB (the WiFi client that asked for the controls still gets them).

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
profile back to factory. `SET MIDI_SYNTH` (16) picks MIDI's synth profile; the reply carries it
and the number of profiles in bytes 34 and 35, and its dirty bits are 24 bits wide (byte 3 holds
bit 16 and up). `SET DETENTS`, from older apps, shows the nearest stepped profile.

Whole profiles travel as JSON text with a CRC-32 over the complete text. Uploads must arrive
in order; anything else fails the transfer rather than applying a damaged profile.

Incoming reports arrive in the TinyUSB task. Replies are queued and sent from the usb task,
together with the streams, so two senders never race for the one IN endpoint. A profile
download is the exception: each piece is sent from TinyUSB's "report sent" callback, one per
1 ms host poll.

Icon upload (`icon_store.c`, commands 0x01 to 0x04) shares the interface. Uploaded icons are
held in RAM and lost on restart.

### 10.1 Extensions

`ext_proto.h` and `ext_link.c` add commands 0x20 to 0x3F (replies and events 0xC0 to 0xCF),
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
| 11 | `HOME` and `EXT_TAG_HOME`: HOME's lamps, imported over USB, their state over any link (section 8.4). The first command of the second range, 0x30-0x3F: 0x20-0x2F is full |
| 12 | `HOME EDIT` (rename, icon, order, remove; any link) and the lamp's icon, address and dialect in `EXT_TAG_HOME`; `SYNTH` and `EXT_TAG_SYNTH`: the synth list, a synth's JSON (read in 48-byte pieces: offset 0 alone first, the knob writes the JSON then; the rest may be asked for several at a time), upload, save / revert / remove, the midi task's status, and go to a parameter (section 8.5) |

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
- **USB only:** the WiFi network and password, the key, SERIAL boot, HOME's lamps (their
  tokens), and icon uploads
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

## 11. Sound

The motor is the speaker. `motor_sound.c` plays every sound as a voltage on the motor's
windings, on top of what the control loop sends: the detent click, a short decaying burst
`A·e^(−t/τ)·sin(2πft)` or its square wave, next to the q-axis click pulse; and the startup
chime. On the **d axis** a sound makes no torque; the windings and magnets push on the
housing. On the **q axis** it shakes the rotor and the knob with it. There are no audio files
and no audio task.

The board also carries a MAX98357A I²S amplifier and a transducer. They were the sound until
2026-10-07, when the motor turned out louder on two units, at the same frequency on both, and
no quieter with a hand on the knob. The I²S code is gone; `main.c` holds the amplifier's three
pins low so it stays shut down. The successor board leaves the parts out.

- **Voltage.** The supply is 5 V, so 2.5 V (half the bus) is the most any vector can be. A
  d-axis sound gets what Vq (capped at 2.0 V) leaves inside that circle, so a SAW click pulse
  still leaves 1.5 V; the limit follows the chord of the circle, which needs no square root in
  the loop. A q-axis sound adds to Vq, up to 2.5 V.
- **Rate.** Sounds are stepped at the PWM rate, 32 kHz, not the loop's 10 kHz (which reaches
  4 kHz at best: 2.5 samples a cycle). While anything sounds, an interrupt on the PWM timer
  writes the three compare values every period (`motor_driver_tone()` and `tone_on_period()`
  in `motor_driver.c`). The loop still decides everything, each tick: the phase voltages
  without the sound, each voice's pitch and level (so envelopes and the chirp move in 100 µs
  steps), the clip limits, and what 1 V on the d and on the q axis is on each phase. The
  interrupt steps up to five voices (a 256-entry sine table, the sign for a square, or for
  noise whole cycles of the sine, each upside down or not at random),
  sums them per axis, clips, and adds them to the phases. Integers only: interrupts don't get
  the FPU (`CONFIG_FREERTOS_FPU_IN_ISR` is off). It is switched on when a sound starts and off
  when it ends, so a silent knob pays nothing.
- **Frequency.** A click is the calibrated frequency times the profile's PITCH, clamped to
  500 Hz to 10 kHz. On hardware (2026-10-07) tones up to 10 kHz were about as loud as the
  3–4 kHz ones.
- **Loudness.** The profile's AMP sets the voltage on a curve, `a·(2 − a)` of the maximum
  (15% gives 28%, 50% gives 75%): the motor is faint, so the low settings get more than a
  straight line would give them. 0 is silent.
- **Parts.** A click is one or two parts (`motor_sound_part_t` in `motor_sound.h`), sounding
  together or one after the other: each has a wave (sine, square, noise), a time constant and
  length, a pitch and a level as fractions of the click's, and a delay. Two voices are the
  click's, three the chime's.
- **Shapes since 32 kHz (2026-10-07).** TICK (sine, 1.5 ms, an octave up), TING (two sines
  1 : 2.7, 6 ms) and TAP (2 ms of noise), after the six plain ones below. Fourteen were
  tried on hardware; SQR 8MS, SQR 8MS CH, SNAP, TOCK and DOUBLE were dropped. Profiles that stored one
  of the fourteen (or of the first eight) are mapped to today's list on load
  (`MOTOR_SOUND_SHAPE_FROM_14` and `_FROM_10`; the stored word carries flags for which list it indexes).
- **Voices on the q axis share the voltage.** Together they get no more than one voice at
  full level (`motor_sound_tick()` scales them); on the d axis they may stack and clip, which
  is louder. Reason: TOCK, a full square at 0.75 of the pitch under full noise at the pitch,
  cut the USB power with a single click on the q axis (reset reason POWERON, no panic or
  brownout), while the same square alone on q, and TOCK on the d axis, were fine
  (2026-10-07). Clipping two close pitches makes their difference tone, a slow one, and on q
  that is torque. That this is the mechanism is a reading of those three results, not a
  measurement.
- **The end-stop knock plays on the d axis only.** With the voltage shared, TOCK on q was
  fine with the SAW feel and still cut the power with SINE (AMP 100%, or 50% on a fast spin).
  So sharing was not the whole answer and the cause is open: something between a q-axis
  sound with a low part and the SINE feel's control. TOCK was dropped; the knock, built the
  same way, stays on d. A new click with a low part is untested on q.
- **Direction.** A step one way clicks 2% higher, the other way 2% lower
  (`CLICK_DIRECTION_PITCH` in `control_task.c`).
- **End stop.** A list at its end answers the push with a low knock in place of the click
  (`motor_sound_thud()`: a square at half the click's pitch under noise, both at full level), at the profile's AMP.
- **Tunes.** `motor_sound_jingle()` plays a list of notes on three voices, from any task
  (the request is one atomic; the loop starts it on its next tick). Each note has its start,
  length, pitch, wave, level and decay, and may slide in pitch; note n takes voice n mod 3.
  The tunes: the startup chime; two notes up when something is saved (with the SAVED! toast);
  one low note when an edit or an armed action is cancelled, a save fails, or an agent is
  denied; three rising notes (C7 E7 G7) when an agent asks for approval and two soft ones for
  any other agent notification (once per item, in any mode, whether it taps or not); the
  coin's two notes when an agent is allowed. `MOTOR_TONE_RICH` is a sine with its second and
  third harmonics from a second table; all but the two soft notes use it.
- **Shapes.** Six plain ones, in `motor_sound.h`: sine at 2 ms and 4 ms (the envelope's time constant;
  a click lasts 1.5 times that), square at 2 and 4 ms, and two with a **chirp** (sine
  4 ms and square 4 ms), whose pitch falls to 0.6 of its start through the click, like the
  old speaker click did. A steady pitch rings hollow; the chirp crosses more of the body's
  resonances. Lengths are times, not cycle counts, so a low PITCH does not stretch a click (a
  stretched chirp sounds like a bird). Each shape's decay, glide and length are worked out once
  at start-up, so picking one in the loop is an index.
- **Heat.** `sysmon` adds Vd² to its coil current and copper heat.
- **Startup chime.** `motor_sound_chime()`, asked for by the loading screen 3.05 s in, just
  after its two tiles land (a knock, `MOTOR_SOUND_JINGLE_LAND`, d axis only, comes at 2.75 s when they land; a
  tune asked for before the motor is up waits for it): COIN, B5 then E6 held
  (988 and 1319 Hz, rich wave), and the pair again at a third of the level as an echo; about
  1.1 s. Picked by ear from six. Loud, low tunes are a risk on the q axis: some of the others,
  in the same range, cut the USB power there (as the TOCK click did), and were too quiet on
  the d axis. Try a new tune on both axes.

**DEVICE → SOUND CAL** finds what carries best. F1 starts it; it then owns the motor (no
spring, no detents) and F1–F3 until it ends. F4, leaving the screen, or the knob turning while
a sound plays stops it: more than 0.05 rad for a d-axis sound, 0.35 rad for a q-axis one,
which is torque and lets the free knob creep. (A q-axis tone starts at its peak, not at zero:
from zero its torque sets the knob drifting one way for the whole tone, which stopped the
first q sweep on hardware.)

1. **Sweep.** 16 log-spaced tones, 1 to 10 kHz (500 Hz to 4 kHz until the sound moved to the
   PWM rate; answers stored then are dropped on load, the click itself is kept), 300 ms each at 1.4 V, on the d axis and
   then again on the q axis. Then: which sweep was louder? F1 the first (d), F3 the second
   (q), F2 plays both again. The rest uses that axis.
2. **Tones.** Each tone from 0.3 V up through 0.7, 1.4 and 2.5 V until F1 (heard); F3 is "not
   heard", F2 plays it again. The best tone is the one heard at the quietest level (of several,
   the middle one). The screen draws the answers as bars.

The frequency and the axis go to NVS (`snd_cal`) and are used from then on; until then clicks
play on the q axis at 5.4 kHz (the values from the maintainer's knob). Every answer is a console line, e.g.
`sndcal axis=q f=3482 amp=0.70 heard=1`, and the result is `sndcal result axis=… f=… level=…`.

**The wave is per haptic profile**, with AMP and PITCH: WIDE can have a long square click and
FINE a short sine. **HAPTICS → CLICK** (the eighth item on the ring) picks it for the profile
shown and draws it: the wave as it is played, as wide as it is long, a dot running along it.
F2 saves it with the profile; holding F2 puts it back to the profile's factory one (SQR 2MS for FINE, SIN 2MS for the
others) with the rest. The five shapes are packed, four bits each (three in blobs from when there were eight;
a flag bit tells them apart), into the one spare word of the stored
profiles (`hprof_cfg`; it held the speaker's timbre), with a flag bit to say they are there,
so profiles tuned before this still load: they all start from the one shape the device had
then (`snd_cal` still carries it). The companion sets it too: `HOST_SET_CLICK`, id 6, and
byte 22 of the settings report with bit 7 set (the id and the byte were the speaker's timbre,
so an app can tell which it is talking to).

**DEVICE → CLICK** switches the axis, D or Q, without a new calibration: a direct screen,
turning switches it and, being a detent, plays it; F2 saves.

The sequence runs in the `sndcal` task
on Core 1 (4 KB stack, internal, since it writes NVS); the control loop only plays what its
atomics say. 150 ms of silence follows every sound, which keeps the windings under what a held
detent draws.

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
press) picks the next style; the usb task stores it (`user_prefs_poll`). The idle screen starts after 12 seconds without input (in AGENTS, not while an agent is working or asking);
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
agent; MUSIC shows the volume as an arc while the knob turns; CLOCK can sweep the seconds;
HOME shows the chosen lamp's scale (section 8.4) and MIDI the parameter's value (section 8.5).

## 14. SYS INFO

`sysmon.c` collects the numbers behind DEVICE → SYS INFO and the companion's System info page.

- **Loop timing.** The control task adds each iteration's cycle counts to plain statics and
  hands the sums over every 1000 ticks. Reported: rate, average and maximum work, jitter,
  missed ticks, spikes per second, and the four sections.
- **Core load.** Whatever the idle task on each core did not get.
- **Power.** An estimate, not a measurement: motor current from `vq` and the phase resistance,
  LED current from the colours sent, and a fixed figure for the rest of the board.
- **Heat.** The chip's temperature sensor.
- **System.** Free heap and its minimum, dropped HID wheel events, encoder CRC
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
| `spiffs` | 640 KiB | LittleFS: stored app profiles (`/fs/profiles`) and synth profiles (`/fs/synths`) |
| `coredump` | 64 KB | Crash dumps |

The firmware image is about 1.32 MB (78% of a slot), most of the growth being WiFi. NVS also
holds the namespaces `user_prefs` (idle word, LIGHTS, cover style), `clock`, `home` (HOME's lamps
and their tokens) and `net` (WiFi network,
password and pairing key), in plain text: flash encryption is off.

The slots grew from 1.25 MB, and the profile store moved and shrank from 1.4 MB, when WiFi
came in. A device flashed before then needs the new table; `tools/quadra.py flash
--partitions` writes it and blanks the new profile store.

## 16. Building, and things that bite

```sh
cd NanoDepsidf
pio run                                   # build (platform pinned: platformio/espressif32@7.1.3)
pio run -t upload --upload-port <port>    # flash
tools/profile_json_test/run.sh            # host test for profile JSON
tools/midi_synth_test/run.sh              # host test for the synth profiles
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
