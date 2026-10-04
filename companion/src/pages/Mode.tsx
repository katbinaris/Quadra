// MODE: what the knob sends (APP / HOME / MOUSE / KEYS / MIDI) and, per mode, what goes with it: the
// app profile in use, the haptic profile, the MIDI channel.

import { HapticProfiles, HidType, ProfileFlag, Set } from "../proto";
import { createProfile, duplicateProfile, MAX_PROFILES } from "../profiles";
import { device, href, inUse, use } from "../store";
import { Box, Card, Dial, PageHead } from "../ui/controls";
import { ProfileIcon } from "../ui/icons";
import { titleCase } from "../ui/shell";

const MODES = [
  { value: HidType.APP, name: "App", sub: "Your app profiles: shortcuts, command wheels and macros per app" },
  { value: HidType.HOME, name: "Home", sub: "Your Xiaomi lamps on the network: brightness, white and colour" },
  { value: HidType.MOUSE, name: "Mouse", sub: "A scroll wheel" },
  { value: HidType.KEYBOARD, name: "Keys", sub: "Keyboard keys" },
  { value: HidType.MIDI, name: "MIDI", sub: "Channel only for now: no MIDI is sent yet" },
];

export function origin(flags: number): string {
  if (flags & ProfileFlag.BUILTIN) return flags & (ProfileFlag.STORED | ProfileFlag.LIVE) ? "Built-in, changed" : "Built-in";
  return flags & ProfileFlag.STORED ? "Yours" : "New, not saved";
}

export function ModePage() {
  use("settings", "profiles");
  const s = device.settings!;
  const bit = (id: number) => ((s.dirty >> id) & 1) === 1;
  const using = inUse.value;
  const current = device.profiles[s.profile];
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
                  <ProfileIcon icon={p.icon} name={p.name} size={48} />
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
        <Box title="Lamps" note="The knob finds them on the network and talks to them itself">
          <span class="hint">
            The lamps and their keys come from your Xiaomi account, once, over USB: run the token extractor, then{" "}
            <span class="mono">quadra.py home import</span>. On the knob: turn to pick a lamp, F1 to change it (F1 again
            for the next setting), F2 switches it on or off, F3 goes back or looks again.
          </span>
        </Box>
      )}

      {s.hidType === HidType.MIDI && (
        <Box title="MIDI channel" note="Stored only for now: no MIDI is sent yet">
          <div class="line">
            <button class="btn sm" aria-label="Channel down" onClick={() => device.set(Set.MIDI_CH, s.midiChannel - 1)}>
              ‹
            </button>
            <span class="mono big2">{String(s.midiChannel).padStart(2, "0")}</span>
            <button class="btn sm" aria-label="Channel up" onClick={() => device.set(Set.MIDI_CH, s.midiChannel + 1)}>
              ›
            </button>
          </div>
        </Box>
      )}
    </>
  );
}
