// GOG's Wing Commanders dropped on the page, whichever edition (the user, 2026-11-15: GOG's Wing Commanders became
// "35th Anniversary Edition" packs with the old release beside them as "Legacy Edition"; "If you detect a strange
// version can you instruct the user to download Legacy Edition from GOG instead", then, of the Anniversary
// installers: "Ideally we just make them work").  The page's own file input is given: the Legacy Edition's installer
// and the two Anniversary Editions' (1.1 GB and 850 MB: the game in their DOS/ directory, the same files as the
// Legacy Edition's, known to the hooks, nothing to say); then what is no known release and gets the advice: WC.EXE
// with the hooks' string changed (another build), a folder with the game's data but none of its programs, something
// that is no installer under an Anniversary Edition's name (Wing Commander's and Wing Commander II's); and a game
// that is no Wing Commander (no advice).
//
//   GAME_FILE=<Legacy Edition installer> [ANN1=<Wing Commander's Anniversary installer>] [ANN2=<Wing Commander II's>]
//     node scripts/web-legacy.mjs PAGE_URL
//
// PLAYWRIGHT_DIR and CHROME as scripts/web-rv.mjs has them.  No lobby server is needed.
import { createRequire } from "node:module";
import path from "node:path";
import fs from "node:fs";
import zlib from "node:zlib";
import { createHash } from "node:crypto";

const [pageUrl] = process.argv.slice(2);
const gameFile = process.env.GAME_FILE;
if (!pageUrl || !gameFile) { console.error("usage: GAME_FILE=installer node web-legacy.mjs PAGE_URL"); process.exit(2); }
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), "package.json"));
const { chromium } = require("playwright");
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME || "/usr/bin/google-chrome", args: ["--no-sandbox"] });
const checks = [];
const check = (what, ok, detail = "") => { checks.push({ what, ok }); console.log(`${ok ? "ok  " : "FAIL"} ${what}${detail ? ": " + detail : ""}`); };

// a .zip of stored files
function zip(files) {
  const parts = [], dir = [];
  let at = 0;
  for (const [name, data] of files) {
    const n = Buffer.from(name), crc = zlib.crc32(data);
    const h = Buffer.alloc(30);
    h.writeUInt32LE(0x04034b50, 0); h.writeUInt16LE(20, 4); h.writeUInt32LE(crc, 14); h.writeUInt32LE(data.length, 18); h.writeUInt32LE(data.length, 22); h.writeUInt16LE(n.length, 26);
    const c = Buffer.alloc(46);
    c.writeUInt32LE(0x02014b50, 0); c.writeUInt16LE(20, 4); c.writeUInt16LE(20, 6); c.writeUInt32LE(crc, 16); c.writeUInt32LE(data.length, 20); c.writeUInt32LE(data.length, 24);
    c.writeUInt16LE(n.length, 28); c.writeUInt32LE(at, 42);
    parts.push(h, n, data); dir.push(c, n);
    at += 30 + n.length + data.length;
  }
  const d = Buffer.concat(dir), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(files.length, 8); end.writeUInt16LE(files.length, 10); end.writeUInt32LE(d.length, 12); end.writeUInt32LE(at, 16);
  return Buffer.concat([...parts, d, end]);
}

async function drop(name, buffer, want = "") {
  const context = await browser.newContext();
  const page = await context.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.goto(pageUrl + (pageUrl.includes("?") ? "&" : "?") + "server=http://127.0.0.1:9/");
  await page.waitForFunction(() => document.getElementById("sourceStatus").dataset.state === "ready", null, { timeout: 180000 });
  await page.evaluate(() => { document.getElementById("sourceStatus").dataset.state = ""; });
  // (a file's path for a big one, its bytes for one made here)
  const t0 = Date.now();
  await page.setInputFiles("#gamefile", buffer ? { name, mimeType: "application/octet-stream", buffer } : name);
  await page.waitForFunction(() => ["ready", "error"].includes(document.getElementById("sourceStatus").dataset.state), null, { timeout: 180000 });
  const got = await page.evaluate(() => {
    const el = document.getElementById("sourceStatus"), a = el.querySelector("a");
    return { state: el.dataset.state, text: el.textContent, href: a ? a.href : "", program: document.getElementById("program").value };
  });
  // the installed game's files (the copy the browser kept): their hashes, and their bytes when the next cases need them
  got.files = await page.evaluate(async (bytes) => {
    const { loadGames } = await import("./gamefiles.js");
    const g = (await loadGames()).find((x) => x.id === "wc1" || x.id === "wc2");
    if (!g) return null;
    const hex = (b) => Array.from(new Uint8Array(b), (x) => x.toString(16).padStart(2, "0")).join("");
    const b64 = (d) => { let t = ""; for (let i = 0; i < d.length; i += 32768) t += String.fromCharCode(...d.subarray(i, i + 32768)); return btoa(t); };
    return Promise.all(g.files.map(async (f) => [f.path, bytes ? b64(f.data) : hex(await crypto.subtle.digest("SHA-1", f.data))]));
  }, want === "bytes").catch(() => null);
  got.errors = errors;
  got.seconds = (Date.now() - t0) / 1000;
  got.toJSON = () => ({ state: got.state, text: got.text, href: got.href, program: got.program });
  if (want === "page") { got.page = page; got.close = () => context.close(); return got; }
  await context.close();
  return got;
}
const WC1 = "wing_commander_35th_anniversary_edition", WC2 = "wing_commander_ii_vengeance_of_the_kilrathi_35th_anniversary_edition";

