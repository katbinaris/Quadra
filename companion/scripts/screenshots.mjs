// The user guide's screenshots (docs/app-*.png), from demo mode: 1280x800 at 2x, in WebKit (the
// engine the Mac app uses); the not-connected one in Chromium, which has WebHID. Retake them
// after any UI change:
//     pnpm dev &                      (the page on http://localhost:1420)
//     npx playwright install webkit chromium   (once)
//     node scripts/screenshots.mjs [base url]

import { chromium, webkit } from "playwright";
import { fileURLToPath } from "node:url";
import { readFileSync, writeFileSync } from "node:fs";

const BASE = process.argv[2] ?? "http://localhost:1420";
const OUT = fileURLToPath(new URL("../docs/", import.meta.url));
const VIEW = { viewport: { width: 1280, height: 800 }, deviceScaleFactor: 2 };

const br = await webkit.launch();
const pg = await br.newPage(VIEW);
const errors = [];
pg.on("pageerror", (e) => errors.push(e.message));

const go = async (hash, ms = 1200) => {
  await pg.evaluate((h) => (location.hash = h), hash);
  await pg.waitForTimeout(ms);
};
const shot = async (name) => {
  await pg.mouse.move(0, 799); // no hover state in the picture
  await pg.waitForTimeout(150);
  await pg.screenshot({ path: `${OUT}${name}.png` });
  console.log(`docs/${name}.png`);
};
const click = (sel, text) => pg.locator(sel, text ? { hasText: text } : {}).first().click();

await pg.goto(`${BASE}/?demo#/mode`);
await pg.waitForTimeout(6000); // every profile and its icon
await document_fonts();

await shot("app-mode");
await windowMap();
await click(".card", "Mouse");
await pg.waitForTimeout(600);
await shot("app-mode-mouse");
await click(".card", "Home");
await pg.waitForTimeout(600);
await shot("app-mode-home");
await click(".card", "MIDI");
await pg.waitForTimeout(600);
await shot("app-mode-midi");
// Synths while MIDI is the mode, so the knob's parameter shows (the green row).
await go("#/synths/minilogue-xd/params", 3000);
await click(".prow", "WAVE");
await pg.waitForTimeout(600);
await shot("app-synths");
await go("#/synths/minilogue-xd/programs");
await shot("app-synths-programs");
await go("#/mode", 800);
await click(".card", "App");
await pg.waitForTimeout(600);

await go("#/lamps", 2500);
await shot("app-lamps");
await go("#/lamps/import", 1500);
await click(".btn", "Find the lamps");
await pg.waitForTimeout(3000);
await shot("app-lamps-import");

await go("#/haptics");
await shot("app-haptics");

await go("#/profile/figma/general", 2000);
await shot("app-profile-editor");
await go("#/profile/figma/keys/f1");
await shot("app-editor-keys");
await go("#/profile/figma/wheel");
await shot("app-editor-wheel");

// A macro to show: EXPORT PNG = Cmd+Shift+E, a pause, "png", Return.
await go("#/profile/figma/macros");
await click(".btn", "+ Macro");
await pg.waitForTimeout(300);
const name = pg.locator(".split.side .box").nth(1).locator("input.field").first();
await name.fill("EXPORT PNG");
await name.dispatchEvent("input");
await click(".btn", "Record");
await pg.waitForTimeout(300); // the recorder attaches after the click renders
await pg.keyboard.press("Meta+Shift+KeyE");
await pg.waitForTimeout(200);
await click(".btn", "Stop");
await click(".btn", "+ Pause");
await pg.locator(".steprow input").last().fill("300");
await pg.locator(".steprow input").last().dispatchEvent("change");
await click(".btn", "+ Text");
await pg.locator(".steprow input").last().fill("png");
await pg.locator(".steprow input").last().dispatchEvent("input");
await click(".btn", "Record");
await pg.waitForTimeout(300);
await pg.keyboard.press("Enter");
await pg.waitForTimeout(200);
await click(".btn", "Stop");
await pg.waitForTimeout(800);
await shot("app-editor-macros");

// Something unsaved in two places, and the list of it open.
await go("#/haptics", 800);
const track = await pg.locator(".trow", { hasText: "Snap" }).locator(".track").boundingBox();
await pg.mouse.click(track.x + track.width * 0.18, track.y + 3);
await pg.waitForTimeout(800);
await click(".pend");
await pg.waitForTimeout(400);
await pg.screenshot({ path: `${OUT}app-unsaved.png` });
console.log("docs/app-unsaved.png");
await click(".pop .btn", "Revert all");
await pg.waitForTimeout(1200);

