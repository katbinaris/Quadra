// One knob, as the UI sees it: connection, the latest settings / state / SYS INFO, the app
// profiles with their icons, and a short history for the charts. Views subscribe and redraw.

import { CLOCK_SLOTS, Cmd, EXT_CLOCK_VERSION, EXT_IDLE_VERSION, EXT_HAPTICS_VERSION, EXT_CONTROLS_VERSION, EXT_HOME_VERSION, EXT_SYNTH_VERSION, EXT_WIFI_LINK_VERSION, EXT_NET_VERSION, ExtCmd, ExtStatus, ExtTag, NetOp, Res, RES_TEXT, SYNTH_CHUNK, SYNTH_PUT_CHUNK, SYNTH_RESULT, SynthOp, TEXT_CHUNK, Tag, crc32, decode, encode, ICON_BYTES, ICON_CHUNK, UploadFlag, type Hello, type ClockSlot, type HapticEntry, type HapticSet, type Idle, type ScreenPrefs, type Lamp, type Lights, type MidiStatus, type Net, type Prefs, type Profile, type Result, type SetId, type Settings, type State, type SynthEntry, type SysA, type SysB } from "./proto";
import { tidySynth, type SynthJson } from "./synth";
import { rgb565ToImage, tidy, type ProfileJson } from "./profile";
import { loadPairing, savePairing, type Pairing, type Transport } from "./transport";

export type Status = "searching" | "needs-permission" | "connected" | "unsupported";

// What changed, so a view redraws only for what it shows: "state" arrives 30 times a second
// while streaming, the rest when something really changes.
export const TOPICS = ["conn", "settings", "state", "profiles", "sys", "prefs", "idle", "net", "clock", "lamps", "synths", "midi"] as const;
export type Topic = (typeof TOPICS)[number];

export interface ProfileEntry extends Profile {
  icon: ImageData | null; // 48x48, decoded from the device's RGB565
}

export const HISTORY_SECONDS = 60;
export interface History {
  totalMa: number[];
  chipC: number[];
  load0: number[];
  load1: number[];
}

const STREAM_HZ = 30;
const SEARCH_MS = 1000;
const TRANSFER_MS = 8000;
const PREFS_MS = 1000; // settings, LIGHTS and the idle word can change on the knob too (its menu)
const LIGHTS_RETRY_MS = 60; // the knob takes one LIGHTS at a time
const LIST_RETRY_MS = 2000; // a step of the profile list unanswered this long is asked again
// An icon's chunks asked for at once. One at a time, a list of eight took ~25 s over WiFi (672
// round trips); six in flight, ~3 s. The knob holds 16 replies, and the poll asks up to 8 at once.
const ICON_WINDOW = 6;
const SYNTH_WINDOW = 6; // a synth's JSON pieces asked for at once (READ), like an icon's

// What the pages watch, polled with the settings every second: HOME's lamps, the midi task.
export type Watch = "lamps" | "midi";

// A HOME lamp to import (LampImport.tsx): what EXT_HOME_LAMP carries.
export interface ImportLamp {
  did: number;
  ip: string;
  token: Uint8Array;
  proto: number;
  caps: number;
  ctMin: number;
  ctMax: number;
  siid: number[];
  piid: number[];
  name: string;
  kind: number;
}

export class DeviceError extends Error {}

export class Device {
  status: Status = "searching";
  hello: Hello | null = null;
  settings: Settings | null = null;
  state: State | null = null;
  sysA: SysA | null = null;
  sysB: SysB | null = null;
  profiles: ProfileEntry[] = [];
  history: History = { totalMa: [], chipC: [], load0: [], load1: [] };
  // ext_proto.h: its version (0 = firmware without the extensions, null = not known yet), and
  // LIGHTS + the idle word.
  ext: number | null = null;
  prefs: Prefs | null = null;
  idle: Idle | null = null; // the screen: brightness, screensaver, sleep hours (extensions v13)
  net: Net | null = null; // WiFi, from extensions v4
  paired: Pairing | null = loadPairing(); // this app over WiFi, from extensions v7
  // The knob's controls over WiFi (extensions v10): this app types and scrolls for it. null: not
  // here (USB, WebHID, older firmware); "needs-permission": macOS Accessibility isn't allowed yet.
  controls: "on" | "needs-permission" | null = null;
  private keyAsked = false; // every HID client sees the knob's key reply: only take one asked for
  clockSlots: (ClockSlot | null)[] = []; // the CLOCK app, from extensions v5: slot 0 has the format too
  // HOME's lamps as the knob sees them (extensions v11), while a page watches them.
  lamps: Lamp[] = [];
  lampCount: number | null = null; // null: not asked yet
  // MIDI's synth profiles (extensions v12; empty before) and the midi task now.
  synths: SynthEntry[] = [];
  midi: MidiStatus | null = null;
  error: string | null = null;
  private watching = new globalThis.Set<Watch>();
  private waitAck: { cmd: number; done: (status: number) => void } | null = null;
  private waitHaptic: { profile: number; feel: number; done: (h: HapticEntry) => void; fail: (e: Error) => void } | null = null;
  private waitSynth: { what: number; done: (r: { res: number; index: number; removed: boolean; why: string }) => void } | null = null;
  private synthRead: { index: number; buf: Uint8Array | null; crc: number; have: globalThis.Set<number>; sent: number; at: number; done: (t: string) => void; fail: (e: Error) => void; tries: number } | null = null;

