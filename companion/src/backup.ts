// A backup of the knob's setup in one file, and a restore of any part of one (pages/Backup.tsx).
// In it: the settings, the look, the clock, every haptic profile's tuning, the user's app profiles
// and synths (and the built-ins they changed). Not in it, on purpose: the motor and sound
// calibration (they belong to the unit), the WiFi network and the pairing key, and HOME's lamps
// (their tokens can't be read back; Lamps › Import brings them in again).

import { signal } from "@preact/signals";
import type { Opened } from "./files";
import { CLOCK_SLOTS, COVER_STYLES, EXT_CLOCK_VERSION, EXT_IDLE_VERSION, HapticProfiles, HidType, MAX_SYNTHS, ProfileFlag, SAVERS, Set, SynthFlag, type HapticTune, type Lights, type ScreenPrefs } from "./proto";
import { parseProfile, tidy, type ProfileJson } from "./profile";
import { MAX_PROFILES, waitFor } from "./profiles";
import { device } from "./store";
import { parseSynth, tidySynth, type SynthJson } from "./synth";

export const BACKUP_FORMAT = 1;

// A backup dropped on the window (main.tsx), for the Backup tab to open.
export const droppedBackup = signal<Opened | null>(null);

export interface HapticBackup {
  profile: number; // HapticProfiles
  feel: number; // the feel it uses
  click: number; // ClickWaves
  tune: (HapticTune | null)[]; // per feel: SAW, SINE, VISCOSE; null where it has no such feel
}

export interface Backup {
  kind: "quadra-backup";
  format: number;
  made: string; // ISO time
  firmware: string; // the knob's version when it was made
  ext: number; // its extensions version
  settings?: { hidType: number; midiChannel: number; boot: number; rotation: number; host: number; profile?: string; synth?: string };
  look?: { lights: Lights; idleText: string; cover?: number; screen?: ScreenPrefs };
  clock?: { flags: number; zones: { label: string; rule: string }[] }; // zones: slots 1-4
  haptics?: { modes: number[]; profiles: HapticBackup[] }; // modes: KEYBOARD, MOUSE, MIDI, APP
  profiles?: ProfileJson[];
  synths?: SynthJson[];
}

export type Section = "settings" | "look" | "clock" | "haptics" | "profiles" | "synths";
export const SECTION_TITLES: Record<Section, string> = {
  settings: "Mode and device",
  look: "Look",
  clock: "Clock",
  haptics: "Haptics",
  profiles: "App profiles",
  synths: "Synths",
};

const MODE_NAMES: Record<number, string> = { [HidType.APP]: "App", [HidType.HOME]: "Home", [HidType.MOUSE]: "Mouse", [HidType.KEYBOARD]: "Keys", [HidType.MIDI]: "MIDI" };
const MODE_HAPTIC_NAMES = ["Keys", "Mouse", "MIDI", "App"];
const FEEL_NAMES = ["Saw", "Sine", "Viscose"];
const hhmm = (m: number) => `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;
const hapticName = (i: number) => HapticProfiles[i]?.name ?? `#${i + 1}`;

export const dated = (prefix: string) => {
  const d = new Date();
  const p = (n: number) => String(n).padStart(2, "0");
  return `${prefix}-${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}.json`;
};

// --- making one ---

// Every haptic profile in every feel (extensions v14).
async function readHaptics(): Promise<NonNullable<Backup["haptics"]>> {
  const first = await device.getHaptic(0, 0);
  const profiles: HapticBackup[] = [];
  for (let p = 0; p < first.profiles; p++) {
    const tune: (HapticTune | null)[] = [];
    let feel = 0, click = 0;
    for (let f = 0; f < first.feelCount; f++) {
      const h = p === 0 && f === 0 ? first : await device.getHaptic(p, f);
      feel = h.useFeel;
      click = h.click;
      tune.push((h.feels >> f) & 1 ? h.tune : null);
    }
    profiles.push({ profile: p, feel, click, tune });
  }
  return { modes: first.modes, profiles };
}

const isUsers = (flags: number, builtin: number, changed: number) => !(flags & builtin) || (flags & changed) !== 0;

