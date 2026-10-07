// The app's shared state, for the views: the knob (one Device), a signal per Device topic so a
// view re-renders only for what it reads, the route (the window's #hash), and SAVE / REVERT
// for everything that isn't stored on the knob yet -- settings, LIGHTS and app profiles.

import { batch, computed, signal, type Signal } from "@preact/signals";
import { Device, TOPICS, type Topic } from "./device";
import { HidType, Op, ProfileFlag, Set, SynthEdit, SynthFlag } from "./proto";
import { createTransport } from "./transport";

export const device = new Device(await createTransport());

// --- topics ---

const ticks = Object.fromEntries(TOPICS.map((t) => [t, signal(0)])) as Record<Topic, Signal<number>>;
// Many reports arrive per frame: bump each topic at most once per animation frame.
const due = new globalThis.Set<Topic>();
let queued = false;
device.subscribe((t) => {
  due.add(t);
  if (queued) return;
  queued = true;
  requestAnimationFrame(() => {
    queued = false;
    batch(() => {
      for (const x of due) ticks[x].value++;
    });
    due.clear();
  });
});

// Call in a component (or a computed) to re-render when any of these change.
export function use(...topics: Topic[]) {
  for (const t of topics) void ticks[t].value;
  return device;
}

export const connected = computed(() => (use("conn", "settings"), device.status === "connected" && device.settings !== null));

// --- route ---

// #/mode, #/haptics, #/lamps[/import], #/synths/<id>/<tab>, #/profile/<id>/<tab>[/<input>],
// #/look/<tab>, #/device/<tab>, #/sys
export type Route =
  | { page: "mode" }
  | { page: "haptics" }
  | { page: "lamps"; sub: "list" | "import" }
  | { page: "synths"; id: string; tab: SynthTab }
  | { page: "profile"; id: string; tab: ProfileTab; input: string }
  | { page: "look"; tab: "lights" | "screen" | "clock" }
  | { page: "device"; tab: "general" | "wifi" }
  | { page: "sys" };
export type ProfileTab = "general" | "keys" | "wheel" | "macros";
export type SynthTab = "params" | "programs" | "monitor";

