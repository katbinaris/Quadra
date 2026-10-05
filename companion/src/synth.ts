// A synth profile as the knob stores it (midi_synths.c, /fs/synths/<id>.json): the synth's
// parameters, each a CC (or KORG's 10-bit pair) with its range or its switch options. The
// limits are the firmware's (midi.h); `problem` says what the knob would refuse.

export interface SynthOption {
  name: string;
  value: number; // 0..127, sent for this option
}
export interface SynthParam {
  group: string; // the caption above the name: "FILTER"
  name: string;
  sends?: "cc" | "korg10";
  cc: number;
  centred?: boolean;
  options?: SynthOption[]; // a switch
}
export interface SynthJson {
  id: string;
  maker: string; // "KORG", "ROLAND", "MIDI" (GENERIC), or ""
  name: string;
  channel: number; // its factory channel, 0 = none
  programs?: { scheme: "pc" | "korg-bank100"; count: number };
  params: SynthParam[];
}

export const SYNTH_MAX = { id: 23, maker: 11, name: 15, label: 11, option: 9, params: 64, options: 8 } as const;
export const SYNTH_ID_RE = /^[a-z0-9-]{1,23}$/;

const printable = (s: string) => /^[\x20-\x7e]*$/.test(s);

export function problem(s: SynthJson): string | null {
  if (!SYNTH_ID_RE.test(s.id)) return "The id is 1 to 23 of a-z, 0-9 and -";
  if (!s.name || s.name.length > SYNTH_MAX.name || !printable(s.name)) return `The name is 1 to ${SYNTH_MAX.name} characters`;
  if (s.maker.length > SYNTH_MAX.maker || !printable(s.maker)) return `The maker is up to ${SYNTH_MAX.maker} characters`;
  if (s.channel < 0 || s.channel > 16) return "The channel is 1 to 16, or none";
  if (s.params.length < 1 || s.params.length > SYNTH_MAX.params) return `A synth has 1 to ${SYNTH_MAX.params} parameters`;
  for (const [i, p] of s.params.entries()) {
    const at = `Parameter ${i + 1}${p.name ? ` (${p.name})` : ""}`;
    if (!p.name || p.name.length > SYNTH_MAX.label || !printable(p.name)) return `${at}: a name of 1 to ${SYNTH_MAX.label} characters`;
    if (p.group.length > SYNTH_MAX.label || !printable(p.group)) return `${at}: a group of up to ${SYNTH_MAX.label} characters`;
    if (!Number.isInteger(p.cc) || p.cc < 0 || p.cc > 127) return `${at}: a CC number 0 to 127`;
    if (p.options) {
      if (p.options.length < 2 || p.options.length > SYNTH_MAX.options) return `${at}: a switch has 2 to ${SYNTH_MAX.options} options`;
      for (const [k, o] of p.options.entries()) {
        if (!o.name || o.name.length > SYNTH_MAX.option || !printable(o.name)) return `${at}, option ${k + 1}: a name of 1 to ${SYNTH_MAX.option} characters`;
        if (!Number.isInteger(o.value) || o.value < 0 || o.value > 127) return `${at}, option ${k + 1}: a value 0 to 127`;
      }
    }
  }
  return null;
}

// What the knob turns through: 0..127, 0..1023 for KORG's 10-bit, or the options.
export const paramMax = (p: SynthParam) => (p.options ? p.options.length - 1 : p.sends === "korg10" ? 1023 : 127);

export function valueText(p: SynthParam, v: number): string {
  if (v < 0) return "--";
  if (p.options) return p.options[Math.max(0, Math.min(p.options.length - 1, v))].name;
  if (p.centred) {
    const d = Math.round(v - (paramMax(p) + 1) / 2);
    return d > 0 ? `+${d}` : String(d);
  }
  return String(v);
}

export function rangeText(p: SynthParam): string {
  if (p.options) return p.options.map((o) => o.name).join(" · ");
  const max = paramMax(p);
  return p.centred ? `−${(max + 1) / 2} … +${(max - 1) / 2}, centred` : `0 … ${max}`;
}

