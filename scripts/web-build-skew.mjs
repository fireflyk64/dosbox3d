// Headless test of the pages' build check (web/build.js): a page left open across a deploy is another build, and
// its flights would not match.  Each page here is given a build.js of its own (Playwright serves it): ALPHA the
// newest, BRAVO older, CHARLIE a page from before builds were compared (it says nothing of its build), DELTA another
// site's page (/terrain/).  In the lobby BRAVO learns it is older; in a room both say so, the older one is told to
// reload, CHARLIE is asked in the chat (all an old page shows of ours), DELTA is told its game is another page's;
// and a host's start takes along no page of another build.  Driven by scripts/web-smoke.sh skew.
//
//   node scripts/web-build-skew.mjs PAGE_URL LOBBY_URL
import { createRequire } from "node:module";
import path from "node:path";
const [pageUrl, lobbyUrl] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome", args: ["--no-sandbox"] });
const results = [];
const check = (what, ok, detail) => { results.push(!!ok); console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail === undefined ? "" : ": " + JSON.stringify(detail)}`); };
const run = Math.random().toString(36).slice(2, 6).toUpperCase();
const real = await (await fetch(new URL("build.js", pageUrl))).text();
check("the served build.js is stamped by build-web.sh", /PAGE_BUILD = \{ id: "[0-9a-f]+\+?-\d+", at: \d+ \}/.test(real), real.match(/PAGE_BUILD = .*/)?.[0]);

// a page's build.js: its build, its site, or none at all (a page from before the check: it sends no build and
// notices nothing)
function moduleOf({ id, at, site, none }) {
  let s = real.replace(/^export const PAGE_BUILD = .*$/m, `export const PAGE_BUILD = { id: ${JSON.stringify(id || "x")}, at: ${at || 0} };`);
  if (site) s = s.replace(/^export const PAGE_SITE = .*$/m, `export const PAGE_SITE = ${JSON.stringify(site)};`);
  if (none) s = s.replace(/^export const buildFields = .*$/m, "export const buildFields = () => ({});")
                 .replace(/^export function compareBuild\(m\) \{$/m, "export function compareBuild(m) { return \"same\";");
  return s;
}
async function open(name, build, { hall = "HALL-" + run, lobby = true } = {}) {
  const context = await browser.newContext();
  const page = await context.newPage();
  page.on("pageerror", (e) => console.log(`[${name}] PAGE ERROR: ${e.message}`));
  await page.route("**/build.js", (r) => r.fulfill({ contentType: "text/javascript", body: moduleOf(build) }));
  const u = new URL(pageUrl);
  for (const [k, v] of Object.entries({ server: lobbyUrl, callsign: name, hall, ...(lobby ? {} : { lobby: "off" }) })) u.searchParams.set(k, v);
  await page.goto(u.toString());
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready", null, { timeout: 60000 });
  return page;
}
const text = (page, id) => page.$eval("#" + id, (el) => el.innerText);
const sees = (page, id, re, timeout = 20000) => page.waitForFunction(([i, src]) => new RegExp(src).test(document.getElementById(i).innerText), [id, re.source], { timeout }).then(() => true, () => false);
async function join(page, code, seats) {
  if (seats) await page.fill("#seats", String(seats));
  await page.fill("#room", code);
  await page.click("#join");
}

// -- the lobby: the older page learns it is older ---------------------------------------------------------------
const alpha = await open("ALPHA", { id: "aaa-2000", at: 2000 });
const bravo = await open("BRAVO", { id: "bbb-1000", at: 1000 });
check("in the lobby the older page says it is older, and how to reload", await sees(bravo, "hallLog", /A newer version of this page is out: reload with Ctrl\+Shift\+R/), (await text(bravo, "hallLog")).slice(-300));
check("and the newer one says nothing of it", !/newer version|out of date/.test(await text(alpha, "hallLog")), (await text(alpha, "hallLog")).slice(-300));

// -- a room: both say it; the host's start does not take the older page along -------------------------------------
const room1 = `RV-${run}1`;
await join(alpha, room1);
await join(bravo, room1);
check("in a room the newer page says the other's is older", await sees(alpha, "chatLog", /BRAVO's page is out of date\./), (await text(alpha, "chatLog")).slice(-300));
check("the older page says it is older, how to reload, in the chat and the status line, its Fly held", (await sees(bravo, "chatLog", /ALPHA has a newer version of this page: reload with Ctrl\+Shift\+R/)) && (await sees(bravo, "status", /This page is out of date: reload with Ctrl\+Shift\+R/)) && (await bravo.$eval("#fly", (b) => b.disabled)), [(await text(bravo, "chatLog")).slice(-300), await text(bravo, "status")]);
await alpha.click("#fly");
check("the host flying: its page says the older one will not fly along", await sees(alpha, "chatLog", /BRAVO won't fly along \(their page is out of date\)/), (await text(alpha, "chatLog")).slice(-300));
check("and the older page does not follow the start, and says why", (await sees(bravo, "status", /This page is out of date: reload with Ctrl\+Shift\+R/)) && !(await bravo.evaluate(() => !!(window.DOSBox && window.DOSBox.running))), await text(bravo, "status"));
await alpha.context().close(); await bravo.context().close();

// -- a page from before the check: asked in the chat ------------------------------------------------------------
const alpha2 = await open("ALPHA2", { id: "aaa-2000", at: 2000 }, { lobby: false });
const charlie = await open("CHARLIE", { none: true }, { lobby: false });
const room2 = `RV-${run}2`;
await join(alpha2, room2);
await join(charlie, room2);
check("a page that says nothing of its build is called out of date, and asked", await sees(alpha2, "chatLog", /CHARLIE's page is out of date \(asked to reload\)/), (await text(alpha2, "chatLog")).slice(-300));
check("the old page shows the request as a line of chat", await sees(charlie, "chatLog", /ALPHA2:\s*\(automatic\) Your page is out of date: please reload with Ctrl\+Shift\+R/), (await text(charlie, "chatLog")).slice(-300));
await alpha2.context().close(); await charlie.context().close();

// -- another site's page --------------------------------------------------------------------------------------
const alpha3 = await open("ALPHA3", { id: "aaa-2000", at: 2000 }, { lobby: false });
const delta = await open("DELTA", { id: "ddd-3000", at: 3000, site: "/terrain/" }, { lobby: false });
const room3 = `RV-${run}3`;
await join(alpha3, room3);
await join(delta, room3);
check("a page of another site is named as such, not newer", (await sees(alpha3, "chatLog", /DELTA is on another page \(\/terrain\/\): fly from one page/)) && !/newer version|out of date/.test(await text(alpha3, "chatLog")), (await text(alpha3, "chatLog")).slice(-300));
await alpha3.context().close(); await delta.context().close();

// -- the same build: nothing said ----------------------------------------------------------------------------
const echo = await open("ECHO", { id: "eee-2000", at: 2000 }, { lobby: false });
const fox = await open("FOX", { id: "eee-2000", at: 2000 }, { lobby: false });
const room4 = `RV-${run}4`;
await join(echo, room4);
await join(fox, room4);
await sees(echo, "chatLog", /FOX is in the room/);
await new Promise((r) => setTimeout(r, 2000));
check("two pages of one build say nothing at all of it", !/version|out of date|another page|build/.test(await text(echo, "chatLog")) && !/version|out of date|another page|build/.test(await text(fox, "chatLog")), [(await text(echo, "chatLog")).slice(-200), (await text(fox, "chatLog")).slice(-200)]);

await browser.close();
const passed = results.filter(Boolean).length;
console.log(`${passed} of ${results.length} checks passed`);
process.exit(passed === results.length ? 0 : 1);
