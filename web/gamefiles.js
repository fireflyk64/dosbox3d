// Bringing your own game files: everything stays in the browser.
//
//   readZip(bytes)              -> [{path, data}] from a .zip (stored/deflate)
//   extractInstaller(bytes, cb) -> [{path, data}] from an Inno Setup / GOG
//                                  installer, using innoextract compiled to
//                                  WebAssembly in a worker (inno-worker.js)
//   identifyGame(files)         -> which game this is and how to run it
//   saveGame / loadGame / forgetGame: IndexedDB cache so the files only have
//                                  to be dropped once per browser
//
// The game registry is the only game-specific knowledge here: a game is
// recognised by an executable, run by a DOS command, and flagged when the
// emulator's multiplayer hooks apply to it (Wing Commander 1 and 2).
// Optional: `campaign`, the missions the host can pick for everybody: the
// series as the game numbers them, each with a name and its missions (a
// count, or one note per mission), and how a picked mission reaches the game:
// the hooks' MIS / SERIES environment (Wing Commander), or `missionArgs`,
// arguments for the game's own command line (Wing Commander II's developer
// switches), with `hints` for the page's lines about it.  `saves`, the files the game keeps its saved games in (relative to
// the game directory; the page keeps a copy in the browser and offers them as
// a download), and `cycles`, the emulated CPU speed the game plays well at
// (DOSBox's default of 3000 is what Ctrl+F11 / Ctrl+F12 adjust; Wing
// Commander 1 has no frame limiter, so its cutscenes and menus take their
// speed from this, while in flight the hooks hold 20 frames a second with
// 16000 cycles: WCFPS and WCFLIGHTCYCLES in the environment), and `pointer`,
// for a controller's stick (web/gamepad.js): where the mouse pointer rests
// when the game steers by it and how far it reaches, as fractions of the
// mouse range.  Both games park the pointer in the middle of the cockpit's
// window, which is another rectangle in every ship (318,52 of 640x200 in
// Wing Commander's Hornet, 34 up in its Rapier) and in every turret, and
// turn in steps by its distance from there: `fromGame` asks the running
// game for both (web/wc.js, steeringPointer).  Fixed figures {x, y, rx, ry}
// are the other form, for a game the hooks know nothing about.
// `firstName`: the game's people use the pilot's first name, and the page
// asks for one.
// `dosbox`: what the game's own setup expects of the machine, as the text of
// a DOSBox configuration file.

// Wing Commander's Vega campaign: series 1.. and mission 0.. within the
// series (what the MIS / SERIES environment of the hooks takes).  The system
// of each series and the number of missions were read off the game's own
// briefing screens (an index past the last mission shows a black screen);
// series 14 and up do not exist in WC.EXE.  40 missions in all.
const WC1_SERIES = [
  { series: 1, name: "Enyo", missions: 2 },
  { series: 2, name: "McAuliffe", missions: 3 },
  { series: 3, name: "Gateway", missions: 3 },
  { series: 4, name: "Gimle", missions: 3 },
  { series: 5, name: "Brimstone", missions: 3 },
  { series: 6, name: "Chengdu", missions: 3 },
  { series: 7, name: "Dakota", missions: 3 },
  { series: 8, name: "Port Hedland", missions: 3 },
  { series: 9, name: "Kurasawa", missions: 3 },
  { series: 10, name: "Rostov", missions: 3 },
  { series: 11, name: "Hubble's Star", missions: 3 },
  { series: 12, name: "Venice", missions: 4 },
  { series: 13, name: "Hell's Kitchen", missions: 4 },
];

// Wing Commander II: twelve series of four missions (docs/wc2-port.md has
// the survey).  The note says who the story sends along: the second player
// flies that wingman's ship.  The story flies the others alone: in a
// Broadsword or a Sabre the second player is the GUNNER in the leader's
// turrets, in a ship without turrets a DRONE that rides along.
const DRONE = "flown alone: the second player is a drone";
const GUNNER = "flown alone: the second player is the turret gunner";
const WC2_SERIES = [
  { series: 1, missions: ["with Shadow", "with Shadow", "with Shadow", "with Shadow"] },
  { series: 2, missions: [GUNNER, DRONE, DRONE, DRONE] },
  { series: 3, missions: ["with Hobbes", "with Hobbes", "with Hobbes", "with Hobbes"] },
  { series: 4, missions: ["with Doomsday", "with Doomsday", "with Doomsday", GUNNER] },
  { series: 5, missions: ["with Spirit", "with Spirit", DRONE, "with Spirit"] },
  { series: 6, missions: ["with Stingray", "with Stingray", "with Stingray", "with Stingray"] },
  { series: 7, missions: ["with Angel", "with Angel", "with Angel", DRONE] },
  { series: 8, missions: ["with Jazz", "with Jazz", GUNNER, GUNNER] },
  { series: 9, missions: [DRONE, DRONE, GUNNER, DRONE] },
  { series: 10, missions: ["with Doomsday", "with Doomsday", "with Doomsday", GUNNER] },
  { series: 11, missions: ["with Stingray", "with Stingray", "with Stingray", "with Stingray"] },
  { series: 12, missions: ["with Jazz", "with Jazz", GUNNER, "with the Sabre escort"] },
];

