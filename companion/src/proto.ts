// The Quadra companion protocol -- a mirror of NanoDepsidf/src/host_proto.h. Change both.
// 64-byte reports, no report ID; [0] = command (to the device) or reply tag (from it).
// Little-endian; floats are IEEE-754 single.

export const REPORT_SIZE = 64;
export const PROTO_VERSION = 3;
export const TEXT_CHUNK = 60; // profile JSON per report
export const ICON_BYTES = 48 * 48 * 2;
export const ICON_CHUNK = 56; // icon bytes per report (HOST_ICON_CHUNK)
export const LED_COUNT = 68; // 0-59 the ring (clockwise from 12 o'clock), 60-67 the keys, two each

export const Cmd = {
  HELLO: 0x10,
  GET_SETTINGS: 0x11,
  SET: 0x12,
  SAVE: 0x13,
  REVERT: 0x14,
  STREAM: 0x15,
  PROFILE: 0x16,
  PROFILE_ICON: 0x17,
  RESET_PEAKS: 0x18,
  PROFILE_READ: 0x19,
  UPLOAD_BEGIN: 0x1a,
  UPLOAD_DATA: 0x1b,
  UPLOAD_END: 0x1c,
  PROFILE_OP: 0x1d,
  HAPTIC_RESET: 0x1e,
} as const;

export const UploadFlag = { SAVE: 0x01 } as const;
export const Op = { SAVE: 1, REVERT: 2, REMOVE: 3 } as const;
export const Res = { OK: 0, INVALID: 1, TRANSFER: 2, FULL: 3, STORAGE: 4, BAD_INDEX: 5, BUSY: 6 } as const;
export const RES_TEXT = ["OK", "Not a valid profile", "The transfer failed", "No room for more profiles", "Storage error", "No such profile", "The knob is busy"];

// HOST_TAG_PROFILE [3]
export const ProfileFlag = { ICON: 0x01, BUILTIN: 0x02, STORED: 0x04, LIVE: 0x08 } as const;

export const Tag = {
  HELLO: 0xb0,
  SETTINGS: 0xb1,
  PROFILE: 0xb2,
  PROFILE_ICON: 0xb3,
  LEDS: 0xb4,
  STATE: 0xb5,
  SYS_A: 0xb6,
  SYS_B: 0xb7,
  PROFILE_BEGIN: 0xb8,
  PROFILE_DATA: 0xb9,
  RESULT: 0xba,
  ERROR: 0xbf,
  ICON_UPLOAD: 0xa0, // icon_store.h
} as const;

// Extensions -- a mirror of NanoDepsidf/src/ext_proto.h (commands 0x20-0x2F, tags 0xC0-0xCF).
// Firmware without them answers Tag.ERROR, and the app leaves out what needs them.
export const ExtCmd = { HELLO: 0x20, TEXT: 0x22, LIGHTS: 0x23, PREFS: 0x24, NET: 0x29, CLOCK: 0x2b, SCREEN: 0x2c, INPUT: 0x2d, MUSIC: 0x2e } as const;
export const ExtTag = { HELLO: 0xc0, ACK: 0xc1, PREFS: 0xc2, NET: 0xc4, CLOCK: 0xc5, SCREEN: 0xc6, KEY: 0xc7 } as const;
export const EXT_SCREEN_VERSION = 6; // the live screen (EXT_CMD_SCREEN) and the knob from here (EXT_CMD_INPUT)
export const InputOp = { KEYS: 1, TURN: 2 } as const;
export const SCREEN_SIZE = 240;
export const EXT_CLOCK_VERSION = 5; // the CLOCK app (EXT_CMD_CLOCK) from this extensions version on
export const ClockOp = { FORMAT: 1, ZONE: 2, GET: 3 } as const;
export const ClockFlag = { H24: 0x01, SECONDS: 0x02, DATE: 0x04, LED: 0x08 } as const; // clock.h CLOCK_*
export const CLOCK_SLOTS = 5; // 0 = LOCAL (the Mac service sends it), 1-4 the user's
export const EXT_NET_VERSION = 4; // WiFi (EXT_CMD_NET) from this extensions version on
export const NetOp = { SSID: 1, PASS_A: 2, PASS_B: 3, APPLY: 4, STATUS: 5, KEY: 6, CONTROLS: 7 } as const;
// The companion over WiFi (net_link.h): paired over USB with the knob's key (NetOp.KEY).
export const EXT_WIFI_LINK_VERSION = 7;
// The knob's controls over WiFi (NetOp.CONTROLS; the reports, EXT_TAG_HID, are handled in Rust:
// src-tauri/src/input.rs) from this extensions version on.
export const EXT_CONTROLS_VERSION = 10;
export const NET_STATE = ["OFF", "CONNECTING", "CONNECTED", "NETWORK NOT FOUND", "WRONG PASSWORD"] as const; // net_state_t
export const NetState = { OFF: 0, CONNECTING: 1, CONNECTED: 2 } as const;
export const ExtStatus = { OK: 0, BAD_PARAM: 1, UNKNOWN: 2, STORAGE: 3, USB_ONLY: 4 } as const;
export const EXT_LIGHTS_SAVE = 0x01;
export const IDLE_TEXT_MAX = 12; // USER_TEXT_MAX: printable ASCII; "" = QUADRA
export const LightSrc = { APP: 0, CUSTOM: 1 } as const;
export const LIGHT_FX = ["GRADIENT", "SOLID", "BREATHE", "SPIN", "RAINBOW", "OFF"] as const; // LIGHT_FX_*, in order
export const LIGHT_FX_MOVING = new globalThis.Set([2, 3, 4]); // the ones SPEED changes
// MUSIC's cover on the now-playing screen (user_prefs.h cover_style_t, EXT_CMD_MUSIC, v8), in order.
export const COVER_STYLES = ["FLAT", "RECORD", "SLIDE", "BLEED"] as const;

