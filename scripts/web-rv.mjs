// RetroVega in the browser build (headless Chrome pages, a local lobby
// server), as scripts/web-smoke.sh rv runs it.  RetroVega is built into
// every page (since 2026-11-03; ?retrovega in the address changes nothing),
// and:
//
//   0. a page with no game files, ?retrovega or not, plays RetroVega, and
//      its lobby is the one public lobby, every game's (WC-LOBBY);
//   1. a page with no game files at all (a fresh browser, a site with no
//      wc.tar.gz) has RetroVega in its "Play" menu, enters the public lobby
//      by itself, joins a room and flies alone: the game starts, finds the
//      OPL3, and the keyboard reaches it (Tab jumps between hotspots, a held
//      arrow key steers the pointer);
//   1b. a room of one (Players 1) picks a mission too, the campaign as the
//      game starts it by default: the game starts at the picked one;
//   2. a host and a wingman meet in an RV- room by its link; the host's Fly
//      starts both games;
//   3. a shared flight (retrovega/docs/multiplayer.md): the host's menu has
//      every mission of rv-campaign.json and 30 frames a second; the host
//      flies mission 0 and the wingman asked for 3: both fly the host's,
//      each has the other's ship where it is (offsets from wcnet_rv.h).
//
//   node scripts/web-rv.mjs PAGE_URL LOBBY_URL [seconds]
//
// PAGE_URL must serve a page without wc.tar.gz beside it (web-smoke.sh rv
// serves a copy of web/dist without it).  Playwright comes from
// PLAYWRIGHT_DIR; SHOT_DIR gets screenshots, the page logs and each game's
// RVLOG.TXT.
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, secondsArg = "30"] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl) {
  console.error("usage: node web-rv.mjs PAGE_URL LOBBY_URL [seconds]");
  process.exit(2);
}
const seconds = Number(secondsArg);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const chrome = process.env.CHROME || "/usr/bin/google-chrome";
const shotDir = process.env.SHOT_DIR || "";
const tag = Math.random().toString(36).slice(2, 6).toUpperCase();
const hallCode = "RVHALL-" + tag;
const browser = await chromium.launch({ headless: true, executablePath: chrome,
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const checks = [];
const check = (what, ok, detail = "") => { checks.push({ what, ok }); console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail ? ": " + detail : ""}`); };
const logs = {};

async function open(name, query) {
  // (each page its own browser context: nothing saved from another page)
  const context = await browser.newContext();
  const page = await context.newPage();
  logs[name] = [];
  page.on("console", (m) => logs[name].push(m.text()));
  page.on("pageerror", (e) => { logs[name].push("PAGE ERROR: " + e.message); console.log(`[${name}] PAGE ERROR: ${e.message}`); });
  const u = new URL(pageUrl);
  u.searchParams.set("server", lobbyUrl);
  u.searchParams.set("hall", hallCode);
  for (const [k, v] of Object.entries(query)) u.searchParams.set(k, v);
  await page.goto(u.toString());
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready", null, { timeout: 60000 });
  return page;
}
const rvlog = (page) => page.evaluate(() => {
  try { return new TextDecoder().decode(window.DOSBox.FS.readFile("/game/RVLOG.TXT")); } catch (e) { return ""; }
});
const waitLog = async (page, re, ms) => {
  const until = Date.now() + ms;
  for (;;) {
    const t = await rvlog(page);
    if (re.test(t)) return t;
    if (Date.now() > until) return t;
    await sleep(500);
  }
};
async function keep(name, page) {
  if (!shotDir) return;
  await page.screenshot({ path: path.join(shotDir, `rv-${name}.png`) });
  fs.writeFileSync(path.join(shotDir, `rv-${name}.log`), logs[name].join("\n") + "\n");
  fs.writeFileSync(path.join(shotDir, `rv-${name}-RVLOG.TXT`), await rvlog(page));
}

// -- 0. without ?retrovega: RetroVega all the same ----------------------------------
console.log(`lobby ${hallCode} at ${lobbyUrl}`);
// (hall "": the lobby's own name, which the page shows; lobby=off: it does not go in)
const plain = await open("plain", { callsign: "PLAIN", hall: "", lobby: "off" });
check("without ?retrovega a page with no files plays RetroVega", (await plain.$eval("#program", (el) => el.value)) === "rv",
  await plain.$eval("#program", (el) => [...el.options].map((o) => o.value).join(", ")));
check("and its lobby is the public one, every game's: WC-LOBBY", (await plain.$eval("#hallNote code.lobbyName", (el) => el.textContent)) === "WC-LOBBY",
  await plain.$eval("#hallNote code.lobbyName", (el) => el.textContent));
await plain.context().close();

// -- 1. no files: RetroVega, the lobby, a flight alone ---------------------------
// (/verbose: the Tab and steering checks read the frames' lines, RV.EXE keeps them to itself otherwise)
const solo = await open("solo", { callsign: "SOLO", retrovega: "", cmd: "rv /noquit /verbose" });
check("with no files the page plays RetroVega", (await solo.$eval("#program", (el) => el.value)) === "rv",
  await solo.$eval("#program", (el) => [...el.options].map((o) => o.textContent).join(", ")));
await solo.waitForFunction(() => window.__wcHall && window.__wcHall.inside(), null, { timeout: 30000 }).catch(() => {});
check("and is in the public lobby at once", await solo.evaluate(() => !!(window.__wcHall && window.__wcHall.inside())));
const code = await solo.$eval("#room", (el) => el.value);
check("its room code is an RV- code", /^RV-\d{4}$/.test(code), code);
await solo.click("#join");
await solo.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
await solo.click("#fly");
let t = await waitLog(solo, /input: mouse/, 90000);
check("the game started (fetched from the site)", logs.solo.some((l) => /fetched \d+ files of RetroVega/.test(l)) && /rvHello: DS is ready/.test(t));
check("it plays its music on the OPL3", /music: OPL3 at 388h, our bank/.test(t), (t.match(/music: [^\n]*/) || [""])[0]);
check("the hooks know RV.EXE", logs.solo.some((l) => /RetroVega loaded at/.test(l)));
await sleep(4000);
await solo.focus("#canvas");
await solo.keyboard.press("Enter");                     // the welcome message
await sleep(1500);
await solo.keyboard.press("Tab");                       // the pointer on a hotspot (said in RVLOG.TXT)
await sleep(1500);
await solo.keyboard.down("ArrowLeft");                  // then steered left (said when the key goes up)
await sleep(700);
await solo.keyboard.up("ArrowLeft");
await sleep(1500);
t = await rvlog(solo);
const tab = /tab: to (\d+),(\d+)/.exec(t), steered = /keys: steered to (\d+),(\d+)/.exec(t);
check("Tab puts the pointer on a hotspot", !!tab, tab ? tab[0] : "");
check("a held arrow key steers it", !!(tab && steered && Number(steered[1]) < Number(tab[1]) - 20 && steered[2] === tab[2]),
  steered ? steered[0] : "no steering in the log");
await sleep(Math.max(0, seconds * 1000 - 10000));
t = await rvlog(solo);
check("no stop in the game", !/FAIL /.test(t));
await keep("solo", solo);

// -- 2. a host and a wingman ----------------------------------------------------
const room = "RV-" + String(Math.floor(Math.random() * 9000) + 1000);
const host = await open("host", { room, callsign: "HOST", retrovega: "" });
await host.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
const wing = await open("wing", { room, callsign: "WINGMAN" });   // (an RV- room's link offers it too)
await wing.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 1, null, { timeout: 30000 });
await host.waitForFunction(() => document.getElementById("roster").children.length > 1, null, { timeout: 30000 });
check("both pages play RetroVega in room " + room, (await host.$eval("#program", (el) => el.value)) === "rv" && (await wing.$eval("#program", (el) => el.value)) === "rv");
await host.click("#fly");
const [th, tw] = await Promise.all([waitLog(host, /input: mouse/, 90000), waitLog(wing, /input: mouse/, 90000)]);
check("the host's Fly started the host's game", /rvHello: DS is ready/.test(th) && /music: OPL3/.test(th));
check("and the wingman's", /rvHello: DS is ready/.test(tw) && /music: OPL3/.test(tw));
check("the wingman's page saw the host start", logs.wing.some((l) => /fetched \d+ files of RetroVega/.test(l)));
await sleep(Math.max(0, seconds * 1000 - 10000));
check("no stop in either game", !/FAIL /.test(await rvlog(host)) && !/FAIL /.test(await rvlog(wing)));
await keep("host", host);
await keep("wing", wing);

// -- 3. a shared flight ----------------------------------------------------------
await host.context().close();
await wing.context().close();
const rv = Object.fromEntries([...fs.readFileSync(new URL("../src/cpu/wcnet_rv.h", import.meta.url), "utf8")
  .matchAll(/#define (RV_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b/g)].map((m) => [m[1], Number(m[2])]));
const bytes = (page, off, n) => page.evaluate(([o, k]) => Array.from({ length: k }, (_, i) => window.DOSBox._wc_web_ds_byte(o + i)), [off, n]);
const word = (b, i) => b[i] | (b[i + 1] << 8);
const long = (b, i) => (b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24));
async function shared(page) {
  const slots = rv.RV_MAX_SHIP_SLOT + 1;
  const pos = await bytes(page, rv.RV_DS_gPositionVector, 12 * slots);
  const s = { role: (await bytes(page, rv.RV_DS_rvNetRole, 1))[0], node: (await bytes(page, rv.RV_DS_currentSeries, 1))[0],
    mission: (await bytes(page, rv.RV_DS_currentMission, 1))[0], frame: word(await bytes(page, rv.RV_DS_frameCounter, 2), 0),
    ctl: await bytes(page, rv.RV_DS_rvController, slots), net: await bytes(page, rv.RV_DS_rvNetId, slots),
    flags: await bytes(page, rv.RV_DS_rvFlags, 2 * slots), pos: [] };
  for (let i = 0; i < slots; i++) s.pos.push([long(pos, 12 * i), long(pos, 12 * i + 4), long(pos, 12 * i + 8)]);
  return s;
}
const km = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]) / 256;
// The new pilot's way in (the user, 2026-11-03): a host in a room of two with nobody else presses Fly and flies (the
// first mission together never waits for anybody); the Fly advertised the room in the lobby; a pilot there clicks its
// code, presses Fly, and joins the mission in progress.
const h2 = await open("host2", { callsign: "CHEATHOST", cmd: "rv /noquit /fly 0" });
const w2 = await open("wing2", { callsign: "WINGMAN", cmd: "rv /noquit /fly 3" });
const inLobby = (page) => page.waitForFunction(() => !document.getElementById("hallBody").hidden && !!window.__wcHall.test.net(), null, { timeout: 30000 }).then(() => true, () => false);
check("both pilots are in the lobby", (await inLobby(h2)) && (await inLobby(w2)));
await h2.click("#join");
await h2.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
const room2 = await h2.$eval("#room", (el) => el.value);
const campaign = await (await fetch(new URL("rv-campaign.json", pageUrl))).json();
const missions = campaign.series.reduce((n, s) => n + s.missions.length, 0);
await h2.waitForFunction((n) => document.querySelectorAll("#mission option").length === n + 1, missions, { timeout: 20000 }).catch(() => {});
check(`the host's menu: the barracks and the campaign's ${missions} missions`, (await h2.$$eval("#mission option", (os) => os.length)) === missions + 1);
check("the host's frame rate is 30", (await h2.$eval("#fps", (el) => el.value)) === "30");
await h2.click("#fly");
const flying = (page) => page.waitForFunction(() => window.DOSBox && window.DOSBox._wc_web_in_flight && window.DOSBox._wc_web_in_flight() === 1, null, { timeout: 150000 }).then(() => true, () => false);
check(`a host alone in room ${room2} of two flies: nobody is waited for`, await flying(h2));
check("as the host", (await bytes(h2, rv.RV_DS_rvNetRole, 1))[0] === 1);
const offered = await w2.waitForFunction((code) => Array.from(document.querySelectorAll("#hallLog a.roomcode")).some((x) => x.textContent === code), room2, { timeout: 20000 }).then(() => true, () => false);
check("the host's Fly advertised the room in the lobby", offered, await w2.$eval("#hallLog", (el) => el.textContent.slice(-160)));
check("and the host is in the lobby still", await inLobby(h2));
if (offered) await w2.evaluate((code) => Array.from(document.querySelectorAll("#hallLog a.roomcode")).find((x) => x.textContent === code).click(), room2);
else { await w2.fill("#room", room2); await w2.click("#join"); }   // (the code typed: the checks after this still run)
await w2.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 1, null, { timeout: 30000 });
await sleep(10000);   // (the host flies on alone meanwhile)
await w2.click("#fly");
check("the wingman's Fly takes it into the mission in progress", await flying(w2));
for (const page of [h2, w2]) { await page.focus("#canvas"); await page.keyboard.press("s"); }   // (stopped: where each is holds still)
await sleep(8000);
const [hs, ws] = [await shared(h2), await shared(w2)];
check("the host is the host and the wingman a guest", hs.role === 1 && ws.role === 2, `${hs.role} ${ws.role}`);
check("the wingman flies the host's mission", hs.node === ws.node && hs.mission === ws.mission, `host ${hs.node}/${hs.mission}, wingman ${ws.node}/${ws.mission}`);
const me = ws.net[0];
check("the host has the wingman's ship, a human's puppet, where it is", me >= 1 && me <= rv.RV_MAX_SHIP_SLOT && hs.ctl[me] === 2 && (hs.flags[2 * me] & 32) && km(hs.pos[me], ws.pos[0]) < 150,
  me >= 1 ? `slot ${me}, ${km(hs.pos[me], ws.pos[0]).toFixed(0)} km off` : `net id ${me}`);
const body = ws.net.findIndex((n, i) => i > 0 && n === 0 && ws.ctl[i] === 2);
check("the wingman has the host's ship where it is", body > 0 && km(ws.pos[body], hs.pos[0]) < 150, body > 0 ? `${km(ws.pos[body], hs.pos[0]).toFixed(0)} km off` : "none");
check("no stop in either game", !/FAIL /.test(await rvlog(h2)) && !/FAIL /.test(await rvlog(w2)));
// only 0 opens the comms prompt: a tap of Shift did too, and pilots brushing the right Shift above the arrows lost the
// stick to it (the user, 2026-11-10: "Shift is right above the keypad and people are losing control all the time")
const said = () => logs.wing2.filter((l) => /comms: message sent/.test(l)).length;
const saidBefore = said();
await w2.focus("#canvas");
await w2.keyboard.press("Shift");
await sleep(400);
await w2.keyboard.type("zz", { delay: 80 });
await w2.keyboard.press("Enter");
await sleep(1500);
check("a tap of Shift opens no comms prompt", said() === saidBefore, `${said() - saidBefore} line(s) sent`);
// and an arrow held as 0 opens the prompt is let go in the game: no release reaches it while the prompt is open, and
// the ship turned on till the line was sent and after (the user: "the arrow keys would hold down with no recourse")
const front = async (page) => { const b = await bytes(page, rv.RV_DS_gOrientationFrontVector, 12); return [long(b, 0), long(b, 4), long(b, 8)]; };
const degrees = (a, b) => Math.acos(Math.max(-1, Math.min(1, (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (Math.hypot(...a) * Math.hypot(...b))))) * 180 / Math.PI;
const heldFrom = await front(w2);
await w2.keyboard.down("ArrowLeft");
await sleep(700);
const heldTurn = degrees(heldFrom, await front(w2));
await w2.keyboard.press("0");
await sleep(200);
await w2.keyboard.up("ArrowLeft");
await sleep(500);
const typingFrom = await front(w2);
await sleep(2000);
const typingTurn = degrees(typingFrom, await front(w2));
check("an arrow held turns the ship", heldTurn > 1, `${heldTurn.toFixed(1)} degrees in 0.7 s`);
check("and is let go when 0 opens the prompt", typingTurn < 0.5, `${typingTurn.toFixed(1)} degrees in 2 s at the prompt`);
await w2.keyboard.type("hello room", { delay: 80 });
await w2.keyboard.press("Enter");
await sleep(1500);
check("the 0 key's prompt sends a line", said() === saidBefore + 1, `${said() - saidBefore} line(s) sent`);
const sentFrom = await front(w2);
await sleep(2000);
const sentTurn = degrees(sentFrom, await front(w2));
check("and the ship flies straight after the line", sentTurn < 0.5, `${sentTurn.toFixed(1)} degrees in 2 s`);
await keep("host2", h2);
await keep("wing2", w2);
await h2.context().close();
await w2.context().close();

// -- 1b. a room of one picks a mission -------------------------------------------------
// (last: it reads the game's campaign place as section 3 does)
const alone = await open("alone", { callsign: "ALONE" });
await alone.fill("#seats", "1");
await alone.dispatchEvent("#seats", "change");
await alone.click("#join");
await alone.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
await alone.waitForFunction((n) => document.querySelectorAll("#mission option").length === n + 1, missions, { timeout: 20000 }).catch(() => {});
const menu = await alone.evaluate(() => {
  const sel = document.getElementById("mission");
  return { shown: !document.getElementById("missionRow").hidden && !sel.parentElement.hidden, enabled: !sel.disabled,
           n: sel.options.length, first: sel.options[0] ? sel.options[0].textContent : "", value: sel.value,
           hint: document.getElementById("missionHint").textContent };
});
check(`a room of one has the mission menu: the campaign, then the ${missions} missions`,
  menu.shown && menu.enabled && menu.n === missions + 1 && /^Campaign from your quarters/.test(menu.first), JSON.stringify(menu));
check("to begin with the campaign as the game starts it", menu.value === "" && /quarters/.test(menu.hint), menu.hint);
const pick = campaign.series[1];       // (the second node's first mission)
await alone.selectOption("#mission", `${pick.node + 1}/0`);
const pickedHint = await alone.$eval("#missionHint", (el) => el.textContent);
const pickName = pick.id.charAt(0) + pick.id.slice(1).toLowerCase();
check("a picked mission is said beside the menu", pickedHint.startsWith(`${pickName} 1 `) && /You start in the briefing room for it/.test(pickedHint), pickedHint);
await alone.click("#fly");
await waitLog(alone, /input: mouse/, 90000);
check("the game is launched at it", logs.alone.some((l) => l.includes(`rv /noquit /brief ${pick.missions[0].n}`)),
  logs.alone.filter((l) => /\/brief|alone/.test(l)).join(" | "));
await sleep(3000);
const at = { node: (await bytes(alone, rv.RV_DS_currentSeries, 1))[0], mission: (await bytes(alone, rv.RV_DS_currentMission, 1))[0] };
check("and its campaign is there", at.node === pick.node && at.mission === 0, JSON.stringify(at));
check("no stop in the game", !/FAIL /.test(await rvlog(alone)));
await keep("alone", alone);

await browser.close();
const ok = checks.every((c) => c.ok);
console.log(ok ? `RV OK (${checks.length} checks)` : `RV FAILED (${checks.filter((c) => !c.ok).length} of ${checks.length} checks)`);
process.exit(ok ? 0 : 1);
