// Page glue for the browser build of DOSBox + Wing Commander multiplayer.
//
// Loads the lobbylink browser client and the Emscripten DOSBox module,
// takes the game files from the player (a .zip of the game folder or the
// GOG installer, unpacked in the browser; see gamefiles.js), or from this
// server's wc.tar.gz when it has one, installs them into the in-memory
// file system, sets the WCNET environment from the form and runs DOSBox.
// The multiplayer transport in the wasm talks to lobbylink through
// Module.P2PGame (see src/wclobby_web.js).
import { P2PGame } from "./p2p-client.js";
import { readZip, extractInstaller, looksLikeInstaller, identifyGame, installFiles,
         saveGame, loadGames, forgetGame, totalSize } from "./gamefiles.js";

const $ = (id) => document.getElementById(id);
const logEl = $("log");
const statusEl = $("status");
const form = $("setup");
const canvas = $("canvas");

const DEFAULT_SERVER = "https://pqrstuvw.xyz/lobbylink";
const DATA_URL = "wc.tar.gz";
const GAME_ROOT = "/game";
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
function sourceStatus(text) { $("sourceStatus").textContent = text; }
const mb = (n) => (n / 1048576).toFixed(1) + " MB";

// Prefill from the URL (?room=CODE&callsign=...) so a link can be shared.
{
  const q = new URLSearchParams(location.search);
  for (const k of ["room", "callsign", "lastname", "players"]) if (q.get(k)) $(k).value = q.get(k);
  if (q.get("relay")) $("relay").checked = true;
  if (q.get("verbose")) $("verbose").checked = true;
  if (!$("room").value) $("room").value = "WC-" + Math.random().toString(36).slice(2, 6).toUpperCase();
}

// -- game files ----------------------------------------------------------------

// The selected source: { label, game, root, files } with files as
// [{path, data}] relative to the game directory (drive C).
let source = null;
let saved = [];

function setSource(s) {
  source = s;
  $("fly").disabled = !s;
  if (s) {
    sourceStatus(`Ready: ${s.game.title} from ${s.label} (${s.files.length} files, ${mb(totalSize(s.files))}).` +
                 (s.game.multiplayer ? "" : " The multiplayer hooks only exist for Wing Commander 1, so this runs single-player."));
  }
}

async function useFiles(files, label, { persist = true } = {}) {
  const id = identifyGame(files);
  if (!id.game) {
    $("pickerLabel").hidden = false;
    const sel = $("exePicker");
    sel.innerHTML = "";
    for (const c of id.candidates) { const o = document.createElement("option"); o.value = c; o.textContent = c; sel.appendChild(o); }
    sourceStatus(id.candidates.length
      ? `${label}: no game I recognise (${files.length} files). Pick the executable to run.`
      : `${label}: no DOS executable found among ${files.length} files.`);
    sel.onchange = () => {
      const exe = sel.value;
      const root = exe.includes("/") ? exe.slice(0, exe.lastIndexOf("/")) : "";
      const prefix = root ? root + "/" : "";
      const name = exe.slice(prefix.length);
      const game = { id: "custom-" + name.toLowerCase(), title: name, run: name.replace(/\.(exe|com|bat)$/i, ""), multiplayer: false };
      const sub = files.filter((f) => f.path.startsWith(prefix)).map((f) => ({ path: f.path.slice(prefix.length), data: f.data }));
      setSource({ label, game, root, files: sub });
      if (persist) void saveGame({ id: game.id, title: game.title, run: game.run, multiplayer: false, label, files: sub, when: Date.now() });
    };
    if (id.candidates.length) sel.onchange();
    return;
  }
  $("pickerLabel").hidden = true;
  setSource({ label, game: id.game, root: id.root, files: id.files });
  if (persist) {
    const ok = await saveGame({ id: id.game.id, title: id.game.title, run: id.game.run, multiplayer: id.game.multiplayer, label, files: id.files, when: Date.now() });
    if (ok) { await refreshSaved(); log(`kept ${id.game.title} in this browser for next time`); }
  }
}

async function handleFile(file) {
  try {
    for (const el of ["useServer", "useSaved", "forget"]) $(el).disabled = true;
    sourceStatus(`Reading ${file.name} (${mb(file.size)})…`);
    const bytes = new Uint8Array(await file.arrayBuffer());
    let files;
    if (bytes[0] === 0x50 && bytes[1] === 0x4b) {
      files = await readZip(bytes);
      log(`read ${files.length} files from ${file.name}`);
    } else if (looksLikeInstaller(bytes)) {
      sourceStatus(`Unpacking the installer ${file.name} in your browser… (innoextract)`);
      files = await extractInstaller(bytes, log);
    } else {
      throw new Error(`${file.name} is neither a .zip nor a Windows installer`);
    }
    await useFiles(files, file.name);
  } catch (e) {
    log("game files: " + (e && e.message ? e.message : e));
    sourceStatus(`Could not use ${file.name}: ${e && e.message ? e.message : e}. ` +
                 `A .zip of the installed game folder always works; for a GOG installer you can also unpack it with innoextract and zip the result.`);
  } finally {
    for (const el of ["useServer", "useSaved", "forget"]) $(el).disabled = false;
  }
}