function parse(hash: string): Route {
  const [page, a, b, c] = hash.replace(/^#\/?/, "").split("/");
  switch (page) {
    case "haptics":
      return { page };
    case "lamps":
      return { page, sub: a === "import" ? "import" : "list" };
    case "synths":
      return { page, id: a ?? "", tab: (["params", "programs", "monitor"].includes(b) ? b : "params") as SynthTab };
    case "profile":
      return { page, id: a ?? "", tab: (["general", "keys", "wheel", "macros"].includes(b) ? b : "general") as ProfileTab, input: c ?? "knob" };
    case "look":
      return { page, tab: a === "screen" || a === "clock" ? a : "lights" };
    case "device":
      return { page, tab: a === "wifi" ? "wifi" : "general" };
    case "sys":
      return { page };
    default:
      return { page: "mode" };
  }
}

export function href(r: Route): string {
  switch (r.page) {
    case "profile":
      return `#/profile/${r.id}/${r.tab}${r.tab === "keys" ? `/${r.input}` : ""}`;
    case "look":
    case "device":
      return `#/${r.page}/${r.tab}`;
    case "lamps":
      return r.sub === "import" ? "#/lamps/import" : "#/lamps";
    case "synths":
      return `#/synths/${r.id}/${r.tab}`;
    default:
      return `#/${r.page}`;
  }
}

export const route = signal<Route>(parse(location.hash));
window.addEventListener("hashchange", () => (route.value = parse(location.hash)));
export function go(r: Route) {
  location.hash = href(r);
}

// --- what isn't saved ---

// Settings bits (Set) by the page they live on.
const SETTING_PAGES: [string, number[]][] = [
  ["Haptics", [Set.DETENTS, Set.KP, Set.KD, Set.FEEL, Set.AMP, Set.PITCH, Set.SHAPE, Set.HAPTIC_PROFILE]],
  ["Mode", [Set.HID_TYPE, Set.MIDI_CH, Set.MIDI_SYNTH, Set.PROFILE, Set.MODE_HAPTIC]],
  ["Device", [Set.BOOT, Set.HOST]],
  ["Screen", [Set.ROTATION]],
];

// The open profile editor, if any: its edits not sent yet, and how to load it again.
export interface OpenEditor {
  id: string;
  flush(): Promise<void>; // send what's typed but not sent yet
  reload(): Promise<void>; // read it from the knob again (after a revert)
  discard(): void; // drop what's waiting to be sent
}
export const openEditor = signal<OpenEditor | null>(null);
export const editorDirty = signal(false); // typed, not sent yet
// The same for the open synth (pages/synths/session.ts).
export const openSynth = signal<OpenEditor | null>(null);
export const synthDirty = signal(false);

export interface Unsaved {
  settings: string[]; // page names: Haptics, Mode, Device, Screen, Lights
  profiles: { index: number; id: string; name: string }[];
  synths: { index: number; id: string; name: string }[];
}

export const unsaved = computed<Unsaved>(() => {
  use("settings", "prefs", "profiles", "synths");
  const dirty = device.settings?.dirty ?? 0;
  const settings = SETTING_PAGES.filter(([, bits]) => bits.some((b) => (dirty >> b) & 1)).map(([name]) => name);
  if (device.prefs?.lightsDirty) settings.push("Lights");
  const profiles = device.profiles.filter((p) => p && p.flags & ProfileFlag.LIVE).map((p) => ({ index: p.index, id: p.id, name: p.name }));
  const ed = openEditor.value;
  if (ed && editorDirty.value && !profiles.some((p) => p.id === ed.id)) {
    const p = device.profiles.find((x) => x?.id === ed.id);
    if (p) profiles.push({ index: p.index, id: p.id, name: p.name });
  }
  const synths = device.synths.filter((x) => x && x.flags & SynthFlag.LIVE).map((x) => ({ index: x.index, id: x.id, name: x.name }));
  const os = openSynth.value;
  if (os && synthDirty.value && !synths.some((x) => x.id === os.id)) {
    const x = device.synths.find((y) => y?.id === os.id);
    if (x) synths.push({ index: x.index, id: x.id, name: x.name });
  }
  return { settings, profiles, synths };
});
export const unsavedCount = computed(() => unsaved.value.settings.length + unsaved.value.profiles.length + unsaved.value.synths.length);

export const saving = signal(false);
export const saveError = signal<string | null>(null);

async function job(fn: () => Promise<void>) {
  saving.value = true;
  saveError.value = null;
  try {
    await fn();
  } catch (e) {
    saveError.value = e instanceof Error ? e.message : String(e);
  } finally {
    saving.value = false;
  }
}

const indexOf = (id: string) => device.profiles.findIndex((p) => p?.id === id);

// Everything on the knob as it is now, kept.
export function saveAll() {
  return job(async () => {
    await openEditor.value?.flush();
    await openSynth.value?.flush();
    const u = unsaved.value;
    if (u.settings.length) await device.save();
    for (const p of u.profiles) {
      const i = indexOf(p.id);
      if (i >= 0) await device.profileOp(i, Op.SAVE);
    }
    for (const x of u.synths) {
      const i = device.synths.findIndex((y) => y?.id === x.id);
      if (i >= 0) await device.synthOp(i, SynthEdit.SAVE);
    }
  });
}

export function revertSynth(id: string) {
  return job(() => revertSynthNow(id));
}
async function revertSynthNow(id: string) {
  const ed = openSynth.value;
  if (ed?.id === id) ed.discard();
  const i = device.synths.findIndex((y) => y?.id === id);
  if (i < 0) return;
  const r = await device.synthOp(i, SynthEdit.REVERT);
  if (ed?.id === id) {
    if (r.removed) go({ page: "synths", id: "", tab: "params" });
    else await ed.reload();
  }
}

// Settings and LIGHTS go back together (the knob reverts them as one).
export function revertSettings() {
  return job(() => device.revert());
}

export function revertProfile(id: string) {
  return job(() => revertProfileNow(id));
}

export function revertAll() {
  return job(async () => {
    const u = unsaved.value;
    if (u.settings.length) await device.revert();
    for (const p of u.profiles) await revertProfileNow(p.id);
    for (const x of u.synths) await revertSynthNow(x.id);
  });
}
async function revertProfileNow(id: string) {
  const ed = openEditor.value;
  if (ed?.id === id) ed.discard();
  const i = indexOf(id);
  if (i < 0) return;
  const r = await device.profileOp(i, Op.REVERT);
  if (ed?.id === id) {
    if (r.removed) go({ page: "mode" });
    else await ed.reload();
  }
}

// --- small helpers the views share ---

export const inUse = computed(() => {
  use("settings");
  const s = device.settings;
  return s && s.hidType === HidType.APP ? s.profile : -1;
});
