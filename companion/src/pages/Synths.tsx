// SYNTHS: MIDI mode's synth profiles, edited like app profiles. On the left the knob's synths
// (built in, changed, or your own) and its screen; on the right the open one in three tabs --
// its parameters (the order F1 steps through), its programs and channel, and a monitor of what
// the knob sends on USB MIDI. The green row is the parameter the knob is on now.

import { signal, useSignal } from "@preact/signals";
import { useEffect, useRef } from "preact/hooks";
import { EXT_SYNTH_VERSION, HidType, MAX_SYNTHS, Set, SynthEdit, SynthFlag, type SynthEntry } from "../proto";
import { describe, evenValues, freeId, groups, newSynth, paramMax, parseSynth, rangeText, SYNTH_MAX, tidySynth, type SynthJson, type SynthParam } from "../synth";
import { device, go, href, use, type SynthTab } from "../store";
import { openJson, saveJson, savedText } from "../files";
import { isTauri } from "../transport";
import { Box, cls, Confirm, Num, PageHead, Row, Seg, SubTabs, Text } from "../ui/controls";
import { drawLogo, drawMidiScreen, hasLogo } from "../ui/knobart";
import { synthSession, type SynthSession } from "./synths/session";

export function synthOrigin(flags: number): string {
  if (flags & SynthFlag.BUILTIN) return flags & (SynthFlag.STORED | SynthFlag.LIVE) ? "Built-in, changed" : "Built-in";
  return flags & SynthFlag.STORED ? "Yours" : "New, not saved";
}

export function MakerMark(p: { maker: string; scale?: number }) {
  const ref = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    if (ref.current) drawLogo(ref.current, p.maker);
  }, [p.maker]);
  const z = p.scale ?? 2;
  if (!hasLogo(p.maker)) return <span class="tag">{p.maker && p.maker !== "MIDI" ? p.maker : "MIDI"}</span>;
  return <canvas ref={ref} class="mark" width={27} height={7} style={{ width: `${27 * z}px`, height: `${7 * z}px` }} aria-label={p.maker} />;
}

const inUseIndex = () => (device.settings?.hidType === HidType.MIDI ? device.settings.midiSynth : -1);

export function SynthsPage(p: { id: string; tab: SynthTab }) {
  use("synths", "settings", "conn");
  const list = device.synths.filter(Boolean);
  useEffect(() => {
    if (!p.id && list.length) {
      const s = device.settings, i = s ? Math.min(s.midiSynth, list.length - 1) : 0;
      go({ page: "synths", id: list[i].id, tab: "params" });
    }
  }, [p.id, list.length]);
  if ((device.ext ?? 0) < EXT_SYNTH_VERSION)
    return (
      <>
        <PageHead title="Synths" />
        <Box>
          <span class="hint">Editing synths needs firmware with extensions v12. The synth and the channel are on the Mode page.</span>
        </Box>
      </>
    );
  const entry = list.find((x) => x.id === p.id);
  return (
    <div class="syn-split">
      <SynthList current={p.id} />
      {entry ? <Editor key={p.id} entry={entry} tab={p.tab} /> : <Box><span class="hint">{list.length ? "There's no synth with that name on the knob." : "Reading the synths…"}</span></Box>}
    </div>
  );
}