// What's on the knob now (live: what isn't saved yet too). `step` says what it's reading.
export async function makeBackup(step: (what: string) => void = () => {}): Promise<Backup> {
  const s = device.settings, pr = device.prefs, ext = device.ext ?? 0;
  if (!s) throw new Error("The knob isn't connected");
  const b: Backup = { kind: "quadra-backup", format: BACKUP_FORMAT, made: new Date().toISOString(), firmware: device.hello?.version ?? "", ext };
  b.settings = {
    hidType: s.hidType,
    midiChannel: s.midiChannel,
    boot: s.boot,
    rotation: s.rotation,
    host: s.host,
    profile: device.profiles[s.profile]?.id,
    synth: device.synths[s.midiSynth]?.id,
  };
  if (pr) {
    const { src, fx, hue, sat, speed, level } = pr;
    b.look = { lights: { src, fx, hue, sat, speed, level }, idleText: pr.idleText };
    if (pr.coverStyles) b.look.cover = pr.coverStyle;
    const i = device.idle;
    if (ext >= EXT_IDLE_VERSION && i) b.look.screen = { bright: i.bright, saver: i.saver, saverS: i.saverS, sleepOn: i.sleepOn, sleepFrom: i.sleepFrom, sleepTo: i.sleepTo, darkS: i.darkS, wake: i.wake };
  }
  if (ext >= EXT_CLOCK_VERSION && device.clockSlots[0]) {
    const zones = [];
    for (let k = 1; k < CLOCK_SLOTS; k++) zones.push({ label: device.clockSlots[k]?.label ?? "", rule: device.clockSlots[k]?.rule ?? "" });
    b.clock = { flags: device.clockSlots[0].flags, zones };
  }
  if (device.hasHaptics) {
    step("Reading the haptics");
    b.haptics = await readHaptics();
  }
  const mine = device.profiles.filter((p) => p && isUsers(p.flags, ProfileFlag.BUILTIN, ProfileFlag.STORED | ProfileFlag.LIVE));
  b.profiles = [];
  for (const p of mine) {
    step(`Reading ${p.name}`);
    b.profiles.push(tidy(await device.readProfile(p.index)));
  }
  const synths = device.synths.filter((x) => x && isUsers(x.flags, SynthFlag.BUILTIN, SynthFlag.STORED | SynthFlag.LIVE));
  b.synths = [];
  for (const x of synths) {
    step(`Reading ${x.name}`);
    b.synths.push(tidySynth(await device.readSynth(x.index)));
  }
  return b;
}

// --- reading one ---

// A file's backup, checked: what it holds that this app can't use is dropped with a word in
// `notes`. Throws when it isn't a backup at all.
export function parseBackup(data: unknown): { backup: Backup; notes: string[] } {
  const b = data as Backup;
  if (!b || typeof b !== "object" || b.kind !== "quadra-backup") throw new Error("That isn't a Quadra backup");
  if (b.format > BACKUP_FORMAT) throw new Error("That backup is from a newer companion: update the app first");
  const notes: string[] = [];
  if (b.ext > (device.ext ?? 0)) notes.push(`It was made on newer firmware (${b.firmware || "unknown"}): what this knob doesn't have is left out.`);
  return { backup: b, notes };
}

// --- the summary ---

export interface ItemPlan {
  id: string;
  name: string;
  action: "add" | "replace";
  problem: string | null; // can't be restored, and why
}

export interface SectionPlan {
  key: Section;
  present: boolean; // the backup has it
  lines: string[]; // what changes; empty: nothing does
  why?: string; // can't be restored here
}

export interface Plan {
  backup: Backup;
  notes: string[];
  sections: SectionPlan[];
  profiles: ItemPlan[];
  synths: ItemPlan[];
}

function change(lines: string[], what: string, from: string | number | undefined, to: string | number | undefined) {
  if (to !== undefined && from !== to) lines.push(`${what}: ${from ?? "?"} → ${to}`);
}

function itemPlans<T extends { id: string; name: string }>(list: unknown[] | undefined, parse: (d: unknown) => T, have: { id: string }[], max: number, kind: string): ItemPlan[] {
  let count = have.length;
  return (list ?? []).map((d, i) => {
    const raw = d as { id?: string; name?: string };
    const id = typeof raw?.id === "string" ? raw.id : `#${i + 1}`, name = typeof raw?.name === "string" && raw.name ? raw.name : id;
    let problem: string | null = null;
    try {
      parse(structuredClone(d));
    } catch (e) {
      problem = e instanceof Error ? e.message : String(e);
      for (const pre of [`${raw?.name}: `, `${id}: `]) if (problem.startsWith(pre)) problem = problem.slice(pre.length); // the row names it
    }
    const replace = have.some((x) => x.id === id);
    if (!replace && !problem && ++count > max) problem = `The knob holds ${max} ${kind}`;
    return { id, name, action: replace ? "replace" : "add", problem };
  });
}

