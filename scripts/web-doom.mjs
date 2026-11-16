// DOOM in the browser, two to four pages in one room of a local lobby server (scripts/web-doom.sh): each page is given
// the shareware's zip as a player drops it (doom19s.zip: its pieces unpacked by the page), the host presses Fly and
// the guests' pages follow; each page's launcher (DOOMNET.COM) meets the others and runs DOOM as one co-operative
// network game.  Checks, each printed with OK or FAIL: the files recognised, the settings the page wrote, the game
// started for everybody with the right number of players, every page's game making its tics and seeing every other
// player's to the end (no page's game ended, no consistency failure); the pictures of every page every 20 s in
// SHOT_DIR, each page's speed.  Ends "DOOM PAGE OK" when every check passed.
//
//   node web-doom.mjs PAGE_URL LOBBY_URL ROOM ZIP HOST_SCRIPT GUEST_SCRIPT [seconds]
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR, CYCLES, PAGE_ENV, PLAYERS (2..4), MISSION (the host's pick, "4/2"),
//        DEMO (the page's "Free to play now" button, not ZIP), LEAVE_AT (seconds: the last guest's page closed then)
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, room, zip, hostScript, guestScript, secondsArg = "120"] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl || !room || !zip) {
  console.error("usage: node web-doom.mjs PAGE_URL LOBBY_URL ROOM ZIP HOST_SCRIPT GUEST_SCRIPT [seconds]");
  process.exit(2);
}
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const PLAYERS = Math.max(2, Math.min(4, Number(process.env.PLAYERS) || 2));
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (ok, what) => { console.log(`${stamp()} ${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function openPage(who, script, callsign, maker) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 1100 } });
  const logs = [], tics = [];
  page.on("console", (m) => {
    const t = m.text();
    logs.push(t);
    // "DOOM: 55.0s: tics 1841 (35.0 a second), seen p2 1841; packets ..."
    const f = /DOOM: ([0-9.]+)s: tics (\d+) \(([0-9.]+) a second\), seen((?: p\d \d+)+)/.exec(t);
    if (f) tics.push({ wall: Date.now(), at: Number(f[1]), tics: Number(f[2]), rate: Number(f[3]), seen: f[4].trim().split(/\s+/).filter((x) => /^\d+$/.test(x)).map(Number) });
    if (/DOOM: (HOST|GUEST|launcher|In the room|Nobody|Waiting|Looking|game |\d players|running|.* is ready|.* is in the room|player \d|.*ended|the network game|.*left the room|DOOM by itself)|DOOM's settings|screen: |lobby: (joined|player)|could not|Aborted|RuntimeError|not a build/.test(t)) {
      console.log(`${stamp()} [${who}] ${t.slice(0, 200)}`);
    }
  });
  page.on("pageerror", (e) => console.log(`${stamp()} [${who}] PAGE ERROR: ${e.message}`));
  page.on("crash", () => console.log(`${stamp()} [${who}] PAGE CRASHED`));
  const u = new URL(pageUrl);
  // (a room of more than two is made by its host with the Players box set first: a room link joins as the page loads)
  if (!maker || PLAYERS === 2) u.searchParams.set("room", room);
  u.searchParams.set("server", lobbyUrl);
  u.searchParams.set("callsign", callsign);
  if (script) {
    u.searchParams.set("env.WCNET_KEYSCRIPT", script);
    u.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
  }
  u.searchParams.set("env.WCNET_LOG", "2");
  if (process.env.CYCLES) u.searchParams.set("cycles", process.env.CYCLES);
  for (const kv of (process.env.PAGE_ENV || "").split(/\s+/).filter(Boolean)) {
    const i = kv.indexOf("=");
    u.searchParams.set("env." + kv.slice(0, i), kv.slice(i + 1));
  }
  await page.goto(u.toString());
  if (process.env.DEMO) await page.click("#demoDoom"); else await page.setInputFiles("#gamefile", zip);
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
    || (document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "doom"), null, { timeout: 120000 })
    .catch(() => {});
  const st = await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 110)}`);
  check(st.startsWith("ready") && await page.$eval("#program", (el) => el.value) === "doom", `${who}: the zip recognised as DOOM (${st})`);
  if (maker && PLAYERS > 2 && !process.env.DEMO) {
    await page.fill("#seats", String(PLAYERS));
    await page.fill("#room", room);
    await page.click("#join");
  }
  return { page, logs, who, tics };
}
const all = (p, re) => p.logs.filter((l) => re.test(l));

