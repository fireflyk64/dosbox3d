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
import { readZip, extractInstaller, looksLikeInstaller, identifyGame, installFiles,
         saveGame, loadGames, forgetGame, totalSize } from "./gamefiles.js";

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
for (const k of ["room", "callsign", "lastname", "players"]) if (query.get(k)) $(k).value = query.get(k);
if (query.get("relay")) $("relay").checked = true;
if (query.get("verbose")) $("verbose").checked = true;
if (!$("room").value) $("room").value = "WC-" + Math.random().toString(36).slice(2, 6).toUpperCase();

// -- game files ----------------------------------------------------------------

// The selected source: { label, game, root, files } with files as
// [{path, data}] relative to the game directory (drive C).
let source = null;
let saved = [];

function setSource(s) {
  source = s;
  if (s) {
    sourceStatus(`Ready: ${s.game.title} from ${s.label} (${s.files.length} files, ${mb(totalSize(s.files))}).` +
                 (s.game.multiplayer ? "" : " The multiplayer hooks only exist for Wing Commander 1, so this runs single-player."));
  }
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

// -- missions ------------------------------------------------------------------

// Wing Commander's Vega campaign as the game numbers it: series 1.. and
// mission 0.. within the series (what the MIS / SERIES environment of the
// hooks takes).  The system of each series and the number of missions were
// read off the game's own briefing screens (an index past the last mission
// shows a black screen); series 14 and up do not exist in WC.EXE.  40 in all.  With a forced mission everyone skips the barracks and
// flies the same mission from a fresh state, so nobody's save game matters.
const CAMPAIGN = [
  { series: 1, system: "Enyo", missions: 2 },
  { series: 2, system: "McAuliffe", missions: 3 },
  { series: 3, system: "Gateway", missions: 3 },
  { series: 4, system: "Gimle", missions: 3 },
  { series: 5, system: "Brimstone", missions: 3 },
  { series: 6, system: "Chengdu", missions: 3 },
  { series: 7, system: "Dakota", missions: 3 },
  { series: 8, system: "Port Hedland", missions: 3 },
  { series: 9, system: "Kurasawa", missions: 3 },
  { series: 10, system: "Rostov", missions: 3 },
  { series: 11, system: "Hubble's Star", missions: 3 },
  { series: 12, system: "Venice", missions: 4 },
  { series: 13, system: "Hell's Kitchen", missions: 4 },
];
const missionKey = (m) => (m ? `${m.series}/${m.mis}` : "");
function parseMission(v) {
  const m = /^(\d+)\/(\d+)$/.exec(v || "");
  return m ? { series: Number(m[1]), mis: Number(m[2]) } : null;
}
function missionLabel(m) {
  if (!m) return "the campaign from the barracks";
  const s = CAMPAIGN.find((c) => c.series === m.series);
  return s ? `${s.system} ${m.mis + 1}` : `series ${m.series}, mission ${m.mis + 1}`;
}
function buildMissionMenu() {
  const sel = $("mission");
  sel.innerHTML = "";
  const add = (value, text) => { const o = document.createElement("option"); o.value = value; o.textContent = text; sel.appendChild(o); };
  add("", "Campaign: start in the barracks (the host's save game decides)");
  for (const s of CAMPAIGN) for (let i = 0; i < s.missions; i++) add(`${s.series}/${i}`, `${s.system} ${i + 1}  (series ${s.series}, mission ${i + 1})`);
}
function renderMission() {
  const sel = $("mission");
  sel.value = missionKey(lobby.mission);
  sel.disabled = !isHost() || running;
  $("missionHint").textContent = lobby.mission
    ? `Everyone flies ${missionLabel(lobby.mission)} from a fresh start with the callsigns entered above.`
    : (isHost() ? "The barracks: your save game and your walk to the briefing decide the mission; wingmen must walk into the briefing room too, and get your mission there."
                : "The host flies from the barracks: after the host starts, walk into the briefing room on your ship and you get the host's mission.");
}

// -- the room ------------------------------------------------------------------

const lobby = { game: null, names: new Map(), links: new Map(), flying: new Set(), unsubscribe: null, mission: null,
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
const sayHello = (to, reply) => sendLobby({ t: "hello", name: myName(), flying: running, reply: !!reply, mission: isHost() ? missionKey(lobby.mission) : undefined }, to);

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
        if (ev.from === 0 && !isHost() && m.mission !== undefined) {
          const chosen = parseMission(m.mission);
          if (missionKey(chosen) !== missionKey(lobby.mission)) {
            lobby.mission = chosen;
            chatLine(`${esc(m.name)} picked ${esc(missionLabel(chosen))}`, "sys");
            renderMission();
          }
        }
        if (!m.reply) sayHello(ev.from, true);
        renderRoster(); updateActions();
      } else if (m.t === "chat") {
        chatLine(`<span class="name">${esc(m.name)}:</span> ${esc(m.text)}`);
      } else if (m.t === "start" && ev.from === 0 && !running) {
        lobby.mission = parseMission(m.mission);
        renderMission();
        chatLine(`${esc(lobby.names.get(0) || "The host")} started ${esc(missionLabel(lobby.mission))}`, "sys");
        lobby.flying.add(0); renderRoster();
        if (source) void start(false);
        else status("The host started the game. Load your game files above, then press Fly to join.");
      }
      break;
    }
    case "player-joined": case "player-rejoined": case "player-replaced":
      lobby.links.set(ev.playerId, "down"); lobby.names.delete(ev.playerId); lobby.flying.delete(ev.playerId);
      renderRoster(); break;
    case "player-left":
      if (ev.reason === "explicit-leave") {
        chatLine(`${esc(lobby.names.get(ev.playerId) || "Player " + (ev.playerId + 1))} left the room`, "sys");
        lobby.names.delete(ev.playerId); lobby.links.delete(ev.playerId); lobby.flying.delete(ev.playerId);
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
      create: { maxPlayers: Math.max(2, Math.min(3, Number($("players").value) || 3)), waitUntilFull: false, allowLateJoin: true, allowReconnect: true, allowReplacement: true },
      storage: "session",
      storageKey: "wclobby-" + code,
      forceRelay: $("relay").checked,
    });
    lobby.game = game;
    lobby.names.clear(); lobby.links.clear(); lobby.flying.clear();
    lobby.unsubscribe = game.onEvent(onLobbyEvent);
    const url = new URL(location.href);
    url.searchParams.set("room", code);
    url.searchParams.delete("callsign"); url.searchParams.delete("lastname");
    history.replaceState(null, "", url);
    $("lobby").hidden = false;
    $("leave").hidden = false;
    if (game.selfId === 0) lobby.mission = parseMission(query.get("mission")) || { series: 1, mis: 0 };
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
  lobby.names.clear(); lobby.links.clear(); lobby.flying.clear();
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
  chatLine(`<span class="name">${esc(myName())}:</span> ${esc(text)}`);
  sendLobby({ t: "chat", name: myName(), text });
});
$("callsign").addEventListener("change", () => { if (lobby.game) { sayHello(); renderRoster(); } });
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
  if (isHost()) sendLobby({ t: "start", mission: missionKey(lobby.mission) });
  void start(true);
});
$("fullscreen").addEventListener("click", () => goFullscreen());
function goFullscreen() {
  const req = canvas.requestFullscreen || canvas.webkitRequestFullscreen;
  if (!req) return;
  Promise.resolve(req.call(canvas)).catch(() => { /* needs a click: the button stays */ });
  canvas.focus();
}

