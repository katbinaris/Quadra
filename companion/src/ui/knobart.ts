// The knob's HOME and MIDI art, drawn the way the knob draws it (whole pixels on black; PIXEL_ART.md
// §10: where the app shows the knob itself): the lamps (ui_extras.cpp lamp_big / lamp_small),
// the Mi badge, KORG's and Roland's wordmarks (ui_gfx.cpp), and the two screens with the LED ring.
// Ported, not baked: a lamp shows in its own colour and brightness.

import { HomeCap, HomeFlag, type Lamp } from "../proto";
import { valueText, paramMax, type SynthJson } from "../synth";

const BLACK = 0x000000, WHITE = 0xffffff, GREY = 0x6e6e6e, DARK = 0x3a3a3a, AMBER = 0xffc94d, MI_ORANGE = 0xff6900;
const hex = (c: number) => "#" + (c >>> 0).toString(16).padStart(6, "0");
const scaleRgb = (c: number, k: number) => {
  const f = (s: number) => Math.max(0, Math.min(255, Math.round(((c >> s) & 255) * k)));
  return (f(16) << 16) | (f(8) << 8) | f(0);
};
const BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]];
const dith = (x: number, y: number, d: number) => BAYER4[y & 3][x & 3] < d * 16;
// A lamp's light at a brightness: its colour, dimmed, never below a third (or it reads as off).
const lampLit = (rgb: number, b: number) => scaleRgb(rgb, 0.34 + (0.66 * b) / 100);

interface Sprite {
  w: number;
  h: number;
  rows: string;
}

// Drawing into a canvas in whole pixels, like ui_gfx.cpp.
class G {
  ctx: CanvasRenderingContext2D;
  constructor(c: HTMLCanvasElement) {
    this.ctx = c.getContext("2d")!;
    this.ctx.imageSmoothingEnabled = false;
  }
  rect(x: number, y: number, w: number, h: number, c: number) {
    if (w <= 0 || h <= 0) return;
    this.ctx.fillStyle = hex(c);
    this.ctx.fillRect(Math.round(x), Math.round(y), w, h);
  }
  cut(x: number, y: number, w: number, h: number, c: number) {
    this.rect(x + 1, y, w - 2, h, c);
    this.rect(x, y + 1, w, h - 2, c);
  }
  disc(cx: number, cy: number, r: number, c: number) {
    const rows = Math.floor(2 * r);
    for (let yy = 0; yy < rows; yy++) {
      const dy = yy - r + 0.5, hw = Math.sqrt(Math.max(0, r * r - dy * dy));
      const x0 = Math.round(cx - hw), x1 = Math.round(cx + hw);
      this.rect(x0, Math.round(cy - r) + yy, x1 - x0, 1, c);
    }
  }
  sprite(s: Sprite, x: number, y: number, c: number, sc = 1) {
    for (let j = 0; j < s.h; j++) for (let i = 0; i < s.w; i++) if (s.rows[j * s.w + i] === "#") this.rect(x + i * sc, y + j * sc, sc, sc, c);
  }
  text(s: string, x: number, y: number, c: number, size = 8, align: CanvasTextAlign = "center") {
    this.ctx.font = `${size}px Silkscreen`;
    this.ctx.textAlign = align;
    this.ctx.textBaseline = "top";
    this.ctx.fillStyle = hex(c);
    this.ctx.fillText(s, Math.round(x), Math.round(y));
    return this.ctx.measureText(s).width;
  }
  width(s: string, size = 8) {
    this.ctx.font = `${size}px Silkscreen`;
    return this.ctx.measureText(s).width;
  }
}

// --- the lamps ---

export function kelvinRgb(k: number): number {
  const t = k / 100;
  let r: number, g: number, b: number;
  if (t <= 66) {
    r = 255;
    g = 99.47 * Math.log(t) - 161.12;
    b = t <= 19 ? 0 : 138.52 * Math.log(t - 10) - 305.04;
  } else {
    r = 329.7 * Math.pow(t - 60, -0.1332);
    g = 288.12 * Math.pow(t - 60, -0.0755);
    b = 255;
  }
  const c = (v: number) => Math.max(0, Math.min(255, v | 0));
  return (c(r) << 16) | (c(g) << 8) | c(b);
}

