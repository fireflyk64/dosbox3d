// Headless test of voice between two browser pages: both join a room, one
// opts in (the other's control lights up and says so), the other opts in
// (the voice link connects both ways, with a fake microphone), one opts out
// (the link goes), and Fly with a voice on one side only says so in the chat
// of both.  Driven by scripts/web-smoke.sh voice.
//
//   node scripts/web-voice.mjs PAGE_URL LOBBY_URL             # the server's wc.tar.gz
//   GAME_FILE=wc2.zip node scripts/web-voice.mjs PAGE_URL LOBBY_URL   # the pages' own game files
import { createRequire } from "node:module";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
const [pageUrl, lobbyUrl] = process.argv.slice(2);
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
// The microphones' sound: something like a voice, half-second vowels (a glottal pulse's harmonics under three
// formants, syllables in them) with pauses of 2.4 s between, looped by Chrome.  Its fake device's own beeps last a twentieth
// of a second, too short to be taken for a voice (web/voice.js lowers the game only under one).
function speechWav() {
  const rate = 48000, seconds = 9, n = rate * seconds, data = Buffer.alloc(44 + n * 2);
  const formant = (f) => Math.exp(-(((f - 700) / 180) ** 2)) + 0.6 * Math.exp(-(((f - 1200) / 220) ** 2)) + 0.25 * Math.exp(-(((f - 2600) / 300) ** 2));
  let phase = 0;
  for (let i = 0; i < n; i++) {
    const t = i / rate, inBurst = t % 3, on = inBurst < 0.6;
    let v = 0;
    if (on) {
      const f0 = 130 + 30 * (inBurst / 0.6);
      phase += (2 * Math.PI * f0) / rate;
      for (let h = 1; h <= 20; h++) v += formant(h * f0) * Math.sin(h * phase) / Math.sqrt(h);
      const edge = Math.min(1, inBurst / 0.03, (0.6 - inBurst) / 0.03);
      v *= 0.12 * edge * (0.7 + 0.3 * Math.sin(2 * Math.PI * 5 * inBurst));
    }
    data.writeInt16LE(Math.max(-32767, Math.min(32767, Math.round(v * 32767))), 44 + i * 2);
  }
  data.write("RIFF", 0); data.writeUInt32LE(36 + n * 2, 4); data.write("WAVE", 8); data.write("fmt ", 12);
  data.writeUInt32LE(16, 16); data.writeUInt16LE(1, 20); data.writeUInt16LE(1, 22); data.writeUInt32LE(rate, 24);
  data.writeUInt32LE(rate * 2, 28); data.writeUInt16LE(2, 32); data.writeUInt16LE(16, 34); data.write("data", 36); data.writeUInt32LE(n * 2, 40);
  const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "wcvoice-")), "speech.wav");
  fs.writeFileSync(file, data);
  return file;
}
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome",
  args: ["--no-sandbox", "--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream", `--use-file-for-fake-audio-capture=${speechWav()}`] });
const results = [];
const check = (what, ok, detail) => { results.push(ok); console.log(`${ok ? "ok  " : "FAIL"} ${what}: ${JSON.stringify(detail)}`); };
const sleep = (s) => new Promise((r) => setTimeout(r, s * 1000));
const room = "VOICE-" + Math.random().toString(36).slice(2, 8).toUpperCase();
const logs = { host: [], wing: [] };
async function open(name, callsign) {
  const context = await browser.newContext();
  const page = await context.newPage();
  page.on("console", (m) => logs[name].push(m.text()));
  page.on("pageerror", (e) => console.log(`[${name}] PAGE ERROR: ${e.message}`));
  const u = new URL(pageUrl);
  for (const [k, v] of Object.entries({ room, server: lobbyUrl, callsign, "env.MIS": "0", "env.SERIES": "1" })) u.searchParams.set(k, v);
  await page.goto(u.toString());
  if (process.env.GAME_FILE) await page.setInputFiles("#gamefile", process.env.GAME_FILE);
  await page.waitForFunction(() => /^(ready|error)$/.test(document.getElementById("sourceStatus").dataset.state || ""), null, { timeout: 300000 });
  // (The page joins the room on its own, ?room= being in the URL.)
  await page.waitForFunction(() => !document.getElementById("lobby").hidden && document.getElementById("roster").children.length > 0, null, { timeout: 60000 });
  return page;
}
const choose = (page, v) => page.selectOption("#voice", v);
const voiceState = (page) => page.$eval("#voiceState", (el) => el.textContent);
const lit = (page) => page.$eval("#voiceLabel", (el) => el.classList.contains("attention"));
const links = (page) => page.evaluate(() => window.__wcVoiceLinks());
const chat = (page) => page.$eval("#chatLog", (el) => el.innerText);

