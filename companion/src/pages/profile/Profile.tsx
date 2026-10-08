// An app profile's page: who it is and its state, then four tabs -- General (name, icon, key
// labels, the main screen), Knob & keys, Command wheel, Macros. Every profile can be edited, the
// built-in ones too (a changed built-in is stored on the knob; Reset to default brings the
// original back).

import { useEffect, useRef } from "preact/hooks";
import { HidType, Op, ProfileFlag, Set } from "../../proto";
import { iconImage, imageToIcon, MAX, type ProfileJson } from "../../profile";
import { duplicateProfile, exportProfile, MAX_PROFILES } from "../../profiles";
import { device, editorDirty, go, href, inUse, use, type ProfileTab } from "../../store";
import { Box, Confirm, Row, Seg, SubTabs, Text } from "../../ui/controls";
import { drawMainScreen } from "../../ui/draw";
import { ProfileIcon } from "../../ui/icons";
import { titleCase } from "../../ui/shell";
import { origin } from "../Mode";
import { KeysTab } from "./Keys";
import { MacrosTab } from "./Macros";
import { session, type Session } from "./session";
import { WheelTab } from "./Wheel";

export function ProfilePage(p: { id: string; tab: ProfileTab; input: string }) {
  use("profiles", "settings");
  const entry = device.profiles.find((x) => x?.id === p.id);
  const listDone = device.hello !== null && device.profiles.filter(Boolean).length >= device.hello.profileCount;
  if (!entry)
    return (
      <Box>
        <span class="hint">{listDone ? "There's no profile with that name on the knob." : "Reading the profiles…"}</span>
      </Box>
    );
  return <Editor key={p.id} {...p} />;
}

function Editor(p: { id: string; tab: ProfileTab; input: string }) {
  const s = session.value;
  use("profiles", "settings");
  if (!s || s.id !== p.id) return null;
  void s.rev.value;
  const entry = device.profiles.find((x) => x?.id === p.id)!;
  const draft = s.draft.value;
  const using = inUse.value === entry.index;
  const live = (entry.flags & ProfileFlag.LIVE) !== 0 || editorDirty.value;
  const icon = (draft?.icon48 ? iconImage(draft, 48) : null) ?? entry.icon;
  const tabs = [
    { value: "general" as const, label: "General", href: href({ page: "profile", id: p.id, tab: "general", input: p.input }) },
    { value: "keys" as const, label: "Knob & keys", href: href({ page: "profile", id: p.id, tab: "keys", input: p.input }) },
    { value: "wheel" as const, label: "Command wheel", count: draft?.rings?.length ?? 0, href: href({ page: "profile", id: p.id, tab: "wheel", input: p.input }) },
    { value: "macros" as const, label: "Macros", count: draft?.macros?.length ?? 0, href: href({ page: "profile", id: p.id, tab: "macros", input: p.input }) },
  ];
  const st = s.status.value;
  return (
    <>
      <div class="ph">
        <ProfileIcon icon={icon} name={entry.name} size={48} />
        <div class="t">
          <div class="line" style={{ gap: "8px" }}>
            <span class="title">{titleCase(draft?.name ?? entry.name)}</span>
            {using ? <span class="badge ok">In use</span> : null}
            <span class="badge">{origin(entry.flags & ~ProfileFlag.LIVE)}</span>
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
              if (device.settings?.hidType !== HidType.APP) await device.set(Set.HID_TYPE, HidType.APP);
              await device.set(Set.PROFILE, entry.index);
            }}
          >
            Use on the knob
          </button>
        )}
        <button class="btn ghost" disabled={device.profiles.length >= MAX_PROFILES} onClick={() => void duplicateProfile(entry.index)}>
          Duplicate
        </button>
        <button
          class="btn ghost"
          onClick={async () => {
            await s.flush();
            await exportProfile(entry.index);
          }}
        >
          Export…
        </button>
      </div>
      <SubTabs tabs={tabs} value={p.tab} />
      {!draft ? null : p.tab === "keys" ? <KeysTab s={s} p={draft} input={p.input} /> : p.tab === "wheel" ? <WheelTab s={s} p={draft} /> : p.tab === "macros" ? <MacrosTab s={s} p={draft} /> : <GeneralTab s={s} p={draft} flags={entry.flags} icon={icon} />}
    </>
  );
}

// --- General ---

