// DOOM alone in the browser (scripts/web-doom.sh with SOLO=1): one page, a room of one (Players 1), the shareware's
// zip dropped, Fly.  The launcher must run the game by itself (no -net): DOOM as it is, its title screen and menus.
// The page's keys then start a new game from the game's own menu (Enter: the menu, New Game, the episode, the skill),
// walk forward and fire; the pictures land in SHOT_DIR.  With MISSION ("4/2") the picked skill and level are on the
// game's command line and it starts in the level.  Ends "DOOM SOLO OK".
//
//   node web-doom-solo.mjs PAGE_URL LOBBY_URL ROOM ZIP
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, room, zip] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (ok, what) => { console.log(`${stamp()} ${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const page = await browser.newPage({ viewport: { width: 1280, height: 1100 } });
const logs = [];
page.on("console", (m) => {
  const t = m.text();
  logs.push(t);
  if (/DOOM: |DOOM's settings|screen: |could not|Aborted|RuntimeError/.test(t)) console.log(`${stamp()} ${t.slice(0, 200)}`);
});
page.on("pageerror", (e) => console.log(`${stamp()} PAGE ERROR: ${e.message}`));
const u = new URL(pageUrl);
u.searchParams.set("server", lobbyUrl);
u.searchParams.set("callsign", "LONEMARINE");
u.searchParams.set("env.WCNET_LOG", "2");
await page.goto(u.toString());
await page.setInputFiles("#gamefile", zip);
await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "doom", null, { timeout: 120000 }).catch(() => {});
check(await page.$eval("#program", (el) => el.value) === "doom", "the zip recognised as DOOM");
await page.fill("#seats", "1");
await page.fill("#room", room);
await page.click("#join");
await page.waitForFunction(() => !document.getElementById("lobby").hidden, null, { timeout: 30000 }).catch(() => {});
if (process.env.MISSION) {
  await page.selectOption("#mission", process.env.MISSION).catch((e) => console.log(`${stamp()} mission: ${e.message.split("\n")[0]}`));
}
console.log(`${stamp()} the mission menu: ${await page.$eval("#mission", (el) => (el.selectedOptions.length ? el.selectedOptions[0].textContent : "(none)"))}`);
await page.evaluate(() => { const b = document.getElementById("fullStart"); if (b && b.checked) b.click(); });
await page.click("#fly");
await sleep(22000);
await page.locator("#canvas").screenshot({ path: path.join(shotDir, "solo-title.png") });
const ran = logs.filter((l) => /DOOM: running DOOM\.EXE/.test(l));
check(ran.length === 1 && !/-net/.test(ran[0]), `the game run by itself (${ran[0] ? ran[0].replace(/^.*DOOM: /, "") : "not run"})`);
if (process.env.MISSION) check(/-skill \d -warp 1 \d/.test(ran[0] || ""), "the picked skill and level on its command line");
await page.locator("#canvas").click();
if (!process.env.MISSION) {
  // the game's own menu: Enter (the menu), Enter (New Game), Enter (Knee-Deep in the Dead), Enter (Hurt me plenty)
  for (let i = 0; i < 4; i++) { await page.keyboard.press("Enter", { delay: 120 }); await sleep(900); }
  await sleep(4000);
}
await page.locator("#canvas").screenshot({ path: path.join(shotDir, "solo-level.png") });
await page.keyboard.down("ArrowUp"); await sleep(1200); await page.keyboard.up("ArrowUp");
await page.keyboard.down("Control"); await sleep(500); await page.keyboard.up("Control");
await sleep(1500);
await page.locator("#canvas").screenshot({ path: path.join(shotDir, "solo-walked.png") });
const a = fs.readFileSync(path.join(shotDir, "solo-level.png")), b = fs.readFileSync(path.join(shotDir, "solo-walked.png"));
check(Buffer.compare(a, b) !== 0, "the page's keys walk the marine (the picture changed)");
check(!logs.some((l) => /DOOM: .* ended|screen: /.test(l)), "the game still running at the end");
fs.writeFileSync(path.join(shotDir, "solo.log"), logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `DOOM SOLO FAILED (${failed})` : "DOOM SOLO OK");
process.exit(failed ? 1 : 0);
