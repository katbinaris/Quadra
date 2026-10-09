// One profile being edited: read from the knob, changed here, and sent back live a moment after
// each change (so the knob follows as you go). The top bar's Save keeps it; Revert reads it
// again. One session at a time; leaving the page sends what's still waiting.

import { effect, signal, untracked } from "@preact/signals";
import { DeviceError } from "../../device";
import { HapticProfiles } from "../../proto";
import { problem, type Action, type ProfileJson } from "../../profile";
import { device, editorDirty, openEditor, route } from "../../store";
import { forgetUsage } from "../../usedby";

const APPLY_MS = 350;

export class Session {
  draft = signal<ProfileJson | null>(null);
  rev = signal(0); // bumped on every change: what reads the draft re-renders
  status = signal<{ msg: string; bad: boolean }>({ msg: "", bad: false });
  private timer = 0;
  private sent = 0; // the rev last sent
  private edits = 0; // the rev of the last change

  constructor(readonly id: string) {}

  index() {
    return device.profiles.findIndex((p) => p?.id === this.id);
  }

  async load() {
    window.clearTimeout(this.timer);
    this.say("Reading it from the knob…");
    try {
      // Just connected: the profile list comes one by one.
      for (let t = 0; this.index() < 0 && t < 50; t++) await new Promise((r) => window.setTimeout(r, 100));
      const i = this.index();
      if (i < 0) throw new DeviceError("This profile isn't on the knob any more");
      this.draft.value = await device.readProfile(i);
      this.sent = this.edits;
      editorDirty.value = false;
      this.rev.value++;
      this.say("");
    } catch (e) {
      this.say(msg(e), true);
    }
  }

  // After changing the draft: re-render, and send it a moment later.
  touch() {
    this.edits++;
    this.rev.value++;
    editorDirty.value = true;
    window.clearTimeout(this.timer);
    this.timer = window.setTimeout(() => void this.apply(), APPLY_MS);
  }

  async apply() {
    window.clearTimeout(this.timer);
    const p = this.draft.value;
    if (!p || this.sent === this.edits) return;
    const bad = problem(p);
    if (bad) return this.say(bad, true);
    const rev = this.edits;
    this.say("Sending…");
    try {
      await device.uploadProfile(p, false);
      forgetUsage(this.id);
      this.sent = rev;
      editorDirty.value = this.sent !== this.edits;
      this.say("Live on the knob");
    } catch (e) {
      this.say(msg(e), true);
    }
  }

  flush() {
    return this.apply();
  }

  // Drop what's waiting to be sent (the profile is being reverted or removed).
  discard() {
    window.clearTimeout(this.timer);
    this.sent = this.edits;
    editorDirty.value = false;
  }

  private say(m: string, bad = false) {
    this.status.value = { msg: m, bad };
  }
}

function msg(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

// The session for the profile page, made when a profile opens.
export const session = signal<Session | null>(null);

function openSession(id: string): Session {
  const cur = session.value;
  if (cur?.id === id) return cur;
  void cur?.flush();
  const s = new Session(id);
  session.value = s;
  openEditor.value = { id, flush: () => s.flush(), reload: () => s.load(), discard: () => s.discard() };
  void s.load();
  return s;
}

function closeSession() {
  const cur = session.value;
  if (!cur) return;
  void cur.flush();
  session.value = null;
  openEditor.value = null;
  editorDirty.value = false;
}

// --- an input's haptic profile ---

// What the firmware does with an input's (feel, detents) (menu_haptic_for): VISCOSE -> SMOOTH,
// a step count -> the nearest stepped profile, neither -> the mode's own (-1). The editor only
// ever writes one of those, so an input picks a haptic profile and nothing else.
export const SMOOTH = HapticProfiles.findIndex((h) => h.detents === 0);

export function inputHaptic(a: Action | undefined): number {
  if (!a) return -1;
  if (a.feel === "viscose") return SMOOTH;
  if (!a.detents) return -1;
  let best = 0;
  HapticProfiles.forEach((h, i) => {
    if (h.detents && Math.abs(h.detents - a.detents!) < Math.abs(HapticProfiles[best].detents - a.detents!)) best = i;
  });
  return best;
}

export function setInputHaptic(a: Action, h: number) {
  if (h === SMOOTH) {
    a.feel = "viscose";
    a.detents = undefined;
  } else {
    a.feel = undefined;
    a.detents = h < 0 ? undefined : HapticProfiles[h].detents;
  }
}

// The profile page's session follows the route: a profile opened, another one, or none.
effect(() => {
  const r = route.value;
  untracked(() => (r.page === "profile" ? openSession(r.id) : closeSession()));
});
