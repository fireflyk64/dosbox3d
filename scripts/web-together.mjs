// The "Free to play now" buttons find company (web/wc.js playTogether; the user, 2026-11-15): pages in a room of a local
// lobby server, each its own browser (its own saved games).  A clicks a game's button: no room is open, so it opens one
// and offers it in the lobby (Fly Alone still offered); B clicks the same button: it joins A's room; C opens a link to
// that room with no copy of the game: the game is fetched, then the room joined (a third seat when the game has one);
// D opens the page with ?game=tv: Terminal Velocity is the game picked (fetched).  The Internet Archive's zip is served
// from a local copy (ARCHIVE_ZIP), to spare its bandwidth.  Each check printed with OK or FAIL; ends "TOGETHER OK".
//
//   node scripts/web-together.mjs PAGE_URL LOBBY_URL [button ...]     (buttons: demoTfd demoRv demoRap demoTv)
//   env: PLAYWRIGHT_DIR, CHROME, ARCHIVE_ZIP, HALL (the lobby's name: a test's own, as on the live site with LOBBY_URL
//        "public": never WC-LOBBY itself)
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl, ...buttons] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl) {
  console.error("usage: node web-together.mjs PAGE_URL LOBBY_URL [button ...]");
  process.exit(2);
}
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--autoplay-policy=no-user-gesture-required"] });
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (ok, what) => { console.log(`${stamp()} ${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };
const archive = process.env.ARCHIVE_ZIP ? fs.readFileSync(process.env.ARCHIVE_ZIP) : null;

async function openPage(who, params = {}) {
  const context = await browser.newContext({ viewport: { width: 1280, height: 1000 } });
  const page = await context.newPage();
  const logs = [];
  let archived = 0;
  page.on("console", (m) => logs.push(m.text()));
  page.on("pageerror", (e) => console.log(`${stamp()} [${who}] PAGE ERROR: ${e.message}`));
  if (archive) {
    await page.route("https://archive.org/**", (route) => {
      archived++;
      route.fulfill({ status: 200, body: archive, headers: { "content-type": "application/zip", "access-control-allow-origin": "*" } });
    });
  }
  const u = new URL(pageUrl);
  if (lobbyUrl !== "public") u.searchParams.set("server", lobbyUrl);
  if (process.env.HALL) u.searchParams.set("hall", process.env.HALL);    // (a lobby of the test's own: never WC-LOBBY)
  u.searchParams.set("callsign", who.toUpperCase() + "PILOT");
  for (const [k, v] of Object.entries(params)) u.searchParams.set(k, v);
  await page.goto(u.toString());
  return { page, logs, who, fetched: () => archived };
}
const inRoom = (p) => p.page.waitForFunction(() => !document.getElementById("leave").hidden, null, { timeout: 90000 }).then(() => true, () => false);
const roomCode = (p) => p.page.$eval("#room", (el) => el.value);
const said = (p) => p.page.$eval("#status", (el) => el.textContent).catch(() => "");

for (const button of buttons.length ? buttons : ["demoTfd", "demoRv"]) {
  console.log(`${stamp()} == ${button}`);
  const a = await openPage("a" + button);
  await a.page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready", null, { timeout: 60000 }).catch(() => {});
  await a.page.click("#" + button);
  check(await inRoom(a), `A: in a room after the button (${(await said(a)).slice(0, 140)})`);
  const code = await roomCode(a);
  const game = await a.page.inputValue("#program");
  await a.page.waitForFunction((c) => document.getElementById("hallLog").textContent.includes(c), code, { timeout: 15000 }).catch(() => {});
  check(/offered there/.test(await said(a)) && (await a.page.$eval("#hallLog", (el) => el.textContent)).includes(code),
    `A: no open room, so its own opened and offered in the lobby (${code}, ${game}; ${(await said(a)).slice(0, 90)})`);
  check(await a.page.$eval("#flySolo", (b) => !b.hidden), "A: Fly Alone offered while nobody has come");
  await a.page.waitForTimeout(1500);
  const b = await openPage("b" + button);
  await b.page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready", null, { timeout: 60000 }).catch(() => {});
  await b.page.click("#" + button);
  check(await inRoom(b), `B: in a room after the button (${(await said(b)).slice(0, 140)})`);
  check(await roomCode(b) === code, `B: joined A's open room (${await roomCode(b)})`);
  await a.page.waitForFunction(() => document.getElementById("roster").children.length > 1, null, { timeout: 30000 }).catch(() => {});
  check(await a.page.$eval("#flySolo", (b) => b.hidden), "A: Fly Alone gone once B is in");
  // a link to the room, opened with no copy of the game (a fresh browser: the page's own game first)
  const c = await openPage("c" + button, { room: code });
  const seats = await a.page.$eval("#seats", (el) => Number(el.value));
  if (seats > 2) {
    const joined = await inRoom(c);
    check(joined && await roomCode(c) === code && await c.page.inputValue("#program") === game,
      `C: the room's link, its game got first, the room joined (${await c.page.inputValue("#program")}; ${(await said(c)).slice(0, 120)})`);
  } else {
    await c.page.waitForFunction(() => /is full/.test(document.getElementById("status").textContent), null, { timeout: 90000 }).catch(() => {});
    check(await c.page.inputValue("#program") === game && /is full/.test(await said(c)),
      `C: the room's link, its game got first, the room of ${seats} full and said so (${await c.page.inputValue("#program")}; ${(await said(c)).slice(0, 90)})`);
  }
  if (button === "demoTfd") check(c.fetched() === 1 && a.fetched() === 1 && b.fetched() === 1, `each fetched the Internet Archive's zip once (${a.fetched()}, ${b.fetched()}, ${c.fetched()})`);
  for (const p of [a, b, c]) await p.page.context().close();
}

// ?game=tv: Terminal Velocity picked (fetched from this site), not the page's own game
const d = await openPage("d", { game: "tv" });
await d.page.waitForFunction(() => document.getElementById("program").value === "tv", null, { timeout: 60000 }).catch(() => {});
check(await d.page.inputValue("#program") === "tv", `?game=tv: Terminal Velocity picked (${await d.page.inputValue("#program")})`);
const e = await openPage("e", { game: "tf" });
await e.page.waitForFunction(() => document.getElementById("program").value === "tiedemo", null, { timeout: 60000 }).catch(() => {});
check(await e.page.inputValue("#program") === "tiedemo", `?game=tf with no TIE Fighter: the demo (${await e.page.inputValue("#program")})`);
await browser.close();
console.log(failed ? `TOGETHER FAILED (${failed})` : "TOGETHER OK");
process.exit(failed ? 1 : 0);
