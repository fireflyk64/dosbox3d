// Page glue for the browser build of DOSBox + Wing Commander multiplayer.
//
// Flow: the player brings the game files (gamefiles.js), joins a lobbylink
// room from the page (roster + chat over the same WebRTC links the game
// will use), and the host starts the game for everyone.  DOSBox then runs
// with that live room connection: src/wclobby_web.js adopts Module.lobbyGame
// instead of joining the room a second time.  Lobby chat and presence
// travel as reliable messages tagged with a 4-byte prefix that the game
// protocol never produces, so the transport filters them out.
import { P2PGame } from "./p2p-client.js";
import { initControls } from "./gamepad.js";
import { readZip, extractInstaller, looksLikeInstaller, identifyGame, installFiles,
         saveGame, loadGames, forgetGame, totalSize, gameById } from "./gamefiles.js";

const $ = (id) => document.getElementById(id);
const logEl = $("log");
const canvas = $("canvas");

const DEFAULT_SERVER = "https://pqrstuvw.xyz/lobbylink";
const DATA_URL = "wc.tar.gz";
const GAME_ROOT = "/game";
const LOBBY_MAGIC = [0x57, 0x43, 0x4c, 0x01]; // "WCL" + version; never a valid protobuf start
const query = new URLSearchParams(location.search);

// ?server=URL picks another lobby server (its allowed-origin list must
// include the origin this page is served from).
$("server").value = query.get("server") || DEFAULT_SERVER;

function log(line) {
  console.log(line);
  const atBottom = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 4;
  logEl.textContent += line + "\n";
  if (logEl.textContent.length > 200000) logEl.textContent = logEl.textContent.slice(-100000);
  if (atBottom) logEl.scrollTop = logEl.scrollHeight;
}
function status(text) { $("status").textContent = text; }
function sourceStatus(text) { $("sourceStatus").textContent = text; }
const mb = (n) => (n / 1048576).toFixed(1) + " MB";

// Prefill from the URL (?room=CODE&callsign=...) so a link can be shared.
for (const k of ["room", "callsign", "firstname", "lastname", "players"]) if (query.get(k)) $(k).value = query.get(k);
if (query.get("relay")) $("relay").checked = true;
if (query.get("verbose")) $("verbose").checked = true;
if (!$("room").value) $("room").value = "WC-" + Math.random().toString(36).slice(2, 6).toUpperCase();

// -- game files ----------------------------------------------------------------

// The selected source: { label, game, root, files } with files as
// [{path, data}] relative to the game directory (drive C).
let source = null;
let saved = [];

function setSource(s) {
  // What the page knows about a game is the registry's, also for a copy this
  // browser saved when the registry said something else.
  if (s && gameById(s.game.id)) { const g = gameById(s.game.id); s.game = { id: g.id, title: g.title, run: g.run, multiplayer: g.multiplayer }; }
  source = s;
  if (s) {
    sourceStatus(`Ready: ${s.game.title} from ${s.label} (${s.files.length} files, ${mb(totalSize(s.files))}).` +
                 (s.game.multiplayer ? "" : " The multiplayer hooks only exist for Wing Commander 1 and 2, so this runs single-player."));
  }
  describeSaves();
  // (Wing Commander II's people use a first name; the first game has none.)
  $("firstnameLabel").hidden = !(gameInfo() && gameInfo().firstName);
  if (lobby.game) { sayHello(); buildMissionMenu(); renderMission(); }
  updateActions();
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
    sourceButtons(false);
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
    sourceButtons(true);
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

// The buttons that change the game files: off while a file is being read
// and once the game runs.
function sourceButtons(on) {
  for (const el of [$("useServer"), $("forget"), ...$("savedList").querySelectorAll("button")]) el.disabled = !on;
}
const useSavedCopy = (g) => setSource({ label: "the saved copy (" + g.label + ")", game: { id: g.id, title: g.title, run: g.run, multiplayer: g.multiplayer }, root: "", files: g.files });

// One button per game this browser kept, the latest first.
async function refreshSaved() {
  saved = (await loadGames()).sort((a, b) => (b.when || 0) - (a.when || 0));
  $("forget").hidden = saved.length === 0;
  $("forget").textContent = saved.length > 1 ? "Forget the saved copies" : "Forget the saved copy";
  const list = $("savedList");
  list.innerHTML = "";
  for (const g of saved) {
    const b = document.createElement("button");
    b.type = "button"; b.className = "secondary";
    b.textContent = `Use the saved copy (${g.title}, ${mb(totalSize(g.files))})`;
    b.addEventListener("click", () => useSavedCopy(g));
    list.appendChild(b); list.appendChild(document.createTextNode(" "));
  }
}

async function initSources() {
  const drop = $("dropzone");
  for (const ev of ["dragenter", "dragover"]) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); });
  for (const ev of ["dragleave", "drop"]) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove("over"); });
  drop.addEventListener("drop", (e) => { const f = e.dataTransfer.files && e.dataTransfer.files[0]; if (f) void handleFile(f); });
  $("gamefile").addEventListener("change", (e) => { const f = e.target.files && e.target.files[0]; if (f) void handleFile(f); });

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
  if (saved.length) useSavedCopy(saved[0]);
  else if (serverCopy) $("useServer").click();
}

