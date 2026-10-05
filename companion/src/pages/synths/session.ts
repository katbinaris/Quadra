// One synth being edited, like an app profile (pages/profile/session.ts): read from the knob,
// changed here, sent back live a moment after each change. The top bar's Save keeps it; Revert
// reads it again. Follows the Synths page's route.

import { effect, signal, untracked } from "@preact/signals";
import { DeviceError } from "../../device";
import { problem, type SynthJson } from "../../synth";
import { device, openSynth, route, synthDirty } from "../../store";

const APPLY_MS = 350;

export class SynthSession {
  draft = signal<SynthJson | null>(null);
  rev = signal(0);
  status = signal<{ msg: string; bad: boolean }>({ msg: "", bad: false });
  private timer = 0;
  private sent = 0;
  private edits = 0;

  constructor(readonly id: string) {}

  index() {
    return device.synths.findIndex((x) => x?.id === this.id);
  }

  async load() {
    window.clearTimeout(this.timer);
    this.say("Reading it from the knob…");
    try {
      for (let t = 0; this.index() < 0 && t < 50; t++) await new Promise((r) => window.setTimeout(r, 100));
      const i = this.index();
      if (i < 0) throw new DeviceError("This synth isn't on the knob any more");
      this.draft.value = await device.readSynth(i);
      this.sent = this.edits;
      synthDirty.value = false;
      this.rev.value++;
      this.say("");
    } catch (e) {
      this.say(msg(e), true);
    }
  }

  touch() {
    this.edits++;
    this.rev.value++;
    synthDirty.value = true;
    window.clearTimeout(this.timer);
    this.timer = window.setTimeout(() => void this.apply(), APPLY_MS);
  }

  async apply() {
    window.clearTimeout(this.timer);
    const s = this.draft.value;
    if (!s || this.sent === this.edits) return;
    const bad = problem(s);
    if (bad) return this.say(bad, true);
    const rev = this.edits;
    this.say("Sending…");
    try {
      await device.putSynth(s, false);
      this.sent = rev;
      synthDirty.value = this.sent !== this.edits;
      this.say("Live on the knob");
    } catch (e) {
      this.say(msg(e), true);
    }
  }

  flush() {
    return this.apply();
  }

  discard() {
    window.clearTimeout(this.timer);
    this.sent = this.edits;
    synthDirty.value = false;
  }

  say(m: string, bad = false) {
    this.status.value = { msg: m, bad };
  }
}

function msg(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

export const synthSession = signal<SynthSession | null>(null);

function open(id: string) {
  const cur = synthSession.value;
  if (cur?.id === id) return;
  void cur?.flush();
  const s = new SynthSession(id);
  synthSession.value = s;
  openSynth.value = { id, flush: () => s.flush(), reload: () => s.load(), discard: () => s.discard() };
  void s.load();
}

function close() {
  const cur = synthSession.value;
  if (!cur) return;
  void cur.flush();
  synthSession.value = null;
  openSynth.value = null;
  synthDirty.value = false;
}

effect(() => {
  const r = route.value;
  untracked(() => (r.page === "synths" && r.id ? open(r.id) : close()));
});
