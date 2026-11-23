// Armada with a pad on the page (the user, 2026-11-22: "please ensure ... joystick ... work"): one browser, a room of one,
// a pad (navigator.getGamepads mocked: a standard pad) picked as the Built in joystick, the game's JOY 1 calibrated in its
// Options (Alt+O: upper left, lower right, centre, each with the button), then a Gauntlet flight with the stick moved;
// the control packet's stick bytes (WCNET_ARM_PACKET) must follow it, the arrows' signs: right +, left -, forward -
// (the nose down, as the Up arrow), back +.
// PLAYWRIGHT_DIR points to a directory with Playwright installed.
//   node scripts/web-armada-pad.mjs PAGE_URL LOBBY_URL GOG_INSTALLER
import assert from "node:assert/strict";
import fs from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";

const [pageUrl, lobbyUrl, installer] = process.argv.slice(2);
assert(pageUrl && lobbyUrl && installer, "usage: web-armada-pad.mjs PAGE_URL LOBBY_URL GOG_INSTALLER");
const require = createRequire(path.resolve(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const out = process.env.SHOT_DIR || "/tmp/web-armada-pad";
fs.mkdirSync(out, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });

function clickAt(time, x, y) {
  return `${time}:!mouse=${x}:${y},${time + 0.4}:!button=0:1,${time + 0.6}:!button=0:0`;
}
// the calibration's steps: at these emulated seconds the pad goes to a corner and its button is pressed
const CAL = [[15.5, -1, -1], [17.5, 1, 1], [19.5, 0, 0]];
const STICK = [[50, 1, 0, "right"], [52, -1, 0, "left"], [54, 0, -1, "forward"], [56, 0, 1, "back"]];
const route = ["7:esc", "10:esc", "12:+alt", "12.1:o", "12.3:-alt", clickAt(14, 0.453, 0.733),
  ...CAL.map(([t]) => `${t}:!frame=cal${t}`), clickAt(22, 0.705, 0.88),
  clickAt(25, 0.5, 0.6), clickAt(29, 0.5, 0.63), clickAt(32, 0.5, 0.3), "36:a", "36.3:c", "36.6:e", "37:enter", "42:enter",
  "47:backspace", ...STICK.map(([t]) => `${t}:!frame=stick${t}`), "58:!frame=end"];

const page = await browser.newPage({ viewport: { width: 1200, height: 960 } });
await page.addInitScript(() => {
  window.__pad = { id: "Mock Pad (STANDARD GAMEPAD)", index: 0, connected: true, mapping: "standard", timestamp: 0,
    axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  navigator.getGamepads = () => [window.__pad];
  sessionStorage.setItem("wcpad:choice", "joy|0:Mock Pad (STANDARD GAMEPAD)");
});
const logs = [];
page.on("console", (line) => {
  logs.push(line.text());
  if (/Armada packet|joystick|failed/.test(line.text())) console.log(line.text());
});
page.on("pageerror", (error) => console.error(error.message));
try {
  const url = new URL(pageUrl);
  url.searchParams.set("server", lobbyUrl);
  url.searchParams.set("callsign", "PAD");
  url.searchParams.set("advertise", "off");
  url.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
  url.searchParams.set("env.WCNET_LOG", "2");
  url.searchParams.set("env.WCNET_ARM_PACKET", "1");
  url.searchParams.set("env.WCNET_KEYSCRIPT", route.join(",").split(",").sort((a, b) => parseFloat(a) - parseFloat(b)).join(","));
  await page.goto(url.href);
  await page.setInputFiles("#gamefile", installer);
  await page.waitForFunction(() => document.querySelector("#sourceStatus").dataset.state === "ready" &&
    document.querySelector("#program").value === "armada", null, { timeout: 90_000 });
  await page.locator("#fullStart").evaluate((el) => { if (el.checked) el.click(); });
  await page.fill("#seats", "1");
  await page.dispatchEvent("#seats", "change");
  await page.click("#join");
  await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30_000 });
  await page.click("#fly");
  const at = async (time) => page.waitForFunction((line) => document.querySelector("#log").textContent.includes(line),
    `key script ${time.toFixed(1)}s`, { timeout: 200_000 });
  const pad = (x, y, button) => page.evaluate(([x, y, button]) => {
    window.__pad.axes = [x, y, 0, 0];
    window.__pad.buttons[0] = { pressed: button, touched: button, value: button ? 1 : 0 };
    window.__pad.timestamp++;
  }, [x, y, button]);
  for (const [time, x, y] of CAL) {
    await at(time);
    await pad(x, y, false);
    await page.waitForTimeout(400);
    await pad(x, y, true);
    await page.waitForTimeout(300);
    await pad(x, y, false);
    await page.locator("#canvas").screenshot({ path: path.join(out, `cal${time}.png`) });
  }
  const seen = {};
  for (const [time, x, y, name] of STICK) {
    await at(time);
    const mark = logs.length;
    await pad(x, y, false);
    await page.waitForTimeout(900);
    await page.locator("#canvas").screenshot({ path: path.join(out, `stick-${name}.png`) });
    await pad(0, 0, false);
    await page.waitForTimeout(400);
    seen[name] = logs.slice(mark).map((l) => /Armada packet: .*\(x ([+-]\d+), y ([+-]\d+)\)/.exec(l)).filter(Boolean)
      .map(([, px, py]) => [Number(px), Number(py)]);
  }
  console.log(JSON.stringify(seen));
  const most = (name, axis, sign) => Math.max(0, ...seen[name].map((v) => sign * v[axis]));
  assert(most("right", 0, 1) >= 20, `the stick right: x + (${JSON.stringify(seen.right)})`);
  assert(most("left", 0, -1) >= 20, `the stick left: x - (${JSON.stringify(seen.left)})`);
  assert(most("forward", 1, -1) >= 20, `the stick forward: y - (the nose down) (${JSON.stringify(seen.forward)})`);
  assert(most("back", 1, 1) >= 20, `the stick back: y + (the nose up) (${JSON.stringify(seen.back)})`);
  for (const name of Object.keys(seen)) {
    const last = seen[name].at(-1);
    assert(last && Math.abs(last[0]) <= 6 && Math.abs(last[1]) <= 6, `${name}: centred again (${JSON.stringify(last)})`);
  }
  console.log(`ARMADA PAD OK. Screenshots in ${out}`);
} finally {
  await browser.close();
}
