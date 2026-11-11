// TIE Fighter in the browser as a guest of a native host (scripts/web-tf.sh starts both): the page takes GOG's installer,
// joins the host's room, flies the Combat Chamber's first mission from the same front-end key script as the native rig
// (scripts/tfnet-duo.sh), and its wall-clock reports go to SHOT_DIR/guest.wall for scripts/tfnet-join.py.
//
//   PLAYWRIGHT_DIR=... SHOT_DIR=... node scripts/web-tf-guest.mjs PAGE_URL LOBBY_URL ROOM INSTALLER SCRIPT [seconds]
//   CHAT="110:0:hello there;130:shift": lines typed at the comms prompt that many seconds after Fly (shift: a bare tap,
//   which must open nothing since 2026-11-10)
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
// the picture's shape and the room's options for TIE Fighter (no Conn, no Collide; Full screen on the mission's line)
console.log(`${stamp()} picture and options: ` + await page.evaluate(() => {
  const c = document.getElementById("canvas"), r = c.getBoundingClientRect();
  const shown = (id) => !document.getElementById(id).parentElement.hidden;
  const top = (el) => Math.round((el.getBoundingClientRect().top + el.getBoundingClientRect().bottom) / 20);   // (middles, to 10 px)
  return `canvas ${Math.round(r.width)}x${Math.round(r.height)} on the page; Conn ${shown("latency") ? "shown" : "hidden"}, Collide ${shown("collisions") ? "shown" : "hidden"}; ` +
    `Full screen ${top(document.getElementById("fullStartLabel")) === top(document.getElementById("mission").parentElement) ? "on the mission's line" : "on a line of its own"}`;
}));
await page.screenshot({ path: path.join(shotDir, `web-guest-room.png`), fullPage: true });
// (cinema is for the page's own picture: the flight not opened full screen)
if (process.env.CINEMA_AT) await page.evaluate(() => { const b = document.getElementById("fullStart"); if (b.checked) b.click(); });
await page.click("#fly");
const seconds = Number(secondsArg);
await new Promise((r) => setTimeout(r, 5000));
console.log(`${stamp()} the emulator's calls: wc_web_tie_mouse ${await page.evaluate(() => typeof (window.DOSBox && window.DOSBox._wc_web_tie_mouse))}, ` +
  `wc_web_stick ${await page.evaluate(() => typeof (window.DOSBox && window.DOSBox._wc_web_stick))}`);
// CHAT="<seconds after Fly>:<0|shift>:<text>;...": the comms prompt as a player opens it (the 0 key), the line typed and
// Enter, by the browser's own keys; a screenshot a second after.  "shift" only taps Shift and types nothing: a tap of
// Shift opened the prompt until 2026-11-10 (pilots brushing the right Shift lost the stick), and now must not
for (const one of (process.env.CHAT || "").split(";").filter(Boolean)) {
  const [at, how, ...rest] = one.split(":");
  const text = rest.join(":");
  setTimeout(async () => {
    await page.focus("#canvas");
    if (how === "shift") {
      await page.keyboard.press("Shift");
      console.log(`${stamp()} Shift tapped (no prompt should open)`);
      await new Promise((r) => setTimeout(r, 1000));
      await page.screenshot({ path: path.join(shotDir, `web-guest-chat-${at}.png`) });
      return;
    }
    await page.keyboard.press("0");
    await new Promise((r) => setTimeout(r, 300));
    await page.keyboard.type(text, { delay: 80 });
    await page.keyboard.press("Enter");
    console.log(`${stamp()} chat typed (${how}): ${text}`);
    await new Promise((r) => setTimeout(r, 1000));
    await page.screenshot({ path: path.join(shotDir, `web-guest-chat-${at}.png`) });
  }, Number(at) * 1000);
}
// cinema (the picture as large as the window allows) for one shot, then back
const cinemaAt = Number(process.env.CINEMA_AT || 0);
for (let t = 20; t <= seconds; t += 20) {
  if (cinemaAt && t === cinemaAt) {
    await page.evaluate(() => document.getElementById("cinema").click());
    await new Promise((r) => setTimeout(r, 1500));
    const r = await page.evaluate(() => { const b = document.getElementById("canvas").getBoundingClientRect(); return `${Math.round(b.width)}x${Math.round(b.height)} at ${Math.round(b.left)},${Math.round(b.top)} in ${innerWidth}x${innerHeight}`; });
    console.log(`${stamp()} cinema on: canvas ${r}`);
    await page.screenshot({ path: path.join(shotDir, `web-guest-cinema.png`) });
    await page.evaluate(() => document.getElementById("cinema").click());
  }
  await new Promise((r) => setTimeout(r, 20000));
  await page.screenshot({ path: path.join(shotDir, `web-guest-${t}.png`) });
  console.log(`${stamp()} status line: ` + await page.$eval("#status", (el) => el.textContent.trim().slice(0, 140)).catch(() => "?"));
}
fs.writeFileSync(path.join(shotDir, "guest.log"), logs.join("\n") + "\n");
fs.writeFileSync(path.join(shotDir, "guest.wall"), logs.filter((l) => l.includes("TIE: wall")).join("\n") + "\n");
console.log(`${stamp()} ${logs.filter((l) => l.includes("TIE: wall")).length} wall-clock reports`);
await browser.close();
