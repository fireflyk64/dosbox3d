// Wing Commander 1 at 640 x 400 in a room of two on slow browsers: two headless Chrome pages, each held to 1/THROTTLE of
// this machine's speed (Chrome's CPU throttling, default 2: half speed), host and wingman, both with the High resolution
// (2x) box ticked, fly a fight (MIS/SERIES, default Gimle 1, the first Raptor mission, with Angel: Salthi and Jalthi at
// once, the user's choice of a heavy one; Hell's Kitchen 1 is MIS=0 SERIES=13), each pilot put behind a foe now and
// then.  The frames each page's game makes
// in each second of the WALL clock are counted (the user's measure: the fewest in any second of 30 of fighting), with
// the picture at 640 x 400 (level 2) and the game's own (level 0) by turns, BLOCK seconds each (so both see the same
// fight); with the emulator's speed against real time and the wall time the picture's drawing took.  Pictures in $OUT.
// Prints "WC1 HIRES PERF OK" when no second at 640 x 400 fell more than SLACK frames (default 2) below the fewest of the
// game's own picture's seconds and the means are within 5%.
//
//   node scripts/web-wc1-hires-perf.mjs PAGE_URL LOBBY_URL     (scripts/web-smoke.sh wc1hiresperf)
import { createRequire } from "node:module";
import fs from "node:fs";
import path from "node:path";
const [pageUrl, lobbyUrl] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const OUT = process.env.OUT || ".";
const THROTTLE = Number(process.env.THROTTLE || 2);
const BLOCK = Number(process.env.BLOCK || 6), BLOCKS = Number(process.env.BLOCKS || 12);
const SLACK = Number(process.env.SLACK || 2);
const MIS = process.env.MIS || "0", SERIES = process.env.SERIES || "4";
// (each pilot behind a foe now and then; no guns of ours: the others' kills and the foes' fire make the fight's
//  explosions, and it lasts)
const FIGHT = process.env.FIGHT ||
  "12:!tail=foe:300,20:!tail=foe:300,28:!tail=foe:300,36:!tail=foe:300,44:!tail=foe:300,52:!tail=foe:300,60:!tail=foe:300," +
  "68:!tail=foe:300,76:!tail=foe:300,84:!tail=foe:300,92:!tail=foe:300,100:!tail=foe:300";
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const room = "PERF-" + Math.random().toString(36).slice(2, 8).toUpperCase();
const logs = {};

async function open(name, callsign) {
  const page = await (await browser.newContext()).newPage();
  logs[name] = [];
  page.on("console", (m) => logs[name].push(m.text()));
  page.on("pageerror", (e) => console.log(`[${name}] PAGE ERROR: ${e.message}`));
  const cdp = await page.context().newCDPSession(page);
  await cdp.send("Emulation.setCPUThrottlingRate", { rate: THROTTLE });
  const u = new URL(pageUrl);
  for (const [k, v] of Object.entries({ room, server: lobbyUrl, callsign, "env.MIS": MIS, "env.SERIES": SERIES,
                                        "env.WCNET_AUTOKEYS": "1", "env.WCNET_KEYSCRIPT": FIGHT })) u.searchParams.set(k, v);
  await page.addInitScript(() => { try { localStorage.setItem("wc:wcHires", "1"); } catch (e) { /* fresh */ } });
  await page.goto(u.toString());
  await page.waitForFunction(() => /^(ready|error)$/.test(document.getElementById("sourceStatus").dataset.state || ""), null, { timeout: 300000 });
  await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 120000 });
  await page.click("#fly");
  return page;
}

console.log(`room ${room}, each page at 1/${THROTTLE} speed, mission ${MIS} of series ${SERIES}, ${BLOCKS} blocks of ${BLOCK} s`);
const host = await open("host", "CHEAT1");
await host.waitForFunction(() => /Room .*: joining|Share this page|gives friends the way in|to play with friends/.test(document.getElementById("status").textContent), null, { timeout: 180000 });
await sleep(8000);
const wing = await open("wing", "CHEAT2");
const pages = { host, wing };
for (const [name, page] of Object.entries(pages)) {
  await page.waitForFunction(() => window.DOSBox && window.DOSBox._wc_web_in_flight && window.DOSBox._wc_web_in_flight() === 1, null, { timeout: 600000 });
  console.log(`[${name}] in flight`);
}
// (each page counts its own frames against its own clock, ten times a second, from inside the page)
for (const page of Object.values(pages)) {
  await page.evaluate(() => {
    const M = window.DOSBox;
    window.__wall = [];
    setInterval(() => window.__wall.push([performance.now(), M._wc_web_perf(6), M._wc_web_perf(7), M._wc_web_emulated_ms(), M._wc_web_wc_hires(-1)]), 100);
  });
}
// (the first foe: the key script's 12 s from the first frame in space)
await sleep(Number(process.env.SETTLE || 13) * 1000);

