// The TIE Fighter demo in the browser, alone (docs/tiedemo.md): one Chrome page with the page's link to the download
// there, tiedemo.zip dropped as a player drops it (DOS Games Archive's: the zips of its two floppies), the page's
// unpacking checked (TIEMISS.EXE ready, the Play menu's "tiedemo"), a room of one flown: the key script (from boot)
// skips the opening with Esc; the canvas every 10 s in SHOT_DIR.  Ends "TIEDEMO PAGE OK".
//
//   node scripts/web-tiedemo.mjs PAGE_URL ZIP [seconds] [key script]
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR, PAGE_ENV
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, zip, secondsArg = "60", script = "8:esc"] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (ok, what) => { console.log(`${stamp()} ${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };
const page = await browser.newPage({ viewport: { width: 1280, height: 1100 } });
const logs = [];
page.on("console", (m) => {
  const t = m.text();
  logs.push(t);
  if (/TIE Fighter demo|tiedemo|TIEMISS|settings|could not|Aborted|RuntimeError|error/i.test(t)) console.log(`${stamp()} ${t.slice(0, 200)}`);
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
const link = await page.$eval("#demoTfd", (a) => a.href).catch(() => "");
check(/dosgamesarchive\.com\/.*tiedemo/.test(link), `the download link (${link})`);
const t1 = Date.now();
await page.setInputFiles("#gamefile", zip);
await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
  || (document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "tiedemo"), null, { timeout: 120000 })
  .catch(() => {});
const st = await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 160)}`);
check(await page.inputValue("#program") === "tiedemo", `the zip installed as the TIE Fighter demo in ${((Date.now() - t1) / 1000).toFixed(1)} s (${st})`);
await page.fill("#seats", "1");
await page.dispatchEvent("#seats", "change");
await page.click("#join");
await page.waitForTimeout(1500);
await page.click("#fly");
const seconds = Number(secondsArg);
for (let t = 10; t <= seconds; t += 10) {
  await page.waitForTimeout(10000);
  await page.locator("#canvas").screenshot({ path: path.join(shotDir, `tiedemo-${t}.png`) }).catch(() => {});
}
check(logs.some((l) => /TIE Fighter demo's settings/.test(l)), "the settings written (LANDRU.CFG, OPTIONS.CFG)");
fs.writeFileSync(path.join(shotDir, "tiedemo.log"), logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `TIEDEMO PAGE FAILED (${failed})` : "TIEDEMO PAGE OK");
process.exit(failed ? 1 : 0);
