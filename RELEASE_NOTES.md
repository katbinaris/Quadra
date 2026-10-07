# Quadra 2.1.0

**Firmware 2.1.0 · Companion 0.3.0 (macOS) · Mac service**

Two big things since 2.0.0: MIDI mode now really sends MIDI, and the knob makes its own sound.
The clicks come from the motor, louder than the speaker they replace. HOME, for the lamps on
your network, is new as well.

Install the firmware and the companion from the same release; they are built to work
together.

## Firmware 2.1.0

### Sound from the motor

- **The knob is the speaker.** Every detent plays a short burst through the motor's own
  windings, and so does the startup chime. On the two units tested it is louder than the old
  speaker, rings at the same pitch on both, and a hand on the knob doesn't muffle it.
- **Nine click sounds:** sine or square, 2 or 4 ms, some with a falling pitch, and TICK (short
  and high), TING (a small bell) and TAP (a knock of noise). **HAPTICS → CLICK** picks one and
  draws it as it sounds.
- **More to hear:** a step one way clicks a little higher than a step the other way; a list
  at its end answers with a low knock; saving plays two rising notes, cancelling an edit one
  low note. An agent's request arrives with three rising notes, other notifications with
  two soft ones, and allowing or denying answers with its own sound.
- **A new startup chime,** two notes and their echo, in the manner of a game.
- **A click per haptic profile.** WIDE, COARSE, MEDIUM, FINE and SMOOTH each keep their own,
  next to their volume and pitch.
- **DEVICE → SOUND CAL** tunes the click to the pitch your knob rings at: it plays a sweep
  two ways, asks which was louder, then asks about each tone. A few minutes, hands off the
  knob. **DEVICE → CLICK** switches between the two ways afterwards: D rings the housing, Q
  shakes the knob.
- **The speaker path is gone.** The amplifier and transducer on current boards are no longer
  driven, the WOOD / THUD timbre and SYS INFO's audio gaps went with them, and the firmware is
  about 20 KB smaller.

### MIDI

- **MIDI mode sends MIDI.** Pick a synth, turn the knob, and the synth follows.
- **Synth profiles** built from each maker's own MIDI documentation: KORG minilogue xd (48
  parameters at its full 10-bit resolution, all 500 programs), Roland JU-06A (33), Roland
  TR-8S (54), and GENERIC, standard controllers for any synth or a DAW's MIDI learn.
- **Feels like the synth's own panel:** fine steps when you turn slowly, big ones on a flick,
  a click per option on switches, and walls at the ends of the range.
- **Keys:** F1 for the next parameter (hold F1 and turn to pick from the list), F2 / F3 to
  step the synth's programs.
- **Two ports at once:** class-compliant USB MIDI (no driver on Mac, Windows, Linux or iPad)
  and the 3.5 mm TRS jacks, so it plays a synth with no computer at all.
- **Stays in sync:** if the synth sends its own knob moves back, the knob's value follows.
- **Synths are data.** Each synth profile is a JSON file on the knob, and the companion can
  edit them or add your own.
- In MIDI mode the knob is a MIDI device instead of a keyboard and mouse, so switching into
  or out of MIDI reconnects USB for about a second.

### Also new

- **HOME:** the knob as a remote for the Xiaomi lamps on your network, with no cloud and no
  computer: power, brightness, colour and white, per lamp.
- **The knob's controls over WiFi.** With no USB host, the companion types and scrolls for
  the knob. Two WiFi clients can be connected at once (the app and the Mac service).
- **MUSIC:** the album cover on a spinning record, in three styles (RECORD, SLIDE, BLEED).
- **USB power is 5 V 3 A only.** The knob asks again if a charger offers more, and
  `quadra.py pd` sets the USB-PD chip to 5 V 3 A for good.
- **Ready in about a second.** The knob used to wait some 11 s after power-on before the
  motor came up. Keys for the power-on modes must now be held while plugging in.
- **A new loading screen** in the idle screen's manner: a Q tile and an Espressif tile, glued
  back to back, hop, spin-jump and split with a flash; they land side by side with a knock
  and the chime plays.
- **Idle after 12 s** (was 5), and never in AGENTS while an agent is working or asking.
- **A finer volume ring in MUSIC:** a 1 px circle with a 2 px arc and a small dot, in place of
  the thick, uneven one.
