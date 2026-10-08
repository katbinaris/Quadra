// LOOK: the lights, the knob's screen (idle word, rotation, MUSIC's cover) and the CLOCK app.
// LIGHTS and rotation are live while you change them and kept by Save; the idle word, the cover
// and the clock are stored on the knob at once (the firmware's extensions, ext_proto.h).

import { useSignal } from "@preact/signals";
import { useRef } from "preact/hooks";
import { serviceName } from "../platform";
import { CLOCK_SLOTS, COVER_STYLES, ClockFlag, EXT_CLOCK_VERSION, IDLE_TEXT_MAX, LIGHT_FX, LIGHT_FX_MOVING, LightSrc, Set, type Lights } from "../proto";
import { device, use } from "../store";
import { CITIES } from "../tzdata";
import { Box, Card, cls, PageHead, Row, Slider, SubTabs, Text } from "../ui/controls";
import { titleCase } from "../ui/shell";

const TABS = [
  { value: "lights" as const, label: "Lights", href: "#/look/lights" },
  { value: "screen" as const, label: "Screen & music", href: "#/look/screen" },
  { value: "clock" as const, label: "Clock", href: "#/look/clock" },
];

export function LookPage(p: { tab: "lights" | "screen" | "clock" }) {
  use("conn");
  const ext = device.ext ?? 0;
  return (
    <>
      <PageHead title="Look" hint="The lights, the knob's screen, the cover for music and the clock" />
      <SubTabs tabs={TABS} value={p.tab} />
      {p.tab === "screen" ? <ScreenTab /> : !ext ? <NeedsExt /> : p.tab === "clock" ? <ClockTab /> : <LightsTab />}
    </>
  );
}

function NeedsExt() {
  return (
    <Box>
      <span class="hint">This needs Quadra firmware with the extensions (2.0 or later). The firmware on this knob doesn't have them.</span>
    </Box>
  );
}

// --- lights ---

const hsl = (hue: number, sat: number) => `hsl(${hue} ${sat}% ${50 + (100 - sat) / 4}%)`;
const FX_SWATCH = [
  (c: string) => `conic-gradient(${c}, #fff6, ${c})`,
  (c: string) => c,
  (c: string) => `radial-gradient(${c}, #0b0c0d)`,
  (c: string) => `conic-gradient(${c} 0 25%, #2a2c30 25%)`,
  () => "conic-gradient(#ff4d4d,#ffd24d,#5ad17a,#4dd2ff,#6b6bff,#ff4dd2,#ff4d4d)",
  () => "transparent",
];

