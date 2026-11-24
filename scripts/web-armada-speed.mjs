// Armada's battle at 640x400 in the browser, its speed close up (the user, 2026-11-24: "the battle was slow in the
// browser with netplay enabled"; "Games measure framerate on maximum frame time"): two pilots in one LobbyLink room,
// both with High resolution (2x) ticked, fly at each other in Battle mode and stop close, each when the other's ship is
// large in its view.  Then the host's work (the texture spans, the stars, the room's wait: src/cpu/wcnet_armada_hires.cpp
// "done here") is switched off and on again every 10 s (!armhost) and each pilot's frames timed (WCARMHIRESTIME: real
// milliseconds a frame, the most of each 2 s, and the emulated work).  The worst real frame of each half is printed.
// THROTTLE=<n>: each page's CPU n times slower from Fly on (Chrome's own throttling: a slower machine).
// PROFILE=1: Chrome's CPU profile of each page for 6 s in the second "the game's own" half and the second "done here"
// half: each function's own time, the top printed and all in SHOT_DIR/<who>-<half>.prof.txt.
// PLAYWRIGHT_DIR points to a directory with Playwright installed.  SHOT_DIR (default /tmp/web-armada-speed).
// node scripts/web-armada-speed.mjs PAGE_URL LOBBY_URL GOG_INSTALLER
import assert from "node:assert/strict";
import fs from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";

