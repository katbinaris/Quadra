// Quadra companion: the knob's settings and app profiles. The same page runs in the Tauri app
// (Rust HID pipe) and in Chrome / Edge (WebHID). Pages follow the #route (store.ts).

import { effect } from "@preact/signals";
import { render } from "preact";
import { HapticsPage } from "./pages/Haptics";
import { LampImportPage } from "./pages/LampImport";
import { LampsPage } from "./pages/Lamps";
import { SynthsPage } from "./pages/Synths";
import "./pages/synths/session"; // the synth page's session follows the route
import { DevicePage } from "./pages/DevicePage";
import { LookPage } from "./pages/Look";
import { ModePage } from "./pages/Mode";
import { ProfilePage } from "./pages/profile/Profile";
import "./pages/profile/session"; // the profile page's session follows the route
import { SysPage } from "./pages/Sys";
import { droppedBackup } from "./backup";
import { kindOf, onDropFiles } from "./files";
import { importProfile } from "./profiles";
import { device, go, route, saveError } from "./store";
import { parseSynth } from "./synth";
import { Shell } from "./ui/shell";

function App() {
  const r = route.value;
  return (
    <Shell>
      {r.page === "haptics" ? (
        <HapticsPage />
      ) : r.page === "lamps" ? (
        r.sub === "import" ? <LampImportPage /> : <LampsPage />
      ) : r.page === "synths" ? (
        <SynthsPage id={r.id} tab={r.tab} />
      ) : r.page === "profile" ? (
        <ProfilePage id={r.id} tab={r.tab} input={r.input} />
      ) : r.page === "look" ? (
        <LookPage tab={r.tab} />
      ) : r.page === "device" ? (
        <DevicePage tab={r.tab} />
      ) : r.page === "sys" ? (
        <SysPage />
      ) : (
        <ModePage />
      )}
    </Shell>
  );
}

// The live stream costs the knob work: only System info shows it.
effect(() => device.setStreaming(route.value.page === "sys"));
// The lamps and the midi task are asked for every second, only while a page shows them.
effect(() => {
  const r = route.value;
  device.watch("lamps", r.page === "lamps" || r.page === "mode");
  device.watch("midi", r.page === "synths" || r.page === "mode");
});

// A file dropped on the window: a backup opens on Device › Backup, a profile or a synth comes in
// like Import does.
void onDropFiles(
  async (f) => {
    saveError.value = null;
    if (device.status !== "connected") return void (saveError.value = "Connect the knob first");
    try {
      switch (kindOf(f.data)) {
        case "backup":
          droppedBackup.value = f;
          return go({ page: "device", tab: "backup" });
        case "profile":
          return void (await importProfile(f.data));
        case "synth": {
          const s = parseSynth(f.data);
          await device.putSynth(s, false);
          return go({ page: "synths", id: s.id, tab: "params" });
        }
        default:
          saveError.value = `${f.name} isn't a Quadra backup, app profile or synth`;
      }
    } catch (e) {
      saveError.value = e instanceof Error ? e.message : String(e);
    }
  },
  (m) => (saveError.value = m),
);

render(<App />, document.getElementById("app")!);