// -- save games ----------------------------------------------------------------

// A game's registry entry lists the files it keeps its saved games in (Wing
// Commander: one file holding all eight bunks).  This browser keeps a copy in
// local storage, keyed by game: it is put back into the game directory at
// every start and refreshed whenever the running game changes the file.
const gameInfo = () => (source && gameById(source.game.id)) || null;
const saveFiles = () => { const g = gameInfo(); return (g && g.saves) || []; };
const saveKey = (rel) => `wcsave:${source.game.id}:${rel.toUpperCase()}`;
const baseName = (p) => p.slice(p.lastIndexOf("/") + 1);
const sameBytes = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const saveSeen = new Map();  // file -> its bytes as last seen in the running game
function saveStatus(text) { $("saveStatus").textContent = text; }

function storedSave(rel) {
  try {
    const r = JSON.parse(localStorage.getItem(saveKey(rel)) || "null");
    return r && r.b64 ? { when: r.when, data: Uint8Array.from(atob(r.b64), (c) => c.charCodeAt(0)) } : null;
  } catch (e) { return null; }
}
function storeSave(rel, data) {
  try {
    let bin = "";
    for (let i = 0; i < data.length; i += 0x8000) bin += String.fromCharCode(...data.subarray(i, i + 0x8000));
    localStorage.setItem(saveKey(rel), JSON.stringify({ when: Date.now(), b64: btoa(bin) }));
    return true;
  } catch (e) {
    log("save games: this browser would not store the copy: " + (e && e.message ? e.message : e));
    return false;
  }
}
// DOS names ignore case, the emulator's file system does not: find the file
// in whatever case the player's copy of the game uses.
function resolveSave(FS, rel) {
  const parts = rel.split("/");
  let dir = GAME_ROOT;
  for (let i = 0; i < parts.length; i++) {
    let names;
    try { names = FS.readdir(dir); } catch (e) { return null; }
    const hit = names.find((n) => n.toUpperCase() === parts[i].toUpperCase());
    if (i === parts.length - 1) return { path: `${dir}/${hit || parts[i]}`, exists: !!hit };
    if (!hit) return null;
    dir += "/" + hit;
  }
  return null;
}
function readSave(FS, rel) {
  const r = resolveSave(FS, rel);
  if (!r || !r.exists) return null;
  try { return FS.readFile(r.path); } catch (e) { return null; }
}
function writeSave(FS, rel, data) {
  const r = resolveSave(FS, rel);
  if (!r) return false;
  try { FS.writeFile(r.path, data); return true; } catch (e) { return false; }
}
const liveFS = () => (running && window.DOSBox && window.DOSBox.FS) || null;

function describeSaves() {
  const files = saveFiles();
  $("saves").hidden = files.length === 0;
  if (!files.length) return;
  const kept = files.map(storedSave).filter(Boolean);
  saveStatus(kept.length
    ? `This browser keeps your save games (last change ${new Date(Math.max(...kept.map((k) => k.when))).toLocaleString()}).`
    : "No save game of yours is kept here yet: save in a bunk and it will be.");
}
// At start: put the kept copies back, and note what the game starts with.
function restoreSaves(FS) {
  saveSeen.clear();
  for (const rel of saveFiles()) {
    const kept = storedSave(rel);
    if (kept && writeSave(FS, rel, kept.data)) log(`save games: put back ${baseName(rel)} from ${new Date(kept.when).toLocaleString()}`);
    const now = readSave(FS, rel);
    if (now) saveSeen.set(rel, now.slice());
  }
}
// While running: keep a copy whenever the game has changed a save file.
function keepSaves() {
  const FS = liveFS();
  if (!FS) return;
  for (const rel of saveFiles()) {
    const now = readSave(FS, rel);
    const before = saveSeen.get(rel);
    if (!now || (before && sameBytes(before, now))) continue;
    saveSeen.set(rel, now.slice());
    if (storeSave(rel, now)) { log(`save games: kept ${baseName(rel)} in this browser`); describeSaves(); }
  }
}
$("saveDownload").addEventListener("click", () => {
  let count = 0;
  for (const rel of saveFiles()) {
    const FS = liveFS();
    const kept = storedSave(rel);
    const pristine = source.files.find((f) => f.path.toUpperCase() === rel.toUpperCase());
    const data = (FS && readSave(FS, rel)) || (kept && kept.data) || (pristine && pristine.data);
    if (!data) continue;
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([data], { type: "application/octet-stream" }));
    a.download = baseName(rel);
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    count++;
  }
  if (!count) saveStatus("Nothing to download: this copy of the game has no save file yet.");
});
$("saveFile").addEventListener("change", async (e) => {
  const file = e.target.files && e.target.files[0];
  e.target.value = "";
  const files = saveFiles();
  if (!file || !files.length) return;
  const rel = files.find((p) => baseName(p).toUpperCase() === file.name.toUpperCase()) || files[0];
  const data = new Uint8Array(await file.arrayBuffer());
  const pristine = source.files.find((f) => f.path.toUpperCase() === rel.toUpperCase());
  if (!data.length || (pristine && data.length !== pristine.data.length)) {
    saveStatus(`${file.name} is not this game's ${baseName(rel)}: ${data.length} bytes` +
               (pristine ? `, the game's file has ${pristine.data.length}.` : "."));
    return;
  }
  if (!storeSave(rel, data)) return;
  const FS = liveFS();
  if (FS && writeSave(FS, rel, data)) saveSeen.set(rel, data.slice());
  describeSaves();
  log(`save games: restored ${baseName(rel)} from ${file.name}` + (FS ? " (the bunks show it the next time you open them)" : ""));
});

