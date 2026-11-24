// X-Wing Collector's CD flown by two browser pages in a room of a lobby server (docs/xwing.md): each page is given the
// game as a player drops it (the CD's image, or a zip), both press Fly; each page's key script flies its own front end
// from boot to Tour of Duty I's first mission (scripts/xwroute-tod1.txt, then the extra script).  Checked: the hooks
// found BWING.EXE, the guest joined, the launch together, the guest's own craft of Red, both flying each other's
// craft from its reports, each page making the other's shots.  The canvases every 10 s in SHOT_DIR.  Ends "XW PAGE OK".
//
//   node scripts/web-xw.mjs PAGE_URL LOBBY_URL ROOM ISO|ZIP HOST_EXTRA GUEST_EXTRA [seconds]
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR, PAGE_ENV, CYCLES
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, room, zip, hostExtra, guestExtra, secondsArg = "200"] = process.argv.slice(2);
const route = fs.readFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)), "xwroute-tod1.txt"), "utf8").trim();
const hostScript = route + (hostExtra ? "," + hostExtra : ""), guestScript = route + (guestExtra ? "," + guestExtra : "");
if (!pageUrl || !lobbyUrl || !room || !zip) {
  console.error("usage: node web-xw.mjs PAGE_URL LOBBY_URL ROOM ISO|ZIP HOST_EXTRA GUEST_EXTRA [seconds]");
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
    if (/X-Wing: (HOST|GUEST|In |Room|.*joined|.*gone|Waiting|launch|the launch|the host's seed|.*down|.*flight ends|the flight|BWING|flight group|we fly|a call came back|the mission made)|X-Wing's settings|lobby:|could not|Aborted|RuntimeError/.test(t) && !/key script/.test(t)) {
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
  await page.setInputFiles("#gamefile", zip);
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
    || (document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "xwing"), null, { timeout: 180000 })
    .catch(() => {});
  const st = await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 110)}`);
  check(await page.$eval("#program", (el) => el.value) === "xwing", `${who}: the game installed as X-Wing (${st})`);
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
check(all(host, /X-Wing: BWING\.EXE loaded/).length > 0, "host: the hooks found BWING.EXE");
check(all(guest, /X-Wing: BWING\.EXE loaded/).length > 0, "guest: the hooks found BWING.EXE");
check(all(host, /joined as pilot 2/).length > 0, "host: the guest joined the game's room");
check(all(host, /launch \d+: everybody there/).length > 0, "host: the launch together (everybody at the mission's start)");
check(all(guest, /the host's seed .* we fly craft 1 of flight group 0/).length > 0, "guest: the host's seed, its own craft of Red (craft 1)");
check(all(guest, /the flight's frames are coming \(our object 0/).length > 0 && all(host, /the flight's frames are coming \(our object 1/).length > 0,
  "both: flying, the host object 1 and the guest object 0");
const lines = (p) => all(p, /X-Wing: [0-9.]+s: frame \d+ as pilot/);
const sum = (p, re) => lines(p).reduce((s, l) => s + Number((re.exec(l) || [0, 0])[1]), 0);
const hc = sum(host, /sent (\d+) craft/), gc = sum(guest, /sent (\d+) craft/);
const hm = sum(host, /(\d+) made/), gm = sum(guest, /(\d+) made/);
const hOther = lines(host).filter((l) => / obj 0 of pilot 2: here/.test(l)).length, gOther = lines(guest).filter((l) => / obj 1 of pilot 1: here/.test(l)).length;
const rate = (p) => { const m = lines(p).map((l) => Number((/\(([0-9.]+) ms a frame\)/.exec(l) || [0, 0])[1])).filter(Boolean); return m.length ? m.sort((a, b) => a - b)[m.length >> 1] : 0; };
console.log(`${stamp()} craft reports sent: host ${hc}, guest ${gc}; the other's shots made: host ${hm}, guest ${gm}; the other pilot's craft seen: host ${hOther} s, guest ${gOther} s; a frame ${rate(host)} / ${rate(guest)} ms`);
check(hc > 100 && gc > 20 && hOther > 10 && gOther > 10, "both pages fly the other's craft from its reports");
check(hm > 0 && gm > 0, "each page made the other pilot's shots");
for (const p of [host, guest]) fs.writeFileSync(path.join(shotDir, `web-${p.who}.log`), p.logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `XW PAGE FAILED (${failed})` : "XW PAGE OK");
process.exit(failed ? 1 : 0);