await go("#/look/lights");
await shot("app-look");
await go("#/look/screen");
await shot("app-look-screen");
await pg.locator(".box", { hasText: "Screensaver" }).first().evaluate((e) => e.scrollIntoView({ block: "start" }));
await shot("app-look-sleep");
await go("#/look/clock");
await shot("app-look-clock");
await go("#/device/general");
await shot("app-device");
await go("#/device/wifi");
await shot("app-device-wifi");
await go("#/device/backup");
await shot("app-device-backup");
// A restore's summary: this knob's own backup, with a few things changed in it.
{
  const [dl] = await Promise.all([pg.waitForEvent("download"), click("button", "Back up to a file")]);
  const chunks = [];
  for await (const c of await dl.createReadStream()) chunks.push(c);
  const b = JSON.parse(Buffer.concat(chunks).toString("utf8"));
  b.look.screen.bright = 60;
  b.look.screen.sleepFrom = 22 * 60;
  b.haptics.profiles[2].tune[0].kp = 3.2;
  b.settings.host = 1;
  const figma = JSON.parse(readFileSync(new URL("../src/demo_builtins.json", import.meta.url), "utf8")).find((p) => p.id === "figma");
  b.profiles.push({ ...figma, id: "figma", name: "FIGMA" }, { ...figma, id: "sketch", name: "SKETCH" });
  const [fc] = await Promise.all([pg.waitForEvent("filechooser"), click("button", "Restore from a file")]);
  await fc.setFiles({ name: "quadra-backup-2026-10-01.json", mimeType: "application/json", buffer: Buffer.from(JSON.stringify(b)) });
  await pg.waitForTimeout(2000);
  await shot("app-device-restore");
  await click("button", "Cancel");
}
await go("#/sys", 6000); // some history in the charts
await shot("app-sys-info");
await br.close();

// Not connected: Chromium has WebHID, so the page asks to connect.
const cr = await chromium.launch();
const cp = await cr.newPage(VIEW);
await cp.goto(`${BASE}/#/mode`);
await cp.waitForTimeout(2000);
await cp.screenshot({ path: `${OUT}app-not-connected.png` });
console.log("docs/app-not-connected.png");
await cr.close();

if (errors.length) {
  console.error("Page errors:\n" + errors.join("\n"));
  process.exit(1);
}

// docs/fig-window.svg: the Mode page at 1x with numbered callouts and a legend, so the map is
// always the real window.
async function windowMap() {
  const png = (await pg.screenshot({ scale: "css" })).toString("base64");
  const dot = (n, x, y) =>
    `<circle cx="${x}" cy="${y}" r="15" fill="#ffc94d" stroke="#0b0c0d" stroke-width="3"/><text x="${x}" y="${y + 6}" fill="#15120a" text-anchor="middle" font-size="17" font-weight="700">${n}</text>`;
  const legend = [
    ["1  Your knob", "What its screen shows now,", "and how it's connected"],
    ["2  Pages", "Knob, app profiles, setup.", "Green ring: the profile in use"],
    ["3  Where you are"],
    ["4  One save", "Settings, lights and profiles.", "An amber label lists what", "isn't saved yet"],
    ["5  The page", "Some have tabs. Amber marks", "the choice; an amber dot, a", "change not saved yet"],
  ];
  let y = 70;
  const text = legend
    .map(([head, ...rest]) => {
      let s = `<text x="1330" y="${y}" fill="#ececec" font-size="20" font-weight="600">${head}</text>`;
      for (const line of rest) s += `<text x="1330" y="${(y += 26)}" fill="#9a9ca3" font-size="17">${line}</text>`;
      y += 58;
      return s;
    })
    .join("\n  ");
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="1640" height="840" viewBox="0 0 1640 840" font-family="Geist, -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif">
  <title>Map of the companion window</title>
  <desc>The Mode page with five callouts: the knob, the pages, where you are, one save, and the page.</desc>
  <rect width="1640" height="840" rx="16" fill="#0b0c0d"/>
  <image x="20" y="20" width="1280" height="800" href="data:image/png;base64,${png}"/>
  <rect x="20.5" y="20.5" width="1279" height="799" rx="8" fill="none" stroke="#2a2c30"/>
  ${dot(1, 205, 45)}
  ${dot(2, 120, 310)}
  ${dot(3, 425, 45)}
  ${dot(4, 1228, 84)}
  ${dot(5, 1265, 260)}
  ${text}
</svg>
`;
  writeFileSync(`${OUT}fig-window.svg`, svg);
  console.log("docs/fig-window.svg");
}

async function document_fonts() {
  await pg.evaluate(() => document.fonts.ready);
}
