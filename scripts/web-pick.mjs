// The Game menu and the room's three buttons (web/wc.js; the user, 2026-11-17: one dropdown for every game, Host,
// Join open game, Join by a code), pages in rooms of a local lobby server, each its own browser (its own saved games):
//   - a fresh page: the free games in the menu (not fetched yet), the games that need files greyed, the code box empty
//   - a free game picked in the menu is fetched; Host with no code typed makes a room named after the game (TFD-4821)
//   - Join open game on a page with another game picked and no room of it: the open room of a game it can fetch
//   - a code of another game's prefix left in the box does not switch the host's game (the pinning bug)
//   - Join with a code typed: the room joined, its host's game fetched and switched to; no such room: none made
//   - Join open game prefers the picked game's room over another's
//   - ?tv=1, ?tf=1 (the demo with no TIE Fighter), ?game=tv; a visit with no link's game starts with RetroVega (no Wing Commander here)
// The Internet Archive's zip is served from a local copy (ARCHIVE_ZIP).  Each check printed with OK or FAIL; ends
// "PICK OK".
//
//   node scripts/web-pick.mjs PAGE_URL LOBBY_URL
//   env: PLAYWRIGHT_DIR, CHROME, ARCHIVE_ZIP, OVERRIDE_WC_JS (a wc.js served in place of the page's), HALL (the lobby's name: a test's own, as on the live site with LOBBY_URL
//        "public": never WC-LOBBY itself)
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";