export interface LampLook {
  line: number;
  body: number;
  light: number; // 0: none (off or not answering)
  bright: number;
}
const lampRgb = (l: Lamp) => (l.rgb[0] << 16) | (l.rgb[1] << 8) | l.rgb[2];
export function lampLook(l: Lamp, chosen: boolean): LampLook {
  const online = (l.flags & HomeFlag.ONLINE) !== 0, on = (l.flags & HomeFlag.KNOWN) !== 0 && (l.flags & HomeFlag.ON) !== 0;
  if (!online) return { line: DARK, body: BLACK, light: 0, bright: 0 };
  return { line: chosen ? WHITE : GREY, body: DARK, light: on ? lampRgb(l) || kelvinRgb(4000) : 0, bright: l.bright };
}
// A lamp as it would look lit (for the icon picker, whatever the lamp is doing now).
export const LIT: LampLook = { line: WHITE, body: DARK, light: kelvinRgb(3000), bright: 80 };
export const UNLIT: LampLook = { line: DARK, body: BLACK, light: 0, bright: 0 };

const thick = (g: G, xa: number, ya: number, xb: number, yb: number, w: number, c: number) => {
  const n = Math.max(Math.abs(xb - xa), Math.abs(yb - ya));
  for (let i = 0; i <= n; i++) g.rect(Math.round(xa + ((xb - xa) * i) / n), Math.round(ya + ((yb - ya) * i) / n), w, 1, c);
};
function cone(g: G, x0: number, x1: number, y0: number, y1: number, spread: number, rgb: number, bright: number) {
  for (let y = y0; y <= y1; y++) {
    const d = (y - y0) / (y1 > y0 ? y1 - y0 : 1), half = (y - y0) * spread, dens = (0.06 + (0.32 * bright) / 100) * (1 - 0.7 * d);
    for (let x = Math.round(x0 - half); x <= Math.round(x1 + half); x++) if (dith(x, y, dens)) g.rect(x, y, 1, 1, rgb);
  }
}

// About 36 x 32 px, centred on (cx, cy).
function lampBig(g: G, kind: number, cx: number, cy: number, o: LampLook) {
  const glow = o.light ? lampLit(o.light, o.bright) : 0;
  if (kind === 1) { // Desk Lamp 2: a block foot, a straight column, a broad head
    if (o.light) cone(g, cx - 6, cx + 16, cy - 11, cy + 13, 0.3, o.light, o.bright);
    g.cut(cx - 17, cy + 10, 22, 5, o.line); g.rect(cx - 16, cy + 11, 20, 3, o.body); g.rect(cx - 2, cy + 12, 4, 1, o.line);
    g.rect(cx - 13, cy - 13, 5, 24, o.line); g.rect(cx - 12, cy - 12, 3, 22, o.body);
    g.cut(cx - 13, cy - 17, 32, 6, o.line); g.rect(cx - 12, cy - 16, 30, 4, o.body);
    g.rect(cx - 6, cy - 12, 23, 1, glow || DARK);
  } else if (kind === 2) { // Desk Lamp 1S: a round foot, one slim arm, a long thin light bar
    if (o.light) cone(g, cx - 1, cx + 14, cy - 11, cy + 13, 0.32, o.light, o.bright);
    g.cut(cx - 16, cy + 10, 16, 5, o.line); g.rect(cx - 15, cy + 11, 14, 3, o.body);
    thick(g, cx - 9, cy + 9, cx - 3, cy - 11, 2, o.line);
    g.cut(cx - 5, cy - 15, 4, 4, o.line);
    g.cut(cx - 3, cy - 15, 21, 4, o.line); g.rect(cx - 2, cy - 14, 19, 2, o.body);
    g.rect(cx - 1, cy - 12, 16, 1, glow || DARK);
  } else if (kind === 3) { // Lightstrip: the controller, then the strip in a wave with its LEDs
    const ys = (x: number) => cy + 1 + Math.round(4 * Math.sin(((x - (cx - 10)) / 26) * 2 * Math.PI));
    if (o.light)
      for (let x = cx - 10; x <= cx + 18; x++)
        for (let d = -4; d <= 4; d++) if (Math.abs(d) > 1 && dith(x, ys(x) + d, (0.06 + (0.26 * o.bright) / 100) * (1 - Math.abs(d) / 5))) g.rect(x, ys(x) + d, 1, 1, o.light);
    g.rect(cx - 21, cy, 3, 1, o.line);
    g.cut(cx - 18, cy - 3, 8, 7, o.line); g.rect(cx - 17, cy - 2, 6, 5, o.body); g.rect(cx - 15, cy, 2, 1, o.light || GREY);
    let prev = ys(cx - 10);
    for (let x = cx - 10; x <= cx + 18; x++) {
      const y = ys(x), lo = Math.min(y, prev), hi = Math.max(y, prev);
      g.rect(x, lo - 1, 1, hi - lo + 1, o.line); g.rect(x, lo + 1, 1, hi - lo + 1, o.line);
      g.rect(x, y, 1, 1, (x & 1) === 0 ? glow || DARK : o.body);
      prev = y;
    }
  } else { // a bulb: glass, neck, threads; rays that grow with brightness
    const gy = cy - 6;
    if (o.light) {
      const n = 1 + Math.round(o.bright / 50);
      for (const a of [180, 225, 270, 315, 0]) {
        const r = (a * Math.PI) / 180;
        for (let k = 0; k < n; k++) g.rect(Math.round(cx + Math.cos(r) * (15 + k * 4)) - 1, Math.round(gy + Math.sin(r) * (15 + k * 4)) - 1, 2, 2, o.light);
      }
    }
    g.disc(cx, gy, 11, o.line); g.disc(cx, gy, 10, glow || BLACK);
    const neck = [7, 6, 6, 5, 5, 4];
    for (let i = 0; i < 6; i++) { g.rect(cx - neck[i], gy + 8 + i, 2 * neck[i], 1, o.line); g.rect(cx - neck[i] + 1, gy + 8 + i, 2 * neck[i] - 2, 1, glow || BLACK); }
    for (let j = 0; j < 6; j++) g.rect(cx - (j & 1 ? 4 : 5), gy + 14 + j, j & 1 ? 8 : 10, 1, j & 1 ? o.body : o.line);
    g.rect(cx - 2, gy + 20, 4, 1, o.line);
    if (o.light) { g.rect(cx - 6, gy - 5, 2, 3, WHITE); g.rect(cx - 4, gy - 7, 2, 1, WHITE); }
  }
}

