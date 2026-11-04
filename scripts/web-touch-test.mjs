// Real multi-touch against both pages' actual markup/styles and the shared controller, with host input recorded.
// Serve the repository: WEB_ROOT=$PWD python3 web/serve.py 8010
// PLAYWRIGHT_DIR=build-web/playwright node scripts/web-touch-test.mjs http://localhost:8010
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
const require = createRequire(path.resolve(process.env.PLAYWRIGHT_DIR || ".", "package.json"));
const { chromium, devices } = require("playwright");
const base = process.argv[2] || "http://localhost:8010";
const browser = await chromium.launch({ executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox"], headless: true });
const cases = [["portrait", 412, 915], ["small", 320, 568], ["landscape", 667, 375], ["fullscreen", 915, 412]];
try {
  for (const file of ["web/index.html", "retrovega/host/web/game.html"]) {
    for (const [name, width, height] of cases) {
      const context = await browser.newContext({ ...devices["Pixel 7"], viewport: { width, height } });
      const page = await context.newPage(), errors = [];
      page.on("pageerror", (e) => errors.push(e.message));
      const url = `${base}/${file}`;
      // Only replace the host application scripts; exercise the production DOM, CSS and touch module unchanged.
      await page.route(url, (route) => route.fulfill({ contentType: "text/html",
        body: readFileSync(file, "utf8").replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, "") }));
      await page.route("**/touch.css", (route) => route.fulfill({ contentType: "text/css", body: readFileSync("web/touch.css", "utf8") }));
      await page.goto(url);
      await page.evaluate(async () => {
        const screen = document.getElementById("screen");
        document.body.replaceChildren(screen);
        document.getElementById("cover")?.remove();
        const canvas = document.getElementById("canvas");
        window.test = { flying: false, keys: [], steering: [], canvasEvents: 0 };
        for (const ev of ["touchstart", "pointerdown", "mousedown"])
          canvas.addEventListener(ev, () => window.test.canvasEvents++);
        const { initTouch } = await import("/web/touch.js");
        window.test.controls = initTouch({
          log: () => {}, inFlight: () => window.test.flying,
          send: (key, down) => { window.test.keys.push([key, down]); return true; },
          steer: (x, y) => { window.test.steering.push([x, y]); return true; },
        });
        window.test.controls.show(true);
      });
      await page.locator("#canvas").tap({ position: { x: 150, y: 90 } });
      const roomEvents = await page.evaluate(() => window.test.canvasEvents);
      assert(roomEvents > 0, "room taps still reach the game");
      await page.evaluate(() => { window.test.flying = true; });
      await page.waitForFunction(() => !document.getElementById("touchMovement").hidden);
      if (name === "fullscreen") {
        await page.evaluate(() => {
          const b = document.createElement("button"); b.id = "testFull"; b.textContent = "Fullscreen";
          b.onclick = () => document.getElementById("screen").requestFullscreen(); document.body.prepend(b);
        });
        await page.locator("#testFull").tap();
        await page.waitForFunction(() => !!document.fullscreenElement);
      }
      await page.locator("#canvas").tap({ position: { x: 150, y: 90 } });
      assert.equal(await page.evaluate(() => window.test.canvasEvents), roomEvents, "cockpit taps are consumed");
      await page.locator("#touchFire").scrollIntoViewIfNeeded();
      const boxes = await page.locator("#touch button, #touchStick").evaluateAll((items) => items.map((b) => {
        const r = b.getBoundingClientRect(); return { id: b.id, x: r.x, y: r.y, w: r.width, h: r.height };
      }));
      for (const b of boxes) {
        assert(b.w >= 44 && b.h >= 44, `${b.id} is at least a 44px touch target`);
        assert(b.x >= 0 && b.x + b.w <= width + 1 && b.y >= 0 && b.y + b.h <= height + 1,
          `${b.id} fits ${width}x${height}: ${JSON.stringify(b)}`);
        for (const a of boxes.filter((a) => a.id < b.id))
          assert(!(a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h), `${a.id} overlaps ${b.id}`);
      }
      const pad = boxes.find((b) => b.id === "touchStick"), fire = boxes.find((b) => b.id === "touchFire");
      let left = { id: 1, x: pad.x + pad.w / 2 + pad.w * .25, y: pad.y + pad.h / 2 };
      let right = { id: 2, x: fire.x + fire.w / 2, y: fire.y + fire.h / 2 };
      const cdp = await context.newCDPSession(page);
      const touch = (type, touchPoints) => cdp.send("Input.dispatchTouchEvent", { type, touchPoints });
      const state = () => page.evaluate(() => ({ keys: window.test.keys, steering: window.test.steering.at(-1),
        firing: document.getElementById("touchFire").classList.contains("on") }));
      const clear = () => page.evaluate(() => { window.test.keys.length = 0; });
      await clear();
      await touch("touchStart", [left]);
      assert((await state()).steering[0] > .4, "left thumb turns right");
      assert.deepEqual((await state()).keys, [], "steering sends no fire key");
      await touch("touchStart", [left, right]);
      assert((await state()).firing, "right thumb fires while left steers");
      left = { ...left, x: pad.x + pad.w * .15, y: pad.y + pad.h * .2 };
      await touch("touchMove", [left, right]);
      assert((await state()).steering[0] < 0 && (await state()).steering[1] < 0, "turn and pitch while firing");
      // Captured Fire remains held when the thumb slides off, until the actual release.
      right = { ...right, x: fire.x - 10 };
      await touch("touchMove", [left, right]);
      assert((await state()).firing, "sliding off Fire keeps the trigger held");
      await touch("touchEnd", [right]);
      await page.waitForTimeout(150);
      assert(!(await state()).firing, "releasing Fire releases only the trigger");
      assert((await state()).steering[0] < 0, "the left thumb retains steering");
      await touch("touchEnd", []);
      assert.deepEqual((await state()).steering, [0, 0], "releasing the pad centres steering");
      assert.deepEqual((await state()).keys, [["space", 1], ["space", 0]]);
      for (const [id, key] of Object.entries({ touchAuto: "a", touchTgt: "t", touchMissile: "enter",
        touchComm: "c", touchNav: "n", touchWeapon: "w" })) {
        await clear(); await page.locator(`#${id}`).tap();
        assert.deepEqual((await state()).keys, [[key, 1], [key, 0]], `${id} sends ${key}`);
      }
      for (const [id, key] of Object.entries({ touchBurn: "tab", touchSlower: "minus", touchFaster: "equals" })) {
        await clear(); await page.locator(`#${id}`).tap();
        assert.deepEqual((await state()).keys, [[key, 1], [key, 0]], `${id} holds/releases ${key}`);
      }
      right = { ...right, x: fire.x + fire.w / 2 };
      await clear(); await touch("touchStart", [left, right]);
      await touch("touchCancel", []);
      assert.deepEqual((await state()).steering, [0, 0], "touch cancellation centres steering");
      assert.deepEqual((await state()).keys, [["space", 1], ["space", 0]], "cancellation releases Fire");
      await clear(); await touch("touchStart", [left, right]);
      await page.evaluate(() => window.dispatchEvent(new Event("blur")));
      assert.deepEqual((await state()).keys, [["space", 1], ["space", 0]], "lost focus releases Fire");
      assert.deepEqual((await state()).steering, [0, 0], "lost focus centres steering");
      await touch("touchEnd", []);
      await clear(); await touch("touchStart", [left, right]);
      await page.evaluate(() => { window.test.flying = false; });
      await page.waitForFunction(() => document.getElementById("touchMovement").hidden);
      assert.deepEqual((await state()).keys, [["space", 1], ["space", 0]], "leaving flight releases Fire");
      await touch("touchEnd", []);
      await page.locator("#touchKeys").tap();
      await page.keyboard.insertText("Kit 2");
      await page.waitForTimeout(100);
      assert(await page.locator("#touchKeys").evaluate((b) => b.classList.contains("on")), "Keys opens the text field");
      await page.locator("#touchKeys").tap();
      assert.deepEqual(errors, []);
      console.log(`ok ${file}: ${name}, layout, two thumbs, actions, cancellation, focus, room taps, keyboard`);
      await context.close();
    }
  }
} finally { await browser.close(); }
