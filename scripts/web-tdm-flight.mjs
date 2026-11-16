// The TIE Fighter demo's flight controls in the browser (docs/tiedemo.md section 9; the user, 2026-11-15: "point to aim
// mouse mode should be default. They can select drag to aim just like on cd version. also joystick should be like cd
// version too"): one page with a stand-in gamepad, the demo dropped, a room of one flown.  Checks, each printed with OK
// or FAIL: the Flight choice shown with the demo and "point to aim" by default; the pad picked up by a button; in
// flight, the mouse pushed right (the key script's !mickey: headless Chrome has no pointer lock, and !mickey is the
// motion a captured mouse gives) turns the nose with the aim ring drawn right of the gunsight; the Flight choice
// "joystick" on the page: the pad's stick turns the nose (the page's wc_web_stick), the mouse steers nothing; "point to
// aim" again with the pad connected and at rest: the mouse turns the nose.  Our craft's nose from the key script's
// !tdmpose every half second (emulated time); the page's actions wait for those lines.  Ends "TDM FLIGHT OK".
//
//   node scripts/web-tdm-flight.mjs PAGE_URL ZIP
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, zip] = process.argv.slice(2);
if (!pageUrl || !zip) {
  console.error("usage: node web-tdm-flight.mjs PAGE_URL ZIP");
  process.exit(2);
}
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (ok, what) => { console.log(`${stamp()} ${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };
const context = await browser.newContext({ viewport: { width: 1280, height: 1100 } });
await context.addInitScript(() => {
  window.__pad = { id: "Mock Pad (STANDARD GAMEPAD)", index: 0, connected: true, mapping: "standard", timestamp: 0,
                   axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  navigator.getGamepads = () => [window.__pad];
});
const page = await context.newPage();
const logs = [];
page.on("console", (m) => {
  const t = m.text();
  logs.push(t);
  if (/Flight controls|flight controls|bank into|could not|Aborted|RuntimeError/i.test(t)) console.log(`${stamp()} ${t.slice(0, 200)}`);
});
page.on("pageerror", (e) => console.log(`${stamp()} PAGE ERROR: ${e.message}`));
const pad = {
  axis: (i, v) => page.evaluate(([i, v]) => { window.__pad.axes[i] = v; window.__pad.timestamp++; }, [i, v]),
  button: (i, v) => page.evaluate(([i, v]) => { window.__pad.buttons[i] = { pressed: v > 0.1, touched: v > 0, value: v }; window.__pad.timestamp++; }, [i, v]),
};
// (the key script, emulated seconds from boot: on from the showroom, past the opening; the pose every half second; the
// mouse pushed right at 20 and back at 23, again at 30 (joystick: nothing) and back at 31.5, again at 34, back at 36)
const steps = ["3:g", "10:esc", "20:!mickey=150:0", "23:!mickey=-150:0", "30:!mickey=150:0", "31.5:!mickey=-150:0",
               "34:!mickey=150:0", "36:!mickey=-150:0"];
for (let t = 19; t <= 40; t += 0.5) steps.push(`${t}:!tdmpose`);
steps.sort((a, b) => parseFloat(a) - parseFloat(b));     // (the key script takes its items in order: by time)
const u = new URL(pageUrl);
u.searchParams.set("lobby", "off");
u.searchParams.set("env.WCNET_KEYSCRIPT", steps.join(","));
u.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
u.searchParams.set("env.WCNET_LOG", "2");
await page.goto(u.toString());
await page.setInputFiles("#gamefile", zip);
await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready"
  && document.getElementById("program").value === "tiedemo", null, { timeout: 120000 }).catch(() => {});
check(await page.$eval("#tfFlightLabel", (el) => !el.hidden) && await page.inputValue("#tfFlight") === "pointer",
  `the Flight choice shown with the demo, "${await page.inputValue("#tfFlight")}" (point to aim) by default`);
check(/point to aim/.test(await page.$eval("#gameKeys", (el) => el.textContent)), "the demo's keys line tells of the Flight choice");
await pad.button(0, 1); await page.waitForTimeout(300); await pad.button(0, 0);
check((await page.inputValue("#padSelect")) === "0:Mock Pad (STANDARD GAMEPAD)", `a button press picks the pad up (${await page.inputValue("#padSelect")})`);
await page.fill("#seats", "1");
await page.dispatchEvent("#seats", "change");
await page.click("#join");
await page.waitForTimeout(1500);
await page.click("#fly");

// the poses, by their emulated second
const pose = (t) => {
  const re = new RegExp(`key script ${t.toFixed(1)}s: TIE demo: pose: .* nose (-?\\d+) (-?\\d+) (-?\\d+)`);
  for (const l of logs) { const m = re.exec(l); if (m) return [Number(m[1]), Number(m[2]), Number(m[3])]; }
  return null;
};
const until = async (t) => {
  for (let i = 0; i < 900 && !pose(t); i++) await page.waitForTimeout(100);
  if (!pose(t)) {
    console.log(`${stamp()} no pose at ${t}s; the page's last lines:\n  ${logs.slice(-12).map((l) => l.slice(0, 160)).join("\n  ")}`);
    fs.writeFileSync(path.join(shotDir, "tdm-flight.log"), logs.join("\n") + "\n");
    await browser.close();
    console.log("TDM FLIGHT FAILED (no poses)");
    process.exit(1);
  }
  return pose(t);
};
const degrees = (a, b) => {
  if (!a || !b) return -1;
  const n = (v) => Math.hypot(...v), d = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (n(a) * n(b));
  return Math.acos(Math.max(-1, Math.min(1, d))) * 180 / Math.PI;
};
// the ring: its pixels right of the gunsight's middle
const ringAt = async (name) => {
  const file = path.join(shotDir, `${name}.png`);
  await page.locator("#canvas").screenshot({ path: file });
  return page.evaluate(async (b64) => {
    const img = new Image();
    img.src = "data:image/png;base64," + b64;
    await img.decode();
    const c = document.createElement("canvas");
    c.width = img.width; c.height = img.height;
    const g = c.getContext("2d");
    g.drawImage(img, 0, 0);
    const d = g.getImageData(0, 0, c.width, c.height).data;
    let left = 0, right = 0;
    for (let y = 0; y < c.height; y++) for (let x = 0; x < c.width; x++) {
      const i = 4 * (y * c.width + x);
      // (the ring's amber, 255 176 16, or as the browser's surface shows it, red and blue swapped: 16 176 255)
      const amber = (a, b) => a > 230 && d[i + 1] > 150 && d[i + 1] < 200 && b < 40;
      if (amber(d[i], d[i + 2]) || amber(d[i + 2], d[i])) { if (x > c.width / 2) right++; else left++; }
    }
    return { left, right, w: c.width };
  }, fs.readFileSync(file).toString("base64"));
};

await until(19);
const centred = await ringAt("tdm-flight-centred");
const p20 = await until(20);
await until(21);
const pushed = await ringAt("tdm-flight-pushed");
const p23 = await until(23);
const aimTurn = degrees(p20, p23);
check(aimTurn > 5, `point to aim: the mouse pushed right turned the nose ${aimTurn.toFixed(1)} degrees in 3 s`);
check(pushed.right > 20 && pushed.right > 3 * pushed.left, `point to aim: the ring right of the gunsight (its pixels ${pushed.left} left, ${pushed.right} right; centred: ${centred.left}, ${centred.right})`);

await until(25);
await page.selectOption("#tfFlight", "joystick");
const p26 = await until(26);
await pad.axis(0, 0.7);
await until(28);
await pad.axis(0, 0);
const p285 = await until(28.5);
const padTurn = degrees(p26, p285);
check(padTurn > 5, `joystick: the pad's stick right 2 s turned the nose ${padTurn.toFixed(1)} degrees`);
const p305 = await until(30.5);
const p32 = await until(32);
const mouseInJoy = degrees(p305, p32);
check(mouseInJoy < 1.5, `joystick: the mouse pushed steered nothing (${mouseInJoy.toFixed(2)} degrees in 1.5 s)`);

await until(33);
await page.selectOption("#tfFlight", "pointer");
const p34 = await until(34);
const p36 = await until(36);
const aimAgain = degrees(p34, p36);
check(aimAgain > 4, `point to aim again, the pad at rest: the mouse turned the nose ${aimAgain.toFixed(1)} degrees in 2 s`);
check(logs.some((l) => /the bank into a turn at .* found/.test(l)), "the demo's bank into a turn found (point to aim leaves it out)");
fs.writeFileSync(path.join(shotDir, "tdm-flight.log"), logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `TDM FLIGHT FAILED (${failed})` : "TDM FLIGHT OK");
process.exit(failed ? 1 : 0);
