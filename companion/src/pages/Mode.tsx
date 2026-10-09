// MODE: what the knob sends (APP / HOME / MOUSE / KEYS / MIDI) and, per mode, what goes with it: the
// app profile in use, the haptic profile, the MIDI synth and channel.

import { EXT_SYNTH_VERSION, HapticProfiles, HidType, HomeFlag, MidiSynths, ProfileFlag, Set } from "../proto";
import { createProfile, duplicateProfile, MAX_PROFILES } from "../profiles";
import { device, href, inUse, use } from "../store";
import { Box, Card, Dial, PageHead } from "../ui/controls";
import { ProfileIcon } from "../ui/icons";
import { titleCase } from "../ui/shell";
import { LampTile, lampState } from "./Lamps";
import { MakerMark } from "./Synths";

const MODES = [
  { value: HidType.APP, name: "App", sub: "Your app profiles: shortcuts, command wheels and macros per app" },
  { value: HidType.HOME, name: "Home", sub: "Your Xiaomi lamps on the network: brightness, white and colour" },
  { value: HidType.MOUSE, name: "Mouse", sub: "A scroll wheel" },
  { value: HidType.KEYBOARD, name: "Keys", sub: "Keyboard keys" },
  { value: HidType.MIDI, name: "MIDI", sub: "A synth's parameters, over USB MIDI and the TRS jacks" },
];

export function origin(flags: number): string {
  if (flags & ProfileFlag.BUILTIN) return flags & (ProfileFlag.STORED | ProfileFlag.LIVE) ? "Built-in, changed" : "Built-in";
  return flags & ProfileFlag.STORED ? "Yours" : "New, not saved";
}

