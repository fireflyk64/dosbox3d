// The page's loose finder (web/gamefiles.js identifyGame, gatherFiles, expandInnerZips) on archives laid out the ways
// people zip a game: a level deeper, the files in other folders than the program, other cases, a zip in a zip.
//
//   node scripts/web-gather.mjs RAPTOR_ZIP TV_FOLDER      (Steam's Raptor zipped; the registered TV unpacked)
//
// Ends "GATHER OK".
import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { readZip, identifyGame, expandSplitZips, expandInnerZips } from "../web/gamefiles.js";

const [rapZip, tvDir] = process.argv.slice(2);
let failed = 0;
const check = (ok, what) => { console.log(`${ok ? "OK  " : "FAIL"} ${what}`); if (!ok) failed++; };
const names = (id) => new Set(id.files.map((f) => f.path.toUpperCase()));
const find = async (files) => {
  files = await expandSplitZips(files);
  let id = identifyGame(files);
  if (!id.game) { const more = await expandInnerZips(files); if (more !== files) id = identifyGame(more); }
  return id;
};
const RAP = ["RAP.EXE", "FILE0000.GLB", "FILE0001.GLB", "FILE0002.GLB", "FILE0003.GLB", "FILE0004.GLB", "FILE_ID.DIZ", "SETUP.INI"];
const hasAll = (id, list) => list.every((n) => names(id).has(n));

// Raptor as Steam's folder zipped
const rap = await readZip(new Uint8Array(fs.readFileSync(rapZip)));
let id = await find(rap);
check(id.game && id.game.id === "rap" && hasAll(id, RAP) && !id.gathered.length, `Raptor, Steam's folder zipped: found in "${id.root}", ${id.files.length} files, nothing moved`);
const game = rap.filter((f) => RAP.includes(path.posix.basename(f.path).toUpperCase()) && !/dosbox\//i.test(f.path));
const lay = (fn) => game.map((f) => ({ path: fn(path.posix.basename(f.path)), data: f.data }));
// shallower: everything at the top
id = await find(lay((n) => n));
check(id.game && id.game.id === "rap" && id.root === "" && hasAll(id, RAP), "Raptor, the files at the archive's top");
// deeper, other folder names
id = await find(lay((n) => `My Backups/DOS/Games (old)/RAPTOR/${n}`));
check(id.game && id.game.id === "rap" && hasAll(id, RAP), `Raptor, four folders deep ("${id.root}")`);
// lower case
id = await find(lay((n) => `raptor/${n.toLowerCase()}`));
check(id.game && id.game.id === "rap" && hasAll(id, RAP), "Raptor, lower-case names");
// the data beside, above and below the program
id = await find(lay((n) => n === "RAP.EXE" ? `Raptor/bin/${n}` : /GLB$/.test(n) ? `Raptor/data/${n}` : n === "FILE_ID.DIZ" ? n : `Raptor/bin/cfg/${n}`));
check(id.game && id.game.id === "rap" && id.root === "Raptor/bin" && hasAll(id, RAP) && id.gathered.length === 7,
      `Raptor, the program in one folder and its files in others: ${id.gathered.length} gathered (${id.gathered.slice(0, 2).join("; ")} ...)`);
// two copies of a file: the one nearest the program
const two = lay((n) => `A/${n}`).concat([{ path: "B/FILE0001.GLB", data: new Uint8Array(3) }]);
id = await find(two.filter((f) => f.path !== "A/FILE0001.GLB").concat([{ path: "A/data/FILE0001.GLB", data: game.find((f) => /FILE0001/i.test(f.path)).data }]));
check(id.game && id.files.find((f) => f.path.toUpperCase() === "FILE0001.GLB").data.length > 3, "Raptor, a file twice in the archive: the one under the program's folder taken");
// a zip in a zip
const tmp = fs.mkdtempSync("/tmp/gather-");
fs.copyFileSync(rapZip, path.join(tmp, "inner game.zip"));
execFileSync("python3", ["-c", "import zipfile; zipfile.ZipFile('outer.zip', 'w').write('inner game.zip')"], { cwd: tmp });
id = await find(await readZip(new Uint8Array(fs.readFileSync(path.join(tmp, "outer.zip")))));
check(id.game && id.game.id === "rap" && hasAll(id, RAP), `Raptor, a zip inside a zip ("${id.root}")`);
fs.rmSync(tmp, { recursive: true });

// Terminal Velocity, the registered game: the cinematics loose, the archives in another folder
const tv = [];
const walk = (d, rel) => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const r = rel ? `${rel}/${e.name}` : e.name; if (e.isDirectory()) walk(path.join(d, e.name), r); else tv.push({ path: r, data: new Uint8Array(fs.readFileSync(path.join(d, e.name)).subarray(0, 65536)) }); } };
walk(tvDir, "");
id = await find(tv.map((f) => ({ path: "tv/" + f.path, data: f.data })));
const TV = ["TV.EXE", "GAME.EXE", "CDROM.POD", "STARTUP.POD", "CDROM.INI", "SB16.COM", "SHARE.VOX", "TVI/LOGO.TVI"];
check(id.game && id.game.id === "tv" && hasAll(id, TV) && !id.gathered.length, "Terminal Velocity, the installed folder a level deeper: nothing moved");
id = await find(tv.map((f) => ({ path: /\.POD$/i.test(f.path) ? `DISK2/${f.path}` : /\.TVI$/i.test(f.path) ? `movies/${path.posix.basename(f.path)}` : /\.EXE$/i.test(f.path) ? `TERMVEL/${f.path}` : `TERMVEL/extra/${f.path}`, data: f.data })));
check(id.game && id.game.id === "tv" && id.root === "TERMVEL" && hasAll(id, TV), `Terminal Velocity, its archives, cinematics and drivers in other folders: ${id.gathered ? id.gathered.length : 0} gathered`);
console.log(failed ? `GATHER FAILED (${failed})` : "GATHER OK");
process.exit(failed ? 1 : 0);
