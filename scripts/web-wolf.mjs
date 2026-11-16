// Wolfenstein 3-D in the browser, two to four pages in one room of a local lobby server (scripts/web-wolf.sh): each
// page is given the game as a player drops it (the shareware's 1wolf14.zip, or GOG's installer of the full game), the
// host presses Fly and the guests' pages follow; every page's game starts straight in the host's floor.  Checks, each
// printed with OK or FAIL: the files recognised, the program found by the layer, every page's game in step with the
// room's level and seeing every other player to the end; the pictures of every page every 20 s in SHOT_DIR, each
// page's speed.  Ends "WOLF PAGE OK" when every check passed.
//
//   node web-wolf.mjs PAGE_URL LOBBY_URL ROOM ZIP HOST_SCRIPT GUEST_SCRIPT [seconds]
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR, CYCLES, PAGE_ENV, PLAYERS (2..4), MISSION (the host's pick, "2/3": the
//        skill's series and the floor), DEMO (the page's "Free to play now" button, not ZIP), LATE (seconds: with
//        PLAYERS=3, the third page comes into the room and presses Fly that long after the start: it must come into
//        the floor as it is), FALLEN (the fighters every page must have seen fall by the end)
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, room, zip, hostScript, guestScript, secondsArg = "90"] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl || !room || !zip) {
  console.error("usage: node web-wolf.mjs PAGE_URL LOBBY_URL ROOM ZIP HOST_SCRIPT GUEST_SCRIPT [seconds]");
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
  const logs = [], beats = [];
  const self = { page, logs, who, beats };
  page.on("console", (m) => {
    const t = m.text();
    logs.push(t);
    // "Wolfenstein 3-D: 25.0s: level 179 in step, 1 others playing; actors: 0 ours, 1 shown, 2 fallen; ..."
    const f = /Wolfenstein 3-D: ([0-9.]+)s: level (\d+) (in step|not in step), (\d+) others playing; actors: (\d+) ours, (\d+) shown, (\d+) fallen/.exec(t);
    if (f) beats.push({ wall: Date.now(), at: Number(f[1]), level: f[2], inStep: f[3] === "in step", others: Number(f[4]), ours: Number(f[5]), shown: Number(f[6]), fallen: Number(f[7]) });
    else if (/Wolfenstein 3-D|lobby: (joined|player)|could not|Aborted|RuntimeError|key script/.test(t)) console.log(`${stamp()} [${who}] ${t.slice(0, 230)}`);
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
  if (process.env.DEMO) await page.click("#demoWolf"); else await page.setInputFiles("#gamefile", zip);
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
    || (document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "wolf"), null, { timeout: 180000 })
    .catch(() => {});
  const st = await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 110)}`);
  check(st.startsWith("ready") && await page.$eval("#program", (el) => el.value) === "wolf", `${who}: the game recognised as Wolfenstein 3-D (${st})`);
  if (maker && PLAYERS > 2 && !process.env.DEMO) {
    await page.fill("#seats", String(PLAYERS));
    await page.fill("#room", room);
    await page.click("#join");
  }
  return self;
}
const all = (p, re) => p.logs.filter((l) => re.test(l));

const host = await openPage("host", hostScript, "HOSTBJ", true);
await host.page.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 0, null, { timeout: 60000 });
await sleep(2000);
const LATE = Number(process.env.LATE) || 0;
const guests = [];
let late = null;
for (let g = 1; g < PLAYERS - (LATE ? 1 : 0); g++) guests.push(await openPage(g === 1 ? "guest" : `guest${g}`, guestScript, g === 1 ? "GUESTBJ" : `BJ${g + 1}`, false));
const everyone = [host, ...guests];
const inRoom = (n) => (document.getElementById("roster").innerText.match(/BJ/g) || []).length >= n;
const IN_GAME = everyone.length;
for (const p of everyone) await p.page.waitForFunction(inRoom, IN_GAME, { timeout: 60000 }).catch(() => {});
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
for (let t = 10; t <= seconds; t += 10) {
  await sleep(10000);
  if (t % 20 === 0 || t === 10) {
    for (const p of late ? [...everyone, late] : everyone) await p.page.locator("#canvas").screenshot({ path: path.join(shotDir, `web-${p.who}-${t}.png`) }).catch(() => {});
  }
  if (LATE && !late && t >= LATE) {
    late = await openPage("late", process.env.LATE_KEYS || "", "LATEBJ", false);
    await late.page.waitForFunction(inRoom, IN_GAME + 1, { timeout: 60000 }).catch(() => {});
    await late.page.evaluate(() => { const b = document.getElementById("fullStart"); if (b && b.checked) b.click(); });
    const can = await late.page.$eval("#fly", (b) => !b.disabled);
    console.log(`${stamp()} late: in the room; its Fly button ${can ? "can be pressed" : "is shut"}`);
    if (can) await late.page.click("#fly");
  }
}
const players = late ? [...everyone, late] : everyone;
const wantFallen = Number(process.env.FALLEN) || 0;
for (const p of players) {
  check(all(p, /Wolfenstein 3-D: found after/).length > 0, `${p.who}: WOLF3D.EXE found by the layer`);
  const cmd = all(p, /tedlevel \d+ \w+ nowait/);
  check(cmd.length > 0, `${p.who}: started straight into a floor (${cmd.length ? /tedlevel \d+ \w+/.exec(cmd[0])[0] : "no TEDLEVEL start"})`);
  const ended = all(p, /WOLF3D\.EXE ended|not a build the hooks know|playing alone|Aborted|RuntimeError/);
  check(ended.length === 0, `${p.who}: the game still running with the room at the end${ended.length ? " (" + ended.slice(0, 3).join(" / ").slice(0, 160) + ")" : ""}`);
  const last = p.beats[p.beats.length - 1];
  const fresh = last && Date.now() - last.wall < 15000;
  check(!!fresh && last.inStep, `${p.who}: in step with the room's level to the end (level ${last ? last.level : "none"})`);
  check(!!last && last.others === players.length - 1, `${p.who}: every other player playing in its level (${last ? last.others : 0} of ${players.length - 1})`);
  if (wantFallen) check(!!last && last.fallen >= wantFallen, `${p.who}: ${last ? last.fallen : 0} fighters fallen in its level (${wantFallen} wanted)`);
  const f = p.beats.slice(1);
  if (f.length > 2) {
    const a = f[0], b = f[f.length - 1];
    const speed = (b.at - a.at) * 1000 / (b.wall - a.wall);
    const perf = await p.page.evaluate(() => (window.DOSBox && window.DOSBox._wc_web_perf ? [0, 1, 3].map((i) => window.DOSBox._wc_web_perf(i)) : null)).catch(() => null);
    console.log(`${stamp()} ${p.who}: the emulator at ${Math.round(100 * speed)}% of real time over ${Math.round(b.at - a.at)} game seconds`);
    check(speed > 0.85, `${p.who}: the emulator at more than 85% of real time`);
  }
}
const levels = new Set(players.map((p) => (p.beats.length ? p.beats[p.beats.length - 1].level : "none")));
check(levels.size === 1, `every page in the same level of the room (${[...levels].join(", ")})`);
for (const p of players) fs.writeFileSync(path.join(shotDir, `${p.who}.log`), p.logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `WOLF PAGE FAILED (${failed})` : "WOLF PAGE OK");
process.exit(failed ? 1 : 0);
