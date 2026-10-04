// The window: the sidebar (the knob's picture, then where to go), the top bar (where you are,
// and one SAVE for everything not stored on the knob yet), and the page.

import { useSignal } from "@preact/signals";
import { useEffect, useRef } from "preact/hooks";
import deviceUrl from "../assets/device.png";
import { HidType, ProfileFlag } from "../proto";
import { connected, device, href, inUse, revertAll, revertProfile, revertSettings, route, saveAll, saveError, saving, unsaved, unsavedCount, use, type Route } from "../store";
import { isTauri } from "../transport";
import { cls } from "./controls";
import { drawMainScreen } from "./draw";
import { ProfileIcon } from "./icons";
import { createProfile, MAX_PROFILES } from "../profiles";

const MODE_NAMES: Record<number, string> = { [HidType.APP]: "App", [HidType.HOME]: "Home", [HidType.MOUSE]: "Mouse", [HidType.KEYBOARD]: "Keys", [HidType.MIDI]: "MIDI" };

export function Shell(p: { children: preact.ComponentChildren }) {
  const on = connected.value;
  return (
    <div class={cls("app", isTauri() ? "tauri" : "web")}>
      <Sidebar />
      <div class="col">
        <Topbar />
        <main class="page">{on ? p.children : <NotConnected />}</main>
      </div>
    </div>
  );
}

// --- sidebar ---

function Sidebar() {
  use("conn", "settings", "profiles");
  const r = route.value;
  const s = device.settings;
  const on = connected.value;
  const dirty = unsaved.value;
  const changed = (name: string) => dirty.settings.includes(name);
  const live = (id: string) => dirty.profiles.some((p) => p.id === id);
  const using = inUse.value;
  const nav = (to: Route, label: string, icon: preact.ComponentChildren, extra?: preact.ComponentChildren, active = r.page === to.page) => (
    <a class={cls("nav", active && "on")} href={href(to)} aria-current={active ? "page" : undefined}>
      {icon}
      <span class="nl">{label}</span>
      {extra}
    </a>
  );
  return (
    <nav class="sb" aria-label="Quadra">
      <div class="drag" data-tauri-drag-region />
      <KnobPicture />
      {on && (
        <div class="navs">
          <div class="grp">
            <div class="gh">Knob</div>
            {nav({ page: "mode" }, "Mode", <Icon d="M8 2v4" circle />, changed("Mode") ? <i class="chg" /> : <span class="val">{MODE_NAMES[s!.hidType] ?? ""}</span>)}
            {nav({ page: "haptics" }, "Haptics", <Icon d="M1 8c2-5 4-5 6 0s4 5 6 0" />, changed("Haptics") && <i class="chg" />)}
          </div>
          <div class="grp">
            <div class="gh">App profiles</div>
            {device.profiles.filter(Boolean).map((p) =>
              nav(
                { page: "profile", id: p.id, tab: "general", input: "knob" },
                titleCase(p.name),
                <ProfileIcon icon={p.icon} name={p.name} size={18} ring={p.index === using} />,
                live(p.id) ? <i class="chg" /> : p.flags & ProfileFlag.BUILTIN ? null : <span class="val">Yours</span>,
                r.page === "profile" && r.id === p.id,
              ),
            )}
            <button type="button" class="nav" disabled={device.profiles.length >= MAX_PROFILES} onClick={() => void createProfile()}>
              <Icon d="M8 3v10M3 8h10" />
              <span class="nl">New profile</span>
            </button>
          </div>
          <div class="grp">
            <div class="gh">Setup</div>
            {nav({ page: "look", tab: "lights" }, "Look", <Icon d="M8 1v2M8 13v2M1 8h2M13 8h2M3 3l1.5 1.5M11.5 11.5L13 13M3 13l1.5-1.5M11.5 4.5L13 3" circle small />, (changed("Lights") || changed("Screen")) && <i class="chg" />)}
            {nav({ page: "device", tab: "general" }, "Device", <Icon d="M5 2h6a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2z" />, changed("Device") && <i class="chg" />)}
            {nav({ page: "sys" }, "System info", <Icon d="M3 13V8M8 13V3M13 13V6" />)}
          </div>
        </div>
      )}
    </nav>
  );
}

function Icon(p: { d: string; circle?: boolean; small?: boolean }) {
  return (
    <svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" aria-hidden="true">
      {p.circle && <circle cx="8" cy="8" r={p.small ? 3 : 6} />}
      <path d={p.d} />
    </svg>
  );
}

// Names the knob shows in capitals, as their makers write them.
const NAMES: Record<string, string> = { AUTOCAD: "AutoCAD" };
export const titleCase = (s: string) => NAMES[s] ?? s.toLowerCase().replace(/(^|[\s/-])\S/g, (c) => c.toUpperCase());

// The knob: its render, and on its glass what its main screen shows (drawn here from what the
// knob reports -- not streamed).
function KnobPicture() {
  use("conn", "settings", "profiles");
  const canvas = useRef<HTMLCanvasElement>(null);
  const on = connected.value;
  const s = device.settings;
  const p = s && s.hidType === HidType.APP ? device.profiles[s.profile] : undefined;
  const info = s ? { icon: p?.icon ?? null, name: p ? p.name : (MODE_NAMES[s.hidType] ?? "").toUpperCase(), feel: s.feel, legend: p ? p.legend : s.hidType === HidType.HOME ? ["EDIT", "POWER", "SCAN", "MENU"] : ["SEL", "", "BACK", "MENU"] } : null;
  const key = info ? `${info.name}|${info.feel}|${info.legend.join()}|${!!info.icon}` : "";
  useEffect(() => {
    if (!canvas.current || !info) return;
    const c = canvas.current;
    void document.fonts.load("8px Silkscreen").then(() => drawMainScreen(c, info));
  }, [key]);
  const status = on ? `${device.kind === "wifi" ? "Wi-Fi" : "USB"} · firmware ${device.hello?.version ?? ""}` : device.status === "needs-permission" ? "Not connected" : device.status === "unsupported" ? "No USB access" : "Looking for it…";
  return (
    <div class="me">
      <div class={cls("devpic", !on && "off")}>
        <img src={deviceUrl} alt="" draggable={false} />
        {on && info && <canvas ref={canvas} class="glass" width={240} height={240} />}
      </div>
      <div class="who">
        <b>{device.net?.host ? device.net.host.replace(/^quadra-/, "Quadra ") : "Quadra"}</b>
        <span>
          <i class={cls("gdot", !on && "off")} />
          {status}
        </span>
      </div>
    </div>
  );
}