async function readTarGz(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`cannot fetch ${url}: ${res.status} ${res.statusText}`);
  const stream = res.body.pipeThrough(new DecompressionStream("gzip"));
  const bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  const files = [];
  const text = (off, len) => { let end = off; while (end < off + len && bytes[end] !== 0) end++; return new TextDecoder("latin1").decode(bytes.subarray(off, end)); };
  let off = 0;
  while (off + 512 <= bytes.length) {
    if (bytes[off] === 0) break;
    const name = text(off, 100);
    const size = parseInt(text(off + 124, 12).trim() || "0", 8);
    const typeflag = bytes[off + 156];
    const prefix = text(off + 345, 155);
    off += 512;
    if (typeflag === 0 || typeflag === 0x30) files.push({ path: (prefix ? prefix + "/" : "") + name, data: bytes.slice(off, off + size) });
    off += Math.ceil(size / 512) * 512;
  }
  return files;
}

async function refreshSaved() {
  saved = await loadGames();
  $("useSaved").hidden = saved.length === 0;
  $("forget").hidden = saved.length === 0;
  if (saved.length) $("useSaved").textContent = `Use the saved copy (${saved[0].title}, ${mb(totalSize(saved[0].files))})`;
}

async function initSources() {
  const drop = $("dropzone");
  for (const ev of ["dragenter", "dragover"]) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); });
  for (const ev of ["dragleave", "drop"]) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove("over"); });
  drop.addEventListener("drop", (e) => { const f = e.dataTransfer.files && e.dataTransfer.files[0]; if (f) void handleFile(f); });
  $("gamefile").addEventListener("change", (e) => { const f = e.target.files && e.target.files[0]; if (f) void handleFile(f); });

  $("useSaved").addEventListener("click", () => {
    const g = saved[0];
    if (g) setSource({ label: "the saved copy (" + g.label + ")", game: { id: g.id, title: g.title, run: g.run, multiplayer: g.multiplayer }, root: "", files: g.files });
  });
  $("forget").addEventListener("click", async () => {
    for (const g of saved) await forgetGame(g.id);
    await refreshSaved();
    if (source && source.label.startsWith("the saved copy")) setSource(null);
    sourceStatus("Forgot the saved game files.");
  });
  $("useServer").addEventListener("click", async () => {
    try {
      sourceStatus("Fetching this server's copy…");
      await useFiles(await readTarGz(DATA_URL), "this server", { persist: false });
    } catch (e) { sourceStatus("Server copy unavailable: " + e.message); }
  });

  await refreshSaved();
  let serverCopy = false;
  try { serverCopy = (await fetch(DATA_URL, { method: "HEAD" })).ok; } catch (e) { /* none */ }
  $("useServer").hidden = !serverCopy;
  if (saved.length) $("useSaved").click();
  else if (serverCopy) $("useServer").click();
}

// -- running -------------------------------------------------------------------

form.addEventListener("submit", (ev) => {
  ev.preventDefault();
  if (!source) return;
  $("fly").disabled = true;
  for (const el of form.elements) el.disabled = true;
  for (const el of ["useServer", "useSaved", "forget", "gamefile", "exePicker"]) $(el).disabled = true;
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
  const env = { WCNET_LOG: cfg.verbose ? "2" : "1" };
  if (source.game.multiplayer) {
    env.WCROOM = cfg.room;
    env.WCLOBBY = cfg.server;
    env.WCPLAYERS = cfg.players;
    if (cfg.relay) env.WCLOBBY_RELAY = "1";
  } else {
    log(`${source.game.title}: no multiplayer hooks for this game, running single-player`);
  }
  if (cfg.callsign) env.WCCALLSIGN = cfg.callsign;
  if (cfg.lastname) env.WCLASTNAME = cfg.lastname;
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

  status(`Installing ${source.game.title} (${source.files.length} files)…`);
  installFiles(Module.FS, GAME_ROOT, source.files);
  log(`installed ${source.files.length} files of ${source.game.title} under ${GAME_ROOT}`);

  status(source.game.multiplayer ? `Room ${cfg.room}: joining…` : `Starting ${source.game.title}…`);
  canvas.focus();
  // ?cmd=... runs another DOS command instead of the game (debugging aid).
  const cmd = new URLSearchParams(location.search).get("cmd") || source.game.run;
  Module.callMain(["-c", `mount c ${GAME_ROOT}`, "-c", "c:", "-c", cmd]);
  window.DOSBox = Module;
  status(source.game.multiplayer
    ? `Room ${cfg.room}. Share this page's link so friends land in the same room.`
    : `${source.game.title} is running.`);
}

void initSources();
