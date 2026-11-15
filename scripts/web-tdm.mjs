// The TIE Fighter demo in the browser, two pages in one room of a local lobby server (scripts/web-tdm.sh): each page
// is given tiedemo.zip as a player drops it (the zips of its two floppies, unpacked by the page), both press Fly; each
// page's key script (env.WCNET_KEYSCRIPT, from boot) skips the opening and flies, both firing.  Checks, each printed
// with OK or FAIL: the zip installed, the program found by the hooks, the room, the launch together (the host's go
// with its seeds), the guest in its own craft of T/F Alpha, both flying the same craft (each page's reports of the
// craft it flies taken by the other), the shots made on the other page; the canvases every 10 s in SHOT_DIR.  Ends
// "TDM PAGE OK" when every check passed.
//
//   node web-tdm.mjs PAGE_URL LOBBY_URL ROOM ZIP|fetch HOST_SCRIPT GUEST_SCRIPT [seconds]   (fetch: the page's button)
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR, CYCLES, PAGE_ENV
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, room, zip, hostScript, guestScript, secondsArg = "60"] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl || !room || !zip || !hostScript || !guestScript) {
  console.error("usage: node web-tdm.mjs PAGE_URL LOBBY_URL ROOM ZIP HOST_SCRIPT GUEST_SCRIPT [seconds]");
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

async function openPage(who, script, callsign) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 1100 } });
  const logs = [];
  page.on("console", (m) => {
    const t = m.text();
    logs.push(t);
    if (/TIE demo: (HOST|GUEST|In |Room|.*joined|.*gone|Waiting|launch|the launch|the host's seeds|.*down|.*flight ends|the flight|TIEMISS|no craft|a call came back|the verdict)|TIE demo campaign|TIE Fighter demo's settings|lobby: (joined|player)|could not|Aborted|RuntimeError|not the TIE/.test(t)) {
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
  if (zip === "fetch") await page.click("#demoTfd");      // (the page's button: the Internet Archive's zip, fetched)
  else await page.setInputFiles("#gamefile", zip);
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
    || (document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "tiedemo"), null, { timeout: 120000 })
    .catch(() => {});
  const st = await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 110)}`);
  check(await page.$eval("#program", (el) => el.value) === "tiedemo", `${who}: the zip installed as the TIE Fighter demo (${st})`);
  return { page, logs, who };
}

const host = await openPage("host", hostScript, "HOSTPILOT");
await host.page.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 0, null, { timeout: 60000 });
await new Promise((r) => setTimeout(r, 2000));
const guest = await openPage("guest", guestScript, "GUESTPILOT");
await guest.page.waitForFunction(() => document.getElementById("roster").children.length > 1, null, { timeout: 60000 });
await host.page.waitForFunction(() => document.getElementById("roster").children.length > 1, null, { timeout: 60000 });
for (const p of [host, guest]) {
  await p.page.evaluate(() => { const b = document.getElementById("fullStart"); if (b && b.checked) b.click(); });
}
await host.page.click("#fly");
await new Promise((r) => setTimeout(r, 2000));
if (await guest.page.$eval("#fly", (b) => !b.disabled)) await guest.page.click("#fly");
const seconds = Number(secondsArg);
for (let t = 10; t <= seconds; t += 10) {
  await new Promise((r) => setTimeout(r, 10000));
  for (const p of [host, guest]) {
    await p.page.locator("#canvas").screenshot({ path: path.join(shotDir, `web-${p.who}-${t}.png`) }).catch(() => {});
  }
}
const all = (p, re) => p.logs.filter((l) => re.test(l));
check(all(host, /TIE demo: TIEMISS\.EXE loaded/).length > 0, "host: the hooks found TIEMISS.EXE");
check(all(guest, /TIE demo: TIEMISS\.EXE loaded/).length > 0, "guest: the hooks found TIEMISS.EXE");
check(all(host, /joined as pilot 2/).length > 0, "host: the guest joined the game's room");
check(all(host, /launch \d+: everybody there/).length > 0, "host: the launch together (everybody at the mission's start)");
check(all(guest, /the host's seeds .* we fly craft 1 of flight group 0/).length > 0, "guest: the host's seeds, its own craft of T/F Alpha (craft 1)");
check(all(guest, /the flight's frames are coming \(our object 0\)/).length > 0 && all(host, /the flight's frames are coming \(our object 1\)/).length > 0,
  "both: flying, the host object 1 and the guest object 0 on both");
const lines = (p) => all(p, /TIE demo: [0-9.]+s: frame \d+ as pilot/);
const sum = (p, re) => lines(p).reduce((s, l) => s + Number((re.exec(l) || [0, 0])[1]), 0);
const hc = sum(host, /sent (\d+) craft/), gc = sum(guest, /sent (\d+) craft/);
const hm = sum(host, /(\d+) made/), gm = sum(guest, /(\d+) made/);
const hOther = lines(host).filter((l) => / obj 0 of pilot 2: here/.test(l)).length, gOther = lines(guest).filter((l) => / obj 1 of pilot 1: here/.test(l)).length;
console.log(`${stamp()} craft reports sent: host ${hc}, guest ${gc}; the other's shots made: host ${hm}, guest ${gm}; the other pilot's craft seen: host ${hOther} s, guest ${gOther} s`);
check(hc > 100 && gc > 20 && hOther > 10 && gOther > 10, "both pages fly the other's craft from its reports");
check(hm > 0 && gm > 0, "each page made the other pilot's shots");
// the campaign (docs/tiedemo.md section 8): the demo's own mission first on both; a host's scripted win (!tdmwin)
// takes both pages to the next mission, the guest by the host's verdict and go
check(all(host, /campaign: mission 1 of 4, The Ambush/).length > 0 && all(guest, /campaign: mission 1 of 4, The Ambush/).length > 0,
  "both: the campaign's first mission, the demo's own");
if (/!tdmwin/.test(hostScript)) {
  check(all(host, /the verdict on launch \d+ sent: next mission 1/).length > 0, "host: won, its verdict sent (next: The Shuttles Run)");
  check(all(host, /campaign: mission 2 of 4, The Shuttles Run \(written over/).length > 0, "host: The Shuttles Run written and flown");
  check(all(guest, /campaign: mission 2 of 4, The Shuttles Run \(written over/).length > 0, "guest: The Shuttles Run flown on the host's go");
  check(all(guest, /we fly craft 1 of flight group 0/).length > 1, "guest: its own interceptor of Alpha again");
}
for (const p of [host, guest]) {
  const fr = lines(p).map((l) => Number((/\(([0-9.]+) ms a frame\)/.exec(l) || [0, 0])[1])).filter((x) => x > 0);
  if (fr.length) console.log(`${stamp()} ${p.who}: ${(1000 / (fr.reduce((s, x) => s + x, 0) / fr.length)).toFixed(1)} frames a second in flight (the game's clock)`);
}
for (const p of [host, guest]) fs.writeFileSync(path.join(shotDir, `${p.who}.log`), p.logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `TDM PAGE FAILED (${failed})` : "TDM PAGE OK");
process.exit(failed ? 1 : 0);