async function start(fromGesture) {
  if (running) return;
  running = true;
  $("fly").disabled = true;
  for (const el of ["gamefile", "useServer", "useSaved", "forget", "exePicker", "callsign", "lastname", "verbose", "leave"]) $(el).disabled = true;
  try {
    const cfg = {
      code: lobby.game.code,
      callsign: $("callsign").value.trim(),
      lastname: $("lastname").value.trim(),
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
    if (cfg.callsign) env.WCCALLSIGN = cfg.callsign;
    if (cfg.lastname) env.WCLASTNAME = cfg.lastname;
    if (lobby.mission && source.game.multiplayer) {
      env.MIS = String(lobby.mission.mis);
      env.SERIES = String(lobby.mission.series);
    }
    // Any ?env.NAME=value lands in DOSBox's environment: the same knobs as
    // the native build (MIS, SERIES, WCNET_AUTOKEYS, WCNET_LOG, ...).
    for (const [k, v] of query) if (k.startsWith("env.")) env[k.slice(4)] = v;
    if (env.MIS !== undefined || env.SERIES !== undefined) {
      log(`flying ${missionLabel({ series: Number(env.SERIES || 1), mis: Number(env.MIS || 0) })} straight from the hangar`);
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

    sayHello();
    renderRoster(); renderMission();
    status(source.game.multiplayer ? `Room ${cfg.code}: starting…` : `Starting ${source.game.title}…`);
    canvas.focus();
    // ?cmd=... runs another DOS command instead of the game (debugging aid).
    const cmd = query.get("cmd") || source.game.run;
    window.DOSBox = Module;
    Module.callMain(["-c", `mount c ${GAME_ROOT}`, "-c", "c:", "-c", cmd]);
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

// -- go ------------------------------------------------------------------------

void initSources();
// A shared link (?room=CODE) puts the visitor straight into the room.
if (query.get("room")) void joinRoom();