export interface Lights {
  src: number;
  fx: number;
  hue: number; // 0..359
  sat: number; // 0..100
  speed: number; // 1..10
  level: number; // % of the stock brightness, 10..200
}
export interface Prefs extends Lights {
  lightsDirty: boolean; // differ from what's saved
  idleText: string;
  coverStyle: number; // COVER_STYLES
  coverStyles: number; // how many the knob has (0: firmware before extensions v8)
}
// One report of the screen stream (screen_stream.h): a slice of the frame numbered `seq`.
export interface ScreenChunk {
  seq: number;
  first: boolean;
  last: boolean;
  bytes: Uint8Array;
}
export interface ClockSlot {
  flags: number; // ClockFlag
  valid: boolean; // the knob's clock is set
  slot: number;
  offsetMin: number; // the zone's UTC offset now
  label: string; // "" = an empty slot
  rule: string; // POSIX TZ
}
export interface Net {
  state: number; // NetState / NET_STATE
  on: boolean;
  rssi: number; // dBm, while connected
  ip: string; // "" = none
  timeSet: boolean; // the knob's clock is set (SNTP)
  ssid: string;
  host: string; // <host>.local
}

// HOST_SET_* -- also the bit order of Settings.dirty.
export const Set = {
  DETENTS: 0,
  KP: 1,
  KD: 2,
  FEEL: 3,
  AMP: 4,
  PITCH: 5,
  SOUND: 6,
  HID_TYPE: 7,
  MIDI_CH: 8,
  PROFILE: 9,
  BOOT: 10,
  ROTATION: 11,
  HOST: 12,
  SHAPE: 13,
  HAPTIC_PROFILE: 14,
  MODE_HAPTIC: 15,
  MIDI_SYNTH: 16,
} as const;
export type SetId = (typeof Set)[keyof typeof Set];
const FLOAT_SETTINGS: ReadonlySet<number> = new globalThis.Set([Set.KP, Set.KD, Set.PITCH]);

// Enums as the firmware stores them (menu.h, haptic_params.h, boot_mode.h).
export const Feel = { SAW: 0, SINE: 1, VISCOSE: 2 } as const;
export const HidType = { KEYBOARD: 0, MOUSE: 1, MIDI: 2, APP: 3, HOME: 4 } as const;
export const Host = { MAC: 0, PC: 1 } as const;
export const Boot = { SERIAL: 0, HID: 1 } as const; // boot_usb_mode_t

// Limits, as the firmware clamps them (haptic_params.h, audio_trigger.h).
// The haptic profiles (haptic_params.h HAPTIC_PROFILES), by id. KP, KD, SHAPE, FEEL, AMP and
// PITCH in Settings are the values of one of them (Settings.hapticProfile) in its feel.
export const HapticProfiles = [
  { name: "WIDE", detents: 8 },
  { name: "COARSE", detents: 12 },
  { name: "MEDIUM", detents: 24 },
  { name: "FINE", detents: 36 },
  { name: "SMOOTH", detents: 0 }, // VISCOSE only: no felt steps
] as const;

