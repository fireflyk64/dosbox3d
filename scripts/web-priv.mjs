// Privateer in the browser, a room of one (scripts/web-priv.sh starts the page's server and a local lobby): GOG's
// installer dropped into the page, the game found and started, scripts/privrun.sh's route into flight from Achilles
// (as a key script), then the page's own keyboard: Right held 0.6 s, a tap, and the emulator's log of the ramp
// (src/cpu/wcnet_priv_controls.cpp); pictures in SHOT_DIR; the saved-game panel.  Prints "ok"/"FAIL" lines and
// "PRIV OK" when every check passed.
//
//   PLAYWRIGHT_DIR=... SHOT_DIR=... node scripts/web-priv.mjs PAGE_URL LOBBY_URL INSTALLER [seconds]
//   RF=1: Righteous Fire (the menu's other program)
//   HIRES=1: the page's "High resolution (2x)" (ticked by default) left ticked: 640 x 400 in flight, the frame rate
//   held, ` cycles it (all, the space alone, off), Hi-res cockpit unticked and ticked; HIRES=0: unticked before Fly, the game's own picture and none of the high resolution's hooks
//   CLASSIC=1: the page opened with ?privclassic=1: the box unticked and greyed, the game as it was (WCPRIVCLASSIC)
//   ROOM=<code>: the page joins that room as a guest (scripts/web-priv.sh ROOM=1 makes a native host's) and checks the
//   room's presence lines (src/cpu/wcnet_priv_session.cpp) instead of flying alone
//   PAD=1: a stand-in gamepad (scripts/web-pad.mjs's) picked in flight, its stick pushed right: the page's stick steers
//   (src/cpu/wcnet_priv_controls.cpp, priv_controls_join); the Controller list's two "no controller" entries (the
//   pointer shown or not) told to the emulator.  PAD=builtin: the pad picked "Built in" before the launch: the game's
//   own joystick on the port, its mode picked, the stick read by the game
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, installer, secondsArg = "60"] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl || !installer) {
  console.error("usage: node web-priv.mjs PAGE_URL LOBBY_URL INSTALLER [seconds]");
  process.exit(2);
}
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const shotDir = process.env.SHOT_DIR || ".";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const page = await browser.newPage({ viewport: { width: 1280, height: 1000 } });
const PAD = process.env.PAD || "";
const PADKEY = "0:Mock Pad (STANDARD GAMEPAD)";
if (PAD) {
  await page.addInitScript(([builtin, key]) => {
    window.__pad = { id: "Mock Pad (STANDARD GAMEPAD)", index: 0, connected: true, mapping: "standard", timestamp: 0,
                     axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
    navigator.getGamepads = () => [window.__pad];
    if (builtin) sessionStorage.setItem("wcpad:choice", "joy|" + key);
  }, [PAD === "builtin", PADKEY]);
}
// SAVES=1: twelve save games kept in this browser before the page loads (the user, 2026-11-23: "I can only download 8
// of my save games"): the list shows them newest first, the download is one zip of all of them, the zip restores
if (process.env.SAVES) {
  await page.addInitScript(() => {
    if (localStorage.getItem("wcsave-test-seeded")) return;
    localStorage.setItem("wcsave-test-seeded", "1");
    for (let i = 0; i < 12; i++) {
      const bytes = new Uint8Array(2000 + i).map((_, k) => (k * 13 + i) & 255);
      let bin = "";
      for (const b of bytes) bin += String.fromCharCode(b);
      localStorage.setItem(`wcsave:priv:TEST${String(i).padStart(2, "0")}.SAV`, JSON.stringify({ when: Date.now() - i * 3600e3, b64: btoa(bin) }));
    }
  });
}
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
const logs = [];
page.on("console", (m) => {
  const t = m.text();
  logs.push(`${stamp()} ${t}`);
  if (/Privateer|could not|Aborted|RuntimeError|save games|comms:/.test(t) && !/arrows [+-]/.test(t)) console.log(`${stamp()} [page] ${t.slice(0, 200)}`);
});
page.on("pageerror", (e) => console.log(`${stamp()} PAGE ERROR: ${e.message}`));
page.on("crash", () => console.log(`${stamp()} PAGE CRASHED`));
let failed = 0;
const check = (what, ok, detail) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail === undefined ? "" : ": " + (typeof detail === "string" ? detail : JSON.stringify(detail))}`);
  if (!ok) failed++;
};
const want = process.env.RF ? "privrf" : "priv";
// (the route of scripts/privrun.sh: Esc through the opening, NEW, a pilot "joe" / "ace", the Tarsus clicked on the pad)
const route = "4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter," +
  "15:!mouse=0.45:0.6,16:!button=0:1,16.3:!button=0:0";
const u = new URL(pageUrl);
u.searchParams.set("server", lobbyUrl);
u.searchParams.set("hall", "PRVHALL-" + Math.random().toString(36).slice(2, 6).toUpperCase());
u.searchParams.set("callsign", "PRIVTEST");
if (process.env.ROOM) u.searchParams.set("room", process.env.ROOM);
if (process.env.CLASSIC) u.searchParams.set("privclassic", "1");
if (process.env.RENDERER) u.searchParams.set("renderer", process.env.RENDERER);     // (?renderer=software: 2D canvas)
u.searchParams.set("env.WCNET_KEYSCRIPT", route);
u.searchParams.set("env.WCNET_KEYSCRIPT_FROM", "boot");
u.searchParams.set("env.WCNET_LOG", "3");
await page.goto(u.toString());
await page.setInputFiles("#gamefile", installer);
await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "error"
  || (document.getElementById("sourceStatus").dataset.state === "ready" && /^priv/.test(document.getElementById("program").value)), null, { timeout: 600000 });
const state = await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 140)}`);
check("the installer unpacked and the game recognised", state.startsWith("ready"), state);
const programs = await page.$$eval("#program option", (os) => os.map((o) => o.value));
check("the menu has Privateer and Righteous Fire", programs.includes("priv") && programs.includes("privrf"), programs);
if (want !== "priv") {
  await page.selectOption("#program", want);
  await sleep(500);
}
const keys = await page.$eval("#gameKeys", (el) => (el.hidden ? "" : el.textContent));
check("the flight's keys under the game", /Privateer in flight/.test(keys) || want === "privrf", keys.slice(0, 60));
const saves = await page.$eval("#saves", (el) => (el.hidden ? "hidden" : document.getElementById("saveStatus").textContent));
check("the saved-game panel", /save in the game|keeps your save games/.test(saves), saves);
await page.screenshot({ path: path.join(shotDir, "web-priv-page.png"), fullPage: true });
if (process.env.SAVES) {
  const listed = await page.$$eval("#saveList li", (li) => li.map((e) => e.textContent));
  const test = listed.filter((t) => /^TEST\d\d\.SAV/.test(t)).map((t) => t.slice(0, 10));
  check("the saves listed, newest first", test.length === 12 && test.join() === [...Array(12).keys()].map((i) => `TEST${String(i).padStart(2, "0")}.SAV`).join(), test.join(" "));
  const [dl] = await Promise.all([page.waitForEvent("download"), page.click("#saveDownload")]);
  const zipPath = path.join(shotDir, "saves.zip");
  await dl.saveAs(zipPath);
  const zip = fs.readFileSync(zipPath);
  const names = [];
  for (let i = 0; i + 30 < zip.length; i++) {
    if (zip.readUInt32LE(i) === 0x04034B50) { const n = zip.readUInt16LE(i + 26); names.push(zip.subarray(i + 30, i + 30 + n).toString()); }
  }
  check("one download: a zip of every save game", /\.zip$/.test(dl.suggestedFilename()) && names.filter((n) => /^TEST/.test(n)).length === 12, `${dl.suggestedFilename()}: ${names.length} files`);
  await page.evaluate(() => { for (let i = 0; i < 12; i++) localStorage.removeItem(`wcsave:priv:TEST${String(i).padStart(2, "0")}.SAV`); });
  const mark = logs.length;
  await page.setInputFiles("#saveFile", zipPath);
  await sleep(1500);
  const restored = logs.slice(mark).filter((l) => /save games: restored TEST\d\d\.SAV/.test(l)).length;
  const back = await page.evaluate(() => Object.keys(localStorage).filter((k) => /^wcsave:priv:TEST/.test(k)).length);
  check("the zip restores every save game", restored === 12 && back === 12, `${restored} restored, ${back} kept`);
}
if (process.env.ROOM) {
  // the room's link joined it on load: Fly once the page lets a guest (a native host never says it flies: 5 s)
  await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 60000 });
  console.log(`${stamp()} in the room: ` + await page.$eval("#status", (el) => el.textContent.trim().slice(0, 140)));
  const max = await page.$eval("#seats", (el) => el.max);
  check("the Players box takes 32 for Privateer (open rooms)", max === "32", max);
  // the Face box seen in a room (it sat in the room's row of choices, hidden for a game the room does not fly: the user,
  // 2026-11-09, "I also don't see an option to select comm portrait")
  const face = await page.evaluate(() => { const el = document.getElementById("privFace"); return el && el.offsetParent !== null ? el.options.length : 0; });
  check("the Face box shows in the room, with its 8 faces", face === 8, face);
} else if (process.env.SEATS) {
  // a room of SEATS made by this page, its host, alone in it (the lobby server's own limit on a room's seats)
  await page.fill("#seats", process.env.SEATS);
  await page.dispatchEvent("#seats", "change");
  await page.click("#join");
  await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
  console.log(`${stamp()} in the room: ` + await page.$eval("#status", (el) => el.textContent.trim().slice(0, 140)));
  const face = await page.evaluate(() => { const el = document.getElementById("privFace"); return el && el.offsetParent !== null ? el.options.length : 0; });
  check("the Face box shows in the room, with its 8 faces", face === 8, face);
} else {
  // a room of one (Players 1): the game alone, as it is
  await page.fill("#seats", "1");
  await page.dispatchEvent("#seats", "change");
  await page.click("#join");
  await page.waitForFunction(() => !document.getElementById("lobby").hidden && !document.getElementById("fly").disabled, null, { timeout: 30000 });
}
if (process.env.CLASSIC) {
  const box = await page.evaluate(() => { const el = document.getElementById("privHires"); return !!el && el.disabled && !el.checked; });
  check("?privclassic=1: the High resolution box unticked and greyed", box);
} else {
  const box = await page.evaluate(() => { const el = document.getElementById("privHires"); return !!el && el.offsetParent !== null && el.checked && el.type === "checkbox"; });
  check("the High resolution checkbox shows, ticked by default", box);
  check("the Hi-res cockpit checkbox shows, ticked by default",
    await page.evaluate(() => { const el = document.getElementById("privHiresCockpit"); return !!el && el.offsetParent !== null && el.checked && !el.disabled; }));
  if (process.env.HIRES === "0") await page.uncheck("#privHires");
}
// the mouse's pitch (the user, 2026-11-22: "The mouse is inverted. Is there a way to change it back. An option or toggle")
check("the Invert mouse checkbox shows, unticked by default",
  await page.evaluate(() => { const el = document.getElementById("privMouseInvert"); return !!el && el.offsetParent !== null && !el.checked && !el.disabled; }));
