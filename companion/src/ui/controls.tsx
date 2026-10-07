// The form controls every page uses, in the app's style (style.css): choices, fields, the key
// recorder, sliders, and the small drawings of steps and feels. Each reports a change through
// its `on...`; none of them re-renders anything else.

import type { ComponentChildren, HTMLAttributes } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { hidFromCode, keyName, Mod, modifiersFromEvent, type Key } from "../profile";
import { waveY } from "./draw";
import type { ClickWaveDef } from "../proto";

type Style = HTMLAttributes<HTMLElement>["style"];

export const cls = (...c: (string | false | null | undefined)[]) => c.filter(Boolean).join(" ");

// --- layout ---

export function Box(p: { title?: ComponentChildren; note?: ComponentChildren; children?: ComponentChildren; class?: string; style?: Style }) {
  return (
    <section class={cls("box", p.class)} style={p.style}>
      {(p.title || p.note) && (
        <div class="bh">
          {p.title && <b>{p.title}</b>}
          {p.note && <span class="hint">{p.note}</span>}
        </div>
      )}
      {p.children}
    </section>
  );
}

export function Row(p: { label: ComponentChildren; children?: ComponentChildren; top?: boolean; for?: string }) {
  return (
    <div class={cls("row", p.top && "top")}>
      {p.for ? (
        <label class="lab" for={p.for}>
          {p.label}
        </label>
      ) : (
        <span class="lab">{p.label}</span>
      )}
      <div class="line">{p.children}</div>
    </div>
  );
}

export function PageHead(p: { title: ComponentChildren; hint?: ComponentChildren; children?: ComponentChildren }) {
  return (
    <div class="ph">
      <div class="t">
        <span class="title">{p.title}</span>
        {p.hint && <span class="hint">{p.hint}</span>}
      </div>
      <span class="spacer" />
      {p.children}
    </div>
  );
}

export function SubTabs<T extends string>(p: { tabs: { value: T; label: string; count?: number; href: string }[]; value: T }) {
  return (
    <nav class="subtabs">
      {p.tabs.map((t) => (
        <a class={cls("st", t.value === p.value && "on")} href={t.href}>
          {t.label}
          {t.count !== undefined && <span>{t.count}</span>}
        </a>
      ))}
    </nav>
  );
}

// --- choices ---

export interface Option<T> {
  value: T;
  label: ComponentChildren;
  disabled?: boolean;
}

export function Seg<T>(p: { options: Option<T>[]; value: T; on: (v: T) => void; label?: string }) {
  return (
    <span class="seg" role="radiogroup" aria-label={p.label}>
      {p.options.map((o) => (
        <button type="button" role="radio" aria-checked={o.value === p.value} class={cls("opt", o.value === p.value && "on")} disabled={o.disabled} onClick={() => p.on(o.value)}>
          {o.label}
        </button>
      ))}
    </span>
  );
}

export function Bits(p: { options: { bit: number; label: ComponentChildren }[]; value: number; on: (v: number) => void }) {
  return (
    <span class="seg">
      {p.options.map((o) => {
        const on = (p.value & o.bit) !== 0;
        return (
          <button type="button" aria-pressed={on} class={cls("opt", on && "on")} onClick={() => p.on(p.value ^ o.bit)}>
            {o.label}
          </button>
        );
      })}
    </span>
  );
}

// A choice card: a big target with a name, a line under it and maybe a drawing above.
export function Card(p: { on?: boolean; dirty?: boolean; disabled?: boolean; left?: boolean; dashed?: boolean; onClick?: () => void; children?: ComponentChildren; title?: string; style?: Style }) {
  return (
    <button type="button" title={p.title} aria-pressed={!!p.on} class={cls("card", p.on && "on", p.dirty && "dirty", p.left && "left", p.dashed && "dashed")} disabled={p.disabled} onClick={p.onClick} style={p.style}>
      {p.children}
    </button>
  );
}

// --- fields ---

// A text field that keeps what's typed while it has focus; the knob's value shows otherwise.
export function Text(p: { value: string; max: number; on: (v: string) => void; upper?: boolean; placeholder?: string; width?: number; id?: string; mono?: boolean; class?: string; onEnter?: (v: string) => void }) {
  const ref = useRef<HTMLInputElement>(null);
  useEffect(() => {
    const i = ref.current;
    if (i && document.activeElement !== i && i.value !== p.value) i.value = p.value;
  }, [p.value]);
  return (
    <input
      ref={ref}
      id={p.id}
      type="text"
      class={cls("field", p.mono && "mono", p.class)}
      style={p.width ? { width: `${p.width}px` } : undefined}
      defaultValue={p.value}
      maxLength={p.max}
      spellcheck={false}
      autoComplete="off"
      placeholder={p.placeholder}
      onInput={(e) => {
        const i = e.currentTarget;
        if (p.upper !== false) {
          const at = i.selectionStart;
          i.value = i.value.toUpperCase();
          i.setSelectionRange(at, at);
        }
        p.on(i.value);
      }}
      onKeyDown={(e) => {
        e.stopPropagation();
        if (e.key === "Enter") p.onEnter?.(e.currentTarget.value);
      }}
    />
  );
}

