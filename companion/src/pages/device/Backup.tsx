// DEVICE › BACKUP: the knob's setup to a file, and back. A restore shows what the file would
// change, part by part (each app profile and synth on its own), and brings back only what's ticked
// -- after saving what's on the knob now, unless that's unticked too. backup.ts does the work.

import { useSignal } from "@preact/signals";
import { useEffect } from "preact/hooks";
import { applyRestore, dated, droppedBackup, makeBackup, parseBackup, planRestore, SECTION_TITLES, type ItemPlan, type Plan, type Section } from "../../backup";
import { openJson, saveJson, savedText, type Opened } from "../../files";
import { HidType } from "../../proto";
import { device, unsavedCount, use } from "../../store";
import { Box, Check } from "../../ui/controls";
import { titleCase } from "../../ui/shell";

const msg = (e: unknown) => (e instanceof Error ? e.message : String(e));

export function BackupTab() {
  use("conn", "settings", "profiles", "synths");
  const busy = useSignal<string | null>(null); // what's being done
  const said = useSignal<{ text: string; bad?: boolean } | null>(null);
  const file = useSignal<string>("");
  const plan = useSignal<Plan | null>(null);
  const sections = useSignal(new globalThis.Set<Section>());
  const profiles = useSignal(new globalThis.Set<string>());
  const synths = useSignal(new globalThis.Set<string>());
  const safety = useSignal(true);
  const failed = useSignal<string[] | null>(null);

  const backUp = async () => {
    said.value = null;
    try {
      busy.value = "Reading the knob";
      const b = await makeBackup((w) => (busy.value = w));
      busy.value = null;
      const path = await saveJson(dated("quadra-backup"), b);
      if (path !== null) said.value = { text: savedText(path) };
    } catch (e) {
      said.value = { text: msg(e), bad: true };
    } finally {
      busy.value = null;
    }
  };

  const load = async (f: Opened) => {
    said.value = null;
    failed.value = null;
    plan.value = null;
    try {
      const { backup, notes } = parseBackup(f.data);
      busy.value = "Comparing with the knob";
      const p = await planRestore(backup, notes);
      file.value = f.name;
      plan.value = p;
      sections.value = new globalThis.Set(p.sections.filter((s) => s.present && !s.why).map((s) => s.key));
      const ok = (l: ItemPlan[]) => new globalThis.Set(l.filter((i) => !i.problem).map((i) => i.id));
      profiles.value = ok(p.profiles);
      synths.value = ok(p.synths);
    } catch (e) {
      said.value = { text: msg(e), bad: true };
    } finally {
      busy.value = null;
    }
  };

  // A backup dropped on the window.
  useEffect(() => {
    const f = droppedBackup.value;
    if (!f || busy.value) return;
    droppedBackup.value = null;
    void load(f);
  }, [droppedBackup.value]);

  const pick = async () => {
    try {
      const f = await openJson();
      if (f) await load(f);
    } catch (e) {
      said.value = { text: msg(e), bad: true };
    }
  };

  const restore = async () => {
    const p = plan.value!;
    said.value = null;
    failed.value = null;
    try {
      if (safety.value) {
        busy.value = "Backing up what's on the knob";
        const now = await makeBackup((w) => (busy.value = w));
        busy.value = null;
        const path = await saveJson(dated("quadra-before-restore"), now);
        if (path === null) {
          said.value = { text: "Nothing restored: the backup of what's on the knob now wasn't saved.", bad: true };
          return;
        }
      }
      const out = await applyRestore(p, { sections: sections.value, profiles: profiles.value, synths: synths.value }, (w) => (busy.value = w));
      plan.value = null;
      failed.value = out;
      said.value = out.length ? { text: "Restored, except:", bad: true } : { text: `Restored from ${file.value} and saved on the knob.` };
    } catch (e) {
      said.value = { text: msg(e), bad: true };
    } finally {
      busy.value = null;
    }
  };

  const toggle = <T,>(sig: { value: globalThis.Set<T> }, v: T, on: boolean) => {
    const n = new globalThis.Set(sig.value);
    if (on) n.add(v);
    else n.delete(v);
    sig.value = n;
  };

  const p = plan.value;
  const chosen = p
    ? [...sections.value].filter((k) => (k === "profiles" ? profiles.value.size > 0 : k === "synths" ? synths.value.size > 0 : true)).length
    : 0;
  const reconnects = p?.backup.settings && sections.value.has("settings") && device.kind !== "wifi" && (p.backup.settings.hidType === HidType.MIDI) !== (device.settings?.hidType === HidType.MIDI);

  return (
    <div class="grid2 backup">
      <Box title="Back up" note="Everything you've set up, in one file">
        <span class="hint">
          In it: the mode and device settings, the look (lights, screen, sleep hours), the clock, the tuning of the five haptic profiles, and your app profiles and synths
          (built-in ones too, if you've changed them).
        </span>
        <span class="hint">
          Not in it: Wi-Fi and the lamps in Home mode (bring them back under Lamps › Import), and the motor and sound calibration, which belong to this knob.
        </span>
        {unsavedCount.value > 0 && <span class="hint amber">Some changes aren't saved yet. The backup takes them as they are now.</span>}
        <div class="line">
          <button class="btn primary" disabled={!!busy.value} onClick={() => void backUp()}>
            Back up to a file…
          </button>
        </div>
      </Box>

      <Box title="Restore" note={p ? file.value : "Pick what to bring back"}>
        {!p ? (
          <div class="line">
            <button class="btn" disabled={!!busy.value} onClick={() => void pick()}>
              Restore from a file…
            </button>
            <span class="hint">or drop a backup on this window</span>
          </div>
        ) : (
          <>
            <span class="hint">
              Made {new Date(p.backup.made).toLocaleString()}
              {p.backup.firmware && <> on firmware {p.backup.firmware}</>}
            </span>
            {p.notes.map((n) => (
              <span class="hint amber">{n}</span>
            ))}
            <div class="rlist">
              {p.sections
                .filter((s) => s.present)
                .map((s) => (
                  <div class="ritem">
                    <Check on={sections.value.has(s.key) && !s.why} disabled={!!s.why || !!busy.value} set={(v) => toggle(sections, s.key, v)}>
                      <b>{SECTION_TITLES[s.key]}</b>
                    </Check>
                    <div class="rdet">
                      {s.why ? (
                        <span class="amber">{s.why}</span>
                      ) : s.key === "profiles" || s.key === "synths" ? (
                        (s.key === "profiles" ? p.profiles : p.synths).map((it) => (
                          <Check
                            on={(s.key === "profiles" ? profiles : synths).value.has(it.id) && !it.problem}
                            disabled={!!it.problem || !sections.value.has(s.key) || !!busy.value}
                            set={(v) => toggle(s.key === "profiles" ? profiles : synths, it.id, v)}
                          >
                            {s.key === "profiles" ? titleCase(it.name) : it.name}
                            <span class={it.problem ? "amber" : "faint"}> · {it.problem ?? (it.action === "replace" ? "replaces the one on the knob" : "added")}</span>
                          </Check>
                        ))
                      ) : s.lines.length ? (
                        s.lines.map((l) => <span>{l}</span>)
                      ) : (
                        <span class="faint">Same as on the knob</span>
                      )}
                    </div>
                  </div>
                ))}
            </div>
            <Check on={safety.value} disabled={!!busy.value} set={(v) => (safety.value = v)}>
              Back up what's on the knob first <span class="faint">(you pick where)</span>
            </Check>
            {reconnects && <span class="hint">The mode changes to or from MIDI: the knob reconnects at the end (about a second).</span>}
            <div class="line">
              <button class="btn primary" disabled={!chosen || !!busy.value} onClick={() => void restore()}>
                Restore and save
              </button>
              <button class="btn ghost" disabled={!!busy.value} onClick={() => (plan.value = null)}>
                Cancel
              </button>
            </div>
          </>
        )}
      </Box>
      {(busy.value || said.value) && (
        <span class={said.value?.bad && !busy.value ? "hint amber" : "hint"} role="status" style={{ gridColumn: "1 / -1" }}>
          {busy.value ? `${busy.value}…` : said.value?.text}
          {!busy.value && failed.value?.length ? (
            <ul class="fails">
              {failed.value.map((f) => (
                <li>{f}</li>
              ))}
            </ul>
          ) : null}
        </span>
      )}
    </div>
  );
}