// Option values spread over 0..127 the way a synth that splits the range into equal zones reads
// them (the firmware's Roland switches: 0 / 127, 0 / 64 / 127, 0 / 43 / 85 / 127).
export const evenValues = (n: number) => Array.from({ length: n }, (_, i) => (n === 1 ? 0 : Math.round((i * 127) / (n - 1))));

// An id not taken yet, from a name.
export function freeId(base: string, taken: string[]): string {
  const stem = base.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 18) || "synth";
  if (!taken.includes(stem)) return stem;
  for (let n = 2; ; n++) if (!taken.includes(`${stem}-${n}`)) return `${stem}-${n}`;
}

export function newSynth(taken: string[]): SynthJson {
  return {
    id: freeId("my-synth", taken),
    maker: "",
    name: "MY SYNTH",
    channel: 1,
    programs: { scheme: "pc", count: 128 },
    params: [
      { group: "FILTER", name: "CUTOFF", sends: "cc", cc: 74 },
      { group: "FILTER", name: "RESONANCE", sends: "cc", cc: 71 },
      { group: "AMP", name: "ATTACK", sends: "cc", cc: 73 },
      { group: "AMP", name: "RELEASE", sends: "cc", cc: 72 },
    ],
  };
}

// The JSON the knob takes: keys in its order, nothing it doesn't know.
export function tidySynth(s: SynthJson): SynthJson {
  return {
    id: s.id,
    maker: s.maker,
    name: s.name,
    channel: s.channel,
    programs: { scheme: s.programs?.scheme ?? "pc", count: s.programs?.count ?? 128 },
    params: s.params.map((p) => ({
      group: p.group,
      name: p.name,
      sends: p.options ? "cc" : (p.sends ?? "cc"),
      cc: p.cc,
      ...(p.centred && !p.options ? { centred: true } : {}),
      ...(p.options ? { options: p.options.map((o) => ({ name: o.name, value: o.value })) } : {}),
    })),
  };
}

// The groups in the order they first appear, each with its parameters' indexes.
export function groups(s: SynthJson): { group: string; rows: number[] }[] {
  const out: { group: string; rows: number[] }[] = [];
  s.params.forEach((p, i) => {
    const last = out[out.length - 1];
    if (last && last.group === p.group) last.rows.push(i);
    else out.push({ group: p.group, rows: [i] });
  });
  return out;
}

// One MIDI message, as the monitor shows it, and the parameter it moved (CC 63 is KORG's low bits).
export function describe(bytes: number[], s: SynthJson | null): { type: string; ch: number; data: string; what: string } {
  const st = bytes[0] ?? 0, ch = (st & 0x0f) + 1, kind = st & 0xf0;
  const d = bytes.slice(1).join(" · ");
  if (kind === 0xb0) {
    const cc = bytes[1], v = bytes[2];
    if (cc === 0) return { type: "Bank select", ch, data: d, what: `Bank MSB ${v}` };
    if (cc === 32) return { type: "Bank select", ch, data: d, what: `Bank LSB ${v}` };
    if (cc === 63) return { type: "Control change", ch, data: d, what: "KORG 10-bit: the low bits" };
    const p = s?.params.find((x) => x.cc === cc);
    if (!p) return { type: "Control change", ch, data: d, what: `CC ${cc} = ${v}` };
    const opt = p.options?.reduce((best, o, i) => (Math.abs(o.value - v) < Math.abs(p.options![best].value - v) ? i : best), 0);
    const shown = p.options ? p.options[opt!].name : p.sends === "korg10" ? `${v << 3}…` : valueText(p, v);
    return { type: "Control change", ch, data: d, what: `${p.group ? p.group + " " : ""}${p.name} = ${shown}` };
  }
  if (kind === 0xc0) return { type: "Program change", ch, data: d, what: `Program ${bytes[1] + 1}` };
  if (kind === 0x90) return { type: "Note on", ch, data: d, what: "" };
  if (kind === 0x80) return { type: "Note off", ch, data: d, what: "" };
  return { type: `0x${st.toString(16)}`, ch, data: d, what: "" };
}
