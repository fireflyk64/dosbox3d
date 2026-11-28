// Privateer's crew in two browser pages (docs/privateer.md section 30; scripts/web-priv-crew.sh serves the page and a
// local lobby): the host's page keeps a saved game (SAVE: a ship with a turret, docked), picks "one ship, crewed" in
// the lobby's Ships menu, makes a room of 2 and flies -- its key script loads the save, launches at 50 s and lands at
// LAND_AT (default 170); the guest's page joins the room by its code, hears the room's kind, registers a new pilot
// and launches once the host flies.  Checked: the guest's launch from the pilot's base in the pilot's ship, aboard in
// its turret, both pictures, its volleys drawing on the pilot's energy, its tractor beam (B) bringing a pod the pilot
// expelled into the pilot's hold, the landing with the pilot and its own ship back.  Prints "ok"/"FAIL" and "CREW OK".
//
//   PLAYWRIGHT_DIR=... SHOT_DIR=... SAVE=<file.SAV> node web-priv-crew.mjs PAGE_URL LOBBY_URL INSTALLER
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, installer] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const save = fs.readFileSync(process.env.SAVE);
const saveName = path.basename(process.env.SAVE).toUpperCase();
const landAt = Number(process.env.LAND_AT || 200);
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (what, ok, detail) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail === undefined ? "" : ": " + (typeof detail === "string" ? detail : JSON.stringify(detail))}`);
  if (!ok) failed++;
};
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
async function open(name, script, room, seed) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 1000 } });
  const page = await ctx.newPage();
  const logs = [];
  page.on("console", (m) => {
    const t = m.text();
    logs.push(t);
    if (/crew|reports error|could not|Aborted|RuntimeError|PRCD.EXE (loaded|ended)|destroyed/.test(t)) console.log(`${stamp()} [${name}] ${t.slice(0, 190)}`);
  });
  page.on("pageerror", (e) => console.log(`${stamp()} ${name} PAGE ERROR: ${e.message}`));
  page.on("crash", () => console.log(`${stamp()} ${name} PAGE CRASHED`));
  if (seed) {
    await page.addInitScript(([n, b64]) => {
      localStorage.setItem(`wcsave:priv:${n}`, JSON.stringify({ when: Date.now(), b64 }));
    }, [saveName, save.toString("base64")]);
  }
  const u = new URL(pageUrl);
  u.searchParams.set("server", lobbyUrl);
  u.searchParams.set("hall", "CREWHALL");
  u.searchParams.set("callsign", name);
  if (room) u.searchParams.set("room", room);
  u.searchParams.set("env.WCNET_KEYSCRIPT", script);
  u.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
  u.searchParams.set("env.WCNET_LOG", "2");
  await page.goto(u.toString());
  await page.setInputFiles("#gamefile", installer);
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
    || (document.getElementById("sourceStatus").dataset.state === "ready" && /^priv/.test(document.getElementById("program").value)), null, { timeout: 600000 });
  return { page, logs, name };
}
const waitLog = async (p, re, ms) => {
  const until = Date.now() + ms;
  while (Date.now() < until) {
    if (p.logs.some((l) => re.test(l))) return true;
    await sleep(500);
  }
  return false;
};
const shot = async (p, file) => {
  const box = await p.page.$eval("#canvas", (c) => { const b = c.getBoundingClientRect(); return { x: b.x, y: b.y, width: b.width, height: b.height }; });
  await p.page.screenshot({ path: path.join(shotDir, file), clip: box });
};
// the host: the title's LOAD, the save's name, LOAD; the pad's ship at 50 s; the nav's ships put away; the base at LAND_AT
let hs = "4:esc,7:!mouse=0.38:0.94,7.5:!button=0:1,7.8:!button=0:0,10:!mouse=0.31:0.405,10.5:!button=0:1,10.8:!button=0:0,12:!mouse=0.31:0.345,12.5:!button=0:1,12.8:!button=0:0,"
  + "15:!mouse=0.85:0.345,15.5:!button=0:1,15.8:!button=0:0,50:!mouse=0.60:0.72,50.5:!button=0:1,50.8:!button=0:0,57:backspace";
for (let t = 58; t < landAt + 30; t += 5) hs += `,${t}:!privpeace`;
// (the energy while the gunner fires; then the hold expelled and the pod put behind the ship, in the tail turret's view)
for (const t of [100, 103, 106, 109, 112]) hs += `,${t}:!privguns`;
hs += ",120:x,123:!privaim=150:1:b,124:!privguns,140:!privguns";
for (let t = 125; t < 175; t += 2) hs += `,${t}:!privaim=150:1:b:if300`;     // (the pod kept within the beam's reach)
// (the items run in their order: sorted by their times)
hs = hs.split(",").map((it) => [Number(it.slice(0, it.indexOf(":"))), it]).sort((a, b) => a[0] - b[0]).map((x) => x[1]).join(",");
hs += `,${landAt}:!privgonav=3:0`;
const host = await open("PILOT", hs, "", true);
const listed = await host.page.$$eval("#saveList li", (li) => li.map((e) => e.textContent.slice(0, 14)));
check("the host's saved game is the page's", listed.some((t) => t.startsWith(saveName)), listed.join(" "));
await host.page.fill("#seats", "2");
await host.page.dispatchEvent("#seats", "change");
await host.page.click("#join");
await host.page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
const shown = await host.page.evaluate(() => { const el = document.getElementById("privCrew"); return !!el && el.offsetParent !== null && !el.disabled; });
check("the host's lobby has the Ships menu", shown);
await host.page.selectOption("#privCrew", "crew");
const code = await host.page.evaluate(() => document.getElementById("room").value);
console.log(`${stamp()} the room: ${code}; ` + await host.page.$eval("#status", (el) => el.textContent.trim().slice(0, 120)));
// the guest: a new pilot, the pad's ship at 90 s of its own boot (the host's Fly starts both: the pilot launches at 50)
const gs = "4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter,89:!mouse=0.45:0.6,90:!button=0:1,90.3:!button=0:0"
  + ",126:!privtargetpod";
const guest = await open("GUNNER", gs, code, false);
await guest.page.waitForFunction(() => !document.getElementById("lobby").hidden, null, { timeout: 60000 });
await sleep(3000);
const guestSees = await guest.page.evaluate(() => { const el = document.getElementById("privCrew"); return el ? `${el.value}${el.disabled ? " (greyed)" : ""}${el.offsetParent === null ? " hidden" : ""}` : "none"; });
check("the guest's lobby shows the host's choice, greyed", guestSees === "crew (greyed)", guestSees);
const chat = await guest.page.evaluate(() => document.body.textContent);
check("the guest is told it mans a turret", /one ship, crewed: PILOT flies/.test(chat), (chat.match(/[^.]*one ship, crewed: PILOT[^.]*\./) || [""])[0].slice(0, 120));
await host.page.screenshot({ path: path.join(shotDir, "crew-host-lobby.png"), fullPage: true });
await guest.page.screenshot({ path: path.join(shotDir, "crew-guest-lobby.png"), fullPage: true });
await host.page.evaluate(() => { document.getElementById("fullStart").checked = false; });
await guest.page.evaluate(() => { document.getElementById("fullStart").checked = false; });
await host.page.click("#fly");
check("the host's emulator: a crew's room, its pilot", await waitLog(host, /crew: we are the pilot/, 120000));
check("the host launched: its ship told", await waitLog(host, /crew: our ship for the gunners/, 120000), (host.logs.find((l) => /our ship for the gunners/.test(l)) || "").slice(0, 120));
// (the host's Fly started the guest's page too, or its own Fly does)
if (!guest.logs.some((l) => /PRCD\.EXE loaded/.test(l))) {
  const can = await guest.page.evaluate(() => { const f = document.getElementById("fly"); return !!f && !f.disabled && f.offsetParent !== null; });
  if (can) await guest.page.click("#fly");
}
check("the guest's emulator: the gunner of turret 1", await waitLog(guest, /crew: we are the gunner of turret 1/, 120000));
check("the guest's launch begins at the pilot's base", await waitLog(guest, /crew: this launch is from the pilot's base/, 120000),
  (guest.logs.find((l) => /this launch is from/.test(l)) || "").slice(0, 150));
check("the guest's flight is flown in the pilot's ship", await waitLog(guest, /crew: this flight is flown in the pilot's ship/, 30000),
  (guest.logs.find((l) => /flown in the pilot's ship/.test(l)) || "").slice(0, 150));
check("the guest is aboard, in its turret", await waitLog(guest, /crew: aboard the pilot's ship in \w+ \(in our turret\)/, 60000),
  (guest.logs.find((l) => /aboard the pilot/.test(l)) || "").slice(0, 150));
await sleep(4000);
await shot(host, "crew-host-flight.png");
await shot(guest, "crew-guest-turret.png");
// the gunner fires: its volleys shown in the pilot's game
await guest.page.click("#canvas");
await guest.page.keyboard.down("Space");
await sleep(4000);
await shot(guest, "crew-guest-firing.png");
await sleep(8000);      // (the pilot's log says it every 20 bolts)
await guest.page.keyboard.up("Space");
check("the gunner's volleys shown in the pilot's game", await waitLog(host, /crew: a gunner's turret 1 fired here/, 15000),
  (host.logs.filter((l) => /turret 1 fired here/.test(l)).pop() || "").slice(0, 120));
// the power: the turret's volleys drawn from the pilot's pool (one pool for the ship), the gunner's copy on it
const energies = (p) => p.logs.map((l) => l.match(/guns [\d.]+: .*energy (\d+) of (\d+)/)).filter(Boolean).map((m) => [Number(m[1]), Number(m[2])]);
await waitLog(host, /guns 11\d\.\d: .*energy/, 30000);
const he = energies(host);
check("the pilot's energy spent by the gunner's volleys", he.some(([e, max]) => e < max), he.map(([e]) => e).join(" "));
// the tractor beam: the pilot expels its hold at 120 s and the pod is behind the ship; the gunner targets it (126 s)
// and holds B in its turret; the pilot's game pulls it into the pilot's hold
check("the gunner's target is the pilot's pod", await waitLog(guest, /!privtargetpod: the target is pod/, 90000),
  (guest.logs.find((l) => /privtargetpod/.test(l)) || "").slice(0, 120));
await guest.page.click("#canvas");
await guest.page.keyboard.down("KeyB");
check("the pilot's game: the gunner's tractor beam on its pod", await waitLog(host, /crew: turret 1's tractor beam on our cargo pod/, 15000),
  (host.logs.find((l) => /tractor beam on our/.test(l)) || "").slice(0, 120));
check("the pod's cargo in the pilot's hold", await waitLog(host, /crew: our cargo pod ds:\w+ is gone \(turret 1's beam on it\): \d+ units aboard/, 20000),
  (host.logs.find((l) => /our cargo pod ds:\w+ is gone/.test(l)) || "").slice(0, 150));
check("the gunner told the cargo is aboard", await waitLog(guest, /Cargo aboard: \d+ units into the hold/, 10000),
  (guest.logs.find((l) => /Cargo aboard/.test(l)) || "").slice(0, 120));
await shot(guest, "crew-guest-tractor.png");
await guest.page.keyboard.up("KeyB");
check("the guest lands with the pilot, its own ship its own again", await waitLog(guest, /crew: landed: the base's ship is our own again/, (landAt + 120) * 1000),
  (guest.logs.find((l) => /landed: the base/.test(l)) || "").slice(0, 150));
await sleep(8000);
await shot(host, "crew-host-landed.png");
await shot(guest, "crew-guest-landed.png");
for (const p of [host, guest]) {
  const errs = p.logs.filter((l) => /reports error|PRCD\.EXE ended/.test(l));
  check(`${p.name}: no error of the game's`, errs.length === 0, errs.slice(0, 2).join(" | ").slice(0, 160));
}
console.log(failed ? `CREW FAILED (${failed})` : "CREW OK");
await browser.close();
process.exit(failed ? 1 : 0);
