// App profiles as JSON -- a mirror of NanoDepsidf/src/app_profiles/profile_json.h (the format)
// and app_profile.h (what the fields mean). Change both. A field that's missing reads as 0 /
// false / none on the device, so the editor leaves out what's 0.

export const PROFILE_FORMAT = 1;

export type Key = [modifier: number, keycode: number];
export type Element = [op: number, color: number, x: number, y: number, w: number, h: number, arg: number, d: number, flags: number];

export const KINDS = ["none", "drag", "wheel", "keys", "tap", "commands", "media"] as const;
// "media": Consumer-page usages in a key's keycode (`cw` / `ccw` on the knob, `cw` on a key,
// sent on press) -- the MUSIC profile. Shift+Option on a volume key = a quarter step on a Mac.
export const MEDIA_USAGES = [
  { usage: 0xcd, label: "PLAY / PAUSE" },
  { usage: 0xb5, label: "NEXT" },
  { usage: 0xb6, label: "PREVIOUS" },
  { usage: 0xe9, label: "VOLUME +" },
  { usage: 0xea, label: "VOLUME -" },
  { usage: 0xe2, label: "MUTE" },
] as const;
export const FEELS = ["saw", "sine", "viscose"] as const;
export const FXS = ["none", "zoom", "orbit", "pan", "flash"] as const;
export const SLOTS = ["knob", "f1", "f2", "f3", "f4"] as const;
export type Kind = (typeof KINDS)[number];
export type FeelName = (typeof FEELS)[number];
export type SlotName = (typeof SLOTS)[number];

export interface Action {
  kind?: Kind;
  label?: string;
  buttons?: number; // mouse buttons: 1 left, 2 right, 4 middle
  modifier?: number; // held during a drag / wheel
  axis_y?: boolean;
  px_per_rad?: number;
  sign?: number; // +1 / -1
  cw?: Key;
  ccw?: Key;
  tap?: Key;
  macro?: string; // TAP: runs this macro instead of `cw`
  tap_macro?: string; // quick tap: runs this macro instead of `tap`
  feel?: FeelName;
  detents?: number; // 0 = the HAPTICS tab's STEPS
  fx?: (typeof FXS)[number];
}

export interface Scene {
  base?: Element[];
  frames: { ms: number; el: Element[] }[];
}

export interface Param {
  label: string;
  label_neg?: string;
  steps?: [number, number, number];
  free_step?: number;
  px_per_step?: number;
  start?: number;
  min?: number;
  max?: number;
  decimals?: number;
  deg?: boolean;
  axes?: boolean;
  planes?: boolean;
  uniform?: boolean;
  visual?: string;
  modes?: number;
  enter?: Key;
  axis_default?: number;
}

export interface Command {
  name: string;
  kind?: "keys" | "actions" | "macro";
  key?: Key;
  phrase?: string;
  macro?: string;
  scene?: Scene;
  param?: Param;
}

export interface Ring {
  name: string;
  tab: string;
  slot?: SlotName;
  cmds: Command[];
}

// Macros run on the knob: key taps, text (plain ASCII, typed as US keys) and pauses.
export type Step = { key: Key } | { text: string } | { wait: number };
export interface Macro {
  name: string;
  steps: Step[];
}

export interface ProfileJson {
  format: number;
  id: string;
  name: string;
  legend: [string, string, string, string];
  icon48?: string; // base64 RGB565 big-endian
  icon24?: string;
  visual?: "label" | "shape";
  shape?: "cube" | "pyramid" | "octa";
  shape_style?: "face" | "grips" | "thick";
  shape_stepped?: boolean;
  accents?: [number, number, number]; // "plasma" before 2026-10-04: the firmware reads both
  slots?: Partial<Record<SlotName, Action>>;
  rings?: Ring[];
  search?: { open?: Key; open_wait?: number; result_wait?: number };
  param_keys?: Record<string, unknown>; // kept as it came (parameter mode isn't edited yet)
  macros?: Macro[];
}

// Limits the device checks (profile_json.c).
export const MAX = { id: 11, name: 15, legend: 7, label: 23, tab: 6, phrase: 60, rings: 8, cmds: 32, macros: 16, steps: 64, text: 120, waitMs: 10000 };
export const ASCII_RE = /^[\x20-\x7e]*$/;
export const ID_RE = /^[a-z0-9_-]{1,11}$/;

export function blankProfile(id: string, name: string): ProfileJson {
  return {
    format: PROFILE_FORMAT,
    id,
    name,
    legend: ["-", "-", "-", "MENU"],
    slots: { knob: { kind: "wheel", label: "SCROLL", sign: 1 } },
  };
}

