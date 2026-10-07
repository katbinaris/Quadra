// HAPTICS: the five haptic profiles and, for the one picked, its feel and tuning. Each keeps its
// own values per feel, inside limits the knob sets, so it can't be tuned into instability.
// Live on the knob while you drag; Save (or F2 on the knob) keeps them.

import { ClickWaves, Feel, HapticProfiles, Limits, Set, type SetId } from "../proto";
import { device, use } from "../store";
import { Box, Card, ClickWave, Dial, PageHead, Slider, Wave } from "../ui/controls";
import { titleCase } from "../ui/shell";
import { throttled } from "../ui/throttle";

const send = throttled((id: SetId, v: number) => device.set(id, v));

const FEELS = [
  { value: Feel.SAW, name: "Saw", sub: "Crisp snap" },
  { value: Feel.SINE, name: "Sine", sub: "Round bump" },
  { value: Feel.VISCOSE, name: "Viscose", sub: "Smooth drag" },
];

export function HapticsPage() {
  use("settings");
  const s = device.settings!;
  const bit = (id: number) => ((s.dirty >> id) & 1) === 1;
  const prof = HapticProfiles[s.hapticProfile];
  const name = titleCase(prof?.name ?? "");
  const feelName = FEELS.find((f) => f.value === s.feel)?.name ?? "";
  return (
    <>
      <PageHead title="Haptics" hint="Five haptic profiles. Every input of every app profile uses one of them, so each feel is tuned once.">
        <button class="btn ghost" onClick={() => device.resetHaptic()}>
          Reset {name} to factory
        </button>
      </PageHead>
      <Box title="Haptic profile" note="Pick one to tune it; the knob takes it on while you do">
        <div class="cards">
          {HapticProfiles.map((h, i) => (
            <Card on={s.hapticProfile === i} dirty={s.hapticProfile === i && bit(Set.HAPTIC_PROFILE)} onClick={() => device.set(Set.HAPTIC_PROFILE, i)}>
              <Dial steps={h.detents} size={40} />
              <span class="nm">{titleCase(h.name)}</span>
              <span class="sub">{h.detents ? `${h.detents} per turn` : "No steps · Viscose"}</span>
            </Card>
          ))}
        </div>
      </Box>
      <div class="split">
        <Box title="Feel" note="How a step pushes back">
          <div class="stack">
            {FEELS.map((f) => {
              const allowed = ((s.feels >> f.value) & 1) === 1;
              return (
                <Card left on={s.feel === f.value} dirty={s.feel === f.value && bit(Set.FEEL)} disabled={!allowed} title={allowed ? undefined : `${name} doesn't have this feel`} onClick={() => device.set(Set.FEEL, f.value)} style={{ flexDirection: "row", gap: "14px" }}>
                  <Wave feel={f.value} shape={s.shape / 100} on={s.feel === f.value} />
                  <span class="cardtext">
                    <span class="nm">{f.name}</span>
                    <span class="sub">{f.sub}</span>
                  </span>
                </Card>
              );
            })}
          </div>
        </Box>
        <Box title={`Tune ${name} · ${feelName}`} note="Drag, scroll, or type a value">
          <div class="stack" style={{ gap: "18px" }}>
            <Slider label="Snap" caption="Spring strength (kp)" min={s.kpMin} max={s.kpMax} step={Limits.kp.step} value={s.kp} format={(v) => v.toFixed(2)} on={(v) => send(Set.KP, v)} muted={s.feel === Feel.VISCOSE} dirty={bit(Set.KP)} />
            <Slider label="Damp" caption="Damping (kd)" min={s.kdMin} max={s.kdMax} step={Limits.kd.step} value={s.kd} format={(v) => v.toFixed(3)} on={(v) => send(Set.KD, v)} dirty={bit(Set.KD)} />
            <Slider label="Shape" caption="Saw only" min={Limits.shape.min} max={Limits.shape.max} step={Limits.shape.step} value={s.shape} format={(v) => `${v}%`} on={(v) => send(Set.SHAPE, v)} muted={s.feel !== Feel.SAW} dirty={bit(Set.SHAPE)} />
            <Slider label="Click volume" caption="Motor click (amp)" min={0} max={s.ampMax} step={Limits.amp.step} value={s.amp} format={(v) => `${v}%`} on={(v) => send(Set.AMP, v)} dirty={bit(Set.AMP)} />
            <Slider label="Click pitch" caption="Motor click" min={s.pitchMin} max={s.pitchMax} step={Limits.pitch.step} value={s.pitch} format={(v) => `${v.toFixed(2)}×`} on={(v) => send(Set.PITCH, v)} dirty={bit(Set.PITCH)} />
          </div>
        </Box>
      </div>
      {s.click !== null && (
        <Box title={`Click · ${name}`} note="The sound the motor makes on each step; the knob plays it as you pick">
          <div class="cards">
            {ClickWaves.map((c, i) => (
              <Card on={s.click === i} dirty={s.click === i && bit(Set.CLICK)} onClick={() => device.set(Set.CLICK, i)}>
                <ClickWave wave={c} on={s.click === i} />
                <span class="nm">{c.name}</span>
                <span class="sub">{c.sub}</span>
              </Card>
            ))}
          </div>
        </Box>
      )}
    </>
  );
}
