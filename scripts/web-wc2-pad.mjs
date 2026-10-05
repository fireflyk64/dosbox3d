// Headless test of a controller's stick in Wing Commander II in the browser
// build: one Chrome page with a simulated gamepad flies a mission (the
// mission test, cockpit within seconds) and reads back what the game makes
// of the stick: the turn it is asked for (wc_web_steer).  The game parks its
// pointer in the middle of the cockpit's window, which is another in every
// ship, and a stick that rested anywhere else was a constant dive: at rest
// the turn must be none, and the same stick the same turn in two cockpits.
// Driven by scripts/web-wc2.sh pad.
//
//   GAME_FILE=wc2.zip node scripts/web-wc2-pad.mjs PAGE_URL LOBBY_URL
import { createRequire } from "node:module";
import path from "node:path";
const [pageUrl, lobbyUrl] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required", "--use-fake-device-for-media-stream"] });
const results = [];
const check = (what, ok, detail) => { results.push(ok); console.log(`${ok ? "ok  " : "FAIL"} ${what}: ${JSON.stringify(detail)}`); };
const mock = () => {
  window.__pad = { id: "Mock Pad (STANDARD GAMEPAD)", index: 0, connected: true, mapping: "standard", timestamp: 0,
                   axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  navigator.getGamepads = () => [window.__pad];
};

// Series 1 is flown in a Ferret, series 2 mission 0 in a Broadsword: two
// cockpits with windows of different heights.
for (const [ship, mission] of [["Ferret", "s1 m0"], ["Broadsword", "s2 m0"]]) {
  const context = await browser.newContext();
  await context.addInitScript(mock);
  const page = await context.newPage();
  page.on("pageerror", (e) => console.log(`PAGE ERROR: ${e.message}`));
  const u = new URL(pageUrl);
  // (An empty WCROOM: the game runs without a session, so nobody is waited for.)
  for (const [k, v] of Object.entries({ room: "PAD-" + Math.random().toString(36).slice(2, 8).toUpperCase(), server: lobbyUrl, callsign: "HOST",
                                        "env.WCROOM": "", cmd: `loadfix -34 wc2 Origin -k l ${mission}` })) u.searchParams.set(k, v);
  await page.goto(u.toString());
  const axis = (i, v) => page.evaluate(([i, v]) => { window.__pad.axes[i] = v; }, [i, v]);
  const button = (i, v) => page.evaluate(([i, v]) => { window.__pad.buttons[i] = { pressed: v > 0.1, touched: v > 0, value: v }; }, [i, v]);
  await page.setInputFiles("#gamefile", process.env.GAME_FILE);
  await page.waitForFunction(() => /^(Ready|Could not)/.test(document.getElementById("sourceStatus").textContent), null, { timeout: 300000 });
  await button(0, 1); await page.waitForTimeout(300); await button(0, 0);
  check(`${ship}: a button press picks the controller up`, (await page.inputValue("#padSelect")).startsWith("0:Mock Pad"), await page.inputValue("#padSelect"));
  await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 60000 });
  await page.click("#fly");
  await page.waitForFunction(() => window.DOSBox && window.DOSBox._wc_web_in_flight && window.DOSBox._wc_web_in_flight() === 1, null, { timeout: 180000 });
  await page.waitForTimeout(4000);
  const view = await page.evaluate(() => [0, 1, 2, 3].map((i) => window.DOSBox._wc_web_steer(0, i)));
  const turn = () => page.evaluate(() => [window.DOSBox._wc_web_steer(4, 0), window.DOSBox._wc_web_steer(4, 1)]);
  // What the game asks for with the stick held there (across, down), and at rest again.
  const held = async (a, v) => { await axis(a, v); await page.waitForTimeout(700); const t = await turn(); await axis(a, 0); await page.waitForTimeout(500); return t; };
  console.log(`${ship}: the view is ${view.join(",")} of 640x200`);

  let t = await turn();
  check(`${ship}: the stick at rest asks for no turn`, t[0] === 0 && t[1] === 0, t);
  // (The default layout has the pitch inverted: pulling back raises the nose,
  // and the game's "down" is then negative.)
  const back = await held(1, 1), forward = await held(1, -1), right = await held(0, 1), left = await held(0, -1);
  check(`${ship}: full stick back and forward are the same turn up and down`, back[1] === -5 && forward[1] === 5 && back[0] === 0 && forward[0] === 0, [back, forward]);
  check(`${ship}: full stick right and left are the same turn`, right[0] === 5 && left[0] === -5 && right[1] === 0 && left[1] === 0, [right, left]);
  const half = await held(0, 0.55), little = await held(1, 0.3);
  check(`${ship}: less stick is less turn`, half[0] >= 2 && half[0] <= 4 && little[1] <= -1 && little[1] >= -2, [half, little]);
  t = await turn();
  check(`${ship}: and let go it is none again`, t[0] === 0 && t[1] === 0, t);
  // At full sensitivity the stick reaches the game's fastest turn (the view's edge).
  await page.$eval("#padSensitivity", (el) => { el.value = "100"; el.dispatchEvent(new Event("input")); el.dispatchEvent(new Event("change")); });
  const edge = await held(0, 1), up = await held(1, 1);
  check(`${ship}: at full sensitivity full stick is the fastest turn`, edge[0] === 8 && up[1] === -8, [edge, up]);
  await context.close();
}
await browser.close();
console.log(results.every(Boolean) ? "WC2 PAD OK" : "WC2 PAD FAILED");
process.exit(results.every(Boolean) ? 0 : 1);
