// The TIE Fighter demo on a phone (docs/tiedemo.md section 9; the user, 2026-11-19: "On mobile turning joystick does
// nothing and there's no way to adjust lasers to increased power", then "the one button for laser power up/down for
// now. And then start with one tick up of charging in case they don't know"): one page as a Pixel 7, the demo dropped,
// a room of one flown (a phone's Players is 1).  Checks, each printed with OK or FAIL: the touch controls shown in
// flight, their Nav button the demo's "Lasers"; the flight begun with the lasers' recharge one step up (our craft's
// setting, the key script's !tdmcd: 3); the touch stick held up pitches the way the Down arrow does (a climb); held
// right it turns the nose, and let go the turn stops; the Lasers button a step further (4).  Our craft's nose from the key
// script's !tdmpose every half second (emulated time); the page's actions wait for those lines.  Ends "TDM TOUCH OK".
//
//   node scripts/web-tdm-touch.mjs PAGE_URL ZIP
//   env: PLAYWRIGHT_DIR, CHROME, SHOT_DIR
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, zip] = process.argv.slice(2);
if (!pageUrl || !zip) {
  console.error("usage: node web-tdm-touch.mjs PAGE_URL ZIP");
  process.exit(2);
}
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium, devices } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (ok, what) => { console.log(`${stamp()} ${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };
const context = await browser.newContext({ ...devices["Pixel 7"] });
await context.addInitScript(() => {
  Element.prototype.requestPointerLock = undefined;
  Element.prototype.requestFullscreen = undefined;
  Element.prototype.webkitRequestFullscreen = undefined;
});
const page = await context.newPage();
const logs = [];
page.on("console", (m) => {
  const t = m.text();
  logs.push(t);
  if (/touch screen|could not|Aborted|RuntimeError/i.test(t)) console.log(`${stamp()} ${t.slice(0, 200)}`);
});
page.on("pageerror", (e) => console.log(`${stamp()} PAGE ERROR: ${e.message}`));
// (the key script, emulated seconds from boot: on from the showroom, past the opening; the pose every half second;
// our craft's data at 19.5 and 32.5; the Down arrow held from 23 to 23.6)
const steps = ["3:g", "10:esc", "19.5:!tdmcd", "23:down*0.6", "32.5:!tdmcd"];
for (let t = 19; t <= 33; t += 0.5) steps.push(`${t}:!tdmpose`);
steps.sort((a, b) => parseFloat(a) - parseFloat(b));
const u = new URL(pageUrl);
u.searchParams.set("lobby", "off");
u.searchParams.set("env.WCNET_KEYSCRIPT", steps.join(","));
u.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
u.searchParams.set("env.WCNET_LOG", "2");
await page.goto(u.toString());
await page.setInputFiles("#gamefile", zip);
await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready"
  && document.getElementById("program").value === "tiedemo", null, { timeout: 120000 }).catch(() => {});
check(await page.inputValue("#seats") === "1", `a phone's Players is 1 (${await page.inputValue("#seats")}): a room of one, said to nobody`);
await page.click("#join");
await page.waitForTimeout(1500);
await page.click("#fly");

const pose = (t) => {
  const re = new RegExp(`key script ${t.toFixed(1)}s: TIE demo: pose: hpr (\\d+) (\\d+) (\\d+) nose (-?\\d+) (-?\\d+) (-?\\d+)`);
  for (const l of logs) { const m = re.exec(l); if (m) return { pitch: Number(m[2]), nose: [Number(m[4]), Number(m[5]), Number(m[6])] }; }
  return null;
};
const until = async (t) => {
  for (let i = 0; i < 900 && !pose(t); i++) await page.waitForTimeout(100);
  if (!pose(t)) {
    console.log(`${stamp()} no pose at ${t}s; the page's last lines:\n  ${logs.slice(-12).map((l) => l.slice(0, 160)).join("\n  ")}`);
    fs.writeFileSync(path.join(shotDir, "tdm-touch.log"), logs.join("\n") + "\n");
    await page.screenshot({ path: path.join(shotDir, "tdm-touch-failed.png") });
    await browser.close();
    console.log("TDM TOUCH FAILED (no poses)");
    process.exit(1);
  }
  return pose(t);
};
const degrees = (a, b) => {
  const n = (v) => Math.hypot(...v), d = (a.nose[0] * b.nose[0] + a.nose[1] * b.nose[1] + a.nose[2] * b.nose[2]) / (n(a.nose) * n(b.nose));
  return Math.acos(Math.max(-1, Math.min(1, d))) * 180 / Math.PI;
};
const wrap = (d) => ((d + 32768) & 0xFFFF) - 32768;      // (a 16-bit angle's change)
// our craft's laser recharge setting (CraftData +0xC9) from the key script's !tdmcd at t
const lasers = (t) => {
  const re = new RegExp(`key script ${t.toFixed(1)}s: TIE demo craft data: ([0-9a-f]+)`);
  for (const l of logs) { const m = re.exec(l); if (m) return parseInt(m[1].substr(2 * 0xC9, 2), 16); }
  return -1;
};

await until(19);
await page.waitForFunction(() => !document.getElementById("touchMovement").hidden, null, { timeout: 10000 }).catch(() => {});
check(await page.$eval("#touchMovement", (el) => !el.hidden), "the touch stick and the flight's buttons shown in flight");
check(await page.$eval("#touchNav", (el) => el.textContent) === "Lasers", `the Nav button is the demo's "${await page.$eval("#touchNav", (el) => el.textContent)}"`);
await page.locator("#touchFire").scrollIntoViewIfNeeded();
const pad = await page.locator("#touchStick").boundingBox();
const cdp = await context.newCDPSession(page);
const touch = (type, touchPoints) => cdp.send("Input.dispatchTouchEvent", { type, touchPoints });
const mid = { x: pad.x + pad.width / 2, y: pad.y + pad.height / 2 };

const p20 = await until(20);
check(lasers(19.5) === 3, `the flight begun with the lasers' recharge one step up (the setting ${lasers(19.5)}; 2 as the craft is made)`);
// (the pitch first, from level flight and gently, half the stick: after a banked turn the pitch angle's sign is the
// roll's to say)
await touch("touchStart", [{ x: mid.x, y: mid.y - pad.width * .18, id: 1 }]);
const p21 = await until(21);
await touch("touchEnd", []);
const up = wrap(p21.pitch - p20.pitch);
const p225 = await until(22.5), p245 = await until(24.5);     // (the Down arrow held from 23 to 23.6: no pose between)
const key = wrap(p245.pitch - p225.pitch);
check(Math.abs(up) > 300 && Math.abs(key) > 300 && Math.sign(up) === Math.sign(key),
  `the touch stick held up pitched the craft as the Down arrow does, a climb (the pitch ${up} and ${key} of 65536)`);
const p25 = await until(25);
await touch("touchStart", [{ x: pad.x + pad.width * .85, y: mid.y, id: 1 }]);
await until(25.5);
await page.screenshot({ path: path.join(shotDir, "tdm-touch-right.png") });
const p27 = await until(27);
await touch("touchEnd", []);
check(degrees(p25, p27) > 5, `the touch stick held right turned the nose ${degrees(p25, p27).toFixed(1)} degrees in 2 s`);
// (from 1.5 s after: this test lets go when it has read the 27 s pose, some tenths of a second of the game's time on)
const p285 = await until(28.5), p30 = await until(30);
check(degrees(p285, p30) < 1.5, `let go, the turn stopped (${degrees(p285, p30).toFixed(2)} degrees in 1.5 s)`);
await page.locator("#touchNav").tap();
await until(33);
check(lasers(32.5) === 4, `the Lasers button: the recharge a step further (the setting ${lasers(32.5)})`);
await page.screenshot({ path: path.join(shotDir, "tdm-touch-end.png") });
fs.writeFileSync(path.join(shotDir, "tdm-touch.log"), logs.join("\n") + "\n");
await browser.close();
console.log(failed ? `TDM TOUCH FAILED (${failed})` : "TDM TOUCH OK");
process.exit(failed ? 1 : 0);
