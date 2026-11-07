// RetroVega's difficulty on the page (scripts/web-smoke.sh rvdiff; the user, 2026-11-07: "For RetroVega remove the
// current options from the dropdown and just have easy medium hard", "it should be selected from either the menu or
// the web page ui"): a page with no files, RetroVega: its Difficulty menu in Collide's place; hard picked: the byte in
// this browser's kept RVSETUP.CFG; Fly: the game's file has it and the game's log says the flight's difficulty hard.
//
//   node scripts/web-rv-difficulty.mjs PAGE_URL LOBBY_URL
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";
const [pageUrl, lobbyUrl] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl) {
  console.error("usage: node web-rv-difficulty.mjs PAGE_URL LOBBY_URL");
  process.exit(2);
}
const outDir = process.env.SHOT_DIR || "";
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const checks = [];
const check = (what, ok, detail = "") => { checks.push(ok); console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail ? ": " + detail : ""}`); };
const logs = [];
const page = await (await browser.newContext()).newPage();
page.on("console", (m) => logs.push(m.text()));
page.on("pageerror", (e) => logs.push("PAGE ERROR: " + e.message));
const u = new URL(pageUrl);
u.searchParams.set("server", lobbyUrl);
u.searchParams.set("hall", "DIFF-" + Math.random().toString(36).slice(2, 6).toUpperCase());
u.searchParams.set("callsign", "DIFFA");
u.searchParams.set("cmd", "rv /noquit /fly 0 /verbose");
await page.goto(u.toString());
await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready", null, { timeout: 60000 });
await page.waitForFunction(() => window.__wcHall && window.__wcHall.inside(), null, { timeout: 30000 }).catch(() => {});
await page.click("#join");
await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
const shown = await page.evaluate(() => ({
  diff: !document.getElementById("difficultyLabel").hidden, collide: !document.getElementById("collisions").parentElement.hidden,
  value: document.getElementById("difficulty").value }));
check("RetroVega's room shows Difficulty, not Collide", shown.diff && !shown.collide, JSON.stringify(shown));
check("its difficulty is medium at first", shown.value === "medium", shown.value);
await page.selectOption("#difficulty", "hard");
const kept = await page.evaluate(() => {
  const r = JSON.parse(localStorage.getItem("wcsave:rv:RVSETUP.CFG") || "null");
  return r ? Array.from(atob(r.b64), (c) => c.charCodeAt(0)) : null;
});
check("hard is the kept RVSETUP.CFG's ninth byte, 2", !!kept && kept[8] === 2 && String.fromCharCode(...kept.slice(0, 6)) === "RVCFG1",
  JSON.stringify(kept));
await page.click("#fly");
const flying = await page.waitForFunction(() => window.DOSBox && window.DOSBox._wc_web_in_flight && window.DOSBox._wc_web_in_flight() === 1,
  null, { timeout: 180000 }).then(() => true, () => false);
check("the game flies", flying);
const game = await page.evaluate(() => {
  const r = (p) => { try { return window.DOSBox.FS.readFile(p); } catch (e) { return null; } };
  const cfg = r("/game/RVSETUP.CFG"), log = r("/game/RVLOG.TXT");
  return { cfg: cfg ? Array.from(cfg) : null, log: log ? new TextDecoder().decode(log) : "" };
});
check("the game's RVSETUP.CFG has it", !!game.cfg && game.cfg[8] === 2, JSON.stringify(game.cfg));
check("the game read it at the flight's start", /setup: the flight's difficulty hard/.test(game.log),
  (game.log.match(/^setup:.*$/m) || ["no setup line"])[0]);
check("no stop in the game", !/FAIL /.test(game.log));
if (outDir) {
  fs.writeFileSync(path.join(outDir, "rvdiff-RVLOG.TXT"), game.log);
  fs.writeFileSync(path.join(outDir, "rvdiff.log"), logs.join("\n") + "\n");
  await page.screenshot({ path: path.join(outDir, "rvdiff.png") });
}
await browser.close();
const ok = checks.every(Boolean);
console.log(ok ? `RVDIFF OK (${checks.length} checks)` : `RVDIFF FAILED (${checks.filter((c) => !c).length} of ${checks.length})`);
process.exit(ok ? 0 : 1);
