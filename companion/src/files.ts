// Export and import: a JSON file out through a Save dialog, one in through an Open dialog (the app:
// tauri-plugin-dialog, then files.rs reads or writes it; the web page and ?demo: a download and a
// file input). What a file holds is told by its shape (kindOf).

import { isTauri } from "./transport";

const FILTERS = [{ name: "Quadra files (JSON)", extensions: ["json"] }];

export interface Opened {
  name: string; // the file's name, for messages
  data: unknown; // parsed JSON
}

export type FileKind = "backup" | "profile" | "synth" | null;

// A backup (backup.ts), an app profile (profile.ts) or a synth profile (synth.ts).
export function kindOf(data: unknown): FileKind {
  if (!data || typeof data !== "object" || Array.isArray(data)) return null;
  const o = data as Record<string, unknown>;
  if (o.kind === "quadra-backup") return "backup";
  if (Array.isArray(o.params)) return "synth";
  if (typeof o.id === "string" && Array.isArray(o.legend)) return "profile";
  return null;
}

// Null: cancelled. Otherwise where it went ("" in a browser: its Downloads).
export async function saveJson(name: string, data: unknown): Promise<string | null> {
  const text = JSON.stringify(data, null, 2) + "\n";
  if (isTauri()) {
    const { save } = await import("@tauri-apps/plugin-dialog");
    const path = await save({ defaultPath: name, filters: FILTERS });
    if (!path) return null;
    const { invoke } = await import("@tauri-apps/api/core");
    await invoke("write_text", { path, text });
    return path;
  }
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([text], { type: "application/json" }));
  a.download = name;
  a.click();
  window.setTimeout(() => URL.revokeObjectURL(a.href), 10_000);
  return "";
}

function parse(name: string, text: string): Opened {
  try {
    return { name, data: JSON.parse(text) };
  } catch {
    throw new Error(`${name} isn't a JSON file`);
  }
}

const baseName = (path: string) => path.split(/[\\/]/).pop() ?? path;

export async function readPath(path: string): Promise<Opened> {
  const { invoke } = await import("@tauri-apps/api/core");
  return parse(baseName(path), await invoke<string>("read_text", { path }));
}

export async function readFile(f: File): Promise<Opened> {
  return parse(f.name, await f.text());
}

// Null: cancelled.
export async function openJson(): Promise<Opened | null> {
  if (isTauri()) {
    const { open } = await import("@tauri-apps/plugin-dialog");
    const path = await open({ multiple: false, directory: false, filters: FILTERS });
    return typeof path === "string" ? readPath(path) : null;
  }
  return new Promise((resolve, reject) => {
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".json,application/json";
    input.onchange = () => {
      const f = input.files?.[0];
      if (f) readFile(f).then(resolve, reject);
      else resolve(null);
    };
    input.oncancel = () => resolve(null);
    input.click();
  });
}

// A file dropped on the window: in the app the webview reports paths, in a browser the drop
// event has the file. `take` gets each JSON file in turn.
export async function onDropFiles(take: (f: Opened) => void, fail: (msg: string) => void): Promise<void> {
  const run = (p: Promise<Opened>) => p.then(take, (e) => fail(e instanceof Error ? e.message : String(e)));
  if (isTauri()) {
    const { getCurrentWebview } = await import("@tauri-apps/api/webview");
    await getCurrentWebview().onDragDropEvent((e) => {
      if (e.payload.type !== "drop") return;
      for (const p of e.payload.paths) if (/\.json$/i.test(p)) void run(readPath(p));
    });
    return;
  }
  window.addEventListener("dragover", (e) => {
    if (e.dataTransfer?.types.includes("Files")) e.preventDefault();
  });
  window.addEventListener("drop", (e) => {
    const files = Array.from(e.dataTransfer?.files ?? []).filter((f) => /\.json$/i.test(f.name));
    if (!files.length) return;
    e.preventDefault();
    for (const f of files) void run(readFile(f));
  });
}

// "Saved as …" for a message, or nothing in a browser (the download shows itself).
export const savedText = (path: string) => (path ? `Saved as ${path}` : "Downloaded");
