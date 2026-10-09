// Knob & keys: the five inputs as a row (what each does, its haptic profile), and the one picked
// below it. An input picks one of the five haptic profiles and nothing else -- its feel and
// strength are the haptic profile's (tuned under Haptics).

import { BUTTON_NAMES, MAX, MEDIA_USAGES, Mod, type Action, type Kind, type ProfileJson, type SlotName, SLOTS } from "../../profile";
import { HapticProfiles } from "../../proto";
import { href } from "../../store";
import { Bits, Box, Card, cls, Dial, KeyField, Num, Row, Seg, Text } from "../../ui/controls";
import { titleCase } from "../../ui/shell";
import { inputHaptic, setInputHaptic, SMOOTH, type Session } from "./session";

export const TITLE: Record<SlotName, string> = { knob: "Knob", f1: "F1", f2: "F2", f3: "F3", f4: "F4" };
const NOTE: Record<SlotName, string> = {
  knob: "Turning it, no key held",
  f1: "Hold F1 and turn, or tap it",
  f2: "Hold F2 and turn, or tap it",
  f3: "Hold F3 and turn, or tap it",
  f4: "Hold F4 and turn (held still, it opens the menu)",
};
const KIND_LABEL: Record<Kind, string> = { none: "Off", wheel: "Scroll", drag: "Drag", keys: "Keys", tap: "Tap", commands: "Wheel menu", media: "Media" };
// What each input may do (app_profile.h): the knob only turns; F4 is also the menu key, so it
// has no press actions. MEDIA on a key fires on press, like TAP.
const KINDS_FOR: Record<SlotName, Kind[]> = {
  knob: ["none", "wheel", "drag", "keys", "media"],
  f1: ["none", "wheel", "drag", "keys", "tap", "commands", "media"],
  f2: ["none", "wheel", "drag", "keys", "tap", "commands", "media"],
  f3: ["none", "wheel", "drag", "keys", "tap", "commands", "media"],
  f4: ["none", "wheel", "drag", "keys"],
};
const FINE = Mod.SHIFT | Mod.ALT; // on a volume key: a quarter step on a Mac
const MOD_OPTS = [
  { bit: Mod.CTRL, label: "⌃ Control" },
  { bit: Mod.ALT, label: "⌥ Option" },
  { bit: Mod.SHIFT, label: "⇧ Shift" },
  { bit: Mod.GUI, label: "⌘ Command" },
];
const mediaName = (u: number | undefined) => titleCase(MEDIA_USAGES.find((m) => m.usage === u)?.label ?? "");
export const turns = (k: Kind | undefined) => k !== undefined && k !== "none" && k !== "tap";

// A line about what an input does, for its tile.
function summary(p: ProfileJson, name: SlotName): string {
  const a = p.slots?.[name];
  const k = a?.kind ?? "none";
  const tap = a?.tap_macro ? `tap: ${a.tap_macro}` : a?.tap ? "tap: a key" : "";
  switch (k) {
    case "none":
      return tap ? titleCase(tap) : "Not used";
    case "commands":
      return `${p.rings?.length ?? 0} ring${p.rings?.length === 1 ? "" : "s"}`;
    case "media":
      return name === "knob" ? "Volume" : mediaName(a?.cw?.[1]);
    case "tap":
      return titleCase(a?.label || a?.macro || "a key");
    default:
      return titleCase(a?.label ?? "") || KIND_LABEL[k];
  }
}

export function KeysTab(x: { s: Session; p: ProfileJson; input: string }) {
  const { s, p } = x;
  const name = (SLOTS as readonly string[]).includes(x.input) ? (x.input as SlotName) : "knob";
  p.slots ??= {};
  return (
    <>
      <div class="inputs">
        {SLOTS.map((n) => {
          const a = p.slots![n];
          const k = a?.kind ?? "none";
          const h = turns(k) ? inputHaptic(a) : null;
          return (
            <a class={cls("inp", n === name && "on")} href={href({ page: "profile", id: p.id, tab: "keys", input: n })} aria-current={n === name ? "true" : undefined}>
              <span class="k">
                {TITLE[n]}
                <span>{h === null ? "—" : h < 0 ? "Default" : titleCase(HapticProfiles[h].name)}</span>
              </span>
              <span class={k === "none" ? "what off" : "what"}>{KIND_LABEL[k]}</span>
              <span class="det">{summary(p, n)}</span>
            </a>
          );
        })}
      </div>
      <Input key={name} s={s} p={p} name={name} />
    </>
  );
}

