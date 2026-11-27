// Headless test of Wing Commander 1's arrows on the page: one Chrome page flies
// Enyo 1 alone and holds Space (the guns) while it turns with the Right arrow,
// then lets the arrow go with Space still down.  The game itself zeroes the
// turn only when no key at all is held, so the ship went on turning until
// Space came up too; the emulator now stops an axis whose keys are all up
// (steer_keys_held, src/cpu/wcnet_hooks.cpp).  The Controller list has the
// game's own keyboard as "Original keyboard" (WCNET_TURNLATCH at the launch,
// wc_web_turn_latch while flying), and this checks both, switched in flight.
//
//   node scripts/web-wc1-keys.mjs PAGE_URL LOBBY_URL
//   GAME_SOURCE=gog GAME_FILE=<GOG installer> node scripts/web-wc1-keys.mjs PAGE_URL LOBBY_URL
//
// Prints "WC1 KEYS OK" when every check passed.
import { createRequire } from "node:module";
import path from "node:path";
const [pageUrl, lobbyUrl] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const results = [];
const check = (what, ok, detail) => { results.push(ok); console.log(`${ok ? "ok  " : "FAIL"} ${what}: ${JSON.stringify(detail)}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const FRONT = 0xB4B6;     // WC.EXE dseg: slot 0's front vector, three int32

const context = await browser.newContext();
const page = await context.newPage();
page.on("pageerror", (e) => console.log(`PAGE ERROR: ${e.message}`));
const u = new URL(pageUrl);
// (An empty WCROOM: no session, nobody waited for; Enyo 1 after the briefing, tapped through; our ship
// made invulnerable; the key script holds the autopilot's keys off the flight.)
for (const [k, v] of Object.entries({ room: "KEYS-" + Math.random().toString(36).slice(2, 8).toUpperCase(), server: lobbyUrl, callsign: "HOST",
                                      "env.WCROOM": "", "env.MIS": "0", "env.SERIES": "1",
                                      "env.WCNET_AUTOKEYS": "1", "env.WCNET_KEYSCRIPT": "0.5:!poke=00BA:00" })) u.searchParams.set(k, v);
await page.addInitScript(() => { try { localStorage.removeItem("wc:wc1Keys"); } catch (e) { /* fresh */ } });
await page.goto(u.toString());
if (process.env.GAME_FILE) await page.setInputFiles("#gamefile", process.env.GAME_FILE);
await page.waitForFunction(() => /^(ready|error)$/.test(document.getElementById("sourceStatus").dataset.state || ""), null, { timeout: 300000 });
// (the list is drawn again on the controls' next poll after the game is known: a dropped installer takes a moment)
await page.waitForFunction(() => !!document.querySelector('#padSelect option[value="none-original"]'), null, { timeout: 15000 }).catch(() => {});
const options = await page.$$eval("#padSelect option", (os) => os.map((o) => [o.value, o.textContent]));
check("the Controller list: Keyboard + mouse and Original keyboard", JSON.stringify(options.slice(0, 2)) ===
      JSON.stringify([["none", "Keyboard + mouse"], ["none-original", "Original keyboard"]]), options);
check("Keyboard + mouse picked by default", (await page.inputValue("#padSelect")) === "none", await page.inputValue("#padSelect"));
await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 60000 });
await page.click("#fly");
await page.waitForFunction(() => window.DOSBox && window.DOSBox._wc_web_in_flight && window.DOSBox._wc_web_in_flight() === 1, null, { timeout: 300000 });
await sleep(4000);

const latch = () => page.evaluate(() => window.DOSBox._wc_web_turn_latch(-1));
const yaw = () => page.evaluate(() => window.DOSBox._wc_web_steer(4, 0));   // (steer 4: 0 the yaw, 1 the pitch)
const front = () => page.evaluate((at) => {
  const b = (i) => window.DOSBox._wc_web_ds_byte(at + i) & 0xff;
  return [0, 4, 8].map((o) => (b(o) | (b(o + 1) << 8) | (b(o + 2) << 16) | (b(o + 3) << 24)));
}, FRONT);
const degrees = (a, b) => Math.acos(Math.max(-1, Math.min(1, (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (Math.hypot(...a) * Math.hypot(...b))))) * 180 / Math.PI;

// Space held, Right held 0.8 s (the ramp to its top), Right let go: the turn 0.3 s on, and how far the nose goes in 1 s after.
async function letGoWithSpace() {
  await page.keyboard.down("Space");
  await sleep(200);
  await page.keyboard.down("ArrowRight");
  await sleep(800);
  const held = await yaw();
  await page.keyboard.up("ArrowRight");
  await sleep(300);
  const after = await yaw();
  const from = await front();
  await sleep(1000);
  const moved = degrees(from, await front());
  await page.keyboard.up("Space");
  await sleep(400);
  const allUp = await yaw();
  await sleep(1500);
  return { held, after, moved: Math.round(moved * 10) / 10, allUp };
}

check("in flight: the emulator stops a released arrow's turn", (await latch()) === 0, await latch());
const ours = await letGoWithSpace();
check("Keyboard + mouse: Right turns while held", Math.abs(ours.held) >= 5, ours);
check("Keyboard + mouse: Right let go with Space down, the turn stops", ours.after === 0 && ours.moved < 2, ours);

await page.selectOption("#padSelect", "none-original");
await sleep(300);
check("Original keyboard picked in flight: the game's own keys", (await latch()) === 1 &&
      (await page.evaluate(() => localStorage.getItem("wc:wc1Keys"))) === "original", await latch());
const orig = await letGoWithSpace();
check("Original keyboard: the turn goes on while Space is down", Math.abs(orig.after) >= 5 && orig.moved > 20, orig);
check("Original keyboard: Space up too, the turn stops", orig.allUp === 0, orig);

await page.selectOption("#padSelect", "none");
await sleep(300);
check("Keyboard + mouse picked again", (await latch()) === 0 &&
      (await page.evaluate(() => localStorage.getItem("wc:wc1Keys"))) === "steady", await latch());
const again = await letGoWithSpace();
check("Keyboard + mouse again: the turn stops", again.after === 0 && again.moved < 2, again);
await page.screenshot({ path: path.join(process.env.OUT || ".", "wc1-keys.png") });

await browser.close();
const ok = results.every(Boolean);
console.log(ok ? "WC1 KEYS OK" : "WC1 KEYS FAILED");
process.exit(ok ? 0 : 1);