function LightsTab() {
  use("prefs");
  // The whole look goes out each time (the knob takes one LIGHTS at a time), at most every
  // 40 ms; what's on its way shows until the knob says it has it.
  const want = useRef<{ l: Lights; at: number } | null>(null);
  const timer = useRef(0);
  const sentAt = useRef(0);
  const tick = useSignal(0);
  const pr = device.prefs;
  if (!pr) return <Box><span class="hint">Reading the lights…</span></Box>;
  const w = want.current;
  if (w && ((w.l.src === pr.src && w.l.fx === pr.fx && w.l.hue === pr.hue && w.l.sat === pr.sat && w.l.speed === pr.speed && w.l.level === pr.level) || performance.now() - w.at > 2000)) want.current = null;
  const l = want.current?.l ?? pr;
  void tick.value;
  const dirty = pr.lightsDirty || want.current !== null;
  const change = (patch: Partial<Lights>) => {
    const next = { ...(want.current?.l ?? pr), ...patch };
    want.current = { l: next, at: performance.now() };
    tick.value++;
    const go = () => {
      sentAt.current = performance.now();
      if (want.current) void device.setLights(want.current.l);
    };
    window.clearTimeout(timer.current);
    const since = performance.now() - sentAt.current;
    if (since >= 40) go();
    else timer.current = window.setTimeout(go, 40 - since);
  };
  const custom = l.src === LightSrc.CUSTOM;
  const color = custom ? hsl(l.hue, l.sat) : "#ffb21a";
  return (
    <>
      <Box title="Color" note="The ring and the keys">
        <div class="cards narrow">
          <Card left on={l.src === LightSrc.APP} dirty={l.src === LightSrc.APP && dirty} onClick={() => change({ src: LightSrc.APP })}>
            <span class="nm">From the app</span>
            <span class="sub">The profile's colors, or the album cover's</span>
          </Card>
          <Card left on={custom} dirty={custom && dirty} onClick={() => change({ src: LightSrc.CUSTOM })}>
            <span class="nm">Custom</span>
            <span class="sub">Your own hue</span>
          </Card>
        </div>
        <Slider label="Hue" caption="0 to 359" min={0} max={355} step={5} value={l.hue} format={(v) => `${v}`} on={(v) => change({ hue: v })} muted={!custom} dirty={dirty} track="linear-gradient(90deg,#ff4d4d,#ffd24d,#5ad17a,#4dd2ff,#6b6bff,#ff4dd2,#ff4d4d)" />
        <Slider label="Saturation" caption="Pale to full" min={0} max={100} step={5} value={l.sat} format={(v) => `${v}%`} on={(v) => change({ sat: v })} muted={!custom} dirty={dirty} track={`linear-gradient(90deg, ${hsl(l.hue, 0)}, ${hsl(l.hue, 100)})`} />
      </Box>
      <Box title="Effect" note="When the knob is at rest">
        <div class="cards">
          {LIGHT_FX.map((name, i) => (
            <Card on={l.fx === i} dirty={l.fx === i && dirty} onClick={() => change({ fx: i })}>
              <span class={cls("swatch", i === LIGHT_FX.length - 1 && "off")} style={{ background: FX_SWATCH[i]?.(color) }} />
              <span class="nm">{titleCase(name)}</span>
            </Card>
          ))}
        </div>
        <Slider label="Speed" caption="Breathe, Spin, Rainbow" min={1} max={10} step={1} value={l.speed} format={(v) => `${v}`} on={(v) => change({ speed: v })} muted={!LIGHT_FX_MOVING.has(l.fx)} dirty={dirty} />
        <Slider label="Brightness" caption="Within the USB power budget" min={10} max={200} step={10} value={l.level} format={(v) => `${v}%`} on={(v) => change({ level: v })} dirty={dirty} />
      </Box>
    </>
  );
}

// --- screen & music ---

const COVER_SUB = ["The cover, full screen", "The glass is the record", "A sleeve; the record slides out", "A big sleeve slides away"];

function ScreenTab() {
  use("settings", "prefs", "conn");
  const s = device.settings!;
  const pr = device.prefs;
  const typed = useRef<string | null>(null);
  const setWord = (w: string) => {
    typed.current = null;
    void device.setIdleText(w.replace(/[^\x20-\x7e]/g, "").trim());
  };
  const rotDirty = ((s.dirty >> Set.ROTATION) & 1) === 1;
  return (
    <>
      {pr && (
        <Box title="Idle word" note="On the idle screen · saved on the knob at once">
          <div class="line">
            <Text value={typed.current ?? pr.idleText} max={IDLE_TEXT_MAX} upper={false} placeholder="QUADRA" width={260} class="pixel" on={(v) => (typed.current = v)} onEnter={setWord} />
            <button class="btn primary" onClick={() => setWord(typed.current ?? pr.idleText)}>
              Set
            </button>
            <button class="btn ghost" onClick={() => setWord("")}>
              Back to QUADRA
            </button>
            <span class="hint">Up to {IDLE_TEXT_MAX} characters</span>
          </div>
        </Box>
      )}
      {pr && pr.coverStyles > 0 && (
        <Box title="Music cover" note="The now-playing screen · saved on the knob at once">
          <div class="cards">
            {COVER_STYLES.map((name, i) => (
              <Card left on={pr.coverStyle === i} onClick={() => void device.setCoverStyle(i)}>
                <span class={`cover c${i}`} aria-hidden="true">
                  <i />
                  <i />
                </span>
                <span class="nm">{titleCase(name)}</span>
                <span class="sub">{COVER_SUB[i]}</span>
              </Card>
            ))}
          </div>
          <span class="hint">On the knob: tap F4 while music plays to step through them.</span>
        </Box>
      )}
      <Box title="Screen rotation" note="Which way is up · Save keeps it">
        <div class="cards narrow">
          {[0, 1, 2, 3].map((q) => (
            <Card on={s.rotation === q} dirty={s.rotation === q && rotDirty} onClick={() => device.set(Set.ROTATION, q)}>
              <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" style={{ transform: `rotate(${q * 90}deg)` }} aria-hidden="true">
                <path d="M12 20V5M6 11l6-6 6 6" />
              </svg>
              <span class="sub">{q * 90}°</span>
            </Card>
          ))}
        </div>
      </Box>
    </>
  );
}