function Input(x: { s: Session; p: ProfileJson; name: SlotName }) {
  const { s, p, name } = x;
  void s.rev.value;
  const a = (): Action => (p.slots![name] ??= {});
  const act = p.slots![name];
  const kind = act?.kind ?? "none";
  const t = s.touch.bind(s);

  const pickKind = (k: Kind) => {
    const y = a();
    const was = y.kind;
    y.kind = k;
    // Sensible starting values for what's new.
    if ((k === "wheel" || k === "drag") && !y.sign) y.sign = 1;
    if (k === "drag") {
      y.buttons ||= 4;
      y.px_per_rad ||= 120;
      if (was !== "drag" && y.feel === undefined && !y.detents) setInputHaptic(y, SMOOTH);
    }
    if (k === "commands") {
      if (y.feel === "viscose") setInputHaptic(y, -1); // a wheel needs steps
      p.rings = p.rings?.length ? p.rings : [{ name: "COMMANDS", tab: "CMDS", slot: name, cmds: [{ name: "UNDO", key: [8, 29] }] }];
    }
    if (k === "media" && was !== "media") {
      // As the MUSIC profile has them: the knob is a fine volume dial, a key plays / pauses.
      if (name === "knob") {
        y.cw = [FINE, 0xe9];
        y.ccw = [FINE, 0xea];
      } else {
        y.cw = [0, 0xcd];
      }
      y.tap = y.tap_macro = undefined;
    }
    t();
  };

  const x_ = act ?? {};
  const dir = (
    <Row label="Direction">
      <Seg
        options={[
          { value: 1, label: "Normal" },
          { value: -1, label: "Reversed" },
        ]}
        value={x_.sign ?? 1}
        on={(v) => ((a().sign = v), t())}
      />
    </Row>
  );
  const mods = (
    <Row label="Holding">
      <Bits options={MOD_OPTS} value={x_.modifier ?? 0} on={(v) => ((a().modifier = v), t())} />
    </Row>
  );

  return (
    <Box title={TITLE[name]} note={NOTE[name]} class="inspector">
      <Row label="Does">
        <Seg options={KINDS_FOR[name].map((k) => ({ value: k, label: KIND_LABEL[k] }))} value={kind} on={pickKind} label="What it does" />
      </Row>
      {kind !== "none" && (
        <Row label="Name on screen" for="in-label">
          <Text id="in-label" value={x_.label ?? ""} max={MAX.label} placeholder="Optional" on={(v) => ((a().label = v || undefined), t())} />
        </Row>
      )}
      {kind === "wheel" && (
        <>
          {mods}
          {dir}
        </>
      )}
      {kind === "drag" && (
        <>
          <Row label="Mouse button">
            <Bits options={BUTTON_NAMES.map(([bit, label]) => ({ bit, label: titleCase(label) }))} value={x_.buttons ?? 0} on={(v) => ((a().buttons = v), t())} />
          </Row>
          {mods}
          <Row label="Axis">
            <Seg
              options={[
                { value: false, label: "Left / right" },
                { value: true, label: "Up / down" },
              ]}
              value={!!x_.axis_y}
              on={(v) => ((a().axis_y = v), t())}
            />
          </Row>
          <Row label="Speed" for="in-speed">
            <Num id="in-speed" value={x_.px_per_rad ?? 120} min={10} max={2000} step={10} unit="pixels per radian" on={(v) => ((a().px_per_rad = v), t())} />
          </Row>
          {dir}
        </>
      )}
      {kind === "keys" && (
        <>
          <Row label="Turn right">
            <KeyField value={x_.cw} on={(k) => ((a().cw = k), t())} />
          </Row>
          <Row label="Turn left">
            <KeyField value={x_.ccw} on={(k) => ((a().ccw = k), t())} />
          </Row>
        </>
      )}
      {kind === "tap" && <KeyOrMacro label="Sends" p={p} x={a()} keyField="cw" macroField="macro" touch={t} />}
      {kind === "commands" && (
        <Row label="Wheel">
          <span class="hint">
            Hold to open the wheel, turn to pick, let go to run. <a href={href({ page: "profile", id: p.id, tab: "wheel", input: name })}>Edit its rings</a>
          </span>
        </Row>
      )}
      {kind === "media" &&
        (name === "knob" ? (
          <>
            <Row label="Turn right">
              <Seg options={MEDIA_USAGES.map((m) => ({ value: m.usage as number, label: titleCase(m.label) }))} value={x_.cw?.[1] ?? 0} on={(v) => ((a().cw = [a().cw?.[0] ?? 0, v]), t())} />
            </Row>
            <Row label="Turn left">
              <Seg options={MEDIA_USAGES.map((m) => ({ value: m.usage as number, label: titleCase(m.label) }))} value={x_.ccw?.[1] ?? 0} on={(v) => ((a().ccw = [a().ccw?.[0] ?? 0, v]), t())} />
            </Row>
            <Row label="Volume step">
              <Seg
                options={[
                  { value: true, label: "Fine" },
                  { value: false, label: "Normal" },
                ]}
                value={!!((x_.cw?.[0] ?? 0) & FINE)}
                on={(v) => {
                  const y = a();
                  for (const f of ["cw", "ccw"] as const) if (y[f]) y[f] = [v ? FINE : 0, y[f]![1]];
                  t();
                }}
              />
              <span class="hint">Fine: a quarter step on a Mac (⇧⌥)</span>
            </Row>
          </>
        ) : (
          <Row label="Sends">
            <Seg options={MEDIA_USAGES.map((m) => ({ value: m.usage as number, label: titleCase(m.label) }))} value={x_.cw?.[1] ?? 0} on={(v) => ((a().cw = [0, v]), t())} />
            <span class="hint">On press, to whatever is playing</span>
          </Row>
        ))}
      {turns(kind) && !(kind === "media" && name !== "knob") && (
        <Row label="Haptic profile" top>
          <div class="stack" style={{ gap: "8px" }}>
            <div class="cards hap">
              {[-1, ...HapticProfiles.map((_, i) => i)].map((h) => {
                const cur = inputHaptic(act);
                const no = kind === "commands" && h === SMOOTH;
                return (
                  <Card on={cur === h} disabled={no} title={no ? "A wheel menu needs steps" : undefined} onClick={() => (setInputHaptic(a(), h), t())}>
                    <Dial steps={h < 0 ? -1 : HapticProfiles[h].detents} size={28} />
                    <span class="nm">{h < 0 ? "Mode default" : titleCase(HapticProfiles[h].name)}</span>
                    <span class="sub">{h < 0 ? "The App mode's" : HapticProfiles[h].detents ? `${HapticProfiles[h].detents} per turn` : "No steps"}</span>
                  </Card>
                );
              })}
            </div>
            <span class="hint">
              The feel and the strength come from the haptic profile. <a href="#/haptics">Tune them under Haptics</a>
            </span>
          </div>
        </Row>
      )}
      {name !== "knob" && name !== "f4" && kind !== "tap" && kind !== "media" && <KeyOrMacro label="Quick tap" hint="Pressed and let go without turning" p={p} x={a()} keyField="tap" macroField="tap_macro" touch={t} />}
    </Box>
  );
}