// -- missions ------------------------------------------------------------------

// The host picks a mission of its game's campaign for everybody (the game
// registry lists them), or none: then everyone starts in the barracks and the
// host's saved game decides.  A mission is {series, mis} as the game numbers
// them: series 1.., mission 0.. within the series.  The menu and its labels
// are for the host's game, which the others learn from its hello.
const lobbyGameId = () => (isHost() ? (source ? source.game.id : "") : (lobby.games.get(0) || (source ? source.game.id : "")));
const lobbyCampaign = () => { const g = gameById(lobbyGameId()); return (g && g.campaign) || null; };
const missionKey = (m) => (m ? `${m.series}/${m.mis}` : "");
function parseMission(v) {
  const m = /^(\d+)\/(\d+)$/.exec(v || "");
  return m ? { series: Number(m[1]), mis: Number(m[2]) } : null;
}
function missionLabel(m) {
  if (!m) return "the campaign from the barracks";
  const c = lobbyCampaign();
  const s = c && c.series.find((e) => e.series === m.series);
  if (!s) return `series ${m.series}, mission ${m.mis + 1}`;
  const note = Array.isArray(s.missions) ? s.missions[m.mis] : "";
  return (s.name ? `${s.name} ${m.mis + 1}` : `series ${s.series}, mission ${m.mis + 1}`) + (note ? ` (${note})` : "");
}
function buildMissionMenu() {
  const sel = $("mission");
  const c = lobbyCampaign();
  const key = lobbyGameId();
  if (sel.dataset.game === key && sel.options.length) return;
  sel.dataset.game = key;
  sel.innerHTML = "";
  const add = (value, text) => { const o = document.createElement("option"); o.value = value; o.textContent = text; sel.appendChild(o); };
  add("", "Campaign: start in the barracks (the host's save game decides)");
  if (!c) return;
  for (const s of c.series) {
    const n = Array.isArray(s.missions) ? s.missions.length : s.missions;
    for (let i = 0; i < n; i++) {
      const note = Array.isArray(s.missions) ? s.missions[i] : "";
      add(`${s.series}/${i}`, s.name ? `${s.name} ${i + 1}  (series ${s.series}, mission ${i + 1})`
                                     : `Series ${s.series}, mission ${i + 1}` + (note ? `  (${note})` : ""));
    }
  }
}
function renderMission() {
  buildMissionMenu();
  const sel = $("mission");
  const c = lobbyCampaign();
  // A mission the host's game does not have (the host changed games) is none.
  if (lobby.mission && isHost() && !Array.from(sel.options).some((o) => o.value === missionKey(lobby.mission))) lobby.mission = null;
  sel.value = missionKey(lobby.mission);
  sel.disabled = !isHost() || running;
  $("rocks").value = lobby.rocks;
  $("rocks").disabled = !isHost();  // the host may switch them while flying too
  const hints = (c && c.hints) || {};
  $("missionHint").textContent = !c ? (lobbyGameId() ? "" : "Load the game files to pick a mission.")
    : lobby.mission ? `${missionLabel(lobby.mission)[0].toUpperCase()}${missionLabel(lobby.mission).slice(1)}. ${hints.forced || ""}`
    : (isHost() ? hints.host : hints.wing) || "";
}
// Everyone in a room has to run the same game.
function checkSameGame(from) {
  const theirs = lobby.games.get(from);
  if (!source || !theirs || theirs === source.game.id) return;
  const g = gameById(theirs);
  chatLine(`${esc(lobby.names.get(from) || "Player " + (from + 1))} has ${esc(g ? g.title : theirs)} loaded and you have ${esc(source.game.title)}: everyone in a room needs the same game.`, "sys");
}

