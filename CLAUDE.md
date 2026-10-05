# Working on Quadra

- Firmware: `NanoDepsidf/` (ESP-IDF via PlatformIO). How it works, and the rules for changing
  the control loop: `NanoDepsidf/docs/FIRMWARE.md`. Build with `pio run` in `NanoDepsidf/`.
- Desktop companion: `companion/` (Tauri + TypeScript, Preact views). Building: `companion/README.md`.
  The wire protocol is `NanoDepsidf/src/host_proto.h` mirrored in `companion/src/proto.ts`;
  change both together.
- **Anything visual** (device screens, sprites, icons, command cards, animations, the
  companion's UI, doc screenshots) follows `NanoDepsidf/docs/PIXEL_ART.md`. Read it first.
- Check UI changes with `NanoDepsidf/tools/ui_preview/run.sh` (the companion: demo mode,
  `companion/scripts/screenshots.mjs`), profile JSON changes with
  `NanoDepsidf/tools/profile_json_test/run.sh`, and synth profile changes with
  `NanoDepsidf/tools/midi_synth_test/run.sh`. Type-check the companion with
  `npx tsc --noEmit` in `companion/`.
- Code in the 10 kHz control loop must not block and must run from IRAM (`CONTROL_HOT`);
  see FIRMWARE.md section 17.
- Internal RAM is tight (~62 KB free in HID mode; the 115 KB frame sprite stays internal for
  frame rate). Put new buffers in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`,
  `EXT_RAM_BSS_ATTR`, `xQueueCreateWithCaps`); `tasks_common.h` says which task stacks may live
  there. The console's `memory_report()` (sysmon.c, 10 s and 60 s after boot) shows the heaps.
- USB has no free IN endpoint (5 with EP0, all used): MIDI mode is a second USB personality
  (USB MIDI instead of the keyboard / mouse HID) and switching re-enumerates; see
  `NanoDepsidf/docs/FIRMWARE.md` section 9 before adding a USB interface.
- USB power is 5 V 3 A, always (the user's rule): `pd_status.c` asks again at boot if a charger
  gave more, and the STUSB4500's NVM is set to 5 V 3 A only.
- The Mac service is `NanoDepsidf/tools/mac/quadrad.py` (USB, else WiFi). The knob serves two
  WiFi clients at once (the app and the service); every client must send something every
  second or two, or a third one takes its slot.
- When a change alters what the device or the companion shows or does, update `README.md`,
  `companion/docs/COMPANION.md` and the affected screenshots in the same change.
