// HOME's lamp import, the app's half of quadra.py's `home import`: the extractor's devices, the
// light service in each one's MIoT spec, a name and an icon for the knob. The computer's part
// (the file, the network, the spec site) is in src-tauri/src/home.rs; ?demo answers it here.

import { HomeCap, HomeKind, HomeProto } from "./proto";

export interface XDevice {
  name: string;
  model: string;
  did: string;
  ip: string;
  token: string; // hex
}
export interface XFile {
  path: string;
  modified_ms: number;
  devices: XDevice[];
}

export interface LightProps {
  siid: number[]; // on, brightness, colour temperature, colour (0 = none)
  piid: number[];
  caps: number; // HomeCap
  ct: [number, number];
}

const demo = new URLSearchParams(location.search).has("demo");

async function call<T>(cmd: string, args: Record<string, unknown>, fake: () => T): Promise<T> {
  if (demo) return new Promise((r) => window.setTimeout(() => r(fake()), 400));
  const { invoke } = await import("@tauri-apps/api/core");
  return invoke<T>(cmd, args);
}

// --- what the demo answers (the real thing never comes near these) ---
const DEMO_DEVICES: XDevice[] = [
  { name: "Desk Lamp 2", model: "xiaomi.light.lamp32", did: "301", ip: "192.168.1.100", token: "00".repeat(16) },
  { name: "Mi Smart Lightstrip", model: "yeelink.light.stripb", did: "302", ip: "192.168.0.4", token: "00".repeat(16) },
  { name: "Desk lamp", model: "yeelink.light.lamp4", did: "303", ip: "192.168.0.6", token: "00".repeat(16) },
  { name: "Mi Smart LED left", model: "yeelink.light.color5", did: "304", ip: "192.168.0.9", token: "00".repeat(16) },
  { name: "Mi Watch", model: "hmpace.watch.v1", did: "305", ip: "", token: "00".repeat(16) },
  { name: "Mi Router 4A", model: "xiaomi.router.r4a", did: "306", ip: "192.168.0.1", token: "00".repeat(16) },
];
const DEMO_SPECS: Record<string, LightProps> = {
  "xiaomi.light.lamp32": { siid: [2, 2, 2, 0], piid: [1, 2, 3, 0], caps: HomeCap.BRIGHT | HomeCap.TEMP, ct: [2700, 5100] },
  "yeelink.light.stripb": { siid: [2, 2, 0, 2], piid: [1, 3, 0, 4], caps: HomeCap.BRIGHT | HomeCap.COLOR, ct: [0, 0] },
  "yeelink.light.lamp4": { siid: [2, 2, 2, 0], piid: [1, 2, 3, 0], caps: HomeCap.BRIGHT | HomeCap.TEMP, ct: [2600, 5000] },
  "yeelink.light.color5": { siid: [2, 2, 2, 2], piid: [1, 2, 3, 4], caps: HomeCap.BRIGHT | HomeCap.TEMP | HomeCap.COLOR, ct: [1700, 6500] },
};

export const readDevices = () => call<XFile | null>("home_devices", {}, () => ({ path: "~/.quadra/xiaomi-devices.json", modified_ms: Date.now() - 86400000, devices: DEMO_DEVICES }));
export const findLamps = (ips: string[]) => call<Record<string, string>>("home_find", { ips }, () => ({ "301": "192.168.1.100", "302": "192.168.0.5", "303": "192.168.0.4" }));
export const probeLamp = (ip: string, token: string, did: string, siid: number, piid: number) =>
  call<number | null>("home_probe", { ip, token, did, siid, piid }, () => (did === "303" ? HomeProto.LEGACY : did === "304" ? null : HomeProto.MIOT));
export const runExtractor = () => call<void>("home_run_extractor", {}, () => undefined);

// From a model's MIoT spec: its light service's on / brightness / colour temperature / colour.
export async function lightProps(model: string): Promise<LightProps | null> {
  if (demo) return new Promise((r) => window.setTimeout(() => r(DEMO_SPECS[model] ?? null), 300));
  const { invoke } = await import("@tauri-apps/api/core");
  const text = await invoke<string | null>("miot_spec", { model });
  if (!text) return null;
  const spec = JSON.parse(text) as { services?: { iid: number; type: string; properties?: { iid: number; type: string; access?: string[]; "value-range"?: number[] }[] }[] };
  const out: LightProps = { siid: [0, 0, 0, 0], piid: [0, 0, 0, 0], caps: 0, ct: [0, 0] };
  const svc = spec.services?.find((s) => s.type.split(":")[3] === "light");
  if (!svc) return null;
  for (const p of svc.properties ?? []) {
    if (!p.access?.includes("write")) continue;
    const slot = ["on", "brightness", "color-temperature", "color"].indexOf(p.type.split(":")[3]);
    if (slot < 0 || out.siid[slot]) continue;
    out.siid[slot] = svc.iid;
    out.piid[slot] = p.iid;
    if (slot === 1) out.caps |= HomeCap.BRIGHT;
    if (slot === 2) {
      out.caps |= HomeCap.TEMP;
      const [lo, hi] = p["value-range"] ?? [2700, 6500];
      out.ct = [Math.round(lo), Math.round(hi)];
    }
    if (slot === 3) out.caps |= HomeCap.COLOR;
  }
  return out.siid[0] ? out : null;
}

export const isLight = (d: XDevice) => d.model.includes(".light.");

// Which icon the knob draws, from the model: a strip, a desk lamp with a slim arm (lamp1, lamp4),
// another desk lamp, or a bulb.
export function lampKind(model: string): number {
  const m = model.split(".").pop() ?? "";
  if (m.includes("strip")) return HomeKind.STRIP;
  if (m === "lamp1" || m === "lamp4") return HomeKind.DESK_ARM;
  if (m.startsWith("lamp")) return HomeKind.DESK;
  return HomeKind.BULB;
}

// The knob's font is ASCII capitals: accents go, so does a leading brand word.
export function knobName(name: string): string {
  const n = name
    .replace(/[Łł]/g, "L")
    .replace(/[Øø]/g, "O")
    .replace(/ß/g, "SS")
    .normalize("NFKD")
    .replace(/[^\x20-\x7e]/g, "")
    .toUpperCase()
    .trim()
    .replace(/^(XIAOMI|MIJIA|MI|YEELIGHT)\s+(?=\S)/, "");
  return n.slice(0, 19).trim() || "LAMP";
}

export function tokenBytes(hex: string): Uint8Array {
  const b = new Uint8Array(16);
  for (let i = 0; i < 16; i++) b[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16) || 0;
  return b;
}

export const KIND_NAMES = ["Bulb", "Desk lamp", "Desk lamp, arm", "Lightstrip"];
export const PROTO_NAMES = ["MIoT", "Older protocol"];