- **More free memory:** about 62 KB of internal RAM in normal use, up from 18.

## Companion 0.3.0

- **Mode:** HOME and MIDI, with the synth and channel for MIDI.
- **Lamps:** import your Xiaomi lamps and name them.
- **Synths:** edit a synth profile's parameters and programs, or make your own, with the
  knob's screen for the parameter and a monitor of what the knob sends.
- **Haptics:** a **Click** picker, the nine click sounds drawn as they sound, for the
  haptic profile you are tuning. The knob plays each as you pick it.
- **Over WiFi:** the profile list loads in about 5 s, and changes made on the knob show in
  the app.
- The speaker's timbre and the audio gaps counter are gone, with the speaker.

## Mac service

- Works over **WiFi** as well as USB (music, covers, agents, the clock).
- **GitHub Copilot in VS Code** joins Claude Code, Codex and Cursor in AGENTS.

## Updating from 2.0.0

- Flash the firmware as usual; the partition table is the same.
- **Run DEVICE → SOUND CAL once.** Clicks work before that, at a default pitch.
- Your tuned haptic profiles are kept. All five start with the same click sound; change them
  one by one and save.
- Install Companion 0.3.0 with it. An older companion still connects, but has no Click picker.

---

# Companion 0.2.0

**Companion 0.2.0 (macOS) · works with firmware 2.0.0**

A new interface for the companion, built for how much it now does. The firmware is unchanged.

- **A calmer, clearer look.** A sidebar with every page, a top bar that shows where you are,
  and sentence-case text in Geist. Pixel art stays where it belongs: the knob's screen, the
  profile icons and the idle word. Icons sit on the page with no black box behind them.
- **One save for everything.** Settings, lights and profiles are saved together with **Save
  to knob**. An amber label lists what isn't saved yet, each with its own **Revert**.
- **Edit profile opens on the first click.** Before, it sometimes needed several clicks.
- **Each input picks a haptic profile.** The knob and F1 to F4 each choose one of WIDE,
  COARSE, MEDIUM, FINE or SMOOTH. A profile's feel and steps are tuned in one place, under
  **Haptics**.
- **Lighter on the knob.** The live view of the knob is gone. The sidebar shows a small
  picture of the knob with its main screen, drawn from data. System info streams only while
  it is open. The remote (pressing keys and turning the knob from the app) went with the live
  view.

The full guide is in [companion/docs/COMPANION.md](companion/docs/COMPANION.md).

---

# Quadra 2.0.0

**Firmware 2.0.0 · Companion 0.1.0 (macOS) · Mac service**

The first release since 1.0.0, and a new start. The firmware is rewritten from scratch on
ESP-IDF, with a pixel interface on the round screen, app profiles for Figma, Plasticity and
Onshape, and MUSIC, AGENTS and CLOCK. The desktop companion app is new too. It shows the knob
live and edits its settings, haptics and profiles, over USB or WiFi.

Install the firmware and the companion from the same release; they are built to work
together.

---

## Contents

