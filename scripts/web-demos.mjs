// The page's free shareware (web/demos, picked in the Game menu of step 1): one Chrome page with nothing saved
// picks each and must end ready with that game picked, the archive fetched from the site once; a
// second visit must use the copy the browser kept, fetching nothing.  The links (the
// archives, the full games' stores) must be there.  Ends "DEMOS PAGE OK".
//
//   node scripts/web-demos.mjs PAGE_URL
//   env: PLAYWRIGHT_DIR, CHROME
import { createRequire } from "node:module";
import path from "node:path";

const [pageUrl] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome", args: ["--no-sandbox"] });
let failed = 0;
const check = (ok, what) => { console.log(`${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };
const context = await browser.newContext();
const page = await context.newPage({ viewport: { width: 1280, height: 1000 } });
const fetched = [];
page.on("response", (r) => { if (/\/demos\/[^/]+\.zip$/.test(r.url())) fetched.push([r.url(), r.status()]); });
page.on("pageerror", (e) => console.log(`PAGE ERROR: ${e.message}`));
const u = new URL(pageUrl);
u.searchParams.set("lobby", "off");
await page.goto(u.toString());
await page.waitForSelector("#program option[value=rap]", { state: "attached" });
const links = await page.$$eval("#demos a", (as) => as.map((a) => a.href));
console.log(`links: ${links.join(" ")}`);
check(links.some((h) => h.endsWith("demos/1rap12.zip")) && links.some((h) => h.endsWith("demos/3dtv12.zip")), "the archives linked");
check(links.some((h) => h.includes("store.steampowered.com/app/358360")) && links.some((h) => h.includes("gog.com/en/game/terminal_velocity")), "the full games' stores linked");
const ready = (id) => page.waitForFunction((id) => document.getElementById("sourceStatus").dataset.state === "error"
  || (document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === id), id, { timeout: 120000 }).catch(() => {});
for (const [button, id, file] of [["Raptor in the Game menu", "rap", "1rap12.zip"], ["Terminal Velocity in the Game menu", "tv", "3dtv12.zip"]]) {
  const before = fetched.length;
  await page.selectOption("#program", id);
  await ready(id);
  const st = await page.$eval("#sourceStatus", (el) => `${el.dataset.state}: ${el.textContent.slice(0, 120)}`);
  check(await page.inputValue("#program") === id && st.startsWith("ready"), `${button}: ${id} ready (${st})`);
  check(fetched.length === before + 1 && fetched[fetched.length - 1][0].endsWith(file) && fetched[fetched.length - 1][1] === 200, `${button}: ${file} fetched once from the site`);
}
await page.waitForFunction(() => /Raptor/.test(document.getElementById("savedList").textContent)
  && /Terminal Velocity/.test(document.getElementById("savedList").textContent), null, { timeout: 60000 }).catch(() => {});
const kept = await page.$eval("#savedList", (el) => el.textContent);
console.log(`saved here: ${kept}`);
check(/Raptor/.test(kept) && /Terminal Velocity/.test(kept), "both kept in this browser");
// a second visit: the kept copies, nothing fetched
await page.reload();
await page.waitForSelector("#program option[value=rap]", { state: "attached" });
const before = fetched.length;
for (const [button, id] of [["Raptor in the Game menu", "rap"], ["Terminal Velocity in the Game menu", "tv"]]) {
  await page.selectOption("#program", id);
  await ready(id);
  check(await page.inputValue("#program") === id, `second visit, ${button}: ${id} from the kept copy`);
}
check(fetched.length === before, `second visit: nothing fetched (${fetched.length - before})`);
await browser.close();
console.log(failed ? `DEMOS PAGE FAILED (${failed})` : "DEMOS PAGE OK");
process.exit(failed ? 1 : 0);