// What a restore of `b` would change on this knob. Reads the haptics to compare them.
export async function planRestore(b: Backup, notes: string[]): Promise<Plan> {
  const s = device.settings!, pr = device.prefs, ext = device.ext ?? 0;
  const sections: SectionPlan[] = [];

  {
    const lines: string[] = [];
    const x = b.settings;
    if (x) {
      change(lines, "Mode", MODE_NAMES[s.hidType], MODE_NAMES[x.hidType]);
      if (x.profile !== undefined) change(lines, "App profile in use", device.profiles[s.profile]?.name, b.profiles?.find((p) => p.id === x.profile)?.name ?? device.profiles.find((p) => p?.id === x.profile)?.name ?? x.profile);
      if (x.synth !== undefined) change(lines, "Synth in use", device.synths[s.midiSynth]?.name, b.synths?.find((y) => y.id === x.synth)?.name ?? device.synths.find((y) => y?.id === x.synth)?.name ?? x.synth);
      change(lines, "MIDI channel", s.midiChannel, x.midiChannel);
      change(lines, "Key bindings", s.host ? "PC" : "Mac", x.host ? "PC" : "Mac");
      change(lines, "Screen rotation", `${s.rotation * 90}°`, `${x.rotation * 90}°`);
      change(lines, "Boot", s.boot ? "Normal" : "Serial", x.boot ? "Normal" : "Serial");
    }
    sections.push({ key: "settings", present: !!x, lines });
  }

  {
    const lines: string[] = [];
    const x = b.look;
    if (x && pr) {
      const l = x.lights;
      if (l.src !== pr.src || l.fx !== pr.fx || l.hue !== pr.hue || l.sat !== pr.sat || l.speed !== pr.speed || l.level !== pr.level) lines.push("Lights: colour, effect and brightness");
      change(lines, "Idle word", pr.idleText || "QUADRA", x.idleText || "QUADRA");
      if (x.cover !== undefined && pr.coverStyles) change(lines, "Music cover", COVER_STYLES[pr.coverStyle], COVER_STYLES[x.cover]);
      const i = device.idle, sc = x.screen;
      if (sc && i && ext >= EXT_IDLE_VERSION) {
        change(lines, "Brightness", `${i.bright}%`, `${sc.bright}%`);
        change(lines, "Screensaver", SAVERS[i.saver], SAVERS[sc.saver]);
        change(lines, "Screensaver after", `${i.saverS} s`, `${sc.saverS} s`);
        change(lines, "Sleep hours", i.sleepOn ? `${hhmm(i.sleepFrom)}–${hhmm(i.sleepTo)}` : "Off", sc.sleepOn ? `${hhmm(sc.sleepFrom)}–${hhmm(sc.sleepTo)}` : "Off");
        if (sc.darkS !== i.darkS || sc.wake !== i.wake) lines.push("Sleep hours: dark screen and waking");
      }
    }
    sections.push({ key: "look", present: !!x, lines, why: x && !pr ? "This firmware has no look settings" : undefined });
  }

  {
    const lines: string[] = [];
    const x = b.clock;
    const ok = ext >= EXT_CLOCK_VERSION;
    if (x && ok) {
      if (x.flags !== device.clockSlots[0]?.flags) lines.push("Clock format");
      x.zones.forEach((z, k) => {
        const c = device.clockSlots[k + 1];
        if (!c || c.label !== z.label || c.rule !== z.rule) lines.push(`Zone ${k + 1}: ${c?.label || "none"} → ${z.label || "none"}`);
      });
    }
    sections.push({ key: "clock", present: !!x, lines, why: x && !ok ? "This firmware has no clock" : undefined });
  }

  {
    const lines: string[] = [];
    const x = b.haptics;
    let why: string | undefined;
    if (x && !device.hasHaptics) why = "Restoring haptics needs firmware with extensions v14";
    else if (x) {
      const now = await readHaptics();
      x.modes.forEach((m, i) => change(lines, `${MODE_HAPTIC_NAMES[i]} mode`, hapticName(now.modes[i]), hapticName(m)));
      for (const h of x.profiles) {
        const c = now.profiles[h.profile];
        if (!c) continue;
        const diff: string[] = [];
        if (h.feel !== c.feel) diff.push(`feel ${FEEL_NAMES[c.feel]} → ${FEEL_NAMES[h.feel]}`);
        if (h.click !== c.click) diff.push("click");
        const tuned = h.tune.some((t, f) => {
          const n = c.tune[f];
          return t && n && (Math.abs(t.kp - n.kp) > 1e-4 || Math.abs(t.kd - n.kd) > 1e-5 || t.shape !== n.shape || t.amp !== n.amp || Math.abs(t.pitch - n.pitch) > 1e-4);
        });
        if (tuned) diff.push("tuning");
        if (diff.length) lines.push(`${hapticName(h.profile)}: ${diff.join(", ")}`);
      }
    }
    sections.push({ key: "haptics", present: !!x, lines, why });
  }

  const profiles = itemPlans(b.profiles, parseProfile, device.profiles.filter(Boolean), MAX_PROFILES, "16 app profiles");
  const synths = itemPlans(b.synths, parseSynth, device.synths.filter(Boolean), MAX_SYNTHS, "16 synths");
  sections.push({ key: "profiles", present: profiles.length > 0, lines: [] });
  sections.push({ key: "synths", present: synths.length > 0, lines: [], why: synths.length && !device.synths.length ? "This firmware has no synth profiles" : undefined });
  return { backup: b, notes, sections, profiles, synths };
}