function SynthList(p: { current: string }) {
  use("synths", "settings");
  const err = useSignal<string | null>(null);
  const list = device.synths.filter(Boolean);
  const using = inUseIndex();
  const add = async (s: SynthJson) => {
    err.value = null;
    try {
      await device.putSynth(s, false);
      go({ page: "synths", id: s.id, tab: "params" });
    } catch (e) {
      err.value = e instanceof Error ? e.message : String(e);
    }
  };
  return (
    <div class="slist">
      {list.map((x) => (
        <a class={cls("scard", x.id === p.current && "on", (x.flags & SynthFlag.LIVE) !== 0 && "dirty")} href={href({ page: "synths", id: x.id, tab: "params" })}>
          <MakerMark maker={x.maker} />
          <span class="nm">{x.name}</span>
          <span class="sub">
            {x.index === using && (
              <>
                <i class="gdot" />
                In use ·{" "}
              </>
            )}
            {synthOrigin(x.flags & ~SynthFlag.LIVE)} · {x.params}
          </span>
        </a>
      ))}
      <button type="button" class="scard dashed" disabled={list.length >= MAX_SYNTHS} onClick={() => void add(newSynth(list.map((x) => x.id)))}>
        <span class="nm">+ New synth</span>
        <span class="sub">Four parameters to start from</span>
      </button>
      <button
        class="btn ghost sm"
        style={{ alignSelf: "flex-start" }}
        disabled={list.length >= MAX_SYNTHS}
        onClick={async () => {
          err.value = null;
          try {
            const f = await openJson();
            if (f) await add(parseSynth(f.data));
          } catch (x) {
            err.value = x instanceof Error ? x.message : String(x);
          }
        }}
      >
        Import a file…
      </button>
      {err.value && <span class="hint amber">{err.value}</span>}
      <KnobPreview />
    </div>
  );
}

// The knob's MIDI screen for the open synth and parameter: live when it's the one in use.
function KnobPreview() {
  use("midi", "settings", "synths");
  const s = synthSession.value;
  void s?.rev.value;
  const draft = s?.draft.value;
  const ref = useRef<HTMLCanvasElement>(null);
  const sel = selected.value;
  const idx = s ? s.index() : -1;
  const live = idx >= 0 && idx === inUseIndex();
  const m = device.midi;
  const pi = live && m ? m.param : sel;
  const value = live && m && m.param === pi ? m.value : -1;
  const ports = { usb: !!m?.usb, trs: !!m?.trs, channel: device.settings?.midiChannel ?? 1 };
  const key = JSON.stringify([draft?.name, draft?.maker, draft?.params[pi], pi, value, ports]);
  useEffect(() => {
    const c = ref.current;
    if (c && draft) void document.fonts.load("8px Silkscreen").then(() => drawMidiScreen(c, draft, pi, value, ports));
  }, [key]);
  if (!draft) return null;
  return (
    <Box class="preview-box">
      <div class="knobview">
        <canvas ref={ref} class="knobring small" width={300} height={300} aria-label="The knob's MIDI screen" />
        <span class="hint">{live ? (m?.active ? "On the knob now" : "In use; the knob isn't in MIDI mode now") : "How the knob shows it"}</span>
      </div>
      {live && (
        <div class="line" style={{ gap: "6px" }}>
          <span class={cls("badge", m?.usb && "ok")}>
            <i class={cls("gdot", !m?.usb && "off")} />
            USB MIDI
          </span>
          <span class={cls("badge", m?.trs && "ok")}>
            <i class={cls("gdot", !m?.trs && "off")} />
            TRS
          </span>
        </div>
      )}
    </Box>
  );
}

// The parameter open in the editor (an index into the draft's params).
const selected = signal(0);

