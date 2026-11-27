// An open Privateer room in browser pages (docs/privateer.md section 37; scripts/web-priv-open.sh serves the page and a
// local lobby; the user, 2026-11-27: "join a room code after it launches and leave ... later you can leave and come
// back ... the room limit is very high ... 32").  ALPHA hosts with the Players box as the page leaves it (32 for
// Privateer) and flies; BRAVO joins by the code once ALPHA is in space and flies at once (nobody waits in an open
// room); both see each other's ship; ALPHA's page is closed (its seat given back); CHARLIE joins after that -- seat 1,
// ALPHA's, free again -- and flies; BRAVO and CHARLIE see each other.  Pictures of both screens at each stage (T
// targets the nearest ship).  Prints "ok"/"FAIL" and "OPEN OK".
//
//   PLAYWRIGHT_DIR=... SHOT_DIR=... node web-priv-open.mjs PAGE_URL LOBBY_URL INSTALLER
import { createRequire } from "node:module";
import path from "node:path";

const [pageUrl, lobbyUrl, installer] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (what, ok, detail) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail === undefined ? "" : ": " + (typeof detail === "string" ? detail : JSON.stringify(detail))}`);
  if (!ok) failed++;
};
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
// a new pilot (scripts/privrun.sh's route), launched from Achilles at 16 s of its own; from 30 s, every 15 s, a turn
// toward the nearest ship (another pilot's, launched from the same pad) for the pictures
let route = "4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter,15:!mouse=0.45:0.6,16:!button=0:1,16.3:!button=0:0";
for (let t = 30; t < 600; t += 15) route += `,${t}:!privturn=ship:90:10`;
async function open(name, room) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 1000 } });
  const page = await ctx.newPage();
  const logs = [];
  page.on("console", (m) => {
    const t = m.text();
    logs.push(t);
    if (/Privateer: (OPEN ROOM|In room|linked|.* is in the room|.* left the room|we decide|pilot \d+ decides|.*'s \w+ is in your space)|reports error|Aborted|RuntimeError|PRCD.EXE (loaded|ended)/.test(t)) console.log(`${stamp()} [${name}] ${t.slice(0, 170)}`);
  });
  page.on("pageerror", (e) => console.log(`${stamp()} ${name} PAGE ERROR: ${e.message}`));
  page.on("crash", () => console.log(`${stamp()} ${name} PAGE CRASHED`));
  const u = new URL(pageUrl);
  u.searchParams.set("server", lobbyUrl);
  u.searchParams.set("hall", "OPENHALL");
  u.searchParams.set("advertise", "off");
  u.searchParams.set("callsign", name);
  if (room) u.searchParams.set("room", room);
  u.searchParams.set("env.WCNET_KEYSCRIPT", route);
  u.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
  u.searchParams.set("env.WCNET_LOG", "2");
  await page.goto(u.toString());
  await page.setInputFiles("#gamefile", installer);
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
    || (document.getElementById("sourceStatus").dataset.state === "ready" && /^priv/.test(document.getElementById("program").value)), null, { timeout: 600000 });
  await page.evaluate(() => { document.getElementById("fullStart").checked = false; });
  return { page, ctx, logs, name };
}
const waitLog = async (p, re, ms) => {
  const until = Date.now() + ms;
  while (Date.now() < until) {
    if (p.logs.some((l) => re.test(l))) return true;
    await sleep(500);
  }
  return false;
};
const found = (p, re) => (p.logs.find((l) => re.test(l)) || "").slice(0, 150);
const shot = async (p, file) => {
  const box = await p.page.$eval("#canvas", (c) => { const b = c.getBoundingClientRect(); return { x: b.x, y: b.y, width: b.width, height: b.height }; });
  await p.page.screenshot({ path: path.join(shotDir, file), clip: box });
};
// T until our targeting system is on the other pilot's ship (T steps through the ships near; the game logs the pilot
// targeted), then a picture of each screen at the same moment
const target = async (p, seat) => {
  for (let i = 0; i < 6; i++) {
    await p.page.focus("#canvas");
    await p.page.keyboard.press("t");
    await sleep(1500);
    const last = p.logs.filter((l) => /our targeting system is on/.test(l)).pop() || "";
    if (last.includes(`pilot ${seat}'s ship`)) return true;
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

// ALPHA: the room made with the page's own Players number
const alpha = await open("ALPHA", "");
const seats = await alpha.page.$eval("#seats", (el) => `${el.value} of ${el.max}`);
check("the Players box: 32 for Privateer, as many as a room takes", seats === "32 of 32", seats);
const beta = await alpha.page.$eval("#seatsBeta", (el) => el.hidden);
check("no beta tag on Privateer's 32", beta === true, String(beta));
await alpha.page.click("#join");
await alpha.page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
const code = await alpha.page.evaluate(() => document.getElementById("room").value);
console.log(`${stamp()} the room: ${code}`);
const roster = await alpha.page.$eval("#roster", (el) => el.textContent);
check("the roster: who is there and the seats free, not 32 lines", /31 seats free/.test(roster), roster.slice(0, 120));
await fly(alpha);
check("ALPHA: an open room of 32, pilot 1", await waitLog(alpha, /Privateer: OPEN ROOM at room \S+, 32 seats/, 120000), found(alpha, /Privateer: (OPEN ROOM|HOST|GUEST) at/));
check("ALPHA in space", await waitLog(alpha, /Privateer: at nav|our presence sent \(a launch\)/, 180000), found(alpha, /presence sent \(a launch\)/));

