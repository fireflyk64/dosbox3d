// A laptop put to sleep in an open Privateer room, and woken (docs/privateer.md section 40; scripts/web-priv-sleep.sh
// serves the page and a local lobby; the user, 2026-11-27: "when they regain connectivity are they going to rejoin
// without returning to the menu.  Thats the ideal experience.  So they can sleep the laptop and then wake it and
// continue playing"; "What happens during the low player handoff during that two minute disconnect dance if it's
// lowest player ... it must be robust").
//
// ALPHA (seat 1 of the room, the lowest: it decides who runs the nav points) and BRAVO fly; each sees the other.
// ALPHA's browser -- a Chrome of its own -- is stopped whole for ASLEEP s (SIGSTOP to every process of it: nothing
// it says, nothing it answers, its connections left as they were: a laptop's lid closed).  BRAVO flies on: it passes
// ALPHA over and decides itself; ALPHA's link fails, its ship goes from BRAVO's space.  ALPHA's browser goes on
// (SIGCONT): its page sees that its clock jumped, joins the room again with its seat's token (the same seat) while its
// game flies on -- never the menu -- the links are made again, each sees the other's ship again, and ALPHA, once it
// has BRAVO's table, decides again.  Pictures of both screens before, during and after.  Prints "ok"/"FAIL" and
// "SLEEP OK".
//
// SCENARIO=reseat: the room is of 2 (full), the seat's claim after CLAIM_MS (the page's ?claimms=, 40 s: more than the
// pages' 25 s keep-alive, roomBeat -- or a live page's seat is claimable too) instead of two minutes; while ALPHA sleeps, CHARLIE comes to the full room and takes ALPHA's silent seat (seat 1), then BRAVO
// leaves (seat 2 free).  ALPHA wakes: its token is no good any more, it joins in seat 2 -- its game takes the new
// seat and flies on (pilot 2 now), links with CHARLIE, each sees the other; CHARLIE, the lowest seat, decides.
//
//   PLAYWRIGHT_DIR=... SHOT_DIR=... ASLEEP=60 SCENARIO=same|reseat node web-priv-sleep.mjs PAGE_URL LOBBY_URL INSTALLER
import { createRequire } from "node:module";
import { execSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

const [pageUrl, lobbyUrl, installer] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const scenario = process.env.SCENARIO || "same";
const asleep = Number(process.env.ASLEEP || (scenario === "reseat" ? 50 : 60));
const claimMs = Number(process.env.CLAIM_MS || 40000);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (what, ok, detail) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail === undefined ? "" : ": " + (typeof detail === "string" ? detail : JSON.stringify(detail))}`);
  if (!ok) failed++;
};
// Chrome processes that are this node's children now (a browser's main process), and a process's whole tree
const chromeChildren = () => {
  const out = execSync(`ps -eo pid=,ppid=,comm=`).toString().trim().split("\n").map((l) => l.trim().split(/\s+/));
  return out.filter(([, ppid, comm]) => Number(ppid) === process.pid && /chrome/.test(comm)).map(([pid]) => Number(pid));
};
const tree = (root) => {
  const rows = execSync(`ps -eo pid=,ppid=`).toString().trim().split("\n").map((l) => l.trim().split(/\s+/).map(Number));
  const all = [root];
  for (let i = 0; i < all.length; i++) for (const [pid, ppid] of rows) if (ppid === all[i]) all.push(pid);
  return all;
};
const launch = () => chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const before = new Set(chromeChildren());
const browserA = await launch();
const alphaChrome = chromeChildren().filter((p) => !before.has(p));
check("ALPHA's own Chrome found", alphaChrome.length === 1, alphaChrome.join(","));
const browserB = await launch();
// a new pilot (scripts/privrun.sh's route), launched from Achilles at 16 s of its own; from 30 s, every 15 s, a turn
// toward the nearest ship (another pilot's, launched from the same pad) for the pictures
let route = "4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter,15:!mouse=0.45:0.6,16:!button=0:1,16.3:!button=0:0";
for (let t = 30; t < 900; t += 15) route += `,${t}:!privturn=ship:90:10`;
// (from 150 s of its own -- after the first pictures, and after the sleep for the sleeper, whose clock stood still --
// each pilot puts itself off the jump point (nav 0: not a base, whose landing zone would land it) every 5 s, `off` metres from its middle: the pilots side by side again for the
// last pictures however far they flew apart while one slept)
const together = (off) => { let r = ""; for (let t = 150; t < 900; t += 5) r += `,${t}:!privgonav=0:${off}`; return r; };
async function open(browser, name, room, off) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 1000 } });
  const page = await ctx.newPage();
  const logs = [];
  page.on("console", (m) => {
    const t = m.text();
    logs.push([Date.now(), t]);
    if (/Privateer: (OPEN ROOM|In room|Back in|linked|.* is in the room|.* left the room|we decide|pilot \d+ decides|pilot \d+ has not said|pilot \d+ says who|back after|the room joined again|.*'s \w+ is in your space|pilot \d+'s .* given back)|reports error|Aborted|RuntimeError|PRCD.EXE (loaded|ended)|joined again|was away|lobby connection ended|session closed/.test(t)) console.log(`${stamp()} [${name}] ${t.slice(0, 170)}`);
  });
  page.on("pageerror", (e) => console.log(`${stamp()} ${name} PAGE ERROR: ${e.message}`));
  page.on("crash", () => console.log(`${stamp()} ${name} PAGE CRASHED`));
  const u = new URL(pageUrl);
  u.searchParams.set("server", lobbyUrl);
  u.searchParams.set("hall", "SLEEPHALL");
  u.searchParams.set("advertise", "off");
  u.searchParams.set("callsign", name);
  if (room) u.searchParams.set("room", room);
  // (the key script runs its items in order: sorted by time, as scripts/privrun.sh does)
  const items = (route + together(off)).split(",").filter(Boolean).map((it, i) => [parseFloat(it), i, it]);
  items.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  u.searchParams.set("env.WCNET_KEYSCRIPT", items.map((x) => x[2]).join(","));
  u.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
  u.searchParams.set("env.WCNET_LOG", "2");
  if (scenario === "reseat") u.searchParams.set("claimms", String(claimMs));
  await page.goto(u.toString());
  await page.setInputFiles("#gamefile", installer);
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
    || (document.getElementById("sourceStatus").dataset.state === "ready" && /^priv/.test(document.getElementById("program").value)), null, { timeout: 600000 });
  await page.evaluate(() => { document.getElementById("fullStart").checked = false; });
  return { page, ctx, logs, name };
}
// a line logged since `since` (ms since the epoch), waited for
const waitLog = async (p, re, ms, since = 0) => {
  const until = Date.now() + ms;
  while (Date.now() < until) {
    if (p.logs.some(([at, l]) => at >= since && re.test(l))) return true;
    await sleep(500);
  }
  return false;
};
const found = (p, re, since = 0) => ((p.logs.find(([at, l]) => at >= since && re.test(l)) || [0, ""])[1]).slice(0, 150);
const shot = async (p, file) => {
  const box = await p.page.$eval("#canvas", (c) => { const b = c.getBoundingClientRect(); return { x: b.x, y: b.y, width: b.width, height: b.height }; });
  await p.page.screenshot({ path: path.join(shotDir, file), clip: box });
};
const target = async (p, seat) => {
  for (let i = 0; i < 12; i++) {    // (T steps through every ship near: an encounter's pirates first, often)
    await p.page.focus("#canvas");
    await p.page.keyboard.press("t");
    await sleep(1500);
    const last = p.logs.filter(([, l]) => /our targeting system is on/.test(l)).pop() || [0, ""];
    if (last[1].includes(`pilot ${seat}'s ship`)) return true;
  }
  return false;
};
const pictures = async (a, aSeat, b, bSeat, name) => {
  check(`${a.name} targets ${b.name}'s ship`, await target(a, bSeat));
  check(`${b.name} targets ${a.name}'s ship`, await target(b, aSeat));
  await shot(a, `${name}-${a.name.toLowerCase()}.png`);
  await shot(b, `${name}-${b.name.toLowerCase()}.png`);
};
const fly = async (p) => {
  await p.page.waitForFunction(() => { const f = document.getElementById("fly"); return !!f && !f.disabled && f.offsetParent !== null; }, null, { timeout: 60000 });
  await p.page.click("#fly");
};