// MIDI mode's synth profiles (midi_synths.c SYNTHS), by index; the knob sends the index.
export const MidiSynths = [
  { id: "generic", maker: "MIDI", name: "GENERIC", channel: 0, params: 18 },
  { id: "minilogue-xd", maker: "KORG", name: "MINILOGUE XD", channel: 1, params: 48 },
  { id: "ju-06a", maker: "ROLAND", name: "JU-06A", channel: 1, params: 33 },
  { id: "tr-8s", maker: "ROLAND", name: "TR-8S", channel: 10, params: 54 },
] as const;

// The widest ranges; a profile's own limits come with Settings.
export const Limits = {
  detents: { min: 3, max: 36, step: 1 },
  kp: { min: 0, max: 20, step: 0.05 },
  kd: { min: 0, max: 0.15, step: 0.005 },
  shape: { min: 0, max: 90, step: 5 },
  amp: { min: 0, max: 100, step: 5 },
  pitch: { min: 0.5, max: 2, step: 0.05 },
};

export interface Hello {
  proto: number;
  profileCount: number;
  serialBoot: boolean;
  version: string;
  date: string;
}

export interface Settings {
  dirty: number;
  detents: number;
  kp: number;
  kd: number;
  feel: number;
  amp: number;
  pitch: number;
  sound: number;
  hidType: number;
  midiChannel: number;
  midiSynth: number; // index into MidiSynths
  profile: number;
  boot: number;
  rotation: number;
  host: number;
  shape: number; // percent; 0 from firmware before SHAPE existed
  hapticProfile: number; // index into HapticProfiles: the one the haptic values are of
  feels: number; // the feels it allows, 1 << Feel
  modeHaptic: number; // the haptic profile the current HID type uses
  kpMin: number;
  kpMax: number;
  kdMin: number;
  kdMax: number;
  ampMax: number;
  pitchMin: number;
  pitchMax: number;
}

export interface Profile {
  index: number;
  count: number;
  flags: number; // ProfileFlag
  hasIcon: boolean;
  id: string;
  name: string;
  legend: string[];
}

export interface State {
  seq: number;
  angle: number; // rad, continuous
  detent: number;
  buttons: number; // bit0 F1 .. bit3 F4
  menuScreen: number; // 0 = menu closed
  screensaver: boolean;
  liveSlot: number;
  clicks: number;
  walls: number;
}

export interface SysA {
  motorMa: number;
  ledMa: number;
  boardMa: number;
  totalMa: number;
  totalPeakMa: number;
  chipOk: boolean;
  chipC: number;
  chipPeakC: number;
  coilMa: number;
  coilPeakMa: number;
  copperW: number;
  usbSource: number; // pd_source_t
  usbMa: number;
  usbMv: number;
}

export interface SysB {
  load: [number, number];
  loadPeak: [number, number];
  loopKhz: number;
  workAvgUs: number;
  workMaxUs: number;
  jitterUs: number;
  missed: number;
  spikesPerS: number;
  heapFree: number;
  heapMin: number;
  hidDrops: number;
  audioGaps: number;
  uptimeS: number;
  sensorCrcErrors: number;
}

export interface IconChunk {
  index: number;
  offset: number;
  bytes: Uint8Array;
}

export interface Result {
  cmd: number;
  res: number; // Res
  index: number;
  count: number;
  removed: boolean;
  why: string;
}

export type Message =
  | { tag: typeof Tag.HELLO; hello: Hello }
  | { tag: typeof Tag.SETTINGS; settings: Settings }
  | { tag: typeof Tag.PROFILE; profile: Profile }
  | { tag: typeof Tag.PROFILE_ICON; chunk: IconChunk }
  | { tag: typeof Tag.STATE; state: State }
  | { tag: typeof Tag.SYS_A; sys: SysA }
  | { tag: typeof Tag.SYS_B; sys: SysB }
  | { tag: typeof Tag.LEDS; first: number; rgb: Uint8Array }
  | { tag: typeof Tag.PROFILE_BEGIN; index: number; length: number; crc: number }
  | { tag: typeof Tag.PROFILE_DATA; offset: number; bytes: Uint8Array }
  | { tag: typeof Tag.RESULT; result: Result }
  | { tag: typeof Tag.ERROR; cmd: number; code: number }
  | { tag: typeof ExtTag.HELLO; ext: number }
  | { tag: typeof ExtTag.ACK; cmd: number; status: number }
  | { tag: typeof ExtTag.PREFS; prefs: Prefs }
  | { tag: typeof ExtTag.NET; net: Net }
  | { tag: typeof ExtTag.CLOCK; clock: ClockSlot }
  | { tag: typeof ExtTag.SCREEN; screen: ScreenChunk }
  | { tag: typeof ExtTag.KEY; key: Uint8Array; port: number }
  | { tag: number };

