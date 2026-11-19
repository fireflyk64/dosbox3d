// A room's connection to the lobby server kept, and made again when it is lost (docs/wcnet-multiplayer.md; the user,
// 2026-11-19: "I just hosted a game on my browser--I waited like 20 minutes and tried to join on another browser...
// but it said the game was not available").  A lobby server of this script's own (so that it can be stopped), the
// page as built, the TIE Fighter demo dropped; a host's room left waiting.
//   1. For 60 s the page says a line to the server every 25 s (the socket's frames watched), and nothing of it shows
//      in the room's chat.
//   2. The server stopped: the page says it lost the connection and is making it again; the server started again 8 s
//      later (its rooms gone with it): the page is in its room again, made anew, as its host.
//   3. A second browser joins the room by its code: two pilots on both rosters.
// Ends "ROOM KEEP OK".
//
//   node scripts/web-room-keep.mjs PAGE_URL LOBBY_PORT LOBBY_BIN ZIP
//   env: PLAYWRIGHT_DIR, CHROME
import { createRequire } from "node:module";
import { spawn } from "node:child_process";
import path from "node:path";
import fs from "node:fs";
import os from "node:os";

const [pageUrl, lobbyPort, lobbyBin, zip] = process.argv.slice(2);
if (!pageUrl || !lobbyPort || !lobbyBin || !zip) {
  console.error("usage: node web-room-keep.mjs PAGE_URL LOBBY_PORT LOBBY_BIN ZIP");
  process.exit(2);
}
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1);
let failed = 0;
const check = (ok, what) => { console.log(`${stamp()} ${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const lobbyUrl = `http://127.0.0.1:${lobbyPort}`;
const conf = path.join(os.tmpdir(), `room-keep-${process.pid}.toml`);
fs.writeFileSync(conf, "[rooms]\nmax_players_hard = 8\n");
let server = null;
const startServer = () => {
  server = spawn(lobbyBin, ["--config", conf, "--listen-http", `127.0.0.1:${lobbyPort}`, "--allowed-origin", new URL(pageUrl).origin,
    "--public-url", lobbyUrl, "--allow-no-origin"], { stdio: "ignore" });
};
const stopServer = () => new Promise((r) => { if (!server) return r(); server.once("exit", r); server.kill("SIGKILL"); server = null; });
startServer();
await sleep(1000);

const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome", args: ["--no-sandbox"] });
async function open(who) {
  const page = await (await browser.newContext({ viewport: { width: 1280, height: 1000 } })).newPage();
  const beats = [];
  page.on("websocket", (ws) => ws.on("framesent", (f) => { if (/"type":"ping"/.test(String(f.payload))) beats.push(Date.now()); }));
  page.on("pageerror", (e) => console.log(`${stamp()} [${who}] PAGE ERROR: ${e.message}`));
  const u = new URL(pageUrl);
  u.searchParams.set("server", lobbyUrl);
  u.searchParams.set("advertise", "off");
  u.searchParams.set("hall", "KEEP-TEST");
  await page.goto(u.toString());
  await page.setInputFiles("#gamefile", zip);
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready" && document.getElementById("program").value === "tiedemo", null, { timeout: 120000 });
  const chat = () => page.evaluate(() => document.getElementById("chatLog") ? document.getElementById("chatLog").innerText : document.getElementById("lobby").innerText);
  const roster = () => page.evaluate(() => document.getElementById("roster").children.length);
  return { page, beats, chat, roster };
}

const host = await open("host");
await host.page.fill("#seats", "2");
await host.page.dispatchEvent("#seats", "change");
await host.page.click("#join");
await host.page.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 0, null, { timeout: 60000 });
const code = await host.page.inputValue("#room");
console.log(`${stamp()} the host waits in room ${code}`);
await sleep(60000);
check(host.beats.length >= 3, `in 60 s of waiting the page said ${host.beats.length} lines to the server (the room's and the lobby chat's, each every 25 s: two alone are the chat's)`);
check(!/Lobby error/.test(await host.chat()), "and nothing of them in the room's chat");

await stopServer();
await host.page.waitForFunction(() => /Lost the lobby server's connection: making it again/.test(document.getElementById("lobby").innerText), null, { timeout: 20000 }).catch(() => {});
check(/Lost the lobby server's connection: making it again/.test(await host.chat()), "the server stopped: the page says its connection is lost and that it is making it again");
await sleep(8000);
startServer();
await host.page.waitForFunction(() => /connection is made again: room/.test(document.getElementById("lobby").innerText), null, { timeout: 60000 }).catch(() => {});
check(/connection is made again: room/.test(await host.chat()), `the server back (its rooms gone): the page is in room ${code} again, made anew`);

const guest = await open("guest");
await guest.page.fill("#room", code);
await guest.page.click("#joinCode");
await guest.page.waitForFunction(() => document.getElementById("roster").children.length > 1, null, { timeout: 30000 }).catch(() => {});
await host.page.waitForFunction(() => document.getElementById("roster").children.length > 1, null, { timeout: 30000 }).catch(() => {});
check(await guest.roster() === 2 && await host.roster() === 2, `a second browser joins ${code}: ${await host.roster()} and ${await guest.roster()} pilots on the two rosters`);
check(await host.page.$eval("#fly", (b) => !b.disabled && /starts everyone/.test(b.textContent)), "and the first is its host still (Fly starts everyone)");
if (failed) console.log(`the host's room:\n${(await host.chat()).slice(-600)}`);
await browser.close();
await stopServer();
fs.rmSync(conf, { force: true });
console.log(failed ? `ROOM KEEP FAILED (${failed})` : "ROOM KEEP OK");
process.exit(failed ? 1 : 0);