await page.click("#fly");
const waitLog = async (re, ms) => {
  const until = Date.now() + ms;
  while (Date.now() < until) {
    if (logs.some((l) => re.test(l))) return true;
    await sleep(500);
  }
  return false;
};
// the music made the Sound Blaster's FM before the launch (GOG's SOUND.CFG asks for General MIDI: none in the browser; the
// user, 2026-11-09: "also zero music here")
check("Privateer's music on the Sound Blaster's FM", logs.some((l) => /Privateer's music: .* changed to 2/.test(l)) || await waitLog(/Privateer's music: .* changed to 2/, 5000),
  (logs.find((l) => /Privateer's music/.test(l)) || "no word of the music").slice(0, 140));
check("PRCD.EXE found by the layer", await waitLog(/Privateer.*PRCD\.EXE loaded at/, 60000),
  (logs.find((l) => /PRCD\.EXE loaded/.test(l)) || "").slice(0, 120));
// (a pad built in at the launch: the game itself takes the joystick's mode when it starts and finds one on the port)
const mode = PAD === "builtin" ? "joystick" : "keyboard";
const flying = await waitLog(new RegExp(`keyboard frames, controls: ${mode}`), 90000);
check(`in flight, the ${mode}'s controls read every frame`, flying, (logs.filter((l) => /keyboard frames/.test(l)).pop() || "").slice(0, 120));
await sleep(3000);
const box = await page.$eval("#canvas", (c) => { const b = c.getBoundingClientRect(); return { x: b.x, y: b.y, width: b.width, height: b.height }; });
await page.screenshot({ path: path.join(shotDir, "web-priv-flight.png"), clip: box });
// the game's pixels kept square, 16:10 (the user, 2026-11-22, after a day of 4:3: "it looks worse because of the 4:3
// stretch. We should keep pixel ratios"); full screen, as large as the screen takes (the element the screen, the
// picture contained in it)
// (the picture as shown: Fly goes full screen when "start in full screen" is ticked, the default, and there the canvas is
// the whole screen with the picture contained in it)
const shown = await page.$eval("#canvas", (c) => {
  const b = c.getBoundingClientRect();
  const k = getComputedStyle(c).objectFit === "contain" ? Math.min(b.width / c.width, b.height / c.height) : 0;
  return k ? { width: c.width * k, height: c.height * k } : { width: b.width, height: b.height };
});
check("the picture keeps its pixels: 16:10", Math.abs(shown.width / shown.height - 16 / 10) < 0.01, `${Math.round(shown.width)} x ${Math.round(shown.height)}`);
await page.evaluate(() => document.getElementById("screen").requestFullscreen());
await sleep(1500);
const full = await page.$eval("#canvas", (c) => { const b = c.getBoundingClientRect(); return { width: b.width, height: b.height, vw: innerWidth, vh: innerHeight, fit: getComputedStyle(c).objectFit, w: c.width, h: c.height }; });
check("full screen: the whole screen, the picture contained at 16:10",
  Math.abs(full.width - full.vw) < 2 && Math.abs(full.height - full.vh) < 2 && full.fit === "contain" && full.w * 10 === full.h * 16,
  `${Math.round(full.width)} x ${Math.round(full.height)} on ${full.vw} x ${full.vh}, ${full.fit}, ${full.w} x ${full.h}`);
await page.screenshot({ path: path.join(shotDir, "web-priv-full.png") });
await page.evaluate(() => document.exitFullscreen());
await sleep(1000);
if (process.env.CLASSIC) {
  const size = await page.$eval("#canvas", (c) => `${c.width} x ${c.height}`);
  check("classic: the game's own picture", size === "640 x 400", size);
  check("classic: the game as it was", logs.some((l) => /the game as it was \(WCPRIVCLASSIC\)/.test(l)));
  check("classic: none of the high resolution's places watched", !logs.some((l) => /high-resolution picture's places watched/.test(l)));
  check("classic: the game's own poll of the game port", !logs.some((l) => /no stick at the start/.test(l)));
}
if (process.env.HIRES === "0") {
  const size = await page.$eval("#canvas", (c) => `${c.width} x ${c.height}`);
  check("unticked: the game's own picture", size === "640 x 400", size);
  check("unticked: none of the high resolution's places watched", !logs.some((l) => /high-resolution picture's places watched/.test(l)));
}
if (process.env.HIRES && process.env.HIRES !== "0") {
  const size = await page.$eval("#canvas", (c) => `${c.width} x ${c.height}`);
  check("the emulator's picture is 640 x 400 (each game pixel 2 x 2)", size === "640 x 400" && logs.some((l) => /the picture is 640 x 400/.test(l)), size);
  await sleep(6000);
  const fps = logs.map((l) => /Privateer: t=[\d.]+, (\d+) \w+ frames/.exec(l)).filter(Boolean).map((m) => +m[1]).slice(-5);
  check("28 frames a second with it", fps.length > 0 && fps.reduce((a, b) => a + b, 0) / fps.length >= 27.5, fps.join(" "));
  const at = await page.$eval("#canvas", (c) => { const b = c.getBoundingClientRect(); return { x: b.x, y: b.y, width: b.width, height: b.height }; });
  await page.screenshot({ path: path.join(shotDir, "web-priv-hires.png"), clip: at });
  await page.focus("#canvas");
  // the ` key cycles the three: all, the space alone, off, all
  const since = async (mark, re, ms) => {
    for (let waited = 0; waited < ms; waited += 250) {
      if (logs.slice(mark).some((l) => re.test(l))) return true;
      await sleep(250);
    }
    return false;
  };
  for (const [want, shot] of [["on in space alone", "space"], ["off", "off"], ["on", ""]]) {
    const mark = logs.length;
    await page.keyboard.down("Backquote");
    await sleep(200);
    await page.keyboard.up("Backquote");
    check(`the backquote key: ${want}`, await since(mark, new RegExp(`high-resolution picture ${want === "on" ? "on(?! in)" : want}`), 5000));
    await sleep(700);
    if (shot) await page.screenshot({ path: path.join(shotDir, `web-priv-hires-${shot}.png`), clip: at });
  }
  // the third mode by the page's box: Hi-res cockpit unticked, then ticked again, while flying
  for (const [tick, want] of [[false, "on in space alone"], [true, "on"]]) {
    const mark = logs.length;
    if (tick) await page.check("#privHiresCockpit"); else await page.uncheck("#privHiresCockpit");
    check(`Hi-res cockpit ${tick ? "ticked" : "unticked"} while flying: ${want}`,
      await since(mark, new RegExp(`high-resolution picture ${want} \\(the page's\\)`), 5000)
        && await page.evaluate(() => localStorage.getItem("wc:privHiresCockpit")) === (tick ? "1" : "0"));
  }
  await page.focus("#canvas");
}
// Invert mouse: the launch's (unticked: pushed up climbs), then ticked and unticked while flying
check("the mouse pushed up climbs (the launch's choice)", logs.some((l) => /the mouse pushed up raises the nose/.test(l)),
  (logs.find((l) => /Privateer: the mouse (pushed|inverted)/.test(l)) || "no word of it").slice(0, 120));
for (const [tick, re] of [[true, /the mouse inverted, as the game has it/], [false, /the mouse pushed up raises the nose/]]) {
  const mark = logs.length;
  if (tick) await page.check("#privMouseInvert"); else await page.uncheck("#privMouseInvert");
  await sleep(500);
  check(`Invert mouse ${tick ? "ticked" : "unticked"} while flying: ${tick ? "the game's own" : "pushed up climbs"}`,
    logs.slice(mark).some((l) => re.test(l)) && await page.evaluate(() => localStorage.getItem("wc:privMouseInvert")) === (tick ? "1" : "0"));
}
// the page's own keyboard: Right held until the stop is reached (at most 4 s: a fixed 0.6 s failed twice when the
// browser stalled for a second and a half after full screen, the game's own 28 frames a second either side), then a tap
await page.focus("#canvas");
const before = logs.length;
await page.keyboard.down("ArrowRight");
for (let held = 0; held < 4000; held += 50) {
  if (held >= 600 && logs.slice(before).filter((l) => /arrows [+-]\d+ -256/.test(l)).length >= 3) break;
  await sleep(50);
}
await page.keyboard.up("ArrowRight");
await sleep(1500);
await page.keyboard.down("ArrowRight");
await sleep(60);
await page.keyboard.up("ArrowRight");
await sleep(1500);
const ramp = logs.slice(before).filter((l) => /arrows [+-]/.test(l)).map((l) => Number(/arrows [+-]\d+ ([+-]\d+)/.exec(l)[1]));
if (mode === "keyboard") {     // (the joystick's mode reads no arrows: the game's own)
  check("the arrow ramped from a ninth of the stop", ramp.length > 0 && ramp[0] === -28, ramp.slice(0, 12).join(" "));
  check("... to the stop when held", ramp.includes(-256), `${ramp.filter((v) => v === -256).length} frames at the stop`);
}
await page.screenshot({ path: path.join(shotDir, "web-priv-turned.png"), clip: box });
if (PAD) {
  // the user, 2026-11-21: a pad in Privateer was "3 bad options" (inert, the blue dot driven, a spin); and the pointer
  // choice as two entries of the Controller list
  const opts = await page.$$eval("#padSelect option", (os) => os.map((o) => o.textContent));
  check("the Controller list: Kbd + mouse (shown), Keyboard + mouse, the pad, the pad built in",
    opts[0] === "Kbd + mouse (shown)" && opts[1] === "Keyboard + mouse" && opts.some((o) => /^1: Mock Pad/.test(o)) && opts.some((o) => /^Built in: 1: Mock Pad/.test(o)), opts.join(" | "));
  check("no Pointer menu of its own", !(await page.$("#privPointer")), "");
  if (PAD === "builtin") {
    check("the pad on the game port", logs.some((l) => /joystick: on the game port/.test(l)), (logs.find((l) => /joystick:/.test(l)) || "").slice(0, 120));
  } else {
    await page.selectOption("#padSelect", PADKEY);
  }
  await sleep(500);
  const mark = logs.length;
  await page.evaluate(() => { window.__pad.axes[0] = 0.6; });
  await sleep(1500);
  await page.evaluate(() => { window.__pad.axes[0] = 0; });
  await sleep(1000);
  const steer = logs.slice(mark).filter((l) => /steered by the page's stick|the joystick \(built in\)/.test(l));
  const yaws = steer.map((l) => Number(/yaw ([+-]\d+)/.exec(l)[1]));
  check(PAD === "builtin" ? "the stick right: the game's joystick turns right (yaw negative)" : "the stick right: the page's stick turns right (yaw negative)",
    yaws.some((v) => v < -60), (steer[0] || "nothing steered").slice(0, 140) + ` (${yaws.join(" ")})`);
  const rest = logs.length;
  await sleep(1500);
  const moving = logs.slice(rest).filter((l) => /steered by the page's stick|the joystick \(built in\)/.test(l));
  check("the stick let go: it stops", moving.length === 0, (moving[0] || "still").slice(0, 140));
  if (PAD === "builtin") {
    // (the page's JOYA.DAT: no dead band of the game's -- GOG's had 45..83 around 64, nothing until a third of the
    // stick -- and the stick off the port's centre by a read at least: a little past the page's 4% turns a little)
    check("JOYA.DAT written for the page's port", logs.some((l) => /JOYA\.DAT: the built-in stick calibrated/.test(l)), "");
    const little = logs.length;
    await page.evaluate(() => { window.__pad.axes[0] = 0.08; });
    await sleep(1500);
    await page.evaluate(() => { window.__pad.axes[0] = 0; });
    await sleep(1000);
    const small = logs.slice(little).filter((l) => /the joystick \(built in\)/.test(l)).map((l) => Number(/yaw ([+-]\d+)/.exec(l)[1]));
    check("a little stick (8%) turns a little: no dead band of the game's", small.length > 0 && small.every((v) => v < 0 && v >= -32), small.join(" "));
  }
  await page.screenshot({ path: path.join(shotDir, "web-priv-pad.png"), clip: box });
  const mark2 = logs.length;
  await page.selectOption("#padSelect", "none-game");
  await sleep(1000);
  check("Keyboard + mouse: the pointer as the game has it", logs.slice(mark2).some((l) => /the mouse's pointer as the game has it/.test(l)), "");
  const mark3 = logs.length;
  await page.selectOption("#padSelect", "none");
  await sleep(1000);
  check("Kbd + mouse (shown): the ring again", logs.slice(mark3).some((l) => /the mouse's pointer shown/.test(l)), "");
  check("the choice kept by the browser", await page.evaluate(() => localStorage.getItem("wc:privPointer")) === "ring", "");
}
if (process.env.SEATS) {
  check(`the room's session: an open room of ${process.env.SEATS} seats`, await waitLog(new RegExp(`Privateer: OPEN ROOM at room \\S+, ${process.env.SEATS} seats`), 20000),
    (logs.find((l) => /Privateer: (OPEN ROOM|HOST) at/.test(l)) || "").slice(0, 140));
}
if (process.env.ROOM) {
  check("in the open room as pilot 2", await waitLog(/Privateer: In room \S+ as pilot 2 of/, 20000), (logs.find((l) => /as pilot/.test(l)) || "").slice(0, 140));
  check("linked with the native pilot", await waitLog(/Privateer: (linked with pilot 1|a pilot at)/, 20000), (logs.find((l) => /linked with|a pilot at/.test(l)) || "").slice(0, 140));
  check("the host's presence: in Troy with us", await waitLog(/Privateer: (HOST launched in Troy|HOST in Troy, [\d,]+ m from you|In Troy with you: HOST)/, 40000),
    (logs.filter((l) => /Privateer: (HOST|In Troy)/.test(l)).pop() || "").slice(0, 140));
  check("the host's ship in our space (a puppet)", await waitLog(/Privateer: HOST's \w+ is in your space/, 30000),
    (logs.find((l) => /is in your space/.test(l)) || "").slice(0, 140));
  // the 0 key's chat prompt in flight, as a player types (the user, 2026-11-09: "0 also doesn't seem to do comms": it
  // opened only for Wing Commander's session)
  await page.focus("#canvas");
  await page.keyboard.press("0");
  await sleep(500);
  await page.keyboard.type("HELLO HOST", { delay: 60 });
  await page.keyboard.press("Enter");
  check("the 0 key's prompt: our line said to the room", await waitLog(/Privateer: said "HELLO HOST"/, 10000),
    (logs.filter((l) => /Privateer: said/.test(l)).pop() || "nothing said").slice(0, 140));
  // (the host's side, told of this autopilot and given our ship, is checked by web-priv.sh from the host's log)
  await page.keyboard.press("a");
}
const seconds = Number(secondsArg);
const until = t0 + seconds * 1000;
while (Date.now() < until) await sleep(2000);
await page.screenshot({ path: path.join(shotDir, "web-priv-end.png"), clip: box });
fs.writeFileSync(path.join(shotDir, "web-priv.log"), logs.join("\n") + "\n");
console.log(failed ? `PRIV FAILED (${failed})` : "PRIV OK");
await browser.close();
process.exit(failed ? 1 : 0);