const [pageUrl, lobbyUrl] = process.argv.slice(2);
if (!pageUrl || !lobbyUrl) {
  console.error("usage: node web-pick.mjs PAGE_URL LOBBY_URL");
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

async function openPage(who, params = {}, context = null) {
  context = context || await browser.newContext({ viewport: { width: 1280, height: 1000 } });
  const page = await context.newPage();
  const logs = [];
  let archived = 0;
  page.on("console", (m) => logs.push(m.text()));
  page.on("pageerror", (e) => console.log(`${stamp()} [${who}] PAGE ERROR: ${e.message}`));
  if (process.env.OVERRIDE_WC_JS) {
    // (this tree's page script in place of the served one: a fix tried on the live site and the public lobby first)
    const js = fs.readFileSync(process.env.OVERRIDE_WC_JS);
    await page.route(/\/wc\.js(\?.*)?$/, (route) => route.fulfill({ status: 200, body: js, headers: { "content-type": "text/javascript" } }));
  }
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

const ready = (p, id) => p.page.waitForFunction((id) => document.getElementById("sourceStatus").dataset.state === "ready"
  && (!id || document.getElementById("program").value === id), id || null, { timeout: 90000 }).then(() => true, () => false);
const game = (p) => p.page.inputValue("#program");
const hostLabel = (p) => p.page.$eval("#join", (el) => el.textContent);
const inLobby = (p, n) => p.page.waitForFunction((n) => !document.getElementById("hallBody").hidden && document.getElementById("hallRoster").children.length >= n, n, { timeout: 40000 }).then(() => true, () => false);
const leave = async (p) => { await p.page.click("#leave"); await p.page.waitForFunction(() => document.getElementById("leave").hidden, null, { timeout: 10000 }).catch(() => {}); };
const settled = (p) => p.page.waitForFunction(() => !/Asking the pilots|Fetching|Joining room/.test(document.getElementById("status").textContent), null, { timeout: 90000 }).catch(() => {});

// -- a fresh page
const a = await openPage("alfa");
await ready(a);
const menu = await a.page.$$eval("#program option", (os) => os.map((o) => `${o.value}${o.disabled ? "-" : ""}`));
console.log(`${stamp()} menu: ${menu.join(" ")}`);
check(await game(a) === "rv" && ["tiedemo", "tv", "rap"].every((id) => menu.includes(id)) && menu.includes("wc1-") && menu.includes("tf-"),
  `a fresh page: RetroVega picked, the free games in the menu, the games that need files greyed`);
check(await roomCode(a) === "" && /^Host RetroVega$/i.test(await hostLabel(a)), `the code box empty, the button "${await hostLabel(a)}"`);
const names = await a.page.$$eval("#gameList button", (bs) => bs.map((b) => `${b.dataset.game}${b.disabled ? "-" : ""}${b.classList.contains("on") ? "*" : ""}`));
check(names.join(" ") === "rv* tv rap tiedemo wc1- wc2- tf- priv-", `every game named beside the menu, the one picked lit, those that need files dim (${names.join(" ")})`);

// -- a free game picked: fetched; Host with no code: a room named after the game
await a.page.click("#gameList button[data-game=tiedemo]");
check(await ready(a, "tiedemo") && a.fetched() === 1 && await a.page.$eval("#gameList button[data-game=tiedemo]", (b) => b.classList.contains("on")), `TIE Fighter's demo picked by its name: fetched once, lit (${a.fetched()}), picked (${await game(a)})`);
check(/^Host TIE Fighter demo$/i.test(await hostLabel(a)), `the button "${await hostLabel(a)}"`);
await inLobby(a, 1);
await a.page.click("#join");
check(await inRoom(a), `A: Host, no code typed: in a room (${(await said(a)).slice(0, 100)})`);
const codeA = await roomCode(a);
await a.page.waitForFunction((c) => document.getElementById("hallLog").textContent.includes(c), codeA, { timeout: 15000 }).catch(() => {});
check(/offered in the lobby/.test(await said(a)) && (await a.page.$eval("#hallLog", (el) => el.textContent)).includes(codeA) && await a.page.$eval("#flySolo", (b) => !b.hidden)
  && !(await a.page.$eval("#private", (el) => el.checked)), `A: Host offers the room in the lobby, Fly Alone still there (${(await said(a)).slice(0, 90)})`);
check(/^TFD-\d{4}$/.test(codeA) && await game(a) === "tiedemo", `the room's code made for the game: ${codeA}`);

// -- Join open game with another game picked: no RetroVega room, so the demo's, fetched for it
const d = await openPage("delta");
await ready(d); await inLobby(d, 2); await inLobby(a, 2);
check(await game(d) === "rv", `D: RetroVega picked (${await game(d)})`);
check(/^Join open RetroVega$/i.test(await d.page.$eval("#findRoom", (el) => el.textContent)), `D: the room panel's button is "${await d.page.$eval("#findRoom", (el) => el.textContent)}"`);
await d.page.click("#findRoom");
await d.page.waitForFunction(() => /No open RetroVega game found/.test(document.getElementById("status").textContent), null, { timeout: 40000 }).catch(() => {});
check(/No open RetroVega game found/.test(await said(d)) && /takes any/.test(await said(d)) && await d.page.$eval("#leave", (el) => el.hidden) && await game(d) === "rv",
  `D: it keeps to RetroVega: A's demo room is not joined (${(await said(d)).slice(0, 110)})`);
await d.page.click("#topAction");
check(await inRoom(d) && await roomCode(d) === codeA, `D: the top Join open game takes any game: A's joined (${await roomCode(d)})`);
await ready(d, "tiedemo");
const chatD = await d.page.$eval("#chatLog", (el) => el.textContent);
check(await game(d) === "tiedemo" && d.fetched() === 1 && /No open RetroVega room: you are in room/.test(chatD),
  `D: its game fetched and picked, and said in the room's chat (${await game(d)}; ${chatD.slice(-150)})`);
await leave(d);
check(await roomCode(d) === "", `D: the room left: the code box empty again ("${await roomCode(d)}")`);
await d.page.context().close();

// -- a code of another game's prefix in the box does not switch the host's game
await leave(a);
check(await roomCode(a) === "" && await game(a) === "tiedemo", `A: the room left: the box empty, the demo still picked`);
const named = "RV-OLD" + Math.floor(Math.random() * 9000 + 1000);
await a.page.fill("#room", named);
await a.page.click("#join");
check(await inRoom(a) && await roomCode(a) === named && await game(a) === "tiedemo",
  `A: Host with ${named} typed: that room, and the game picked stays (${await game(a)})`);

// -- Join by a code: the host's game fetched and switched to; a room that is not there is not made
const b = await openPage("bravo");
await ready(b);
await b.page.fill("#room", "RV-NONE77");
await b.page.click("#joinCode");
await b.page.waitForFunction(() => /is not open/.test(document.getElementById("status").textContent), null, { timeout: 30000 }).catch(() => {});
check(/is not open/.test(await said(b)) && await b.page.$eval("#leave", (el) => el.hidden), `B: Join with a code nobody hosts: no room made (${(await said(b)).slice(0, 90)})`);
await b.page.fill("#room", "");
await b.page.click("#joinCode");
check(/Type the code/.test(await said(b)) && await b.page.$eval("#leave", (el) => el.hidden), `B: Join with no code: asked for one (${(await said(b)).slice(0, 60)})`);
await b.page.fill("#room", named);
await b.page.press("#room", "Enter");
check(await inRoom(b) && await roomCode(b) === named, `B: the code typed and Enter: in A's room (${await roomCode(b)})`);
await ready(b, "tiedemo");
check(await game(b) === "tiedemo" && b.fetched() === 1, `B: the host's game fetched and picked (${await game(b)}, ${b.fetched()} fetch)`);
await b.page.waitForFunction(() => /Switched to TIE Fighter/.test(document.getElementById("chatLog").textContent), null, { timeout: 5000 }).catch(() => {});
check(await b.page.$eval("#chatLog", (el) => /fetching it/.test(el.textContent) && /Switched to TIE Fighter/.test(el.textContent)), `B: said in the room's chat (${(await b.page.$eval("#chatLog", (el) => el.textContent)).slice(-110)})`);
// (B leaves its seat: a page that is simply closed keeps it)
await leave(b);
await b.page.context().close();

// -- Join open game prefers the game picked: an open RetroVega room and A's demo room
const e = await openPage("echo");
await ready(e); await inLobby(e, 2);
await e.page.click("#join");
check(await inRoom(e) && /^RV-\d{4}$/.test(await roomCode(e)), `E: Host RetroVega: ${await roomCode(e)}`);
const codeE = await roomCode(e);
for (let i = 0; i < 2; i++) {
  const f = await openPage("fox" + i);
  await ready(f); await inLobby(f, 3);
  await f.page.click(i === 0 ? "#findRoom" : "#topAction");
  const ok = await inRoom(f);
  if (i === 0) check(ok && await roomCode(f) === codeE && await game(f) === "rv", `F: RetroVega picked, Join open game: E's RetroVega room, not the demo's (${await roomCode(f)})`);
  // (E's room of two is full now: the next pilot with RetroVega picked gets the demo's)
  else { await ready(f, "tiedemo"); check(ok && await roomCode(f) === named && await game(f) === "tiedemo", `F2: E's room full: A's demo room (${await roomCode(f)}, ${await game(f)}; ${(await said(f)).slice(0, 80)})`); }
  if (i === 1) await f.page.context().close();
}
// (?tf=1 with no TIE Fighter: the demo picked, and its room preferred... A's is full or not, the pick is what counts)
const g = await openPage("golf", { tf: "1" });
check(await ready(g, "tiedemo") && await game(g) === "tiedemo", `?tf=1 with no TIE Fighter: the demo picked (${await game(g)})`);
check(/^Join open TIE Fighter$/i.test(await g.page.$eval("#findRoom", (el) => el.textContent)), `its series on the room panel's button: "${await g.page.$eval("#findRoom", (el) => el.textContent)}"`);
await g.page.context().close();
const h = await openPage("hotel", { tv: "1" });
check(await ready(h, "tv") && await game(h) === "tv", `?tv=1: Terminal Velocity picked (${await game(h)})`);
// -- a visit with no link's game starts with RetroVega here (no Wing Commander in this browser), whatever was picked
// last and whatever the newest copy is (the user, 2026-11-19: "default to wing commander if installed or retro Vega
// if not.  Only tf should default to tie fighter")
await h.page.selectOption("#program", "rap");
await ready(h, "rap");
await h.page.selectOption("#program", "tv");
await ready(h, "tv");
const h2 = await openPage("hotel", {}, h.page.context());
await ready(h2);
check(await game(h2) === "rv", `a second visit, Terminal Velocity picked last and Raptor's the newer copy: RetroVega (${await game(h2)})`);
const h4 = await openPage("hotel", { wc1: "1" }, h.page.context());
await ready(h4);
check(/Wing Commander needs your own files/.test(await h4.page.$eval("#sourceStatus", (el) => el.textContent)), `?wc1=1 without its files: said (${(await h4.page.$eval("#sourceStatus", (el) => el.textContent)).slice(0, 90)})`);
await h.page.context().close();
const k = await openPage("kilo", { game: "tv" });
check(await ready(k, "tv"), `?game=tv: Terminal Velocity picked (${await game(k)})`);
// -- Private ticked: Host says nothing in the lobby
await inLobby(k, 1);
await k.page.check("#private");
await k.page.click("#join");
check(await inRoom(k) && /^TV-\d{4}$/.test(await roomCode(k)), `K: Private ticked, Host: ${await roomCode(k)}`);
await k.page.waitForTimeout(2500);
check(await k.page.$eval("#private", (el) => el.checked) && !(await k.page.$eval("#hallLog", (el) => el.textContent)).includes(await roomCode(k)) && !/offered in the lobby/.test(await said(k)),
  `K: the room not said in the lobby (${(await said(k)).slice(0, 60)})`);
await browser.close();
console.log(failed ? `PICK FAILED (${failed})` : "PICK OK");
process.exit(failed ? 1 : 0);
