// DEVICE: the computer it talks to (MAC / PC), how it starts (HID / SERIAL), the firmware;
// WiFi: the network, and this app paired to reach the knob without a cable; and Backup: the
// knob's setup to a file and back (device/Backup.tsx).

import { useSignal } from "@preact/signals";
import { useEffect, useRef } from "preact/hooks";
import { isWindows, thisComputer } from "../platform";
import { Boot, EXT_CONTROLS_VERSION, EXT_NET_VERSION, EXT_WIFI_LINK_VERSION, Host, NET_STATE, NetState, Set } from "../proto";
import { device, use } from "../store";
import { Box, Card, Confirm, PageHead, Row, SubTabs, Text } from "../ui/controls";
import { titleCase } from "../ui/shell";
import { BackupTab } from "./device/Backup";

export function DevicePage(p: { tab: "general" | "wifi" | "backup" }) {
  use("conn");
  const wifi = (device.ext ?? 0) >= EXT_NET_VERSION;
  return (
    <>
      <PageHead title="Device" hint="The computer it talks to, how it starts, and what runs on it" />
      <SubTabs
        tabs={[
          { value: "general" as const, label: "General", href: "#/device/general" },
          ...(wifi ? [{ value: "wifi" as const, label: "Wi-Fi", href: "#/device/wifi" }] : []),
          { value: "backup" as const, label: "Backup", href: "#/device/backup" },
        ]}
        value={p.tab}
      />
      {p.tab === "wifi" && wifi ? <WifiTab /> : p.tab === "backup" ? <BackupTab /> : <GeneralTab />}
    </>
  );
}