// About 18 x 16 px.
function lampSmall(g: G, kind: number, cx: number, cy: number, o: LampLook) {
  const glow = o.light ? lampLit(o.light, o.bright) : DARK;
  if (kind === 1) { g.rect(cx - 8, cy + 6, 10, 2, o.line); g.rect(cx - 6, cy - 5, 2, 11, o.line); g.rect(cx - 6, cy - 8, 15, 3, o.line); g.rect(cx - 3, cy - 5, 11, 1, glow); }
  else if (kind === 2) { g.rect(cx - 7, cy + 6, 7, 2, o.line); thick(g, cx - 4, cy + 5, cx - 1, cy - 5, 1, o.line); g.rect(cx - 2, cy - 7, 11, 2, o.line); g.rect(cx - 1, cy - 5, 9, 1, glow); }
  else if (kind === 3) { g.rect(cx - 9, cy - 1, 3, 3, o.line); for (let x = cx - 5; x <= cx + 8; x++) g.rect(x, cy + Math.round(2 * Math.sin(((x - cx + 5) / 13) * 2 * Math.PI)), 1, 1, (x & 1) === 0 ? glow : o.line); }
  else { g.disc(cx, cy - 2, 6, o.line); g.disc(cx, cy - 2, 5, o.light ? glow : BLACK); g.rect(cx - 3, cy + 4, 6, 1, o.line); g.rect(cx - 2, cy + 5, 4, 1, o.body); g.rect(cx - 3, cy + 6, 6, 1, o.line); g.rect(cx - 1, cy + 7, 2, 1, o.line); }
}

// A lamp on its own 48 x 40 tile (shown at a whole multiple, pixelated).
export function drawLampTile(c: HTMLCanvasElement, kind: number, look: LampLook) {
  c.width = 48;
  c.height = 40;
  const g = new G(c);
  g.rect(0, 0, 48, 40, BLACK);
  lampBig(g, kind, 24, 21, look);
}

// --- marks ---

const SPR_MI: Sprite = { w: 9, h: 9, rows: "........." + "........." + ".#####.#." + ".#.#.#.#." + ".#.#.#.#." + ".#.#.#.#." + ".#.#.#.#." + "........." + "........." };
const SPR_KORG: Sprite = { w: 27, h: 7, rows: "##..##..####..#####...#####" + "##.##..##..##.##..##.##...." + "####...##..##.##..##.##...." + "###....##..##.#####..##.###" + "####...##..##.####...##..##" + "##.##..##..##.##.##..##..##" + "##..##..####..##..##..#####" };
const SPR_ROLAND: Sprite = { w: 27, h: 7, rows: "####.......#..............#" + "#...#......#..............#" + "#...#..##..#..###.###...###" + "####..#..#.#.#..#.#..#.#..#" + "#.#...#..#.#.#..#.#..#.#..#" + "#..#..#..#.#.#..#.#..#.#..#" + "#...#..##..#..###.#..#..###" };
const makerLogo = (maker: string) => (maker.toUpperCase() === "KORG" ? SPR_KORG : maker.toUpperCase() === "ROLAND" ? SPR_ROLAND : null);
export const hasLogo = (maker: string) => makerLogo(maker) !== null;
const miBadge = (g: G, x: number, y: number) => {
  g.cut(x, y, 9, 9, MI_ORANGE);
  g.sprite(SPR_MI, x, y, WHITE);
};

