// Headless test of Wing Commander 1 at 640 x 400 on the page (src/cpu/wcnet_wch.cpp): one Chrome page flies Enyo 1
// alone with the High resolution (2x) box ticked: the box is there for Wing Commander, the emulator has the picture
// on (level 2) and the canvas is 640 x 400 in flight; ` steps to the space alone (1) and to the game's own (0), whose
// canvas is the game's again; ticking and unticking the box while flying turns it on and off.  Pictures of each in
// $OUT.  Prints "WC1 HIRES OK" when every check passed.
//
//   node scripts/web-wc1-hires.mjs PAGE_URL LOBBY_URL
//   GAME_FILE=<GOG installer> node scripts/web-wc1-hires.mjs PAGE_URL LOBBY_URL
import { createRequire } from "node:module";
import path from "node:path";
const [pageUrl, lobbyUrl] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const OUT = process.env.OUT || ".";
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const results = [];
const check = (what, ok, detail) => { results.push(ok); console.log(`${ok ? "ok  " : "FAIL"} ${what}: ${JSON.stringify(detail)}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const page = await (await browser.newContext()).newPage();
page.on("pageerror", (e) => console.log(`PAGE ERROR: ${e.message}`));
const u = new URL(pageUrl);
// (no session; Enyo 1 after the briefing; our ship invulnerable; the key script keeps the autopilot's keys off the flight)
for (const [k, v] of Object.entries({ room: "HIRES-" + Math.random().toString(36).slice(2, 8).toUpperCase(), server: lobbyUrl, callsign: "HOST",
                                      "env.WCROOM": "", "env.MIS": "0", "env.SERIES": "1",
                                      "env.WCNET_AUTOKEYS": "1", "env.WCNET_KEYSCRIPT": "0.5:!poke=00BA:00,3:!face=2" })) u.searchParams.set(k, v);
await page.addInitScript(() => { try { localStorage.setItem("wc:wcHires", "1"); } catch (e) { /* fresh */ } });
await page.goto(u.toString());
if (process.env.GAME_FILE) await page.setInputFiles("#gamefile", process.env.GAME_FILE);
await page.waitForFunction(() => /^(ready|error)$/.test(document.getElementById("sourceStatus").dataset.state || ""), null, { timeout: 300000 });
await page.waitForFunction(() => !document.getElementById("wcHiresLabel").hidden, null, { timeout: 15000 }).catch(() => {});
check("the box is there for Wing Commander, ticked as remembered", !(await page.locator("#wcHiresLabel").isHidden()) && (await page.isChecked("#wcHires")), null);
await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 60000 });
await page.click("#fly");
await page.waitForFunction(() => window.DOSBox && window.DOSBox._wc_web_in_flight && window.DOSBox._wc_web_in_flight() === 1, null, { timeout: 300000 });
await sleep(6000);

const level = () => page.evaluate(() => window.DOSBox._wc_web_wc_hires(-1));
const canvas = () => page.evaluate(() => { const c = document.querySelector("canvas#canvas") || document.querySelector("canvas"); return [c.width, c.height]; });
const shot = (name) => page.locator("canvas").first().screenshot({ path: path.join(OUT, name) });

check("in flight: the picture on (level 2)", (await level()) === 2, await level());
// (the gunsight as Privateer's line art: its shapes are in EMS on the page, which once left it the game's blocks)
const marks0 = await page.evaluate(() => window.DOSBox._wc_web_wc_hires(-2));
await sleep(1500);
const marks1 = await page.evaluate(() => window.DOSBox._wc_web_wc_hires(-2));
check("the gunsight drawn as line art", marks1 > marks0, [marks0, marks1]);
check("the canvas is 640 x 400", JSON.stringify(await canvas()) === "[640,400]", await canvas());
await shot("wc1-hires-all.png");
await page.keyboard.press("Backquote");
await sleep(1500);
check("` steps to the space alone (1)", (await level()) === 1, await level());
await shot("wc1-hires-space.png");
await page.keyboard.press("Backquote");
await sleep(1500);
check("` again: the game's own (0)", (await level()) === 0, await level());
console.log(`(the game's own canvas: ${JSON.stringify(await canvas())})`);
await shot("wc1-hires-off.png");
await page.keyboard.press("Backquote");
await sleep(1500);
check("` again: all (2)", (await level()) === 2, await level());
// (in flight the flight panel covers the options: the box changed as a click would change it)
const tick = (on) => page.evaluate((on) => { const b = document.getElementById("wcHires"); b.checked = on; b.dispatchEvent(new Event("change")); }, on);
await tick(false);
await sleep(1500);
check("the box unticked while flying: off", (await level()) === 0, [await level(), await canvas()]);
await tick(true);
await sleep(1500);
check("ticked again: on", (await level()) === 2, [await level(), await canvas()]);
check("the game still flying", (await page.evaluate(() => window.DOSBox._wc_web_in_flight())) === 1, null);

await browser.close();
const ok = results.every(Boolean);
console.log(ok ? "WC1 HIRES OK" : "WC1 HIRES FAILED");
process.exit(ok ? 0 : 1);
