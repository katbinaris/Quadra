// A pretend knob for working on the UI without hardware: open the page with ?demo. Answers
// the protocol like host_link.c does and streams a slowly turning knob.

import { CLOCK_SLOTS, ClockOp, Cmd, EXT_SYNTH_VERSION, ExtCmd, ExtStatus, ExtTag, HomeCap, HomeEdit, HomeFlag, HomeKind, HomeOp, HomeProto, ICON_BYTES, ICON_CHUNK, NetOp, LED_COUNT, Op, ProfileFlag, REPORT_SIZE, Res, Set, SYNTH_CHUNK, SYNTH_PUT_CHUNK, SynthEdit, SynthFlag, SynthOp, Tag, TEXT_CHUNK, crc32 } from "./proto";
import { tidySynth, type SynthJson } from "./synth";
import synthBuiltins from "./demo_synths.json";
import { b64ToBytes, ID_RE, type ProfileJson } from "./profile";
import type { Transport } from "./transport";
import builtins from "./demo_builtins.json";

// The firmware's built-ins, icons included (scripts/gen_demo_builtins.sh writes the file).
const BUILTINS = builtins as ProfileJson[];

// One registry entry, as app_profiles.c keeps it.
interface Entry {
  builtin: ProfileJson | null;
  stored: ProfileJson | null;
  live: ProfileJson | null;
}
const view = (e: Entry) => (e.live ?? e.stored ?? e.builtin)!;

// The synths, as midi_synths.c keeps them.
interface SynthReg {
  builtin: SynthJson | null;
  stored: SynthJson | null;
  live: SynthJson | null;
}
const sview = (e: SynthReg) => (e.live ?? e.stored ?? e.builtin)!;

// HOME's lamps, as the knob sees them (the names and models of a real setup).
interface MockLamp {
  did: number;
  name: string;
  kind: number;
  caps: number;
  ip: string;
  proto: number;
  online: boolean;
  on: boolean;
  bright: number;
  ct: number;
  rgb: [number, number, number];
}

export class MockTransport implements Transport {
  readonly kind = "tauri";
  readonly needsGesture = false;
  onReport: (r: Uint8Array) => void = () => {};
  onClosed: () => void = () => {};

