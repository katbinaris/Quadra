// LAMPS › IMPORT: the lamps of a Xiaomi account onto the knob, in four steps -- where the keys
// come from (the token extractor's file, or a new sign-in in Terminal), finding the lamps on the
// network, choosing names / icons / order, and sending them over USB. quadra.py's `home import`,
// in the app. The keys go to the knob only, and only over a cable.

import { signal } from "@preact/signals";
import { useEffect } from "preact/hooks";
import { findLamps, isLight, KIND_NAMES, knobName, lampKind, lightProps, PROTO_NAMES, probeLamp, readDevices, runExtractor, tokenBytes, type LightProps, type XDevice, type XFile } from "../home";
import { isWindows, thisComputer } from "../platform";
import { EXT_HOME_VERSION, HOME_MAX_LAMPS, HOME_NAME_MAX, HomeProto } from "../proto";
import { device, go, href, use } from "../store";
import { isTauri } from "../transport";
import { Box, Card, cls, PageHead, Text } from "../ui/controls";
import { LampTile } from "./Lamps";

interface Found {
  dev: XDevice;
  light: boolean;
  props: LightProps | null; // null: no spec or no light service
  where: string | null; // where it answers now
  proto: number | null; // null: didn't answer
  checked: boolean;
  pick: boolean;
  name: string;
  kind: number;
  why: string; // why it can't go on the knob
}

// The import's state lives here, so leaving the page and coming back keeps it.
const step = signal(0);
const file = signal<XFile | null | undefined>(undefined); // undefined: not read yet
const fileErr = signal<string | null>(null);
const found = signal<Found[]>([]);
const progress = signal({ done: 0, total: 0, phase: "" });
const sending = signal<{ state: "idle" | "busy" | "done" | "failed"; msg: string }>({ state: "idle", msg: "" });
const signinOpened = signal(false);

const demo = new URLSearchParams(location.search).has("demo");
const canRun = isTauri() || demo;

async function reread() {
  fileErr.value = null;
  try {
    file.value = await readDevices();
  } catch (e) {
    fileErr.value = e instanceof Error ? e.message : String(e);
    file.value = null;
  }
}

async function find() {
  const devs = file.value?.devices ?? [];
  const rows: Found[] = devs.map((d) => ({ dev: d, light: isLight(d), props: null, where: null, proto: null, checked: false, pick: false, name: knobName(d.name || d.model), kind: lampKind(d.model), why: "" }));
  found.value = rows;
  const lights = rows.filter((r) => r.light && r.dev.token && r.dev.ip);
  rows.filter((r) => r.light && (!r.dev.token || !r.dev.ip)).forEach((r) => (r.why = "No key or address in the list"));
  progress.value = { done: 0, total: lights.length * 2 + 1, phase: "Looking on the network" };
  const bump = (phase?: string) => (progress.value = { ...progress.value, done: progress.value.done + 1, phase: phase ?? progress.value.phase });
  const [where] = await Promise.all([
    findLamps(lights.map((r) => r.dev.ip)).then((w) => (bump("Reading each model's spec"), w)),
    ...lights.map((r) =>
      lightProps(r.dev.model)
        .then((p) => {
          r.props = p;
          if (!p) r.why = "Its model has no light service in the MIoT spec";
        })
        .catch((e) => (r.why = String(e instanceof Error ? e.message : e)))
        .finally(() => bump()),
    ),
  ]);
  progress.value = { ...progress.value, phase: "Asking each lamp" };
  await Promise.all(
    lights.map(async (r) => {
      r.where = where[r.dev.did] ?? null;
      const ip = r.where ?? r.dev.ip;
      if (r.props) {
        const k = r.props.siid.findIndex((x) => x > 0);
        try {
          r.proto = await probeLamp(ip, r.dev.token, r.dev.did, r.props.siid[k] ?? 0, r.props.piid[k] ?? 0);
        } catch {
          r.proto = null;
        }
      }
      r.checked = true;
      r.pick = !!r.props;
      bump();
      found.value = [...rows];
    }),
  );
  // Over the knob's limit: the first ones.
  let n = 0;
  for (const r of rows) if (r.pick && ++n > HOME_MAX_LAMPS) r.pick = false;
  progress.value = { ...progress.value, phase: "Done" };
  found.value = [...rows];
}

async function send() {
  const pick = found.value.filter((r) => r.pick && r.props);
  sending.value = { state: "busy", msg: `Sending ${pick.length} lamps…` };
  try {
    await device.homeImport(
      pick.map((r) => ({
        did: Number(r.dev.did),
        ip: r.where ?? r.dev.ip,
        token: tokenBytes(r.dev.token),
        proto: r.proto ?? HomeProto.MIOT,
        caps: r.props!.caps,
        ctMin: r.props!.ct[0],
        ctMax: r.props!.ct[1],
        siid: r.props!.siid,
        piid: r.props!.piid,
        name: r.name || "LAMP",
        kind: r.kind,
      })),
    );
    sending.value = { state: "done", msg: `${pick.length} lamps on the knob` };
  } catch (e) {
    sending.value = { state: "failed", msg: e instanceof Error ? e.message : String(e) };
  }
}