function GeneralTab() {
  use("settings", "conn", "profiles");
  const s = device.settings!;
  const bit = (id: number) => ((s.dirty >> id) & 1) === 1;
  const h = device.hello;
  const link = device.kind === "tauri" ? "USB (app)" : device.kind === "wifi" ? "Wi-Fi (app)" : "WebHID";
  return (
    <>
      <div class="grid2">
        <Box title="Computer" note="Shortcut bindings">
          <div class="cards">
            <Card left on={s.host === Host.MAC} dirty={s.host === Host.MAC && bit(Set.HOST)} onClick={() => device.set(Set.HOST, Host.MAC)}>
              <span class="nm">Mac</span>
              <span class="sub">Shortcuts sent as written</span>
            </Card>
            <Card left on={s.host === Host.PC} dirty={s.host === Host.PC && bit(Set.HOST)} onClick={() => device.set(Set.HOST, Host.PC)}>
              <span class="nm">PC</span>
              <span class="sub">⌘ is sent as Ctrl</span>
            </Card>
          </div>
          {isWindows && s.host === Host.MAC && <span class="hint amber">This computer runs Windows: choose PC, or ⌘ shortcuts arrive as the Windows key.</span>}
        </Box>
        <Box title="Starts in" note="After the next restart">
          <div class="cards">
            <Card left on={s.boot === Boot.HID} dirty={s.boot === Boot.HID && bit(Set.BOOT)} onClick={() => device.set(Set.BOOT, Boot.HID)}>
              <span class="nm">HID</span>
              <span class="sub">Normal use</span>
            </Card>
            <Card left on={s.boot === Boot.SERIAL} dirty={s.boot === Boot.SERIAL && bit(Set.BOOT)} onClick={() => device.set(Set.BOOT, Boot.SERIAL)}>
              <span class="nm">Serial</span>
              <span class="sub">For flashing; this app can't reach it then</span>
            </Card>
          </div>
          {s.boot === Boot.SERIAL && <span class="hint amber">In serial mode the knob has no HID: this app can't reach it until it starts in HID again.</span>}
        </Box>
      </div>
      <Box title="Firmware" note="On the knob">
        <div class="kvgrid">
          <div class="kv"><span>Version</span><span>{h?.version ?? "—"}</span></div>
          <div class="kv"><span>Built</span><span>{h ? titleCase(h.date) : "—"}</span></div>
          <div class="kv"><span>Protocol</span><span>{h ? String(h.proto) : "—"}</span></div>
          <div class="kv"><span>Extensions</span><span>{device.ext ? `v${device.ext}` : device.ext === 0 ? "None" : "—"}</span></div>
          <div class="kv"><span>App profiles</span><span>{h ? String(h.profileCount) : "—"}</span></div>
          <div class="kv"><span>Link</span><span>{link}</span></div>
        </div>
      </Box>
    </>
  );
}

function WifiTab() {
  use("net", "conn");
  const n = device.net;
  const edited = useRef(false); // typed here: the knob's status doesn't overwrite the field
  const ssid = useRef(n?.ssid ?? "");
  const pass = useSignal("");
  const passRef = useRef<HTMLInputElement>(null);
  useEffect(() => {
    if (!edited.current && n) ssid.current = n.ssid;
  }, [n?.ssid]);
  const usb = device.kind === "tauri", overWifi = device.kind === "wifi";
  const state = !n ? "…" : !n.on ? "Off" : titleCase(NET_STATE[n.state] ?? "?");
  const connect = () => {
    const name = ssid.current.trim();
    if (name && (name !== n?.ssid || pass.value)) void device.setWifi(true, name, pass.value);
    else void device.setWifi(true);
    pass.value = "";
    if (passRef.current) passRef.current.value = "";
    edited.current = false;
  };
  const p = device.paired;
  const canPair = (device.ext ?? 0) >= EXT_WIFI_LINK_VERSION && device.kind !== "webhid";
  return (
    <>
      <Box title="Network" note={overWifi ? "Connected over Wi-Fi: the network changes over USB" : "Set over USB · the password stays on the knob"}>
        {n && (
          <div class="status">
            <span class="line" style={{ gap: "8px" }}>
              <i class={n.on && n.state === NetState.CONNECTED ? "gdot" : "gdot off"} />
              <b>{n.on && n.state === NetState.CONNECTED ? `Connected to ${n.ssid}` : state}</b>
            </span>
            {n.on && n.state === NetState.CONNECTED && (
              <>
                <span class="mono hint">{n.ip}</span>
                <span class="mono hint">{n.rssi} dBm</span>
                <span class="mono hint">{n.host}.local</span>
                {n.timeSet && <span class="tag">Clock set</span>}
              </>
            )}
          </div>
        )}
        {!overWifi && (
          <>
            <Row label="Network name" for="wifi-ssid">
              <Text id="wifi-ssid" value={n?.ssid ?? ""} max={32} upper={false} placeholder="Network name" on={(v) => ((ssid.current = v), (edited.current = true))} />
            </Row>
            <Row label="Password" for="wifi-pass">
              <input ref={passRef} id="wifi-pass" class="field" type="password" maxLength={63} autoComplete="off" placeholder={n?.ssid ? "Unchanged" : "None = an open network"} onInput={(e) => ((pass.value = e.currentTarget.value), (edited.current = true))} onKeyDown={(e) => e.stopPropagation()} />
            </Row>
            <Row label="">
              <button class="btn primary" onClick={connect}>
                Connect
              </button>
              <button class="btn ghost" disabled={!n?.on} onClick={() => void device.setWifi(false)}>
                Turn Wi-Fi off
              </button>
            </Row>
          </>
        )}
      </Box>
      {canPair && (
        <Box title="This app over Wi-Fi" note="Pair once over USB, then no cable is needed">
          <div class="line">
            {p ? <span class="badge ok">Paired</span> : <span class="badge">Not paired</span>}
            {p && <span class="mono">{p.host ? `${p.host}.local` : p.ip}</span>}
            <span class="spacer" />
            <button class="btn" disabled={!usb || n?.state !== NetState.CONNECTED || !n.host} onClick={() => void device.pairWifi(false)}>
              {p ? "Pair again" : "Pair this app"}
            </button>
            <Confirm label="New key" ask="Sure? Unpairs the others" class="ghost" on={() => void device.pairWifi(true)} />
            <button class="btn ghost" disabled={!p} onClick={() => device.forgetWifi()}>
              Forget
            </button>
          </div>
          <span class="hint">{overWifi ? "Connected over Wi-Fi. The network and the key change over USB." : "Once paired, this app reaches the knob over Wi-Fi when no cable is in. A new key unpairs every other computer."}</span>
          {(device.ext ?? 0) >= EXT_CONTROLS_VERSION && (
            <>
              <div class="line">
                <b>The knob's controls</b>
                {device.controls === "on" ? <span class="badge ok">On</span> : device.controls === "needs-permission" ? <span class="badge">Needs permission</span> : <span class="badge">Over USB now</span>}
                <span class="spacer" />
                {device.controls !== "on" && (
                  <button class="btn" onClick={() => void device.allowControls()}>
                    Allow
                  </button>
                )}
              </div>
              <span class="hint">
                {isWindows
                  ? `With no cable in, turning the knob and pressing F1–F4 type and scroll on ${thisComputer} over Wi-Fi. Windows keeps them away from apps running as administrator.`
                  : `With no cable in, turning the knob and pressing F1–F4 type and scroll on ${thisComputer} over Wi-Fi. macOS asks once to allow Quadra under Accessibility.`}
              </span>
            </>
          )}
        </Box>
      )}
    </>
  );
}