- [Highlights](#highlights)
- [Firmware 2.0.0](#firmware-200)
- [Companion 0.1.0](#companion-010)
- [Mac service](#mac-service)
- [Installing and updating](#installing-and-updating)
- [Known limitations](#known-limitations)
- [Credits](#credits)
- [License](#license)

---

## Highlights

- **Haptics you can tune.** Five haptic profiles (WIDE, COARSE, MEDIUM, FINE, SMOOTH), each
  with its own stiffness, damping, shape, and click pitch and volume. Lists end in a wall the
  knob pushes back from.
- **APP mode.** The knob drives Figma, Plasticity and Onshape directly: zoom, orbit, walk the
  layer tree, and dial modelling values to exact numbers.
- **Command wheel.** Hold a key, turn to a command, release to run it. Each command has a
  small animated card.
- **MUSIC, AGENTS, CLOCK.** A volume dial with the album cover on screen. Approve or deny
  Claude Code, Codex and Cursor requests from the knob. A desk clock with up to five time
  zones.
- **WiFi.** The companion app reaches the knob without a cable, over an encrypted link you
  pair once over USB.
- **Companion app.** The knob live on your Mac: its own screen, its LED colours, and the
  keys as you press them. Click and drag it to use it as a remote.

---

## Firmware 2.0.0

### The knob

- **Haptics.** The motor runs a 10 kHz control loop with three feels:
  - **SAW:** crisp detents with a click.
  - **SINE:** a smooth bump.
  - **VISCOSE:** smooth drag, no detents.

  Five haptic profiles package them, from 8 to 36 detents a turn. Each keeps its own
  settings, and the companion can tune them live.
- **Clicks you can hear.** Each detent plays a click through the built-in speaker, at the
  pitch and volume of the profile in use, so a fine step sounds different from a coarse one.
- **Works without any software.** The computer sees a keyboard, a mouse, a gamepad and media
  keys. The companion app is optional.

### Apps

- **Figma:** zoom, walk the layer tree, and a command wheel of auto-layout and component
  commands.
- **Plasticity and Onshape:** orbit, pan and zoom the viewport. After a modelling command,
  **parameter mode** dials the value: fine clicks or exact 0.05 / 0.10 / 1.00 steps, X / Y / Z
  constraints in Plasticity, and the number field in Onshape.
- **MUSIC:** the knob is the system volume and the keys play, pause and skip. With the
  [Mac service](#mac-service) running, the screen shows the album cover, title and artist,
  and a volume ring that follows the knob. The LED ring takes the cover's colours.
- **AGENTS:** Claude Code, Codex and Cursor show their permission requests on the knob. Hold
  F1 to allow, F3 to deny. A dashboard shows which sessions are working or waiting. Needs the
  Mac service.
- **CLOCK:** the time in up to five zones, with daylight saving, set from the Mac or over
  WiFi. LED SECONDS turns the ring into a seconds hand.
- **Blender and AutoCAD** are listed as empty profiles: the knob scrolls until they are
  designed.
- **Your own profiles.** Any profile can be edited, or a new one made, in the companion app.
  They are stored on the knob, together with their macros.

### Screen and lights

- **Pixel interface.** A console-style interface on the round screen, with animated
  transitions. Turn the screen to any of four orientations in DISPLAY.
- **Idle screen.** After 5 seconds untouched, the active app's icon (or QUADRA, or a word of
  your own) jumps around or bursts onto the screen, in that app's colours.
- **LEDs.** A 60-LED ring and two LEDs under each key, in the active app's colours: a spot
  that follows the knob, a pulse on each detent, a flash at an end stop. **LIGHTS** sets the
  colour, effect (gradient, solid, breathe, spin, rainbow, off), speed and brightness. Brightness stays
  within what your USB port can supply.

### WiFi

- Off until you set it up over USB, in the companion (**DEVICE → WIFI**) or with
  `quadra.py wifi`. Open, WPA2 and WPA3 networks work.
- Once connected, the knob is `quadra-xxxx.local` on your network and sets its clock from
  `pool.ntp.org`. That is the only connection it makes outside your network.
- **The companion link is encrypted.** Pair the app once over USB and it gets a random
  256-bit key; every message is authenticated and encrypted. The network settings, the key
  and firmware flashing only change over USB. **NEW KEY** cuts off every paired app.

### Settings menu

F4 opens the menu (in APP mode, hold it). PROFILES picks the mode (APP, KEYBOARD, MOUSE,
MIDI) and the app; HAPTICS tunes the haptic profiles; then DISPLAY, LIGHTS, BOOT MODE, and
DEVICE (BINDINGS for Mac or PC, RECALIBRATE, and SYS INFO with power, heat and timing). F2
saves.

### Fixes for anyone who built from `main`

- **Animations run at full speed in normal (HID) mode again.** After WiFi was added, the
  screen's frame could end up in slower memory, so transitions, the idle screen and list
  scrolling ran slowly unless the knob was started in SERIAL mode.
- Only the knob's own F1 approves an agent request; a click on the companion's picture of
  the knob can't.
- SINE end stops no longer click.

---

## Companion 0.1.0

A macOS app (about 5 MB) in the knob's own pixel style. Changes are live on the knob;
**SAVE** stores them, just as F2 does.

- **The device:** the knob drawn live, with its own screen, the LED ring in its real colours,
  and the keys as you press them. It works as a remote: click a key, or drag or scroll over
  the knob to turn it.
- **HAPTICS:** the haptic profiles and their sliders, with a reset to factory values.
- **PROFILES:** choose the mode and app profile. Edit any profile: key labels, icon, what the
  knob and keys send, the command wheel and macros. Make new ones from scratch or by
  duplicating one, and turn any image into the profile's icon.
- **Macros:** build them step by step or **RECORD** them, then give one to a key or a wheel
  entry. They run on the knob, with no software needed once saved.
- **LOOK:** the idle word, LIGHTS, and the CLOCK app's zones and format.
- **DEVICE:** BINDINGS, rotation, boot mode, WiFi and pairing, and the firmware version.
- **SYS INFO:** power, heat, CPU and system, with a minute of history.

The same interface also runs in Chrome or Edge as a web page, over WebHID. The full guide is
in [companion/docs/COMPANION.md](companion/docs/COMPANION.md).

---

## Mac service

An optional background service for macOS (`NanoDepsidf/tools/mac/`). It powers MUSIC's now
playing, the AGENTS requests and dashboard, and CLOCK's local time. It talks to the knob over
USB.

```sh
python3 -m pip install --user hidapi Pillow
python3 NanoDepsidf/tools/mac/install.py             # install or update
python3 NanoDepsidf/tools/mac/install.py --dry-run   # show what would change
python3 NanoDepsidf/tools/mac/install.py --uninstall # take it all out again
```

- It runs at login (log in `~/Library/Logs/quadrad.log`).
- It adds hooks to Claude Code, Codex and Cursor's settings, next to the ones you have. Each
  file is backed up first, and `--uninstall` removes exactly those hooks. Codex asks you to
  approve them once: run `/hooks` in Codex.
- Its only internet traffic is for album covers: the artwork link the player gives, or the
  iTunes Store's search when a track has none.

---

## Installing and updating

### Firmware

You need [PlatformIO](https://platformio.org/).

```sh
cd NanoDepsidf
pio run -t upload --upload-port <port>
```

If the upload can't find the knob, hold **F3 + F4** while plugging it in. It starts in
SERIAL mode for that one boot, which the uploader can always reach. Or use
`quadra.py flash`, which needs no keys.

> ⚠️ **Updating from 1.0.0 or an earlier preview.**
> - **Flash the partition table too.** The layout changed: the app slots grew and the
>   profile store moved. `pio run -t upload` writes it, or use
>   `quadra.py flash --partitions .pio/build/esp32-s3-devkitm-1/partitions.bin`.
> - **App profiles you made or edited in the companion are erased** by the new layout, and
>   there is no way to back them up to a file yet. Note down anything you want to make again.
>   The built-in profiles, settings, calibration and haptic profiles are kept.
> - **Calibrate before first use.** A new or erased board learns its motor on first boot,
>   in about two seconds. Leave the knob free to turn while it does; see
>   [First calibration](README.md#first-calibration).

### Companion

Download **Quadra.app**, move it to Applications, and open it. The app is not notarized, so
the first time macOS says it is from an unidentified developer: right-click the app, choose
**Open**, then **Open** again. Connect the knob over USB; for WiFi, pair it in
**DEVICE → WIFI → PAIR THIS APP**.

To build it yourself, see [companion/README.md](companion/README.md).

---

## Known limitations

- **macOS first.** Shortcuts are written for macOS; on Windows set DEVICE → BINDINGS to PC.
  They assume a US keyboard layout. The companion app and the Mac service are macOS only; on
  Windows, use the companion in Chrome or Edge.
- **Icons uploaded with `send_icon.py`** are cleared on restart. Icons imported into a
  profile in the companion are kept.
- **MIDI:** Roland's switch values and the minilogue xd's Sync / Ring polarity are still to be
  checked against the synths themselves.
- **Sound:** the built-in speaker of 2.0.0 is not used from 2.1.0 on; the motor plays clicks
  from 500 Hz to 10 kHz.

---

## Credits

Quadra's firmware and companion are by Kafi Devices.

[**@Dviros**](https://github.com/Dviros) contributed MUSIC, AGENTS, CLOCK, WiFi, LIGHTS and the
idle word, the Mac service, the companion's LOOK tab and remote, and `quadra.py`, in
[pull request #17](https://github.com/katbinaris/NanoD_RatchetH1/pull/17). Thank you.

---

## License

From this release, Quadra is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE.md): free for personal and other
noncommercial use; commercial use needs a license from Kafi Devices.
