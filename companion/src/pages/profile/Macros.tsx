// Macros: key presses, text and pauses the knob types itself on one press, used from a key's
// quick tap or a wheel command. Record turns what you type into steps. Renaming a macro updates
// what uses it; deleting one clears those.

import { useSignal } from "@preact/signals";
import { useEffect, useRef } from "preact/hooks";
import { ASCII_RE, hidFromCode, MAX, modifiersFromEvent, type Macro, type ProfileJson, type Step } from "../../profile";
import { isWindows } from "../../platform";
import { Box, Card, cls, Confirm, KeyField, Num, Seg, Text } from "../../ui/controls";
import { titleCase } from "../../ui/shell";
import type { Session } from "./session";

// Every place that names a macro, for renames and deletes.
function eachRef(p: ProfileJson, fn: (get: () => string | undefined, set: (v: string | undefined) => void) => void) {
  for (const a of Object.values(p.slots ?? {})) {
    if (!a) continue;
    fn(() => a.macro, (v) => (a.macro = v));
    fn(() => a.tap_macro, (v) => (a.tap_macro = v));
  }
  for (const r of p.rings ?? []) for (const c of r.cmds) fn(() => c.macro, (v) => (c.macro = v));
}

function uses(p: ProfileJson, name: string): number {
  let n = 0;
  eachRef(p, (get) => get() === name && n++);
  return n;
}

export function MacrosTab(x: { s: Session; p: ProfileJson }) {
  const { s, p } = x;
  void s.rev.value;
  const sel = useSignal(0);
  const macros = (p.macros ??= []);
  const i = Math.min(sel.value, macros.length - 1);
  const t = () => s.touch();
  const add = () => {
    let n = 1;
    while (macros.some((m) => m.name === `MACRO ${n}`)) n++;
    macros.push({ name: `MACRO ${n}`, steps: [] });
    sel.value = macros.length - 1;
    t();
  };
  return (
    <div class="split side">
      <Box title="Macros" note={`${MAX.macros} at most`} class="list-col">
        {macros.map((m, k) => {
          const u = uses(p, m.name);
          return (
            <Card left on={k === i} onClick={() => (sel.value = k)}>
              <span class="nm">{titleCase(m.name)}</span>
              <span class="sub">
                {m.steps.length} step{m.steps.length === 1 ? "" : "s"} · {u ? `used ${u}×` : "not used yet"}
              </span>
            </Card>
          );
        })}
        <button class="btn ghost" disabled={macros.length >= MAX.macros} onClick={add}>
          + Macro
        </button>
        <span class="hint foot">Key presses, text and pauses the knob types on one press. Use one from a key's quick tap or a wheel command.</span>
      </Box>
      {i >= 0 ? (
        <MacroEditor
          key={i}
          p={p}
          m={macros[i]}
          touch={t}
          remove={() => {
            const m = macros[i];
            eachRef(p, (get, set) => get() === m.name && set(undefined));
            macros.splice(i, 1);
            sel.value = Math.max(0, i - 1);
            t();
          }}
        />
      ) : (
        <Box>
          <span class="hint">No macros yet.</span>
        </Box>
      )}
    </div>
  );
}

