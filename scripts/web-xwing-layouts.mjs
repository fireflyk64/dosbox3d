// X-Wing's files found the way the page finds them (web/gamefiles.js: expandCdImages, identifyGame, gatherDisc and
// the registry's xwDisc) in the layouts a copy may come in: GOG's installer unpacked as the page unpacks it (the test
// installer scripts/xwing-gog-exe.py writes from your own CD), its image renamed, in other folders, above the game,
// in an installer's own folder, in the other sector layouts; the disc's files loose beside the game (as TIE Fighter's
// GOG installer has them) or in a folder of their own; the image by itself; another disc's image beside it.
//
//   node scripts/web-xwing-layouts.mjs INSTALLER.exe XWING_ISO [XWNOCP_FOLDER]
//
// Ends "XWING LAYOUTS OK".
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { identifyGame, expandCdImages, gameById, isoImage } from "../web/gamefiles.js";

const [exe, isoPath, nocp] = process.argv.slice(2);
let failed = 0;
const check = (ok, what) => { console.log(`${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };
const xw = gameById("xwing");
const walk = (d, rel = "", out = []) => {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const r = rel ? `${rel}/${e.name}` : e.name;
    if (e.isDirectory()) walk(path.join(d, e.name), r, out); else out.push({ path: r, data: new Uint8Array(fs.readFileSync(path.join(d, e.name))) });
  }
  return out;
};
// (what the page does with dropped files: wc.js useFiles, then the disc the launch mounts: wc.js's imgmount / mount)
const find = (files) => {
  const id = identifyGame(expandCdImages(files));
  const disc = id.game && id.game.disc ? id.game.disc(id.files) : null;
  return { id, disc, names: new Set(id.files.map((f) => f.path.toUpperCase())) };
};
const ofDisc = (r) => (r.disc && r.disc.image ? `image ${r.disc.image}` : r.disc ? `folder "${r.disc.folder}"` : "none");
const isXwingImage = (r) => {
  if (!r.disc || !r.disc.image) return false;
  const f = r.id.files.find((g) => g.path === r.disc.image);
  const iso = f && isoImage((p, n) => f.data.subarray(p, p + n));
  return !!(iso && iso.find("BWING.EXE") && iso.find("MISSION"));
};

// 1. GOG's layout: the test installer unpacked with the worker's own options (web/inno-worker.js)
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "xwlay-"));
execFileSync("innoextract", ["--extract", "--quiet", "--gog", "--exclude-temp", "--language", "en-US", "--output-dir", tmp, exe]);
const gog = walk(tmp);
fs.rmSync(tmp, { recursive: true });
let r = find(gog);
const junk = [...r.names].filter((n) => /^(DOSBOX|__SUPPORT|__REDIST|APP|COMMONAPPDATA|TMP)\/|^GOGGAME-/.test(n));
check(r.id.game === xw && r.id.root === "" && r.disc && r.disc.image === "game.gog" && isXwingImage(r) && !junk.length,
      `GOG's layout (${gog.length} files unpacked): X-Wing at the top, ${r.id.files.length} game files, the CD ${ofDisc(r)}, no installer files (${junk.join(", ") || "none"})`);
check(!r.id.gathered.length, "GOG's layout: nothing had to be moved");
const image = gog.find((f) => f.path === "game.gog").data;
const hd = gog.filter((f) => !f.path.includes("/") && !/^game\.(gog|ins)$|^goggame-/i.test(f.path));
const iso = new Uint8Array(fs.readFileSync(isoPath));

// 2. the image under other names, without the cue sheet
for (const name of ["XWING.GOG", "disc.bin", "X-Wing CD (1994).img", "xwingcd", "CD.ISO"]) {
  r = find([...hd, { path: name, data: image }]);
  check(r.id.game === xw && r.disc && r.disc.image === name, `the image named "${name}": ${ofDisc(r)}`);
}
// 3. the image in a folder of the game's
for (const p of ["CD/game.gog", "Disc Image/X-Wing.iso", "images/cd1/xwing.bin"]) {
  r = find([...hd, { path: p, data: image }]);
  check(r.id.game === xw && r.disc && r.disc.image === p, `the image at "${p}": ${ofDisc(r)}`);
}
// 4. the game a folder down, the image above it (and beside it), as other installers or zips might have it
for (const [game, img] of [["XWING", "game.gog"], ["Star Wars - X-Wing CD (1994)", "CD/xwing.iso"], ["GOG Games/X-Wing/XWING", "GOG Games/X-Wing/game.gog"]]) {
  r = find([...hd.map((f) => ({ path: `${game}/${f.path}`, data: f.data })), { path: img, data: image }]);
  check(r.id.game === xw && r.id.root === game && r.disc && r.disc.image && isXwingImage(r) && !r.names.has("MISSION"),
        `the game in "${game}/", the image at "${img}": root "${r.id.root}", ${ofDisc(r)} (${r.id.gathered.join("; ")})`);
}
// 5. the image in an installer's own folder (left out of the game's files as furniture, found all the same)
r = find([...hd, { path: "__support/game.gog", data: image }, { path: "DOSBOX/DOSBox.exe", data: new Uint8Array(16) }]);
check(r.id.game === xw && isXwingImage(r) && !r.names.has("DOSBOX/DOSBOX.EXE"), `the image in __support/: ${ofDisc(r)} (${r.id.gathered.join("; ")})`);
// 6. the other sector layouts the emulator mounts: 2048-byte sectors (the .iso as it is), raw MODE2 (data at 24)
const sectors = iso.length / 2048;
const mode2 = new Uint8Array(sectors * 2352);
for (let s = 0; s < sectors; s++) mode2.set(iso.subarray(s * 2048, s * 2048 + 2048), s * 2352 + 24);
for (const [what, data] of [["2048-byte sectors", iso], ["raw MODE2 sectors (data at 24)", mode2]]) {
  r = find([...hd, { path: "game.gog", data }]);
  check(r.id.game === xw && isXwingImage(r), `game.gog of ${what}: ${ofDisc(r)}`);
}
// 7. another disc's image beside X-Wing's (bigger, nearer the top): X-Wing's is the CD
const other = new Uint8Array(20e6);
other.set(iso.subarray(0, 17 * 2048));
other.fill(0, 18 * 2048);                       // (its root directory: empty)
r = find([...hd, { path: "aaa-other.iso", data: other }, { path: "CD/xwing.iso", data: iso }]);
check(r.id.game === xw && r.disc && r.disc.image === "CD/xwing.iso", `another disc's image beside it: ${ofDisc(r)}`);
// 8. the image by itself (the CD's top holds the installed game's files)
r = find([{ path: "xwing.iso", data: iso }]);
check(r.id.game === xw && r.id.root === "" && r.disc && r.disc.image === "xwing.iso" && r.names.has("BWING.EXE"), `the image by itself: ${r.id.files.length} files, ${ofDisc(r)}`);
// 9. the disc's files loose beside the game's (TIE Fighter's GOG layout: MISSION/, RESOURCE/ ... at the top)
const files = (dir) => {
  const out = [];
  const img = isoImage((p, n) => iso.subarray(p, p + n));
  const rec = (d, rel) => { for (const e of img.list(d)) { const p = rel ? `${rel}/${e.name}` : e.name; if (e.dir) rec(e, p); else out.push({ path: (dir ? dir + "/" : "") + p, data: img.read(e.extent, e.size) }); } };
  rec(img.root, "");
  return out;
};
const loose = files("").filter((f) => f.path.includes("/"));
r = find([...hd, ...loose]);
check(r.id.game === xw && r.disc && r.disc.folder === "", `the disc's folders loose beside the game (${loose.length} files): ${ofDisc(r)}`);
// 10. the disc's files in a folder beside the game's folder
r = find([...hd.map((f) => ({ path: `XWING/${f.path}`, data: f.data })), ...files("XWINGCD")]);
check(r.id.game === xw && r.id.root === "XWING" && r.disc && r.disc.folder === "CD" && r.names.has("CD/MISSION/DEFECT.XWI"),
      `the game in XWING/, the disc's files in XWINGCD/: ${ofDisc(r)} (${r.id.gathered.join("; ")})`);
// 11. your installed copy with the CD copied into it (XWNOCP: CD/ under the game's folder)
if (nocp) {
  r = find(walk(nocp).map((f) => ({ path: `XWNOCP/${f.path}`, data: f.data })));
  check(r.id.game === xw && r.id.root === "XWNOCP" && r.disc && r.disc.folder === "CD", `the installed copy with CD/ inside: ${ofDisc(r)}`);
}
console.log(failed ? `XWING LAYOUTS FAILED (${failed})` : "XWING LAYOUTS OK");
process.exit(failed ? 1 : 0);