// --- encoding ---

function report(cmd: number): Uint8Array {
  const r = new Uint8Array(REPORT_SIZE);
  r[0] = cmd;
  return r;
}

export const encode = {
  hello: () => report(Cmd.HELLO),
  getSettings: () => report(Cmd.GET_SETTINGS),
  save: () => report(Cmd.SAVE),
  revert: () => report(Cmd.REVERT),
  resetPeaks: () => report(Cmd.RESET_PEAKS),
  hapticReset: () => report(Cmd.HAPTIC_RESET),
  stream: (hz: number) => {
    const r = report(Cmd.STREAM);
    r[1] = Math.max(0, Math.min(50, Math.round(hz)));
    return r;
  },
  set: (id: SetId, value: number) => {
    const r = report(Cmd.SET);
    r[1] = id;
    const v = new DataView(r.buffer);
    if (FLOAT_SETTINGS.has(id)) v.setFloat32(4, value, true);
    else v.setInt32(4, Math.round(value), true);
    return r;
  },
  profile: (index: number) => {
    const r = report(Cmd.PROFILE);
    r[1] = index;
    return r;
  },
  profileIcon: (index: number, offset: number) => {
    const r = report(Cmd.PROFILE_ICON);
    r[1] = index;
    new DataView(r.buffer).setUint16(2, offset, true);
    return r;
  },
  profileRead: (index: number) => {
    const r = report(Cmd.PROFILE_READ);
    r[1] = index;
    return r;
  },
  uploadBegin: (length: number, crc: number, flags: number) => {
    const r = report(Cmd.UPLOAD_BEGIN);
    r[1] = flags;
    const v = new DataView(r.buffer);
    v.setUint32(4, length, true);
    v.setUint32(8, crc, true);
    return r;
  },
  uploadData: (offset: number, bytes: Uint8Array) => {
    const r = report(Cmd.UPLOAD_DATA);
    r[1] = offset & 0xff;
    r[2] = (offset >> 8) & 0xff;
    r[3] = (offset >> 16) & 0xff;
    r.set(bytes.subarray(0, TEXT_CHUNK), 4);
    return r;
  },
  uploadEnd: () => report(Cmd.UPLOAD_END),
  profileOp: (index: number, op: number) => {
    const r = report(Cmd.PROFILE_OP);
    r[1] = index;
    r[2] = op;
    return r;
  },
  extHello: () => report(ExtCmd.HELLO),
  screen: (fps: number) => {
    const r = report(ExtCmd.SCREEN);
    r[1] = fps;
    return r;
  },
  inputKeys: (mask: number) => {
    const r = report(ExtCmd.INPUT);
    r[1] = InputOp.KEYS;
    r[2] = mask & 0x0f;
    return r;
  },
  inputTurn: (detents: number) => {
    const r = report(ExtCmd.INPUT);
    r[1] = InputOp.TURN;
    r[2] = Math.max(-127, Math.min(127, detents)) & 0xff;
    return r;
  },
  clockGet: (slot: number) => {
    const r = report(ExtCmd.CLOCK);
    r[1] = ClockOp.GET;
    r[2] = slot;
    return r;
  },
  clockFormat: (flags: number) => {
    const r = report(ExtCmd.CLOCK);
    r[1] = ClockOp.FORMAT;
    r[2] = flags;
    return r;
  },
  clockZone: (slot: number, label: string, rule: string) => {
    const r = report(ExtCmd.CLOCK);
    r[1] = ClockOp.ZONE;
    r[2] = slot;
    r.set(new TextEncoder().encode(label.slice(0, 12)), 3);
    r.set(new TextEncoder().encode(rule.slice(0, 45)), 15);
    return r;
  },
  net: (op: number, bytes?: Uint8Array) => {
    const r = report(ExtCmd.NET);
    r[1] = op;
    if (bytes) r.set(bytes.subarray(0, 32), 2);
    return r;
  },
  // The WiFi pairing key (USB only); `fresh`: a new one, which unpairs every other companion.
  netKey: (fresh: boolean) => {
    const r = report(ExtCmd.NET);
    r[1] = NetOp.KEY;
    r[2] = fresh ? 1 : 0;
    return r;
  },
  // WiFi only: this app types and scrolls for the knob while no USB host has it.
  netControls: (on: boolean) => {
    const r = report(ExtCmd.NET);
    r[1] = NetOp.CONTROLS;
    r[2] = on ? 1 : 0;
    return r;
  },
  extPrefs: () => report(ExtCmd.PREFS),
  // MUSIC's cover style (COVER_STYLES index): stored on the knob at once.
  coverStyle: (style: number) => {
    const r = report(ExtCmd.MUSIC);
    r[1] = style;
    return r;
  },
  // The idle word: stored on the knob at once (no SAVE).
  idleText: (text: string) => {
    const r = report(ExtCmd.TEXT);
    r.set(new TextEncoder().encode(text.replace(/[^\x20-\x7e]/g, " ").slice(0, IDLE_TEXT_MAX)), 2);
    return r;
  },
  // Live at once; `save` also stores them (otherwise SAVE does, like the other settings).
  lights: (l: Lights, save: boolean) => {
    const r = report(ExtCmd.LIGHTS);
    const v = new DataView(r.buffer);
    r[1] = save ? EXT_LIGHTS_SAVE : 0;
    r[2] = l.src;
    r[3] = l.fx;
    v.setUint16(4, l.hue, true);
    r[6] = l.sat;
    r[7] = l.speed;
    v.setUint16(8, l.level, true);
    return r;
  },
};