function GeneralTab(x: { s: Session; p: ProfileJson; flags: number; icon: ImageData | null }) {
  const { s, p } = x;
  const file = useRef<HTMLInputElement>(null);
  const builtin = (x.flags & ProfileFlag.BUILTIN) !== 0;
  const changed = (x.flags & (ProfileFlag.STORED | ProfileFlag.LIVE)) !== 0;
  const remove = async () => {
    const i = s.index();
    if (i < 0) return;
    s.discard();
    try {
      const r = await device.profileOp(i, Op.REMOVE);
      if (r.removed) go({ page: "mode" });
      else await s.load();
    } catch (e) {
      s.status.value = { msg: e instanceof Error ? e.message : String(e), bad: true };
    }
  };
  return (
    <div class="split wide">
      <div class="stack">
        <Box title="Profile" note={<span class="mono">id: {p.id}</span>}>
          <Row label="Name" for="p-name">
            <Text id="p-name" value={p.name} max={MAX.name} on={(v) => ((p.name = v), s.touch())} />
          </Row>
          <Row label="Icon">
            <ProfileIcon icon={x.icon} name={p.name} size={48} />
            <IconSmall p={p} />
            <button class="btn sm" onClick={() => file.current?.click()}>
              Import image…
            </button>
            <span class="hint">Any picture, fitted to 48 and 24 pixels</span>
            <input
              ref={file}
              type="file"
              accept="image/*"
              hidden
              onChange={async (e) => {
                const f = e.currentTarget.files?.[0];
                e.currentTarget.value = "";
                if (!f) return;
                try {
                  const bmp = await createImageBitmap(f);
                  p.icon48 = imageToIcon(bmp, 48);
                  p.icon24 = imageToIcon(bmp, 24);
                  s.touch();
                } catch {
                  s.status.value = { msg: "That file isn't a picture this app can read", bad: true };
                }
              }}
            />
          </Row>
          <Row label="Key labels">
            {p.legend.map((l, i) => (
              <label class="line" style={{ gap: "6px" }}>
                <span class="mono faint small">F{i + 1}</span>
                <Text value={l} max={MAX.legend} width={84} placeholder="—" on={(v) => ((p.legend[i] = v), s.touch())} />
              </label>
            ))}
          </Row>
        </Box>
        <Box title="Main screen" note="The middle of the knob's screen in this profile">
          <Row label="Shows">
            <Seg
              options={[
                { value: "label" as const, label: "Action name" },
                { value: "shape" as const, label: "3D shape" },
              ]}
              value={p.visual ?? "label"}
              on={(v) => ((p.visual = v), s.touch())}
            />
          </Row>
          {p.visual === "shape" && (
            <Row label="Shape">
              <Seg
                options={[
                  { value: "cube" as const, label: "Cube" },
                  { value: "pyramid" as const, label: "Pyramid" },
                  { value: "octa" as const, label: "Octa" },
                ]}
                value={p.shape ?? "cube"}
                on={(v) => ((p.shape = v), s.touch())}
              />
              <Seg
                options={[
                  { value: "face" as const, label: "Face" },
                  { value: "grips" as const, label: "Grips" },
                  { value: "thick" as const, label: "Thick" },
                ]}
                value={p.shape_style ?? "face"}
                on={(v) => ((p.shape_style = v), s.touch())}
              />
            </Row>
          )}
        </Box>
        {(!builtin || changed) && (
          <section class="box row-box">
            <div class="t">
              <b>{builtin ? "Reset to default" : "Delete this profile"}</b>
              <span class="hint">{builtin ? `Puts ${titleCase(p.name)} back the way it shipped. Your changes to it are lost.` : "Removes it from the knob. This can't be undone."}</span>
            </div>
            <span class="spacer" />
            <Confirm label={builtin ? "Reset to default" : "Delete profile"} ask={builtin ? "Sure? Reset it" : "Sure? Delete it"} on={() => void remove()} />
          </section>
        )}
      </div>
      <Box title="On the knob" class="preview">
        <Preview p={p} icon={x.icon} />
        <span class="hint">Drawn the way the knob draws it</span>
      </Box>
    </div>
  );
}

function IconSmall(p: { p: ProfileJson }) {
  const img = iconImage(p.p, 24);
  const ref = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    if (ref.current && img) ref.current.getContext("2d")!.putImageData(img, 0, 0);
  }, [p.p.icon24]);
  if (!img) return null;
  return <canvas ref={ref} width={24} height={24} class="picon px" style={{ width: "24px", height: "24px" }} aria-hidden="true" />;
}

function Preview(x: { p: ProfileJson; icon: ImageData | null }) {
  use("settings");
  const ref = useRef<HTMLCanvasElement>(null);
  const feel = device.settings?.feel ?? 0;
  const key = `${x.p.name}|${x.p.legend.join()}|${feel}|${x.p.icon48 ?? ""}|${!!x.icon}`;
  useEffect(() => {
    const c = ref.current;
    if (!c) return;
    void document.fonts.load("8px Silkscreen").then(() => drawMainScreen(c, { icon: x.icon, name: x.p.name, feel, legend: x.p.legend }));
  }, [key]);
  return <canvas ref={ref} class="knobscreen" width={240} height={240} aria-label={`The knob's screen: ${x.p.name}`} />;
}
