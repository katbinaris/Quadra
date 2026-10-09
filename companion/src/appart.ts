// A sharper picture for a profile's icon than the knob's 48 pixels, where this computer has
// one: the picture an icon was imported from (kept here when it is imported), or artwork
// bundled for a built-in profile (src/assets/apps/<profile id>.png, .svg or .webp). The knob
// only ever holds the 48 and 24 pixel versions, so everything here is by the knob's icon: an
// imported picture is shown only while the knob still has the icon made from it.

import { b64ToBytes, rgb565ToImage } from "./profile";

const BUNDLED = Object.fromEntries(
  Object.entries(import.meta.glob("./assets/apps/*.{png,svg,webp}", { eager: true, query: "?url", import: "default" }) as Record<string, string>).map(([path, url]) => [
    path.replace(/^.*\/|\.[^.]+$/g, ""),
    url,
  ]),
);

const KEY = "quadra.icon.";
const SIZE = 256; // the kept picture's longer side
const hashes = new WeakMap<ImageData, string>();

// FNV-1a over the icon's pixels: tells one icon from another, nothing more.
function hash(img: ImageData): string {
  let h = hashes.get(img);
  if (h === undefined) {
    let x = 0x811c9dc5;
    for (let i = 0; i < img.data.length; i++) x = Math.imul(x ^ img.data[i], 0x01000193);
    h = (x >>> 0).toString(16);
    hashes.set(img, h);
  }
  return h;
}

// The picture for a profile's icon, or null: the knob's pixels are all there is.
export function artFor(id: string, icon: ImageData | null, builtin: boolean): string | null {
  if (icon) {
    try {
      const kept = JSON.parse(localStorage.getItem(KEY + id) ?? "null") as { h: string; url: string } | null;
      if (kept?.h === hash(icon)) return kept.url;
    } catch {
      // no storage here, or not ours: the bundled one, or the pixels
    }
  }
  return builtin ? (BUNDLED[id] ?? null) : null;
}

// An icon was imported from `src`: keep the picture, by the 48 pixel icon made from it.
export function keepOriginal(id: string, src: CanvasImageSource & { width: number; height: number }, icon48: string) {
  const k = Math.min(1, SIZE / Math.max(src.width, src.height));
  const c = document.createElement("canvas");
  c.width = Math.max(1, Math.round(src.width * k));
  c.height = Math.max(1, Math.round(src.height * k));
  const ctx = c.getContext("2d")!;
  ctx.imageSmoothingQuality = "high";
  ctx.drawImage(src, 0, 0, c.width, c.height);
  try {
    localStorage.setItem(KEY + id, JSON.stringify({ h: hash(rgb565ToImage(b64ToBytes(icon48), 48)), url: c.toDataURL("image/png") }));
  } catch {
    // storage full or off: the knob's pixels show, as before
  }
}