// --- top bar ---

const CRUMBS: Record<Route["page"], [string, string]> = {
  mode: ["Knob", "Mode"],
  haptics: ["Knob", "Haptics"],
  profile: ["App profiles", ""],
  look: ["Setup", "Look"],
  device: ["Setup", "Device"],
  sys: ["Setup", "System info"],
};

function Topbar() {
  use("profiles");
  const r = route.value;
  const open = useSignal(false);
  const n = unsavedCount.value;
  const on = connected.value;
  let [section, page] = CRUMBS[r.page];
  if (r.page === "profile") page = titleCase(device.profiles.find((p) => p?.id === r.id)?.name ?? r.id);
  useEffect(() => {
    if (n === 0) open.value = false;
  }, [n]);
  const u = unsaved.value;
  return (
    <header class="tb" data-tauri-drag-region>
      <div class="crumb" data-tauri-drag-region>
        {on ? (
          <>
            <span>{section}</span>
            <span>›</span>
            <b>{page}</b>
          </>
        ) : (
          <span>Quadra</span>
        )}
      </div>
      <span class="spacer" data-tauri-drag-region />
      {saveError.value && <span class="err" role="alert">{saveError.value}</span>}
      {on &&
        (n > 0 ? (
          <>
            <button class="pend" aria-expanded={open.value} onClick={() => (open.value = !open.value)}>
              <i />
              {n} unsaved: {[...u.settings, ...u.profiles.map((p) => titleCase(p.name))].join(", ")}
            </button>
            <button class="btn ghost" disabled={saving.value} onClick={() => void revertAll()}>
              Revert all
            </button>
            <button class="btn primary" disabled={saving.value} onClick={() => void saveAll()}>
              {saving.value ? "Saving…" : "Save to knob"}
            </button>
          </>
        ) : (
          <>
            <span class="saved">
              <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" aria-hidden="true">
                <path d="M3 8.5l3 3 7-7" />
              </svg>
              Everything is saved on the knob
            </span>
            <button class="btn" disabled>
              Save to knob
            </button>
          </>
        ))}
      {open.value && n > 0 && (
        <div class="pop" role="dialog" aria-label="Not saved yet">
          <b>Not saved yet</b>
          <span class="hint">Live on the knob now. Saving keeps it after a restart; Revert goes back to what is saved.</span>
          {u.settings.length > 0 && (
            <div class="popi">
              <span class="what">
                <b>Settings</b>
                <span>{u.settings.join(", ")} · revert together</span>
              </span>
              <button class="btn sm ghost" disabled={saving.value} onClick={() => void revertSettings()}>
                Revert
              </button>
            </div>
          )}
          {u.profiles.map((p) => (
            <div class="popi">
              <span class="what">
                <b>{titleCase(p.name)}</b>
                <span>App profile</span>
              </span>
              <button class="btn sm ghost" disabled={saving.value} onClick={() => void revertProfile(p.id)}>
                Revert
              </button>
            </div>
          ))}
          <div class="line" style={{ justifyContent: "flex-end", paddingTop: "4px" }}>
            <button class="btn ghost" disabled={saving.value} onClick={() => void revertAll()}>
              Revert all
            </button>
            <button class="btn primary" disabled={saving.value} onClick={() => void saveAll()}>
              Save to knob
            </button>
          </div>
        </div>
      )}
    </header>
  );
}

// --- not connected ---

function NotConnected() {
  use("conn", "net");
  const st = device.status;
  return (
    <div class="empty">
      <svg width="56" height="56" viewBox="0 0 56 56" fill="none" stroke="var(--faint)" stroke-width="2" aria-hidden="true">
        <rect x="10" y="4" width="36" height="48" rx="6" />
        <circle cx="28" cy="22" r="10" />
        <path d="M16 42h6M25 42h6M34 42h6" />
      </svg>
      {st === "unsupported" ? (
        <>
          <span class="title">No USB access here</span>
          <span class="hint">Open this page in Chrome or Edge, or use the Quadra app.</span>
        </>
      ) : st === "needs-permission" ? (
        <>
          <span class="title">Connect your Quadra</span>
          <span class="hint">The browser asks once which device this page may use.</span>
          <button class="btn primary" onClick={() => void device.requestAccess()}>
            Connect
          </button>
        </>
      ) : (
        <>
          <span class="title">Looking for your Quadra</span>
          <span class="hint">Plug it in with USB. It has to be in HID mode, the normal one.{device.paired ? " Paired over Wi-Fi: it is found by itself when no cable is in." : ""}</span>
          <div class="line" style={{ justifyContent: "center" }}>
            <span class="saved">
              <i class="gdot off" />
              Searching…
            </span>
            {device.paired && (
              <span class="saved">
                <i class="gdot off" />
                {device.paired.host ? `${device.paired.host}.local` : device.paired.ip}
              </span>
            )}
          </div>
        </>
      )}
    </div>
  );
}
