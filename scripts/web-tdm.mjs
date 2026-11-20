// The TIE Fighter demo in the browser, two pages in one room of a local lobby server (scripts/web-tdm.sh): each page
// is given tiedemo.zip as a player drops it (the zips of its two floppies, unpacked by the page), both press Fly; each
// page's key script (env.WCNET_KEYSCRIPT, from boot) skips the opening and flies, both firing.  Checks, each printed
// with OK or FAIL: the zip installed, the program found by the hooks, the room, the launch together (the host's go
// with its seeds), the guest in its own craft of T/F Alpha, both flying the same craft (each page's reports of the
// craft it flies taken by the other), the shots made on the other page; the canvases every 10 s in SHOT_DIR.  Ends
// "TDM PAGE OK" when every check passed.
//
//   node web-tdm.mjs PAGE_URL LOBBY_URL ROOM ZIP|fetch HOST_SCRIPT GUEST_SCRIPT [seconds]   (fetch: the page's button)
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR, CYCLES, PAGE_ENV, OVERRIDE_WC_JS (a wc.js served in place of the page's),
//        SLOW_SERVER_COPY (ms: the page's question for wc.tar.gz answered that late, a 404; "read": once the zip is read)
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
  if (process.env.OVERRIDE_WC_JS) {
    // (this tree's page script in place of the served one: a fix tried on the live site first)
    const js = fs.readFileSync(process.env.OVERRIDE_WC_JS);
    await page.route(/\/wc\.js(\?.*)?$/, (route) => route.fulfill({ status: 200, body: js, headers: { "content-type": "text/javascript" } }));
  }
  if (process.env.SLOW_SERVER_COPY) {
    // (the page's question for the server's copy of the game answered late, as the site can: the zip is dropped and
    // installed before it is answered; the page's own game must not then replace it)
    // ("read": answered the moment the dropped zip has been read, before its copy is kept: the window the site hit)
    await page.route(/\/wc\.tar\.gz(\?.*)?$/, async (route) => {
      if (process.env.SLOW_SERVER_COPY === "read") {
        for (let i = 0; i < 600 && !logs.some((l) => /^read \d+ files from/.test(l)); i++) await new Promise((r) => setTimeout(r, 5));
      } else await new Promise((r) => setTimeout(r, Number(process.env.SLOW_SERVER_COPY)));
      route.fulfill({ status: 404, body: "" });
    });
  }
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
  if (zip === "fetch") await page.selectOption("#program", "tiedemo");      // (picked in the Game menu: the Internet Archive's zip, fetched)
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
// (the page's keepalive line to the lobby server, every 25 s, is answered "unknown message type" by a server from
// before it: neither page's console says so, the user: "I get a lot of this in my console ... it's really distracting")
const noise = all(host, /unknown message type "ping"/).length + all(guest, /unknown message type "ping"/).length;
check(noise === 0, `no line of the lobby server's answers to the keepalive in either console (${noise})`);
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
// HOST_LEAVES=<seconds>: the host's page then closed and the guest watched that long (the user, 2026-11-19: "the host
// closed the browser and the guest keeps getting these intermittent waits for a host that is never coming back"): the
// guest is told, never waits for the host again (the emulator stood still 20 s at each wait), and its game goes on,
// by the seconds of its own clock its key script's !tdmpose lines show (GUEST_SCRIPT: one a second).
if (process.env.HOST_LEAVES) {
  const n = Number(process.env.HOST_LEAVES);
  const poses = () => new Set(guest.logs.map((l) => (/key script ([0-9.]+)s: TIE demo: pose/.exec(l) || [])[1]).filter(Boolean)).size;
  const mark = guest.logs.length, p0 = poses();
  await host.page.close();
  await new Promise((r) => setTimeout(r, n * 1000));
  const after = guest.logs.slice(mark);
  const waits = after.filter((l) => /waiting for a connection to player 0|no connection to player 0/.test(l)).length;
  const flown = poses() - p0;
  check(after.some((l) => /the host\) is gone/.test(l)), "guest: told the host is gone");
  check(waits === 0, `guest: no wait for the host that left (${waits} lines of waiting)`);
  check(flown >= 0.7 * n, `guest: its game went on, ${flown} of its own seconds in ${n} s`);
}
for (const p of [host, guest]) fs.writeFileSync(path.join(shotDir, `${p.who}.log`), p.logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `TDM PAGE FAILED (${failed})` : "TDM PAGE OK");
process.exit(failed ? 1 : 0);
