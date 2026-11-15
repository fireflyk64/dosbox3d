// Terminal Velocity in the browser, two pages in one room of a local lobby server (scripts/web-tv.sh): each page is
// given the shareware's zip as a player drops it (3dtv12.zip: its split archive unpacked by the page), the host picks
// the craft in the room's menus, both press Fly; each page's key script (env.WCNET_KEYSCRIPT, from boot) routes its
// game into the first level and flies it.  Checks, each printed with OK or FAIL: the files recognised, the room, the
// craft menus shown to the host only, the settings the page wrote (the log), the program found by the hooks, the
// guest held then flying the host's level, each page drawing the other's craft, hits shared; screenshots of both
// pictures every 20 s in SHOT_DIR.  Ends "TV PAGE OK" when every check passed.
//
//   node web-tv.mjs PAGE_URL LOBBY_URL ROOM ZIP HOST_SCRIPT GUEST_SCRIPT [seconds]
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR, HOST_CRAFT / GUEST_CRAFT (the room's menus: icebrd.bin, snowftr.bin ...), DEMO (the
//   page's "Free to play now" button, not ZIP)
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, room, zip, hostScript, guestScript, secondsArg = "130"] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl || !room || !zip || !hostScript || !guestScript) {
  console.error("usage: node web-tv.mjs PAGE_URL LOBBY_URL ROOM ZIP HOST_SCRIPT GUEST_SCRIPT [seconds]");
  process.exit(2);
}
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const hostCraft = process.env.HOST_CRAFT || "icebrd.bin", guestCraft = process.env.GUEST_CRAFT || "snowftr.bin";
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (ok, what) => { console.log(`${stamp()} ${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };

async function openPage(who, script, callsign) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 1100 } });
  const logs = [], frames = [];
  page.on("console", (m) => {
    const t = m.text();
    logs.push(t);
    // (the game's own second-by-second line: the emulated second and its frames, against the wall clock)
    const f = /Terminal Velocity: ([0-9.]+)s: (\d+) frames, flying/.exec(t);
    if (f) frames.push([Date.now(), Number(f[1]), Number(f[2])]);
    if (/Terminal Velocity: (HOST|GUEST|In |Room|.*joined|.*gone|Waiting|Flying|the program|the model|level \d+ \(|.*flew the exit)|Terminal Velocity's settings|lobby: (joined|player)|could not|Aborted|RuntimeError|not a build/.test(t)) {
      console.log(`${stamp()} [${who}] ${t.slice(0, 190)}`);
    }
  });
  page.on("pageerror", (e) => console.log(`${stamp()} [${who}] PAGE ERROR: ${e.message}`));
  page.on("crash", () => console.log(`${stamp()} [${who}] PAGE CRASHED`));
  const u = new URL(pageUrl);
  u.searchParams.set("room", room);
  u.searchParams.set("server", lobbyUrl);
  u.searchParams.set("callsign", callsign);
  u.searchParams.set("env.WCNET_KEYSCRIPT", script);
  u.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
  u.searchParams.set("env.WCNET_LOG", "2");
  if (process.env.CYCLES) u.searchParams.set("cycles", process.env.CYCLES);
  // (PAGE_ENV="A=1 B=2": more of the emulator's environment on both pages, e.g. WCNET_TV_SPANS=0 for the game's own drawing)
  for (const kv of (process.env.PAGE_ENV || "").split(/\s+/).filter(Boolean)) {
    const i = kv.indexOf("=");
    u.searchParams.set("env." + kv.slice(0, i), kv.slice(i + 1));
  }
  await page.goto(u.toString());
  // (DEMO=1: the page's own "Free to play now" button, the site's copy, not a dropped file)
  // (GUEST_ZIP: the guest's own file, the shareware beside the host's registered game)
  if (process.env.DEMO) await page.click("#demoTv");
  else await page.setInputFiles("#gamefile", who === "guest" && process.env.GUEST_ZIP ? process.env.GUEST_ZIP : zip);
  // (ready for the dropped files: the page's built-in game is ready before them on a page that has one)
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
    || (document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "tv"), null, { timeout: 400000 })
    .catch(() => {});
  const st = await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 110)}`);
  check(st.startsWith("ready") && await page.$eval("#program", (el) => el.value) === "tv", `${who}: the zip recognised as Terminal Velocity (${st})`);
  return { page, logs, who, frames };
}