  private reg: Entry[] = BUILTINS.map((p) => ({ builtin: structuredClone(p), stored: null, live: null }));
  private upload: { buf: Uint8Array; crc: number; got: number; flags: number } | null = null;
  private live = { detents: 12, kp: 6, kd: 0.01, feel: 0, amp: 100, pitch: 1, sound: 0, hidType: 3, midi: 1, midiSynth: 1, profile: 3, boot: 1, rotation: 0, host: 0, shape: 0 };
  private saved = { ...this.live };
  // ext_proto.h: LIGHTS (saved by SAVE like the rest) and the idle word (stored at once).
  private lights = { src: 0, fx: 0, hue: 200, sat: 80, speed: 5, level: 100 };
  private lightsSaved = { ...this.lights };
  private idleText = "";
  private coverStyle = 2; // SLIDE
  // ext_proto.h: WiFi (EXT_CMD_NET, v4) and the CLOCK app (EXT_CMD_CLOCK, v5). The demo knob is
  // on a network already; the screen stream (v6) and the WiFi link (v7) it doesn't speak.
  private net = { state: 2, rssi: -52, ip: [192, 168, 1, 42], on: 1, ssid: "STUDIO", host: "quadra-7142" };
  private clockFlags = 0x07; // 24 h, seconds, date
  private zones = [
    { label: "LOCAL", rule: "CET-1CEST,M3.5.0,M10.5.0/3", off: 120 },
    { label: "TOKYO", rule: "JST-9", off: 540 },
    { label: "NEW YORK", rule: "EST5EDT,M3.2.0,M11.1.0", off: -240 },
    { label: "", rule: "", off: 0 },
    { label: "", rule: "", off: 0 },
  ];
  // Haptic profiles, like menu.c: a feel per profile and one set of values per profile and
  // feel, with the firmware's placeholder factory values and limits.
  private hp = MockTransport.hpFactory();
  private hpSaved = structuredClone(this.hp);
  // haptic_params.h HAPTIC_PROFILES: factory feel and {kp, kd, shape, amp, pitch} in it.
  private static readonly HP_FACTORY = [
    { feel: 0, t: [6, 0.005, 25, 100, 0.85] },
    { feel: 1, t: [2, 0.035, 0, 100, 0.9] },
    { feel: 0, t: [4, 0.115, 55, 90, 1.2] },
    { feel: 0, t: [1.5, 0.15, 80, 70, 1.95] },
    { feel: 2, t: [0, 0.15, 0, 15, 1.85] },
  ];
  private static hpProfile(p: number) {
    const { feel, t } = MockTransport.HP_FACTORY[p];
    // The other feel of a stepped profile starts from the same numbers (SHAPE is SAW's).
    return { feel, tune: [0, 1, 2].map((f) => ({ kp: t[0], kd: t[1], shape: f === 0 ? t[2] : 0, amp: t[3], pitch: t[4] })) };
  }
  private static hpFactory() {
    return { edit: 1, mode: [1, 1, 1, 1, 1], profiles: [0, 1, 2, 3, 4].map((p) => MockTransport.hpProfile(p)) };
  }
  private static hpLimits(feel: number) {
    return feel === 2 ? { kpMin: 0, kpMax: 0, kdMin: 0, kdMax: 0.15, ampMax: 20, pitchMin: 1, pitchMax: 2 } : { kpMin: 0, kpMax: 20, kdMin: 0, kdMax: 0.15, ampMax: 100, pitchMin: 0.5, pitchMax: 2 };
  }
  private static hpFeels(p: number) {
    return p === 4 ? 0b100 : 0b011;
  }
  private synths: SynthReg[] = (synthBuiltins as SynthJson[]).map((x) => ({ builtin: structuredClone(x), stored: null, live: null }));
  private synthPut: { buf: Uint8Array; crc: number; got: number; save: boolean } | null = null;
  private midiParam = 13;
  private midiValue = 712;
  private midiTx = 214;
  private lamps: MockLamp[] = [
    { did: 301, name: "DESK LAMP 2", kind: HomeKind.DESK, caps: HomeCap.BRIGHT | HomeCap.TEMP, ip: "192.168.1.100", proto: HomeProto.MIOT, online: true, on: true, bright: 62, ct: 4000, rgb: [255, 209, 163] },
    { did: 302, name: "SMART LIGHTSTRIP", kind: HomeKind.STRIP, caps: HomeCap.BRIGHT | HomeCap.COLOR, ip: "192.168.0.5", proto: HomeProto.MIOT, online: true, on: true, bright: 80, ct: 0, rgb: [0, 170, 255] },
    { did: 303, name: "DESK LAMP", kind: HomeKind.DESK_ARM, caps: HomeCap.BRIGHT | HomeCap.TEMP, ip: "192.168.0.4", proto: HomeProto.LEGACY, online: true, on: false, bright: 40, ct: 3200, rgb: [255, 190, 130] },
    { did: 304, name: "MI SMART LED LEFT", kind: HomeKind.BULB, caps: HomeCap.BRIGHT | HomeCap.TEMP | HomeCap.COLOR, ip: "192.168.0.9", proto: HomeProto.MIOT, online: false, on: false, bright: 0, ct: 2700, rgb: [255, 166, 87] },
  ];
  private timer = 0;
  private t0 = performance.now();
  private seq = 0;
  private tick = 0;

  async tryConnect() {
    return true;
  }
  async close() {
    window.clearInterval(this.timer);
  }