  private listeners = new Set<(t: Topic) => void>();
  private streamWanted = false; // the live stream (STATE, SYS, LEDs): only while a view needs it
  // The icon being fetched: the offsets in, and how far the requests have gone.
  private icon: { index: number; buf: Uint8Array; have: globalThis.Set<number>; sent: number; at: number } | null = null;
  // On a list reload, icons are fetched again only for these (all when null).
  private staleIcons: globalThis.Set<number> | null = null;
  private searchTimer: number | undefined;
  // Profile transfers run one at a time; each waits for its own reply.
  private chain: Promise<unknown> = Promise.resolve();
  private download: { index: number; buf: Uint8Array | null; crc: number; got: number; done: (t: string) => void; fail: (e: Error) => void } | null = null;
  private waitResult: { cmd: number; done: (r: Result) => void } | null = null;
  private prefsTimer: number | undefined;
  // The profile list comes a request at a time (each reply asks for the next); `want` is the one
  // outstanding. A reply that isn't it (a repeat) is ignored, and one that never comes is asked
  // again, so a lost report can't stall the list.
  private want: { r: Uint8Array; key: string; at: number } | null = null;
  private listTimer: number | undefined;
  private listReload: number | undefined;
  private lightsSent: { l: Lights; save: boolean; retried: boolean } | null = null;
  private idleSent: { s: Partial<ScreenPrefs>; save: boolean; retried: boolean } | null = null;

  constructor(private transport: Transport | null) {
    if (!transport) {
      this.status = "unsupported";
      return;
    }
    transport.onReport = (r) => this.onReport(r);
    transport.onClosed = () => this.onClosed();
    this.search();
  }

  get kind() {
    return this.transport?.kind ?? null;
  }