function MacroEditor(x: { p: ProfileJson; m: Macro; touch: () => void; remove: () => void }) {
  const { p, m, touch } = x;
  const recording = useSignal(false);
  const timing = useSignal(false);
  const prev = useRef(m.name);
  const last = useRef(0);
  const full = m.steps.length >= MAX.steps;
  const push = (st: Step) => {
    if (m.steps.length >= MAX.steps) return;
    m.steps.push(st);
    touch();
  };

  // RECORD: keys pressed while it's on become steps (with the pauses between them, if timing is
  // on). Modifiers alone and held-key repeats don't count. Keys the system takes first (⌘Q,
  // ⌘Tab) never arrive -- add those with + Key.
  useEffect(() => {
    if (!recording.value) return;
    last.current = 0;
    const onKey = (e: KeyboardEvent) => {
      e.preventDefault();
      e.stopPropagation();
      if (e.repeat) return;
      const code = hidFromCode(e.code);
      if (code === undefined) return;
      const now = performance.now();
      if (timing.value && m.steps.length && last.current) {
        const gap = Math.min(MAX.waitMs, Math.round((now - last.current) / 10) * 10);
        if (gap >= 50) push({ wait: gap });
      }
      last.current = now;
      push({ key: [modifiersFromEvent(e), code] });
      if (m.steps.length >= MAX.steps) recording.value = false;
    };
    window.addEventListener("keydown", onKey, true);
    return () => window.removeEventListener("keydown", onKey, true);
  }, [recording.value]);

  const move = (j: number, d: number) => {
    const k = j + d;
    if (k < 0 || k >= m.steps.length) return;
    [m.steps[j], m.steps[k]] = [m.steps[k], m.steps[j]];
    touch();
  };

  return (
    <Box>
      <div class="line">
        <label class="line" style={{ gap: "8px" }}>
          <span class="lab">Name</span>
          <Text
            value={m.name}
            max={MAX.name}
            width={190}
            on={(v) => {
              eachRef(p, (get, set) => get() === prev.current && set(v));
              m.name = v;
              prev.current = v;
              touch();
            }}
          />
        </label>
        <span class="hint">
          {m.steps.length} of {MAX.steps} steps
        </span>
        <span class="spacer" />
        <Confirm label="Delete macro" class="danger sm" on={x.remove} />
      </div>
      <div class="list">
        {m.steps.length === 0 && (
          <div class="li">
            <span class="hint">No steps yet: add some, or press Record and type.</span>
          </div>
        )}
        {m.steps.map((st, j) => (
          <div class="li steprow">
            <span class="tag">{"key" in st ? "Key" : "text" in st ? "Text" : "Pause"}</span>
            {"key" in st ? (
              <KeyField value={st.key} on={(k) => ((st.key = k ?? [0, 0]), touch())} />
            ) : "text" in st ? (
              <span class="line">
                <Text value={st.text} max={MAX.text} upper={false} width={280} placeholder="Plain text (ASCII)" class={cls(!ASCII_RE.test(st.text) && "bad")} on={(v) => ((st.text = v), touch())} />
                {!ASCII_RE.test(st.text) && <span class="hint amber">Plain ASCII only: no accents or emoji</span>}
              </span>
            ) : (
              <Num value={st.wait} min={0} max={MAX.waitMs} step={10} unit="ms" on={(v) => ((st.wait = v), touch())} />
            )}
            <span class="updown">
              <button class="x" aria-label="Move up" disabled={j === 0} onClick={() => move(j, -1)}>
                ↑
              </button>
              <button class="x" aria-label="Move down" disabled={j === m.steps.length - 1} onClick={() => move(j, 1)}>
                ↓
              </button>
            </span>
            <button class="x" aria-label="Remove" onClick={() => (m.steps.splice(j, 1), touch())}>
              ×
            </button>
          </div>
        ))}
      </div>
      <div class="line">
        <button class="btn sm" disabled={full} onClick={() => push({ key: [0, 0] })}>
          + Key
        </button>
        <button class="btn sm" disabled={full} onClick={() => push({ text: "" })}>
          + Text
        </button>
        <button class="btn sm" disabled={full} onClick={() => push({ wait: 100 })}>
          + Pause
        </button>
        <span class="spacer" />
        <Seg
          options={[
            { value: false, label: "Keys only" },
            { value: true, label: "With timing" },
          ]}
          value={timing.value}
          on={(v) => (timing.value = v)}
        />
        <button class={cls("btn sm", recording.value ? "primary rec" : "")} disabled={full && !recording.value} onClick={() => (recording.value = !recording.value)}>
          {recording.value ? "■ Stop" : "● Record"}
        </button>
      </div>
      <span class="hint">Record turns what you type into steps. Keys the system takes first ({isWindows ? "Alt+Tab, Win+L" : "⌘Q, ⌘Tab"}) don't arrive: add those with + Key.</span>
    </Box>
  );
}
