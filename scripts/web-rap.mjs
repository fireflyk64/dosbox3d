// Raptor in the browser, two pages in one room of a local lobby server (scripts/web-rap.sh): each page is given the
// shareware's zip as a player drops it (1rap12.zip: its installer and RAPSW12.SHR, unpacked by the page), both press
// Fly; each page's key script (env.WCNET_KEYSCRIPT, from boot) makes a new pilot, chooses the Bravo sector at the ship
// computer and flies.  Checks, each printed with OK or FAIL: the files recognised, the settings the page wrote, the
// program found by the hooks, the room, the launch together (the guest given the host's wave), the wave begun
// together, each page drawing the other's ship, damage told and taken; screenshots of both pictures every 20 s in
// SHOT_DIR.  Ends "RAP PAGE OK" when every check passed.
//
// PAD=1: the guest flies with a controller (a simulated standard gamepad picked in its Controller menu): in the wave
// the stick left, the stick right and the D-pad up must move its ship that way, and A held must fire (its damage told).
//
//   node web-rap.mjs PAGE_URL LOBBY_URL ROOM ZIP HOST_SCRIPT GUEST_SCRIPT [seconds]
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR, CYCLES, PAGE_ENV, PAD, DEMO (the page's "Free to play now" button, not ZIP)
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, room, zip, hostScript, guestScript, secondsArg = "130"] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl || !room || !zip || !hostScript || !guestScript) {
  console.error("usage: node web-rap.mjs PAGE_URL LOBBY_URL ROOM ZIP HOST_SCRIPT GUEST_SCRIPT [seconds]");
  process.exit(2);
}
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (ok, what) => { console.log(`${stamp()} ${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };

const PAD = !!process.env.PAD;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const mockPad = () => {
  window.__pad = { id: "Mock Pad (STANDARD GAMEPAD)", index: 0, connected: true, mapping: "standard", timestamp: 0,
                   axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  navigator.getGamepads = () => [window.__pad];
};
async function openPage(who, script, callsign, pad = false) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 1100 } });
  if (pad) await page.addInitScript(mockPad);
  const logs = [], frames = [];
  page.on("console", (m) => {
    const t = m.text();
    logs.push(t);
    const f = /Raptor: ([0-9.]+)s: (\d+) frames, in a wave/.exec(t);
    if (f && / run [1-9]/.test(t)) frames.push([Date.now(), Number(f[1]), Number(f[2])]);
    if (/Raptor: (HOST|GUEST|In |Room|.*joined|.*gone|Waiting|everybody|the host's wave|the wave begins|our wave|.*shot down|our ship|the program)|Raptor's settings|lobby: (joined|player)|could not|Aborted|RuntimeError|not a build/.test(t)) {
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
  for (const kv of (process.env.PAGE_ENV || "").split(/\s+/).filter(Boolean)) {
    const i = kv.indexOf("=");
    u.searchParams.set("env." + kv.slice(0, i), kv.slice(i + 1));
  }
  await page.goto(u.toString());
  // (DEMO=1: the page's own "Free to play now" button, the site's copy, not a dropped file)
  if (process.env.DEMO) await page.click("#demoRap"); else await page.setInputFiles("#gamefile", zip);
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
    || (document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "rap"), null, { timeout: 120000 })
    .catch(() => {});
  const st = await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 110)}`);
  check(st.startsWith("ready") && await page.$eval("#program", (el) => el.value) === "rap", `${who}: the zip recognised as Raptor (${st})`);
  if (pad) {
    const key = "0:Mock Pad (STANDARD GAMEPAD)";
    await page.waitForFunction((k) => Array.from(document.getElementById("padSelect").options).some((o) => o.value === k), key, { timeout: 20000 })
      .catch(() => {});
    await page.selectOption("#padSelect", key).catch(() => {});
    check(await page.inputValue("#padSelect") === key, `${who}: the controller picked in the Controller menu`);
  }
  return { page, logs, who, frames };
}
const all = (p, re) => p.logs.filter((l) => re.test(l));