const setLevel = (level) => Promise.all(Object.values(pages).map((p) => p.evaluate((l) => window.DOSBox._wc_web_wc_hires(l), level)));
const now = () => Promise.all(Object.values(pages).map((p) => p.evaluate(() => performance.now())));
// Blocks of BLOCK seconds, 640 x 400 and the game's own by turns, so that both see the same fight: each block's whole
// seconds from half a second after its switch.
const windows = { 2: [], 0: [] };
for (let b = 0; b < BLOCKS; b++) {
  const level = b % 2 ? 0 : 2;
  await setLevel(level);
  const t = await now();
  windows[level].push(t);
  await sleep(BLOCK * 1000 + 300);
  if (b < 2) for (const [name, page] of Object.entries(pages)) await page.locator("canvas").first().screenshot({ path: path.join(OUT, `perf-${name}-${level ? "640x400" : "game"}-${b}.png`) });
}
await setLevel(2);
const per = {};
let pi = 0;
for (const [name, page] of Object.entries(pages)) {
  const s = await page.evaluate(() => window.__wall);
  const at = (t) => s.reduce((best, r) => Math.abs(r[0] - t) < Math.abs(best[0] - t) ? r : best, s[0]);
  per[name] = {};
  for (const level of [2, 0]) {
    const fps = [], cost = [], speed = [], levels = new Set();
    for (const starts of windows[level]) {
      const from = starts[pi] + 500;
      for (let k = 0; k + 1 <= BLOCK - 0.5; k++) {
        const a = at(from + k * 1000), b = at(from + (k + 1) * 1000), wall = b[0] - a[0] || 1;
        fps.push(Math.round((b[1] - a[1]) * 1000 / wall));
        cost.push((b[2] - a[2]) * 1000 / wall);
        speed.push((b[3] - a[3]) / wall);
        levels.add(a[4]);
        levels.add(b[4]);
      }
    }
    const r = { fps, min: Math.min(...fps), mean: fps.reduce((x, y) => x + y, 0) / fps.length,
                cost: cost.reduce((x, y) => x + y, 0) / cost.length, speed: Math.min(...speed), levels: [...levels] };
    per[name][level] = r;
    console.log(`[${name}] ${level ? "640 x 400" : "the game's own"} (level ${r.levels.join("/")}), ${fps.length} wall seconds: fewest frames ` +
                `in one ${r.min}, mean ${r.mean.toFixed(1)}, the emulator at ${(100 * r.speed).toFixed(0)}% of real time at worst, ` +
                `the 640 x 400 drawing ${r.cost.toFixed(1)} ms a second; each second: ${fps.join(" ")}`);
  }
  console.log(`[${name}] game ${(await page.evaluate(() => window.DOSBox._wc_web_perf(0))).toFixed(1)} fps, load ${(100 * await page.evaluate(() => window.DOSBox._wc_web_perf(5))).toFixed(0)}%`);
  pi++;
}
const results = [];
for (const name of Object.keys(pages)) {
  const hi = per[name][2], lo = per[name][0];
  const ok = hi.min >= lo.min - SLACK && hi.mean >= 0.95 * lo.mean && hi.levels.every((l) => l === 2) && lo.levels.every((l) => l === 0);
  results.push(ok);
  console.log(`${ok ? "ok  " : "FAIL"} ${name}: 640 x 400 fewest ${hi.min} (mean ${hi.mean.toFixed(1)}) against the game's own fewest ${lo.min} (mean ${lo.mean.toFixed(1)})`);
}
for (const [name, page] of Object.entries(pages)) {
  results.push((await page.evaluate(() => window.DOSBox._wc_web_in_flight())) === 1);
  fs.writeFileSync(path.join(OUT, `perf-${name}.log`), logs[name].join("\n") + "\n");
}
await browser.close();
const ok = results.every(Boolean);
console.log(ok ? "WC1 HIRES PERF OK" : "WC1 HIRES PERF FAILED");
process.exit(ok ? 0 : 1);
