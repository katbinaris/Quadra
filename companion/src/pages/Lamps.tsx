// LAMPS: HOME's lamps as the knob sees them, asked every second (EXT_TAG_HOME), drawn with the
// knob's own lamp art; and what can change about them here: the name, the icon, the order, or
// the lamp itself (removed). The knob talks to the lamps itself; the app never does.

import { useSignal } from "@preact/signals";
import { useEffect, useRef } from "preact/hooks";
import { KIND_NAMES, PROTO_NAMES, readDevices, type XFile } from "../home";
import { EXT_HOME_VERSION, EXT_SYNTH_VERSION, HOME_MAX_LAMPS, HOME_NAME_MAX, HomeEdit, HomeFlag, NetState, type Lamp } from "../proto";
import { device, href, use } from "../store";
import { isTauri } from "../transport";
import { Box, cls, Confirm, PageHead, Text } from "../ui/controls";
import { capsList, drawHomeScreen, drawLampTile, lampLook, LIT, UNLIT } from "../ui/knobart";

// The extractor's file, for each lamp's model (the knob doesn't keep it). Read once per visit.
const models = new Map<number, string>();
async function loadModels() {
  if (!isTauri() && !new URLSearchParams(location.search).has("demo")) return;
  try {
    const f: XFile | null = await readDevices();
    for (const d of f?.devices ?? []) models.set(Number(d.did), d.model);
  } catch {
    // no file, or no access: the models just don't show
  }
}

export const lampState = (l: Lamp) =>
  !(l.flags & HomeFlag.ONLINE) ? "Offline" : !(l.flags & HomeFlag.KNOWN) ? "No reply" : l.flags & HomeFlag.ON ? "On" : "Off";

// A lamp drawn by the knob's art: as it is now (`lamp`), or lit / unlit for a choice of icon.
export function LampTile(p: { lamp?: Lamp; chosen?: boolean; scale?: number; kind?: number; lit?: boolean }) {
  const ref = useRef<HTMLCanvasElement>(null);
  const k = p.kind ?? p.lamp?.kind ?? 0;
  const look = p.lit ? LIT : p.lamp ? lampLook(p.lamp, p.chosen ?? true) : UNLIT;
  const key = `${k}|${JSON.stringify(look)}`;
  useEffect(() => {
    if (ref.current) drawLampTile(ref.current, k, look);
  }, [key]);
  const z = p.scale ?? 2;
  return <canvas ref={ref} class="lamptile" width={48} height={40} style={{ width: `${48 * z}px`, height: `${40 * z}px` }} aria-hidden="true" />;
}

function KnobHome(p: { lamps: Lamp[]; sel: number }) {
  const ref = useRef<HTMLCanvasElement>(null);
  const key = JSON.stringify(p.lamps) + p.sel;
  useEffect(() => {
    const c = ref.current;
    if (c) void document.fonts.load("8px Silkscreen").then(() => drawHomeScreen(c, p.lamps, p.sel));
  }, [key]);
  return <canvas ref={ref} class="knobring" width={300} height={300} aria-label="The knob's HOME screen" />;
}

