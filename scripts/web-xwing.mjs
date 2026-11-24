// X-Wing Collector's CD in the browser, alone (docs/xwing.md): one Chrome page; the game dropped as a player has it
// (the CD's .iso by itself, or a zip of the installed folder with the CD's files or image in it).  The page's
// unpacking checked (BWING.EXE found, the Play menu's "xwing", the settings written, the CD mounted), a room of one
// flown by the route's keys and clicks from boot (scripts/xwroute-tod1.txt: a new pilot, Tour of Duty I's first
// mission); the canvas every 10 s in SHOT_DIR.  Ends "XWING PAGE OK".
//
//   node scripts/web-xwing.mjs PAGE_URL ISO|ZIP [seconds] [extra key script]
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR, PAGE_ENV
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, game, secondsArg = "150", extra] = process.argv.slice(2);
const here = path.dirname(fileURLToPath(import.meta.url));
const script = fs.readFileSync(path.join(here, "xwroute-tod1.txt"), "utf8").trim() + (extra ? "," + extra : "");
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (ok, what) => { console.log(`${stamp()} ${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };
const context = await browser.newContext({ viewport: { width: 1280, height: 1100 } });
const page = await context.newPage();
const logs = [];
page.on("console", (m) => {
  const t = m.text();
  logs.push(t);
  if (/X-Wing|xwing|BWING|settings|could not|Aborted|RuntimeError|error|CD/i.test(t) && !/key script/.test(t)) console.log(`${stamp()} ${t.slice(0, 220)}`);
});
page.on("pageerror", (e) => console.log(`${stamp()} PAGE ERROR: ${e.message}`));
const u = new URL(pageUrl);
u.searchParams.set("lobby", "off");
u.searchParams.set("env.WCNET_KEYSCRIPT", script);
u.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
u.searchParams.set("env.WCNET_LOG", "2");
for (const kv of (process.env.PAGE_ENV || "").split(/\s+/).filter(Boolean)) {
  const i = kv.indexOf("=");
  u.searchParams.set("env." + kv.slice(0, i), kv.slice(i + 1));
}
await page.goto(u.toString());
const t1 = Date.now();
await page.setInputFiles("#gamefile", game);
await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
  || (document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "xwing"), null, { timeout: 180000 })
  .catch(() => {});
const st = await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 160)}`);
check(await page.inputValue("#program") === "xwing", `${path.basename(game)} installed as X-Wing in ${((Date.now() - t1) / 1000).toFixed(1)} s (${st})`);
await page.fill("#seats", "1");
await page.dispatchEvent("#seats", "change");
await page.click("#join");
await page.waitForTimeout(1500);
await page.click("#fly");
const seconds = Number(secondsArg);
for (let t = 10; t <= seconds; t += 10) {
  await page.waitForTimeout(10000);
  await page.locator("#canvas").screenshot({ path: path.join(shotDir, `xwing-${t}.png`) }).catch(() => {});
}
check(logs.some((l) => /X-Wing: BWING\.EXE loaded/.test(l)), "BWING.EXE loaded and known");
check(!logs.some((l) => /no CD among its files/.test(l)), "its CD found among the files");
check(logs.some((l) => /X-Wing: the flight's frames are coming/.test(l)), "from boot through the front end to the mission's flight");
fs.writeFileSync(path.join(shotDir, "xwing.log"), logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `XWING PAGE FAILED (${failed})` : "XWING PAGE OK");
process.exit(failed ? 1 : 0);
