// The TIE Fighter demo in the browser, alone (docs/tiedemo.md): one Chrome page; the demo got as a player gets it:
// ZIP "fetch" clicks the page's "TIE Fighter demo" button (the Internet Archive's TIE_Fighter_Preview__Dodge_Neon.zip
// from its CORS address, once: the page reloaded and the button clicked again must use the copy the browser kept,
// nothing fetched), else that zip is dropped (the Internet Archive's: its floppies' folders; DOS Games Archive's
// tiedemo.zip: their zips).  The page's unpacking checked (TIEMISS.EXE ready, the Play menu's "tiedemo"), a room of one
// flown: the key script (from boot) goes on from the Neon showroom with G (the first release's: its introduction run
// ahead) and skips the opening with Esc; the canvas every 10 s in SHOT_DIR.  Ends "TIEDEMO PAGE OK".
//
//   node scripts/web-tiedemo.mjs PAGE_URL ZIP|fetch [seconds] [key script]
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR, PAGE_ENV
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, zip, secondsArg = "60", scriptArg] = process.argv.slice(2);
const fetched = zip === "fetch";
const neon = fetched || /neon/i.test(path.basename(zip));          // (the first release: the showroom first)
const script = scriptArg || (neon ? "3:g,10:esc" : "8:esc");
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
let archiveFetches = 0;
page.on("request", (r) => { if (/archive\.org\//.test(r.url())) archiveFetches++; });
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
const ready = () => page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
  || (document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "tiedemo"), null, { timeout: 120000 })
  .catch(() => {});
const status = () => page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 160)}`);
await page.goto(u.toString());
const t1 = Date.now();
if (fetched) {
  await page.click("#demoTfd");
} else {
  await page.setInputFiles("#gamefile", zip);
}
await ready();
const st = await status();
check(await page.inputValue("#program") === "tiedemo", `the ${fetched ? "button's fetch" : "zip"} installed as the TIE Fighter demo in ${((Date.now() - t1) / 1000).toFixed(1)} s (${st})`);
if (fetched) {
  check(archiveFetches === 1, `fetched from the Internet Archive once (${archiveFetches} requests)`);
  await page.waitForFunction(() => /TIE Fighter/.test(document.getElementById("savedList").textContent), null, { timeout: 30000 }).catch(() => {});
  check(/TIE Fighter/.test(await page.$eval("#savedList", (el) => el.textContent)), `kept in this browser (${(await page.$eval("#savedList", (el) => el.textContent)).slice(0, 80)})`);
  await page.goto(u.toString());
  archiveFetches = 0;
  await page.click("#demoTfd");
  await ready();
  const again = await status();
  check(archiveFetches === 0 && await page.inputValue("#program") === "tiedemo",
    `the button again after a reload: the kept copy, nothing fetched (${archiveFetches} requests; ${again})`);
}
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
if (neon) check(logs.some((l) => /Neon showroom's introduction run ahead/.test(l)), "the Neon showroom's introduction run ahead");
check(logs.some((l) => /TIE demo: the flight's frames are coming/.test(l)), "on from the showroom and the opening to the mission's flight");
fs.writeFileSync(path.join(shotDir, "tiedemo.log"), logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `TIEDEMO PAGE FAILED (${failed})` : "TIEDEMO PAGE OK");
process.exit(failed ? 1 : 0);
