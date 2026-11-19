// The page's next steps on a phone (the user, 2026-11-19: "there's a lot to scroll through. Can there be a "Fly Solo"
// button that starts a 1 player room and joins it and then scrolls down to the game part ... a flashing button for
// next steps ... eg host tie fighter scrolls down to the room which has the glowing fly game. and click that and it
// scrolls down to the controls"): the page as a Pixel 7, the TIE Fighter demo dropped, twice.
//   1. Host: Fly Solo at the top, pulsing; Host pressed: the room made, the page scrolled to its Fly button, which
//      pulses (Fly Solo gone); Fly pressed: the game runs, the page scrolled to its picture, nothing pulses.
//   2. Fly Solo pressed: a room of one, the game running, its sound's context going (made in that touch: an iPhone
//      starts a page's sound only so), the page scrolled to its picture.
// A screenshot of each step (SHOT_DIR/steps-N-name.png).  Ends "PHONE STEPS OK".
//
//   node scripts/web-phone-steps.mjs PAGE_URL ZIP
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR
import { createRequire } from "node:module";
import path from "node:path";

const [pageUrl, zip] = process.argv.slice(2);
if (!pageUrl || !zip) {
  console.error("usage: node web-phone-steps.mjs PAGE_URL ZIP");
  process.exit(2);
}
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium, devices } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox"] });       // (the browser's own rule for sound: begun by a touch)
let failed = 0, shots = 0;
const check = (ok, what) => { console.log(`${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };

async function phone() {
  const context = await browser.newContext({ ...devices["Pixel 7"] });
  await context.addInitScript(() => {
    Element.prototype.requestPointerLock = undefined;
    Element.prototype.requestFullscreen = undefined;      // (an iPhone has none: the page's own scroll shows the game)
    Element.prototype.webkitRequestFullscreen = undefined;
  });
  const page = await context.newPage();
  const logs = [];
  page.on("console", (m) => logs.push(m.text()));
  page.on("pageerror", (e) => console.log(`PAGE ERROR: ${e.message}`));
  const u = new URL(pageUrl);
  u.searchParams.set("lobby", "off");
  await page.goto(u.toString());
  await page.setInputFiles("#gamefile", zip);
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready"
    && document.getElementById("program").value === "tiedemo", null, { timeout: 120000 }).catch(() => {});
  const shot = (name) => page.screenshot({ path: path.join(shotDir, `steps-${++shots}-${name}.png`) });
  // an element: shown, pulsing (the next step's animation), and where it is in the window
  const look = (id) => page.evaluate((id) => {
    const el = document.getElementById(id), r = el.getBoundingClientRect(), cs = getComputedStyle(el);
    return { shown: !el.hidden && r.width > 0, pulsing: cs.animationName === "nextStep" && !el.disabled, top: Math.round(r.top), bottom: Math.round(r.bottom),
             inView: r.top >= 0 && r.bottom <= innerHeight, vh: innerHeight };
  }, id);
  const settled = async () => {           // (a smooth scroll over)
    let last = -1;
    for (let i = 0; i < 40; i++) {
      const y = await page.evaluate(() => scrollY);
      if (y === last) return;
      last = y;
      await page.waitForTimeout(150);
    }
  };
  return { page, logs, shot, look, settled };
}

{ // 1. Host, then Fly
  const { page, logs, shot, look, settled } = await phone();
  let solo = await look("topSolo");
  check(solo.shown && solo.pulsing && solo.inView, `at the top, Fly Solo shown and pulsing (${JSON.stringify(solo)})`);
  check(!(await look("fly")).inView, "the room's Fly button is below the first screen");
  await shot("top");
  await page.locator("#join").scrollIntoViewIfNeeded();
  await page.locator("#join").tap();
  await page.waitForFunction(() => !document.getElementById("fly").disabled, null, { timeout: 20000 }).catch(() => {});
  await settled();
  const fly = await look("fly");
  solo = await look("topSolo");
  check(fly.inView && fly.pulsing, `Host pressed: the page scrolled to the room's Fly button, pulsing (${JSON.stringify(fly)})`);
  check(!solo.shown, "in a room, Fly Solo is gone");
  await shot("room");
  await page.locator("#fly").tap();
  await page.waitForFunction(() => !document.getElementById("touch").hidden, null, { timeout: 60000 }).catch(() => {});
  await settled();
  const screen = await look("screen");
  check(logs.some((l) => /DOSBox version/.test(l)), "Fly pressed: the game runs");
  check(screen.top >= -2 && screen.top < 60, `the page scrolled to the game's picture (its top ${screen.top} px down the window of ${screen.vh})`);
  const pulsing = await page.evaluate(() => [...document.querySelectorAll("button")].filter((b) => getComputedStyle(b).animationName === "nextStep").map((b) => b.id));
  check(pulsing.length === 0, `nothing pulses while the game runs (${pulsing.join(" ") || "none"})`);
  await page.waitForTimeout(4000);
  await shot("game");
  await page.context().close();
}
{ // 2. Fly Solo
  const { page, logs, shot, look, settled } = await phone();
  await page.locator("#topSolo").tap();
  await page.waitForFunction(() => !document.getElementById("touch").hidden, null, { timeout: 60000 }).catch(() => {});
  await settled();
  const screen = await look("screen");
  check(logs.some((l) => /flown single player: a room of one/.test(l)), "Fly Solo pressed: a room of one made, nobody told");
  await page.waitForFunction(() => window.DOSBox && window.DOSBox.SDL2 && window.DOSBox.SDL2.audio && window.DOSBox.SDL2.audio.scriptProcessorNode, null, { timeout: 30000 }).catch(() => {});
  await page.waitForTimeout(1500);
  const sound = await page.evaluate(() => { const s = window.DOSBox && window.DOSBox.SDL2, c = s && s.audioContext;
    return c ? `${c.state}${s.audio && s.audio.silenceTimer === undefined ? ", its blocks asked for" : ", no block asked for yet"}` : "none"; });
  check(sound === "running, its blocks asked for", `the game's sound is going, begun by the touch that started it (its context ${sound})`);
  check(logs.some((l) => /DOSBox version/.test(l)), "the game runs");
  check(screen.top >= -2 && screen.top < 60, `the page scrolled to the game's picture (its top ${screen.top} px down the window of ${screen.vh})`);
  await page.waitForTimeout(4000);
  await shot("solo-game");
  await page.context().close();
}
await browser.close();
console.log(failed ? `PHONE STEPS FAILED (${failed})` : "PHONE STEPS OK");
process.exit(failed ? 1 : 0);
