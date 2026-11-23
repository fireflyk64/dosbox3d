// Armada with a pad on the page (the user, 2026-11-22: "please ensure ... joystick ... work"; 2026-11-23: "Can we bypass
// [the in-game calibration] unless they want to *recalibrate*"): one browser, a room of one, a pad (navigator.getGamepads
// mocked: a standard pad) picked as the Built in joystick, no calibration (the page writes JOYA.DAT, the emulator gives
// the game its readings exact), then a Gauntlet flight with the stick moved; the control packet's stick bytes
// (WCNET_ARM_PACKET) must be exact: right +32, left -32, forward -32 (the nose down, as the Up arrow), back +32, at
// rest 0, half stick 17 (steps 3..32 share the travel past the page's 4% dead zone), a little past it 3 (the first
// step the packet counts).  HIRES=1: the same at 640x400, the CPU three times as fast (the readings must not change).
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
// the stick's moves: at these emulated seconds (in flight from ~37 s) the pad goes there and back to the middle
const STICK = [[38, 1, 0, "right"], [40, -1, 0, "left"], [42, 0, -1, "forward"], [44, 0, 1, "back"], [46, 0.5, 0, "half"],
  [48, 0.06, 0, "little"]];
const route = ["7:esc", "10:esc", clickAt(13, 0.5, 0.6), clickAt(17, 0.5, 0.63), clickAt(20, 0.5, 0.3), "24:a", "24.3:c",
  "24.6:e", "25:enter", "30:enter", "35:backspace", ...STICK.map(([t]) => `${t}:!frame=stick${t}`), "50:!frame=end"];

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
  if (/Armada packet|joystick|JOYA|640x400|failed/.test(line.text())) console.log(line.text());
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
  if (process.env.HIRES === "1") url.searchParams.set("env.WCARMHIRES", "1");
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
  const want = { right: [32, 0], left: [-32, 0], forward: [0, -32], back: [0, 32], half: [17, 0], little: [3, 0] };
  for (const [name, [x, y]] of Object.entries(want)) {
    const moved = seen[name].filter((v) => v[0] || v[1]);
    assert(moved.length && moved.every((v) => v[0] === x && v[1] === y), `the stick ${name}: x ${x}, y ${y} every frame (${JSON.stringify(seen[name])})`);
    const last = seen[name].at(-1);
    assert(last && last[0] === 0 && last[1] === 0, `${name}: centred again, 0 (${JSON.stringify(last)})`);
  }
  assert(logs.some((l) => l.includes("Armada's JOYA.DAT")), "the page wrote JOYA.DAT");
  assert(logs.some((l) => l.includes("the joystick's readings exact")), "the emulator gave the readings");
  if (process.env.HIRES === "1") assert(logs.some((l) => l.includes("the flight's picture is 640x400")), "flown at 640x400");
  console.log(`ARMADA PAD OK. Screenshots in ${out}`);
} finally {
  await browser.close();
}