// What's wrong with a profile before the device says so, or null.
export function problem(p: ProfileJson): string | null {
  if (!ID_RE.test(p.id)) return "The id: a-z, 0-9, _ and - (up to 11)";
  if (!p.name || p.name.length > MAX.name) return `The name: 1 to ${MAX.name} characters`;
  if (p.legend.some((l) => l.length > MAX.legend)) return `Key labels: up to ${MAX.legend} characters`;
  const wheel = Object.values(p.slots ?? {}).some((a) => a?.kind === "commands");
  if (wheel && !(p.rings && p.rings.length)) return "An input opens the command wheel, but it has no rings";
  const names = new globalThis.Set<string>();
  for (const m of p.macros ?? []) {
    if (!m.name) return "Every macro needs a name";
    if (names.has(m.name)) return `Two macros are called ${m.name}`;
    names.add(m.name);
    if (m.steps.length > MAX.steps) return `${m.name}: up to ${MAX.steps} steps`;
    for (const st of m.steps) {
      if ("text" in st && (!ASCII_RE.test(st.text) || st.text.length > MAX.text)) return `${m.name}: text can only use plain ASCII (no accents or emoji)`;
    }
  }
  const refOk = (n?: string) => !n || names.has(n);
  for (const a of Object.values(p.slots ?? {})) {
    if (!refOk(a?.macro) || !refOk(a?.tap_macro)) return "An input uses a macro that's gone";
  }
  for (const r of p.rings ?? []) {
    if (!r.name || !r.tab) return "Every ring needs a name and a tab";
    if (!r.cmds.length) return `Ring ${r.name}: no commands`;
    for (const c of r.cmds) {
      if (!c.name) return `Ring ${r.name}: a command without a name`;
      if (c.kind === "actions" && !c.phrase) return `${c.name}: the search text is missing`;
      if (c.kind === "actions" && !p.search?.open?.[1]) return `${c.name}: uses search, but no search key is set`;
      if (c.kind === "macro" && (!c.macro || !refOk(c.macro))) return `${c.name}: pick a macro`;
    }
  }
  return null;
}

// Drops what's 0 / empty, the way the device writes it, so a round trip is stable.
export function tidy(p: ProfileJson): ProfileJson {
  const clean = (v: unknown): unknown => {
    if (Array.isArray(v)) return v.map(clean);
    if (v && typeof v === "object") {
      const o: Record<string, unknown> = {};
      for (const [k, x] of Object.entries(v)) {
        const c = clean(x);
        if (c === 0 || c === false || c === undefined || c === null || c === "") continue;
        if (k === "kind" && c === "none") continue;
        if ((k === "feel" && c === "saw") || (k === "fx" && c === "none")) continue;
        if (Array.isArray(c) && c.length === 2 && c[0] === 0 && c[1] === 0 && k !== "legend") continue; // an empty key
        o[k] = c;
      }
      return o;
    }
    return v;
  };
  const out = clean(p) as ProfileJson;
  out.legend = p.legend; // "" is a valid label
  // A step is its one field, even when that's 0 or "" (a pause of 0, empty text).
  if (p.macros?.length) out.macros = p.macros.map((m) => ({ name: m.name, steps: m.steps.map((st) => ({ ...st })) }));
  out.format = PROFILE_FORMAT;
  return out;
}

// --- keys ---

export const Mod = { CTRL: 0x01, SHIFT: 0x02, ALT: 0x04, GUI: 0x08 } as const;
const RIGHT_MODS = 0xf0; // right-hand modifiers count as left ones here

// KeyboardEvent.code -> HID usage (keyboard page). Positions, not characters: US layout.
const CODE_TO_HID: Record<string, number> = { Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46, BracketLeft: 47, BracketRight: 48, Backslash: 49, Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55, Slash: 56, CapsLock: 57, PrintScreen: 70, ScrollLock: 71, Pause: 72, Insert: 73, Home: 74, PageUp: 75, Delete: 76, End: 77, PageDown: 78, ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82, NumLock: 83, NumpadDivide: 84, NumpadMultiply: 85, NumpadSubtract: 86, NumpadAdd: 87, NumpadEnter: 88, NumpadDecimal: 99, IntlBackslash: 100, NumpadEqual: 103 };
for (let i = 0; i < 26; i++) CODE_TO_HID[`Key${String.fromCharCode(65 + i)}`] = 4 + i;
for (let i = 1; i <= 9; i++) CODE_TO_HID[`Digit${i}`] = 29 + i;
CODE_TO_HID.Digit0 = 39;
for (let i = 1; i <= 12; i++) CODE_TO_HID[`F${i}`] = 57 + i;
for (let i = 13; i <= 24; i++) CODE_TO_HID[`F${i}`] = 91 + i;
for (let i = 1; i <= 9; i++) CODE_TO_HID[`Numpad${i}`] = 88 + i;
CODE_TO_HID.Numpad0 = 98;