const host = await open("host", "HOST");
const wing = await open("wing", "WINGMAN");
await wing.waitForFunction(() => /HOST is in the room/.test(document.getElementById("chatLog").textContent), null, { timeout: 30000 });
await host.waitForFunction(() => /WINGMAN is in the room/.test(document.getElementById("chatLog").textContent), null, { timeout: 30000 });
check("both start with voice off", (await host.$eval("#voice", (el) => el.value)) === "off" && (await wing.$eval("#voice", (el) => el.value)) === "off", null);

await choose(host, "on");
await sleep(2);
check("the other's control lights up when one opts in", await lit(wing), await voiceState(wing));
check("and says who", /HOST opted in/.test(await voiceState(wing)), await voiceState(wing));
check("the one who opted in waits", /no voice until WINGMAN/.test(await voiceState(host)), await voiceState(host));
check("no voice link yet", Object.keys(await links(host)).length === 0 && Object.keys(await links(wing)).length === 0, null);

await choose(wing, "ptt");
await host.waitForFunction(() => { const l = window.__wcVoiceLinks(); return Object.values(l).some((x) => x.state === "connected"); }, null, { timeout: 30000 }).catch(() => {});
await sleep(2);
const lh = await links(host), lw = await links(wing);
check("voice connects both ways once everybody has opted in", lh[1] && lh[1].state === "connected" && lw[0] && lw[0].state === "connected", [lh, lw]);
check("each has a remote audio track", lh[1] && lh[1].hearing && lw[0] && lw[0].hearing, null);
check("the states say so", /voice connected with WINGMAN/.test(await voiceState(host)) && /voice connected with HOST/.test(await voiceState(wing)) && /hold `/.test(await voiceState(wing)), [await voiceState(host), await voiceState(wing)]);
check("the lights are out", !(await lit(host)) && !(await lit(wing)), null);
check("the choice is remembered", (await wing.evaluate(() => localStorage.getItem("wc:voice"))) === "ptt", null);

// Push to talk is ` held; Shift+` goes on to the game (the high-resolution flips are on it then), untouched, and a `
// typed into one of the page's boxes is typed.
{
  const micOn = () => wing.evaluate(async () => (await window.__wcVoiceReport()).microphone.tracks.some((t) => t.enabled));
  await wing.evaluate(() => { window.__keys = []; document.addEventListener("keydown", (e) => window.__keys.push(`${e.shiftKey ? "Shift+" : ""}${e.code}${e.defaultPrevented ? " prevented" : ""}`)); });
  await wing.evaluate(() => document.activeElement && document.activeElement.blur());
  await wing.keyboard.down("Backquote");
  const talking = await micOn();
  await wing.keyboard.up("Backquote");
  const after = await micOn();
  check("` held talks, and stops when let go", talking && !after, { talking, after });
  await wing.keyboard.down("Shift");
  await wing.keyboard.down("Backquote");
  const shifted = await micOn();
  await wing.keyboard.up("Backquote");
  await wing.keyboard.up("Shift");
  const keys = await wing.evaluate(() => window.__keys);
  check("Shift+` does not talk and goes on to the game, untouched", !shifted && keys.includes("Shift+Backquote") && !keys.includes("Backquote"), { shifted, keys });
  await wing.click("#chatInput");
  await wing.keyboard.type("a`b");
  const typed = await wing.$eval("#chatInput", (el) => el.value);
  check("a ` typed into the chat box is typed", typed === "a`b" && !(await micOn()), typed);
  await wing.$eval("#chatInput", (el) => { el.value = ""; el.blur(); });
  // ... and the slash talks too, by the same rules; the voice stays on while either key is held
  await wing.evaluate(() => { window.__keys = []; });
  await wing.keyboard.down("Slash");
  const slashTalks = await micOn();
  await wing.keyboard.down("Backquote");
  await wing.keyboard.up("Slash");
  const stillTalking = await micOn();
  await wing.keyboard.up("Backquote");
  const slashAfter = await micOn();
  check("/ held talks too, and the voice stays on while either key is held", slashTalks && stillTalking && !slashAfter, { slashTalks, stillTalking, slashAfter });
  await wing.keyboard.down("Shift");
  await wing.keyboard.down("Slash");
  const shiftSlash = await micOn();
  await wing.keyboard.up("Slash");
  await wing.keyboard.up("Shift");
  const keys2 = await wing.evaluate(() => window.__keys);
  await wing.click("#chatInput");
  await wing.keyboard.type("1/3");
  const typed2 = await wing.$eval("#chatInput", (el) => el.value);
  await wing.$eval("#chatInput", (el) => { el.value = ""; el.blur(); });
  check("Shift+/ and a / typed into a box are the game's and the box's", !shiftSlash && keys2.includes("Shift+Slash") && !keys2.includes("Slash") && typed2 === "1/3", { shiftSlash, keys2, typed2 });
}

// Exercise the previously silent PTT -> Always path, and check decoded audio
// rather than accepting the existence of a remote track as proof of sound.
await choose(wing, "on");
let reports;
const deadline = Date.now() + 15000;
do {
  reports = await Promise.all([host, wing].map((p) => p.evaluate(() => window.__wcVoiceReport())));
  if (reports.every((r) => r.peers.some((p) => p.receivedEnergy > 0 && p.playback === "playing"))) break;
  await sleep(0.2);
} while (Date.now() < deadline);
check("non-silent voice arrives and playback starts both ways", reports.every((r) => r.peers.some((p) => p.receivedEnergy > 0 && p.playback === "playing")), reports);

// The game's sound under a voice: the host's page lowers the game while one of the microphone's vowels comes in and
// lets it back up in the pauses between them.
async function duckSeen(page, seconds) {
  const seen = { lowered: false, raised: false, levels: [] };
  const end = Date.now() + seconds * 1000;
  while (Date.now() < end && !(seen.lowered && seen.raised)) {
    const r = await page.evaluate(() => window.__wcVoiceReport());
    const level = Math.max(0, ...r.peers.map((p) => p.voiceLevel || 0));
    if (seen.levels.length < 40) seen.levels.push(+level.toFixed(3));
    if (r.gameUnderVoices.lowered) seen.lowered = true;
    else if (seen.lowered) seen.raised = true;
    await sleep(0.05);
  }
  return seen;
}
const duck1 = await duckSeen(host, 15);
check("the game is lowered while the other's voice comes in, and back up after", duck1.lowered && duck1.raised, duck1);
await host.selectOption("#voiceDuck", "off");
const duck2 = await duckSeen(host, 4);
check("left as it is when the pilot says so", !duck2.lowered, duck2);
check("which is remembered", (await host.evaluate(() => localStorage.getItem("wc:voiceDuck"))) === "off", null);
await host.selectOption("#voiceDuck", "lower");

await choose(host, "off");
await sleep(2);
check("one player off ends the voice for both", Object.keys(await links(host)).length === 0 && Object.keys(await links(wing)).length === 0, [await links(host), await links(wing)]);
check("the one still in is told", /no voice until HOST/.test(await voiceState(wing)), await voiceState(wing));
check("and the one who went off sees the light", await lit(host), await voiceState(host));

// Fly with a voice on one side only: both chats say so.
await host.waitForFunction(() => !document.getElementById("fly").disabled, null, { timeout: 30000 });
await host.click("#fly");
await sleep(6);
check("Fly warns the one who is off", /WINGMAN opted into voice and you are off: no voice this session/.test(await chat(host)), (await chat(host)).slice(-200));
check("Fly warns the one who opted in", /No voice this session: HOST did not opt in/.test(await chat(wing)), (await chat(wing)).slice(-200));
// Opting in while the game runs: voice comes up beside it.
await wing.waitForFunction(() => !!window.DOSBox, null, { timeout: 60000 }).catch(() => {});
await choose(host, "on");
await host.waitForFunction(() => Object.values(window.__wcVoiceLinks()).some((x) => x.state === "connected"), null, { timeout: 30000 }).catch(() => {});
await sleep(1);
const lh2 = await links(host), lw2 = await links(wing);
check("voice comes up while both games run", lh2[1] && lh2[1].state === "connected" && lw2[0] && lw2[0].state === "connected" && (await host.evaluate(() => !!window.DOSBox)), [lh2, lw2]);
// ... and the game's own sound goes down under it: the gain the page put between the emulator's sound and the output.
{
  const gains = [];
  const end = Date.now() + 20000;
  let low = false, high = false;
  while (Date.now() < end && !(low && high)) {
    const g = await host.evaluate(() => window.__wcGameLevel());
    if (gains.length < 60) gains.push(g.gain === null ? (g.node ? "node" : null) : +g.gain.toFixed(2));
    if (g.gain !== null && g.gain < 0.5) low = true;
    else if (low && g.gain !== null && g.gain > 0.9) high = true;
    await sleep(0.05);
  }
  check("the running game's sound is lowered under the voice and comes back", low && high, gains);
  // (and the emulator's sound still runs through it: its node is asked for sound, as it was connected straight out)
  const asked = await host.evaluate(() => new Promise((done) => {
    const node = window.DOSBox.SDL2.audio.scriptProcessorNode, was = node.onaudioprocess;
    let n = 0;
    node.onaudioprocess = (e) => { n++; was.call(node, e); };
    setTimeout(() => { node.onaudioprocess = was; done(n); }, 1000);
  }));
  check("the emulator's sound still plays through the page's gain", asked > 5, asked);
}
await browser.close();
console.log(results.every(Boolean) ? "VOICE OK" : "VOICE FAILED");
process.exit(results.every(Boolean) ? 0 : 1);
