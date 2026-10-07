# Quadra companion

The desktop app for the Quadra knob. It changes the knob's settings and app profiles from the
computer: a sidebar with the knob and its pages, one Save for everything, and pixel art only
where the app shows the knob itself.

- **Mode:** what the knob sends (App, Home, Mouse, Keys, MIDI); in App, the profile in use; in Mouse
  and Keys, the haptic profile that mode uses; in MIDI, the synth and the channel.
- **Lamps:** HOME's lamps as the knob sees them (drawn with the knob's own lamp art), renamed,
  re-iconed, reordered or removed; and the import from a Xiaomi account (the token extractor in
  Terminal, then `src-tauri/src/home.rs` finds the lamps, probes them and reads their MIoT specs;
  the keys go to the knob over USB only).
- **Synths:** MIDI's synth profiles, edited like app profiles (live, then Save; your own too, as
  JSON on the knob), with the knob's screen for the parameter and a monitor of the knob's USB
  MIDI port (`src-tauri/src/midi.rs`, CoreMIDI or WinMM through `midir`).
- **Haptics:** the five haptic profiles (Wide / Coarse / Medium / Fine / Smooth) and, for the
  one picked, its feel (Saw / Sine / Viscose) and the sliders Snap, Damp, Shape, Click volume
  and Click pitch, and its **Click**, one of the motor's nine click sounds. **Reset to
  factory** puts that profile back.
- **App profiles:** one page per profile, built-ins included, with four tabs. **General:** name,
  icon (import any picture), key labels, the main screen, and a preview drawn the way the knob
  draws it. **Knob & keys:** what the knob and F1–F4 send, and the one haptic profile each uses
  (feel and strength come from that haptic profile, nothing per input). **Command wheel:** rings
  and commands (shortcut, search, macro). **Macros:** key presses, text and pauses the knob types
  itself, built step by step or recorded. A changed built-in keeps its original in the firmware;
  **Reset to default** brings it back. **New profile** and **Duplicate** make your own.
- **Look:** the lights (colour, effect, speed, brightness); the idle word, the music cover style
  and screen rotation; the Clock app's format and zones.
- **Device:** Mac or PC, HID or serial at start, the firmware, and Wi-Fi with pairing this app.
- **System info:** power (an estimate), heat, CPU and system, with a minute of history.

Changes are live on the knob at once. The top bar counts what isn't stored yet (settings,
lights, profiles, synths) and lists it on a click, each with its own Revert; **Save to knob** stores it
all. F2 on the device stores the settings and lights too.

The app doesn't mirror the knob's screen or LEDs live (the knob does); Lamps and Synths draw the
knob's HOME and MIDI screens from the state they ask for each second. The live stream (power,
heat, CPU) is only asked for while System info is open.

The [user guide](docs/COMPANION.md) covers every page, with screenshots.

## How it talks to the knob

It uses the knob's vendor HID interface: usage page `0xFF00`, 64-byte reports, the same
interface the icon upload uses. No driver is needed on any OS, and on macOS there's no Input
Monitoring prompt. The wire format is
[`NanoDepsidf/src/host_proto.h`](../NanoDepsidf/src/host_proto.h), mirrored in
[`src/proto.ts`](src/proto.ts); change both together. The knob must run firmware with that
protocol (version 3: haptic profiles) and be in **HID** boot mode. With older firmware the
settings still work, but Haptics can't show the profiles' own limits.