// ALPHA makes the room and flies; BRAVO joins by the code and flies
const alpha = await open(browserA, "ALPHA", "", 3000);
if (scenario === "reseat") {
  await alpha.page.evaluate(() => { const el = document.getElementById("seats"); el.value = "2"; el.dispatchEvent(new Event("input", { bubbles: true })); el.dispatchEvent(new Event("change", { bubbles: true })); });
}
await alpha.page.click("#join");
await alpha.page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
const code = await alpha.page.evaluate(() => document.getElementById("room").value);
console.log(`${stamp()} the room: ${code}`);
await fly(alpha);
const roomSeats = scenario === "reseat" ? 2 : 32;
check(`ALPHA: pilot 1 of an open room of ${roomSeats}`, await waitLog(alpha, new RegExp(`Privateer: OPEN ROOM at room \\S+, ${roomSeats} seats`), 120000), found(alpha, /Privateer: (OPEN ROOM|HOST|GUEST) at/));
check("ALPHA in space", await waitLog(alpha, /our presence sent \(a launch\)/, 180000), found(alpha, /presence sent \(a launch\)/));
const bravo = await open(browserB, "BRAVO", code, 3800);
await bravo.page.waitForFunction(() => !document.getElementById("lobby").hidden, null, { timeout: 60000 });
await fly(bravo);
check("BRAVO: pilot 2", await waitLog(bravo, new RegExp(`Privateer: In room \\S+ as pilot 2 of ${roomSeats}`), 120000), found(bravo, /as pilot/));
check("BRAVO sees ALPHA's ship", await waitLog(bravo, /Privateer: ALPHA's \w+ is in your space/, 240000), found(bravo, /is in your space/));
check("ALPHA sees BRAVO's ship", await waitLog(alpha, /Privateer: BRAVO's \w+ is in your space/, 60000), found(alpha, /is in your space/));
check("BRAVO takes ALPHA, the lowest seat, for the decider", await waitLog(bravo, /Privateer: pilot 1 decides the nav points/, 30000), found(bravo, /decides/));
await sleep(12000);
await pictures(alpha, 1, bravo, 2, "sleep-1-before");

// ALPHA's laptop closed: its Chrome stopped, every process of it
const pids = tree(alphaChrome[0]);
if (scenario === "reseat") {
  const slept = Date.now();
  console.log(`${stamp()} ALPHA's computer asleep for ${asleep} s (${pids.length} processes stopped)`);
  for (const pid of pids) { try { process.kill(pid, "SIGSTOP"); } catch (e) { /* gone */ } }
  // (ALPHA's last word to the server was up to 25 s before: its seat claimable CLAIM_MS after that at the latest)
  await sleep(claimMs + 30000);
  const charlie = await open(browserB, "CHARLIE", code, 3800);
  await charlie.page.waitForFunction(() => !document.getElementById("lobby").hidden, null, { timeout: 120000 });
  check("CHARLIE took ALPHA's silent seat in the full room", await waitLog(charlie, /room \S+ was full: seat 1, silent/, 5000), found(charlie, /was full|lobby:/));
  await fly(charlie);
  check("CHARLIE: pilot 1 (ALPHA's seat)", await waitLog(charlie, /Privateer: In room \S+ as pilot 1 of 2/, 120000), found(charlie, /as pilot/));
  await bravo.page.close({ runBeforeUnload: false });
  await bravo.ctx.close();
  console.log(`${stamp()} BRAVO left (seat 2 free)`);
  await sleep(Math.max(5000, asleep * 1000 - (Date.now() - slept)));
  for (const pid of pids) { try { process.kill(pid, "SIGCONT"); } catch (e) { /* gone */ } }
  const woke = Date.now();
  console.log(`${stamp()} ALPHA's computer awake`);
  check("ALPHA's page knows it was away", await waitLog(alpha, /the page was away \d+ s/, 20000, woke), found(alpha, /was away/, woke));
  check("ALPHA's game takes the room's new connection, in seat 2", await waitLog(alpha, /room \S+ joined again as player 1/, 90000, woke), found(alpha, /joined again/, woke));
  check("ALPHA's game takes its new seat", await waitLog(alpha, /Privateer: the room joined again as pilot 2 \(we were pilot 1\)/, 20000, woke), found(alpha, /the room joined again/, woke));
  check("ALPHA hears CHARLIE in seat 1", await waitLog(alpha, /Privateer: CHARLIE is in the room as pilot 1/, 60000, woke), found(alpha, /CHARLIE/, woke));
  check("CHARLIE hears ALPHA in seat 2", await waitLog(charlie, /Privateer: ALPHA is in the room as pilot 2/, 60000, woke), found(charlie, /ALPHA/, woke));
  check("ALPHA sees CHARLIE's ship", await waitLog(alpha, /Privateer: CHARLIE's \w+ is in your space/, 120000, woke), found(alpha, /is in your space/, woke));
  check("CHARLIE sees ALPHA's ship", await waitLog(charlie, /Privateer: ALPHA's \w+ is in your space/, 60000, woke), found(charlie, /is in your space/, woke));
  check("ALPHA takes CHARLIE, the lowest seat, for the decider", await waitLog(alpha, /Privateer: pilot 1 decides the nav points/, 30000, woke), found(alpha, /decides/, woke));
  check("ALPHA never back at the menu", !alpha.logs.some(([at, l]) => at >= slept && /session closed|PRCD\.EXE ended|The game left the room/.test(l)), found(alpha, /session closed|PRCD\.EXE ended|left the room/, slept));
  check("ALPHA and CHARLIE side by side off the jump point", await waitLog(alpha, /put in nav sphere 0/, 240000, woke) && await waitLog(charlie, /put in nav sphere 0/, 240000, woke));
  await sleep(5000);
  await pictures(alpha, 2, charlie, 1, "reseat-after");
  for (const p of [alpha, charlie]) {
    const errs = p.logs.filter(([, l]) => /reports error|PRCD\.EXE ended|RuntimeError|Aborted/.test(l)).map(([, l]) => l);
    check(`${p.name}: no error of the game's, still running`, errs.length === 0 && !p.page.isClosed(), errs.slice(0, 2).join(" | ").slice(0, 160));
  }
  fs.writeFileSync(path.join(shotDir, "alpha.log"), alpha.logs.map(([at, l]) => `${((at - t0) / 1000).toFixed(1)} ${l}`).join("\n"));
  fs.writeFileSync(path.join(shotDir, "charlie.log"), charlie.logs.map(([at, l]) => `${((at - t0) / 1000).toFixed(1)} ${l}`).join("\n"));
  console.log(failed ? `RESEAT FAILED (${failed})` : "RESEAT OK");
  await browserA.close();
  await browserB.close();
  process.exit(failed ? 1 : 0);
}
const slept = Date.now();
console.log(`${stamp()} ALPHA's computer asleep for ${asleep} s (${pids.length} processes stopped)`);
for (const pid of pids) { try { process.kill(pid, "SIGSTOP"); } catch (e) { /* gone */ } }
check("BRAVO passes the sleeping decider over", await waitLog(bravo, /Privateer: pilot 1 has not said who runs the nav points/, 30000, slept), found(bravo, /has not said/, slept));
check("BRAVO decides in its place", await waitLog(bravo, /Privateer: we decide the nav points' owners now/, 10000, slept), found(bravo, /we decide/, slept));
check("ALPHA's link fails at BRAVO: its ship given back", await waitLog(bravo, /Privateer: ALPHA \(pilot 1\) left the room/, Math.max(5000, asleep * 1000 - (Date.now() - slept) - 5000), slept), found(bravo, /left the room/, slept));
const leftAt = Date.now();
await sleep(Math.max(0, asleep * 1000 - (Date.now() - slept) - 4000));
await shot(bravo, "sleep-2-asleep-bravo.png");
await sleep(Math.max(0, asleep * 1000 - (Date.now() - slept)));
for (const pid of pids) { try { process.kill(pid, "SIGCONT"); } catch (e) { /* gone */ } }
const woke = Date.now();
console.log(`${stamp()} ALPHA's computer awake`);
check("ALPHA's page knows it was away", await waitLog(alpha, /the page was away \d+ s/, 20000, woke), found(alpha, /was away/, woke));
check("ALPHA's game knows it was away", await waitLog(alpha, /Privateer: back after \d+ s away/, 20000, woke), found(alpha, /back after/, woke));
check("ALPHA's game takes the room's new connection, the same seat", await waitLog(alpha, /room \S+ joined again as player 0/, 60000, woke), found(alpha, /joined again/, woke));
check("ALPHA linked with BRAVO again", await waitLog(alpha, /Privateer: (linked with pilot 2|BRAVO is in the room)/, 60000, woke), found(alpha, /linked|is in the room/, woke));
check("BRAVO hears ALPHA again", await waitLog(bravo, /Privateer: ALPHA is in the room as pilot 1/, 60000, woke), found(bravo, /ALPHA is in the room/, woke));
check("ALPHA sees BRAVO's ship again", await waitLog(alpha, /Privateer: BRAVO's \w+ is in your space/, 120000, woke), found(alpha, /is in your space/, woke));
check("BRAVO sees ALPHA's ship again", await waitLog(bravo, /Privateer: ALPHA's \w+ is in your space/, 60000, leftAt), found(bravo, /is in your space/, leftAt));
check("ALPHA decides again, BRAVO's table heard", await waitLog(alpha, /Privateer: we decide the nav points' owners now/, 30000, woke), found(alpha, /we decide/, woke));
check("BRAVO takes ALPHA for the decider again", await waitLog(bravo, /Privateer: pilot 1 decides the nav points/, 30000, woke), found(bravo, /pilot 1 decides/, woke));
const menu = await alpha.page.evaluate(() => document.getElementById("canvas").offsetParent !== null);
check("ALPHA never back at the menu: its game still on the page", menu && !alpha.logs.some(([at, l]) => at >= slept && /session closed|PRCD\.EXE ended|The game left the room/.test(l)), found(alpha, /session closed|PRCD\.EXE ended|left the room/, slept));
check("ALPHA and BRAVO side by side off the jump point", await waitLog(alpha, /put in nav sphere 0/, 240000, woke) && await waitLog(bravo, /put in nav sphere 0/, 240000, woke));
await sleep(5000);
await pictures(alpha, 1, bravo, 2, "sleep-3-after");
for (const p of [alpha, bravo]) {
  const errs = p.logs.filter(([, l]) => /reports error|PRCD\.EXE ended|RuntimeError|Aborted/.test(l)).map(([, l]) => l);
  check(`${p.name}: no error of the game's, still running`, errs.length === 0 && !p.page.isClosed(), errs.slice(0, 2).join(" | ").slice(0, 160));
}
fs.writeFileSync(path.join(shotDir, "alpha.log"), alpha.logs.map(([at, l]) => `${((at - t0) / 1000).toFixed(1)} ${l}`).join("\n"));
fs.writeFileSync(path.join(shotDir, "bravo.log"), bravo.logs.map(([at, l]) => `${((at - t0) / 1000).toFixed(1)} ${l}`).join("\n"));
console.log(failed ? `SLEEP FAILED (${failed})` : "SLEEP OK");
await browserA.close();
await browserB.close();
process.exit(failed ? 1 : 0);