export function Num(p: { value: number; min: number; max: number; step?: number; on: (v: number) => void; width?: number; unit?: string; id?: string; empty?: string }) {
  const ref = useRef<HTMLInputElement>(null);
  const shown = p.empty !== undefined && p.value === 0 ? "" : String(p.value);
  useEffect(() => {
    const i = ref.current;
    if (i && document.activeElement !== i) i.value = shown;
  }, [shown]);
  return (
    <span class="line" style={{ gap: "6px" }}>
      <input
        ref={ref}
        id={p.id}
        type="number"
        class="field mono"
        style={{ width: `${p.width ?? 84}px` }}
        defaultValue={shown}
        min={p.min}
        max={p.max}
        step={p.step ?? 1}
        placeholder={p.empty}
        onChange={(e) => {
          const i = e.currentTarget;
          const v = i.value === "" ? 0 : Math.min(p.max, Math.max(p.min, Number(i.value)));
          i.value = p.empty !== undefined && v === 0 ? "" : String(v);
          p.on(v);
        }}
        onKeyDown={(e) => e.stopPropagation()}
      />
      {p.unit && <span class="hint">{p.unit}</span>}
    </span>
  );
}

// --- keys ---

const MOD_GLYPHS: [number, string, string][] = [
  [Mod.CTRL, "⌃", "Control"],
  [Mod.ALT, "⌥", "Option"],
  [Mod.SHIFT, "⇧", "Shift"],
  [Mod.GUI, "⌘", "Command"],
];
const KEY_WORDS: Record<string, string> = { ENTER: "Return", ESC: "Esc", BKSP: "Delete", TAB: "Tab", SPACE: "Space", CAPS: "Caps Lock", DEL: "⌦", LEFT: "←", RIGHT: "→", UP: "↑", DOWN: "↓", PGUP: "Page Up", PGDN: "Page Down", HOME: "Home", END: "End" };

// A shortcut as keycaps: ⌃⌥⇧⌘ then the key (Mac order and names).
export function Keycaps(p: { k: Key | undefined; empty?: string }) {
  const k = p.k;
  if (!k || (!k[0] && !k[1])) return <span class="hint">{p.empty ?? "None"}</span>;
  const m = (k[0] & 0x0f) | ((k[0] & 0xf0) >> 4);
  const name = keyName([0, k[1]]);
  return (
    <span class="keycap" aria-label={keyName(k)}>
      {MOD_GLYPHS.filter(([b]) => m & b).map(([, g, t]) => (
        <b title={t}>{g}</b>
      ))}
      {k[1] ? <b>{KEY_WORDS[name] ?? name}</b> : null}
    </span>
  );
}

// A shortcut: click Record, then press it. The modifiers can also be set by hand -- the system
// takes ⌘Q, ⌘Tab and friends before the page sees them.
export function KeyField(p: { value: Key | undefined; on: (k: Key | undefined) => void }) {
  const [listening, setListening] = useState(false);
  const ref = useRef<HTMLButtonElement>(null);
  const k: Key = p.value ? [p.value[0], p.value[1]] : [0, 0];
  const set = (n: Key) => p.on(n[0] || n[1] ? n : undefined);
  return (
    <span class="line keyfield" style={{ gap: "8px" }}>
      <button
        ref={ref}
        type="button"
        class={cls("keybtn", listening && "listen")}
        onClick={() => {
          setListening(!listening);
          ref.current?.focus();
        }}
        onBlur={() => setListening(false)}
        onKeyDown={(e) => {
          if (!listening) return;
          e.preventDefault();
          e.stopPropagation();
          const code = hidFromCode(e.code);
          if (code === undefined) return; // a modifier alone: wait for the key
          setListening(false);
          set([modifiersFromEvent(e), code]);
        }}
        title="Click, then press the shortcut"
      >
        {listening ? <span class="amber">Press the shortcut…</span> : <Keycaps k={p.value} empty="Click to record" />}
      </button>
      <span class="seg mods">
        {MOD_GLYPHS.map(([b, g, t]) => (
          <button type="button" title={t} aria-pressed={(k[0] & b) !== 0} class={cls("opt", (k[0] & b) !== 0 && "on")} onClick={() => set([k[0] ^ b, k[1]])}>
            {g}
          </button>
        ))}
      </span>
      {(k[0] || k[1]) ? (
        <button type="button" class="x" aria-label="Clear" onClick={() => set([0, 0])}>
          ×
        </button>
      ) : null}
    </span>
  );
}