function Editor(p: { entry: SynthEntry; tab: SynthTab }) {
  use("synths", "settings", "midi");
  const s = synthSession.value;
  if (!s || s.id !== p.entry.id) return null;
  void s.rev.value;
  const e = p.entry;
  const draft = s.draft.value;
  const using = e.index === inUseIndex();
  const live = (e.flags & SynthFlag.LIVE) !== 0;
  const st = s.status.value;
  const tabs = [
    { value: "params" as const, label: "Parameters", count: draft?.params.length ?? e.params, href: href({ page: "synths", id: e.id, tab: "params" }) },
    { value: "programs" as const, label: "Programs and channel", href: href({ page: "synths", id: e.id, tab: "programs" }) },
    { value: "monitor" as const, label: "Monitor", href: href({ page: "synths", id: e.id, tab: "monitor" }) },
  ];
  const exportIt = async () => {
    if (!draft) return;
    try {
      const path = await saveJson(`${draft.id}.json`, tidySynth(draft));
      if (path !== null) s.say(savedText(path));
    } catch (x) {
      s.say(x instanceof Error ? x.message : String(x), true);
    }
  };
  const duplicate = async () => {
    if (!draft) return;
    const copy = structuredClone(tidySynth(draft));
    copy.id = freeId(`${draft.id}-copy`, device.synths.map((x) => x.id));
    copy.name = `${draft.name.slice(0, SYNTH_MAX.name - 2)} 2`;
    try {
      await device.putSynth(copy, false);
      go({ page: "synths", id: copy.id, tab: p.tab });
    } catch (x) {
      s.say(x instanceof Error ? x.message : String(x), true);
    }
  };
  return (
    <div class="stack">
      <div class="ph">
        <div class="t">
          <div class="line" style={{ gap: "8px" }}>
            <span class="title">{draft?.name ?? e.name}</span>
            {using ? (
              <span class="badge ok">
                <i class="gdot" />
                In use
              </span>
            ) : null}
            <span class="badge">{synthOrigin(e.flags & ~SynthFlag.LIVE)}</span>
            {live && <span class="badge warn">Live, not saved</span>}
          </div>
          <span class={st.bad ? "hint amber" : "hint"} role="status">
            {st.msg || "Edits reach the knob as you make them. Save to knob keeps them."}
          </span>
        </div>
        <span class="spacer" />
        {!using && (
          <button
            class="btn"
            onClick={async () => {
              if (device.settings?.hidType !== HidType.MIDI) await device.set(Set.HID_TYPE, HidType.MIDI);
              await device.set(Set.MIDI_SYNTH, e.index);
            }}
          >
            Use on the knob
          </button>
        )}
        <button class="btn ghost" disabled={device.synths.length >= MAX_SYNTHS || !draft} onClick={() => void duplicate()}>
          Duplicate
        </button>
        <button class="btn ghost" disabled={!draft} onClick={() => void exportIt()}>
          Export…
        </button>
      </div>
      <SubTabs tabs={tabs} value={p.tab} />
      {!draft ? null : p.tab === "programs" ? <ProgramsTab s={s} d={draft} entry={e} /> : p.tab === "monitor" ? <MonitorTab d={draft} /> : <ParamsTab s={s} d={draft} using={using} />}
    </div>
  );
}

// --- Parameters ---

// Each part that draws from the draft reads the session's rev: a component whose props didn't
// change re-renders only for a signal it reads (@preact/signals).
function ParamsTab(x: { s: SynthSession; d: SynthJson; using: boolean }) {
  const { s, d } = x;
  void s.rev.value;
  const m = device.midi;
  const here = x.using && m?.active ? m.param : -1;
  const sel = Math.min(selected.value, d.params.length - 1);
  return (
    <>
      <div class="tbl">
        <div class="tr head prow">
          <span>#</span>
          <span>Name</span>
          <span>Sends</span>
          <span>CC</span>
          <span>Range</span>
          <span>Knob</span>
        </div>
        {groups(d).map((g) => (
          <>
            <div class="ghead">
              {g.group || "—"}
              <span>{g.rows.length}</span>
            </div>
            {g.rows.map((i) => {
              const pr = d.params[i];
              return (
                <>
                  <div class={cls("tr prow", i === sel && "sel")} role="button" tabIndex={0} onClick={() => (selected.value = i)} onKeyDown={(e) => e.key === "Enter" && (selected.value = i)}>
                    <span class="idx">{String(i + 1).padStart(2, "0")}</span>
                    <span class="pn">{pr.name}</span>
                    <span>{pr.options ? <span class="tag">Switch · {pr.options.length}</span> : <span class="tag">{pr.sends === "korg10" ? "KORG 10-bit" : "CC"}</span>}</span>
                    <span class="mono">{pr.cc}</span>
                    <span class="hint clip">{rangeText(pr)}</span>
                    <span>
                      {i === here && (
                        <span class="live" title="The knob is on this one">
                          <i class="gdot" />
                          <span class="bar">
                            <i style={{ width: `${m && m.value >= 0 ? (Math.min(m.value, paramMax(pr)) / paramMax(pr)) * 100 : 0}%` }} />
                          </span>
                        </span>
                      )}
                    </span>
                  </div>
                  {i === sel && <ParamEditor s={s} d={d} i={i} using={x.using} />}
                </>
              );
            })}
          </>
        ))}
      </div>
      <div class="line">
        <button
          class="btn sm"
          disabled={d.params.length >= SYNTH_MAX.params}
          onClick={() => {
            const after = d.params[sel];
            d.params.splice(sel + 1, 0, { group: after?.group ?? "", name: "NEW", sends: "cc", cc: 1 });
            selected.value = sel + 1;
            s.touch();
          }}
        >
          + Parameter
        </button>
        <span class="hint">
          Up to {SYNTH_MAX.params}. F1 on the knob steps through them in this order. The feel follows the kind: fine steps for a value, a click per option for a switch.
        </span>
      </div>
    </>
  );
}