export function ModePage() {
  use("settings", "profiles", "lamps", "synths");
  const s = device.settings!;
  const bit = (id: number) => ((s.dirty >> id) & 1) === 1;
  const using = inUse.value;
  const current = device.profiles[s.profile];
  // The knob's own list (extensions v12), else the built-ins this app knows.
  const synths: { id: string; maker: string; name: string; channel: number; params: number }[] = device.synths.length ? device.synths : [...MidiSynths];
  return (
    <>
      <PageHead title="Mode" hint="What the knob sends to the computer. Changes at once; Save keeps it after a restart." />
      <div class="cards">
        {MODES.map((m) => (
          <Card left on={s.hidType === m.value} dirty={s.hidType === m.value && bit(Set.HID_TYPE)} onClick={() => device.set(Set.HID_TYPE, m.value)}>
            <span class="nm">{m.name}</span>
            <span class="sub">{m.sub}</span>
          </Card>
        ))}
      </div>

      {s.hidType === HidType.APP && (
        <>
          <Box title="Profile in use" note="Also from the knob: F4 menu › Profiles">
            <div class="pgrid">
              {device.profiles.filter(Boolean).map((p) => (
                <Card on={p.index === using} dirty={p.index === using && bit(Set.PROFILE)} onClick={() => device.set(Set.PROFILE, p.index)}>
                  <ProfileIcon icon={p.icon} name={p.name} size={48} id={p.id} builtin={(p.flags & ProfileFlag.BUILTIN) !== 0} />
                  <span class="nm">{titleCase(p.name)}</span>
                  <span class={p.flags & ProfileFlag.LIVE ? "sub amber" : "sub"}>{p.flags & ProfileFlag.LIVE ? "Not saved" : origin(p.flags)}</span>
                </Card>
              ))}
              <Card dashed disabled={device.profiles.length >= MAX_PROFILES} onClick={() => void createProfile()}>
                <span class="plus">+</span>
                <span class="nm">New profile</span>
                <span class="sub">From scratch</span>
              </Card>
            </div>
            {current && (
              <div class="line">
                <a class="btn" href={href({ page: "profile", id: current.id, tab: "general", input: "knob" })}>
                  Edit {titleCase(current.name)}
                </a>
                <button class="btn ghost" disabled={device.profiles.length >= MAX_PROFILES} onClick={() => void duplicateProfile(current.index)}>
                  Duplicate {titleCase(current.name)}
                </button>
                <span class="hint" style={{ marginLeft: "auto" }}>
                  {device.profiles.length} of {MAX_PROFILES} profiles
                </span>
              </div>
            )}
          </Box>
          <Box title="Haptics in App mode">
            <span class="hint">
              Each input of a profile (the knob, F1 to F4) picks one of the five haptic profiles. Set them in the profile under Knob &amp; keys; tune the five under <a href="#/haptics">Haptics</a>.
            </span>
          </Box>
        </>
      )}

      {(s.hidType === HidType.MOUSE || s.hidType === HidType.KEYBOARD) && (
        <Box title={`Haptic profile for ${s.hidType === HidType.MOUSE ? "Mouse" : "Keys"}`} note="How the knob feels in this mode">
          <div class="cards">
            {HapticProfiles.map((h, i) => (
              <Card on={s.modeHaptic === i} dirty={s.modeHaptic === i && bit(Set.MODE_HAPTIC)} onClick={() => device.set(Set.MODE_HAPTIC, i)}>
                <Dial steps={h.detents} />
                <span class="nm">{titleCase(h.name)}</span>
                <span class="sub">{h.detents ? `${h.detents} per turn` : "No steps"}</span>
              </Card>
            ))}
          </div>
          <span class="hint">
            Mouse and Keys each keep their own. Tune the five under <a href="#/haptics">Haptics</a>.
          </span>
        </Box>
      )}

      {s.hidType === HidType.HOME && (
        <Box
          title="Lamps"
          note={device.lampCount ? `${device.lamps.filter((l) => l && l.flags & HomeFlag.ONLINE).length} of ${device.lampCount} answering` : "The knob finds them on the network and talks to them itself"}
        >
          {device.lamps.filter(Boolean).length > 0 ? (
            <div class="line" style={{ gap: "8px" }}>
              {device.lamps.filter(Boolean).map((l) => (
                <span class="lampchip">
                  <LampTile lamp={l} scale={0.75} />
                  <span>{l.name}</span>
                  <span class="faint">{lampState(l)}</span>
                </span>
              ))}
            </div>
          ) : (
            <span class="hint">No lamps on the knob yet. They come from your Xiaomi account, once: Import from Xiaomi finds them and sends them over USB.</span>
          )}
          <div class="line">
            <a class="btn" href={href({ page: "lamps", sub: "list" })}>
              Open Lamps
            </a>
            <a class="btn ghost" href={href({ page: "lamps", sub: "import" })}>
              Import from Xiaomi…
            </a>
            <span class="hint" style={{ marginLeft: "auto" }}>
              On the knob: turn to pick a lamp, F1 to change it, F2 power, F3 back or look again
            </span>
          </div>
        </Box>
      )}

      {s.hidType === HidType.MIDI && (
        <>
          <Box title="Synth" note="Also from the knob: F4 menu › Profiles › MIDI">
            <div class="cards">
              {synths.map((m, i) => (
                <Card left on={s.midiSynth === i} dirty={s.midiSynth === i && bit(Set.MIDI_SYNTH)} onClick={() => device.set(Set.MIDI_SYNTH, i)}>
                  <MakerMark maker={m.maker} />
                  <span class="nm">{m.name}</span>
                  <span class="sub">{m.maker === "MIDI" ? "General MIDI controllers, for a DAW's MIDI learn" : `${m.maker ? `${titleCase(m.maker)}, ` : ""}${m.params} parameters`}</span>
                </Card>
              ))}
              {(device.ext ?? 0) >= EXT_SYNTH_VERSION && (
                <a class="card left dashed" href={href({ page: "synths", id: synths[s.midiSynth]?.id ?? "", tab: "params" })}>
                  <span class="nm">Edit synths…</span>
                  <span class="sub">Parameters, switches, programs; add your own</span>
                </a>
              )}
            </div>
          </Box>
          <Box
            title="MIDI channel"
            note={synths[s.midiSynth]?.channel ? `The ${synths[s.midiSynth].name} comes set to ${synths[s.midiSynth].channel}` : "Set the synth to the same one"}
          >
            <div class="line">
              <button class="btn sm" aria-label="Channel down" onClick={() => device.set(Set.MIDI_CH, s.midiChannel - 1)}>
                ‹
              </button>
              <span class={bit(Set.MIDI_CH) ? "mono big2 amber" : "mono big2"}>{String(s.midiChannel).padStart(2, "0")}</span>
              <button class="btn sm" aria-label="Channel up" onClick={() => device.set(Set.MIDI_CH, s.midiChannel + 1)}>
                ›
              </button>
            </div>
          </Box>
          <Box title="On the knob">
            <span class="hint">
              Turn to change the parameter on the screen; F1 moves to the next one (hold F1 and turn to pick from the list);
              F2 and F3 step the synth's program down and up. The knob sends on USB MIDI and on the TRS jacks at once. In MIDI
              mode the knob is a USB MIDI device instead of a keyboard and mouse: changing to or from MIDI reconnects it
              (about a second).
            </span>
          </Box>
        </>
      )}
    </>
  );
}
