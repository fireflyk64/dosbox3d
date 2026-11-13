// The page's "Scan for joysticks" (web/usbpad.js): an Xbox 360-style pad that Chrome on a Mac does not
// list (a Logitech F310 with its switch on X), read by the page over WebUSB.  No game is launched: the
// controller panel is checked against scripts/usbpad-mock.js (a Mac, and a stand-in navigator.usb with
// the pad on it), and the pad's buttons and sticks against the standard layout, through the page's own
// mapping table and the module's Gamepads.
//
//   node scripts/web-usbpad.mjs PAGE_URL        (PAGE_URL: a server of web/dist, or the live page)
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";
const [pageUrl] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const MOCK = path.join(path.dirname(fileURLToPath(import.meta.url)), "usbpad-mock.js");
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome", args: ["--no-sandbox"] });
const results = [];
const check = (what, ok, detail) => { results.push(ok); console.log(`${ok ? "ok  " : "FAIL"} ${what}: ${JSON.stringify(detail)}`); };
const KEY = "4:Logitech Gamepad F310 (USB Vendor: 046d Product: c21d)";
const BUTTON_NAMES = ["A", "B", "X", "Y", "L1", "R1", "L2 (left trigger)", "R2 (right trigger)", "Back", "Start",
                      "L3 (left stick)", "R3 (right stick)", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Home"];
const AXIS_NAMES = ["Left stick, sideways", "Left stick, up/down", "Right stick, sideways", "Right stick, up/down"];
const ready = (page) => page.waitForFunction(() => document.querySelectorAll("#padSelect option").length >= 1 && document.getElementById("padHint").textContent.length > 0, null, { timeout: 60000 });

// Not a Mac (this machine's Chrome, which has WebUSB): no button.
{
  const context = await browser.newContext();
  const page = await context.newPage();
  page.on("pageerror", (e) => console.log(`PAGE ERROR: ${e.message}`));
  await page.goto(pageUrl); await ready(page);
  const info = await page.evaluate(() => ({ usb: !!navigator.usb, platform: navigator.platform, hidden: document.getElementById("padScan").hidden }));
  check("off a Mac the scan is not offered", info.hidden === true && info.usb === true, info);
  await context.close();
}

const context = await browser.newContext();
await context.addInitScript({ path: MOCK });
const page = await context.newPage();
page.on("pageerror", (e) => console.log(`PAGE ERROR: ${e.message}`));
const T = (f, arg) => page.evaluate(f, arg);
const state = () => page.textContent("#padScanState");
const usbPads = () => page.evaluate(async () => (await import("./usbpad.js")).usbPads().map((p) => ({ id: p.id, index: p.index, mapping: p.mapping,
  axes: p.axes.map((v) => Math.round(v * 10000) / 10000), pressed: p.buttons.map((b, i) => (b.pressed ? i : -1)).filter((i) => i >= 0),
  values: p.buttons.map((b) => Math.round(b.value * 1000) / 1000) })));
const scan = async () => { await page.click("#padScan"); await page.waitForFunction(() => !/^Scanning|^$/.test(document.getElementById("padScanState").textContent), null, { timeout: 10000 }); return state(); };
await page.goto(pageUrl); await ready(page);
await page.waitForTimeout(1500);
check("on a Mac the button is there", await page.isVisible("#padScan"), await page.textContent("#padScan"));
check("and nothing of WebUSB is used before it is pressed", (await T(() => window.__usb.calls)).length === 0, await T(() => window.__usb.calls));
await page.evaluate(() => { document.getElementById("padDetails").open = true; });
check("the hint names the scan", /Scan for joysticks/.test(await page.textContent("#padHint")), await page.textContent("#padHint"));

let msg = await scan();
const requests = await T(() => window.__usb.requests);
check("the scan asks the browser for an Xbox 360-style pad (class ff/5d/01)", requests.length === 1 &&
      requests[0].some((f) => f.classCode === 0xff && f.subclassCode === 0x5d && f.protocolCode === 0x01), requests);
check("it says what it found", /^Found Logitech Gamepad F310$/.test(msg), msg);
check("the pad is in the list and chosen", (await page.inputValue("#padSelect")) === KEY &&
      (await page.$$eval("#padSelect option", (os) => os.map((o) => o.textContent))).includes("5: Logitech Gamepad F310 (USB Vendor: 046d Product: c21d)"),
      await page.inputValue("#padSelect"));
check("its player light is set (01 03 06)", JSON.stringify(await T(() => window.__usb.outs)) === "[[1,3,6]]", await T(() => window.__usb.outs));
let pads = await usbPads();
check("the LED and status messages are not read as buttons: at rest nothing is pressed", pads.length === 1 && pads[0].mapping === "standard" &&
      pads[0].pressed.length === 0 && pads[0].axes.every((v) => v === 0), pads);

// Each button, and each stick, read where the standard layout has it.
const wrong = [];
for (let i = 0; i < 17; i++) {
  await T((i) => window.__usb.button(i, 1), i); await page.waitForTimeout(40);
  pads = await usbPads();
  if (JSON.stringify(pads[0].pressed) !== JSON.stringify([i]) || pads[0].values[i] !== 1) wrong.push([i, pads[0].pressed]);
  await T((i) => window.__usb.button(i, 0), i); await page.waitForTimeout(40);
}
check("every button is its standard number (A 0 ... Home 16)", wrong.length === 0, wrong);
await T(() => window.__usb.button(6, 20 / 255)); await page.waitForTimeout(40);
const light = (await usbPads())[0];
await T(() => window.__usb.button(6, 0));
check("a trigger touched (20 of 255) has its value and is not pressed", Math.abs(light.values[6] - 0.078) < 0.001 && light.pressed.length === 0, light.values[6]);
const sticks = [];
for (const [i, v] of [[0, 1], [0, -1], [1, 1], [1, -1], [2, 0.5], [3, -0.25]]) {
  await T(([i, v]) => window.__usb.axis(i, v), [i, v]); await page.waitForTimeout(40);
  sticks.push([i, v, (await usbPads())[0].axes[i]]);
  await T((i) => window.__usb.axis(i, 0), i);
}
check("the sticks: right and down positive, full travel 1", sticks.every(([, v, got]) => Math.abs(got - v) < 0.0002), sticks);

// The page's mapping table names them so (press while "Change" waits).
const bound = [];
for (const i of [2, 6, 14, 16]) {
  await page.click('#padRows button[data-button="target"]');
  await T((i) => window.__usb.button(i, 1), i); await page.waitForTimeout(150);
  await T((i) => window.__usb.button(i, 0), i); await page.waitForTimeout(100);
  bound.push(await page.$eval('#padRows button[data-button="target"]', (b) => b.closest("tr").cells[1].textContent));
}
for (const [i, v] of [[1, -0.9], [0, 0.9]]) {
  await page.click('#padRows button[data-button="target"]');
  await T(([i, v]) => window.__usb.axis(i, v), [i, v]); await page.waitForTimeout(150);
  await T((i) => window.__usb.axis(i, 0), i); await page.waitForTimeout(100);
  bound.push(await page.$eval('#padRows button[data-button="target"]', (b) => b.closest("tr").cells[1].textContent));
}
await page.click('#padRows button[data-axis="roll"]');
await T(() => window.__usb.axis(3, 0.9)); await page.waitForTimeout(150); await T(() => window.__usb.axis(3, 0)); await page.waitForTimeout(100);
bound.push(await page.$eval('#padRows button[data-axis="roll"]', (b) => b.closest("tr").cells[1].textContent));
check("the mapping table: X, L2, D-pad left, Home, the left stick up and right, the right stick up/down",
      JSON.stringify(bound) === JSON.stringify(["X", "L2 (left trigger)", "D-pad left", "Home", "Left stick, up/down −", "Left stick, sideways +", "Right stick, up/down"]), bound);
await page.click("#padReset");

// The scan again: the pad open is not opened twice, the chooser shows, cancelled.
await T(() => { window.__usb.chooser = "cancel"; });
msg = await scan();
check("a second scan, the chooser cancelled: nothing picked, the pad kept", msg === "No controller picked" && (await T(() => window.__usb.requests.length)) === 2 &&
      (await usbPads()).length === 1 && (await page.inputValue("#padSelect")) === KEY, [msg, await page.inputValue("#padSelect")]);

// Unplugged: gone from the list (the choice kept, "not connected").  Plugged back: nothing until a scan,
// which opens it without the chooser (the browser was allowed before) and it is chosen again.
await T(() => window.__usb.unplug()); await page.waitForTimeout(300);
let options = await page.$$eval("#padSelect option", (os) => os.map((o) => o.textContent));
check("unplugged it is not connected", (await usbPads()).length === 0 && options.some((t) => /F310.*\(not connected\)/.test(t)), options);
await T(() => window.__usb.plug()); await page.waitForTimeout(1000);
check("plugged back nothing opens it until a scan", (await usbPads()).length === 0, await usbPads());
msg = await scan();
check("the scan opens it without the chooser and it is the controller again", /^Found/.test(msg) && (await T(() => window.__usb.requests.length)) === 2 &&
      (await usbPads()).length === 1 && (await page.inputValue("#padSelect")) === KEY, [msg, await page.inputValue("#padSelect")]);

// A reload: the window's choice is kept but the pad is not opened until the button.
await page.reload(); await ready(page); await page.waitForTimeout(1500);
options = await page.$$eval("#padSelect option", (os) => os.map((o) => o.textContent));
check("after a reload nothing is touched and the pad waits for the scan", (await T(() => window.__usb.calls)).length === 0 &&
      options.some((t) => /F310.*\(not connected\)/.test(t)), [await T(() => window.__usb.calls), options]);
msg = await scan();
check("the scan brings it back without asking", /^Found/.test(msg) && (await T(() => window.__usb.requests.length)) === 0 && (await page.inputValue("#padSelect")) === KEY, msg);
await context.close();

// The pad held by another tab: said so, nothing listed.
{
  const context = await browser.newContext();
  await context.addInitScript({ path: MOCK });
  const page = await context.newPage();
  await page.goto(pageUrl); await ready(page);
  await page.evaluate(() => { window.__usb.claimFails = true; });
  await page.click("#padScan");
  await page.waitForFunction(() => !/^Scanning|^$/.test(document.getElementById("padScanState").textContent), null, { timeout: 10000 });
  const m = await page.textContent("#padScanState");
  const n = await page.evaluate(async () => (await import("./usbpad.js")).usbPads().length);
  check("a pad in use elsewhere: said so, not listed", /in use/.test(m) && n === 0 && (await page.inputValue("#padSelect")) !== KEY, m);
  await context.close();
}
await browser.close();
console.log(results.every(Boolean) ? "USBPAD OK" : "USBPAD FAILED");
process.exit(results.every(Boolean) ? 0 : 1);