function ParamEditor(x: { s: SynthSession; d: SynthJson; i: number; using: boolean }) {
  const { s, d, i } = x;
  void s.rev.value;
  const p = d.params[i];
  const kind = p.options ? "switch" : p.centred ? "centred" : "value";
  const touch = () => s.touch();
  const setKind = (k: string) => {
    if (k === "switch") {
      if (!p.options) p.options = [{ name: "OFF", value: 0 }, { name: "ON", value: 127 }];
      p.centred = undefined;
      p.sends = "cc";
    } else {
      p.options = undefined;
      p.centred = k === "centred" ? true : undefined;
    }
    touch();
  };
  const move = (dir: number) => {
    const j = i + dir;
    if (j < 0 || j >= d.params.length) return;
    [d.params[i], d.params[j]] = [d.params[j], d.params[i]];
    selected.value = j;
    touch();
  };
  return (
    <div class="ped" onClick={(e) => e.stopPropagation()}>
      <Row label="Name on the knob">
        <Text value={p.name} max={SYNTH_MAX.label} width={150} class="pixel" on={(v) => ((p.name = v), touch())} />
        <span class="lab">Group</span>
        <Text value={p.group} max={SYNTH_MAX.label} width={130} class="pixel" placeholder="—" on={(v) => ((p.group = v), touch())} />
      </Row>
      <Row label="Kind">
        <Seg
          options={[
            { value: "value", label: "Value" },
            { value: "centred", label: "Centred value" },
            { value: "switch", label: "Switch" },
          ]}
          value={kind}
          on={setKind}
        />
      </Row>
      <Row label="Sends">
        <Seg
          options={[
            { value: "cc" as const, label: "CC, 7-bit" },
            { value: "korg10" as const, label: "KORG 10-bit", disabled: !!p.options },
          ]}
          value={p.options ? "cc" : (p.sends ?? "cc")}
          on={(v) => ((p.sends = v), touch())}
        />
        <span class="lab">CC</span>
        <Num value={p.cc} min={0} max={127} width={70} on={(v) => ((p.cc = v), touch())} />
        {p.sends === "korg10" && !p.options && <span class="hint">CC 63 carries the low 3 bits first: 0 to 1023</span>}
      </Row>
      {p.options && <Options p={p} s={s} touch={touch} />}
      <div class="line">
        <button class="btn sm ghost" disabled={i === 0} onClick={() => move(-1)}>
          ↑ Earlier
        </button>
        <button class="btn sm ghost" disabled={i === d.params.length - 1} onClick={() => move(1)}>
          ↓ Later
        </button>
        <span class="spacer" />
        {x.using && (
          <button class="btn sm ghost" onClick={() => void device.synthGoto(i)}>
            Show on the knob
          </button>
        )}
        <button
          class="btn sm danger"
          disabled={d.params.length <= 1}
          onClick={() => {
            d.params.splice(i, 1);
            selected.value = Math.max(0, i - 1);
            touch();
          }}
        >
          Delete
        </button>
      </div>
    </div>
  );
}

