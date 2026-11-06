// RetroVega flown by three pilots in browser pages, as on the /dosbox page (scripts/web-smoke.sh rvtrio; the user,
// 2026-11-06: "Does 3 player work in exactly the same way that it does in WC1...where there are 2 guests and 1
// host? ... can we up the players to 3 and run a test where they do a mission 1 together"): a host makes a room of
// three seats, two wingmen join it by its code, and the host's Fly starts all three on mission 1 (ENIGMA1).  Checks:
// one host and two guests, the host's count of pilots, the same mission, every pilot's ship where it is in the other
// two games (a human's puppet on the host); then the host's autopilot (nav 2 picked first) takes all three to the
// asteroid field short of nav 2: they arrive together and each reads the field's rocks in.
//
//   node scripts/web-rv-trio.mjs PAGE_URL LOBBY_URL
//
// PAGE_URL must serve a page without wc.tar.gz beside it.  Playwright comes from PLAYWRIGHT_DIR; SHOT_DIR gets each
// page's screenshot, its log and its game's RVLOG.TXT.
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";
const [pageUrl, lobbyUrl] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl) {
  console.error("usage: node web-rv-trio.mjs PAGE_URL LOBBY_URL");
  process.exit(2);
}
const outDir = process.env.SHOT_DIR || "";
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const hallCode = "TRIO-" + Math.random().toString(36).slice(2, 6).toUpperCase();
const checks = [];
const check = (what, ok, detail = "") => { checks.push(ok); console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail ? ": " + detail : ""}`); };
const logs = {};
async function open(name, query) {
  const context = await browser.newContext();
  const page = await context.newPage();
  logs[name] = [];
  page.on("console", (m) => logs[name].push(m.text()));
  page.on("pageerror", (e) => logs[name].push("PAGE ERROR: " + e.message));
  const u = new URL(pageUrl);
  u.searchParams.set("server", lobbyUrl);
  u.searchParams.set("hall", hallCode);
  for (const [k, v] of Object.entries(query)) u.searchParams.set(k, v);
  await page.goto(u.toString());
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready", null, { timeout: 60000 });
  return page;
}
const rvlog = (page) => page.evaluate(() => { try { return new TextDecoder().decode(window.DOSBox.FS.readFile("/game/RVLOG.TXT")); } catch (e) { return ""; } });
const flying = (page) => page.waitForFunction(() => window.DOSBox && window.DOSBox._wc_web_in_flight && window.DOSBox._wc_web_in_flight() === 1, null, { timeout: 180000 }).then(() => true, () => false);
const rv = Object.fromEntries([...fs.readFileSync(new URL("../src/cpu/wcnet_rv.h", import.meta.url), "utf8")
  .matchAll(/#define (RV_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b/g)].map((m) => [m[1], Number(m[2])]));
const bytes = (page, off, n) => page.evaluate(([o, k]) => Array.from({ length: k }, (_, i) => window.DOSBox._wc_web_ds_byte(o + i)), [off, n]);
const word = (b, i) => b[i] | (b[i + 1] << 8);
const long = (b, i) => (b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24));
async function shared(page) {
  const slots = rv.RV_MAX_SHIP_SLOT + 1;
  const pos = await bytes(page, rv.RV_DS_gPositionVector, 12 * slots);
  const s = { role: (await bytes(page, rv.RV_DS_rvNetRole, 1))[0], node: (await bytes(page, rv.RV_DS_currentSeries, 1))[0],
    mission: (await bytes(page, rv.RV_DS_currentMission, 1))[0], count: (await bytes(page, rv.RV_DS_rvPlayerCount, 1))[0],
    ctl: await bytes(page, rv.RV_DS_rvController, slots), net: await bytes(page, rv.RV_DS_rvNetId, slots),
    flags: await bytes(page, rv.RV_DS_rvFlags, 2 * slots), pos: [] };
  for (let i = 0; i < slots; i++) s.pos.push([long(pos, 12 * i), long(pos, 12 * i + 4), long(pos, 12 * i + 8)]);
  return s;
}
const km = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]) / 256;
const cmd = "rv /noquit /fly 0 /verbose";
const host = await open("host", { callsign: "HOSTA", cmd, "env.WCNET_KEYSCRIPT": "0.5:s,1:s,40:n,41:a,46:f,50:f" });
await host.waitForFunction(() => window.__wcHall && window.__wcHall.inside(), null, { timeout: 30000 }).catch(() => {});
await host.fill("#seats", "3");                         // (three seats, set before the room is made)
await host.dispatchEvent("#seats", "change");
await host.click("#join");
await host.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
const room = await host.$eval("#room", (el) => el.value);
console.log("room", room);
const wings = [];
for (const [i, cs] of [[1, "WINGB"], [2, "WINGC"]]) {
  wings.push(await open("wing" + i, { room, callsign: cs, cmd, "env.WCNET_KEYSCRIPT": "0.5:s,1:s,46:f,50:f,60:f" }));
}
for (const w of wings)
  await w.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 2, null, { timeout: 30000 }).catch(() => {});
await host.waitForFunction(() => document.getElementById("roster").children.length > 2, null, { timeout: 30000 }).catch(() => {});
check("the room has three pilots", (await host.$$eval("#roster > *", (r) => r.length)) === 3, await host.$eval("#roster", (el) => el.textContent.trim().replace(/\s+/g, " ")));
check("the host's room has three seats", (await host.$eval("#seats", (el) => el.value)) === "3");
await host.click("#fly");
const fl = await Promise.all([host, ...wings].map(flying));
check("all three fly", fl.every(Boolean), fl.join(" "));
for (const p of [host, ...wings]) { await p.focus("#canvas"); }
await sleep(15000);
const [hs, w1, w2] = [await shared(host), await shared(wings[0]), await shared(wings[1])];
check("one host and two guests", hs.role === 1 && w1.role === 2 && w2.role === 2, `${hs.role} ${w1.role} ${w2.role}`);
check("the host counts three pilots", hs.count === 3, `rvPlayerCount ${hs.count}`);
check("all fly the host's mission (ENIGMA1: node 0, mission 0)", [hs, w1, w2].every((s) => s.node === 0 && s.mission === 0),
  [hs, w1, w2].map((s) => `${s.node}/${s.mission}`).join(" "));
for (const [n, ws] of [["the first wingman", w1], ["the second wingman", w2]]) {
  const me = ws.net[0];
  check(`the host has ${n}'s ship, a human's puppet, where it is`, me >= 1 && hs.ctl[me] === 2 && (hs.flags[2 * me] & 32) && km(hs.pos[me], ws.pos[0]) < 150,
    me >= 1 ? `slot ${me}, ${km(hs.pos[me], ws.pos[0]).toFixed(0)} km off` : `net id ${me}`);
  const body = ws.net.findIndex((x, i) => i > 0 && x === 0 && ws.ctl[i] === 2);
  check(`${n} has the host's ship where it is`, body > 0 && km(ws.pos[body], hs.pos[0]) < 150, body > 0 ? `${km(ws.pos[body], hs.pos[0]).toFixed(0)} km off` : "none");
}
{ // each wingman has the other one where it is
  const o1 = w1.net.findIndex((x, i) => i > 0 && x === w2.net[0] && w1.ctl[i] === 2), o2 = w2.net.findIndex((x, i) => i > 0 && x === w1.net[0] && w2.ctl[i] === 2);
  check("the first wingman has the second's ship where it is", o1 > 0 && km(w1.pos[o1], w2.pos[0]) < 150, o1 > 0 ? `${km(w1.pos[o1], w2.pos[0]).toFixed(0)} km off` : "none");
  check("the second wingman has the first's ship where it is", o2 > 0 && km(w2.pos[o2], w1.pos[0]) < 150, o2 > 0 ? `${km(w2.pos[o2], w1.pos[0]).toFixed(0)} km off` : "none");
}
// the autopilot to nav 2 and the field: the host's key script picks nav 2 at 40 s and goes; all three fly on
const t0 = Date.now();
let near = [false, false, false];
while (Date.now() - t0 < 150000 && !near.every(Boolean)) {
  await sleep(10000);
  near = await Promise.all([host, ...wings].map(async (p) => /rocks: near field/.test(await rvlog(p))));
  console.log(`${Math.round((Date.now() - t0) / 1000)} s: near the field ${near.join(" ")}`);
}
const logsNow = await Promise.all([host, ...wings].map(rvlog));
check("the host's autopilot went to nav 2", /autopilot: on to nav/.test(logsNow[0]));
check("all three read the field's rocks in", near.every(Boolean), near.join(" "));
const [hs2, v1, v2] = [await shared(host), await shared(wings[0]), await shared(wings[1])];
check("after the autopilot the three are together (within 3,000 km of the host)", km(v1.pos[0], hs2.pos[0]) < 3000 && km(v2.pos[0], hs2.pos[0]) < 3000,
  `${km(v1.pos[0], hs2.pos[0]).toFixed(0)} and ${km(v2.pos[0], hs2.pos[0]).toFixed(0)} km`);
check("no stop in any game", logsNow.every((t) => !/FAIL /.test(t)));
for (const [n, p] of [["host", host], ["wing1", wings[0]], ["wing2", wings[1]]]) {
  if (!outDir) break;
  fs.writeFileSync(path.join(outDir, `trio-${n}-RVLOG.TXT`), await rvlog(p));
  fs.writeFileSync(path.join(outDir, `trio-${n}.log`), logs[n].join("\n") + "\n");
  await p.screenshot({ path: path.join(outDir, `trio-${n}.png`) });
}
await browser.close();
const ok = checks.every(Boolean);
console.log(ok ? `TRIO OK (${checks.length} checks)` : `TRIO FAILED (${checks.filter((c) => !c).length} of ${checks.length})`);
process.exit(ok ? 0 : 1);