// A maker's wordmark at 1x (27 x 7): the caller scales the canvas by whole pixels.
export function drawLogo(c: HTMLCanvasElement, maker: string, color = WHITE): boolean {
  const s = makerLogo(maker);
  if (!s) return false;
  c.width = s.w;
  c.height = s.h;
  const g = new G(c);
  g.ctx.clearRect(0, 0, s.w, s.h);
  g.sprite(s, 0, 0, color);
  return true;
}

export function drawMiBadge(c: HTMLCanvasElement) {
  c.width = c.height = 9;
  miBadge(new G(c), 0, 0);
}

// --- the screens (240 x 240), in a 300 x 300 canvas with the LED ring round them ---

function tri(g: G, x: number, y: number, dir: number, c: number) {
  for (let i = 0; i < 4; i++) g.rect(dir > 0 ? x + i : x + 3 - i, y + i, 1, 7 - i * 2, c);
}

function keycaps(g: G, legend: string[]) {
  const xs = [60, 100, 140, 180], ys = [146, 154, 154, 146];
  for (let i = 0; i < 4; i++) {
    const off = !legend[i], x = xs[i] - 13, yk = ys[i];
    g.rect(x, yk + 3, 26, 20, off ? 0x262626 : 0x8a8a8a);
    g.rect(x, yk, 26, 18, off ? 0x4a4a4a : 0xe4e4e4);
    g.text(`F${i + 1}`, xs[i], yk + 5, off ? GREY : BLACK);
    g.text(off ? "--" : legend[i], xs[i], yk + 27, off ? GREY : WHITE);
  }
}

function ring(g: G, frac: number, color: number) {
  for (let i = 0; i < 60; i++) {
    const a = -Math.PI / 2 + (i / 60) * 2 * Math.PI;
    g.cut(Math.round(150 + Math.cos(a) * 139) - 2, Math.round(150 + Math.sin(a) * 139) - 2, 5, 5, i < Math.ceil(frac * 60) ? color : 0x2a2a2a);
  }
}

function frame(c: HTMLCanvasElement) {
  c.width = c.height = 300;
  const g = new G(c);
  g.ctx.clearRect(0, 0, 300, 300);
  return g;
}

// HOME's list, `sel` in the middle (ui_extras.cpp draw_home_list).
export function drawHomeScreen(c: HTMLCanvasElement, lamps: Lamp[], sel: number) {
  const g = frame(c);
  const l = lamps[sel];
  const on = l && (l.flags & HomeFlag.ONLINE) && (l.flags & HomeFlag.KNOWN) && (l.flags & HomeFlag.ON);
  ring(g, on ? l.bright / 100 : 0, on ? lampLit(lampRgb(l), 70) : DARK);
  g.ctx.save();
  g.ctx.translate(30, 30);
  g.disc(120, 120, 120, BLACK);
  const tw = g.width("HOME"), x0 = Math.round(120 - (9 + 6 + tw) / 2);
  miBadge(g, x0, 30);
  g.text("HOME", x0 + 15, 31, WHITE, 8, "left");
  g.rect(56, 48, 128, 1, DARK);
  if (!l) {
    lampBig(g, 0, 120, 82, { line: DARK, body: BLACK, light: 0, bright: 0 });
    g.text("NO LAMPS", 120, 108, WHITE, 16);
    keycaps(g, ["", "", "", "MENU"]);
    g.ctx.restore();
    return;
  }
  g.ctx.save();
  g.ctx.beginPath();
  g.ctx.rect(16, 50, 208, 70);
  g.ctx.clip();
  for (let j = -2; j <= 2; j++) {
    const i = sel + j;
    if (i < 0 || i >= lamps.length || !lamps[i]) continue;
    const x = 120 + j * 66;
    if (Math.abs(x - 120) > 112) continue;
    if (j === 0) lampBig(g, lamps[i].kind, x, 84, lampLook(lamps[i], true));
    else lampSmall(g, lamps[i].kind, x, 84, lampLook(lamps[i], false));
  }
  g.ctx.restore();
  if (sel > 0) tri(g, 20, 81, -1, AMBER);
  if (sel < lamps.length - 1) tri(g, 216, 81, 1, AMBER);
  const online = (l.flags & HomeFlag.ONLINE) !== 0;
  g.text(l.name, 120, 108, online ? WHITE : GREY, l.name.length > 12 ? 8 : 16);
  if (on && !(l.flags & HomeFlag.FAILED)) {
    const w = g.width("ON") + 6 + 39, xs = Math.round(120 - w / 2);
    g.text("ON", xs, 129, WHITE, 8, "left");
    const n = Math.round(l.bright / 10);
    for (let i = 0; i < 10; i++) g.rect(xs + g.width("ON") + 6 + i * 4, 130, 3, 5, i < n ? lampLit(lampRgb(l), (i + 1) * 10) : DARK);
  } else {
    const st = !online ? "OFFLINE" : !(l.flags & HomeFlag.KNOWN) || l.flags & HomeFlag.FAILED ? "NO REPLY" : "OFF";
    g.text(st, 120, 129, l.flags & HomeFlag.FAILED ? AMBER : GREY);
  }
  const live = online && (l.flags & HomeFlag.KNOWN) !== 0;
  keycaps(g, [online ? (live ? "EDIT" : "") : "RETRY", live ? "POWER" : "", "SCAN", "MENU"]);
  g.ctx.restore();
}