  async send(r: Uint8Array) {
    const out = new Uint8Array(REPORT_SIZE);
    const v = new DataView(out.buffer);
    const inV = new DataView(r.buffer, r.byteOffset);
    const reply = (): void => void setTimeout(() => this.onReport(out), 1);
    switch (r[0]) {
      case Cmd.HELLO: {
        out[0] = Tag.HELLO;
        out[1] = 1;
        out[1] = 3;
        out[2] = this.reg.length;
        out.set(new TextEncoder().encode("v2.0.0"), 4);
        out.set(new TextEncoder().encode("SEP 30 2026"), 36);
        return reply();
      }
      case Cmd.SET: {
        const keys = ["detents", "kp", "kd", "feel", "amp", "pitch", "sound", "hidType", "midi", "profile", "boot", "rotation", "host", "shape"] as const;
        const k = keys[r[1]];
        const f = r[1] === Set.KP || r[1] === Set.KD || r[1] === Set.PITCH;
        const val = f ? inV.getFloat32(4, true) : inV.getInt32(4, true);
        const prof = this.hp.profiles[this.hp.edit];
        const tune = prof.tune[prof.feel], lim = MockTransport.hpLimits(prof.feel);
        const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));
        if (r[1] === Set.HAPTIC_PROFILE) this.hp.edit = clamp(val, 0, 4);
        else if (r[1] === Set.MODE_HAPTIC) this.hp.mode[this.live.hidType] = clamp(val, 0, 4);
        else if (r[1] === Set.FEEL) {
          if ((MockTransport.hpFeels(this.hp.edit) >> val) & 1) prof.feel = val;
        } else if (r[1] === Set.KP) tune.kp = clamp(val, lim.kpMin, lim.kpMax);
        else if (r[1] === Set.KD) tune.kd = clamp(val, lim.kdMin, lim.kdMax);
        else if (r[1] === Set.SHAPE) tune.shape = clamp(val, 0, 90);
        else if (r[1] === Set.AMP) tune.amp = clamp(val, 0, lim.ampMax);
        else if (r[1] === Set.PITCH) tune.pitch = clamp(val, lim.pitchMin, lim.pitchMax);
        else if (r[1] === Set.MIDI_SYNTH) this.live.midiSynth = clamp(val, 0, this.synths.length - 1);
        else if (r[1] === Set.MIDI_CH) this.live.midi = clamp(val, 1, 16);
        else if (k) (this.live as any)[k] = val;
        this.settings(out);
        return reply();
      }
      case Cmd.HAPTIC_RESET: {
        this.hp.profiles[this.hp.edit] = MockTransport.hpProfile(this.hp.edit);
        this.settings(out);
        return reply();
      }
      case Cmd.SAVE:
        this.saved = { ...this.live };
        this.hpSaved = structuredClone(this.hp);
        this.lightsSaved = { ...this.lights };
        this.settings(out);
        return reply();
      case Cmd.REVERT:
        this.live = { ...this.saved };
        this.hp = { ...structuredClone(this.hpSaved), edit: this.hp.edit };
        this.lights = { ...this.lightsSaved };
        this.settings(out);
        return reply();
      case ExtCmd.HELLO:
        out[0] = ExtTag.HELLO;
        out[1] = EXT_SYNTH_VERSION;
        return reply();
      case ExtCmd.HOME:
        return this.home(r, out, reply);
      case ExtCmd.SYNTH:
        return this.synth(r, out, reply);
      case ExtCmd.NET:
        if (r[1] === NetOp.SSID) this.net.ssid = new TextDecoder().decode(r.subarray(2, 34)).replace(/\0.*$/s, "");
        if (r[1] === NetOp.APPLY) this.net = { ...this.net, on: r[2] === 1 ? 1 : 0, state: r[2] === 1 ? 2 : 0 };
        if (r[1] !== NetOp.STATUS && r[1] !== NetOp.APPLY) return; // the rest answer at APPLY
        this.netStatus(out);
        return reply();
      case ExtCmd.CLOCK: {
        let slot = 0;
        if (r[1] === ClockOp.FORMAT) this.clockFlags = r[2];
        else if (r[1] === ClockOp.ZONE && r[2] >= 1 && r[2] < CLOCK_SLOTS) {
          slot = r[2];
          const s = (a: number, b: number) => new TextDecoder().decode(r.subarray(a, b)).replace(/\0.*$/s, "");
          this.zones[slot] = { label: s(3, 15), rule: s(15, 61), off: 0 };
        } else if (r[1] === ClockOp.GET && r[2] < CLOCK_SLOTS) slot = r[2];
        const z = this.zones[slot];
        out[0] = ExtTag.CLOCK;
        out[1] = this.clockFlags;
        out[2] = 1;
        out[3] = slot;
        v.setInt16(4, z.off, true);
        out.set(new TextEncoder().encode(z.label), 6);
        out.set(new TextEncoder().encode(z.rule), 18);
        return reply();
      }
      case ExtCmd.LIGHTS: {
        const l = this.lights;
        if (r[2] !== 0xff) l.src = r[2];
        if (r[3] !== 0xff) l.fx = r[3];
        if (inV.getUint16(4, true) !== 0xffff) l.hue = inV.getUint16(4, true);
        if (r[6] !== 0xff) l.sat = r[6];
        if (r[7] !== 0xff) l.speed = r[7];
        if (inV.getUint16(8, true) !== 0xffff) l.level = inV.getUint16(8, true);
        if (r[1] & 1) this.lightsSaved = { ...l };
        this.prefs(out);
        return reply();
      }
      case ExtCmd.PREFS:
        this.prefs(out);
        return reply();
      case ExtCmd.MUSIC:
        if (r[1] !== 0xff && r[1] < 4) this.coverStyle = r[1];
        this.prefs(out);
        return reply();
      case ExtCmd.TEXT:
        this.idleText = new TextDecoder().decode(r.subarray(2, 14)).replace(/\0.*$/s, "");
        out[0] = ExtTag.ACK;
        out[1] = ExtCmd.TEXT;
        return reply();
      case Cmd.GET_SETTINGS:
        this.settings(out);
        return reply();
      case Cmd.STREAM:
        window.clearInterval(this.timer);
        if (r[1] > 0) this.timer = window.setInterval(() => this.stream(), 1000 / r[1]);
        return;
      case Cmd.PROFILE: {
        const e = this.reg[r[1]];
        if (!e) return;
        const p = view(e);
        out[0] = Tag.PROFILE;
        out[1] = r[1];
        out[2] = this.reg.length;
        out[3] = (p.icon48 ? ProfileFlag.ICON : 0) | (e.builtin ? ProfileFlag.BUILTIN : 0) | (e.stored ? ProfileFlag.STORED : 0) | (e.live ? ProfileFlag.LIVE : 0);
        out.set(new TextEncoder().encode(p.id), 4);
        out.set(new TextEncoder().encode(p.name), 16);
        p.legend.forEach((l, i) => out.set(new TextEncoder().encode(l), 32 + i * 8));
        return reply();
      }
      case Cmd.PROFILE_ICON: {
        const idx = r[1], off = inV.getUint16(2, true);
        const p = this.reg[idx] && view(this.reg[idx]);
        if (!p?.icon48) return;
        const icon = b64ToBytes(p.icon48);
        const len = Math.min(ICON_CHUNK, ICON_BYTES - off);
        out[0] = Tag.PROFILE_ICON;
        out[1] = idx;
        v.setUint16(2, off, true);
        out[4] = len;
        out.set(icon.subarray(off, off + len), 8);
        return reply();
      }
      case Cmd.PROFILE_READ: {
        const e = this.reg[r[1]];
        if (!e) return;
        const text = new TextEncoder().encode(JSON.stringify(view(e)));
        const begin = new Uint8Array(REPORT_SIZE);
        const bv = new DataView(begin.buffer);
        begin[0] = Tag.PROFILE_BEGIN;
        begin[1] = r[1];
        bv.setUint32(4, text.length, true);
        bv.setUint32(8, crc32(text), true);
        const pieces = [begin];
        for (let o = 0; o < text.length; o += TEXT_CHUNK) {
          const d = new Uint8Array(REPORT_SIZE);
          d[0] = Tag.PROFILE_DATA;
          d[1] = o & 0xff;
          d[2] = (o >> 8) & 0xff;
          d[3] = (o >> 16) & 0xff;
          d.set(text.subarray(o, o + TEXT_CHUNK), 4);
          pieces.push(d);
        }
        setTimeout(() => pieces.forEach((x) => this.onReport(x)), 30);
        return;
      }
      case Cmd.UPLOAD_BEGIN:
        this.upload = { buf: new Uint8Array(inV.getUint32(4, true)), crc: inV.getUint32(8, true), got: 0, flags: r[1] };
        return;
      case Cmd.UPLOAD_DATA: {
        const u = this.upload;
        if (!u) return;
        const off = r[1] | (r[2] << 8) | (r[3] << 16);
        const n = Math.min(TEXT_CHUNK, u.buf.length - off);
        if (off === u.got) {
          u.buf.set(r.subarray(4, 4 + n), off);
          u.got += n;
        }
        return;
      }
      case Cmd.UPLOAD_END: {
        const u = this.upload;
        this.upload = null;
        let res: number = Res.OK, why = "", index = 0;
        if (!u || u.got !== u.buf.length || crc32(u.buf) !== u.crc) res = Res.TRANSFER;
        else {
          try {
            const text = new TextDecoder().decode(u.buf);
            (window as unknown as { __quadraLastUpload?: string }).__quadraLastUpload = text; // tests read it back
            const p = JSON.parse(text) as ProfileJson;
            if (!ID_RE.test(p.id)) throw new Error("id: a-z, 0-9, _ and - only");
            index = this.reg.findIndex((e) => view(e).id === p.id);
            if (index < 0) {
              index = this.reg.length;
              this.reg.push({ builtin: null, stored: null, live: p });
            } else this.reg[index].live = p;
            if (u.flags & 1) this.saveEntry(this.reg[index]);
          } catch (e) {
            res = Res.INVALID;
            why = String((e as Error).message);
          }
        }
        return this.result(Cmd.UPLOAD_END, res, index, false, why);
      }
      case Cmd.PROFILE_OP: {
        const e = this.reg[r[1]];
        if (!e) return this.result(Cmd.PROFILE_OP, Res.BAD_INDEX, r[1], false);
        let removed = false;
        if (r[2] === Op.SAVE) this.saveEntry(e);
        else if (r[2] === Op.REVERT) e.live = null;
        else if (r[2] === Op.REMOVE) e.live = e.stored = null;
        if (!e.builtin && !e.stored && !e.live) {
          this.reg.splice(r[1], 1);
          removed = true;
          if (this.live.profile >= r[1] && this.live.profile > 0) this.live.profile--;
        }
        return this.result(Cmd.PROFILE_OP, Res.OK, r[1], removed);
      }
    }
  }

  private ack(cmd: number, status: number) {
    const out = new Uint8Array(REPORT_SIZE);
    out[0] = ExtTag.ACK;
    out[1] = cmd;
    out[2] = status;
    setTimeout(() => this.onReport(out), 20);
  }

  private home(r: Uint8Array, out: Uint8Array, reply: () => void) {
    const v = new DataView(out.buffer), inV = new DataView(r.buffer, r.byteOffset);
    if (r[1] === HomeOp.STATUS) {
      const l = this.lamps[r[2]];
      out[0] = ExtTag.HOME;
      out[1] = this.lamps.length;
      out[2] = r[2];
      if (l) {
        v.setUint32(3, l.did, true);
        out[7] = (l.online ? HomeFlag.ONLINE | HomeFlag.KNOWN : 0) | (l.online && l.on ? HomeFlag.ON : 0);
        out[8] = l.bright;
        v.setUint16(9, l.ct, true);
        out.set(l.rgb, 11);
        out[14] = l.caps;
        out.set(new TextEncoder().encode(l.name), 15);
        out[35] = l.kind;
        out.set(l.ip.split(".").map(Number), 36);
        out[40] = l.proto;
      }
      return reply();
    }
    if (r[1] === HomeOp.EDIT) {
      const slot = r[2], l = this.lamps[slot];
      if (!l || l.did !== inV.getUint32(4, true)) return this.ack(ExtCmd.HOME, ExtStatus.BAD_PARAM);
      if (r[3] === HomeEdit.NAME) l.name = new TextDecoder().decode(r.subarray(8, 27)).replace(/\0.*$/s, "");
      else if (r[3] === HomeEdit.KIND) l.kind = r[8];
      else if (r[3] === HomeEdit.MOVE) this.lamps.splice(r[8], 0, ...this.lamps.splice(slot, 1));
      else if (r[3] === HomeEdit.REMOVE) this.lamps.splice(slot, 1);
      return this.ack(ExtCmd.HOME, ExtStatus.OK);
    }
    if (r[1] === HomeOp.BEGIN) this.importing = [];
    else if (r[1] === HomeOp.LAMP) {
      const name = new TextDecoder().decode(r.subarray(41, 60)).replace(/\0.*$/s, "");
      this.importing[r[2]] = { did: inV.getUint32(3, true), name, kind: r[61], caps: r[28], ip: Array.from(r.subarray(7, 11)).join("."), proto: r[27], online: true, on: true, bright: 50, ct: 4000, rgb: [255, 209, 163] };
    } else if (r[1] === HomeOp.COMMIT) this.lamps = this.importing.slice(0, r[2]);
    return this.ack(ExtCmd.HOME, ExtStatus.OK);
  }
  private importing: MockLamp[] = [];

  private synthFlags(e: SynthReg) {
    return (e.builtin ? SynthFlag.BUILTIN : 0) | (e.stored ? SynthFlag.STORED : 0) | (e.live ? SynthFlag.LIVE : 0);
  }

  private synthResult(what: number, res: number, index: number, removed = false, why = "") {
    const out = new Uint8Array(REPORT_SIZE);
    out[0] = ExtTag.SYNTH;
    out[1] = SynthOp.RESULT;
    out[2] = what;
    out[3] = res;
    out[4] = index;
    out[5] = removed ? 1 : 0;
    out.set(new TextEncoder().encode(why.slice(0, 55)), 8);
    setTimeout(() => this.onReport(out), 40);
  }

  private synth(r: Uint8Array, out: Uint8Array, reply: () => void) {
    const v = new DataView(out.buffer), inV = new DataView(r.buffer, r.byteOffset);
    out[0] = ExtTag.SYNTH;
    switch (r[1]) {
      case SynthOp.LIST: {
        const e = this.synths[r[2]];
        out[1] = SynthOp.LIST;
        out[2] = r[2];
        out[3] = this.synths.length;
        if (e) {
          const x = sview(e);
          out[4] = this.synthFlags(e);
          out[5] = x.params.length;
          out[6] = x.channel;
          out[7] = x.programs?.scheme === "korg-bank100" ? 1 : 0;
          v.setUint16(8, x.programs?.count ?? 128, true);
          out.set(new TextEncoder().encode(x.id), 10);
          out.set(new TextEncoder().encode(x.maker), 34);
          out.set(new TextEncoder().encode(x.name), 46);
        }
        return reply();
      }
      case SynthOp.READ: {
        const e = this.synths[r[2]];
        const text = e ? new TextEncoder().encode(JSON.stringify(tidySynth(sview(e)))) : new Uint8Array(0);
        const off = inV.getUint32(4, true), n = Math.max(0, Math.min(SYNTH_CHUNK, text.length - off));
        out[1] = SynthOp.READ;
        out[2] = r[2];
        out[3] = n;
        v.setUint32(4, text.length, true);
        v.setUint32(8, crc32(text), true);
        v.setUint32(12, off, true);
        out.set(text.subarray(off, off + n), 16);
        return reply();
      }
      case SynthOp.STATUS: {
        const midi = this.live.hidType === 2;
        out[1] = SynthOp.STATUS;
        out[2] = midi ? 1 : 0;
        out[3] = this.live.midiSynth;
        out[4] = this.midiParam;
        v.setInt16(5, this.midiValue, true);
        v.setInt16(8, 123, true);
        out[10] = this.live.midi;
        out[11] = midi ? 1 : 0;
        out[12] = 1;
        v.setUint32(13, this.midiTx, true);
        v.setUint32(17, 37, true);
        return reply();
      }
      case SynthOp.GOTO:
        this.midiParam = r[2];
        this.midiValue = -1;
        return this.ack(ExtCmd.SYNTH, ExtStatus.OK);
      case SynthOp.PUT_BEGIN:
        this.synthPut = { buf: new Uint8Array(inV.getUint32(4, true)), crc: inV.getUint32(8, true), got: 0, save: (r[2] & 1) !== 0 };
        return;
      case SynthOp.PUT_DATA: {
        const u = this.synthPut, off = r[2] | (r[3] << 8) | (r[4] << 16), n = Math.min(SYNTH_PUT_CHUNK, r[5]);
        if (u && off === u.got) {
          u.buf.set(r.subarray(8, 8 + n), off);
          u.got += n;
        }
        return;
      }
      case SynthOp.PUT_END: {
        const u = this.synthPut;
        this.synthPut = null;
        if (!u || u.got !== u.buf.length || crc32(u.buf) !== u.crc) return this.synthResult(SynthOp.PUT_END, 1, 0, false, "transfer");
        const x = JSON.parse(new TextDecoder().decode(u.buf)) as SynthJson;
        let i = this.synths.findIndex((e) => sview(e).id === x.id);
        if (i < 0) {
          i = this.synths.length;
          this.synths.push({ builtin: null, stored: null, live: x });
        } else this.synths[i].live = x;
        if (u.save) this.synths[i] = { ...this.synths[i], stored: x, live: null };
        return this.synthResult(SynthOp.PUT_END, 0, i);
      }
      case SynthOp.OP: {
        const i = r[2], e = this.synths[i];
        if (!e) return this.synthResult(SynthOp.OP, 4, i);
        if (r[3] === SynthEdit.SAVE) {
          e.stored = sview(e);
          e.live = null;
        } else if (r[3] === SynthEdit.REVERT) e.live = null;
        else if (r[3] === SynthEdit.REMOVE) e.live = e.stored = null;
        const removed = !e.builtin && !e.stored && !e.live;
        if (removed) {
          this.synths.splice(i, 1);
          if (this.live.midiSynth >= i && this.live.midiSynth > 0) this.live.midiSynth--;
        }
        return this.synthResult(SynthOp.OP, 0, i, removed);
      }
    }
  }

  private prefs(out: Uint8Array) {
    const l = this.lights, v = new DataView(out.buffer);
    out[0] = ExtTag.PREFS;
    out[1] = l.src;
    out[2] = l.fx;
    v.setUint16(3, l.hue, true);
    out[5] = l.sat;
    out[6] = l.speed;
    v.setUint16(7, l.level, true);
    out[9] = JSON.stringify(l) !== JSON.stringify(this.lightsSaved) ? 1 : 0;
    out[10] = this.coverStyle;
    out[11] = 4; // cover styles: the demo knob has them
    out.set(new TextEncoder().encode(this.idleText), 16);
  }

  private netStatus(out: Uint8Array) {
    const n = this.net, on = n.state === 2;
    out[0] = ExtTag.NET;
    out[1] = n.state;
    out[2] = on ? n.rssi & 0xff : 0;
    if (on) out.set(n.ip, 3);
    out[7] = on ? 1 : 0;
    out[8] = n.on;
    out.set(new TextEncoder().encode(n.ssid), 9);
    out.set(new TextEncoder().encode(n.host), 41);
  }

  private saveEntry(e: Entry) {
    if (e.live) e.stored = e.live;
    e.live = null;
  }

  private result(cmd: number, res: number, index: number, removed: boolean, why = "") {
    const out = new Uint8Array(REPORT_SIZE);
    out[0] = Tag.RESULT;
    out[1] = cmd;
    out[2] = res;
    out[3] = index;
    out[4] = this.reg.length;
    out[5] = removed ? 1 : 0;
    out.set(new TextEncoder().encode(why.slice(0, 55)), 8);
    setTimeout(() => this.onReport(out), 60);
  }

  private settings(out: Uint8Array) {
    const v = new DataView(out.buffer);
    const l = this.live, s = this.saved;
    const keys = ["sound", "hidType", "midi", "midiSynth", "profile", "boot", "rotation", "host"] as const;
    const ids = [Set.SOUND, Set.HID_TYPE, Set.MIDI_CH, Set.MIDI_SYNTH, Set.PROFILE, Set.BOOT, Set.ROTATION, Set.HOST];
    let dirty = 0;
    keys.forEach((k, i) => {
      if (l[k] !== s[k]) dirty |= 1 << ids[i];
    });
    // The shown haptic profile in its feel, against what's saved; anything else unsaved in
    // the profiles shows on HAPTIC_PROFILE.
    const e = this.hp.edit, prof = this.hp.profiles[e], was = this.hpSaved.profiles[e];
    const t = prof.tune[prof.feel], st = was.tune[prof.feel], lim = MockTransport.hpLimits(prof.feel);
    const ne = (a: number, b: number) => Math.abs(a - b) > 1e-6;
    if (ne(t.kp, st.kp)) dirty |= 1 << Set.KP;
    if (ne(t.kd, st.kd)) dirty |= 1 << Set.KD;
    if (t.shape !== st.shape) dirty |= 1 << Set.SHAPE;
    if (prof.feel !== was.feel) dirty |= 1 << Set.FEEL;
    if (t.amp !== st.amp) dirty |= 1 << Set.AMP;
    if (ne(t.pitch, st.pitch)) dirty |= 1 << Set.PITCH;
    this.hp.profiles.forEach((p, q) => {
      const ps = this.hpSaved.profiles[q];
      p.tune.forEach((x, g) => {
        if ((q !== e || g !== prof.feel) && JSON.stringify(x) !== JSON.stringify(ps.tune[g])) dirty |= 1 << Set.HAPTIC_PROFILE;
      });
      if (q !== e && p.feel !== ps.feel) dirty |= 1 << Set.HAPTIC_PROFILE;
    });
    if (this.hp.mode.join() !== this.hpSaved.mode.join()) dirty |= 1 << Set.MODE_HAPTIC;
    out[0] = Tag.SETTINGS;
    v.setUint16(1, dirty & 0xffff, true);
    out[3] = dirty >>> 16;
    v.setInt32(4, [8, 12, 24, 36, 24][e], true);
    v.setFloat32(8, t.kp, true);
    v.setFloat32(12, t.kd, true);
    out[16] = prof.feel;
    out[17] = t.amp;
    v.setFloat32(18, t.pitch, true);
    out[22] = l.sound;
    out[23] = l.hidType;
    out[24] = l.midi;
    out[25] = l.profile;
    out[26] = l.boot;
    out[27] = l.rotation;
    out[28] = l.host;
    out[29] = t.shape;
    out[30] = e;
    out[31] = MockTransport.hpFeels(e);
    out[32] = lim.ampMax;
    out[33] = this.hp.mode[l.hidType];
    out[34] = l.midiSynth;
    out[35] = this.synths.length;
    v.setFloat32(36, lim.kpMin, true);
    v.setFloat32(40, lim.kpMax, true);
    v.setFloat32(44, lim.kdMin, true);
    v.setFloat32(48, lim.kdMax, true);
    v.setFloat32(52, lim.pitchMin, true);
    v.setFloat32(56, lim.pitchMax, true);
  }

  // Like led_task.c: a dim gradient round the ring, a bright spot at
  // the knob, keys dim (a held one bright). Values as the strips get them: at most ~51.
  private ledFrame(angle: number, held: number) {
    const color = [255, 201, 77];
    const rgb = new Uint8Array(LED_COUNT * 3);
    const pos = (((angle / (2 * Math.PI)) * 60) % 60 + 60) % 60;
    for (let i = 0; i < 60; i++) {
      let d = Math.abs(i - pos);
      d = Math.min(d, 60 - d);
      const spot = Math.max(0, 1 - d / 2.5);
      const rest = 0.25 + 0.1 * Math.sin((i / 60) * 2 * Math.PI);
      for (let c = 0; c < 3; c++) rgb[i * 3 + c] = Math.round(51 * Math.min(1, rest * (color[c] / 255) + spot * ([255, 201, 77][c] / 255)));
    }
    for (let k = 0; k < 4; k++) {
      const on = (held >> k) & 1;
      for (let j = 0; j < 2; j++) for (let c = 0; c < 3; c++) rgb[(60 + k * 2 + j) * 3 + c] = Math.round((on ? 51 : 10) * (color[c] / 255));
    }
    for (let first = 0; first < LED_COUNT; first += 20) {
      const r = new Uint8Array(REPORT_SIZE);
      const n = Math.min(20, LED_COUNT - first);
      r[0] = Tag.LEDS;
      r[1] = first;
      r[2] = n;
      r.set(rgb.subarray(first * 3, (first + n) * 3), 4);
      this.onReport(r);
    }
  }

  private stream() {
    const t = (performance.now() - this.t0) / 1000;
    const angle = Math.sin(t * 0.6) * 2.4 + t * 0.3;
    const st = new Uint8Array(REPORT_SIZE);
    const v = new DataView(st.buffer);
    st[0] = Tag.STATE;
    v.setUint16(1, this.seq++, true);
    v.setInt32(4, Math.round(angle * 1e4), true);
    v.setInt32(8, Math.round((angle / (2 * Math.PI)) * this.live.detents), true);
    st[12] = Math.floor(t) % 7 === 3 ? 1 : 0;
    v.setInt32(16, Math.floor(t * 3), true);
    this.onReport(st);

    if (this.tick % 2 === 0) this.ledFrame(angle, st[12]);
    if (++this.tick % 15 !== 0) return; // SYS twice a second at 30 Hz
    const a = new Uint8Array(REPORT_SIZE);
    const av = new DataView(a.buffer);
    const motor = 40 + Math.round(30 * Math.abs(Math.sin(t)));
    a[0] = Tag.SYS_A;
    av.setUint16(4, motor, true);
    av.setUint16(6, 71, true);
    av.setUint16(8, 150, true);
    av.setUint16(10, motor + 221, true);
    av.setUint16(12, 512, true);
    a[14] = 1;
    av.setFloat32(16, 37.2 + Math.sin(t / 9), true);
    av.setFloat32(20, 38.4, true);
    av.setUint16(24, 180, true);
    av.setUint16(26, 350, true);
    av.setFloat32(28, 0.1, true);
    a[32] = 5;
    av.setUint16(34, 3000, true);
    av.setUint16(36, 5000, true);
    this.onReport(a);

    const b = new Uint8Array(REPORT_SIZE);
    const bv = new DataView(b.buffer);
    b[0] = Tag.SYS_B;
    b[4] = 58;
    b[5] = 9 + Math.round(3 * Math.random());
    b[6] = 61;
    b[7] = 14;
    bv.setFloat32(8, 10, true);
    bv.setFloat32(12, 32, true);
    bv.setFloat32(16, 533, true);
    bv.setFloat32(20, 438, true);
    bv.setUint32(24, 0, true);
    bv.setFloat32(28, 0, true);
    bv.setUint32(32, 186 * 1024, true);
    bv.setUint32(36, 151 * 1024, true);
    bv.setUint32(48, Math.floor(t) + 3700, true);
    this.onReport(b);
  }
}