const fmtDate = (ms: number) => new Date(ms).toLocaleString(undefined, { day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit" });

export function LampImportPage() {
  use("conn", "lamps");
  useEffect(() => {
    if (file.value === undefined && canRun) void reread();
  }, []);
  const go2 = (n: number) => {
    step.value = n;
    if (n === 1 && found.value.length === 0) void find();
    if (n === 3) sending.value = { state: "idle", msg: "" };
  };
  if ((device.ext ?? 0) < EXT_HOME_VERSION)
    return (
      <>
        <PageHead title="Import lamps" />
        <Box>
          <span class="hint">This needs firmware with HOME (extensions v11 and later).</span>
        </Box>
      </>
    );
  const f = file.value;
  const lights = f?.devices.filter(isLight) ?? [];
  const picks = found.value.filter((r) => r.pick).length;
  const steps = [
    ["Account", "Where the keys come from"],
    ["Find", "On your network"],
    ["Choose", "Names, icons, order"],
    ["Send", "Over USB"],
  ];
  const ready = [lights.length > 0, progress.value.phase === "Done" && picks > 0, picks > 0 && picks <= HOME_MAX_LAMPS];
  let body;
  if (!canRun) {
    body = (
      <Box>
        <span class="hint">The import needs the Quadra app on this computer: it reads your Xiaomi list and looks for the lamps on the network, which a web page can't. In a terminal, <span class="mono">quadra.py home import</span> does the same.</span>
      </Box>
    );
  } else if (step.value === 0) {
    body = (
      <>
        <div class="cards">
          <Card left on={!!f && lights.length > 0}>
            <span class="nm">Use the saved list</span>
            <span class="sub">{f ? `${f.path} · ${fmtDate(f.modified_ms)} · ${f.devices.length} devices, ${lights.length} lights` : fileErr.value ?? "No list yet: sign in first"}</span>
          </Card>
          <Card
            left
            onClick={async () => {
              try {
                await runExtractor();
                signinOpened.value = true;
              } catch (e) {
                fileErr.value = e instanceof Error ? e.message : String(e);
              }
            }}
          >
            <span class="nm">Sign in to Xiaomi…</span>
            <span class="sub">Opens the token extractor in {isWindows ? "a command window" : "Terminal"}: for new lamps, or after a lamp was reset</span>
          </Card>
        </div>
        {signinOpened.value && (
          <div class="banner info">
            <span>
              In {isWindows ? "the command window" : "Terminal"}: sign in with the QR code (Mi Home › Profile › Scan) or your password, and choose your server (Europe is <span class="mono">de</span>). When it says it's done, come back here.
            </span>
            <button class="btn sm" onClick={() => void reread().then(() => (found.value = []))}>
              Look again
            </button>
          </div>
        )}
        <div class="banner info">The list holds each lamp's key. It stays on {thisComputer} (readable by you only); the app sends the keys to the knob over USB and never shows them.</div>
      </>
    );
  } else if (step.value === 1) {
    const pr = progress.value;
    body = (
      <>
        <Box title={pr.phase === "Done" ? "Done looking" : `${pr.phase}…`} note="A hello to every address of the networks the lamps were last seen on">
          <div class="progress">
            <i style={{ width: `${pr.total ? (pr.done / pr.total) * 100 : 0}%` }} />
          </div>
        </Box>
        <div class="tbl">
          <div class="tr head find-row">
            <span />
            <span>Device</span>
            <span>Model</span>
            <span>Where</span>
            <span>Speaks</span>
          </div>
          {found.value.map((r) => (
            <div class={cls("tr find-row", !r.light && "dim")}>
              <span>{r.light && <LampTile lit={r.checked && r.proto !== null} scale={0.75} kind={r.kind} />}</span>
              <span>{r.dev.name}</span>
              <span class="mono">{r.dev.model}</span>
              <span>
                {!r.light ? (
                  <span class="faint">Not a light</span>
                ) : r.why ? (
                  <span class="amber">{r.why}</span>
                ) : !r.checked ? (
                  <span class="faint">…</span>
                ) : r.where ? (
                  <>
                    <span class="mono">{r.where}</span>
                    {r.where !== r.dev.ip && <span class="amber"> moved from {r.dev.ip.split(".").pop()}</span>}
                  </>
                ) : (
                  <span class="amber">Not answering</span>
                )}
              </span>
              <span>{r.light && r.checked && r.props ? (r.proto === null ? "MIoT, assumed" : PROTO_NAMES[r.proto]) : ""}</span>
            </div>
          ))}
        </div>
        <span class="hint">A lamp that isn't answering can still go on the knob: it finds it when it's back.</span>
      </>
    );
  } else if (step.value === 2) {
    const rows = found.value.filter((r) => r.light && r.props);
    const move = (r: Found, d: number) => {
      const all = [...found.value], a = all.indexOf(r), others = all.filter((x) => x.light && x.props);
      const b = all.indexOf(others[others.indexOf(r) + d]);
      if (b < 0) return;
      [all[a], all[b]] = [all[b], all[a]];
      found.value = all;
    };
    body = (
      <>
        <div class="tbl">
          <div class="tr head choose-row">
            <span />
            <span />
            <span>Name on the knob</span>
            <span>Icon</span>
            <span>Model</span>
            <span>Order</span>
          </div>
          {rows.map((r, k) => (
            <div class="tr choose-row">
              <button type="button" class={cls("chk", r.pick && "on")} aria-label="Include" aria-pressed={r.pick} onClick={() => ((r.pick = !r.pick), (found.value = [...found.value]))} />
              <LampTile lit kind={r.kind} scale={0.75} />
              <Text value={r.name} max={HOME_NAME_MAX} width={190} class="pixel" on={(v) => ((r.name = v), (found.value = [...found.value]))} />
              <select class="field" style={{ width: "140px" }} value={r.kind} onChange={(e) => ((r.kind = Number(e.currentTarget.value)), (found.value = [...found.value]))}>
                {KIND_NAMES.map((n, i) => (
                  <option value={i}>{n}</option>
                ))}
              </select>
              <span class="mono faint">{r.dev.model}</span>
              <span class="line" style={{ gap: "2px" }}>
                <button class="btn sm ghost" disabled={k === 0} aria-label="Earlier" onClick={() => move(r, -1)}>
                  ↑
                </button>
                <button class="btn sm ghost" disabled={k === rows.length - 1} aria-label="Later" onClick={() => move(r, 1)}>
                  ↓
                </button>
              </span>
            </div>
          ))}
        </div>
        <span class={picks > HOME_MAX_LAMPS ? "hint amber" : "hint"}>
          Names come from Mi Home, in the knob's capitals and without the brand word; the icon is guessed from the model. {picks} of {HOME_MAX_LAMPS} places on the knob.
        </span>
      </>
    );
  } else {
    const usb = device.kind === "tauri";
    const st = sending.value;
    body = (
      <Box title={`Send ${picks} lamps to the knob`} note="Replaces the lamps on the knob">
        {usb ? <div class="banner">Over USB only: the lamps' keys never travel over Wi-Fi.</div> : <div class="banner">Connect the knob with a USB cable: the lamps' keys only go over USB.</div>}
        <div class="tbl">
          {found.value
            .filter((r) => r.pick)
            .map((r) => (
              <div class="tr" style={{ gridTemplateColumns: "44px 1fr auto auto" }}>
                <LampTile lit kind={r.kind} scale={0.75} />
                <span>{r.name}</span>
                <span class="hint">{KIND_NAMES[r.kind]}</span>
                {st.state === "done" ? <span class="badge ok">Stored</span> : <span />}
              </div>
            ))}
        </div>
        <div class="line">
          {st.state === "done" ? (
            <>
              <button class="btn primary" onClick={() => go({ page: "lamps", sub: "list" })}>
                Show the lamps
              </button>
              <span class="hint">On the knob: F4 menu › Profiles › HOME</span>
            </>
          ) : (
            <button class="btn primary" disabled={!usb || st.state === "busy" || picks === 0} onClick={() => void send()}>
              {st.state === "busy" ? "Sending…" : `Send ${picks} lamps`}
            </button>
          )}
          {st.msg && st.state !== "done" && <span class={st.state === "failed" ? "hint amber" : "hint"}>{st.msg}</span>}
        </div>
      </Box>
    );
  }
  return (
    <>
      <PageHead title="Import lamps" hint="From your Xiaomi account to the knob. Again later only for new lamps.">
        <a class="btn ghost" href={href({ page: "lamps", sub: "list" })}>
          {sending.value.state === "done" ? "Close" : "Cancel"}
        </a>
      </PageHead>
      {canRun && (
        <div class="steps">
          {steps.map(([a, b], i) => (
            <button type="button" class={cls("step", i === step.value && "on", i < step.value && "done")} disabled={i > 0 && !ready.slice(0, i).every(Boolean)} onClick={() => go2(i)}>
              <span class="k">{i < step.value ? "✓" : i + 1}</span>
              <span>
                <b>{a}</b>
                <span>{b}</span>
              </span>
            </button>
          ))}
        </div>
      )}
      {body}
      {canRun && (
        <div class="line">
          <button class="btn" disabled={step.value === 0} onClick={() => go2(step.value - 1)}>
            Back
          </button>
          <span class="spacer" />
          {step.value === 1 && progress.value.phase === "Done" && (
            <button class="btn ghost" onClick={() => void find()}>
              Look again
            </button>
          )}
          {step.value < 3 && (
            <button class="btn primary" disabled={!ready[step.value]} onClick={() => go2(step.value + 1)}>
              {["Find the lamps", "Choose", `Send ${picks} lamps…`][step.value]}
            </button>
          )}
        </div>
      )}
    </>
  );
}