export const GAMES = [
  { id: "wc1", title: "Wing Commander", detect: ["WC.EXE"], run: "wc", multiplayer: true,
    saves: ["GAMEDAT/SAVEGAME.WLD"], cycles: 3630,
    pointer: { fromGame: true },
    campaign: { series: WC1_SERIES, hints: {
      forced: "Everyone flies it from a fresh start with the callsigns entered above.",
      host: "The barracks: your save game and your walk to the briefing decide the mission; wingmen must walk into the briefing room too, and get your mission there.",
      wing: "The host flies from the barracks: after the host starts, walk into the briefing room on your ship and you get the host's mission." } } },
  { id: "wc1sm2", title: "Wing Commander: Secret Missions 2", detect: ["SM2.EXE"], run: "sm2", multiplayer: false, secondary: true },
  // "Origin s<series> m<mission>" on WC2's command line puts the story at
  // that mission and starts in the barracks (a different room from base to
  // base: the door that flies the mission is not always in the same place).
  // "loadfix -34" is how GOG starts it, and it matters: loaded lower in
  // memory the game jumps through a null pointer in some in-flight scenes
  // (after the first autopilot of series 2 mission 2, at the start of
  // others) and hangs.
  // Its sound setup (wc2.cfg, "c25": a Sound Blaster at 220, IRQ 5) is GOG's,
  // and so must the emulated card's be: with DOSBox's IRQ 7 the first spoken
  // line never ends, and the game waits for it for ever.
  { id: "wc2", title: "Wing Commander II", detect: ["WC2.EXE"], run: "loadfix -34 wc2", multiplayer: true,
    saves: ["GAMEDAT/SAVEGAME.WC2"], cycles: 8000,
    firstName: true,
    pointer: { fromGame: true },
    dosbox: "[sblaster]\nirq=5\n",
    campaign: { series: WC2_SERIES, missionArgs: (m) => `Origin s${m.series} m${m.mis}`, hints: {
      forced: "Everyone starts in the barracks with the story at that mission: click the door the game calls \"Fly mission\" (point at a door and it is named), and the briefing plays first. In a mission flown alone the second player is the gunner if the ship has turrets (starting in the rear turret; F2 and F3 are the side turrets, F4 the rear one, F1 the pilot's view), and otherwise a drone riding behind the leader as its copilot: Up and Down shift the leader's shields to the rear and the front, Space puts shields into the guns, Enter guns into the weakest shield, + and - set the leader's cruising speed, and its gauges show the leader's. Nothing sees or hits it; 0, then /chase and Enter, flies free.",
      host: "The barracks: your saved game decides the mission. Click \"Fly mission\" (point at a door and the game names it); wingmen do the same and get your place in the story, your briefing and your mission.",
      wing: "The host flies from the barracks: click \"Fly mission\" (point at a door and the game names it), and you get the host's place in the story, the briefing and the mission." } } },
];

// The registry entry of a game id, or null (a game picked by its executable).
export const gameById = (id) => GAMES.find((g) => g.id === id) || null;

// Directories and file types an installer leaves behind that a DOS game
// never reads (GOG's own DOSBox, redistributables, manuals, icons).
const JUNK_DIRS = new Set(["dosbox", "__redist", "__support", "commonappdata", "tmp", "__macosx"]);
const JUNK_EXT = new Set(["pdf", "ico", "dll", "lnk", "url", "hashdb", "info", "script", "jpg", "jpeg", "png", "gif", "html", "htm", "inf", "ini", "zip", "gz", "tar"]);

// -- zip ---------------------------------------------------------------------