// -- the room ------------------------------------------------------------------

const lobby = { game: null, names: new Map(), links: new Map(), flying: new Set(), unsubscribe: null, mission: null,
                // which game each player has loaded (a registry id), from their hello
                games: new Map(),
                // asteroid and mine fields, "on", "soft" (a rock does a
                // fraction of its damage) or "off": the host's choice, for everybody
                rocks: "on",
                // game-protocol messages that arrive before DOSBox has adopted this
                // connection (a wingman faster to the launch than the host), handed
                // to the transport at adoption so nothing is lost
                backlog: [] };
let running = false;
const myName = () => $("callsign").value.trim() || "Pilot " + (lobby.game ? lobby.game.selfId + 1 : "?");
const isHost = () => lobby.game && lobby.game.selfId === 0;

function encodeLobby(obj) {
  const json = new TextEncoder().encode(JSON.stringify(obj));
  const out = new Uint8Array(4 + json.length);
  out.set(LOBBY_MAGIC); out.set(json, 4);
  return out;
}
function decodeLobby(data) {
  if (data.length < 4 || LOBBY_MAGIC.some((b, i) => data[i] !== b)) return null;
  try { return JSON.parse(new TextDecoder().decode(data.subarray(4))); } catch (e) { return null; }
}

function sendLobby(obj, to) {
  const g = lobby.game;
  if (!g) return;
  const targets = to != null ? [to] : g.players.filter((p) => p.occupied && p.id !== g.selfId).map((p) => p.id);
  for (const id of targets) g.sendReliable(id, encodeLobby(obj)).catch((e) => log(`lobby: message to player ${id} failed: ${e.message}`));
}
const sayHello = (to, reply) => sendLobby({ t: "hello", name: myName(), flying: running, reply: !!reply,
  game: source ? source.game.id : "",
  mission: isHost() ? missionKey(lobby.mission) : undefined, rocks: isHost() ? lobby.rocks : undefined }, to);

// Asteroid and mine fields on, soft or off.  The host's page tells the others
// (for their lobby) and its own running game, which tells every wingman's game.
const ROCKS = { off: 0, on: 1, soft: 2 };  // the game's numbers (RocksMode)
const rocksMode = (v) => (Object.prototype.hasOwnProperty.call(ROCKS, v) ? v : "on");
const rocksLabel = (v) => (v === "soft" ? "asteroids soft (one will not kill you)" : `asteroids and mines ${v}`);
function setRocks(v) {
  if (!isHost()) { chatLine("Only the host can switch asteroids and mines.", "sys"); renderMission(); return; }
  v = rocksMode(v);
  if (lobby.rocks === v) return;
  lobby.rocks = v;
  renderMission();
  chatLine(`You switched ${rocksLabel(v)}`, "sys");
  sayHello();
  if (running && window.DOSBox && window.DOSBox._wc_web_set_rocks) window.DOSBox._wc_web_set_rocks(ROCKS[v]);
}

function chatLine(html, cls) {
  const div = document.createElement("div");
  if (cls) div.className = cls;
  div.innerHTML = html;
  const el = $("chatLog");
  el.appendChild(div);
  el.scrollTop = el.scrollHeight;
}
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

function renderRoster() {
  const g = lobby.game;
  const ul = $("roster");
  ul.innerHTML = "";
  if (!g) return;
  for (const p of g.players) {
    const li = document.createElement("li");
    const me = p.id === g.selfId;
    const link = me ? "up" : (lobby.links.get(p.id) || (p.occupied ? "down" : ""));
    const name = me ? myName() : (lobby.names.get(p.id) || (p.occupied ? "…" : "empty seat"));
    const tags = [];
    if (p.id === 0) tags.push("host");
    if (me) tags.push("you");
    if (p.occupied && !me) tags.push(link === "up" ? "connected" : "connecting…");
    if (lobby.flying.has(p.id) || (me && running)) tags.push("flying");
    li.innerHTML = `<span class="dot ${p.occupied ? link : ""}"></span><span class="who">${esc(name)}</span><span class="tag">${tags.join(" · ")}</span>`;
    ul.appendChild(li);
  }
}

