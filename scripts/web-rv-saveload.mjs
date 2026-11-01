// web-rv-saveload.mjs: on a phone, a bunk saved by taps survives the page being reloaded and loads again.  Pixel 7, touch
// only: bunk 2 saved by name with the Keys button, the page reloaded, the bunk menu, bunk 2's LOAD.  Either page, /rv (the
// native build) or /dosbox/?retrovega; prints "SAVELOAD OK" when every step showed.  Screenshots in SHOT_DIR.
//   PLAYWRIGHT_DIR=... SHOT_DIR=... node scripts/web-rv-saveload.mjs PAGE_URL
import { createRequire } from "node:module";
import path from "node:path";

const [pageUrl] = process.argv.slice(2);
if (!pageUrl) { console.error("usage: node web-rv-saveload.mjs PAGE_URL"); process.exit(2); }
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium, devices } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const dosbox = /[?&]retrovega/.test(pageUrl);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
let fails = 0;
const ok = (c, what) => { console.log((c ? "ok   " : "FAIL ") + what); if (!c) fails++; return c; };
const logs = [];
try {
  const context = await browser.newContext({ ...devices["Pixel 7"] });
  await context.addInitScript(() => { Element.prototype.requestPointerLock = undefined; });
  const page = await context.newPage();
  page.on("console", (m) => logs.push(m.text()));
  page.on("pageerror", (e) => logs.push("PAGE ERROR: " + e.message));
  let n = 0;
  const shot = async (name) => page.locator("#canvas").screenshot({ path: path.join(shotDir, `saveload-${dosbox ? "dosbox" : "rv"}-${String(++n).padStart(2, "0")}-${name}.png`) });
  const rvlog = () => page.evaluate((d) => {
    if (!d) return window.RetroVegaPage.log();
    try { return new TextDecoder().decode(window.DOSBox.FS.readFile("/game/RVLOG.TXT")); } catch (e) { return ""; }
  }, dosbox);
  const waitLog = async (re, ms) => {
    const until = Date.now() + ms;
    for (;;) { const t = await rvlog(); if (re.test(t) || Date.now() > until) return t; await sleep(500); }
  };
  // the page opened and the game running, to the picture's place on the screen
  const start = async () => {
    await page.goto(pageUrl);
    if (dosbox) {
      await page.waitForFunction(() => document.getElementById("sourceStatus") && document.getElementById("sourceStatus").dataset.state === "ready",
        null, { timeout: 120000 });
      await page.locator("#seats").fill("1");
      await page.locator("#join").tap();
      await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 120000 });
    } else {
      await page.waitForFunction(() => !document.getElementById("fly").disabled, null, { timeout: 120000 });
    }
    await page.locator("#fly").tap();
    const t = await waitLog(/input: mouse/, 180000);
    ok(/rvHello: DS is ready/.test(t), "the game started");
    await sleep(dosbox ? 6000 : 3000);
    await page.locator("#canvas").scrollIntoViewIfNeeded();
    const b = await page.$eval("#canvas", (c) => { const r = c.getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height }; });
    const pw = Math.min(b.w, b.h * 1.6), ph = pw / 1.6;
    return { x: b.x + (b.w - pw) / 2, y: b.y + (b.h - ph) / 2, w: pw, h: ph };
  };
  let pic = await start();
  const tap = async (gx, gy) => {
    await page.touchscreen.tap(pic.x + gx * pic.w / 640, pic.y + gy * pic.h / 400);
    await sleep(dosbox ? 1500 : 900);
  };
  await tap(320, 200);                  // (the welcome closes)
  await tap(118, 213);                  // the bunk
  await tap(300, 118);                  // bunk 2
  await tap(125, 323);                  // SAVE (S): the name
  await page.locator("#touchKeys").tap();
  await sleep(500);
  await page.keyboard.insertText("Cal Brody");
  await sleep(1500);
  await page.keyboard.press("Enter");
  let t = await waitLog(/saved bunk 2 'Cal Brody'/, 8000);
  ok(/saved bunk 2 'Cal Brody'/.test(t), "saved to bunk 2 by touch");
  await shot("saved");
  await sleep(dosbox ? 4000 : 2000);     // (what a player would do: stay a moment, and go)
  pic = await start();
  await tap(320, 200);                  // (the welcome, if it comes again)
  await tap(118, 213);                  // the bunk
  await shot("menu-after-reload");
  await tap(300, 118);                  // bunk 2
  await shot("bunk-2-selected");
  t = await rvlog();
  ok(!/saved bunk/.test(t), "this is a new run of the game: nothing saved in it yet");
  await tap(238, 323);                  // LOAD (L), the second button of the row
  t = await waitLog(/loaded bunk 2 'Cal Brody'/, 6000);
  ok(/loaded bunk 2 'Cal Brody'/.test(t), "after the reload bunk 2 loads by touch");
  ok(!/saved bunk/.test(t), "and it was the first run's save: nothing saved since the reload");
  await shot("loaded");
  console.log((t.match(/(saved|loaded) bunk.*/g) || []).join(" | "));
} catch (e) {
  ok(false, "stopped: " + (e && e.message ? e.message.split("\n")[0] : e));
  try {
    const page = browser.contexts()[0].pages()[0];
    await page.screenshot({ path: path.join(shotDir, "saveload-stopped.png"), fullPage: true });
    console.log(await page.evaluate(() => JSON.stringify({ status: (document.getElementById("sourceStatus") || {}).textContent,
      state: (document.getElementById("sourceStatus") || { dataset: {} }).dataset.state, lobbyHidden: document.getElementById("lobby").hidden,
      flyDisabled: document.getElementById("fly").disabled, join: document.getElementById("join").disabled })));
  } catch (e2) { /* the page is gone */ }
}
if (logs.some((l) => /PAGE ERROR/.test(l))) console.log(logs.filter((l) => /PAGE ERROR/.test(l)).slice(0, 3).join("\n"));
await browser.close();
console.log(fails ? `SAVELOAD: ${fails} failed` : "SAVELOAD OK");
process.exit(fails ? 1 : 0);