export async function readZip(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let eocd = -1;
  for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 65558); i--) {
    if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error("not a zip file (no end-of-central-directory record)");
  const count = dv.getUint16(eocd + 10, true);
  let off = dv.getUint32(eocd + 16, true);
  if (off === 0xffffffff) throw new Error("zip64 archives are not supported");
  const files = [];
  for (let n = 0; n < count; n++) {
    if (dv.getUint32(off, true) !== 0x02014b50) throw new Error("corrupt zip central directory");
    const flags = dv.getUint16(off + 8, true);
    const method = dv.getUint16(off + 10, true);
    const csize = dv.getUint32(off + 20, true);
    const usize = dv.getUint32(off + 24, true);
    const nlen = dv.getUint16(off + 28, true);
    const elen = dv.getUint16(off + 30, true);
    const clen = dv.getUint16(off + 32, true);
    const local = dv.getUint32(off + 42, true);
    const utf8 = (flags & 0x800) !== 0;
    const name = new TextDecoder(utf8 ? "utf-8" : "latin1").decode(bytes.subarray(off + 46, off + 46 + nlen));
    off += 46 + nlen + elen + clen;
    if (name.endsWith("/")) continue;
    if (flags & 1) throw new Error(`"${name}" is encrypted`);
    if (dv.getUint32(local, true) !== 0x04034b50) throw new Error("corrupt zip local header");
    const start = local + 30 + dv.getUint16(local + 26, true) + dv.getUint16(local + 28, true);
    const packed = bytes.subarray(start, start + csize);
    let data;
    if (method === 0) {
      data = packed.slice();
    } else if (method === 8) {
      const stream = new Blob([packed]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
      data = new Uint8Array(await new Response(stream).arrayBuffer());
    } else {
      throw new Error(`"${name}" uses unsupported zip compression method ${method}`);
    }
    if (data.length !== usize) throw new Error(`"${name}": size mismatch after decompression`);
    files.push({ path: name, data });
  }
  return files;
}

// -- installers --------------------------------------------------------------

export function looksLikeInstaller(bytes) {
  return bytes.length > 2 && bytes[0] === 0x4d && bytes[1] === 0x5a; // "MZ"
}

export function extractInstaller(bytes, onLog) {
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL("./inno-worker.js", import.meta.url), { type: "module" });
    worker.onmessage = (ev) => {
      const m = ev.data;
      if (m.type === "log") onLog?.(m.line);
      else if (m.type === "done") { worker.terminate(); resolve(m.files.map((f) => ({ path: f.path, data: new Uint8Array(f.data) }))); }
      else if (m.type === "error") { worker.terminate(); reject(new Error(m.message)); }
    };
    worker.onerror = (e) => { worker.terminate(); reject(new Error("installer extraction failed: " + (e.message || e))); };
    worker.postMessage({ type: "extract", bytes }, [bytes.buffer]);
  });
}

// -- which game --------------------------------------------------------------

const base = (p) => p.slice(p.lastIndexOf("/") + 1);
const dir = (p) => (p.includes("/") ? p.slice(0, p.lastIndexOf("/")) : "");
const ext = (p) => { const b = base(p); return b.includes(".") ? b.slice(b.lastIndexOf(".") + 1).toLowerCase() : ""; };

function isJunk(path) {
  const parts = path.split("/");
  if (parts.slice(0, -1).some((d) => JUNK_DIRS.has(d.toLowerCase()))) return true;
  return JUNK_EXT.has(ext(path));
}

// Finds the game among the files: the known executable closest to the top
// decides the game and its directory; everything under that directory
// (minus installer junk) becomes drive C.
export function identifyGame(files) {
  const normalized = files.map((f) => ({ path: f.path.replace(/\\/g, "/").replace(/^\.?\/+/, ""), data: f.data }));
  let best = null;
  for (const game of GAMES) {
    if (game.secondary) continue;
    for (const f of normalized) {
      if (isJunk(f.path)) continue;
      if (game.detect.includes(base(f.path).toUpperCase())) {
        const depth = f.path.split("/").length;
        if (!best || depth < best.depth) best = { game, root: dir(f.path), depth };
      }
    }
  }
  if (!best) {
    const exes = normalized.filter((f) => ["exe", "com", "bat"].includes(ext(f.path)) && !isJunk(f.path)).map((f) => f.path);
    return { game: null, root: "", files: [], candidates: exes.slice(0, 40) };
  }
  const prefix = best.root ? best.root + "/" : "";
  const gameFiles = normalized
    .filter((f) => f.path.startsWith(prefix) && !isJunk(f.path.slice(prefix.length)))
    .map((f) => ({ path: f.path.slice(prefix.length), data: f.data }));
  return { game: best.game, root: best.root, files: gameFiles };
}

export function installFiles(FS, root, files) {
  const made = new Set();
  const mkdirp = (path) => {
    if (made.has(path)) return;
    const parent = dir(path);
    if (parent) mkdirp(parent);
    try { FS.mkdir(path); } catch (e) { /* exists */ }
    made.add(path);
  };
  mkdirp(root);
  for (const f of files) {
    const parent = dir(f.path);
    if (parent) mkdirp(root + "/" + parent);
    FS.writeFile(root + "/" + f.path, f.data);
  }
}

// -- IndexedDB cache ---------------------------------------------------------

const DB = "wc-web";
function openDb() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB, 1);
    req.onupgradeneeded = () => req.result.createObjectStore("games", { keyPath: "id" });
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}
function tx(db, mode, fn) {
  return new Promise((resolve, reject) => {
    const t = db.transaction("games", mode);
    const r = fn(t.objectStore("games"));
    t.oncomplete = () => resolve(r && r.result);
    t.onerror = () => reject(t.error);
    t.onabort = () => reject(t.error);
  });
}
export async function saveGame(record) {
  try { const db = await openDb(); await tx(db, "readwrite", (s) => s.put(record)); db.close(); return true; }
  catch (e) { console.warn("cannot save the game files in this browser:", e); return false; }
}
export async function loadGames() {
  try { const db = await openDb(); const all = await tx(db, "readonly", (s) => s.getAll()); db.close(); return all || []; }
  catch (e) { return []; }
}
export async function forgetGame(id) {
  try { const db = await openDb(); await tx(db, "readwrite", (s) => s.delete(id)); db.close(); } catch (e) { /* ignore */ }
}

export function totalSize(files) { return files.reduce((n, f) => n + f.data.byteLength, 0); }