Look, Wi-Fi, the clock, HOME's lamps and the synths use the protocol's extensions:
[`NanoDepsidf/src/ext_proto.h`](../NanoDepsidf/src/ext_proto.h) (commands 0x20–0x3F, now
version 12), mirrored in `src/proto.ts` too. Each feature shows only when the knob's
extensions version has it (HOME's lamps from v11; lamp edits and the synths from v12).

**Over WiFi** the app uses the same reports. They go over a TCP connection to the knob (port
3333, found as `quadra-xxxx.local`), after a handshake on the key the app got over USB, and are
encrypted with AES-256-GCM. The Rust side (`src-tauri/src/lib.rs`) does the networking and the
encryption; the web page has no WiFi.

The protocol and the whole UI are TypeScript, and only the USB connection differs:

| | Desktop app (Tauri) | Web page (WebHID) |
|---|---|---|
| USB access | Rust, `hidapi` (`src-tauri/src/lib.rs`), a thin pipe | The browser's WebHID |
| Browsers | – | Chrome or Edge (not Safari or Firefox) |
| Connecting | Automatic, and reconnects on replug; over WiFi once paired | CONNECT once (the browser asks), then automatic |

## Running it

Needs Node with pnpm, and Rust (`rustup`).

```sh
pnpm install
pnpm tauri dev          # the app, with live reload
pnpm dev                # just the page: open http://localhost:1420 in Chrome for WebHID
```

**Demo mode:** add `?demo` to the page's URL, for example `http://localhost:1420/?demo`. A
simulated knob answers the protocol (and the extensions up to v12: Look, Wi-Fi status, the
clock, HOME's lamps, the synths; the lamp import answers with made-up devices), so the UI can be
worked on without the hardware. Its built-in profiles are the
firmware's, icons included, from `src/demo_builtins.json`; after a built-in profile or icon
changes, rerun `scripts/gen_demo_builtins.sh` (it uses `tools/profile_json_test`); its synths
come from `src/demo_synths.json` (`scripts/gen_demo_synths.sh`, `tools/midi_synth_test`). Every page has an address:
`#/mode`, `#/haptics`, `#/profile/figma/general` (or `keys/f1`, `wheel`, `macros`),
`#/lamps` (`lamps/import`), `#/synths/minilogue-xd/params` (`programs`, `monitor`), `#/look/lights` (`screen`, `clock`), `#/device/general` (`wifi`), `#/sys` — for example
`http://localhost:1420/?demo#/profile/figma/keys/f1`.

**Screenshots** for the user guide come from demo mode, all in one go: with `pnpm dev` running,
`npx playwright install webkit chromium` (once), then `node scripts/screenshots.mjs`. Retake
them after a UI change.

The WiFi link with the cable in (the cable is what powers the knob): pair the app over USB,
then start it with USB hidden, `QUADRA_NO_USB=1 Quadra.app/Contents/MacOS/quadra-companion`
(Windows, PowerShell: `$env:QUADRA_NO_USB=1; pnpm tauri dev`). The Mac service takes the same
variable (`QUADRA_NO_USB=1 python3 tools/mac/quadrad.py`), and so does the Windows one.

The knob's controls over WiFi (no USB host) arrive as `EXT_TAG_HID` reports and are posted
as macOS events by `src-tauri/src/input.rs`, in the WiFi reader thread. They need the app
allowed under **Privacy & Security › Accessibility**. An ad-hoc build is a new app to macOS
each time: after rebuilding, remove Quadra from that list and allow it again. On Windows,
`src-tauri/src/input_windows.rs` sends them with `SendInput` (keys as scan codes, so any
layout reads the right key). Windows asks no permission, but keeps them out of apps running as
administrator unless Quadra runs as administrator too.

## Building

```sh
pnpm tauri build        # -> src-tauri/target/release/bundle/macos/Quadra.app (~5 MB)
```

Local builds are signed ad-hoc, which needs no Apple account. Ad-hoc signing is fine on your
own Mac; when someone else opens the app for the first time, macOS warns about an unidentified
developer.

macOS 27 with Rust 1.93: if the build stops at `can't find crate for phf_macros` (or
`serde_derive`, ...), the stripped proc-macro libraries are being refused by the loader
("mis-aligned LINKEDIT string pool"). Build without stripping:
`CARGO_PROFILE_RELEASE_STRIP=false pnpm tauri build` (the app is a little bigger, ~6 MB).

`bundle.targets` is `["app"]`. Tauri's DMG step styles the disk image by scripting Finder,
which can hang waiting for a permission prompt. For a DMG, use `CI=true pnpm tauri build
--bundles dmg`, which skips the Finder styling, or build it in CI.

### On Windows

Needs the same Node, pnpm and Rust (the MSVC toolchain: `rustup` picks it, with the Visual
Studio Build Tools' **Desktop development with C++**), and WebView2, which Windows 10 and 11
already have.

```powershell
pnpm tauri build        # -> src-tauri\target\release\bundle\nsis\Quadra_<version>_x64-setup.exe
```

`src-tauri/tauri.windows.conf.json` holds what differs from the Mac's build (Tauri merges it on
Windows only): an NSIS installer that installs for the current user, without administrator
rights, and fetches WebView2 if it's missing. The window keeps Windows' own title bar. An
unsigned installer makes SmartScreen warn once (**More info › Run anyway**); signing needs a
code-signing certificate (`bundle.windows.certificateThumbprint`).

The scripts in `scripts/` that end in `.sh` need a POSIX shell: Git Bash or WSL.

### Notarizing (for handing it to other people)

This needs the Apple Developer Program ($99/year). After a one-time setup, Tauri does the
signing, notarization and stapling itself on every build:

1. In the developer account, create a **Developer ID Application** certificate and install it
   in the keychain.
2. In App Store Connect, create an **API key** (Users and Access → Integrations) and download
   the `.p8` file.
3. Build with:

   ```sh
   export APPLE_SIGNING_IDENTITY="Developer ID Application: <name> (<team id>)"
   export APPLE_API_ISSUER=<issuer id>
   export APPLE_API_KEY=<key id>
   export APPLE_API_KEY_PATH=/path/to/AuthKey_<key id>.p8
   pnpm tauri build
   ```

The hardened runtime is already on, which notarization requires. For releases, the same
variables work as GitHub Actions secrets with `tauri-action`.

## Layout

```
src/proto.ts        the protocol (mirror of host_proto.h)
src/profile.ts      profiles as JSON (mirror of profile_json.h), key names, icon conversion
src/transport.ts    Tauri pipe | WebHID, one interface
src/device.ts       connection, settings, profiles + icons, SYS history; changes by topic
src/store.ts        the views' state: a signal per device topic, the #route, save / revert
src/profiles.ts     new profile, duplicate
src/mock.ts         ?demo: a simulated knob (its built-ins: src/demo_builtins.json, src/demo_synths.json)
src/home.ts         the lamp import's logic (names, icons, MIoT specs); the computer's part is home.rs
src/synth.ts        a synth profile: its JSON, limits, and what the monitor shows of a message
src/tzdata.ts       the Clock app's cities and their time zone rules (generated by gen_tzdata.py)
src/main.tsx        the pages by route
src/pages/          Mode, Haptics, Lamps, LampImport, Synths, Look, Device, System info; profile/
                    (the editor's tabs, its session with the knob, an input's haptic profile);
                    synths/session.ts (the open synth's session)
src/ui/             the shell (sidebar, top bar), controls, the knob's screen drawn from data;
                    knobart.ts: the knob's HOME and MIDI art (lamps, marks, both screens, the ring)
src/style.css       the look: tokens, layout, controls
src/assets/         device.png, the top-down render (the sidebar's picture)
src/fonts/          Geist (the app's type) and Silkscreen (the knob's screens), with their licences
scripts/            screenshots.mjs (the user guide's), gen_demo_builtins.sh and gen_demo_synths.sh (the
                    demo knob's profiles and synths), gen_tzdata.py (the clock's cities)
src-tauri/          the Rust side: HID list / open / write / close, reports as events; Wi-Fi;
                    input.rs (the knob's keys over Wi-Fi, posted as macOS events) and
                    input_windows.rs (the same with SendInput);
                    home.rs (the lamp import: the extractor's file, finding and probing lamps,
                    MIoT specs), midi.rs (the knob's USB MIDI port, for the Synths monitor)
```

The views are [Preact](https://preactjs.com) components with
[signals](https://preactjs.com/guide/v10/signals): a page reads the device topics it shows
(`use("settings")`), and re-renders only when one of them changes. A component whose props
don't change re-renders only for a signal it reads itself: an editor's tabs read their session's
`rev`.

---

Look, Wi-Fi and the Media input were contributed by
[@Dviros](https://github.com/Dviros) in
[pull request #17](https://github.com/katbinaris/NanoD_RatchetH1/pull/17).
