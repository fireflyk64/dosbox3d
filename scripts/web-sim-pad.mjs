// A controller's stick in Wing Commander's own simulator, in the browser: a room of one, the key script from boot through
// the title to the rec room, the simulator's pod, SELECT ENEMY.  On that screen the stick is a menu's pointer; in the
// simulator's flight it steers (the layer's stick_flying and the game's own steering table).  RetroVega's simulator has
// the same check in scripts/web-rv-flight.mjs (SIM=1 PAD=1).
//   PLAYWRIGHT_DIR=... GAME_FILE=<the GOG installer> node scripts/web-sim-pad.mjs PAGE_URL
import { createRequire } from "node:module";
import path from "node:path";
const [pageUrl] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR, "package.json"));
const { chromium } = require("playwright");
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const results = [];
const check = (what, ok, detail) => { results.push(ok); console.log(`${ok ? "ok  " : "FAIL"} ${what}: ${JSON.stringify(detail)}`); };
const KEYS = "14:esc,20:esc,25:!mouse=0.52:0.27,26:!button=0:1,26.3:!button=0:0,42:t,42.5:e,43:s,43.5:t,44:enter,48:x,48.5:y,49:enter,56:esc," +
  "62:!mouse=0.12:0.65,63:!button=0:1,63.3:!button=0:0";     // (the title, a name, the rec room, the pod)
const CLICK = "!mouse=0.2:0.25";
const context = await browser.newContext({ viewport: { width: 1280, height: 1000 } });
await context.addInitScript(() => {
  window.__pad = { id: "Mock Pad (STANDARD GAMEPAD)", index: 0, connected: true, mapping: "standard", timestamp: 0,
                   axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  navigator.getGamepads = () => [window.__pad];
});
const page = await context.newPage();
const logs = [];
page.on("console", (m) => logs.push(m.text()));
page.on("pageerror", (e) => console.log(`PAGE ERROR: ${e.message}`));
const u = new URL(pageUrl);
for (const [k, v] of Object.entries({ server: "http://127.0.0.1:9/lobbylink", callsign: "SIM", "env.WCNET_KEYSCRIPT": KEYS + ",72:" + CLICK + ",73:!button=0:1,73.3:!button=0:0,74:!poke=00BA:00,76:!poke=00BA:00,80:!poke=00BA:00",
                                      "env.WCNET_KEYSCRIPT_FROM": "boot", "env.WCNET_LOG": "1" })) u.searchParams.set(k, v);
await page.goto(u.toString());
if (process.env.GAME_FILE) await page.setInputFiles("#gamefile", process.env.GAME_FILE);
await page.waitForFunction(() => /^(ready|error)$/.test(document.getElementById("sourceStatus").dataset.state || ""), null, { timeout: 300000 });
const axis = (i, v) => page.evaluate(([i, v]) => { window.__pad.axes[i] = v; }, [i, v]);
const button = (i, v) => page.evaluate(([i, v]) => { window.__pad.buttons[i] = { pressed: v > 0.1, touched: v > 0, value: v }; }, [i, v]);
await button(0, 1); await page.waitForTimeout(300); await button(0, 0);
check("a button press picks the controller up", (await page.inputValue("#padSelect")).startsWith("0:Mock Pad"), await page.inputValue("#padSelect"));
await page.fill("#seats", "1");
await page.click("#join");
await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 20000 });
await page.click("#fly");
await page.waitForFunction(() => window.DOSBox && window.DOSBox._wc_web_in_flight, null, { timeout: 120000 });
const flying = () => page.evaluate(() => window.DOSBox._wc_web_in_flight());
const turn = () => page.evaluate(() => [window.DOSBox._wc_web_steer(4, 0), window.DOSBox._wc_web_steer(4, 1)]);
const t0 = Date.now();
// (a new game runs a short TrainSim flight of its own before the name is asked for: this waits for the one the rec room's
// pod starts, once the enemy has been picked; between the two the stick is a pointer)
let was = 0, flights = 0;
while (flights < 2) {
  const f = await flying();
  if (f === 1 && was === 0) flights++;
  was = f;
  if (Date.now() - t0 > 600000) { console.log(`SIM PAD FAILED (${flights} flights)`); await browser.close(); process.exit(1); }
  await page.waitForTimeout(250);
}
console.log(`the simulator's flight after ${((Date.now() - t0) / 1000).toFixed(0)} s`);
check("the simulator's flight counts as flying for the stick", (await flying()) === 1, await flying());
await page.waitForTimeout(800);
const view = await page.evaluate(() => [0, 1, 2, 3].map((i) => window.DOSBox._wc_web_steer(0, i)));
console.log(`the view is ${view.join(",")} of 640x200`);
let t = await turn();
check("the stick at rest asks for no turn", t[0] === 0 && t[1] === 0, t);
// (the simulator's game is short: quick holds, and it must still be flying at the end)
const held = async (a, v) => { await axis(a, v); await page.waitForTimeout(500); const r = await turn(); await axis(a, 0); await page.waitForTimeout(400); return r; };
const right = await held(0, 1), left = await held(0, -1), back = await held(1, 1), forward = await held(1, -1);
check("still flying after the holds", (await flying()) === 1, await flying());
check("full stick right and left are the fastest turn", right[0] === 8 && left[0] === -8 && right[1] === 0 && left[1] === 0, [right, left]);
check("full stick back and forward are the fastest turn up and down", back[1] === -8 && forward[1] === 8 && back[0] === 0 && forward[0] === 0, [back, forward]);
t = await turn();
check("and let go it is none again", t[0] === 0 && t[1] === 0, t);
await page.locator("#canvas").screenshot({ path: path.join(process.env.SHOT_DIR || ".", "sim-pad-flight.png") });
await browser.close();
const bad = results.filter((r) => !r).length;
console.log(bad ? `SIM PAD FAILED (${bad})` : `SIM PAD OK (${results.length} checks)`);
process.exit(bad ? 1 : 0);