// A button that asks once: the first click arms it, a second within 3 s does it.
export function Confirm(p: { label: string; ask?: string; on: () => void; class?: string }) {
  const [armed, setArmed] = useState(false);
  useEffect(() => {
    if (!armed) return;
    const t = window.setTimeout(() => setArmed(false), 3000);
    return () => window.clearTimeout(t);
  }, [armed]);
  return (
    <button
      type="button"
      class={cls("btn", p.class ?? "danger", armed && "armed")}
      onClick={() => {
        if (!armed) return setArmed(true);
        setArmed(false);
        p.on();
      }}
    >
      {armed ? (p.ask ?? `${p.label}?`) : p.label}
    </button>
  );
}

// --- slider ---

// A value on a track: drag, scroll, arrow keys, or type it in the box. While dragging, the
// knob's echo doesn't move the thumb back.
export function Slider(p: { label: string; caption?: string; min: number; max: number; step: number; value: number; format: (v: number) => string; on: (v: number) => void; muted?: boolean; dirty?: boolean; track?: string; accent?: boolean }) {
  const [drag, setDrag] = useState<number | null>(null);
  const ref = useRef<HTMLDivElement>(null);
  const v = drag ?? p.value;
  const span = p.max - p.min || 1;
  const pct = Math.max(0, Math.min(100, ((v - p.min) / span) * 100));
  const snap = (x: number) => Math.min(p.max, Math.max(p.min, +(Math.round((x - p.min) / p.step) * p.step + p.min).toFixed(4)));
  const at = (clientX: number) => {
    const r = ref.current!.getBoundingClientRect();
    return snap(p.min + Math.min(1, Math.max(0, (clientX - r.left) / r.width)) * span);
  };
  const put = (x: number) => {
    if (p.muted) return;
    x = snap(x);
    if (x !== v) p.on(x);
    return x;
  };
  return (
    <div class={cls("trow", p.muted && "mute", p.dirty && "dirty")}>
      <div class="tl">
        <b>{p.label}</b>
        {p.caption && <span>{p.caption}</span>}
      </div>
      <div
        ref={ref}
        class="track"
        style={p.track ? { background: p.track } : undefined}
        tabIndex={p.muted ? -1 : 0}
        role="slider"
        aria-label={p.label}
        aria-valuemin={p.min}
        aria-valuemax={p.max}
        aria-valuenow={v}
        onPointerDown={(e) => {
          if (p.muted) return;
          e.currentTarget.setPointerCapture(e.pointerId);
          setDrag(put(at(e.clientX)) ?? null);
        }}
        onPointerMove={(e) => drag !== null && setDrag(put(at(e.clientX)) ?? drag)}
        onPointerUp={() => setDrag(null)}
        onPointerCancel={() => setDrag(null)}
        onWheel={(e) => {
          e.preventDefault();
          put(v + (e.deltaY < 0 || e.deltaX > 0 ? p.step : -p.step));
        }}
        onKeyDown={(e) => {
          if (e.key === "ArrowRight" || e.key === "ArrowUp") put(v + p.step);
          else if (e.key === "ArrowLeft" || e.key === "ArrowDown") put(v - p.step);
          else return;
          e.preventDefault();
        }}
      >
        {!p.track && <div class={cls("fill", p.accent && "am")} style={{ width: `${pct}%` }} />}
        <div class="thumb" style={{ left: `${pct}%` }} />
      </div>
      <ValueBox text={p.muted ? "—" : p.format(v)} disabled={p.muted} on={(t) => {
        const n = parseFloat(t.replace(/[^0-9.\-]/g, ""));
        if (!Number.isNaN(n)) put(n);
      }} />
    </div>
  );
}

function ValueBox(p: { text: string; disabled?: boolean; on: (t: string) => void }) {
  const ref = useRef<HTMLInputElement>(null);
  useEffect(() => {
    const i = ref.current;
    if (i && document.activeElement !== i) i.value = p.text;
  }, [p.text]);
  return (
    <input
      ref={ref}
      class="tv"
      defaultValue={p.text}
      disabled={p.disabled}
      onKeyDown={(e) => {
        e.stopPropagation();
        if (e.key === "Enter") e.currentTarget.blur();
      }}
      onBlur={(e) => {
        p.on(e.currentTarget.value);
        e.currentTarget.value = p.text;
      }}
    />
  );
}

