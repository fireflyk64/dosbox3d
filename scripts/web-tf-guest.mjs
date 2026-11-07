// TIE Fighter in the browser as a guest of a native host (scripts/web-tf.sh starts both): the page takes GOG's installer,
// joins the host's room, flies the Combat Chamber's first mission from the same front-end key script as the native rig
// (scripts/tfnet-duo.sh), and its wall-clock reports go to SHOT_DIR/guest.wall for scripts/tfnet-join.py.
//
//   PLAYWRIGHT_DIR=... SHOT_DIR=... node scripts/web-tf-guest.mjs PAGE_URL LOBBY_URL ROOM INSTALLER SCRIPT [seconds]
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, room, installer, script, secondsArg = "100"] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl || !room || !installer || !script) {
  console.error("usage: node web-tf-guest.mjs PAGE_URL LOBBY_URL ROOM INSTALLER SCRIPT [seconds]");
  process.exit(2);
}
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const page = await browser.newPage();
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
const logs = [];
page.on("console", (m) => {
  const t = m.text();
  logs.push(t);
  if (/TIE: (HOST|GUEST|In |Room|.*joined|.*gone|mission set up|the player flight group)|TIE Fighter: the program|lobby: (joined|player)|could not|Aborted|RuntimeError/.test(t)) {
    console.log(`${stamp()} [page] ${t.slice(0, 200)}`);
  }
});
page.on("pageerror", (e) => console.log(`${stamp()} PAGE ERROR: ${e.message}`));
page.on("crash", () => console.log(`${stamp()} PAGE CRASHED`));
const u = new URL(pageUrl);
u.searchParams.set("room", room);
u.searchParams.set("server", lobbyUrl);
u.searchParams.set("callsign", "WEBGUEST");
u.searchParams.set("env.WCNET_KEYSCRIPT", script);
u.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
u.searchParams.set("env.WCNET_TIE_REPORT_MS", process.env.WCNET_TIE_REPORT_MS || "250");
// (GUEST_ENV="NAME=value ...": more of the emulator's environment for the guest's page, as ?env.NAME=value)
for (const kv of (process.env.GUEST_ENV || "").split(/\s+/).filter(Boolean)) {
  const i = kv.indexOf("=");
  if (i > 0) u.searchParams.set("env." + kv.slice(0, i), kv.slice(i + 1));
}
u.searchParams.set("env.WCNET_LOG", "2");
await page.goto(u.toString());
await page.setInputFiles("#gamefile", installer);
// (ready with TIE Fighter: a page that had another game loaded is "ready" before the installer is unpacked)
await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
  || (document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "tf"), null, { timeout: 600000 });
console.log(`${stamp()} files: ` + await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 120)}`));
await page.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 0, null, { timeout: 60000 });
await page.waitForFunction(() => !document.getElementById("fly").disabled, null, { timeout: 60000 });
console.log(`${stamp()} in the room: ` + await page.$eval("#status", (el) => el.textContent.trim().slice(0, 140)));
// the flight's controls on the page (src/cpu/wcnet_tie_controls.cpp): the Flight choice shown with TIE Fighter loaded
console.log(`${stamp()} flight controls: ` + await page.$eval("#tfFlightLabel", (el) => `shown ${!el.hidden}, ${document.getElementById("tfFlight").value}`)
  .catch((e) => "no Flight choice: " + e.message));
// the room's mission menu (step 8): the Combat Chamber's craft and the battles, in the game's own words
console.log(`${stamp()} mission menu: ` + await page.evaluate(() => {
  const sel = document.getElementById("mission");
  const groups = Array.from(sel.querySelectorAll("optgroup"));
  const opts = Array.from(sel.options);
  return `shown ${!document.getElementById("missionRow").hidden}, ${opts.length} entries in ${groups.length} groups, picked "${sel.value}"; ` +
    `first "${opts[0] && opts[0].textContent}"; ${groups.slice(0, 1).concat(groups.slice(7, 8), groups.slice(-1)).map((g) => `[${g.label}] ` +
    `"${g.querySelector("option") && g.querySelector("option").textContent}"`).join("; ")}; hint "${document.getElementById("missionHint").textContent.slice(0, 120)}"`;
}).catch((e) => "no menu: " + e.message));
await page.click("#fly");
const seconds = Number(secondsArg);
await new Promise((r) => setTimeout(r, 5000));
console.log(`${stamp()} the emulator's calls: wc_web_tie_mouse ${await page.evaluate(() => typeof (window.DOSBox && window.DOSBox._wc_web_tie_mouse))}, ` +
  `wc_web_stick ${await page.evaluate(() => typeof (window.DOSBox && window.DOSBox._wc_web_stick))}`);
for (let t = 20; t <= seconds; t += 20) {
  await new Promise((r) => setTimeout(r, 20000));
  await page.screenshot({ path: path.join(shotDir, `web-guest-${t}.png`) });
  console.log(`${stamp()} status line: ` + await page.$eval("#status", (el) => el.textContent.trim().slice(0, 140)).catch(() => "?"));
}
fs.writeFileSync(path.join(shotDir, "guest.log"), logs.join("\n") + "\n");
fs.writeFileSync(path.join(shotDir, "guest.wall"), logs.filter((l) => l.includes("TIE: wall")).join("\n") + "\n");
console.log(`${stamp()} ${logs.filter((l) => l.includes("TIE: wall")).length} wall-clock reports`);
await browser.close();
