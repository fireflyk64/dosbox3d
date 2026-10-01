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
// emulator's multiplayer hooks apply to it (Wing Commander 1 only, so far).
// Optional: `saves`, the files the game keeps its saved games in (relative to
// the game directory; the page keeps a copy in the browser and offers them as
// a download), and `cycles`, the emulated CPU speed the game plays well at
// (DOSBox's default of 3000 is what Ctrl+F11 / Ctrl+F12 adjust).

export const GAMES = [
  { id: "wc1", title: "Wing Commander", detect: ["WC.EXE"], run: "wc", multiplayer: true,
    saves: ["GAMEDAT/SAVEGAME.WLD"], cycles: 3630 },
  { id: "wc1sm2", title: "Wing Commander: Secret Missions 2", detect: ["SM2.EXE"], run: "sm2", multiplayer: false, secondary: true },
  { id: "wc2", title: "Wing Commander II", detect: ["WC2.EXE"], run: "wc2", multiplayer: false },
];

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
