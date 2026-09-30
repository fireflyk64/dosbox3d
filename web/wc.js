// Page glue for the browser build of DOSBox + Wing Commander multiplayer.
//
// Loads the lobbylink browser client and the Emscripten DOSBox module,
// fetches the game files (wc.tar.gz, produced by scripts/build-web.sh)
// into the in-memory file system, sets the WCNET environment from the
// form and runs DOSBox.  The multiplayer transport in the wasm talks to
// lobbylink through Module.P2PGame (see src/wclobby_web.js).
import { P2PGame } from "./p2p-client.js";

const $ = (id) => document.getElementById(id);
const logEl = $("log");
const statusEl = $("status");
const form = $("setup");
const canvas = $("canvas");

const DEFAULT_SERVER = "https://pqrstuvw.xyz/lobbylink";
const DATA_URL = "wc.tar.gz";
// ?server=URL picks another lobby server (its allowed-origin list must
// include the origin this page is served from).
$("server").value = new URLSearchParams(location.search).get("server") || DEFAULT_SERVER;

function log(line) {
  console.log(line);
  const atBottom = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 4;
  logEl.textContent += line + "\n";
  if (logEl.textContent.length > 200000) logEl.textContent = logEl.textContent.slice(-100000);
  if (atBottom) logEl.scrollTop = logEl.scrollHeight;
}
function status(text) { statusEl.textContent = text; }

// Prefill from the URL (?room=CODE&callsign=...) so a link can be shared.
{
  const q = new URLSearchParams(location.search);
  for (const k of ["room", "callsign", "lastname", "players"]) if (q.get(k)) $(k).value = q.get(k);
  if (q.get("relay")) $("relay").checked = true;
  if (q.get("verbose")) $("verbose").checked = true;
  if (!$("room").value) $("room").value = "WC-" + Math.random().toString(36).slice(2, 6).toUpperCase();
}

form.addEventListener("submit", (ev) => {
  ev.preventDefault();
  $("fly").disabled = true;
  for (const el of form.elements) el.disabled = true;
  start().catch((e) => {
    log("failed: " + (e && e.stack ? e.stack : e));
    status("Failed to start: " + (e && e.message ? e.message : e));
  });
});

async function start() {
  const cfg = {
    room: $("room").value.trim(),
    callsign: $("callsign").value.trim(),
    lastname: $("lastname").value.trim(),
    players: String(Math.max(2, Math.min(3, Number($("players").value) || 3))),
    server: $("server").value.trim() || DEFAULT_SERVER,
    relay: $("relay").checked,
    verbose: $("verbose").checked,
  };
  const url = new URL(location.href);
  url.searchParams.set("room", cfg.room);
  url.searchParams.delete("callsign"); url.searchParams.delete("lastname");
  history.replaceState(null, "", url);

  status("Loading the emulator…");
  const { default: createDOSBox } = await import("./dosbox.js");
  const env = {
    WCROOM: cfg.room,
    WCLOBBY: cfg.server,
    WCPLAYERS: cfg.players,
    WCNET_LOG: cfg.verbose ? "2" : "1",
  };
  if (cfg.callsign) env.WCCALLSIGN = cfg.callsign;
  if (cfg.lastname) env.WCLASTNAME = cfg.lastname;
  if (cfg.relay) env.WCLOBBY_RELAY = "1";
  // Any ?env.NAME=value lands in DOSBox's environment: the same knobs as
  // the native build (MIS, SERIES, WCNET_AUTOKEYS, WCNET_LOG, ...).
  for (const [k, v] of new URLSearchParams(location.search)) {
    if (k.startsWith("env.")) env[k.slice(4)] = v;
  }

  const config = {
    canvas,
    P2PGame,
    print: log,
    printErr: log,
    locateFile: (path) => path,
    preRun: [() => { Object.assign(config.ENV, env); }],
  };
  const Module = await createDOSBox(config);

  status("Loading the game files…");
  await installGameFiles(Module.FS, "/wc", DATA_URL);

  status(`Room ${cfg.room}: joining…`);
  canvas.focus();
  // ?cmd=... runs another DOS command instead of the game (debugging aid).
  const cmd = new URLSearchParams(location.search).get("cmd") || "wc";
  Module.callMain(["-c", "mount c /wc", "-c", "c:", "-c", cmd]);
  window.DOSBox = Module;
  status(`Room ${cfg.room}. Share this page's link so friends land in the same room.`);
}

// -- game files: a gzipped ustar archive written into MEMFS -----------------

async function installGameFiles(FS, root, url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`cannot fetch ${url}: ${res.status} ${res.statusText}`);
  const stream = res.body.pipeThrough(new DecompressionStream("gzip"));
  const bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  try { FS.mkdir(root); } catch (e) { /* exists */ }
  let files = 0;
  untar(bytes, (name, type, data) => {
    const parts = name.replace(/^\.\//, "").split("/").filter((p) => p && p !== ".");
    if (!parts.length) return;
    if (type === "dir") { mkdirp(FS, root, parts); return; }
    mkdirp(FS, root, parts.slice(0, -1));
    FS.writeFile(root + "/" + parts.join("/"), data);
    files++;
  });
  log(`installed ${files} game files under ${root}`);
}

function mkdirp(FS, root, parts) {
  let path = root;
  for (const p of parts) {
    path += "/" + p;
    try { FS.mkdir(path); } catch (e) { /* exists */ }
  }
}

function untar(bytes, onEntry) {
  const text = (off, len) => {
    let end = off;
    while (end < off + len && bytes[end] !== 0) end++;
    return new TextDecoder("latin1").decode(bytes.subarray(off, end));
  };
  let off = 0;
  while (off + 512 <= bytes.length) {
    if (bytes[off] === 0) break; // end-of-archive blocks
    const name = text(off, 100);
    const size = parseInt(text(off + 124, 12).trim() || "0", 8);
    const typeflag = bytes[off + 156];
    const prefix = text(off + 345, 155);
    const full = prefix ? prefix + "/" + name : name;
    off += 512;
    const data = bytes.subarray(off, off + size);
    if (typeflag === 0 || typeflag === 0x30) onEntry(full, "file", data);
    else if (typeflag === 0x35) onEntry(full, "dir", null);
    off += Math.ceil(size / 512) * 512;
  }
}