export function LampsPage() {
  use("lamps", "net", "conn");
  const sel = useSignal<number | null>(null); // the device id of the lamp picked here
  const busy = useSignal<string | null>(null);
  const err = useSignal<string | null>(null);
  const tick = useSignal(0);
  useEffect(() => {
    void loadModels().then(() => tick.value++);
  }, []);
  void tick.value;
  if ((device.ext ?? 0) < EXT_HOME_VERSION)
    return (
      <>
        <PageHead title="Lamps" />
        <Box>
          <span class="hint">This needs firmware with HOME (extensions v11 and later).</span>
        </Box>
      </>
    );
  const lamps = device.lamps.filter(Boolean);
  const editable = (device.ext ?? 0) >= EXT_SYNTH_VERSION; // the edits came with v12
  const i = Math.max(0, lamps.findIndex((l) => l.did === sel.value));
  const l = lamps[i];
  const online = lamps.filter((x) => x.flags & HomeFlag.ONLINE).length;
  const net = device.net;
  const edit = async (what: number, value: number | string, label: string) => {
    if (!l) return;
    busy.value = label;
    err.value = null;
    try {
      await device.homeEdit(l.slot, l.did, what, value);
    } catch (e) {
      err.value = e instanceof Error ? e.message : String(e);
    } finally {
      busy.value = null;
    }
  };
  const head = (
    <PageHead title="Lamps" hint="Your Xiaomi lamps on the knob. The knob finds them and talks to them itself; this page shows what it sees.">
      <a class="btn" href={href({ page: "lamps", sub: "import" })}>
        Import from Xiaomi…
      </a>
    </PageHead>
  );
  if (device.lampCount === null) return <>{head}<Box><span class="hint">Asking the knob…</span></Box></>;
  if (lamps.length === 0)
    return (
      <>
        {head}
        <div class="empty">
          <span class="title">No lamps on the knob yet</span>
          <span class="hint">They come from your Xiaomi account, once: the import finds them on your network and sends them and their keys to the knob over USB.</span>
          <a class="btn primary" href={href({ page: "lamps", sub: "import" })}>
            Import from Xiaomi…
          </a>
        </div>
      </>
    );
  return (
    <>
      {head}
      <div class="status">
        <span>
          <i class={cls("gdot", net?.state !== NetState.CONNECTED && "off")} /> <b>Wi-Fi</b> {net?.state === NetState.CONNECTED ? net.ssid : "not connected"}
        </span>
        <span>
          <b>{lamps.length}</b> of {HOME_MAX_LAMPS} lamps on the knob
        </span>
        <span>
          <b>{online}</b> answering
        </span>
        <span class="hint">The knob looks for them in HOME mode, again every 30 s, and when you press F3 there</span>
      </div>
      <div class="lamps-split">
        <div class="lgrid">
          {lamps.map((x) => {
            const st = lampState(x), on = st === "On";
            const rgb = `rgb(${x.rgb.join(",")})`;
            return (
              <button type="button" class={cls("lcard", x === l && "on")} onClick={() => (sel.value = x.did)}>
                <LampTile lamp={x} chosen={x === l} />
                <span class="meta">
                  <span class="nm">{x.name}</span>
                  <span>
                    {KIND_NAMES[x.kind] ?? "Bulb"}
                    {x.ip && ` · ${PROTO_NAMES[x.proto] ?? ""}`}
                  </span>
                  <span class="mono faint">{[models.get(x.did), x.ip].filter(Boolean).join(" · ") || `id ${x.did}`}</span>
                  <span class="lstate">
                    <span class={cls("badge", on && "ok", st === "No reply" && "warn")}>
                      {on && <i class="gdot" />}
                      {st}
                    </span>
                    {on && (
                      <>
                        <span class="sw" style={{ background: rgb }} />
                        <span class="mono">{x.bright}%</span>
                        <span class="minibar">
                          <i style={{ width: `${x.bright}%`, background: rgb }} />
                        </span>
                        {x.ct > 0 && x.caps & 2 ? <span class="mono">{x.ct} K</span> : null}
                      </>
                    )}
                    {x.flags & HomeFlag.FAILED ? <span class="amber small">The last change got no reply</span> : null}
                  </span>
                </span>
              </button>
            );
          })}
        </div>
        {l && (
          <Box class="lamp-detail">
            <div class="knobview">
              <KnobHome lamps={lamps} sel={i} />
              <span class="hint">On the knob in HOME mode</span>
            </div>
            <div class="stk">
              <span class="lab">Name on the knob</span>
              <div class="line">
                <Text
                  key={l.did}
                  value={l.name}
                  max={HOME_NAME_MAX}
                  width={200}
                  class="pixel"
                  onEnter={(v) => v.trim() && v !== l.name && void edit(HomeEdit.NAME, v.trim(), "Renaming")}
                  on={() => {}}
                />
                <span class="hint">Enter to rename</span>
              </div>
            </div>
            <div class="stk">
              <span class="lab">Icon</span>
              <div class="kinds">
                {KIND_NAMES.map((n, k) => (
                  <button type="button" class={cls("card", l.kind === k && "on")} disabled={!editable || !!busy.value} onClick={() => k !== l.kind && void edit(HomeEdit.KIND, k, "Changing the icon")}>
                    <LampTile kind={k} lit scale={1} />
                    <span class="sub">{n.replace("Desk lamp, arm", "With arm").replace("Desk lamp", "Desk")}</span>
                  </button>
                ))}
              </div>
            </div>
            <div class="row">
              <span class="lab">Order</span>
              <div class="line">
                <button class="btn sm" disabled={!editable || i === 0 || !!busy.value} onClick={() => void edit(HomeEdit.MOVE, i - 1, "Moving")}>
                  ↑ Earlier
                </button>
                <button class="btn sm" disabled={!editable || i === lamps.length - 1 || !!busy.value} onClick={() => void edit(HomeEdit.MOVE, i + 1, "Moving")}>
                  ↓ Later
                </button>
                <span class="hint">
                  {i + 1} of {lamps.length}
                </span>
              </div>
            </div>
            <div class="row">
              <span class="lab">Can change</span>
              <div class="line" style={{ gap: "4px" }}>
                <span class="tag">Power</span>
                {capsList(l.caps).map((c) => (
                  <span class="tag">{c}</span>
                ))}
              </div>
            </div>
            {!(l.flags & HomeFlag.ONLINE) && <div class="banner info">Not answering. Is it plugged in and on the same network? In HOME mode on the knob, F1 tries it again.</div>}
            {err.value && <div class="banner">{err.value}</div>}
            {!editable && <span class="hint">Renaming, icons, order and removing need firmware with extensions v12.</span>}
            <div class="line" style={{ borderTop: "1px solid var(--line)", paddingTop: "12px" }}>
              <span class="hint">{busy.value ? `${busy.value}…` : "Stored on the knob at once"}</span>
              <span class="spacer" />
              {editable && <Confirm label="Remove from knob" ask="Sure? Remove it" class="danger sm" on={() => void edit(HomeEdit.REMOVE, 0, "Removing").then(() => (sel.value = null))} />}
            </div>
          </Box>
        )}
      </div>
    </>
  );
}