function updateActions() {
  const ready = !!source && !!lobby.game && !running;
  $("fly").disabled = !ready;
  if (running) return;
  if (!source && !lobby.game) status("Load the game files and join a room first.");
  else if (!source) status("Join succeeded; now load the game files above.");
  else if (!lobby.game) status("Game files ready; join a room to fly with friends.");
  else if (isHost()) { $("fly").textContent = "Fly (starts everyone)"; status(`You host room ${lobby.game.code}. Press Fly when your wingmen are in.`); }
  else { $("fly").textContent = "Fly"; status(lobby.flying.has(0) ? "The host is already flying: press Fly to join the mission." : "Waiting for the host to start; Fly joins on your own too."); }
}

function onLobbyEvent(ev) {
  switch (ev.type) {
    case "message": {
      if (ev.kind !== "reliable") return;
      const m = decodeLobby(ev.data);
      if (!m) { // game protocol: the wasm transport handles it, once it exists
        if (!lobby.adopted && lobby.backlog.length < 256) lobby.backlog.push({ from: ev.from, data: ev.data });
        return;
      }
      if (m.t === "hello") {
        const fresh = lobby.names.get(ev.from) !== m.name;
        lobby.names.set(ev.from, m.name);
        if (m.flying) lobby.flying.add(ev.from); else lobby.flying.delete(ev.from);
        if (fresh) chatLine(`${esc(m.name)} is in the room${m.flying ? " (flying)" : ""}`, "sys");
        if (typeof m.game === "string" && m.game !== (lobby.games.get(ev.from) || "")) {
          lobby.games.set(ev.from, m.game);
          checkSameGame(ev.from);
          if (ev.from === 0) renderMission();
        }
        if (ev.from === 0 && !isHost() && m.mission !== undefined) {
          const chosen = parseMission(m.mission);
          if (missionKey(chosen) !== missionKey(lobby.mission)) {
            lobby.mission = chosen;
            chatLine(`${esc(m.name)} picked ${esc(missionLabel(chosen))}`, "sys");
            renderMission();
          }
        }
        if (ev.from === 0 && !isHost() && typeof m.rocks === "string" && rocksMode(m.rocks) !== lobby.rocks) {
          lobby.rocks = rocksMode(m.rocks);
          chatLine(`${esc(m.name)} switched ${rocksLabel(lobby.rocks)}`, "sys");
          renderMission();
        }
        if (!m.reply) sayHello(ev.from, true);
        renderRoster(); updateActions();
      } else if (m.t === "chat") {
        chatLine(`<span class="name">${esc(m.name)}:</span> ${esc(m.text)}`);
      } else if (m.t === "start" && ev.from === 0 && !running) {
        if (typeof m.game === "string" && m.game) lobby.games.set(0, m.game);
        lobby.mission = parseMission(m.mission);
        if (typeof m.rocks === "string") lobby.rocks = rocksMode(m.rocks);
        renderMission();
        chatLine(`${esc(lobby.names.get(0) || "The host")} started ${esc(missionLabel(lobby.mission))}`, "sys");
        lobby.flying.add(0); renderRoster();
        const theirs = gameById(lobby.games.get(0) || "");
        if (source && theirs && theirs.id !== source.game.id) status(`The host started ${theirs.title}, and you have ${source.game.title} loaded. Load ${theirs.title} above, then press Fly to join.`);
        else if (source) void start(false);
        else status("The host started the game. Load your game files above, then press Fly to join.");
      }
      break;
    }
    case "player-joined": case "player-rejoined": case "player-replaced":
      lobby.links.set(ev.playerId, "down"); lobby.names.delete(ev.playerId); lobby.flying.delete(ev.playerId); lobby.games.delete(ev.playerId);
      renderRoster(); break;
    case "player-left":
      if (ev.reason === "explicit-leave") {
        chatLine(`${esc(lobby.names.get(ev.playerId) || "Player " + (ev.playerId + 1))} left the room`, "sys");
        lobby.names.delete(ev.playerId); lobby.links.delete(ev.playerId); lobby.flying.delete(ev.playerId); lobby.games.delete(ev.playerId);
      }
      renderRoster(); updateActions(); break;
    case "peer-state":
      lobby.links.set(ev.playerId, ev.state === "connected" ? "up" : (ev.state === "failed" || ev.state === "closed") ? "down" : (lobby.links.get(ev.playerId) || "down"));
      if (ev.state === "connected") sayHello(ev.playerId, false);
      renderRoster(); break;
    case "signaling-closed":
      chatLine(`Lost the lobby server (${esc(ev.code)}): nobody new can join, existing links stay up`, "sys"); break;
    case "lobby-error":
      chatLine(`Lobby error ${esc(ev.code)}: ${esc(ev.message)}`, "sys"); break;
  }
}