const host = await openPage("host", hostScript, "HOSTPILOT");
await host.page.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 0, null, { timeout: 60000 });
await new Promise((r) => setTimeout(r, 2000));
const guest = await openPage("guest", guestScript, "GUESTPILOT", PAD);
await guest.page.waitForFunction(() => document.getElementById("roster").children.length > 1, null, { timeout: 60000 });
await host.page.waitForFunction(() => document.getElementById("roster").children.length > 1, null, { timeout: 60000 });
await host.page.screenshot({ path: path.join(shotDir, "web-host-room.png"), fullPage: true });
for (const p of [host, guest]) {
  await p.page.evaluate(() => { const b = document.getElementById("fullStart"); if (b && b.checked) b.click(); });
}
await host.page.click("#fly");
await new Promise((r) => setTimeout(r, 4000));
if (await guest.page.$eval("#fly", (b) => !b.disabled)) await guest.page.click("#fly");
// (the guest's controller in the wave: where its ship is, from the per-second report)
const padTest = PAD ? (async () => {
  const g = guest.page;
  const ship = () => {
    const l = all(guest, /Raptor: [0-9.]+s: .* run [1-9]/).pop();
    const m = l && /ship at (-?\d+) (-?\d+)/.exec(l);
    return m ? [Number(m[1]), Number(m[2])] : [NaN, NaN];
  };
  const axis = (i, v) => g.evaluate(([i, v]) => { window.__pad.axes[i] = v; }, [i, v]);
  const button = (i, v) => g.evaluate(([i, v]) => { window.__pad.buttons[i] = { pressed: v > 0.1, touched: v > 0, value: v }; }, [i, v]);
  for (let n = 0; n < 240 && !all(guest, /the wave begins together/).length; n++) await sleep(500);
  await sleep(5000);
  const a = ship();
  await axis(0, -1); await sleep(2500); await axis(0, 0); await sleep(1500);
  const b = ship();
  await axis(0, 1); await sleep(2500); await axis(0, 0); await sleep(1500);
  const c = ship();
  await button(12, 1); await sleep(2000); await button(12, 0); await sleep(1500);
  const d = ship();
  console.log(`${stamp()} guest's ship: ${a} -> stick left ${b} -> stick right ${c} -> D-pad up ${d}`);
  check(b[0] < a[0] - 30, "guest: the controller's stick to the left flew the ship left");
  check(c[0] > b[0] + 60, "guest: the stick to the right flew it right");
  check(d[1] < c[1] - 30, "guest: the D-pad up flew it up the screen");
  await button(0, 1);                                   // (A held from here: the guns)
  return Date.now();
})() : null;
const seconds = Number(secondsArg);
for (let t = 20; t <= seconds; t += 20) {
  await new Promise((r) => setTimeout(r, 20000));
  for (const p of [host, guest]) {
    await p.page.locator("#canvas").screenshot({ path: path.join(shotDir, `web-${p.who}-${t}.png`) }).catch(() => {});
  }
}
check(all(host, /Raptor's settings: (SETUP\.INI|the music|the effects)/).length > 0, "host: the settings written (SETUP.INI made, or its cards made the Sound Blaster)");
check(all(host, /Raptor: the program's code at/).length > 0, "host: the hooks found the program");
check(all(guest, /Raptor: the program's code at/).length > 0, "guest: the hooks found the program");
check(all(host, /joined as pilot 2/).length > 0, "host: the guest joined the game's room");
check(all(guest, /Raptor: the host's wave/).length > 0, "guest: given the host's wave at the launch");
check(all(host, /the wave begins together/).length > 0 && all(guest, /the wave begins together/).length > 0, "both: the wave begun together");
const sum = (p, re) => all(p, /Raptor: [0-9.]+s: .* run [1-9]/).reduce((s, l) => s + Number((re.exec(l) || [0, 0])[1]), 0);
const hd = sum(host, /ships drawn (\d+)/), gd = sum(guest, /ships drawn (\d+)/);
const told = sum(host, /damage told (\d+)/) + sum(guest, /damage told (\d+)/);
const taken = sum(host, /taken (\d+)/) + sum(guest, /taken (\d+)/);
console.log(`${stamp()} the other's ship drawn: host ${hd} frames, guest ${gd}; damage told ${told}, taken ${taken}`);
check(hd > 100 && gd > 100, "both pages drew the other's ship");
check(told > 0 && taken > 0, "damage told and taken");
if (padTest) {
  const firing = await padTest;
  const after = all(guest, /Raptor: [0-9.]+s: .* run [1-9]/).slice(-Math.max(1, Math.floor((Date.now() - firing) / 1000) - 1));
  const gt = after.reduce((s, l) => s + Number((/damage told (\d+)/.exec(l) || [0, 0])[1]), 0);
  console.log(`${stamp()} guest: damage told with A held: ${gt}`);
  check(gt > 0, "guest: the controller's A fired (the guest's damage told)");
}
for (const p of [host, guest]) {
  const f = p.frames.slice(3);
  if (f.length > 10) {
    const a = f[0], b = f[f.length - 1];
    const speed = (b[1] - a[1]) * 1000 / (b[0] - a[0]);
    const fps = f.reduce((s, x) => s + x[2], 0) / f.length;
    console.log(`${stamp()} ${p.who}: the emulator at ${Math.round(100 * speed)}% of real time over ${f.length} s, ${fps.toFixed(1)} frames a game second, ${(fps * speed).toFixed(1)} a wall second`);
  }
}
for (const p of [host, guest]) fs.writeFileSync(path.join(shotDir, `${p.who}.log`), p.logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `RAP PAGE FAILED (${failed})` : "RAP PAGE OK");
process.exit(failed ? 1 : 0);