const HID_NAME: Record<number, string> = { 40: "ENTER", 41: "ESC", 42: "BKSP", 43: "TAB", 44: "SPACE", 45: "-", 46: "=", 47: "[", 48: "]", 49: "\\", 51: ";", 52: "'", 53: "`", 54: ",", 55: ".", 56: "/", 57: "CAPS", 70: "PRTSC", 71: "SCRLK", 72: "PAUSE", 73: "INS", 74: "HOME", 75: "PGUP", 76: "DEL", 77: "END", 78: "PGDN", 79: "RIGHT", 80: "LEFT", 81: "DOWN", 82: "UP", 83: "NUMLK", 84: "NUM/", 85: "NUM*", 86: "NUM-", 87: "NUM+", 88: "NUMENT", 99: "NUM.", 100: "ISO\\", 103: "NUM=" };
for (let i = 0; i < 26; i++) HID_NAME[4 + i] = String.fromCharCode(65 + i);
for (let i = 1; i <= 9; i++) HID_NAME[29 + i] = String(i);
HID_NAME[39] = "0";
for (let i = 1; i <= 12; i++) HID_NAME[57 + i] = `F${i}`;
for (let i = 13; i <= 24; i++) HID_NAME[91 + i] = `F${i}`;
for (let i = 1; i <= 9; i++) HID_NAME[88 + i] = `NUM${i}`;
HID_NAME[98] = "NUM0";

export function hidFromCode(code: string): number | undefined {
  return CODE_TO_HID[code];
}

export function modifiersFromEvent(e: KeyboardEvent): number {
  return (e.ctrlKey ? Mod.CTRL : 0) | (e.shiftKey ? Mod.SHIFT : 0) | (e.altKey ? Mod.ALT : 0) | (e.metaKey ? Mod.GUI : 0);
}

// "CMD+SHIFT+Z" -- Mac names: the device sends Cmd as Ctrl when BINDINGS = PC.
export function modifierName(m: number): string {
  m = (m & 0x0f) | ((m & RIGHT_MODS) >> 4);
  const parts: string[] = [];
  if (m & Mod.CTRL) parts.push("CTRL");
  if (m & Mod.ALT) parts.push("OPT");
  if (m & Mod.SHIFT) parts.push("SHIFT");
  if (m & Mod.GUI) parts.push("CMD");
  return parts.join("+");
}

export function keyName(k: Key | undefined): string {
  if (!k || (k[0] === 0 && k[1] === 0)) return "";
  const mods = modifierName(k[0]);
  const key = k[1] ? (HID_NAME[k[1]] ?? `#${k[1]}`) : "";
  return [mods, key].filter(Boolean).join("+");
}

export const BUTTON_NAMES: [number, string][] = [
  [1, "LEFT"],
  [2, "RIGHT"],
  [4, "MIDDLE"],
];

// --- icons ---

export function b64ToBytes(s: string): Uint8Array {
  const bin = atob(s);
  const b = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) b[i] = bin.charCodeAt(i);
  return b;
}

export function bytesToB64(b: Uint8Array): string {
  let s = "";
  for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode(...b.subarray(i, i + 0x8000));
  return btoa(s);
}

// RGB565 big-endian (what the firmware draws with swap565_t) -> ImageData. Black is
// transparent on the device, and here too.
export function rgb565ToImage(b: Uint8Array, size: number): ImageData {
  const img = new ImageData(size, size);
  for (let i = 0; i < size * size; i++) {
    const v = (b[i * 2] << 8) | b[i * 2 + 1];
    const r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, bl = v & 0x1f;
    img.data[i * 4] = (r << 3) | (r >> 2);
    img.data[i * 4 + 1] = (g << 2) | (g >> 4);
    img.data[i * 4 + 2] = (bl << 3) | (bl >> 2);
    img.data[i * 4 + 3] = v ? 255 : 0;
  }
  return img;
}

export function iconImage(p: ProfileJson, size: 48 | 24 = 48): ImageData | null {
  const s = size === 48 ? p.icon48 : p.icon24;
  return s ? rgb565ToImage(b64ToBytes(s), size) : null;
}

// Any image -> the device's icon: scaled to fit `size`, on black (transparency -> black, which
// the device treats as see-through), RGB565 big-endian, base64.
export function imageToIcon(src: CanvasImageSource & { width: number; height: number }, size: number): string {
  const c = document.createElement("canvas");
  c.width = c.height = size;
  const ctx = c.getContext("2d")!;
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, size, size);
  const k = Math.min(size / src.width, size / src.height);
  const w = Math.round(src.width * k), h = Math.round(src.height * k);
  ctx.imageSmoothingQuality = "high";
  ctx.drawImage(src, Math.floor((size - w) / 2), Math.floor((size - h) / 2), w, h);
  const d = ctx.getImageData(0, 0, size, size).data;
  const out = new Uint8Array(size * size * 2);
  for (let i = 0; i < size * size; i++) {
    const a = d[i * 4 + 3] / 255;
    const r = d[i * 4] * a, g = d[i * 4 + 1] * a, b = d[i * 4 + 2] * a;
    const v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    out[i * 2] = v >> 8;
    out[i * 2 + 1] = v & 0xff;
  }
  return bytesToB64(out);
}