// 1. the Legacy Edition's installer: known, nothing to say
const legacy = await drop(gameFile, null, "bytes");
check("the Legacy Edition installer is the game, no advice", legacy.state === "ready" && legacy.program === "wc1" && !/Legacy Edition/.test(legacy.text) && !legacy.href, JSON.stringify(legacy.text));
const files = (legacy.files || []).map(([p, d]) => [p, Buffer.from(d, "base64")]);
const sha = (d) => createHash("sha1").update(d).digest("hex");
check("its files read back", files.some(([p]) => p === "WC.EXE"), `${files.length} files`);

// 1b. the Anniversary Editions' installers: the same game, found in their DOS directory
const same = (a, b) => {   // (a: paths and bytes, b: paths and hashes)
  const m = new Map(b);
  return a.filter(([p, d]) => m.get(p) !== sha(d)).map(([p]) => p).concat(b.filter(([p]) => !a.some(([q]) => q === p)).map(([p]) => "+" + p));
};
if (process.env.ANN1) {
  const ann = await drop(process.env.ANN1, null, "page");
  check("Wing Commander's Anniversary Edition installer is the game, no advice", ann.state === "ready" && ann.program === "wc1" && ann.text === "" && !ann.errors.length,
        `${JSON.stringify(ann.text)} in ${ann.seconds.toFixed(1)} s, ${(ann.files || []).length} files`);
  const diff = same(files, ann.files || []);
  // (but its sound setting, the Roland's, which the page changes when the game is flown; a blank saved game for each
  // program; GOG's DOSBox settings and Steam's file, which the game never reads)
  const expected = ["CRUSADE.CFG", "WINGCMDR.CFG", "+GAMEDAT/CRUSADE.WLD", "+GAMEDAT/SAVEGAME.WLD", "+GAMEDAT/steam_autocloud.vdf", "+dosboxWC.conf", "+dosboxWC_single.conf"];
  check("its files are the Legacy Edition's but for its settings", (ann.files || []).length > 80 && diff.every((p) => expected.includes(p)), diff.join(" ") || "all the same");
  const cfg = await ann.page.evaluate(async () => {
    const { gameById } = await import("./gamefiles.js");
    const made = {}, enc = (t) => new TextEncoder().encode(t);
    const said = [gameById("wc1").prepare({ readFile: () => enc("v r z\r\n\0"), writeFile: (p, d) => { made[p] = Array.from(d); } }, "/c"),
                  gameById("wc1sm2").prepare({ readFile: () => enc("v a904 z\r\n\0"), writeFile: (p, d) => { made["!" + p] = 1; } }, "/c")];
    return { made, said };
  });
  const legacyCfg = JSON.stringify(Array.from(files.find(([p]) => p === "WINGCMDR.CFG")[1]));
  check("flown, its Roland music setting becomes the Legacy Edition's (an AdLib one is left alone)",
        JSON.stringify(cfg.made["/c/WINGCMDR.CFG"]) === legacyCfg && JSON.stringify(cfg.made["/c/CRUSADE.CFG"]) === legacyCfg && Object.keys(cfg.made).length === 2 && cfg.said[1] === "", cfg.said[0]);
  await ann.close();
}
if (process.env.ANN2) {
  const ann = await drop(process.env.ANN2);
  check("Wing Commander II's Anniversary Edition installer is the game, no advice", ann.state === "ready" && ann.program === "wc2" && ann.text === "" && !ann.errors.length,
        `${JSON.stringify(ann.text)} in ${ann.seconds.toFixed(1)} s, ${(ann.files || []).length} files`);
  const names = new Set((ann.files || []).map(([p]) => p));
  check("its three programs and their data", ["WC2.EXE", "SO1.EXE", "SO2.EXE", "wc2.cfg", "GAMEDAT/SERIES.S00"].every((n) => names.has(n)) && (ann.files || []).length < 600, `${names.size} files`);
}

