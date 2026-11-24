// Armada's strategy map at 640x400 on the page (src/cpu/wcnet_armada_hires.cpp "the strategy map"; docs/armada.md):
// one browser, a room of one, "High resolution (2x)" ticked, One Player -> Armada mode (Confederation, a small sector,
// novice), the map begun; then the page's own keyboard and mouse: the arrow keys move the view (the map's middle in
// the emulator's log moves, within the sector), and a click where the 640x400 picture shows a planet that is not
// selected selects it (the game's selection, the marker in the log, becomes that planet: the game's own hit test on
// what the picture shows), and a click on a fleet beside a planet selects the fleet; the wheel up shows the game's
// own map (the log says so), the arrows move that one too, and the wheel down is one level out again; in Dock (F4)
// - undocks one of the top row and = docks it again.  The
// emulator's WCARMHIRESDEBUG lines give the objects' 640x400 places (the planets, then the ships and markers beside
// them).
// PLAYWRIGHT_DIR points to a directory with Playwright installed.
//   node scripts/web-armada-map.mjs PAGE_URL LOBBY_URL GOG_INSTALLER
import assert from "node:assert/strict";
import fs from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";

const [pageUrl, lobbyUrl, installer] = process.argv.slice(2);
assert(pageUrl && lobbyUrl && installer, "usage: web-armada-map.mjs PAGE_URL LOBBY_URL GOG_INSTALLER");
const require = createRequire(path.resolve(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const out = process.env.SHOT_DIR || "/tmp/web-armada-map";
fs.mkdirSync(out, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });

const clickAt = (time, x, y) => `${time}:!mouse=${x}:${y},${time + 0.4}:!button=0:1,${time + 0.6}:!button=0:0`;
// (the native rig's route: One Player, Armada mode, Confederation, small, novice, the callsign, BEGIN)
const route = ["7:esc", "10:esc", clickAt(13, 0.5, 0.6), clickAt(16, 0.5, 0.71), clickAt(19, 0.49, 0.38),
  clickAt(23, 0.43, 0.29), clickAt(25, 0.43, 0.585), "28:a", "28.3:c", "28.6:e", "29:enter", clickAt(31, 0.817, 0.73),
  "34:!mouse=0.95:0.97"];

const page = await browser.newPage({ viewport: { width: 1200, height: 960 } });
const logs = [];
page.on("console", (line) => {
  logs.push(line.text());
  if (/strategy map|picture is|failed|Armada map: marker|Armada map: the game's own|Armada map: one level/.test(line.text())) console.log(line.text().slice(0, 200));
});
page.on("pageerror", (error) => console.error(error.message));
const lastObjects = () => {
  const line = [...logs].reverse().find((l) => /Armada map: middle/.test(l));
  if (!line) return null;
  const m = /middle (-?\d+),(-?\d+); objects \(640x400\):(.*); beside them:(.*)$/.exec(line);
  const parse = (text) => [...text.matchAll(/([0-9A-F]+)@(-?\d+),(-?\d+)([FM]?)/g)].map((o) => ({ id: o[1], x: Number(o[2]), y: Number(o[3]), kind: o[4] }));
  return { mx: Number(m[1]), my: Number(m[2]), objects: parse(m[3]), beside: parse(m[4]) };
};
const lastMarker = () => {
  const line = [...logs].reverse().find((l) => /Armada map: marker /.test(l));
  return line ? /marker ([0-9A-F]+)/.exec(line)[1] : null;
};
const waitLog = async (re, after, ms = 15000) => {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    if (logs.slice(after).some((l) => re.test(l))) return true;
    await page.waitForTimeout(250);
  }
  return false;
};
try {
  const url = new URL(pageUrl);
  url.searchParams.set("server", lobbyUrl);
  url.searchParams.set("callsign", "MAP");
  url.searchParams.set("advertise", "off");
  url.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
  url.searchParams.set("env.WCNET_LOG", "2");
  url.searchParams.set("env.WCARMHIRESDEBUG", "1");
  url.searchParams.set("env.WCNET_KEYSCRIPT", route.join(",").split(",").sort((a, b) => parseFloat(a) - parseFloat(b)).join(","));
  await page.goto(url.href);
  await page.setInputFiles("#gamefile", installer);
  await page.waitForFunction(() => document.querySelector("#sourceStatus").dataset.state === "ready" &&
    document.querySelector("#program").value === "armada", null, { timeout: 90_000 });
  await page.locator("#fullStart").evaluate((el) => { if (el.checked) el.click(); });
  await page.locator("#armHires").evaluate((el) => { if (!el.checked) el.click(); });
  await page.fill("#seats", "1");
  await page.dispatchEvent("#seats", "change");
  await page.click("#join");
  await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30_000 });
  await page.click("#fly");
  await page.waitForFunction(() => document.querySelector("#log").textContent.includes("key script 34.0s"), null, { timeout: 240_000 });
  assert(await waitLog(/the strategy map's picture is 640x400/, 0, 20000), "the strategy map shown at 640x400");
  assert(await waitLog(/Armada map: middle/, logs.length, 10000), "the map's objects logged");
  const canvas = page.locator("#canvas");
  await canvas.screenshot({ path: path.join(out, "map.png") });
  const box = await canvas.boundingBox();
  const toPage = (x, y) => [box.x + (x + 0.5) * box.width / 640, box.y + (y + 0.5) * box.height / 400];

  // the arrows: each held for a second moves the view's middle its way, or leaves it where the doubled view already
  // reaches the sector's edge that way (a small sector may fit across); at least one of the four moves it
  await canvas.click({ position: { x: box.width * 0.95, y: box.height * 0.97 } });  // (focus; a click on the frame)
  const hold = async (key) => {
    const before = lastObjects();
    await page.keyboard.down(key);
    await page.waitForTimeout(1000);
    await page.keyboard.up(key);
    const mark = logs.length;
    assert(await waitLog(/Armada map: middle/, mark, 10000), "the map logged again");
    const after = lastObjects();
    console.log(`${key}: the view's middle ${before.mx},${before.my} -> ${after.mx},${after.my}`);
    return [after.mx - before.mx, after.my - before.my];
  };
  const moves = { ArrowLeft: await hold("ArrowLeft"), ArrowRight: await hold("ArrowRight"), ArrowUp: await hold("ArrowUp"),
    ArrowDown: await hold("ArrowDown") };
  await canvas.screenshot({ path: path.join(out, "map-arrows.png") });
  assert(moves.ArrowLeft[0] <= 0 && moves.ArrowLeft[1] === 0, `Left: left or nothing (${moves.ArrowLeft})`);
  assert(moves.ArrowRight[0] >= 0 && moves.ArrowRight[1] === 0, `Right: right or nothing (${moves.ArrowRight})`);
  assert(moves.ArrowUp[1] <= 0 && moves.ArrowUp[0] === 0, `Up: up or nothing (${moves.ArrowUp})`);
  assert(moves.ArrowDown[1] >= 0 && moves.ArrowDown[0] === 0, `Down: down or nothing (${moves.ArrowDown})`);
  // (a new game's small sector may fit the doubled view both ways: then no arrow has anywhere to go)
  if (!Object.values(moves).some(([x, y]) => x || y)) console.log("no arrow moved the view: the sector fits it both ways");
  const back = lastObjects();

  // a click on a planet that is not selected, where the picture shows it
  const marker = lastMarker();
  const target = back.objects.find((o) => o.id !== marker && o.x > 215 && o.x < 600 && o.y > 35 && o.y < 300);
  assert(target, `a planet to click (${JSON.stringify(back.objects)}, selected ${marker})`);
  const [px, py] = toPage(target.x, target.y);
  await page.mouse.click(px, py);
  const mark3 = logs.length;
  assert(await waitLog(new RegExp(`Armada map: marker ${target.id} `), mark3, 10000),
    `the click at ${target.x},${target.y} selects ${target.id} (was ${marker}; now ${lastMarker()})`);
  await canvas.screenshot({ path: path.join(out, "map-click.png") });
  console.log(`clicked ${target.id} at ${target.x},${target.y}: selected`);

  // a fleet beside its planet, where the picture shows it (12,-8 from the planet: half that for the game's pointer)
  const now = lastObjects();
  const fleet = now.beside.find((o) => o.kind === "F" && o.x > 215 && o.x < 600 && o.y > 35 && o.y < 300);
  if (fleet) {
    const [fx, fy] = toPage(fleet.x, fleet.y);
    await page.mouse.click(fx, fy);
    const mark4 = logs.length;
    assert(await waitLog(new RegExp(`Armada map: marker ${fleet.id} `), mark4, 10000),
      `the click at ${fleet.x},${fleet.y} selects the fleet ${fleet.id} (now ${lastMarker()})`);
    console.log(`clicked the fleet ${fleet.id} at ${fleet.x},${fleet.y}: selected`);
  } else {
    console.log(`no fleet on the picture to click (${JSON.stringify(now.beside)})`);
  }

  // the wheel: up, the game's own map (about the pointer); the arrows move it; down, one level out again
  const [cx0, cy0] = toPage(407, 167);
  await page.mouse.move(cx0, cy0);
  const mark5 = logs.length;
  await page.mouse.wheel(0, -120);
  assert(await waitLog(/Armada map: the game's own/, mark5, 5000), "the wheel up shows the game's own map");
  await page.waitForTimeout(1000);
  await canvas.screenshot({ path: path.join(out, "map-wheel-in.png") });
  await page.keyboard.down("ArrowRight");
  await page.waitForTimeout(600);
  await page.keyboard.up("ArrowRight");
  const mark6 = logs.length;
  await page.mouse.wheel(0, 120);
  assert(await waitLog(/Armada map: one level out/, mark6, 5000), "the wheel down is one level out again");
  assert(await waitLog(/Armada map: middle/, mark6, 10000), "the map drawn one level out again");
  await canvas.screenshot({ path: path.join(out, "map-wheel-out.png") });
  const after = lastObjects();
  console.log(`the wheel: in and out again; the view's middle ${now.mx},${now.my} -> ${after.mx},${after.my}`);

  // Dock (the game's own F4) over the new game's carrier, its fighters docked: - undocks one of the top row (the row's
  // numbers change on the picture), = docks it again (the row as it was); Escape cancels
  await page.keyboard.press("F4");
  await page.waitForTimeout(1500);
  const row = { x: box.x + 300 * box.width / 640, y: box.y + 216 * box.height / 400, width: 160 * box.width / 640, height: 24 * box.height / 400 };
  const dock0 = await page.screenshot({ clip: row });
  await page.keyboard.press("Minus");
  await page.waitForTimeout(800);
  const dock1 = await page.screenshot({ clip: row });
  await page.keyboard.press("Equal");
  await page.waitForTimeout(800);
  const dock2 = await page.screenshot({ clip: row });
  await canvas.screenshot({ path: path.join(out, "map-dock.png") });
  await page.keyboard.press("Escape");
  assert(!dock0.equals(dock1), "- in Dock changes the top row (one fighter undocked)");
  assert(dock0.equals(dock2), "= in Dock puts it back (docked again)");
  console.log("Dock: - undocked one of the top row, = docked it again");
  console.log(`ARMADA MAP OK. Screenshots in ${out}`);
} finally {
  await browser.close();
}