// --- drawings ---

// Steps per turn: a tick per detent; 0 = an unbroken ring (SMOOTH); -1 = dashed (the mode's).
export function Dial(p: { steps: number; size?: number }) {
  const ticks = [];
  for (let i = 1; i < p.steps; i++) {
    const a = (i / p.steps) * 2 * Math.PI - Math.PI / 2, c = Math.cos(a), s = Math.sin(a);
    ticks.push(<line x1={20 + 12 * c} y1={20 + 12 * s} x2={20 + 16 * c} y2={20 + 16 * s} stroke="currentColor" stroke-width="1.6" stroke-linecap="round" />);
  }
  const z = p.size ?? 36;
  return (
    <svg width={z} height={z} viewBox="0 0 40 40" aria-hidden="true">
      {p.steps === 0 && <circle cx="20" cy="20" r="15" fill="none" stroke="currentColor" stroke-width="1.6" />}
      {p.steps < 0 && <circle cx="20" cy="20" r="15" fill="none" stroke="currentColor" stroke-width="1.6" stroke-dasharray="3 4" />}
      {ticks}
      {p.steps > 0 && <circle cx="20" cy="4" r="2.6" fill="var(--amber)" />}
    </svg>
  );
}

// One feel's wave, two periods (SAW bends with SHAPE).
// A motor click as it is played, left to right (the knob draws the same on Haptics > Click):
// its parts summed, each a sine, a square or noise dying away, as wide as it is long, its
// cycles widening where the pitch falls. Fewer cycles than the real click has, so they can be seen.
const NOISE_FLIP = [1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0];
export function ClickWave(p: { wave: ClickWaveDef; on?: boolean; w?: number; h?: number }) {
  const w = p.w ?? 80, h = p.h ?? 28;
  const total = Math.max(...p.wave.parts.map((q) => (q.delay ?? 0) + q.len));
  const cycles = total <= 3 ? 3 : total <= 6 ? 5 : 8;
  const span = total <= 3 ? w / 2 : total <= 6 ? (w * 3) / 4 : w;
  const end = 0.6;
  let d = "";
  for (let x = 0; x <= span; x += 0.5) {
    const t = (x / span) * total;
    let v = 0;
    p.wave.parts.forEach((q, i) => {
      const from = q.delay ?? 0;
      if (t < from || t > from + q.len) return;
      const u = (t - from) / q.len;
      const n = ((cycles * q.len) / total) * (q.pitch ?? 1);
      const ph = q.chirp ? (n * (1 - Math.pow(end, u))) / -Math.log(end) : n * u;
      let s = Math.sin(2 * Math.PI * ph);
      if (q.wave === "square") s = s >= 0 ? 1 : -1;
      if (q.wave === "noise") s *= NOISE_FLIP[(Math.floor(ph) + i * 5) & 15] ? 1 : -1; // each cycle upside down or not
      v += s * (q.level ?? 1) * Math.exp(-(t - from) / q.tau);
    });
    v = Math.max(-1, Math.min(1, v));
    d += `${x ? "L" : "M"}${x.toFixed(1)} ${(h / 2 - v * (h / 2 - 3)).toFixed(1)}`;
  }
  if (span < w) d += `L${span.toFixed(1)} ${h / 2}L${w} ${h / 2}`;
  return (
    <svg width={w} height={h} viewBox={`0 0 ${w} ${h}`} aria-hidden="true">
      <path d={d} fill="none" stroke={p.on ? "var(--amber)" : "currentColor"} stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round" />
    </svg>
  );
}

export function Wave(p: { feel: number; shape?: number; on?: boolean; w?: number; h?: number }) {
  const w = p.w ?? 80, h = p.h ?? 28;
  let d = "";
  for (let x = 0; x <= w; x++) d += `${x ? "L" : "M"}${x} ${(h / 2 - waveY(p.feel, (x / w) * 2, p.shape ?? 0) * (h / 2 - 3)).toFixed(1)}`;
  return (
    <svg width={w} height={h} viewBox={`0 0 ${w} ${h}`} aria-hidden="true">
      <path d={d} fill="none" stroke={p.on ? "var(--amber)" : "currentColor"} stroke-width="2.2" stroke-linejoin="round" stroke-linecap="round" />
    </svg>
  );
}