// zlib's CRC-32, as the firmware checks it.
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
export function crc32(b: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < b.length; i++) c = CRC_TABLE[(c ^ b[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// --- decoding ---

function str(b: Uint8Array, at: number, n: number): string {
  let end = at;
  while (end < at + n && b[end] !== 0) end++;
  return new TextDecoder().decode(b.subarray(at, end));
}

export function decode(b: Uint8Array): Message {
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const u16 = (o: number) => v.getUint16(o, true);
  const i32 = (o: number) => v.getInt32(o, true);
  const u32 = (o: number) => v.getUint32(o, true);
  const f32 = (o: number) => v.getFloat32(o, true);
  switch (b[0]) {
    case Tag.HELLO:
      return {
        tag: Tag.HELLO,
        hello: { proto: b[1], profileCount: b[2], serialBoot: b[3] === 1, version: str(b, 4, 32), date: str(b, 36, 16) },
      };
    case Tag.SETTINGS:
      return {
        tag: Tag.SETTINGS,
        settings: {
          dirty: u16(1) | (b[3] << 16), // 24 bits: [3] carries MIDI_SYNTH and on
          detents: i32(4),
          kp: f32(8),
          kd: f32(12),
          feel: b[16],
          amp: b[17],
          pitch: f32(18),
          sound: b[22],
          hidType: b[23],
          midiChannel: b[24],
          midiSynth: b[35] ? b[34] : 0, // [35] = how many: 0 from firmware before MIDI synths
          profile: b[25],
          boot: b[26],
          rotation: b[27],
          host: b[28],
          shape: b[29],
          // Firmware before haptic profiles (protocol 2) leaves these zero: the old ranges.
          ...(b[31]
            ? { hapticProfile: b[30], feels: b[31], ampMax: b[32], modeHaptic: b[33], kpMin: f32(36), kpMax: f32(40), kdMin: f32(44), kdMax: f32(48), pitchMin: f32(52), pitchMax: f32(56) }
            : { hapticProfile: 1, feels: 7, ampMax: 100, modeHaptic: 1, kpMin: Limits.kp.min, kpMax: Limits.kp.max, kdMin: Limits.kd.min, kdMax: Limits.kd.max, pitchMin: Limits.pitch.min, pitchMax: Limits.pitch.max }),
        },
      };
    case Tag.PROFILE:
      return {
        tag: Tag.PROFILE,
        profile: {
          index: b[1],
          count: b[2],
          flags: b[3],
          hasIcon: (b[3] & ProfileFlag.ICON) !== 0,
          id: str(b, 4, 12),
          name: str(b, 16, 16),
          legend: [0, 1, 2, 3].map((i) => str(b, 32 + i * 8, 8)),
        },
      };
    case Tag.PROFILE_ICON:
      return { tag: Tag.PROFILE_ICON, chunk: { index: b[1], offset: u16(2), bytes: b.slice(8, 8 + b[4]) } };
    case Tag.STATE:
      return {
        tag: Tag.STATE,
        state: {
          seq: u16(1),
          angle: i32(4) / 1e4,
          detent: i32(8),
          buttons: b[12],
          menuScreen: b[13],
          screensaver: b[14] === 1,
          liveSlot: b[15],
          clicks: i32(16),
          walls: i32(20),
        },
      };
    case Tag.SYS_A:
      return {
        tag: Tag.SYS_A,
        sys: {
          motorMa: u16(4),
          ledMa: u16(6),
          boardMa: u16(8),
          totalMa: u16(10),
          totalPeakMa: u16(12),
          chipOk: b[14] === 1,
          chipC: f32(16),
          chipPeakC: f32(20),
          coilMa: u16(24),
          coilPeakMa: u16(26),
          copperW: f32(28),
          usbSource: b[32],
          usbMa: u16(34),
          usbMv: u16(36),
        },
      };
    case Tag.SYS_B:
      return {
        tag: Tag.SYS_B,
        sys: {
          load: [b[4], b[5]],
          loadPeak: [b[6], b[7]],
          loopKhz: f32(8),
          workAvgUs: f32(12),
          workMaxUs: f32(16),
          jitterUs: f32(20),
          missed: u32(24),
          spikesPerS: f32(28),
          heapFree: u32(32),
          heapMin: u32(36),
          hidDrops: u32(40),
          audioGaps: u32(44),
          uptimeS: u32(48),
          sensorCrcErrors: u32(52),
        },
      };
    case Tag.LEDS:
      return { tag: Tag.LEDS, first: b[1], rgb: b.slice(4, 4 + Math.min(20, b[2]) * 3) };
    case Tag.PROFILE_BEGIN:
      return { tag: Tag.PROFILE_BEGIN, index: b[1], length: u32(4), crc: u32(8) };
    case Tag.PROFILE_DATA:
      return { tag: Tag.PROFILE_DATA, offset: b[1] | (b[2] << 8) | (b[3] << 16), bytes: b.slice(4, 4 + TEXT_CHUNK) };
    case Tag.RESULT:
      return {
        tag: Tag.RESULT,
        result: { cmd: b[1], res: b[2], index: b[3], count: b[4], removed: b[5] === 1, why: str(b, 8, 56) },
      };
    case Tag.ERROR:
      return { tag: Tag.ERROR, cmd: b[1], code: b[2] };
    case ExtTag.HELLO:
      return { tag: ExtTag.HELLO, ext: b[1] };
    case ExtTag.ACK:
      return { tag: ExtTag.ACK, cmd: b[1], status: b[2] };
    case ExtTag.KEY:
      return { tag: ExtTag.KEY, key: b.slice(1, 33), port: u16(33) };
    case ExtTag.SCREEN:
      return { tag: ExtTag.SCREEN, screen: { seq: b[1], first: (b[2] & 1) !== 0, last: (b[2] & 2) !== 0, bytes: b.slice(4, 4 + Math.min(60, b[3])) } };
    case ExtTag.CLOCK:
      return { tag: ExtTag.CLOCK, clock: { flags: b[1], valid: b[2] === 1, slot: b[3], offsetMin: (u16(4) << 16) >> 16, label: str(b, 6, 12), rule: str(b, 18, 46) } };
    case ExtTag.NET:
      return {
        tag: ExtTag.NET,
        net: { state: b[1], rssi: (b[2] << 24) >> 24, ip: b[3] | b[4] | b[5] | b[6] ? `${b[3]}.${b[4]}.${b[5]}.${b[6]}` : "", timeSet: b[7] === 1, on: b[8] === 1, ssid: str(b, 9, 32), host: str(b, 41, 23) },
      };
    case ExtTag.PREFS:
      return {
        tag: ExtTag.PREFS,
        prefs: { src: b[1], fx: b[2], hue: u16(3), sat: b[5], speed: b[6], level: u16(7), lightsDirty: b[9] === 1, coverStyle: b[10], coverStyles: b[11], idleText: str(b, 16, 16) },
      };
    default:
      return { tag: b[0] };
  }
}