const host = await openPage("host", hostScript, "HOSTMARINE", true);
await host.page.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 0, null, { timeout: 60000 });
await sleep(2000);
const guests = [];
for (let g = 1; g < PLAYERS; g++) {
  guests.push(await openPage(g === 1 ? "guest" : `guest${g}`, guestScript, g === 1 ? "GUESTMARINE" : `MARINE${g + 1}`, false));
}
const everyone = [host, ...guests];
// (every player's callsign in the roster)
const inRoom = (n) => (document.getElementById("roster").innerText.match(/MARINE/g) || []).length >= n;
for (const p of everyone) await p.page.waitForFunction(inRoom, PLAYERS, { timeout: 60000 }).catch(() => {});
console.log(`${stamp()} the host's roster: ${(await host.page.$eval("#roster", (el) => el.innerText.replace(/\n+/g, " | "))).slice(0, 200)}`);
if (process.env.MISSION) {
  await host.page.selectOption("#mission", process.env.MISSION).catch((e) => console.log(`${stamp()} mission: ${e.message.split("\n")[0]}`));
  await sleep(1500);
}
console.log(`${stamp()} the host's mission menu: ${await host.page.$eval("#mission", (el) => el.selectedOptions.length ? el.selectedOptions[0].textContent : "(none)")}`);
await host.page.screenshot({ path: path.join(shotDir, "web-host-room.png"), fullPage: true });
for (const p of everyone) await p.page.evaluate(() => { const b = document.getElementById("fullStart"); if (b && b.checked) b.click(); });
await host.page.click("#fly");
await sleep(4000);
for (const g of guests) if (await g.page.$eval("#fly", (b) => !b.disabled)) await g.page.click("#fly");

const seconds = Number(secondsArg);
const leaveAt = Number(process.env.LEAVE_AT) || 0;
let left = null;
for (let t = 10; t <= seconds; t += 10) {
  await sleep(10000);
  if (t % 20 === 0 || t === 10) {
    for (const p of everyone) {
      if (p !== left) await p.page.locator("#canvas").screenshot({ path: path.join(shotDir, `web-${p.who}-${t}.png`) }).catch(() => {});
    }
  }
  if (leaveAt && !left && t >= leaveAt) {
    left = guests[guests.length - 1];
    console.log(`${stamp()} ${left.who}'s page closed`);
    await left.page.close();
  }
}
check(all(host, /DOOM's settings: DEFAULT\.CFG/).length > 0, "host: the settings written (DEFAULT.CFG)");
const started = all(host, new RegExp(`DOOM: game \\d+ started: ${PLAYERS} players`)).length > 0;
check(started, `host: the game started for ${PLAYERS} players`);
guests.forEach((g, i) => check(all(g, new RegExp(`DOOM: game \\d+: player ${i + 2} of ${PLAYERS}`)).length > 0, `${g.who}: in the host's game as player ${i + 2} of ${PLAYERS}`));
for (const p of everyone) {
  if (p === left) continue;
  const ended = all(p, /DOOM: .* ended|consistency failure|screen: /);
  check(ended.length === 0, `${p.who}: the game still running at the end${ended.length ? " (" + ended.slice(0, 3).join(" / ").slice(0, 160) + ")" : ""}`);
  const last = p.tics[p.tics.length - 1], mid = p.tics[Math.max(0, p.tics.length - 4)];
  const fresh = last && Date.now() - last.wall < 15000;
  const others = PLAYERS - 1;
  check(!!fresh && last.tics > 300 && last.tics > mid.tics, `${p.who}: making tics to the end (${last ? last.tics : 0})`);
  if (!left) {
    check(!!last && last.seen.length === others && last.seen.every((s) => Math.abs(s - last.tics) < 70), `${p.who}: every other player's tics seen, level with its own (${last ? last.seen.join(", ") : "none"})`);
  }
  const f = p.tics.slice(2);
  if (f.length > 3) {
    const a = f[0], b = f[f.length - 1];
    const speed = (b.at - a.at) * 1000 / (b.wall - a.wall), rate = (b.tics - a.tics) / (b.at - a.at);
    console.log(`${stamp()} ${p.who}: the emulator at ${Math.round(100 * speed)}% of real time over ${Math.round(b.at - a.at)} game seconds; ${rate.toFixed(1)} tics a game second, ${(rate * speed).toFixed(1)} a wall second (35 is DOOM's own)`);
    const perf = await p.page.evaluate(() => (window.DOSBox && window.DOSBox._wc_web_perf ? [0, 1, 3].map((i) => window.DOSBox._wc_web_perf(i)) : null)).catch(() => null);
    if (perf) console.log(`${stamp()} ${p.who}: the page's own figures: ${perf[0].toFixed(1)} frames a second, ${Math.round(100 * perf[1])}% of real time at ${perf[2]} cycles`);
    check(rate * speed > 28, `${p.who}: the game at more than 28 tics a wall second`);
  }
}
if (left) {
  for (const p of everyone) {
    if (p !== left) check(all(p, /is out of the game/).length > 0, `${p.who}: told that ${left.who} left`);
  }
}
for (const p of everyone) fs.writeFileSync(path.join(shotDir, `${p.who}.log`), p.logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `DOOM PAGE FAILED (${failed})` : "DOOM PAGE OK");
process.exit(failed ? 1 : 0);
