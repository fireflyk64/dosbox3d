// Privateer in the browser, a room of one (scripts/web-priv.sh starts the page's server and a local lobby): GOG's
// installer dropped into the page, the game found and started, scripts/privrun.sh's route into flight from Achilles
// (as a key script), then the page's own keyboard: Right held 0.6 s, a tap, and the emulator's log of the ramp
// (src/cpu/wcnet_priv_controls.cpp); pictures in SHOT_DIR; the saved-game panel.  Prints "ok"/"FAIL" lines and
// "PRIV OK" when every check passed.
//
//   PLAYWRIGHT_DIR=... SHOT_DIR=... node scripts/web-priv.mjs PAGE_URL LOBBY_URL INSTALLER [seconds]
//   RF=1: Righteous Fire (the menu's other program)
//   ROOM=<code>: the page joins that room as a guest (scripts/web-priv.sh ROOM=1 makes a native host's) and checks the
//   room's presence lines (src/cpu/wcnet_priv_session.cpp) instead of flying alone
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, installer, secondsArg = "60"] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl || !installer) {
  console.error("usage: node web-priv.mjs PAGE_URL LOBBY_URL INSTALLER [seconds]");
  process.exit(2);
}
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const page = await browser.newPage({ viewport: { width: 1280, height: 1000 } });
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
const logs = [];
page.on("console", (m) => {
  const t = m.text();
  logs.push(`${stamp()} ${t}`);
  if (/Privateer|could not|Aborted|RuntimeError|save games|comms:/.test(t) && !/arrows [+-]/.test(t)) console.log(`${stamp()} [page] ${t.slice(0, 200)}`);
});
page.on("pageerror", (e) => console.log(`${stamp()} PAGE ERROR: ${e.message}`));
page.on("crash", () => console.log(`${stamp()} PAGE CRASHED`));
let failed = 0;
const check = (what, ok, detail) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail === undefined ? "" : ": " + (typeof detail === "string" ? detail : JSON.stringify(detail))}`);
  if (!ok) failed++;
};
const want = process.env.RF ? "privrf" : "priv";
// (the route of scripts/privrun.sh: Esc through the opening, NEW, a pilot "joe" / "ace", the Tarsus clicked on the pad)
const route = "4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter," +
  "15:!mouse=0.45:0.6,16:!button=0:1,16.3:!button=0:0";
const u = new URL(pageUrl);
u.searchParams.set("server", lobbyUrl);
u.searchParams.set("hall", "PRVHALL-" + Math.random().toString(36).slice(2, 6).toUpperCase());
u.searchParams.set("callsign", "PRIVTEST");
if (process.env.ROOM) u.searchParams.set("room", process.env.ROOM);
u.searchParams.set("env.WCNET_KEYSCRIPT", route);
u.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
u.searchParams.set("env.WCNET_LOG", "3");
await page.goto(u.toString());
await page.setInputFiles("#gamefile", installer);
await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
  || (document.getElementById("sourceStatus").dataset.state === "ready" && /^priv/.test(document.getElementById("program").value)), null, { timeout: 600000 });
const state = await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 140)}`);
check("the installer unpacked and the game recognised", state.startsWith("ready"), state);
const programs = await page.$$eval("#program option", (os) => os.map((o) => o.value));
check("the menu has Privateer and Righteous Fire", programs.includes("priv") && programs.includes("privrf"), programs);
if (want !== "priv") {
  await page.selectOption("#program", want);
  await sleep(500);
}
const keys = await page.$eval("#gameKeys", (el) => (el.hidden ? "" : el.textContent));
check("the flight's keys under the game", /Privateer in flight/.test(keys) || want === "privrf", keys.slice(0, 60));
const saves = await page.$eval("#saves", (el) => (el.hidden ? "hidden" : document.getElementById("saveStatus").textContent));
check("the saved-game panel", /save in the game/.test(saves), saves);
await page.screenshot({ path: path.join(shotDir, "web-priv-page.png"), fullPage: true });
if (process.env.ROOM) {
  // the room's link joined it on load: Fly once the page lets a guest (a native host never says it flies: 5 s)
  await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 60000 });
  console.log(`${stamp()} in the room: ` + await page.$eval("#status", (el) => el.textContent.trim().slice(0, 140)));
  const max = await page.$eval("#seats", (el) => el.max);
  check("the Players box takes 8 for Privateer", max === "8", max);
  // the Face box seen in a room (it sat in the room's row of choices, hidden for a game the room does not fly: the user,
  // 2026-11-09, "I also don't see an option to select comm portrait")
  const face = await page.evaluate(() => { const el = document.getElementById("privFace"); return el && el.offsetParent !== null ? el.options.length : 0; });
  check("the Face box shows in the room, with its 8 faces", face === 8, face);
} else if (process.env.SEATS) {
  // a room of SEATS made by this page, its host, alone in it (the lobby server's own limit on a room's seats)
  await page.fill("#seats", process.env.SEATS);
  await page.dispatchEvent("#seats", "change");
  await page.click("#join");
  await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
  console.log(`${stamp()} in the room: ` + await page.$eval("#status", (el) => el.textContent.trim().slice(0, 140)));
  const face = await page.evaluate(() => { const el = document.getElementById("privFace"); return el && el.offsetParent !== null ? el.options.length : 0; });
  check("the Face box shows in the room, with its 8 faces", face === 8, face);
} else {
  // a room of one (Players 1): the game alone, as it is
  await page.fill("#seats", "1");
  await page.dispatchEvent("#seats", "change");
  await page.click("#join");
  await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
}
await page.click("#fly");
const waitLog = async (re, ms) => {
  const until = Date.now() + ms;
  while (Date.now() < until) {
    if (logs.some((l) => re.test(l))) return true;
    await sleep(500);
  }
  return false;
};
// the music made the Sound Blaster's FM before the launch (GOG's SOUND.CFG asks for General MIDI: none in the browser; the
// user, 2026-11-09: "also zero music here")
check("Privateer's music on the Sound Blaster's FM", logs.some((l) => /Privateer's music: .* changed to 2/.test(l)) || await waitLog(/Privateer's music: .* changed to 2/, 5000),
  (logs.find((l) => /Privateer's music/.test(l)) || "no word of the music").slice(0, 140));
check("PRCD.EXE found by the layer", await waitLog(/Privateer.*PRCD\.EXE loaded at/, 60000),
  (logs.find((l) => /PRCD\.EXE loaded/.test(l)) || "").slice(0, 120));
const flying = await waitLog(/keyboard frames, controls: keyboard/, 90000);
check("in flight, the keyboard's controls read every frame", flying, (logs.filter((l) => /keyboard frames/.test(l)).pop() || "").slice(0, 120));
await sleep(3000);
const box = await page.$eval("#canvas", (c) => { const b = c.getBoundingClientRect(); return { x: b.x, y: b.y, width: b.width, height: b.height }; });
await page.screenshot({ path: path.join(shotDir, "web-priv-flight.png"), clip: box });
// the page's own keyboard: Right held 0.6 s, then a tap
await page.focus("#canvas");
const before = logs.length;
await page.keyboard.down("ArrowRight");
await sleep(600);
await page.keyboard.up("ArrowRight");
await sleep(1500);
await page.keyboard.down("ArrowRight");
await sleep(60);
await page.keyboard.up("ArrowRight");
await sleep(1500);
const ramp = logs.slice(before).filter((l) => /arrows [+-]/.test(l)).map((l) => Number(/arrows [+-]\d+ ([+-]\d+)/.exec(l)[1]));
check("the arrow ramped from a ninth of the stop", ramp.length > 0 && ramp[0] === -28, ramp.slice(0, 12).join(" "));
check("... to the stop when held", ramp.includes(-256), `${ramp.filter((v) => v === -256).length} frames at the stop`);
await page.screenshot({ path: path.join(shotDir, "web-priv-turned.png"), clip: box });
if (process.env.SEATS) {
  check(`the room's session: the host of ${process.env.SEATS} seats`, await waitLog(new RegExp(`Privateer: HOST at room \\S+, ${process.env.SEATS} seats`), 20000),
    (logs.find((l) => /Privateer: HOST at/.test(l)) || "").slice(0, 140));
}
if (process.env.ROOM) {
  check("in the host's room", await waitLog(/Privateer: In \S+'s room as pilot 2/, 20000), (logs.find((l) => /room as pilot/.test(l)) || "").slice(0, 140));
  check("the host's presence: in Troy with us", await waitLog(/Privateer: (HOST launched in Troy|HOST in Troy, [\d,]+ m from you|In Troy with you: HOST)/, 40000),
    (logs.filter((l) => /Privateer: (HOST|In Troy)/.test(l)).pop() || "").slice(0, 140));
  check("the host's ship in our space (a puppet)", await waitLog(/Privateer: HOST's \w+ is in your space/, 30000),
    (logs.find((l) => /is in your space/.test(l)) || "").slice(0, 140));
  // the 0 key's chat prompt in flight, as a player types (the user, 2026-11-09: "0 also doesn't seem to do comms": it
  // opened only for Wing Commander's session)
  await page.focus("#canvas");
  await page.keyboard.press("0");
  await sleep(500);
  await page.keyboard.type("HELLO HOST", { delay: 60 });
  await page.keyboard.press("Enter");
  check("the 0 key's prompt: our line said to the room", await waitLog(/Privateer: said "HELLO HOST"/, 10000),
    (logs.filter((l) => /Privateer: said/.test(l)).pop() || "nothing said").slice(0, 140));
  // (the host's side, told of this autopilot and given our ship, is checked by web-priv.sh from the host's log)
  await page.keyboard.press("a");
}
const seconds = Number(secondsArg);
const until = t0 + seconds * 1000;
while (Date.now() < until) await sleep(2000);
await page.screenshot({ path: path.join(shotDir, "web-priv-end.png"), clip: box });
fs.writeFileSync(path.join(shotDir, "web-priv.log"), logs.join("\n") + "\n");
console.log(failed ? `PRIV FAILED (${failed})` : "PRIV OK");
await browser.close();
process.exit(failed ? 1 : 0);