// --- restoring ---

export interface Choice {
  sections: globalThis.Set<Section>;
  profiles: globalThis.Set<string>; // ids
  synths: globalThis.Set<string>;
}

// Writes what's chosen and saves it. Profiles and synths first (the settings name them), the
// mode last: a change to or from MIDI reconnects the knob. Returns what didn't go.
export async function applyRestore(plan: Plan, c: Choice, step: (what: string) => void): Promise<string[]> {
  const b = plan.backup, failed: string[] = [];
  const attempt = async (what: string, fn: () => Promise<unknown>) => {
    step(what);
    try {
      await fn();
    } catch (e) {
      failed.push(`${what}: ${e instanceof Error ? e.message : String(e)}`);
    }
  };

  if (c.sections.has("profiles"))
    for (const it of plan.profiles) {
      const d = b.profiles?.find((p) => p.id === it.id);
      if (d && !it.problem && c.profiles.has(it.id)) await attempt(it.name, () => device.uploadProfile(parseProfile(structuredClone(d)), true));
    }
  if (c.sections.has("synths"))
    for (const it of plan.synths) {
      const d = b.synths?.find((x) => x.id === it.id);
      if (d && !it.problem && c.synths.has(it.id)) await attempt(it.name, () => device.putSynth(parseSynth(structuredClone(d)), true));
    }

  const h = b.haptics;
  if (c.sections.has("haptics") && h && device.hasHaptics) {
    for (const p of h.profiles) {
      await attempt(`Haptics: ${hapticName(p.profile)}`, async () => {
        for (const [f, t] of p.tune.entries()) if (t) await device.setHaptic(p.profile, f, { tune: t });
        await device.setHaptic(p.profile, p.feel, { useFeel: p.feel, click: p.click });
      });
    }
    await attempt("Haptics: each mode's", () => device.setHaptic(0, 0, { modes: h.modes }, true));
  }

  const l = b.look;
  if (c.sections.has("look") && l && device.prefs) {
    await attempt("Lights", () => device.setLights(l.lights, true));
    await attempt("Idle word", () => device.setIdleText(l.idleText));
    if (l.cover !== undefined && device.prefs.coverStyles) await attempt("Music cover", () => device.setCoverStyle(l.cover!));
    if (l.screen) await attempt("Screen", () => device.setIdle(l.screen!, true));
  }

  const k = b.clock;
  if (c.sections.has("clock") && k && (device.ext ?? 0) >= EXT_CLOCK_VERSION) {
    await attempt("Clock format", () => device.setClockFlags(k.flags));
    for (const [i, z] of k.zones.entries()) if (i + 1 < CLOCK_SLOTS) await attempt(`Zone ${i + 1}`, () => device.setClockZone(i + 1, z.label, z.rule));
  }

  const x = b.settings;
  if (c.sections.has("settings") && x) {
    await attempt("Settings", async () => {
      await device.set(Set.MIDI_CH, x.midiChannel);
      await device.set(Set.HOST, x.host);
      await device.set(Set.ROTATION, x.rotation);
      await device.set(Set.BOOT, x.boot);
      if (x.profile) {
        await waitFor(() => device.profiles.some((p) => p?.id === x.profile));
        const i = device.profiles.findIndex((p) => p?.id === x.profile);
        if (i >= 0) await device.set(Set.PROFILE, i);
      }
      if (x.synth) {
        const i = device.synths.findIndex((y) => y?.id === x.synth);
        if (i >= 0) await device.set(Set.MIDI_SYNTH, i);
      }
    });
  }
  step("Saving");
  await device.save(); // everything set live above; the mode haptics too
  if (c.sections.has("settings") && x && x.hidType !== device.settings?.hidType) {
    step("Changing the mode");
    await device.set(Set.HID_TYPE, x.hidType);
    await device.save(); // before the knob reconnects (it waits half a second)
  }
  return failed;
}