  subscribe(fn: (t: Topic) => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  // No topic: everything (a connect or a disconnect).
  private changed(...topics: Topic[]) {
    for (const t of topics.length ? topics : TOPICS) for (const fn of this.listeners) fn(t);
  }

  // The live stream: STATE at 30 Hz, SYS twice a second and the LEDs (host_link.c). The knob
  // works for every report it sends, so it runs only while a view shows some of it. (The LEDs
  // and the knob's own screen, EXT_CMD_SCREEN, aren't shown any more: no live preview.)
  setStreaming(on: boolean) {
    if (on === this.streamWanted) return;
    this.streamWanted = on;
    if (this.status === "connected") void this.send(encode.stream(on ? STREAM_HZ : 0));
  }

  // --- connection ---

  private search() {
    window.clearTimeout(this.searchTimer);
    if (!this.transport || this.status === "connected") return;
    this.transport
      .tryConnect()
      .then((ok) => {
        if (ok) return this.start();
        if (this.transport!.needsGesture && this.status !== "needs-permission") {
          this.status = "needs-permission";
          this.changed("conn");
        }
        this.searchTimer = window.setTimeout(() => this.search(), SEARCH_MS);
      })
      .catch((e) => {
        this.error = String(e);
        this.searchTimer = window.setTimeout(() => this.search(), SEARCH_MS);
      });
  }

  // WebHID: the device picker (from a click).
  async requestAccess() {
    if (this.transport?.request && (await this.transport.request())) await this.start();
  }

  private async start() {
    this.status = "connected";
    this.error = null;
    this.controls = null;
    this.profiles = [];
    this.staleIcons = null;
    this.icon = null;
    this.want = null;
    window.clearInterval(this.listTimer);
    this.listTimer = window.setInterval(() => {
      const w = this.want, ic = this.icon, rd = this.synthRead;
      if (w && Date.now() - w.at > LIST_RETRY_MS) this.ask(w.r, w.key);
      // A synth's lost pieces, asked again (or the start, if even that got lost).
      if (rd && Date.now() - rd.at > LIST_RETRY_MS) {
        rd.at = Date.now();
        if (!rd.buf) void this.send(encode.synthRead(rd.index, 0));
        else for (let o = 0; o < rd.sent; o += SYNTH_CHUNK) if (!rd.have.has(o)) void this.send(encode.synthRead(rd.index, o));
      }
      if (ic && Date.now() - ic.at > LIST_RETRY_MS) {
        ic.at = Date.now();
        for (let o = 0; o < ic.sent; o += ICON_CHUNK) if (!ic.have.has(o)) void this.send(encode.profileIcon(ic.index, o));
      }
    }, LIST_RETRY_MS / 2);
    this.changed();
    this.ask(encode.hello(), "hello");
    await this.send(encode.getSettings());
    if (this.streamWanted) await this.send(encode.stream(STREAM_HZ));
    await this.send(encode.extHello());
  }

  // A page shows the lamps or the MIDI status: they're asked for every second while it does.
  watch(what: Watch, on: boolean) {
    if (on) this.watching.add(what);
    else this.watching.delete(what);
  }

  private onClosed() {
    this.lamps = [];
    this.lampCount = null;
    this.synths = [];
    this.midi = null;
    this.synthRead = null;
    this.status = "searching";
    this.hello = this.settings = this.state = this.sysA = this.sysB = null;
    this.ext = this.prefs = this.idle = this.net = null;
    this.controls = null;
    this.clockSlots = [];
    window.clearInterval(this.prefsTimer);
    window.clearInterval(this.listTimer);
    window.clearTimeout(this.listReload);
    this.want = null;
    this.icon = null;
    this.changed();
    this.search();
  }

  private async send(r: Uint8Array) {
    try {
      await this.transport?.send(r);
    } catch (e) {
      this.error = String(e);
      this.changed("conn");
    }
  }

  // --- commands ---

  set(id: SetId, value: number) {
    return this.send(encode.set(id, value));
  }
  // SAVE and REVERT cover LIGHTS and the screen too (menu_remote_save / _revert).
  async save() {
    await this.send(encode.save());
    await this.askPrefs();
  }
  async revert() {
    await this.send(encode.revert());
    await this.askPrefs();
  }
  private async askPrefs() {
    if (this.ext) await this.send(encode.extPrefs());
    if ((this.ext ?? 0) >= EXT_IDLE_VERSION) await this.send(encode.idle({}));
  }
  setIdleText(text: string) {
    return this.send(encode.idleText(text));
  }
  setCoverStyle(style: number) {
    return this.send(encode.coverStyle(style));
  }
  // WiFi: a new network (`ssid` + `password`, "" = open), or on / off with the stored one.
  async setWifi(on: boolean, ssid?: string, password?: string) {
    if (ssid !== undefined) {
      const pw = new Uint8Array(64);
      pw.set(new TextEncoder().encode(password ?? "").subarray(0, 63));
      await this.send(encode.net(NetOp.SSID, new TextEncoder().encode(ssid)));
      await this.send(encode.net(NetOp.PASS_A, pw.subarray(0, 32)));
      await this.send(encode.net(NetOp.PASS_B, pw.subarray(32, 64)));
    }
    const r = encode.net(NetOp.APPLY);
    r[2] = on ? 1 : 0;
    await this.send(r);
  }
  // Over USB: the knob's WiFi key (made on first use), kept here so this app reaches the knob over
  // WiFi when no cable is in. `fresh`: a new key -- every other paired companion pairs again.
  pairWifi(fresh = false) {
    if ((this.ext ?? 0) < EXT_WIFI_LINK_VERSION || this.kind !== "tauri") return Promise.resolve();
    this.keyAsked = true;
    return this.send(encode.netKey(fresh));
  }
  // Over WiFi: once macOS allows it (Accessibility), the knob's controls come here. Asked again
  // every second (the poll) until they do -- allowing it in System Settings needs no restart.
  private async checkControls(prompt = false) {
    if (this.kind !== "wifi" || (this.ext ?? 0) < EXT_CONTROLS_VERSION || (this.controls === "on" && !prompt)) return;
    const { invoke } = await import("@tauri-apps/api/core");
    const ok = await invoke<boolean>("input_trusted", { prompt });
    const was = this.controls;
    if (ok) await this.send(encode.netControls(true));
    this.controls = ok ? "on" : "needs-permission";
    if (this.controls !== was) this.changed("net");
  }
  // From a click, over USB too (allowed before the cable comes out): macOS asks for
  // Accessibility, and lists the app in System Settings.
  async allowControls() {
    if (this.kind === "wifi") return this.checkControls(true);
    const { invoke } = await import("@tauri-apps/api/core");
    await invoke<boolean>("input_trusted", { prompt: true });
  }
  forgetWifi() {
    savePairing((this.paired = null));
    this.changed("net");
  }
  // The CLOCK app: its format (ClockFlag), and zone `slot` (1-4; label "" = none). Both stored at once.
  setClockFlags(flags: number) {
    return this.send(encode.clockFormat(flags));
  }
  setClockZone(slot: number, label: string, rule: string) {
    return this.send(encode.clockZone(slot, label, rule));
  }
  // The screen (only the fields given): live at once, `save` stores it too.
  setIdle(s: Partial<ScreenPrefs>, save = false) {
    if ((this.ext ?? 0) < EXT_IDLE_VERSION) return Promise.resolve();
    this.idleSent = { s: { ...s }, save, retried: false };
    return this.send(encode.idle(s, save));
  }
  setLights(l: Lights, save = false) {
    this.lightsSent = { l: { ...l }, save, retried: false };
    return this.send(encode.lights(l, save));
  }
  // The shown haptic profile back to its factory feel and values (live, not saved).
  resetHaptic() {
    return this.send(encode.hapticReset());
  }
  resetPeaks() {
    return this.send(encode.resetPeaks());
  }

  // --- every haptic profile's tuning (extensions v14), for backups ---

  get hasHaptics() {
    return (this.ext ?? 0) >= EXT_HAPTICS_VERSION;
  }

  private haptic(r: Uint8Array, profile: number, feel: number, what: string): Promise<HapticEntry> {
    return this.serial(() =>
      this.timeout(
        new Promise<HapticEntry>((done, fail) => {
          this.waitHaptic = { profile, feel, done, fail };
          void this.send(r);
        }),
        what,
      ),
    );
  }
  getHaptic(profile: number, feel: number) {
    return this.haptic(encode.hapticsGet(profile, feel), profile, feel, "Reading the haptics");
  }
  // Live at once; `save` stores the haptic profiles too.
  setHaptic(profile: number, feel: number, h: HapticSet, save = false) {
    return this.haptic(encode.hapticsSet(profile, feel, h, save), profile, feel, "Setting the haptics");
  }

  // --- profiles (JSON, profile.ts) ---

  private serial<T>(job: () => Promise<T>): Promise<T> {
    const run = this.chain.then(job, job);
    this.chain = run.catch(() => undefined);
    return run;
  }

  private timeout<T>(p: Promise<T>, what: string, ms = TRANSFER_MS): Promise<T> {
    return new Promise<T>((resolve, reject) => {
      const t = window.setTimeout(() => {
        this.download = null;
        this.waitResult = null;
        this.waitAck = null;
        this.waitSynth = null;
        this.waitHaptic = null;
        reject(new DeviceError(`${what}: no answer from the knob`));
      }, ms);
      p.then(
        (v) => (window.clearTimeout(t), resolve(v)),
        (e) => (window.clearTimeout(t), reject(e)),
      );
    });
  }

  // The whole profile at `index`, as the device has it right now (live edit included).
  readProfile(index: number): Promise<ProfileJson> {
    return this.serial(() =>
      this.timeout(
        new Promise<string>((done, fail) => {
          this.download = { index, buf: null, crc: 0, got: 0, done, fail };
          this.send(encode.profileRead(index));
        }),
        "Reading the profile",
      ).then((text) => JSON.parse(text) as ProfileJson),
    );
  }

  private result(cmd: number, send: () => Promise<void>, what: string): Promise<Result> {
    return this.timeout(
      new Promise<Result>((done) => {
        this.waitResult = { cmd, done };
        void send();
      }),
      what,
    ).then((r) => {
      if (r.res !== Res.OK) throw new DeviceError(r.why ? `${RES_TEXT[r.res] ?? "Failed"}: ${r.why}` : (RES_TEXT[r.res] ?? "Failed"));
      this.reloadProfiles(r.removed ? undefined : r.index);
      return r;
    });
  }

  // Live on the knob right away; `save` stores it too.
  uploadProfile(p: ProfileJson, save: boolean): Promise<Result> {
    return this.serial(() => {
      const bytes = new TextEncoder().encode(JSON.stringify(tidy(p)));
      return this.result(
        Cmd.UPLOAD_END,
        async () => {
          await this.send(encode.uploadBegin(bytes.length, crc32(bytes), save ? UploadFlag.SAVE : 0));
          for (let off = 0; off < bytes.length; off += TEXT_CHUNK) await this.send(encode.uploadData(off, bytes.subarray(off, off + TEXT_CHUNK)));
          await this.send(encode.uploadEnd());
        },
        "Sending the profile",
      );
    });
  }

  profileOp(index: number, op: number): Promise<Result> {
    return this.serial(() => this.result(Cmd.PROFILE_OP, () => this.send(encode.profileOp(index, op)), "The profile"));
  }

  // The list again (names, flags), after a change -- icons only for `changed` (all if not given:
  // a removal moves the ones after it).
  reloadProfiles(changed?: number) {
    this.icon = null;
    this.staleIcons = changed === undefined ? null : new globalThis.Set([changed]);
    this.ask(encode.hello(), "hello");
  }

  // --- replies ---

  private onReport(r: Uint8Array) {
    const m = decode(r);
    let topic: Topic = "conn";
    switch (m.tag) {
      case Tag.HELLO:
        if ("hello" in m) {
          this.hello = m.hello;
          this.profiles.length = Math.min(this.profiles.length, m.hello.profileCount);
          // Profiles one by one; each reply asks for the next (and its icon).
          if (m.hello.profileCount > 0) this.ask(encode.profile(0), "p0");
          else this.want = null;
        }
        this.changed("profiles");
        break;
      case Tag.SETTINGS:
        if ("settings" in m) this.settings = m.settings;
        topic = "settings";
        break;
      case Tag.PROFILE:
        if ("profile" in m) this.onProfile(m.profile);
        return; // changed() once its icon has arrived
      case Tag.PROFILE_ICON:
        if ("chunk" in m) this.onIconChunk(m.chunk.index, m.chunk.offset, m.chunk.bytes);
        return;
      case Tag.STATE:
        if ("state" in m) this.state = m.state;
        topic = "state";
        break;
      case Tag.SYS_A:
        if ("sys" in m) {
          this.sysA = m.sys as SysA;
          this.push("totalMa", this.sysA.totalMa);
          this.push("chipC", this.sysA.chipC);
        }
        topic = "sys";
        break;
      case Tag.SYS_B:
        if ("sys" in m) {
          this.sysB = m.sys as SysB;
          this.push("load0", this.sysB.load[0]);
          this.push("load1", this.sysB.load[1]);
        }
        topic = "sys";
        break;
      case Tag.PROFILE_BEGIN:
        if ("length" in m && this.download && m.index === this.download.index) {
          this.download.buf = new Uint8Array(m.length);
          this.download.crc = m.crc;
          this.download.got = 0;
          if (m.length === 0) this.finishDownload();
        }
        return;
      case Tag.PROFILE_DATA:
        if ("offset" in m && this.download?.buf) {
          const d = this.download, buf = d.buf!;
          if (m.offset !== d.got) {
            this.download = null;
            d.fail(new DeviceError("The profile arrived out of order"));
            return;
          }
          const n = Math.min(TEXT_CHUNK, buf.length - m.offset);
          buf.set(m.bytes.subarray(0, n), m.offset);
          d.got += n;
          if (d.got >= buf.length) this.finishDownload();
        }
        return;
      case Tag.RESULT:
        if ("result" in m && this.waitResult && this.waitResult.cmd === m.result.cmd) {
          const w = this.waitResult;
          this.waitResult = null;
          w.done(m.result);
        }
        return;
      case ExtTag.HELLO:
        if ("ext" in m) {
          this.ext = m.ext;
          window.clearInterval(this.prefsTimer);
          let tick = 0;
          const poll = () => {
            // Settings too: a mode or profile picked on the knob shows here within a second
            void this.send(encode.getSettings());
            void this.send(encode.extPrefs());
            if ((this.ext ?? 0) >= EXT_IDLE_VERSION) void this.send(encode.idle({}));
            // CLOCK: the format every second, the zones every 5 (the CLI can change them too)
            if ((this.ext ?? 0) >= EXT_CLOCK_VERSION && tick++ % 5 === 4) for (let s = 1; s < CLOCK_SLOTS; s++) void this.send(encode.clockGet(s));
            if ((this.ext ?? 0) >= EXT_NET_VERSION) void this.send(encode.net(NetOp.STATUS));
            if ((this.ext ?? 0) >= EXT_CLOCK_VERSION) void this.send(encode.clockGet(0));
            if ((this.ext ?? 0) >= EXT_HOME_VERSION && this.watching.has("lamps")) this.pollLamps();
            if ((this.ext ?? 0) >= EXT_SYNTH_VERSION && this.watching.has("midi")) void this.send(encode.synthStatus());
            void this.checkControls();
          };
          this.prefsTimer = window.setInterval(poll, PREFS_MS);
          poll();
          if (m.ext >= EXT_CLOCK_VERSION) for (let s = 1; s < CLOCK_SLOTS; s++) void this.send(encode.clockGet(s));
          if (m.ext >= EXT_SYNTH_VERSION) this.reloadSynths();
        }
        break;
      case ExtTag.HOME:
        if ("lamp" in m) {
          const l = m.lamp;
          this.lampCount = l.count;
          this.lamps.length = Math.min(this.lamps.length, l.count);
          if (l.slot < l.count) this.lamps[l.slot] = l;
        }
        topic = "lamps";
        break;
      case ExtTag.SYNTH:
        if ("synth" in m) {
          const e = m.synth;
          this.synths.length = Math.min(this.synths.length, e.count);
          if (e.index < e.count) {
            this.synths[e.index] = e;
            if (e.index + 1 < e.count) void this.send(encode.synthList(e.index + 1));
          }
          topic = "synths";
        } else if ("status" in m) {
          this.midi = m.status;
          topic = "midi";
        } else if ("what" in m) {
          const w = this.waitSynth;
          if (w && w.what === m.what) {
            this.waitSynth = null;
            w.done(m);
          }
          return;
        } else if ("offset" in m) {
          this.onSynthPiece(m.index, m.length, m.crc, m.offset, m.bytes);
          return;
        }
        break;
      case ExtTag.HAPTICS:
        if ("haptic" in m) {
          const w = this.waitHaptic;
          if (w && w.profile === m.haptic.profile && w.feel === m.haptic.feel) {
            this.waitHaptic = null;
            w.done(m.haptic);
          }
        }
        return;
      case ExtTag.PREFS:
        if ("prefs" in m) this.prefs = m.prefs;
        topic = "prefs";
        break;
      case ExtTag.IDLE:
        if ("idle" in m) this.idle = m.idle;
        topic = "idle";
        break;
      case ExtTag.NET:
        if ("net" in m) {
          this.net = m.net;
          // The paired knob got a new address (DHCP): the fallback follows it.
          const p = this.paired;
          if (p && m.net.host === p.host && m.net.ip && m.net.ip !== p.ip) savePairing((this.paired = { ...p, ip: m.net.ip }));
        }
        topic = "net";
        break;
      case ExtTag.KEY:
        if ("key" in m && this.keyAsked) {
          this.keyAsked = false;
          const hex = Array.from(m.key, (x) => x.toString(16).padStart(2, "0")).join("");
          savePairing((this.paired = { host: this.net?.host ?? "", ip: this.net?.ip ?? "", port: m.port, key: hex }));
        }
        topic = "net";
        break;
      case ExtTag.CLOCK:
        if ("clock" in m && m.clock.slot < CLOCK_SLOTS) this.clockSlots[m.clock.slot] = m.clock;
        topic = "clock";
        break;
      case ExtTag.ACK:
        if ("status" in m) {
          const wh = this.waitHaptic;
          if (wh && m.cmd === ExtCmd.HAPTICS) {
            this.waitHaptic = null;
            wh.fail(new DeviceError("The knob refused those haptic values"));
            return;
          }
          const wa = this.waitAck;
          if (wa && wa.cmd === m.cmd) {
            this.waitAck = null;
            wa.done(m.status);
            return;
          }
          if (m.cmd === ExtCmd.SYNTH && this.synthRead) {
            // A piece asked for after the knob's copy changed (an edit elsewhere): start over.
            const rd = this.synthRead;
            if (rd.tries++ < 3) this.restartSynthRead();
            else {
              this.synthRead = null;
              rd.fail(new DeviceError("The synth changed while it was being read"));
            }
            return;
          }
          const sent = this.lightsSent;
          if (m.cmd === ExtCmd.LIGHTS && m.status === ExtStatus.BAD_PARAM && sent && !sent.retried) {
            sent.retried = true; // the knob was still applying the one before
            window.setTimeout(() => void this.send(encode.lights(sent.l, sent.save)), LIGHTS_RETRY_MS);
            return;
          }
          const isent = this.idleSent;
          if (m.cmd === ExtCmd.IDLE && m.status === ExtStatus.BAD_PARAM && isent && !isent.retried) {
            isent.retried = true; // likewise
            window.setTimeout(() => void this.send(encode.idle(isent.s, isent.save)), LIGHTS_RETRY_MS);
            return;
          }
          if (m.status !== ExtStatus.OK)
            this.error =
              m.status === ExtStatus.STORAGE ? "The knob couldn't store that"
              : m.status === ExtStatus.USB_ONLY ? "Only over USB"
              : `the knob refused 0x${m.cmd.toString(16)} (${m.status})`;
          else if (m.cmd === ExtCmd.TEXT) void this.send(encode.extPrefs());
        }
        break;
      case Tag.ERROR:
        if ("cmd" in m) {
          if (m.cmd >= ExtCmd.HELLO && m.cmd <= 0x2f) {
            this.ext = 0; // stock firmware: no extensions
            break;
          }
          if (m.cmd === Cmd.PROFILE || m.cmd === Cmd.PROFILE_ICON) {
            // The list changed under it (another client): start it over, after a pause so an
            // error that stays doesn't spin.
            this.want = null;
            this.icon = null;
            window.clearTimeout(this.listReload);
            this.listReload = window.setTimeout(() => this.status === "connected" && this.reloadProfiles(), LIST_RETRY_MS);
            break;
          }
          if (m.cmd === Cmd.PROFILE_READ && this.download) {
            const d = this.download;
            this.download = null;
            d.fail(new DeviceError("The knob couldn't read that profile"));
            return;
          }
          this.error = `device refused command 0x${m.cmd.toString(16)} (${m.code})`;
        }
        break;
      default:
        return;
    }
    this.changed(topic);
  }

  private finishDownload() {
    const d = this.download!;
    this.download = null;
    if (crc32(d.buf!) !== d.crc) d.fail(new DeviceError("The profile was damaged on the way (CRC)"));
    else d.done(new TextDecoder().decode(d.buf!));
  }

  // --- HOME's lamps (extensions v11 / v12) ---

  // Every stored lamp's state: slot 0 tells how many there are.
  private pollLamps() {
    const n = this.lampCount ?? 1;
    for (let s = 0; s < Math.max(1, n); s++) void this.send(encode.homeStatus(s));
  }

  private ack(r: Uint8Array, cmd: number, what: string, ms = TRANSFER_MS): Promise<void> {
    return this.timeout(
      new Promise<number>((done) => {
        this.waitAck = { cmd, done };
        void this.send(r);
      }),
      what,
      ms,
    ).then((st) => {
      if (st === ExtStatus.OK) return;
      throw new DeviceError(st === ExtStatus.STORAGE ? `${what}: the knob couldn't store it` : st === ExtStatus.USB_ONLY ? `${what}: only over USB` : `${what}: the knob refused it`);
    });
  }

  // A rename, an icon, a move or a removal (HomeEdit); stored on the knob at once.
  homeEdit(slot: number, did: number, what: number, value: number | string): Promise<void> {
    return this.serial(async () => {
      await this.ack(encode.homeEdit(slot, did, what, value), ExtCmd.HOME, "The lamp");
      this.pollLamps();
    });
  }

  // A new list of lamps, tokens and all: USB only.
  homeImport(lamps: ImportLamp[]): Promise<void> {
    return this.serial(async () => {
      await this.ack(encode.homeBegin(), ExtCmd.HOME, "Starting the import");
      for (const [i, l] of lamps.entries()) await this.ack(encode.homeLamp(i, l), ExtCmd.HOME, `Lamp ${i + 1}`);
      await this.ack(encode.homeCommit(lamps.length), ExtCmd.HOME, "Storing the lamps", 10000);
      this.lampCount = lamps.length;
      this.lamps = [];
      this.pollLamps();
    });
  }

  // --- MIDI's synth profiles (extensions v12) ---

  reloadSynths() {
    if ((this.ext ?? 0) >= EXT_SYNTH_VERSION) void this.send(encode.synthList(0));
  }

  // The synth at `index` as the knob has it now (a live edit included).
  readSynth(index: number): Promise<SynthJson> {
    return this.serial(() =>
      this.timeout(
        new Promise<string>((done, fail) => {
          this.synthRead = { index, buf: null, crc: 0, have: new globalThis.Set(), sent: 0, at: Date.now(), done, fail, tries: 0 };
          this.restartSynthRead();
        }),
        "Reading the synth",
      ).then((t) => JSON.parse(t) as SynthJson),
    ).finally(() => (this.synthRead = null));
  }

  private restartSynthRead() {
    const rd = this.synthRead!;
    rd.buf = null;
    rd.have.clear();
    rd.sent = 0;
    rd.at = Date.now();
    void this.send(encode.synthRead(rd.index, 0)); // offset 0 alone: the knob writes the JSON then
  }

  private onSynthPiece(index: number, length: number, crc: number, offset: number, bytes: Uint8Array) {
    const rd = this.synthRead;
    if (!rd || rd.index !== index) return;
    if (length === 0) {
      this.synthRead = null;
      rd.fail(new DeviceError("There's no such synth on the knob"));
      return;
    }
    if (!rd.buf || rd.crc !== crc || rd.buf.length !== length) {
      if (offset !== 0) return; // from an older copy
      rd.buf = new Uint8Array(length);
      rd.crc = crc;
      rd.have.clear();
      rd.sent = SYNTH_CHUNK;
    }
    if (offset % SYNTH_CHUNK || rd.have.has(offset)) return;
    rd.buf.set(bytes.subarray(0, Math.min(SYNTH_CHUNK, length - offset)), offset);
    rd.have.add(offset);
    rd.at = Date.now();
    if (rd.have.size >= Math.ceil(length / SYNTH_CHUNK)) {
      this.synthRead = null;
      if (crc32(rd.buf) !== crc) rd.fail(new DeviceError("The synth was damaged on the way (CRC)"));
      else rd.done(new TextDecoder().decode(rd.buf));
      return;
    }
    while (rd.sent < length && rd.sent / SYNTH_CHUNK - (rd.have.size - 1) < SYNTH_WINDOW) {
      void this.send(encode.synthRead(index, rd.sent));
      rd.sent += SYNTH_CHUNK;
    }
  }

  private synthResult(what: number, send: () => Promise<void>, label: string) {
    return this.timeout(
      new Promise<{ res: number; index: number; removed: boolean; why: string }>((done) => {
        this.waitSynth = { what, done };
        void send();
      }),
      label,
    ).then((r) => {
      this.reloadSynths();
      if (r.res !== 0) throw new DeviceError(r.why ? `${SYNTH_RESULT[r.res] ?? "Failed"}: ${r.why}` : (SYNTH_RESULT[r.res] ?? "Failed"));
      return r;
    });
  }

  // Live on the knob at once (the one with its id is replaced, else it's added); `save` stores it.
  putSynth(s: SynthJson, save: boolean) {
    return this.serial(() => {
      const bytes = new TextEncoder().encode(JSON.stringify(tidySynth(s)));
      return this.synthResult(
        SynthOp.PUT_END,
        async () => {
          await this.send(encode.synthPutBegin(bytes.length, crc32(bytes), save));
          for (let off = 0; off < bytes.length; off += SYNTH_PUT_CHUNK) await this.send(encode.synthPutData(off, bytes.subarray(off, off + SYNTH_PUT_CHUNK)));
          await this.send(encode.synthPutEnd());
        },
        "Sending the synth",
      );
    });
  }

  // SynthEdit: SAVE, REVERT (to what's stored, or built in) or REMOVE.
  synthOp(index: number, op: number) {
    return this.serial(() => this.synthResult(SynthOp.OP, () => this.send(encode.synthOp(index, op)), "The synth"));
  }

  synthGoto(param: number) {
    return this.send(encode.synthGoto(param));
  }

  // SYS arrives twice a second: HISTORY_SECONDS worth of samples.
  private push(key: keyof History, v: number) {
    const a = this.history[key];
    a.push(v);
    if (a.length > HISTORY_SECONDS * 2) a.shift();
  }

  private ask(r: Uint8Array, key: string) {
    this.want = { r, key, at: Date.now() };
    void this.send(r);
  }

  private onProfile(p: Profile) {
    if (this.want?.key !== `p${p.index}`) return;
    const old = this.profiles[p.index];
    const same = !!old && old.id === p.id;
    // Keep the old icon on screen until the new one is in (no flicker on a reload).
    this.profiles[p.index] = { ...p, icon: same ? old.icon : null };
    const fresh = same && (old.icon !== null) === p.hasIcon && this.staleIcons !== null && !this.staleIcons.has(p.index);
    if (p.hasIcon && !fresh) {
      this.want = null; // the icon's own requests now (the list timer asks again for lost ones)
      this.icon = { index: p.index, buf: new Uint8Array(ICON_BYTES), have: new globalThis.Set(), sent: 0, at: Date.now() };
      this.pumpIcon();
    } else {
      this.nextProfile(p.index);
    }
    this.changed("profiles");
  }

  // Up to ICON_WINDOW of the icon's chunks asked for and not in yet; each one in asks for the next.
  private pumpIcon() {
    const ic = this.icon!;
    while (ic.sent < ICON_BYTES && ic.sent / ICON_CHUNK - ic.have.size < ICON_WINDOW) {
      void this.send(encode.profileIcon(ic.index, ic.sent));
      ic.sent += ICON_CHUNK;
    }
  }

  private onIconChunk(index: number, offset: number, bytes: Uint8Array) {
    const ic = this.icon;
    if (!ic || ic.index !== index || offset % ICON_CHUNK || offset >= ic.sent || ic.have.has(offset)) return;
    ic.buf.set(bytes.subarray(0, Math.min(ICON_CHUNK, ICON_BYTES - offset)), offset);
    ic.have.add(offset);
    ic.at = Date.now();
    if (ic.have.size < Math.ceil(ICON_BYTES / ICON_CHUNK)) {
      this.pumpIcon();
      return;
    }
    this.icon = null;
    if (this.profiles[index]) this.profiles[index].icon = rgb565ToImage(ic.buf, 48);
    this.nextProfile(index);
    this.changed("profiles");
  }

  private nextProfile(index: number) {
    if (this.hello && index + 1 < this.hello.profileCount) this.ask(encode.profile(index + 1), `p${index + 1}`);
    else this.want = null;
  }
}

export { Cmd };
