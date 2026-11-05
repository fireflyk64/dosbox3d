// A wingman's page gone in the middle of a shared flight, in the browser build (the user, 2026-11-04: "if I'm the
// wingleader and the other player disconnects my CPU is slammed at 100% and the emulator can barely keep up"; and the
// guest's "lobby: player 0 lost its lobby connection; game link unaffected", again and again, until the host left).
// A host flies alone in a room of two, a wingman joins the mission in progress, both fly; then the wingman's page
// closes.  The host's game is measured before and after: frames a second (RetroVega's frameCounter), the emulator's
// share of real time and the page's own busy time (its perf lines), and what its lobby link says.
//
//   node scripts/web-rv-drop.mjs PAGE_URL LOBBY_URL [seconds after the drop]
//
// As scripts/web-smoke.sh rvdrop runs it (a local lobby server, web/dist without wc.tar.gz).  PLAYWRIGHT_DIR, CHROME,
// SHOT_DIR as scripts/web-rv.mjs has them.
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, afterArg = "40"] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl) {
  console.error("usage: node web-rv-drop.mjs PAGE_URL LOBBY_URL [seconds after the drop]");
  process.exit(2);
}
const after = Number(afterArg);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const chrome = process.env.CHROME || "/usr/bin/google-chrome";
const shotDir = process.env.SHOT_DIR || "";
const hallCode = "RVHALL-" + Math.random().toString(36).slice(2, 6).toUpperCase();
const browser = await chromium.launch({ headless: true, executablePath: chrome,
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const checks = [];
const check = (what, ok, detail = "") => { checks.push({ what, ok }); console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail ? ": " + detail : ""}`); };
const logs = {};

async function open(name, query) {
  const context = await browser.newContext();
  const page = await context.newPage();
  logs[name] = [];
  page.on("console", (m) => logs[name].push(`${(Date.now() / 1000).toFixed(1)} ${m.text()}`));
  page.on("pageerror", (e) => logs[name].push("PAGE ERROR: " + e.message));
  const u = new URL(pageUrl);
  u.searchParams.set("server", lobbyUrl);
  u.searchParams.set("hall", hallCode);
  for (const [k, v] of Object.entries(query)) u.searchParams.set(k, v);
  await page.goto(u.toString());
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready", null, { timeout: 60000 });
  return page;
}
const rv = Object.fromEntries([...fs.readFileSync(new URL("../src/cpu/wcnet_rv.h", import.meta.url), "utf8")
  .matchAll(/#define (RV_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b/g)].map((m) => [m[1], Number(m[2])]));
const frame = (page) => page.evaluate((o) => window.DOSBox._wc_web_ds_byte(o) | (window.DOSBox._wc_web_ds_byte(o + 1) << 8), rv.RV_DS_frameCounter);
const inLobby = (page) => page.waitForFunction(() => !document.getElementById("hallBody").hidden && !!window.__wcHall.test.net(), null, { timeout: 30000 }).then(() => true, () => false);
const flying = (page) => page.waitForFunction(() => window.DOSBox && window.DOSBox._wc_web_in_flight && window.DOSBox._wc_web_in_flight() === 1, null, { timeout: 150000 }).then(() => true, () => false);
// frames a second over `s` seconds (frameCounter is 16 bits)
async function fps(page, s) {
  const a = await frame(page), t0 = Date.now();
  await sleep(s * 1000);
  const b = await frame(page);
  return ((b - a + 65536) % 65536) / ((Date.now() - t0) / 1000);
}
const perfSince = (name, t) => logs[name].filter((l) => /perf: /.test(l) && Number(l.split(" ")[0]) >= t);
// the page's main thread busy, its share of real time over `s` seconds (Chrome's TaskDuration): a page that never
// lets go of the thread is 1.0
async function busy(cdp, s) {
  const get = async () => Object.fromEntries((await cdp.send("Performance.getMetrics")).metrics.map((m) => [m.name, m.value]));
  const a = await get(), t0 = Date.now();
  await sleep(s * 1000);
  const b = await get();
  return (b.TaskDuration - a.TaskDuration) / ((Date.now() - t0) / 1000);
}

const host = await open("host", { callsign: "CHEATHOST", cmd: "rv /noquit /fly 0" });
const cdp = await host.context().newCDPSession(host);
await cdp.send("Performance.enable");
const wing = await open("wing", { callsign: "WINGMAN", cmd: "rv /noquit /fly 3" });
check("both pilots are in the lobby", (await inLobby(host)) && (await inLobby(wing)));
await host.click("#join");
await host.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
const room = await host.$eval("#room", (el) => el.value);
await host.click("#fly");
check(`the host flies in room ${room}`, await flying(host));
await wing.fill("#room", room);
await wing.click("#join");
await wing.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 1, null, { timeout: 30000 });
await wing.click("#fly");
check("the wingman joins the mission in progress", await flying(wing));
for (const page of [host, wing]) { await page.focus("#canvas"); await page.keyboard.press("s"); }
await sleep(5000);
const busyBefore = await busy(cdp, 5);
const before = await fps(host, 10);
const tBefore = Date.now() / 1000;
await sleep(10000);
const perfBefore = perfSince("host", tBefore);
console.log(`the host before the drop: ${before.toFixed(1)} frames a second; ${perfBefore.slice(-1)[0] || "no perf line"}`);
const tDrop = Date.now() / 1000;
await wing.context().close();
console.log("the wingman's page is closed");
const busyWait = [];
for (let i = 0; i < 3; i++) busyWait.push(await busy(cdp, 5));   // (the wait for the link to be called gone)
const samples = [];
for (let i = 0; i < Math.max(1, Math.floor(after / 10)); i++) samples.push(await fps(host, 10));
const perfAfter = perfSince("host", tDrop);
console.log(`the host after the drop: ${samples.map((f) => f.toFixed(1)).join(", ")} frames a second`);
const busyAfter = await busy(cdp, 5);
console.log(`the host page's main thread busy: ${busyBefore.toFixed(2)} flying together, ${busyWait.map((b) => b.toFixed(2)).join(", ")} in the 15 s after the drop, ${busyAfter.toFixed(2)} after`);
for (const l of perfAfter.slice(-4)) console.log("  " + l);
const lost = logs.host.filter((l) => Number(l.split(" ")[0]) >= tDrop && /lobby|link lost|disconnected|signaling/.test(l));
for (const l of lost.slice(0, 20)) console.log("  host: " + l);
const worst = Math.min(...samples);
check("the host's game keeps its pace after the wingman's page is gone", worst > before * 0.85,
  `${worst.toFixed(1)} against ${before.toFixed(1)} frames a second`);
check("and its own lobby link stays", !lost.some((l) => /signaling lost|lobby session over/.test(l)));
check("its page does not spin while it waits for the gone pilot's link", Math.max(...busyWait) < Math.max(0.6, busyBefore * 1.5),
  `${busyWait.map((b) => b.toFixed(2)).join(", ")} against ${busyBefore.toFixed(2)} before`);
if (shotDir) {
  fs.writeFileSync(path.join(shotDir, "rvdrop-host.log"), logs.host.join("\n") + "\n");
  fs.writeFileSync(path.join(shotDir, "rvdrop-wing.log"), logs.wing.join("\n") + "\n");
}
await browser.close();
const failed = checks.filter((c) => !c.ok).length;
console.log(failed ? `RVDROP FAILED (${failed} of ${checks.length})` : `RVDROP OK (${checks.length} checks)`);
process.exit(failed ? 1 : 0);