function Options(x: { p: SynthParam; s: SynthSession; touch: () => void }) {
  void x.s.rev.value;
  const o = x.p.options!;
  return (
    <div class="row top">
      <span class="lab">Options</span>
      <div class="optrows">
        {o.map((opt, k) => (
          <div class="optrow">
            <span class="mono faint">{k + 1}</span>
            <Text value={opt.name} max={SYNTH_MAX.option} width={120} class="pixel" on={(v) => ((opt.name = v), x.touch())} />
            <Num value={opt.value} min={0} max={127} width={70} on={(v) => ((opt.value = v), x.touch())} />
            <span class="vbar" aria-hidden="true">
              <i style={{ left: `${(opt.value / 127) * 100}%` }} />
            </span>
            <button type="button" class="x" aria-label="Remove option" disabled={o.length <= 2} onClick={() => (o.splice(k, 1), x.touch())}>
              ×
            </button>
          </div>
        ))}
        <div class="line">
          <button class="btn sm" disabled={o.length >= SYNTH_MAX.options} onClick={() => (o.push({ name: `OPT ${o.length + 1}`, value: 127 }), x.touch())}>
            + Option
          </button>
          <button class="btn sm ghost" onClick={() => (evenValues(o.length).forEach((v, k) => (o[k].value = v)), x.touch())}>
            Space evenly
          </button>
          <span class="hint">The value sent for each; what the synth sends back counts as the nearest.</span>
        </div>
      </div>
    </div>
  );
}

// --- Programs and channel ---

function ProgramsTab(x: { s: SynthSession; d: SynthJson; entry: SynthEntry }) {
  const { s, d, entry } = x;
  void s.rev.value;
  const builtin = (entry.flags & SynthFlag.BUILTIN) !== 0;
  const changed = (entry.flags & (SynthFlag.STORED | SynthFlag.LIVE)) !== 0;
  const prog = d.programs ?? { scheme: "pc" as const, count: 128 };
  const remove = async () => {
    const i = s.index();
    if (i < 0) return;
    s.discard();
    try {
      const r = await device.synthOp(i, SynthEdit.REMOVE);
      if (r.removed) go({ page: "synths", id: "", tab: "params" });
      else await s.load();
    } catch (e) {
      s.say(e instanceof Error ? e.message : String(e), true);
    }
  };
  return (
    <div class="stack">
      <Box title="Synth" note={<span class="mono">id: {d.id}</span>}>
        <Row label="Name on the knob">
          <Text value={d.name} max={SYNTH_MAX.name} width={200} class="pixel" on={(v) => ((d.name = v), s.touch())} />
        </Row>
        <Row label="Maker">
          <Seg
            options={[
              { value: "KORG", label: "KORG" },
              { value: "ROLAND", label: "Roland" },
              { value: "", label: "Other" },
            ]}
            value={d.maker === "KORG" || d.maker === "ROLAND" ? d.maker : ""}
            on={(v) => ((d.maker = v), s.touch())}
          />
          <MakerMark maker={d.maker} />
          <span class="hint">KORG and Roland show their logo on the knob</span>
        </Row>
        <Row label="Comes set to">
          <Num value={d.channel} min={0} max={16} width={70} empty="—" on={(v) => ((d.channel = v), s.touch())} />
          <span class="hint">The synth's own channel from the factory, shown as a hint on the Mode page. The knob sends on the channel set there.</span>
        </Row>
      </Box>
      <Box title="Programs" note="F2 and F3 on the knob step through them">
        <Row label="Sent as">
          <Seg
            options={[
              { value: "pc" as const, label: "Program change" },
              { value: "korg-bank100" as const, label: "KORG, banks of 100" },
            ]}
            value={prog.scheme}
            on={(v) => ((d.programs = { ...prog, scheme: v, count: v === "pc" ? Math.min(prog.count, 128) : prog.count }), s.touch())}
          />
        </Row>
        <Row label="How many">
          <Num value={prog.count} min={1} max={prog.scheme === "pc" ? 128 : 12800} width={90} on={(v) => ((d.programs = { ...prog, count: v }), s.touch())} />
          <span class="hint">{prog.scheme === "pc" ? "Program change 1 to 128" : "Bank select (MSB 0, LSB the hundreds), then program change"}</span>
        </Row>
      </Box>
      {(!builtin || changed) && (
        <section class="box row-box">
          <div class="t">
            <b>{builtin ? "Reset to default" : "Delete this synth"}</b>
            <span class="hint">{builtin ? `Puts the ${d.name} back the way it shipped. Your changes to it are lost.` : "Removes it from the knob. This can't be undone; export it first to keep a copy."}</span>
          </div>
          <span class="spacer" />
          <Confirm label={builtin ? "Reset to default" : "Delete synth"} ask={builtin ? "Sure? Reset it" : "Sure? Delete it"} on={() => void remove()} />
        </section>
      )}
    </div>
  );
}