// BRAVO: in by the code while ALPHA flies, and flying at once
const bravo = await open("BRAVO", code);
await bravo.page.waitForFunction(() => !document.getElementById("lobby").hidden, null, { timeout: 60000 });
await fly(bravo);
check("BRAVO joined after the launch: pilot 2", await waitLog(bravo, /Privateer: In room \S+ as pilot 2 of 32/, 120000), found(bravo, /as pilot/));
check("BRAVO linked with ALPHA", await waitLog(bravo, /Privateer: (linked with pilot 1|ALPHA is in the room)/, 120000), found(bravo, /linked|is in the room/));
check("BRAVO sees ALPHA's ship", await waitLog(bravo, /Privateer: ALPHA's \w+ is in your space/, 240000), found(bravo, /is in your space/));
check("ALPHA sees BRAVO's ship", await waitLog(alpha, /Privateer: BRAVO's \w+ is in your space/, 60000), found(alpha, /is in your space/));
await sleep(12000);
await pictures(alpha, 1, bravo, 2, "open-1");

// ALPHA goes: its page closed (its seat given back at once)
await alpha.page.close({ runBeforeUnload: false });
await alpha.ctx.close();
check("BRAVO hears ALPHA leave", await waitLog(bravo, /Privateer: ALPHA \(pilot 1\) left the room/, 60000), found(bravo, /left the room/));
check("BRAVO decides the nav points now", await waitLog(bravo, /Privateer: we decide the nav points' owners now/, 30000), found(bravo, /decide/));
await sleep(4000);
check("BRAVO's game goes on", !bravo.logs.some((l) => /session closed|the room is closed/.test(l)), found(bravo, /session closed|room is closed/));

// CHARLIE: a visitor of the page (no link: it enters the public lobby -- here this test's own), after ALPHA left; told
// of the room in the lobby's chat as it comes in, by BRAVO, the lowest seat now -- whose page a link brought and which
// was not in the lobby: it went in to answer (the user: "the lowest player/ host if connected keeps answering open
// privateer games if it's public so people loading the page late after game start still see PRV-1212 open
// messages").  CHARLIE clicks the room's code there and is in, in the seat ALPHA gave back.
check("BRAVO, the room's lowest seat now, goes into the lobby to answer", await waitLog(bravo, /in the lobby to tell newcomers of our room/, 30000), found(bravo, /in the lobby to tell/));
const charlie = await open("CHARLIE", "");
const told = async () => {
  for (let i = 0; i < 60; i++) {
    const lines = await charlie.page.$eval("#hallLog", (el) => Array.from(el.children).map((c) => c.textContent));
    const line = lines.find((l) => l.includes(code));
    if (line) return line;
    await sleep(1000);
  }
  return "";
};
const toldLine = await told();
check("CHARLIE, loading the page late, is told of the room in the lobby by BRAVO", /BRAVO/.test(toldLine) && /seats? free/.test(toldLine), toldLine.slice(0, 120));
const clicked = await charlie.page.evaluate((c) => {
  const a = Array.from(document.querySelectorAll("#hallLog a, #hallLog [data-code], #hallLog .code")).find((e) => e.textContent.trim().toUpperCase() === c.toUpperCase());
  if (!a) return false;
  a.click();
  return true;
}, code);
check("CHARLIE clicks the room's code in the lobby's chat", clicked);
await charlie.page.waitForFunction(() => !document.getElementById("lobby").hidden, null, { timeout: 60000 });
const crew = await charlie.page.evaluate(() => { const el = document.getElementById("privCrew"); return `${el.value}${el.disabled ? " (greyed)" : ""}`; });
check("CHARLIE, in seat 1 of a room it did not make, cannot change its Ships choice", crew === "own (greyed)", crew);
await fly(charlie);
check("CHARLIE: pilot 1, ALPHA's seat", await waitLog(charlie, /Privateer: In room \S+ as pilot 1 of 32/, 120000), found(charlie, /as pilot/));
check("CHARLIE sees BRAVO's ship", await waitLog(charlie, /Privateer: BRAVO's \w+ is in your space/, 240000), found(charlie, /is in your space/));
check("BRAVO sees CHARLIE's ship", await waitLog(bravo, /Privateer: CHARLIE's \w+ is in your space/, 60000), found(bravo, /CHARLIE/));
check("CHARLIE, the lowest seat, decides once it has heard BRAVO", await waitLog(charlie, /Privateer: we decide the nav points' owners now/, 30000), found(charlie, /decide/));
await sleep(12000);
await pictures(charlie, 1, bravo, 2, "open-2");
for (const p of [bravo, charlie]) {
  const errs = p.logs.filter((l) => /reports error|PRCD\.EXE ended|RuntimeError|Aborted/.test(l));
  check(`${p.name}: no error of the game's, still running`, errs.length === 0 && !p.page.isClosed(), errs.slice(0, 2).join(" | ").slice(0, 160));
}
console.log(failed ? `OPEN FAILED (${failed})` : "OPEN OK");
await browser.close();
process.exit(failed ? 1 : 0);