async function joinRoom() {
  const code = $("room").value.trim();
  if (!/^[A-Za-z0-9_-]{4,64}$/.test(code)) { status("Room codes are 4-64 letters, digits, - or _."); return; }
  $("join").disabled = true;
  status(`Joining room ${code}…`);
  try {
    const game = await P2PGame.connect({
      server: $("server").value.trim() || DEFAULT_SERVER,
      code,
      create: { maxPlayers: Math.max(2, Math.min(3, Number($("players").value) || 2)), waitUntilFull: false, allowLateJoin: true, allowReconnect: true, allowReplacement: true },
      storage: "session",
      storageKey: "wclobby-" + code,
      forceRelay: $("relay").checked,
    });
    lobby.game = game;
    lobby.names.clear(); lobby.links.clear(); lobby.flying.clear(); lobby.games.clear();
    lobby.unsubscribe = game.onEvent(onLobbyEvent);
    const url = new URL(location.href);
    url.searchParams.set("room", code);
    url.searchParams.delete("callsign"); url.searchParams.delete("firstname"); url.searchParams.delete("lastname");
    history.replaceState(null, "", url);
    $("lobby").hidden = false;
    $("leave").hidden = false;
    if (game.selfId === 0) lobby.mission = parseMission(query.get("mission")) || { series: 1, mis: 0 };
    lobby.rocks = game.selfId === 0 ? rocksMode({ 0: "off", 2: "soft" }[query.get("rocks")] || query.get("rocks")) : "on";
    renderMission();
    for (const id of ["room", "players", "server", "relay"]) $(id).disabled = true;
    chatLine(`You are ${esc(myName())}, player ${game.selfId + 1} of ${game.maxPlayers} in room ${esc(code)}${game.selfId === 0 ? " (host)" : ""}. Share this page's link.`, "sys");
    log(`lobby: joined room ${code} as player ${game.selfId} of ${game.maxPlayers}`);
    renderRoster(); updateActions();
    $("chatInput").focus();
  } catch (e) {
    status(`Could not join room ${code}: ${e && e.code ? e.code + ": " : ""}${e && e.message ? e.message : e}`);
    $("join").disabled = false;
  }
}

function leaveRoom() {
  if (lobby.unsubscribe) lobby.unsubscribe();
  if (lobby.game) { try { lobby.game.close(); } catch (e) { /* gone */ } }
  lobby.game = null; lobby.unsubscribe = null; lobby.adopted = false; lobby.backlog = [];
  lobby.names.clear(); lobby.links.clear(); lobby.flying.clear(); lobby.games.clear();
  $("lobby").hidden = true; $("leave").hidden = true; $("join").disabled = false;
  for (const id of ["room", "players", "server", "relay"]) $(id).disabled = false;
  $("roster").innerHTML = "";
  updateActions();
}

$("setup").addEventListener("submit", (ev) => { ev.preventDefault(); void joinRoom(); });
$("leave").addEventListener("click", leaveRoom);
$("chatForm").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const text = $("chatInput").value.trim();
  if (!text || !lobby.game) return;
  $("chatInput").value = "";
  const cmd = /^\/rocks(?:\s+(on|soft|off))?$/i.exec(text);
  if (cmd) { // a command, not a message
    if (cmd[1]) setRocks(cmd[1].toLowerCase());
    else chatLine(`${rocksLabel(lobby.rocks)[0].toUpperCase()}${rocksLabel(lobby.rocks).slice(1)}; the host switches them with /rocks on, /rocks soft or /rocks off.`, "sys");
    return;
  }
  chatLine(`<span class="name">${esc(myName())}:</span> ${esc(text)}`);
  sendLobby({ t: "chat", name: myName(), text });
});
$("callsign").addEventListener("change", () => { if (lobby.game) { sayHello(); renderRoster(); } });
$("rocks").addEventListener("change", () => setRocks($("rocks").value));
$("mission").addEventListener("change", () => {
  if (!isHost()) return;
  lobby.mission = parseMission($("mission").value);
  renderMission();
  chatLine(`You picked ${esc(missionLabel(lobby.mission))}`, "sys");
  sayHello();
});
buildMissionMenu();

// -- flying --------------------------------------------------------------------

$("fly").addEventListener("click", () => {
  if (!source || !lobby.game || running) return;
  if (isHost()) sendLobby({ t: "start", game: source.game.id, mission: missionKey(lobby.mission), rocks: lobby.rocks });
  void start(true);
});
$("fullscreen").addEventListener("click", () => goFullscreen());
function goFullscreen() {
  const req = canvas.requestFullscreen || canvas.webkitRequestFullscreen;
  if (!req) return;
  // Where the browser can (Chrome, Edge), ask for Esc in full screen: a tap
  // then goes to the game and only holding it leaves full screen.
  if (navigator.keyboard && navigator.keyboard.lock) navigator.keyboard.lock(["Escape"]).catch(() => { /* not allowed here */ });
  Promise.resolve(req.call(canvas)).catch(() => { /* needs a click: the button stays */ });
  canvas.focus();
}
document.addEventListener("fullscreenchange", () => {
  if (!document.fullscreenElement && navigator.keyboard && navigator.keyboard.unlock) navigator.keyboard.unlock();
});

