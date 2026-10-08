// LOOK › Screen & music: the screen's brightness, the screensaver, and the sleep hours
// (user_prefs.h screen_t, EXT_CMD_IDLE from extensions v13). Live on the knob while you change
// them, kept by Save like LIGHTS. The sleep hours need the knob to know the local time: the
// Quadra service on this computer sends it, and without it the knob stays on as by day.

import { useSignal } from "@preact/signals";
import { useRef } from "preact/hooks";
import { DARK_AFTER, EXT_IDLE_VERSION, SAVER_AFTER, SAVERS, Saver, Wake, type ScreenPrefs } from "../../proto";
import { device, use } from "../../store";
import { Box, Card, cls, Row, Slider } from "../../ui/controls";
import { titleCase } from "../../ui/shell";

const SAVER_SUB: Record<number, string> = {
  [Saver.AUTO]: "What's playing, else the clock, else the icon",
  [Saver.ICON]: "The profile's icon or your idle word, jumping",
  [Saver.BOUNCE]: "The icon bouncing round the glass over stars",
  [Saver.CLOCK]: "The time, big and dim, moving a little each minute",
  [Saver.MUSIC]: "The cover, title and artist of what's playing",
  [Saver.BLANK]: "The screen turns off",
  [Saver.NEVER]: "The screen stays as it is",
};

export const secs = (s: number) => (s === 0 ? "Right away" : s < 60 ? `${s} s` : `${s / 60} min`);
export const hhmm = (m: number) => `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(m % 60).padStart(2, "0")}`;
const TIMES = Array.from({ length: 96 }, (_, i) => i * 15);

export function Screensaver() {
  use("idle", "conn");
  // The whole screen goes out each time (the knob takes one at a time), at most every 40 ms; what's
  // on its way shows until the knob says it has it -- as LIGHTS does.
  const want = useRef<{ s: ScreenPrefs; at: number } | null>(null);
  const timer = useRef(0);
  const sentAt = useRef(0);
  const tick = useSignal(0);
  if ((device.ext ?? 0) < EXT_IDLE_VERSION) return null;
  const idle = device.idle;
  if (!idle) return <Box><span class="hint">Reading the screen…</span></Box>;
  const w = want.current;
  const same = (a: ScreenPrefs, b: ScreenPrefs) =>
    a.bright === b.bright && a.saver === b.saver && a.saverS === b.saverS && a.sleepOn === b.sleepOn && a.sleepFrom === b.sleepFrom
    && a.sleepTo === b.sleepTo && a.darkS === b.darkS && a.wake === b.wake;
  if (w && (same(w.s, idle) || performance.now() - w.at > 2000)) want.current = null;
  const s = want.current?.s ?? idle;
  void tick.value;
  const dirty = idle.dirty || want.current !== null;
  const change = (patch: Partial<ScreenPrefs>) => {
    const next = { ...(want.current?.s ?? idle), ...patch };
    want.current = { s: next, at: performance.now() };
    tick.value++;
    const go = () => {
      sentAt.current = performance.now();
      if (want.current) void device.setIdle(want.current.s);
    };
    window.clearTimeout(timer.current);
    const since = performance.now() - sentAt.current;
    if (since >= 40) go();
    else timer.current = window.setTimeout(go, 40 - since);
  };
  const status = !s.sleepOn
    ? ""
    : !idle.trusted
      ? "Paused: the knob doesn't know the local time. Run the Quadra service on this computer; until then the knob stays on."
      : idle.dark
        ? "Sleeping now: the screen is dark."
        : idle.sleeping
          ? "In the sleep hours now."
          : "";
  return (
    <>
      <Box title="Brightness" note="The screen's backlight · Save keeps it">
        <Slider label="Brightness" caption="The LEDs have their own, under Lights" min={10} max={100} step={10} value={s.bright} format={(v) => `${v}%`} on={(v) => change({ bright: v })} dirty={dirty} />
      </Box>
      <Box title="Screensaver" note="When the knob is left alone · Save keeps it">
        <div class="cards">
          {SAVERS.map((name, i) => (
            <Card left on={s.saver === i} dirty={s.saver === i && dirty} onClick={() => change({ saver: i })}>
              <span class="nm">{titleCase(name)}</span>
              <span class="sub">{SAVER_SUB[i]}</span>
            </Card>
          ))}
        </div>
        <Row label="Starts after" for="saver-after">
          <select id="saver-after" class="field" value={String(s.saverS)} onChange={(e) => change({ saverS: Number(e.currentTarget.value) })}>
            {SAVER_AFTER.map((v) => (
              <option value={String(v)}>{secs(v)}</option>
            ))}
          </select>
          <span class="hint">Without a turn or a key</span>
        </Row>
      </Box>
      <Box title="Sleep hours" note="Screen and lights off at night · Save keeps it">
        <Row label="Sleep">
          <button type="button" class="chk" role="switch" aria-checked={s.sleepOn} onClick={() => change({ sleepOn: !s.sleepOn })}>
            <span class={cls("toggle", s.sleepOn && "on")} />
            {s.sleepOn ? "On" : "Off"}
          </button>
          {status && <span class="hint">{status}</span>}
        </Row>
        <Row label="From" for="sleep-from">
          <select id="sleep-from" class="field" disabled={!s.sleepOn} value={String(s.sleepFrom)} onChange={(e) => change({ sleepFrom: Number(e.currentTarget.value) })}>
            {TIMES.map((m) => (
              <option value={String(m)}>{hhmm(m)}</option>
            ))}
          </select>
          <span class="hint">to</span>
          <select id="sleep-to" class="field" aria-label="To" disabled={!s.sleepOn} value={String(s.sleepTo)} onChange={(e) => change({ sleepTo: Number(e.currentTarget.value) })}>
            {TIMES.map((m) => (
              <option value={String(m)}>{hhmm(m)}</option>
            ))}
          </select>
        </Row>
        <Row label="Goes dark" for="dark-after">
          <select id="dark-after" class="field" disabled={!s.sleepOn} value={String(s.darkS)} onChange={(e) => change({ darkS: Number(e.currentTarget.value) })}>
            {DARK_AFTER.map((v) => (
              <option value={String(v)}>{v === 0 ? "With the screensaver" : `${secs(v)} into the screensaver`}</option>
            ))}
          </select>
        </Row>
        <Row label="Woken">
          <div class="cards narrow">
            <Card left on={s.wake === Wake.NORMAL} dirty={s.wake === Wake.NORMAL && dirty} disabled={!s.sleepOn} onClick={() => change({ wake: Wake.NORMAL })}>
              <span class="nm">Normal</span>
              <span class="sub">Full brightness</span>
            </Card>
            <Card left on={s.wake === Wake.DIM} dirty={s.wake === Wake.DIM && dirty} disabled={!s.sleepOn} onClick={() => change({ wake: Wake.DIM })}>
              <span class="nm">Dim</span>
              <span class="sub">A night level for the screen and lights</span>
            </Card>
          </div>
        </Row>
        <span class="hint">In the sleep hours only a turn or a key wakes the knob. Notifications and tracks wait until then.</span>
      </Box>
    </>
  );
}