// A key or one of the profile's macros (choosing one clears the other).
function KeyOrMacro(x: { label: string; hint?: string; p: ProfileJson; x: Action; keyField: "cw" | "tap"; macroField: "macro" | "tap_macro"; touch: () => void }) {
  const y = x.x;
  const mode = y[x.macroField] !== undefined ? "macro" : "key";
  const names = (x.p.macros ?? []).map((m) => m.name).filter(Boolean);
  return (
    <Row label={x.label}>
      <Seg
        options={[
          { value: "key" as const, label: "Key" },
          { value: "macro" as const, label: "Macro" },
        ]}
        value={mode}
        on={(m) => {
          if (m === "key") y[x.macroField] = undefined;
          else y[x.macroField] = names[0] ?? "";
          x.touch();
        }}
      />
      {mode === "key" ? (
        <KeyField value={y[x.keyField]} on={(k) => ((y[x.keyField] = k), (y[x.macroField] = undefined), x.touch())} />
      ) : names.length ? (
        <Seg options={names.map((n) => ({ value: n, label: titleCase(n) }))} value={y[x.macroField] ?? ""} on={(n) => ((y[x.macroField] = n), (y[x.keyField] = undefined), x.touch())} />
      ) : (
        <span class="hint">No macros yet: add one under Macros</span>
      )}
      {x.hint && mode === "key" && <span class="hint">{x.hint}</span>}
    </Row>
  );
}