// --- clock ---

const utc = (min: number) => {
  const a = Math.abs(min);
  return `UTC${min < 0 ? "−" : "+"}${Math.floor(a / 60)}${a % 60 ? `:${String(a % 60).padStart(2, "0")}` : ""}`;
};
const FORMAT = [
  { bit: ClockFlag.H24, label: "24-hour" },
  { bit: ClockFlag.SECONDS, label: "Seconds" },
  { bit: ClockFlag.DATE, label: "Date" },
  { bit: ClockFlag.LED, label: "Seconds on the LED ring" },
  { bit: ClockFlag.BOARD, label: "Board: every zone at once" },
];

function ClockTab() {
  use("clock", "conn");
  if ((device.ext ?? 0) < EXT_CLOCK_VERSION) return <NeedsExt />;
  const c0 = device.clockSlots[0];
  return (
    <>
      <Box title="Clock app" note="Saved on the knob as you change it">
        {c0 && (
          <Row label="Show">
            {FORMAT.map((f) => {
              const on = (c0.flags & f.bit) !== 0;
              return (
                <button type="button" class="chk" role="switch" aria-checked={on} onClick={() => void device.setClockFlags(c0.flags ^ f.bit)}>
                  <span class={cls("toggle", on && "on")} />
                  {f.label}
                </button>
              );
            })}
          </Row>
        )}
        <Row label="Local time">
          {c0?.valid ? (
            <>
              <span class="mono">{c0.label}</span>
              <span class="tag">{utc(c0.offsetMin)}</span>
              <span class="hint">From this computer</span>
            </>
          ) : (
            <span class="hint">Not set yet: connect Wi-Fi, or run {serviceName}</span>
          )}
        </Row>
      </Box>
      <Box title="World zones" note="Turn the knob in the Clock app to step through them">
        {Array.from({ length: CLOCK_SLOTS - 1 }, (_, k) => k + 1).map((slot) => {
          const z = device.clockSlots[slot];
          const i = z?.label ? CITIES.findIndex((c) => c.label === z.label && c.rule === z.rule) : -1;
          const value = !z?.label ? "" : i >= 0 ? String(i) : "other";
          return (
            <Row label={`Zone ${slot}`} for={`zone${slot}`}>
              <select
                id={`zone${slot}`}
                class="field"
                value={value}
                onChange={(e) => {
                  const v = e.currentTarget.value;
                  const c = v === "" ? null : CITIES[Number(v)];
                  if (c || v === "") void device.setClockZone(slot, c ? c.label : "", c ? c.rule : "");
                }}
              >
                <option value="">None</option>
                {CITIES.map((c, k) => (
                  <option value={String(k)}>{c.city}</option>
                ))}
                {value === "other" && <option value="other">{z!.label}</option>}
              </select>
              {z?.label && <span class="tag">{utc(z.offsetMin)}</span>}
            </Row>
          );
        })}
      </Box>
      <span class="hint">On the knob, in the Clock app: F1 switches 12 / 24 hours, F2 seconds, F3 the date.</span>
    </>
  );
}
