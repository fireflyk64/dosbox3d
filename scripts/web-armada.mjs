// Two browsers in one LobbyLink room, exercising the game's own network menus.
// PLAYWRIGHT_DIR points to a directory with Playwright installed.
// MODE=arcade or MODE=strategy exercises Proving Grounds or Armada's strategy mode.
// node scripts/web-armada.mjs PAGE_URL LOBBY_URL GOG_INSTALLER [GUEST_ZIP]
import assert from "node:assert/strict";
import fs from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { KEY } from "../web/gamepad.js";

const [pageUrl, lobbyUrl, installer, guestFiles = installer] = process.argv.slice(2);
assert(pageUrl && lobbyUrl && installer, "usage: web-armada.mjs PAGE_URL LOBBY_URL GOG_INSTALLER [GUEST_ZIP]");
const require = createRequire(path.resolve(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const out = process.env.SHOT_DIR || "/tmp/web-armada";
fs.mkdirSync(out, { recursive: true });
const mode = process.env.MODE || "battle";
assert(["battle", "strategy", "arcade"].includes(mode), `unknown MODE ${mode}`);
const arena = mode === "arcade";
const strategy = mode === "strategy";
const program = arena ? "armadapg" : "armada";
const room = `${arena ? "PG" : "ARM"}-${mode}-${Date.now()}`;
const browser = await chromium.launch({ executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });

function clickAt(time, x, y) {
  return `${time}:!mouse=${x}:${y},${time + 0.4}:!button=0:1,${time + 0.6}:!button=0:0`;
}
function typeAt(time, name) {
  return [...name, "enter"].map((key, i) => `${time + i * 0.3}:${key}`).join(",");
}
const entry = arena ? ["7:esc", "10:esc", "14:space", clickAt(19, 0.48, 0.51)]
  : ["7:esc", "10:esc", clickAt(13, 0.5, 0.71), clickAt(17, 0.5, 0.615), "21:y"];
const routes = arena ? {
  host: [clickAt(46, 0.31, 0.475), "48:p", "49:enter", "51:n", "52:enter", clickAt(72, 0.125, 0.475)],
  guest: [clickAt(60, 0.35, 0.205), clickAt(62, 0.5, 0.475)],
} : {
  host: [clickAt(65, 0.31, 0.475), strategy ? "69:a" : "69:b", "70:enter", clickAt(92, 0.125, 0.475)],
  guest: [clickAt(82, 0.5, 0.475)],
};
if (strategy) routes.host.push("73:c", "74:enter", clickAt(98, 0.43, 0.294));
const checkpoints = strategy ? { lobby: 88, turn1: 108, turn2: 126 }
  : arena ? { lobby: 69, ships: 88, cockpit: 103 } : { lobby: 88, ships: 108, cockpit: 123 };

async function pilot(who, files) {
  const page = await browser.newPage({ viewport: { width: 1200, height: 960 } });
  const logs = [];
  page.on("console", (line) => {
    logs.push(line.text());
    if (/IPX|extract|lobby: joined|failed|key script/.test(line.text())) console.log(`${who}: ${line.text()}`);
  });
  page.on("pageerror", (error) => console.error(`${who}: ${error.message}`));
  const url = new URL(pageUrl);
  url.searchParams.set("server", lobbyUrl);
  url.searchParams.set("callsign", who.toUpperCase());
  url.searchParams.set("advertise", "off");
  url.searchParams.set("hall", `${room}-hall`);
  if (who === "guest") url.searchParams.set("room", room);
  url.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
  url.searchParams.set("env.WCNET_LOG", "2");
  const route = [...entry, typeAt(arena ? 23 : 33, who), ...routes[who]];
  if (strategy) route.push(clickAt(104, 0.82, 0.735), clickAt(114, 0.78, 0.925), clickAt(118, 0.48, 0.35));
  else {
    const time = arena ? 78 : 98;
    route.push(clickAt(time, who === "host" ? 0.12 : 0.9, 0.13), clickAt(time + 5, 0.16, 0.97), clickAt(time + 14, 0.16, 0.91));
  }
  route.push(...Object.entries(checkpoints).map(([name, time]) => `${time}:!frame=${name}`));
  url.searchParams.set("env.WCNET_KEYSCRIPT", route.sort((a, b) => parseFloat(a) - parseFloat(b)).join(","));
  await page.goto(url.href);
  await page.setInputFiles("#gamefile", files);
  await page.waitForFunction((want) => {
    const state = document.querySelector("#sourceStatus").dataset.state;
    return state === "error" || (state === "ready" && document.querySelector("#program").value === want);
  }, who === "host" ? "armada" : program, { timeout: 90_000 });
  assert.equal(await page.locator("#sourceStatus").getAttribute("data-state"), "ready", await page.locator("#sourceStatus").textContent());
  assert.equal(await page.locator("#program option[value=armadapg]").count(), 1, "bundled Proving Grounds is available");
  if (arena && who === "host") await page.selectOption("#program", program);
  assert.equal(await page.locator("#seats").getAttribute("max"), arena ? "6" : "2");
  if (arena) assert.equal(await page.locator("#seats").getAttribute("min"), "2");
  await page.locator("#fullStart").evaluate((el) => { if (el.checked) el.click(); });
  return { who, page, logs };
}

async function screenshotAt({ who, page }, name, time) {
  const line = `key script ${time.toFixed(1)}s`;
  await page.waitForFunction(({ line, name }) => document.querySelector("#log").textContent.split("\n")
    .some((entry) => entry.includes(line) && entry.includes(`/${name}.ppm`)), { line, name }, { timeout: 170_000 });
  await page.locator("#canvas").screenshot({ path: path.join(out, `${who}-${name}.png`) });
}

// The right-button drag steers through the emulator (wc_web_arm_steer: in proportion, as a stick) and the left button
// with it fires (Space); the emulator's log says the drag steered and was let go.
async function flyWithMouse({ page, logs }) {
  await page.evaluate(() => {
    const send = DOSBox._wc_web_key, steer = DOSBox._wc_web_arm_steer;
    window.mouseFlightKeys = [];
    window.mouseFlightSteer = [];
    DOSBox._wc_web_key = (key, pressed) => { window.mouseFlightKeys.push([key, pressed]); send(key, pressed); };
    DOSBox._wc_web_arm_steer = (x, y, on) => { window.mouseFlightSteer.push([x, y, on]); steer(x, y, on); };
  });
  const canvas = page.locator("#canvas");
  await canvas.focus();
  const { x, y, width, height } = await canvas.boundingBox();
  await page.mouse.move(x + width / 2, y + height / 3);
  await page.mouse.down({ button: "right" });
  await page.mouse.move(x + width / 2 + 32, y + height / 3, { steps: 8 });
  await page.waitForTimeout(600);
  await page.mouse.move(x + width / 2 + 100, y + height / 3, { steps: 8 });
  await page.mouse.down();
  await page.waitForTimeout(800);
  await canvas.screenshot({ path: path.join(out, "host-mouse-flight.png") });
  await page.mouse.up();
  await page.mouse.up({ button: "right" });
  const { keys, steer } = await page.evaluate(() => ({ keys: window.mouseFlightKeys, steer: window.mouseFlightSteer }));
  assert(keys.some(([k, down]) => k === KEY.space && down === 1), "mouse flight pressed Space");
  assert(keys.some(([k, down]) => k === KEY.space && down === 0), "mouse flight released Space");
  assert(!keys.some(([k]) => [KEY.kp4, KEY.kp6, KEY.kp8, KEY.kp2].includes(k)), "the drag steers without the keypad");
  assert(steer.some(([sx, , on]) => on === 1 && sx > 0.3 && sx < 0.7), `a half drag steered half (${JSON.stringify(steer.slice(0, 12))})`);
  assert(steer.some(([sx, , on]) => on === 1 && sx === 1), "the full drag steered full");
  assert.deepEqual(steer.at(-1), [0, 0, 0], "letting go centred the drag");
  for (const said of ["Armada: the drag steers", "Armada: the drag let go"]) {
    assert(logs.some((line) => line.includes(said)), `the emulator said "${said}"`);
  }
}

try {
  const host = await pilot("host", installer);
  await host.page.fill("#room", room);
  await host.page.check("#private");
  if (arena) await host.page.fill("#seats", "6");
  await host.page.click("#join");
  await host.page.waitForFunction(() => !document.querySelector("#lobby").hidden);
  const guest = await pilot("guest", guestFiles);
  for (const { page } of [host, guest]) {
    await page.waitForFunction(() => document.querySelectorAll("#roster .dot.up").length === 2);
  }
  await host.page.click("#fly");
  for (const [name, time] of Object.entries(checkpoints)) {
    await Promise.all([host, guest].map((p) => screenshotAt(p, name, time)));
  }
  if (!strategy) {
    await flyWithMouse(host);
    await guest.page.locator("#canvas").press("ArrowRight", { delay: 850 });
    await guest.page.locator("#canvas").press("Space", { delay: 850 });
    await guest.page.locator("#canvas").screenshot({ path: path.join(out, "guest-keyboard-flight.png") });
  }
  for (const [pilot, other] of [[host, "GUEST"], [guest, "HOST"]]) {
    assert(pilot.logs.some((line) => line.includes(`IPX: ${other} is on the network`)), `${pilot.who} saw the other IPX node`);
    const traffic = pilot.logs.filter((line) => /packets sent \d+, delivered \d+/.test(line));
    assert(traffic.length >= 2, `${pilot.who}: IPX traffic reports`);
    const last = /packets sent (\d+), delivered (\d+)/.exec(traffic.at(-1));
    const minimum = strategy ? 45 : 100;
    assert(Number(last[1]) > minimum && Number(last[2]) > minimum, `${pilot.who}: bidirectional match traffic (${traffic.at(-1)})`);
  }
  if (!strategy) {
    // (the guest's Right, held 850 ms: ramped from a ninth of the stop to the stop, as Wing Commander's arrows)
    const full = arena ? 20 : 30;
    const ramp = guest.logs.find((line) => /Armada: Right held [0-9.]+ s, \d+ frames, ramped/.test(line));
    assert(ramp, "the guest's Right was ramped");
    const [, first, last] = /ramped ([+-]\d+) to ([+-]\d+)/.exec(ramp);
    assert(Number(first) > 0 && Number(first) <= Math.ceil(full / 9) && Number(last) === full, `ramped from a ninth to the stop (${ramp})`);
  }
  console.log(`ARMADA PAGE OK (${mode}). Screenshots in ${out}`);
} finally {
  await browser.close();
}