// Caps Lock is the game's Esc everywhere: browsers keep Esc for leaving full
// screen.  The key never reaches the emulator as Caps Lock; instead a short
// Esc press is injected.  A Mac reports Caps Lock as a key-down when it turns
// on and a lone key-up when it turns off, so a key-up without a recent
// key-down counts as a tap as well.
let capsDownAt = -1e9;
function capsLockAsEscape(e) {
  if (e.code !== "CapsLock" || !running) return;
  if (document.activeElement !== canvas && document.fullscreenElement !== canvas) return;
  e.preventDefault();
  e.stopImmediatePropagation();
  const tap = window.DOSBox && window.DOSBox._wc_web_tap_escape;
  if (!tap) return;
  if (e.type === "keydown") {
    if (e.repeat) return;
    capsDownAt = performance.now();
    tap();
  } else if (performance.now() - capsDownAt > 500) {
    tap();
  }
}
window.addEventListener("keydown", capsLockAsEscape, true);
window.addEventListener("keyup", capsLockAsEscape, true);

async function start(fromGesture) {
  if (running) return;
  running = true;
  $("fly").disabled = true;
  for (const el of ["gamefile", "exePicker", "callsign", "firstname", "lastname", "verbose", "leave"]) $(el).disabled = true;
  sourceButtons(false);
  try {
    const cfg = {
      code: lobby.game.code,
      callsign: $("callsign").value.trim(),
      lastname: $("lastname").value.trim(),
      firstname: gameInfo() && gameInfo().firstName ? $("firstname").value.trim() : "",
      players: String(lobby.game.maxPlayers),
      server: $("server").value.trim() || DEFAULT_SERVER,
      relay: $("relay").checked,
      verbose: $("verbose").checked,
    };
    status("Loading the emulator…");
    const { default: createDOSBox } = await import("./dosbox.js");
    const env = { WCNET_LOG: cfg.verbose ? "2" : "1", SDL_EMSCRIPTEN_KEYBOARD_ELEMENT: "#canvas" };
    if (source.game.multiplayer) {
      env.WCROOM = cfg.code; env.WCLOBBY = cfg.server; env.WCPLAYERS = cfg.players;
      if (cfg.relay) env.WCLOBBY_RELAY = "1";
    } else {
      log(`${source.game.title}: no multiplayer hooks for this game, running single-player`);
    }
    // The host's game decides and tells the wingmen's; theirs start the same.
    if (source.game.multiplayer) env.WCROCKS = String(ROCKS[lobby.rocks]);
    if (cfg.callsign) env.WCCALLSIGN = cfg.callsign;
    if (cfg.lastname) env.WCLASTNAME = cfg.lastname;
    if (cfg.firstname) env.WCFIRSTNAME = cfg.firstname;
    // The picked mission: in the hooks' environment, or on the game's own
    // command line (the registry says which).
    const campaign = gameInfo() && gameInfo().campaign;
    let missionArgs = "";
    if (lobby.mission && source.game.multiplayer && campaign) {
      if (campaign.missionArgs) {
        missionArgs = " " + campaign.missionArgs(lobby.mission);
      } else {
        env.MIS = String(lobby.mission.mis);
        env.SERIES = String(lobby.mission.series);
      }
    }
    // Any ?env.NAME=value lands in DOSBox's environment: the same knobs as
    // the native build (MIS, SERIES, WCNET_AUTOKEYS, WCNET_LOG, ...).
    for (const [k, v] of query) if (k.startsWith("env.")) env[k.slice(4)] = v;
    if (env.MIS !== undefined || env.SERIES !== undefined) {
      log(`flying ${missionLabel({ series: Number(env.SERIES || 1), mis: Number(env.MIS || 0) })} straight from the hangar`);
    } else if (missionArgs) {
      log(`${missionLabel(lobby.mission)}: ${source.game.run}${missionArgs}`);
    }

    const config = {
      canvas,
      P2PGame,
      lobbyGame: lobby.game,          // the transport adopts this connection
      takeLobbyBacklog: () => { lobby.adopted = true; return lobby.backlog.splice(0); },
      onLobbyClosed: () => { chatLine("The game left the room", "sys"); leaveRoom(); },
      print: log,
      printErr: log,
      locateFile: (path) => path,
      preRun: [() => { Object.assign(config.ENV, env); }],
    };
    const Module = await createDOSBox(config);

    status(`Installing ${source.game.title} (${source.files.length} files)…`);
    installFiles(Module.FS, GAME_ROOT, source.files);
    log(`installed ${source.files.length} files of ${source.game.title} under ${GAME_ROOT}`);
    window.DOSBox = Module;
    restoreSaves(Module.FS);

    sayHello();
    renderRoster(); renderMission();
    status(source.game.multiplayer ? `Room ${cfg.code}: starting…` : `Starting ${source.game.title}…`);
    canvas.focus();
    // ?cmd=... runs another DOS command instead of the game (debugging aid).
    const cmd = query.get("cmd") || source.game.run + missionArgs;
    // The emulated CPU speed: the game's own good value (the registry), or
    // ?cycles=N; Ctrl+F11 / Ctrl+F12 still adjust it while playing.
    const cycles = Number(query.get("cycles")) || (gameInfo() && gameInfo().cycles) || 0;
    const args = ["-c", `mount c ${GAME_ROOT}`, "-c", "c:"];
    if (cycles > 0) args.push("-c", `cycles=${Math.round(cycles)}`);
    // What the game's own setup expects of the machine (the registry), as a
    // configuration file: DOSBox's `config -set` rewrites the list of
    // startup commands it is itself run from, and the game's is lost.
    const conf = gameInfo() && gameInfo().dosbox;
    if (conf) { Module.FS.writeFile("/game.conf", conf); args.unshift("-conf", "/game.conf"); }
    Module.callMain([...args, "-c", cmd]);
    setInterval(keepSaves, 4000);
    setInterval(showPerformance, 1000);
    for (const ev of ["pagehide", "visibilitychange"]) window.addEventListener(ev, keepSaves);
    $("fullscreen").hidden = false;
    if (fromGesture) goFullscreen();
    status(source.game.multiplayer
      ? `Room ${cfg.code}. Share this page's link so friends land in the same room.`
      : `${source.game.title} is running.`);
  } catch (e) {
    running = false;
    log("failed: " + (e && e.stack ? e.stack : e));
    status("Failed to start: " + (e && e.message ? e.message : e));
  }
}