// MIDI's parameter screen (ui_extras.cpp draw_midi): the maker and synth, the parameter and its
// value, the meter, the ports. `value` -1: not known yet.
export function drawMidiScreen(c: HTMLCanvasElement, s: SynthJson, pi: number, value: number, ports: { usb: boolean; trs: boolean; channel: number }) {
  const g = frame(c);
  const p = s.params[Math.max(0, Math.min(s.params.length - 1, pi))];
  if (!p) return;
  const max = paramMax(p), v = value < 0 ? -1 : Math.min(max, value);
  ring(g, v < 0 ? 0 : v / max, 0xb8902f);
  g.ctx.save();
  g.ctx.translate(30, 30);
  g.disc(120, 120, 120, BLACK);
  const logo = makerLogo(s.maker), nw = g.width(s.name), w = (logo ? logo.w + 6 : 0) + nw, x0 = Math.round(120 - w / 2);
  if (logo) g.sprite(logo, x0, 31, WHITE);
  g.text(s.name, x0 + (logo ? logo.w + 6 : 0), 31, WHITE, 8, "left");
  g.rect(56, 48, 128, 1, DARK);
  g.text(p.group, 120, 56, GREY);
  g.text(p.name, 120, 67, WHITE, 16);
  const vt = valueText(p, v), vw = g.text(vt, 120, 86, AMBER, vt.length > 5 ? 16 : 24);
  tri(g, 120 - vw / 2 - 10, 92, -1, AMBER);
  tri(g, 120 + vw / 2 + 6, 92, 1, AMBER);
  if (p.options) {
    const n = p.options.length, cw = Math.floor((100 - (n - 1) * 3) / n), total = n * cw + (n - 1) * 3, bx = Math.round(120 - total / 2);
    for (let i = 0; i < n; i++) g.rect(bx + i * (cw + 3), 116, cw, 5, i === v ? AMBER : DARK);
  } else {
    const n = v < 0 ? 0 : Math.round((v / max) * 40);
    for (let i = 0; i < 40; i++) g.rect(70 + i * 2.5, 116, 2, 5, i < n ? WHITE : DARK);
  }
  const ch = `CH ${String(ports.channel).padStart(2, "0")}`;
  const parts: [string, number][] = [[ch, WHITE], ["USB", ports.usb ? WHITE : GREY], ["TRS", ports.trs ? WHITE : GREY]];
  const total = parts.reduce((a, [t]) => a + g.width(t), 0) + 2 * 14;
  let x = Math.round(120 - total / 2);
  for (const [t, col] of parts) x += g.text(t, x, 129, col, 8, "left") + 14;
  keycaps(g, ["NEXT", "PROG-", "PROG+", "MENU"]);
  g.ctx.restore();
}

export const capsList = (caps: number) => [caps & HomeCap.BRIGHT && "Brightness", caps & HomeCap.TEMP && "White", caps & HomeCap.COLOR && "Colour"].filter(Boolean) as string[];