const host = await openPage("host", hostScript, "HOSTPILOT");
await host.page.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 0, null, { timeout: 60000 });
await new Promise((r) => setTimeout(r, 2000));
const guest = await openPage("guest", guestScript, "GUESTPILOT");
await guest.page.waitForFunction(() => document.getElementById("roster").children.length > 1, null, { timeout: 60000 });
await host.page.waitForFunction(() => document.getElementById("roster").children.length > 1, null, { timeout: 60000 });
// the room's craft menus: the host's to choose, shown to the guest disabled
const menus = async (p) => p.evaluate(() => ({ shown: !document.getElementById("tvCraftLabel").hidden,
  disabled: document.getElementById("tvCraftHost").disabled }));
const hm = await menus(host.page), gm = await menus(guest.page);
check(hm.shown && !hm.disabled, `host: the craft menus shown and open (${JSON.stringify(hm)})`);
check(gm.shown && gm.disabled, `guest: the craft menus shown, the host's to choose (${JSON.stringify(gm)})`);
await host.page.selectOption("#tvCraftHost", hostCraft);
await host.page.selectOption("#tvCraftGuest", guestCraft);
await host.page.screenshot({ path: path.join(shotDir, "web-host-room.png"), fullPage: true });
for (const p of [host, guest]) {
  await p.page.evaluate(() => { const b = document.getElementById("fullStart"); if (b && b.checked) b.click(); });
}
await host.page.click("#fly");
// (the host's Fly starts everybody's game in a room; a guest's own Fly only if it did not)
await new Promise((r) => setTimeout(r, 4000));
if (await guest.page.$eval("#fly", (b) => !b.disabled)) await guest.page.click("#fly");
const seconds = Number(secondsArg);
for (let t = 20; t <= seconds; t += 20) {
  await new Promise((r) => setTimeout(r, 20000));
  for (const p of [host, guest]) {
    await p.page.locator("#canvas").screenshot({ path: path.join(shotDir, `web-${p.who}-${t}.png`) }).catch(() => {});
  }
}
const all = (p, re) => p.logs.filter((l) => re.test(l));
check(all(host, /Terminal Velocity's settings: .*VOX\.INI: 640 x 480/).length > 0, "host: the settings written (640 x 480)");
check(all(host, /Terminal Velocity: the program's code at/).length > 0, "host: the hooks found the program");
check(all(guest, /Terminal Velocity: the program's code at/).length > 0, "guest: the hooks found the program");
check(all(host, /Terminal Velocity: GUEST(PILOT)? joined|joined as pilot 2/).length > 0, "host: the guest joined the game's room");
check(all(guest, /Flying .* with /).length > 0, "guest: flew the host's level");
check(all(guest, /the model icebrd\.bin loaded/).length > 0 || hostCraft !== "icebrd.bin", "guest: the host's craft (the IceRider) loaded");
check(all(host, /the model snowftr\.bin loaded/).length > 0 || guestCraft !== "snowftr.bin", "host: the guest's craft (the SnowScout) loaded");
const lastSeat = (p) => { const l = all(p, /seat \d .*hits sent/); return l.length ? l[l.length - 1] : ""; };
const hs = lastSeat(host), gs = lastSeat(guest);
console.log(`${stamp()} host's last report: ${hs.slice(0, 260)}`);
console.log(`${stamp()} guest's last report: ${gs.slice(0, 260)}`);
check(/drawn [1-9]/.test(hs) && /drawn [1-9]/.test(gs), "both pages drew the other's ship");
check(/hits sent [1-9]/.test(hs) || /hits sent [1-9]/.test(gs), "hits shared");
// how fast each page ran: emulated seconds against the wall clock while flying, and the game's frames a second
for (const p of [host, guest]) {
  const f = p.frames.slice(5);
  if (f.length > 10) {
    const a = f[0], b = f[f.length - 1];
    const speed = (b[1] - a[1]) * 1000 / (b[0] - a[0]);
    const fps = f.reduce((s, x) => s + x[2], 0) / f.length;
    console.log(`${stamp()} ${p.who}: the emulator at ${Math.round(100 * speed)}% of real time over ${f.length} s, ${fps.toFixed(1)} frames a game second, ${(fps * speed).toFixed(1)} a wall second`);
  }
}
for (const p of [host, guest]) fs.writeFileSync(path.join(shotDir, `${p.who}.log`), p.logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `TV PAGE FAILED (${failed})` : "TV PAGE OK");
process.exit(failed ? 1 : 0);