// --- Monitor: the knob's USB MIDI port, read here (src-tauri/src/midi.rs) ---

interface Logged {
  t: string;
  bytes: number[];
}
const log = signal<Logged[]>([]);
const port = signal<string>("");

function MonitorTab(x: { d: SynthJson }) {
  use("midi", "settings");
  void synthSession.value?.rev.value;
  useEffect(() => {
    if (!isTauri()) return;
    let off: (() => void)[] = [];
    let gone = false;
    void (async () => {
      const { invoke } = await import("@tauri-apps/api/core");
      const { listen } = await import("@tauri-apps/api/event");
      port.value = await invoke<string>("midi_port");
      const a = await listen<{ us: number; bytes: number[] }>("midi-msg", (e) => {
        const now = new Date();
        const t = `${now.toTimeString().slice(0, 8)}.${String(now.getMilliseconds()).padStart(3, "0")}`;
        log.value = [...log.value.slice(-199), { t, bytes: e.payload.bytes }];
      });
      const b = await listen<string>("midi-port", (e) => (port.value = e.payload));
      off = [a, b];
      if (gone) off.forEach((f) => f());
      await invoke("midi_watch", { on: true });
    })();
    return () => {
      gone = true;
      off.forEach((f) => f());
      void import("@tauri-apps/api/core").then(({ invoke }) => invoke("midi_watch", { on: false }));
    };
  }, []);
  const m = device.midi;
  if (!isTauri())
    return (
      <Box>
        <span class="hint">The monitor reads the knob's USB MIDI port, which needs the Quadra app. In a browser, NanoDepsidf/tools/midi_monitor.html does the same.</span>
      </Box>
    );
  const rows = log.value.slice(-60).reverse();
  return (
    <>
      <div class="line">
        <span class={cls("badge", port.value && "ok")}>
          <i class={cls("gdot", !port.value && "off")} />
          {port.value || "No MIDI port"}
        </span>
        <span class="hint">{port.value ? "What the knob sends, on USB MIDI and the TRS jacks alike." : "The knob has a MIDI port in MIDI mode only (F4 menu › Profiles › MIDI, or the Mode page)."}</span>
        <span class="spacer" />
        {m && (
          <span class="hint mono">
            sent {m.tx} · got {m.rx}
          </span>
        )}
        <button class="btn sm ghost" onClick={() => (log.value = [])}>
          Clear
        </button>
      </div>
      <div class="tbl mlog">
        <div class="tr head mrow">
          <span>Time</span>
          <span>Message</span>
          <span>Ch</span>
          <span>Data</span>
          <span>Parameter</span>
        </div>
        {rows.length === 0 ? (
          <div class="tr">
            <span class="hint">Nothing yet. Turn the knob.</span>
          </div>
        ) : (
          rows.map((r) => {
            const dsc = describe(r.bytes, x.d);
            return (
              <div class="tr mrow">
                <span class="faint">{r.t}</span>
                <span>{dsc.type}</span>
                <span>{dsc.ch}</span>
                <span>{dsc.data}</span>
                <span class="hint">{dsc.what}</span>
              </div>
            );
          })
        )}
      </div>
      <span class="hint">Messages the synth sends back come in on the knob, not here: the knob shows their values (sent / got above counts them).</span>
    </>
  );
}