// 2. its WC.EXE with the string the hooks look for changed: another build
const other = files.map(([p, d]) => {
  if (p !== "WC.EXE") return [p, d];
  const e = Buffer.from(d), at = e.indexOf("Loading WING COMMANDER");
  e.write("Loading Wing Commander", at);
  return [p, e];
});
const strange = await drop("my-wc-folder.zip", zip(other));
check("another build of WC.EXE: single-player, and the Legacy Edition named with its store page",
      strange.state === "ready" && /not the build/.test(strange.text) && /"Wing Commander - Legacy Edition"/.test(strange.text) && strange.href.endsWith("/" + WC1), JSON.stringify(strange));

// 3. the game's data with a program of another name
const renamed = files.filter(([p]) => !/^(WC|SM2)\.EXE$/.test(p)).concat([["WINGCMDR.EXE", files.find(([p]) => p === "WC.EXE")[1]]]);
const nogame = await drop("games.zip", zip(renamed));
check("the game's data without its program: not a known release, the Legacy Edition named",
      /not a release of Wing Commander this page knows/.test(nogame.text) && /"Wing Commander - Legacy Edition"/.test(nogame.text) && nogame.href.endsWith("/" + WC1), JSON.stringify(nogame));

// 4. no installer the page can unpack, under the Anniversary Editions' names
const junk = Buffer.concat([Buffer.from("MZ"), Buffer.alloc(4096, 7)]);
const ann1 = await drop(`setup_${WC1}_v1.0_(99999).exe`, junk);
check("an Anniversary Edition installer that cannot be unpacked: the Legacy Edition named",
      ann1.state === "error" && /Could not use/.test(ann1.text) && /"Wing Commander - Legacy Edition"/.test(ann1.text) && ann1.href.endsWith("/" + WC1), JSON.stringify(ann1));
const ann2 = await drop(`setup_${WC2}_v1.0_(99999).exe`, junk);
check("Wing Commander II's: its own Legacy Edition and store page",
      ann2.state === "error" && /"Wing Commander II - Legacy Edition"/.test(ann2.text) && ann2.href.endsWith("/" + WC2), JSON.stringify(ann2));

// 5. not a Wing Commander of this page's: nothing about a Legacy Edition
const foo = await drop("setup_wing_commander_iii_1.4.zip", zip([["FOO.EXE", Buffer.from("MZ" + "x".repeat(64))], ["DATA.BIN", Buffer.alloc(100)]]));
check("another game: no advice", /no game I recognise|FOO/i.test(foo.text) && !/Legacy Edition/.test(foo.text) && !foo.href, JSON.stringify(foo));

// 6. the names
const page = await (await browser.newContext()).newPage();
await page.goto(pageUrl);
const names = await page.evaluate(async (list) => {
  const { legacyOf } = await import("./gamefiles.js");
  return list.map((n) => legacyOf(n));
}, ["setup_wing_commander_1.01_gog_v1_(76764).exe", "setup_wing_commander_i_35th_anniversary_edition_v1.0_(95103).exe",
    "setup_wing_commandertm_ii_vengeance_of_the_kilrathi_35th_anniversary_edition_v1.0_(95103).exe",
    "setup_wing_commander_ii_1.0_gog_v1.exe", "setup_wing_commander_iii_1.4_cs_(76763).exe", "setup_wing_commander_privateer_1.01.exe",
    "setup_wing_commander_iv_30th.exe", "3dtv12.zip", "Wing Commander 2.zip"]);
check("names told apart", JSON.stringify(names) === JSON.stringify(["wc1", "wc1", "wc2", "wc2", "", "", "", "", "wc2"]), JSON.stringify(names));
check("no page errors", [legacy, strange, nogame, ann1, ann2, foo].every((g) => !g.errors.length), JSON.stringify([legacy, strange, nogame, ann1, ann2, foo].map((g) => g.errors)));

await browser.close();
const bad = checks.filter((c) => !c.ok);
console.log(bad.length ? `LEGACY FAILED (${bad.length} of ${checks.length})` : `LEGACY OK (${checks.length})`);
process.exit(bad.length ? 1 : 0);