const [pageUrl, lobbyUrl, installer] = process.argv.slice(2);
assert(pageUrl && lobbyUrl && installer, "usage: web-armada-speed.mjs PAGE_URL LOBBY_URL GOG_INSTALLER");
const require = createRequire(path.resolve(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const out = process.env.SHOT_DIR || "/tmp/web-armada-speed";
fs.mkdirSync(out, { recursive: true });
const room = `ARM-speed-${Date.now()}`;
const browser = await chromium.launch({ executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });

function clickAt(time, x, y) {
  return `${time}:!mouse=${x}:${y},${time + 0.4}:!button=0:1,${time + 0.6}:!button=0:0`;
}
function typeAt(time, name) {
  return [...name, "enter"].map((key, i) => `${time + i * 0.3}:${key}`).join(",");
}
const entry = ["7:esc", "10:esc", clickAt(13, 0.5, 0.71), clickAt(17, 0.5, 0.615), "21:y"];
// (each stops when the other's ship is large in its 640x400 view: 10,000 texture pixels a frame, !armstopnear)
const routes = {
  host: [clickAt(65, 0.31, 0.475), "69:b", "70:enter", clickAt(92, 0.125, 0.475), "130:!armstopnear=10000"],
  guest: [clickAt(82, 0.5, 0.475), "130:!armstopnear=10000"],
};
// the host's work off and on (each half 10 s, timed in 2 s windows), the pictures at the close-up
const halves = [[196, 0], [206, 1], [216, 0], [226, 1]];
const end = 236;

async function pilot(who) {
  const page = await browser.newPage({ viewport: { width: 1200, height: 960 } });
  const logs = [];
  page.on("console", (line) => {
    logs.push(line.text());
    if (/lobby: joined|failed|spans, stars|stopped close|Armada: the (texture|stars|room)/.test(line.text())) console.log(`${who}: ${line.text()}`);
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
  url.searchParams.set("env.WCNET_ARM_FPS", "1");
  url.searchParams.set("env.WCARMHIRESTIME", "2000");
  const route = [...entry, typeAt(33, who), ...routes[who],
    clickAt(98, who === "host" ? 0.12 : 0.9, 0.13), clickAt(103, 0.16, 0.97), clickAt(112, 0.16, 0.91),
    "194:!frame=close", ...halves.map(([t, on]) => `${t}:!armhost=${on}`), `${end}:!frame=end`];
  url.searchParams.set("env.WCNET_KEYSCRIPT", route.join(",").split(",").sort((a, b) => parseFloat(a) - parseFloat(b)).join(","));
  await page.goto(url.href);
  await page.setInputFiles("#gamefile", installer);
  await page.waitForFunction(() => {
    const state = document.querySelector("#sourceStatus").dataset.state;
    return state === "error" || (state === "ready" && document.querySelector("#program").value === "armada");
  }, null, { timeout: 90_000 });
  assert.equal(await page.locator("#sourceStatus").getAttribute("data-state"), "ready", await page.locator("#sourceStatus").textContent());
  await page.locator("#fullStart").evaluate((el) => { if (el.checked) el.click(); });
  return { who, page, logs };
}

async function waitFrame({ who, page }, name, time) {
  const line = `key script ${time.toFixed(1)}s`;
  await page.waitForFunction(({ line, name }) => document.querySelector("#log").textContent.split("\n")
    .some((e) => e.includes(line) && e.includes(`/${name}.ppm`)), { line, name }, { timeout: 400_000 });
  await page.locator("#canvas").screenshot({ path: path.join(out, `${who}-${name}.png`) });
}

try {
  const host = await pilot("host");
  await host.page.fill("#room", room);
  await host.page.check("#private");
  await host.page.click("#join");
  await host.page.waitForFunction(() => !document.querySelector("#lobby").hidden);
  const guest = await pilot("guest");
  for (const { page } of [host, guest]) {
    await page.waitForFunction(() => document.querySelectorAll("#roster .dot.up").length === 2);
  }
  for (const { page } of [host, guest]) {
    assert.equal(await page.locator("#armHiresCockpit").count(), 0, "no Hi-res cockpit box");
    await page.check("#armHires");
  }
  await host.page.click("#fly");
  const throttle = Number(process.env.THROTTLE || 1);
  if (throttle > 1) {
    for (const { who, page } of [host, guest]) {
      const cdp = await page.context().newCDPSession(page);
      await cdp.send("Emulation.setCPUThrottlingRate", { rate: throttle });
      console.log(`${who}: the CPU ${throttle} times slower`);
    }
  }
  await Promise.all([host, guest].map((p) => waitFrame(p, "close", 194)));
  if (process.env.PROFILE === "1") {
    for (const [t, half] of [[216, "own"], [226, "here"]]) {
      await Promise.all([host, guest].map((p) => p.page.waitForFunction((line) => document.querySelector("#log").textContent
        .includes(line), `key script ${t.toFixed(1)}s`, { timeout: 600_000 })));
      await host.page.waitForTimeout(1500);
      const cdps = await Promise.all([host, guest].map(async (p) => {
        const cdp = await p.page.context().newCDPSession(p.page);
        await cdp.send("Profiler.enable");
        await cdp.send("Profiler.setSamplingInterval", { interval: 500 });
        await cdp.send("Profiler.start");
        return cdp;
      }));
      await host.page.waitForTimeout(6000);
      for (const [i, p] of [host, guest].entries()) {
        const { profile } = await cdps[i].send("Profiler.stop");
        const self = new Map(), byId = new Map(profile.nodes.map((n) => [n.id, n]));
        let total = 0;
        profile.samples.forEach((id, k) => {
          const dt = (profile.timeDeltas[k] || 0) / 1000;
          const f = byId.get(id).callFrame, name = f.functionName || `(${f.url.split("/").pop()})`;
          self.set(name, (self.get(name) || 0) + dt);
          total += dt;
        });
        const top = [...self.entries()].sort((a, b) => b[1] - a[1]);
        fs.writeFileSync(path.join(out, `${p.who}-${half}.prof.txt`), top.map(([n, ms]) => `${ms.toFixed(1)}\t${(100 * ms / total).toFixed(1)}%\t${n}`).join("\n") + "\n");
        console.log(`${p.who} ${half}: ${top.slice(0, 14).map(([n, ms]) => `${n} ${(100 * ms / total).toFixed(1)}%`).join(", ")}`);
      }
    }
  }
  await Promise.all([host, guest].map((p) => waitFrame(p, "end", end)));
  for (const p of [host, guest]) {
    fs.writeFileSync(path.join(out, `${p.who}.log`), p.logs.join("\n") + "\n");
    assert(p.logs.some((l) => l.includes("Armada: the first frame at 640x400")), `${p.who}: flew at 640x400`);
    // each 2 s window's line, in the half it fell in (the first window after each switch straddles it: dropped)
    let on = -1, fresh = false;
    const by = { 0: [], 1: [] };
    for (const l of p.logs) {
      const sw = /the spans, stars and wait (the game's own|done here)/.exec(l);
      if (sw) { on = sw[1] === "done here" ? 1 : 0; fresh = true; continue; }
      const m = /Armada time: (\d+) frames, ([0-9.]+) ms a frame .*work ([0-9.]+) ms \(most ([0-9.]+), ([0-9.]+)M.*real ([0-9.]+) ms a frame \(most ([0-9.]+)\)/.exec(l);
      if (!m || on < 0) continue;
      if (fresh) { fresh = false; continue; }
      by[on].push(m.slice(1).map(Number));
    }
    for (const [half, name] of [[0, "the game's own"], [1, "done here"]]) {
      const w = by[half];
      if (!w.length) continue;
      const real = Math.max(...w.map((x) => x[6])), mean = w.reduce((a, x) => a + x[5], 0) / w.length;
      const work = Math.max(...w.map((x) => x[3])), instr = Math.max(...w.map((x) => x[4]));
      console.log(`${p.who}: ${name}: real ${mean.toFixed(1)} ms a frame (${(1000 / mean).toFixed(1)} fps), the worst ${real.toFixed(1)} ms; ` +
        `the most emulated work ${work.toFixed(1)} ms (${instr.toFixed(2)}M instructions); ${w.length} windows`);
    }
  }
  console.log(`ARMADA SPEED DONE. Screenshots in ${out}`);
} finally {
  await browser.close();
}