// -- how it is running -----------------------------------------------------------

// The game's own figures from the flight loop (src/cpu/wcnet_perf.h): frames
// per second, how much of each frame's time the game needed, whether the
// emulator keeps up with the clock, and how long a frame waits for the other
// players.  They answer "why is it slow": a load near 100% means the game
// needs more cycles in flight (?env.WCFLIGHTCYCLES=N), an emulator below 100%
// means this computer cannot deliver the cycles asked for (use fewer), and a
// long wait means the other player's computer or the connection is the brake.
function showPerformance() {
  const M = window.DOSBox;
  const el = $("perf");
  if (!M || !M._wc_web_perf || !M._wc_web_in_flight || !M._wc_web_in_flight() || M._wc_web_perf(0) < 0) { el.textContent = ""; return; }
  const fps = M._wc_web_perf(0), speed = M._wc_web_perf(1), wait = M._wc_web_perf(2), cycles = M._wc_web_perf(3), load = M._wc_web_perf(5);
  // (No load figure for a game that paces itself, as Wing Commander II does.)
  // The host sets the pace and a wingman's game waits for the host's frames
  // as a matter of course; it is the host's wait that says somebody is slow.
  const alone = !lobby.game || lobby.game.players.filter((p) => p.occupied).length < 2;
  const parts = [`${fps.toFixed(1)} frames/s`, load < 0 ? `${cycles} cycles` : `game busy ${Math.round(100 * load)}% of the time at ${cycles} cycles`,
                 `emulator at ${Math.round(100 * speed)}% of real time`];
  if (!alone) parts.push(isHost() ? `${wait.toFixed(0)} ms/frame waiting for the other players` : "following the host's pace");
  const warn = speed < 0.93 ? "This computer cannot keep up with the emulated CPU: fewer cycles would run smoother (Ctrl+F11)."
             : load > 0.95 ? "The game needs more CPU than it is given: try ?env.WCFLIGHTCYCLES=20000 in the address."
             : (!alone && isHost() && wait > 10) ? "Another player's computer or connection is holding the game back." : "";
  el.innerHTML = esc(parts.join(" · ")) + (warn ? ` <span class="warn">${esc(warn)}</span>` : "");
}

// -- controllers ---------------------------------------------------------------

// A game controller chosen in this window drives the running game's mouse and
// keyboard (web/gamepad.js); the registry says where the game's steering
// pointer rests and how far it reaches.
initControls({
  module: () => (running && window.DOSBox) || null,
  pointer: () => (gameInfo() && gameInfo().pointer) || null,
  log,
});

// -- go ------------------------------------------------------------------------

void initSources();
// A shared link (?room=CODE) puts the visitor straight into the room.
if (query.get("room")) void joinRoom();
