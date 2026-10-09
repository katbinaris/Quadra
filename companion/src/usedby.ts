// Every app profile's JSON, read from the knob once and kept, for Haptics' "Used by": which
// inputs of which profiles turn with a haptic profile. A profile is read again when its entry
// in the list changes (saved, reverted, renamed) or after an edit here was sent (forgetUsage).

import { signal } from "@preact/signals";
import type { ProfileJson } from "./profile";
import { device } from "./store";

const cache = new Map<string, { key: string; json: ProfileJson }>();
let running = false;
let again = false;

export const usageRev = signal(0); // bumped when a profile is in (or gone): readers re-render
export const usageLoading = signal(false);

export function usage(id: string): ProfileJson | undefined {
  return cache.get(id)?.json;
}

export function forgetUsage(id: string) {
  cache.delete(id);
}

export async function loadUsage() {
  if (running) {
    again = true;
    return;
  }
  running = true;
  usageLoading.value = true;
  try {
    do {
      again = false;
      const list = device.profiles.filter(Boolean);
      for (const id of [...cache.keys()]) if (!list.some((p) => p.id === id)) cache.delete(id);
      for (const p of list) {
        const key = `${p.index}|${p.flags}|${p.name}`;
        if (cache.get(p.id)?.key === key) continue;
        try {
          cache.set(p.id, { key, json: await device.readProfile(p.index) });
          usageRev.value++;
        } catch {
          // not connected any more, or the list moved under us: the next call reads it
        }
      }
    } while (again);
  } finally {
    running = false;
    usageLoading.value = false;
    usageRev.value++;
  }
}
