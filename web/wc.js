// Page glue for the browser build of DOSBox + Wing Commander multiplayer.
//
// Flow: the player brings the game files (gamefiles.js), or picks the game
// built into the page (RetroVega: no files to bring), joins a lobbylink
// room from the page (roster + chat over the same WebRTC links the game
// will use), and the host starts the game for everyone.  Pilots who have
// nobody to fly with meet in the public lobby (hall.js), a chat room where
// a room's code can be said and clicked; the page connects to it only once
// a game the hooks can fly is loaded.  DOSBox then runs
// with that live room connection: src/wclobby_web.js adopts Module.lobbyGame
// instead of joining the room a second time.  Lobby chat and presence
// travel as reliable messages tagged with a 4-byte prefix that the game
// protocol never produces, so the transport filters them out.
import { P2PGame } from "./p2p-client.js";
import { initControls, KEY, stepped } from "./gamepad.js";
import { initTouch } from "./touch.js";
import { initVoice } from "./voice.js";
import { loadSo3 } from "./so3.js";
import { readZip, extractInstaller, looksLikeInstaller, identifyGame, installFiles,
         saveGame, loadGames, forgetGame, totalSize, gameById, gameByTag, programsIn, knownBuild, GAMES } from "./gamefiles.js";
import { newRoomCode, retagRoomCode, normalizeCode, roomTag, GAME_TAGS, MAX_CHARS, CALLSIGNS, pickCallsign, givesWay, sameName } from "./chatfilter.js";
import { initHall, OTHER_SERVER, serverDown } from "./hall.js";
import { createSetProvider, readTarGz } from "./rvsets.js";

const $ = (id) => document.getElementById(id);
const logEl = $("log");
const canvas = $("canvas");

const DEFAULT_SERVER = "https://pqrstuvw.xyz/lobbylink";
const DATA_URL = "wc.tar.gz";
const GAME_ROOT = "/game";
const LOBBY_MAGIC = [0x57, 0x43, 0x4c, 0x01]; // "WCL" + version; never a valid protobuf start
const query = new URLSearchParams(location.search);
// RetroVega, the registry's built-in game, is in every page's "Play" menu and
// the game of a page with no other (the user, 2026-11-03: "make the default
// site include the retrovega flag by default and default to that system if
// there are no other games"; before, only with ?retrovega, which still does
// no harm).  Its pilots meet everyone else's in WC-LOBBY (lobbyName).
// The "Capship hull" box is offered only with ?capship (or &capship, or
// #capship) in the address (the user's call, 2026-10-22); a wingman's game
// takes the host's choice either way.
const capshipOption = query.has("capship") || /capship/i.test(location.hash) || query.get("caphull") === "1";

// The lobby server only introduces the browsers to each other, and any
// lobbylink server does that: should the public one be down, pilots agree on
// another and pass this page's address around with ?server=URL in it (the
// field under Options is the same).  The other server has to allow this
// page's origin, and to be served over https when the page is.  A server
// that is not the public one is named at the top of the page and is in every
// link the page makes, so that those who follow one meet there.
function normalizeServer(v) {
  v = String(v || "").trim();
  return !v ? "" : /^[a-z][a-z0-9+.-]*:\/\//i.test(v) ? v : "https://" + v;
}
$("server").value = normalizeServer(query.get("server")) || DEFAULT_SERVER;
const serverUrl = () => normalizeServer($("server").value) || DEFAULT_SERVER;
const otherServer = () => serverUrl() !== DEFAULT_SERVER;
const withServer = (url) => { if (otherServer()) url.searchParams.set("server", serverUrl()); else url.searchParams.delete("server"); return url; };
// This page's address for a room: what a room code in the lobby links to.
function roomLink(code) {
  const url = new URL(location.href);
  url.search = ""; url.hash = "";
  url.searchParams.set("room", code);
  return withServer(url).toString();
}
function showServer() {
  let name = serverUrl();
  try { const u = new URL(name); name = u.host + u.pathname.replace(/\/+$/, ""); } catch (e) { /* as typed */ }
  $("serverTag").hidden = !otherServer();
  $("serverTag").textContent = otherServer() ? `lobby server: ${name}` : "";
}
showServer();
$("server").addEventListener("change", () => {
  $("server").value = serverUrl();
  history.replaceState(null, "", withServer(new URL(location.href)));
  showServer();
  hall.serverChanged();
});

function log(line) {
  console.log(line);
  const atBottom = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 4;
  logEl.textContent += line + "\n";
  if (logEl.textContent.length > 200000) logEl.textContent = logEl.textContent.slice(-100000);
  if (atBottom) logEl.scrollTop = logEl.scrollHeight;
  noticeGameEnd(String(line));
}
function status(text) { $("status").textContent = text; }

// A game that stops by itself leaves a DOS prompt, or a picture that stands
// still, and a player with nothing to report but "it crashed".  The hooks
// log the game's end with the text it left on the screen and its stack
// (src/cpu/wcnet_game.cpp, game_program_ended), and a trap in the emulator
// itself comes as a page error: both are said in the status line, and
// "Copy log" takes the whole log to the clipboard for a report.
let endNotice = null;
let emulatorStarted = false;   // (declared here: an error can come before the rest of this file has run)
// The game's one line of help, "To transmit comms use the '0' key or tap Shift", is for
// a player who has never chatted: once a message has gone out from this
// browser it is not shown again, and the box in the room form turns it off
// before that.
const HINT_KEY = "wc:hint";
try { if (localStorage.getItem(HINT_KEY) === "off") $("hint").checked = false; } catch (e) { /* shown */ }
$("hint").addEventListener("change", () => { try { localStorage.setItem(HINT_KEY, $("hint").checked ? "on" : "off"); } catch (e) { /* not kept */ } });
function noticeChat(line) {
  if (/^wcnet: comms: message sent$/.test(line)) {
    $("hint").checked = false;
    try { localStorage.setItem(HINT_KEY, "off"); } catch (e) { /* not kept */ }
  }
}
// A pilot who flies stays in the public lobby (the user, 2026-11-03: "right
// now actually everyone can always stay in the lobby during game. It's too
// quiet for the moment to worry--we can deploy lobby departure as a future
// optimization if it gets too busy"); hall.shut is kept for that day.  When
// the game is over on this page, however it ended, the lobby is open to it
// again (a no-op while it never closed).
function flightOver() { setTimeout(() => hall.reopen(), 0); }
// The host's game holds at a briefing for a wingman who has flown a mission
// with it (the layer's lockstep; the first mission together never waits),
// and a wingman's for the host's briefing: said in the room's chat, where
// the other pilot's lines come (the user: "before freezing it output
// 'waiting on other player...' in the place where player chat can appear").
var briefingAsked = null;   // (var: a log line can come before this file has run this far)
function noticeWaiting(line) {
  let m;
  if ((m = /^wcnet: waiting on the other player, (.*), at the briefing$/.exec(line))) {
    setTimeout(() => { chatLine(`Waiting on the other player (${esc(m[1])})…`, "sys"); status(`Waiting on the other player (${m[1]}) at the briefing…`); }, 0);
  } else if ((m = /^wcnet: the wait: (.*)$/.exec(line))) {
    // (a wait of the link's at the launch: the picture says it too, across its middle)
    setTimeout(() => status(m[1]), 0);
  } else if (/^wcnet: the wait is over$/.test(line)) {
    setTimeout(() => status(""), 0);
  } else if (/^wcnet: the other player is at the briefing/.test(line)) {
    setTimeout(() => { chatLine("The other player is at the briefing: the mission begins.", "sys"); status(""); }, 0);
  } else if (/^wcnet: asking the server which briefing to load$/.test(line)) {
    // (a wingman's: said only when the host's answer takes a while)
    clearTimeout(briefingAsked);
    briefingAsked = setTimeout(() => { briefingAsked = null; chatLine("Waiting on the host…", "sys"); }, 1500);
  } else if (/^wcnet: briefing for mission /.test(line) && briefingAsked) {
    clearTimeout(briefingAsked);
    briefingAsked = null;
  }
}
function noticeGameEnd(line) {
  noticeChat(line);
  noticeWaiting(line);
  if (/^wcnet: RV ABI mismatch: /.test(line)) {
    // (the game and the emulator of this page were not built from one interface: RetroVega flies, alone)
    setTimeout(() => status(`This page's RetroVega and its emulator do not match (${line.replace(/^wcnet: RV ABI mismatch: /, "").split(";")[0]}): the game flies alone, shared flights are off. Reload the page; if it stays, "Copy log" has the details for a report.`), 0);
  }
  if (/^Exit to error: /.test(line)) {
    // (DOSBox itself gave up: an instruction or a device it does not emulate.)
    setTimeout(() => status(`The emulator stopped: ${line.slice(15, 175)}. Reload the page to fly again; "Copy log" under the picture has the details for a report.`), 0);
    flightOver();
  } else if (/^wcnet: .+ ended$/.test(line)) {
    endNotice = { lines: [] };
    flightOver();
    setTimeout(() => {
      const said = endNotice.lines.join(" / ");
      endNotice = null;
      status(`The game stopped by itself${said ? `: "${said.slice(0, 160)}"` : ""}. Reload the page to fly again; "Copy log" under the picture has the details for a report.`);
    }, 300);
  } else if (endNotice && /^wcnet:\s+screen: /.test(line) && !/^[A-Z]:\\/.test(line.replace(/^wcnet:\s+screen: /, ""))) {
    endNotice.lines.push(line.replace(/^wcnet:\s+screen: /, "").trim());
  }
}
function emulatorFailed(what) {
  if (!emulatorStarted) return;
  log(`the emulator stopped: ${what}`);
  status(`The emulator stopped (${String(what).split("\n")[0].slice(0, 120)}). Reload the page to fly again; "Copy log" under the picture has the details for a report.`);
  flightOver();
}
window.addEventListener("error", (e) => emulatorFailed((e.error && e.error.stack) || e.message));
window.addEventListener("unhandledrejection", (e) => emulatorFailed((e.reason && (e.reason.stack || e.reason.message)) || String(e.reason)));
$("copyLog").addEventListener("click", async () => {
  const text = `${navigator.userAgent}\n${location.href.replace(/([?&]callsign=)[^&]*/, "$1…")}\n\n${logEl.textContent}`;
  try { await navigator.clipboard.writeText(text); $("copyLog").textContent = "Copied"; }
  catch (e) { const r = document.createRange(); r.selectNodeContents(logEl); const sel = getSelection(); sel.removeAllRanges(); sel.addRange(r); $("copyLog").textContent = "Selected: press Ctrl+C"; }
  setTimeout(() => { $("copyLog").textContent = "Copy log"; }, 2500);
});
// The line under the drop zone: progress and trouble.  A game that is simply
// ready says nothing there (the "Play" menu shows it); data-state tells the
// page tests what the text no longer does: "busy", "ready" or "error".
function sourceStatus(text, state) { const el = $("sourceStatus"); el.textContent = text; el.removeAttribute("title"); if (state) el.dataset.state = state; }
const mb = (n) => (n / 1048576).toFixed(1) + " MB";

// Prefill from the URL (?room=CODE&callsign=...) so a link can be shared.
for (const k of ["room", "callsign", "firstname", "lastname"]) if (query.get(k)) $(k).value = query.get(k);
// The pilot's names are remembered in this browser and filled in on the next
// visit.  A name the link brought is the pilot's own (the page's links carry
// no names but the ones typed here) and is kept too; what the pilot types
// over either is what is kept from then on.
const NAME_KEY = (k) => `wc:name:${k}`;
const keepName = (k) => { try { localStorage.setItem(NAME_KEY(k), $(k).value.trim()); } catch (e) { /* not kept */ } };
for (const k of ["callsign", "firstname", "lastname"]) {
  if ($(k).value) keepName(k);
  else { try { $(k).value = localStorage.getItem(NAME_KEY(k)) || ""; } catch (e) { /* none */ } }
  $(k).addEventListener("change", () => keepName(k));
}
// The callsign a pilot goes by until they type one: one of the list, given
// once and remembered here with the moment it was given (of two pilots called
// the same, the one given it later takes another: chatfilter's givesWay).
const GIVEN_KEY = "wc:name:given";
let given = (() => {
  try {
    const g = JSON.parse(localStorage.getItem(GIVEN_KEY) || "null");
    if (g && CALLSIGNS.includes(g.name) && Number.isFinite(g.since)) return { name: g.name, since: g.since };
  } catch (e) { /* a fresh one */ }
  return null;
})();
function giveCallsign(taken) {
  given = { name: pickCallsign(taken), since: Date.now() };
  try { localStorage.setItem(GIVEN_KEY, JSON.stringify(given)); } catch (e) { /* not kept */ }
  $("callsign").placeholder = given.name;
}
if (given) $("callsign").placeholder = given.name; else giveCallsign([]);
if (query.get("relay")) $("relay").checked = true;
if (query.get("verbose")) $("verbose").checked = true;
// A link to a room (?room=CODE) whose room this page has not tried to join
// yet: joinLinked, at the end of this file, says when it does.
let linked = !!query.get("room");
const linkWaits = () => linked && !lobby.game && normalizeCode($("room").value.trim()) === normalizeCode(query.get("room").trim());
// A new room's code is the game's tag and four digits (WC1-4821; "WC-" as
// long as no game is loaded): said in the public lobby it tells who can
// join.  `autoCode` is the code this page made up: it follows the game the
// player loads until the player types another or joins.
let autoCode = "";
if (!$("room").value) { autoCode = newRoomCode(""); $("room").value = autoCode; }
function retagRoom() {
  if (lobby.game || !autoCode || $("room").value !== autoCode) return;
  autoCode = retagRoomCode(autoCode, gameTag());
  $("room").value = autoCode;
}
// The game a code is for, when it says: the tag of a registry game.
const codeGame = (code) => (GAME_TAGS.includes(roomTag(code)) ? gameByTag(roomTag(code)) : null);

// -- game files ----------------------------------------------------------------

// The selected source: { label, game, root, files } with files as
// [{path, data}] relative to the game directory (drive C); a game built into
// the page (`builtin`) has no files until it is flown (start fetches them).
let source = null;
let saved = [];
// The files dropped on this page while a built-in game is picked: still in
// the "Play" menu, as the saved copies are.
let shelved = null;

// A game directory can hold more than one program of the registry (Wing
// Commander's has The Secret Missions 2, Wing Commander II's the two Special
// Operations).  Which one runs: the one the player picks in the menu
// (`want`), else the one the room's code names, else the one picked the last
// time, else the game itself.
const PROGRAM_KEY = (base) => `wc:program:${base}`;
function setSource(s, want) {
  // What the page knows about a game is the registry's, also for a copy this
  // browser saved when the registry said something else.
  if (s && gameById(s.game.id)) {
    const base = gameById(gameById(s.game.id).partOf || s.game.id);
    const programs = programsIn(s.files);
    if (!programs.includes(base)) programs.unshift(base);
    const roomCode = lobby.game ? lobby.game.code : ($("room").value !== autoCode ? $("room").value.trim() : "");
    let kept = null;
    try { kept = localStorage.getItem(PROGRAM_KEY(base.id)); } catch (e) { /* none */ }
    // (In a room the host's program is everyone's: the code only says what
    // the room was made for.)
    const hostsProgram = lobby.game && !isHost() ? lobby.games.get(0) || "" : "";
    const g = programs.find((p) => p.id === want) || programs.find((p) => p.id === hostsProgram) || programs.find((p) => p === codeGame(roomCode)) || programs.find((p) => p.id === kept) || base;
    s.base = base.id; s.programs = programs;
    s.game = { id: g.id, title: g.title, run: g.run, multiplayer: g.multiplayer };
    // (A file of the game's name is not yet the game: the hooks fly one
    // build of each program, and the public lobby is for pilots who have it.
    // A game built into the page is the build the hooks know.)
    s.known = s.builtin ? !!g.builtin : knownBuild(g, s.files);
  } else if (s) {
    s.base = s.game.id; s.programs = []; s.known = false;
  }
  source = s;
  renderPrograms();
  if (s) {
    // Ready: the "Play" menu says which, and the line under it speaks only
    // of what runs single-player.  (Where the files came from and how many
    // is the menu's tooltip: the panel stays a few lines, and Fly in view.)
    sourceStatus(!s.game.multiplayer ? `${s.game.title}: the multiplayer hooks do not know this program, so it runs single-player.`
                 : !s.known ? `${s.game.title}: its ${gameById(s.game.id).detect[0]} is not the build the multiplayer hooks know: it runs single-player, and the public lobby stays closed.` : "", "ready");
    $("programLabel").title = (s.builtin ? `${s.game.title} is built into this page: its files come from the site when you fly.`
                               : `From ${s.label}: ${s.files.length} files, ${mb(totalSize(s.files))}.`)
                              + " The menu has every program of the games this browser holds; in a room everyone plays the host's.";
  }
  $("source").classList.toggle("loaded", !!s);
  // The help under the game is the loaded game's own; so are its keys, where it has a list (RetroVega's flight).
  const keys = s && gameInfo() && gameInfo().keys;
  $("gameKeys").innerHTML = keys || "";
  $("tfFlightLabel").hidden = !isTf();
  gameLook();
  $("gameKeys").hidden = !keys;
  const hints = s && gameInfo() && gameInfo().campaign && gameInfo().campaign.hints;
  $("helpMission").textContent = (hints && hints.help) || (gameInfo() && gameInfo().help)
    || (s ? "The multiplayer hooks do not know this program: it runs single-player." : "Load the game files (step 1): the help is the game's own.");
  describeSaves();
  // (Wing Commander II's people use a first name; the first game has none.)
  $("firstnameLabel").hidden = !(gameInfo() && gameInfo().firstName);
  retagRoom();
  if (lobby.game) { sayHello(); buildMissionMenu(); renderMission(); }
  hall.gameChanged();
  updateActions();
  joinLinked();
}
// Which files run a program: the loaded ones ("loaded"), the files dropped
// before a built-in game was picked ("shelved"), the newest copy this
// browser saved that has it (its record), the page itself ("builtin"), or
// nothing.
const runs = (s, id) => !!s && (s.game.id === id || (s.programs || []).some((p) => p.id === id));
function holderOf(id) {
  if (!id) return null;
  if (runs(source, id)) return "loaded";
  if (runs(shelved, id)) return "shelved";
  const copy = saved.find((g) => g.id === id || programsIn(g.files).some((p) => p.id === id));
  if (copy) return copy;
  return builtins().some((g) => g.id === id) ? "builtin" : null;
}
// The games built into the page.
const builtins = () => GAMES.filter((g) => g.builtin);
// The game built into the page, picked.
function useBuiltin(id) {
  const g = gameById(id);
  if (source && !source.builtin && !source.saved) shelved = source;
  setSource({ label: "this page", builtin: true, game: { id: g.id, title: g.title, run: g.run, multiplayer: g.multiplayer },
              root: "", files: [] }, id);
}
// Switches to a program wherever this browser has it (holderOf).
function switchTo(holder, id) {
  if (holder === "loaded") setSource(source, id);
  else if (holder === "shelved") { const s = shelved; shelved = null; setSource(s, id); }
  else if (holder === "builtin") useBuiltin(id);
  else if (holder) useSavedCopy(holder, id);
}
// The "Play" menu: every program of every game this browser holds (the
// loaded files and the saved copies), by game, the loaded one selected.
// Picking a program of another saved copy switches to that copy.
function renderPrograms() {
  const sel = $("program");
  sel.innerHTML = "";
  const groups = new Map();  // base game id -> its optgroup
  const seen = new Set();
  const add = (g) => {
    if (seen.has(g.id)) return;
    seen.add(g.id);
    const base = gameById(g.partOf || g.id) || g;
    let grp = groups.get(base.id);
    if (!grp) { grp = document.createElement("optgroup"); grp.label = base.title; groups.set(base.id, grp); sel.appendChild(grp); }
    const o = document.createElement("option"); o.value = g.id; o.textContent = g.title; grp.appendChild(o);
  };
  if (source) {
    for (const g of source.programs || []) add(g);
    if (!gameById(source.game.id)) add({ id: source.game.id, title: source.game.title });
  }
  if (shelved) for (const g of shelved.programs || []) add(g);
  for (const copy of saved) for (const g of (gameById(copy.id) ? programsIn(copy.files) : [{ id: copy.id, title: copy.title }])) add(g);
  for (const g of builtins()) add(g);
  // (One game with one program needs no heading over it.)
  if (groups.size === 1) { const grp = groups.values().next().value; if (grp.children.length === 1) { sel.appendChild(grp.firstChild); grp.remove(); } }
  if (source) sel.value = source.game.id;
  $("programLabel").hidden = !sel.options.length;
}
// The public lobby is for a pilot with a game to fly: the page connects to
// it only while one is loaded that the hooks know.
const lobbyReady = () => !!(source && source.known);
$("program").addEventListener("change", () => {
  if (running) return;
  const id = $("program").value;
  const holder = holderOf(id);
  if (!holder) return;
  const g = gameById(id);
  if (g) { try { localStorage.setItem(PROGRAM_KEY(g.partOf || g.id), id); } catch (e) { /* not kept */ } }
  switchTo(holder, id);  // (a program of another game this browser holds, or the page's own)
});
// The loaded game's tag for a room code ("WC1"), or "" (none, or a program
// the registry does not know).
const gameTag = () => (gameInfo() && gameInfo().tag) || "";

async function useFiles(files, label, { persist = true } = {}) {
  const id = identifyGame(files);
  if (!id.game) {
    $("pickerLabel").hidden = false;
    const sel = $("exePicker");
    sel.innerHTML = "";
    for (const c of id.candidates) { const o = document.createElement("option"); o.value = c; o.textContent = c; sel.appendChild(o); }
    sourceStatus(id.candidates.length
      ? `${label}: no game I recognise (${files.length} files). Pick the executable to run.`
      : `${label}: no DOS executable found among ${files.length} files.`, "error");
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
    sourceStatus(`Reading ${file.name} (${mb(file.size)})…`, "busy");
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
                 `A .zip of the installed game folder always works; for a GOG installer you can also unpack it with innoextract and zip the result.`, "error");
  } finally {
    sourceButtons(true);
  }
}

// --- RetroVega's ship sets, fetched when a flight asks for them ---
//
// RetroVega's ship sets and asteroid rocks, fetched and made when the game asks (RVWANT.TXT: web/rvsets.js)
const rvSets = createSetProvider({ base: "rv/", root: GAME_ROOT, status, log });

// The buttons that change the game files: off while a file is being read
// and once the game runs.
function sourceButtons(on) {
  for (const el of [$("useServer"), $("program"), ...$("savedList").querySelectorAll("button")]) el.disabled = !on;
}
// (Said with its date and its file: a game that is simply there on the
// next visit is the copy this browser kept, not one the site has.)
const useSavedCopy = (g, want) => setSource({ label: `the copy this browser saved${g.when ? " on " + new Date(g.when).toLocaleDateString() : ""} (${g.label})`, saved: true, savedId: g.id,
  game: { id: g.id, title: g.title, run: g.run, multiplayer: g.multiplayer }, root: "", files: g.files }, want);

// The copies this browser kept, side by side on one line: each with its
// size and a "forget" for one that is wrong or damaged.  (Playing one is
// the "Play" menu's business: it lists their programs.)
async function refreshSaved() {
  saved = (await loadGames()).sort((a, b) => (b.when || 0) - (a.when || 0));
  const list = $("savedList");
  list.innerHTML = "";
  if (saved.length) list.appendChild(document.createTextNode("Saved here: "));
  saved.forEach((g, i) => {
    const chip = document.createElement("span");
    chip.className = "saved";
    chip.title = `${g.title}: ${g.files.length} files from ${g.label}${g.when ? ", saved " + new Date(g.when).toLocaleDateString() : ""}. Kept in this browser only.`;
    chip.append(`${g.title} (${mb(totalSize(g.files))}, `);
    const b = document.createElement("button");
    b.type = "button"; b.className = "link"; b.textContent = "forget";
    b.title = `Remove this copy of ${g.title} from the browser; the files on your disk are untouched`;
    b.addEventListener("click", () => void forgetSaved(g));
    chip.append(b, ")");
    list.appendChild(chip);
    if (i < saved.length - 1) list.appendChild(document.createTextNode(" · "));
  });
  renderPrograms();
}
async function forgetSaved(g) {
  if (!confirm(`Forget the saved copy of ${g.title}? Load its files again to play it.`)) return;
  await forgetGame(g.id);
  await refreshSaved();
  if (source && source.savedId === g.id) {
    const own = builtins()[0];
    if (own) useBuiltin(own.id); else setSource(null);
  }
  sourceStatus(`Forgot the saved copy of ${g.title}.`);
}

async function initSources() {
  const drop = $("dropzone");
  for (const ev of ["dragenter", "dragover"]) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); });
  for (const ev of ["dragleave", "drop"]) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove("over"); });
  drop.addEventListener("drop", (e) => { const f = e.dataTransfer.files && e.dataTransfer.files[0]; if (f) void handleFile(f); });
  $("gamefile").addEventListener("change", (e) => { const f = e.target.files && e.target.files[0]; if (f) void handleFile(f); });

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
  // (A link to a room for one game picks that game among the saved copies.)
  // (Nothing saved and no server copy: the game built into the page, so
  // that everybody can fly, and meet in the lobby, with nothing to bring.)
  const wanted = codeGame(lobby.game ? lobby.game.code : $("room").value.trim());
  const fits = wanted && saved.find((g) => programsIn(g.files).includes(wanted));
  if (wanted && builtins().includes(wanted)) useBuiltin(wanted.id);
  else if (saved.length) useSavedCopy(fits || saved[0]);
  else if (serverCopy) $("useServer").click();
  else if (builtins().length) useBuiltin(builtins()[0].id);
}

// -- save games ----------------------------------------------------------------

// A game's registry entry lists the files it keeps its saved games in (Wing
// Commander: one file holding all eight bunks).  This browser keeps a copy in
// local storage, keyed by game: it is put back into the game directory at
// every start and refreshed whenever the running game changes the file.
const gameInfo = () => (source && gameById(source.game.id)) || null;
const saveKey = (rel) => `wcsave:${source.game.id}:${rel.toUpperCase()}`;
// A registry entry's saves may be patterns ("*.SAV": Privateer names its saved games after what the player types):
// then they are the files at the top of the game directory that match, and the copies of such files this browser
// keeps.  A game with names only (every other) gets its list as it is.
const savePatterns = () => { const g = gameInfo(); return (g && g.saves) || []; };
const isSavePattern = (rel) => rel.includes("*");
const savesByPattern = () => savePatterns().some(isSavePattern);
const savePatternRe = (pat) => new RegExp("^" + pat.toUpperCase().replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*") + "$");
function keptSaveNames() {
  const prefix = source ? `wcsave:${source.game.id}:` : null;
  const out = [];
  try {
    for (let i = 0; prefix && i < localStorage.length; i++) {
      const k = localStorage.key(i);
      if (k && k.startsWith(prefix)) out.push(k.slice(prefix.length));
    }
  } catch (e) { /* none kept */ }
  return out;
}
const saveFiles = () => {
  const list = savePatterns();
  if (!list.some(isSavePattern)) return list;
  const names = new Set(list.filter((p) => !isSavePattern(p)));
  const pats = list.filter(isSavePattern).map(savePatternRe);
  const FS = liveFS();
  let here = [];
  try { here = FS ? FS.readdir(GAME_ROOT) : []; } catch (e) { here = []; }
  for (const n of [...here, ...keptSaveNames()]) if (pats.some((r) => r.test(n.toUpperCase()))) names.add(n.toUpperCase());
  return [...names];
};
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
// A freeze RetroVega has reported about itself (retrovega/src/freeze.c): the report and what led to it, ready to copy.
let memorySaid = false;
function showFreeze() {
  const FS = liveFS();
  if (!FS || !$("freezeBox").hidden) return;
  let text;
  try { text = new TextDecoder().decode(FS.readFile(`${GAME_ROOT}/RVLOG.TXT`)); } catch (e) { return; }
  // (the game has gone without a buffer, with less of something for it: retrovega/src/mem.h)
  const short = /^MEMORY:.*$/m.exec(text);
  if (short && !memorySaid) { memorySaid = true; log(`RetroVega ${short[0]}`); }
  const at = text.indexOf("FREEZE:");
  // (twice: 80 s stuck, not just a very slow moment; the first report comes at 20 s, the next a minute later)
  if (at < 0 || text.indexOf("FREEZE:", at + 1) < 0) return;
  $("freezeReport").value = text.slice(0, at).split("\n").slice(-25).join("\n") + text.slice(at);
  $("freezeBox").hidden = false;
}

function describeSaves() {
  const files = saveFiles();
  $("saves").hidden = savePatterns().length === 0;
  if (!savePatterns().length) return;
  const kept = files.map(storedSave).filter(Boolean);
  saveStatus(kept.length
    ? `This browser keeps your save games (${savesByPattern() ? `${kept.length}, ` : ""}last change ${new Date(Math.max(...kept.map((k) => k.when))).toLocaleString()}).`
    : savesByPattern() ? "No save game of yours is kept here yet: save in the game and it will be."
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
    // (the game's bunk menu changed its difficulty: the menu here follows, and the room hears the host's)
    const d = difficultyInfo();
    if (d && rel.toUpperCase() === d.file.toUpperCase() && lobby.game) { renderMission(); if (isHost()) sayHello(); }
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
  if (!file || !savePatterns().length) return;
  // (a game whose saves are patterns takes any file of a matching name, as itself)
  const byName = savesByPattern() && savePatterns().filter(isSavePattern).some((p) => savePatternRe(p).test(file.name.toUpperCase()));
  if (savesByPattern() && !byName && !files.some((p) => baseName(p).toUpperCase() === file.name.toUpperCase())) {
    saveStatus(`${file.name} is not one of this game's saved games (${savePatterns().join(", ")}).`);
    return;
  }
  const rel = byName ? file.name.toUpperCase() : files.find((p) => baseName(p).toUpperCase() === file.name.toUpperCase()) || files[0];
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
// A game with no flight yet (RetroVega): no mission to pick, no flight options.
const lobbyFlies = () => { const g = gameById(lobbyGameId()); return !(g && g.noFlight); };
function missionLabel(m) {
  if (!m) return lobbyFlies() ? ((lobbyCampaign() || {}).noneLabel || "the campaign from the barracks") : (gameById(lobbyGameId()) || {}).title || "the game";
  const c = lobbyCampaign();
  // (a campaign that words its own missions: TIE Fighter's)
  if (c && c.label) return c.label(m);
  const s = c && c.series.find((e) => e.series === m.series);
  if (!s) return `ch ${m.series}, mission ${m.mis + 1}`;
  const note = Array.isArray(s.missions) ? s.missions[m.mis] : "";
  return (s.name ? `${s.name} ${m.mis + 1}` : `ch ${s.series}, mission ${m.mis + 1}`) + (note ? ` (${note})` : "");
}
// A campaign whose series come from a file beside the page (the registry's
// `campaign.manifest`: RetroVega's rv-campaign.json, made with the game's own
// data), fetched once; the menu is made again when it has come.
const manifests = new Map();
function loadCampaignManifest(g) {
  const c = g && g.campaign;
  if (!c || !c.manifest || manifests.has(g.id)) return;
  manifests.set(g.id, fetch(c.manifest).then((r) => {
    if (!r.ok) throw new Error(`${c.manifest}: HTTP ${r.status}`);
    return r.json();
  }).then((m) => {
    // a node a series, numbered from 1 as the menu numbers series; its missions' titles, and the numbers the
    // game's /brief takes (missionArgs)
    c.series.length = 0;
    for (const s of m.series) {
      const name = s.id.charAt(0) + s.id.slice(1).toLowerCase();
      c.series.push({ series: s.node + 1, name, missions: s.missions.map((x) => x.title), packed: s.missions.map((x) => x.n) });
    }
    $("mission").dataset.game = "";
    if (lobby.game) renderMission();
  }).catch((e) => { log(`the mission menu of ${g.title}: ${e.message || e}`); }));
}
// A room of one picks a mission only for a game that takes it on its own
// command line (the registry's campaign says `alone`); the others fly from
// their title screen there (the mission is the hooks', and they are not in).
const missionAlone = () => !!(lobbyCampaign() && lobbyCampaign().alone);
function buildMissionMenu() {
  const sel = $("mission");
  const c = lobbyCampaign();
  const one = soloRoom();
  const key = lobbyGameId() + (one ? ":alone" : "");
  if (sel.dataset.game === key && sel.options.length) return;
  sel.dataset.game = key;
  sel.innerHTML = "";
  const add = (value, text) => { const o = document.createElement("option"); o.value = value; o.textContent = text; sel.appendChild(o); };
  add("", (c && c.start && c.start[one ? "alone" : "room"]) || "Campaign from the barracks (the host's save game decides)");
  if (!c) return;
  let group = null;
  for (const s of c.series) {
    // (a campaign with its own words and groups: TIE Fighter's Combat Chamber and battles)
    if (c.label && s.files) {
      if (!group || group.label !== (s.title ? `${s.group}: ${s.title}` : s.group)) {
        group = document.createElement("optgroup");
        group.label = s.title ? `${s.group}: ${s.title}` : s.group;
        sel.appendChild(group);
      }
      for (let i = 0; i < s.files.length; i++) {
        const o = document.createElement("option");
        o.value = `${s.series}/${i}`;
        o.textContent = c.label({ series: s.series, mis: i }, "menu");
        group.appendChild(o);
      }
      continue;
    }
    const n = Array.isArray(s.missions) ? s.missions.length : s.missions;
    for (let i = 0; i < n; i++) {
      const note = Array.isArray(s.missions) ? s.missions[i] : "";
      // ("Ch" for the game's series, to keep the row short: the user's word.)
      add(`${s.series}/${i}`, s.name ? `${s.name} ${i + 1}  (ch ${s.series}, mission ${i + 1})`
                                     : `Ch ${s.series}, mission ${i + 1}` + (note ? `  (${note})` : ""));
    }
  }
}
// A campaign worded from the game's own files (the registry's `campaign.fromFiles`: TIE Fighter's craft, battles
// and missions in its own words), once, from the loaded copy of the game; the menu is made again after.
function wordCampaign() {
  const c = lobbyCampaign();
  if (!c || !c.fromFiles || c.worded || !source || source.game.id !== lobbyGameId() || !source.files.length) return;
  c.worded = true;
  try {
    c.fromFiles(source.files);
  } catch (e) {
    log(`the mission menu's words: ${e.message || e}`);
  }
  $("mission").dataset.game = "";
}
// A campaign the running game takes while it runs (the registry's `campaign.live`, an export of the emulator taking
// the mission's name: TIE Fighter flies many missions in one run, so the host may change it between flights).
let liveMissionSent = null;
function liveMission() {
  const c = lobbyCampaign();
  const M = running && window.DOSBox;
  if (!M) {
    liveMissionSent = null;     // (the next run is told at its launch: WCNET_TIE_MISSION)
    return;
  }
  if (!c || !c.live || !M["_" + c.live] || soloRoom()) return;
  const name = lobby.mission && c.file ? c.file(lobby.mission) : "";
  if (name === liveMissionSent) return;
  liveMissionSent = name;
  const bytes = Array.from(name, (ch) => ch.charCodeAt(0) & 0x7f);
  const p = M._malloc(bytes.length + 1);
  bytes.forEach((b, i) => { M.HEAPU8[p + i] = b; });
  M.HEAPU8[p + bytes.length] = 0;
  M["_" + c.live](p);
  M._free(p);
  log(`the room's mission given to the game: ${name || "none"}`);
}
function renderMission() {
  $("missionRow").hidden = !lobbyFlies();
  loadCampaignManifest(gameById(lobbyGameId()));
  wordCampaign();
  buildMissionMenu();
  liveMission();
  const sel = $("mission");
  const c = lobbyCampaign();
  // A mission the host's game does not have (the host changed games) is none.
  if (lobby.mission && isHost() && !Array.from(sel.options).some((o) => o.value === missionKey(lobby.mission))) lobby.mission = null;
  sel.value = missionKey(lobby.mission);
  sel.disabled = !isHost() || (running && !(c && c.live));
  // (A room of one flies the game as it is: its own campaign, collisions and
  // all, and a mission picked only where the game takes it itself.  The frame
  // rate stays: the hooks pace a flight alone too.)
  const one = soloRoom();
  const pickAlone = one && missionAlone();
  $("mission").parentElement.hidden = one && !pickAlone;
  $("mission").parentElement.title = one ? "The mission to start at, or the campaign as the game starts it" : "The mission the host picked for everyone";
  // (a game with none of the room's options, TIE Fighter's: no Conn, no Collide)
  const noRoomOptions = !!(gameById(lobbyGameId()) || {}).noRoomOptions;
  for (const id of ["latency", "collisions"]) $(id).parentElement.hidden = one || noRoomOptions;
  gameLook();
  // (a game with a difficulty, RetroVega: its Difficulty menu in Collide's place, asteroids on and wing bumps harmless,
  // in a room of one too: the game's bunk menu or this, either)
  const diff = (gameById(lobbyGameId()) || {}).difficulty;
  $("difficultyLabel").hidden = !diff || noRoomOptions;
  if (diff) {
    $("collisions").parentElement.hidden = true;
    lobby.rocks = "on";
    lobby.wingBumps = "harmless";
    if (isHost()) lobby.difficulty = difficultyNow();
    $("difficulty").value = lobby.difficulty;
    $("difficulty").disabled = !isHost();
  }
  // (a game whose asteroids are its own, each machine's: only the wing bumps are the room's to choose)
  const ownRocks = !!(gameById(lobbyGameId()) || {}).ownRocks;
  for (const o of $("collisions").options) o.hidden = ownRocks && !o.value.startsWith("on-");
  if (ownRocks) lobby.rocks = "on";
  $("collisions").value = `${lobby.rocks}-${lobby.wingBumps}`;
  $("collisions").disabled = !isHost();  // the host may switch them while flying too
  $("latency").value = lobby.latency;
  $("latency").disabled = !isHost();
  const fpsRange = lobbyFpsRange();
  $("fpsLabel").hidden = !fpsRange || !lobbyFlies();
  if (fpsRange) {
    const inp = $("fps");
    inp.min = String(fpsRange.min);
    inp.max = String(fpsRange.max);
    if (document.activeElement !== inp || !isHost() || running) inp.value = String(fpsInForce());
    inp.disabled = !isHost() || running;
  }
  $("capHullLabel").hidden = !capshipOption || !lobbyCapHull() || !lobbyFlies() || one;
  $("capHull").checked = lobby.capHull;
  $("capHull").disabled = !isHost() || running;
  const hints = (c && c.hints) || {};
  // (One line, cut with an ellipsis; the whole of it is the tooltip.)
  const hint = one && !pickAlone ? "The game from its title screen: its own campaign and saved games, its own wingman."
    : !c ? (lobbyGameId() ? "" : "Load the game files to pick a mission.")
    : lobby.mission ? `${missionLabel(lobby.mission)[0].toUpperCase()}${missionLabel(lobby.mission).slice(1)}. ${(one ? hints.aloneForced : hints.forced) || ""}`
    : one ? hints.alone || ""
    : (isHost() ? hints.host : hints.wing) || "";
  $("missionHint").textContent = hint;
  $("missionHint").title = hint;
}
// Everyone in a room runs the host's program.  A wingman's page switches
// to it when the loaded files have it (Wing Commander's directory holds
// The Secret Missions 2, Wing Commander II's the Special Operations) or a
// copy this browser saved does; otherwise the status line asks for the
// files (wrongGame).  The room's code only says what the room was made for,
// so the host may switch programs and the room stays together (the
// user's wish of 2026-10-17: pilots found it hard to change games).
function followHost() {
  if (isHost() || !source) return;
  const theirs = lobby.games.get(0);
  if (!theirs || theirs === source.game.id) return;
  const g = gameById(theirs);
  switchTo(holderOf(theirs), theirs);
  const who = esc(lobby.names.get(0) || "The host");
  if (source && source.game.id === theirs) chatLine(`${who} plays ${esc(g ? g.title : theirs)}: switched to it.`, "sys");
  else chatLine(`${who} plays ${esc(g ? g.title : theirs)}, which this browser has no files for: load them (step 1) to fly along.`, "sys");
}

// -- the room ------------------------------------------------------------------

const lobby = { game: null, names: new Map(), links: new Map(), flying: new Set(), unsubscribe: null, mission: null,
                // which game each player has loaded (a registry id), from their hello
                games: new Map(),
                // asteroid and mine fields, "on", "soft" (a rock does a
                // fraction of its damage) or "off": the host's choice, for everybody
                rocks: "on",
                // two wingmen's ships touching: "harmless" (the default) or
                // "game" (the game's collision damage): the host's choice
                wingBumps: "harmless",
                // a game's difficulty (its registry's `difficulty`: RetroVega's), "easy", "medium" or "hard": the
                // host's, as its own game has it (the settings file the game and the menu both write)
                difficulty: "medium",
                // the exchange mode, "low" (the default) or "high" latency, or
                // "auto" (the host's game measures the connection and picks,
                // which proved disruptive in play): the host's choice
                latency: "low",
                // frames a second in flight, for a game the hooks pace (in its
                // registry's `fps` range; 0: its default): the host's choice
                fps: 0,
                // capital ships with three times their hull: the host's choice
                capHull: false,
                // the host keeps the room out of "Join open game" (its Private box)
                hostPrivate: false,
                // game-protocol messages that arrive before DOSBox has adopted this
                // connection (a wingman faster to the launch than the host), handed
                // to the transport at adoption so nothing is lost
                backlog: [] };
let running = false;
let joystickPort = false;  // (the launch put the game port in: a pad was picked built in then)
let seeking = false;  // ("Join open game" is asking the lobby)
const PRIVATE_KEY = "wc:private";
const privateOn = () => $("private").checked;  // (the room is kept out of "Join open game")
const myName = () => $("callsign").value.trim() || given.name;
const isHost = () => lobby.game && lobby.game.selfId === 0;
// A room of one (Players 1): the page's own, with no lobby server and no
// seat for anybody else.  Its flight is the game alone, as it is (WCSOLO):
// its own wingman, menus and saved games; the hooks keep only the
// controller's part and the frame pacing.
const soloRoom = () => !!(lobby.game && lobby.game.solo);
function soloGame(code) {
  return { code, solo: true, selfId: 0, maxPlayers: 1, players: [{ id: 0, occupied: true }], iceServers: [],
           onEvent: () => () => {}, sendReliable: async () => {}, close: () => {} };
}
const ROOM_HINT = $("lobbyHint").textContent;
const SOLO_HINT = "A room of one, on this page alone: nobody else comes in. Leave it and join a room with Players at 2 or 3 to fly with company.";
// The seats of a room this page makes: 1, 2 (the default) or 3.
const seatCount = () => Math.min(3, Math.max(1, Math.round(Number($("seats").value)) || 2));
// A phone or a tablet makes a room of one to begin with (the user, 2026-11-03,
// after a host on a phone was dropped at random: "default the number of
// people per room in mobile to 1..that way mobile players can try the game
// but have to specifically try multiplayer to get it"): a device whose
// pointers are all coarse, a finger (a touch-screen laptop or a tablet with
// a trackpad has a fine one too, and keyboards).  Players 2 or 3 is a
// choice there; a room's link or "Join open game" still joins a room.
const phoneLike = () => { try { return matchMedia("(pointer: coarse)").matches && !matchMedia("(any-pointer: fine)").matches; } catch (e) { return false; } };
if (phoneLike()) $("seats").value = "1";

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
// (since: when the given callsign was given, for a pilot called the same: givesWay)
const sayHello = (to, reply) => sendLobby({ t: "hello", name: myName(), flying: running, reply: !!reply,
  since: $("callsign").value.trim() ? undefined : given.since,
  game: source ? source.game.id : "", voice: voice.choice(),
  mission: isHost() ? missionKey(lobby.mission) : undefined, rocks: isHost() ? lobby.rocks : undefined,
  wingBumps: isHost() ? lobby.wingBumps : undefined, difficulty: isHost() ? lobby.difficulty : undefined,
  latency: isHost() ? lobby.latency : undefined, fps: isHost() ? lobby.fps : undefined,
  capHull: isHost() ? lobby.capHull : undefined, private: isHost() ? privateOn() : undefined }, to);

// Another pilot (seat `seat` of the lobby or the room this page is seat
// `mine` of; `since` their given callsign's moment, none for a typed one) is
// called by this page's given callsign.  When this page is the one to give
// way it takes another, one that none of the pilots it knows of has, and
// says so to the room and the lobby.  True when it did.
function callsignClash(seat, since, mine) {
  if (!givesWay({ since: given.since, seat: mine }, { since, seat })) return false;
  const was = given.name;
  giveCallsign([was, ...lobby.names.values(), ...hall.names()]);
  log(`callsign: another pilot is ${was}: this one is ${given.name} now`);
  if (lobby.game) { sayHello(); renderRoster(); }
  hall.announce();
  return true;
}

// Asteroid and mine fields on, soft or off.  The host's page tells the others
// (for their lobby) and its own running game, which tells every wingman's game.
const ROCKS = { off: 0, on: 1, soft: 2 };  // the game's numbers (RocksMode)
const WING_BUMPS = { game: 0, harmless: 1 };  // the game's numbers (WingBumps)
const wingBumpsMode = (v) => (Object.prototype.hasOwnProperty.call(WING_BUMPS, v) ? v : "harmless");
const wingBumpsLabel = (v) => (v === "game" ? "wing bumps to hurt, as the game has it" : "wing bumps to harmless");
function setWingBumps(v) {
  if (!isHost()) return;
  if (difficultyInfo()) { chatLine("This game's wing bumps are always harmless.", "sys"); return; }
  v = wingBumpsMode(v);
  if (lobby.wingBumps === v) return;
  lobby.wingBumps = v;
  renderMission();
  chatLine(`You switched ${wingBumpsLabel(v)}`, "sys");
  sayHello();
  if (running && window.DOSBox && window.DOSBox._wc_web_set_wing_bumps) window.DOSBox._wc_web_set_wing_bumps(WING_BUMPS[v]);
}
const rocksMode = (v) => (Object.prototype.hasOwnProperty.call(ROCKS, v) ? v : "on");
const rocksLabel = (v) => (v === "soft" ? "asteroids soft (one will not kill you)" : `asteroids and mines ${v}`);
// A game's difficulty (RetroVega's: its registry's `difficulty`, a byte of its settings file): this browser's copy
// of the file as the running game has it, or as it is kept here; "medium" when there is none yet.
const DIFFICULTIES = ["easy", "medium", "hard"];
const difficultyInfo = () => { const g = gameInfo(); return (g && g.difficulty) || null; };
function difficultyBytes() {
  const d = difficultyInfo();
  if (!d) return null;
  const FS = liveFS(), kept = storedSave(d.file);
  const b = (FS && readSave(FS, d.file)) || (kept && kept.data);
  const ok = b && b.length > d.at && String.fromCharCode(...b.slice(0, d.magic.length)) === d.magic;
  return ok ? Uint8Array.from(b) : null;
}
function difficultyNow() {
  const d = difficultyInfo(), b = difficultyBytes();
  if (!d || !b) return lobby.difficulty || "medium";
  return DIFFICULTIES.find((k) => d.values[k] === b[d.at]) || "medium";
}
// The host (or a room of one) picks it: the byte in the game's file, the running game's and this browser's copy, which
// the game reads as its next flight begins.
function setDifficulty(v) {
  const d = difficultyInfo();
  if (!d || !DIFFICULTIES.includes(v)) return;
  if (!isHost()) { chatLine("Only the host sets the difficulty.", "sys"); renderMission(); return; }
  let b = difficultyBytes();
  if (!b) { b = Uint8Array.from(d.fresh); }
  if (b.length <= d.at) { const g = new Uint8Array(d.at + 1); g.set(b); b = g; }
  b[d.at] = d.values[v];
  const FS = liveFS();
  if (FS && writeSave(FS, d.file, b)) saveSeen.set(d.file, b.slice());
  storeSave(d.file, b);
  describeSaves();
  if (lobby.difficulty !== v) chatLine(`You set the difficulty ${v}${running ? " (from the next flight)" : ""}`, "sys");
  lobby.difficulty = v;
  renderMission();
  sayHello();
}

function setRocks(v) {
  if (!isHost()) { chatLine("Only the host can switch asteroids and mines.", "sys"); renderMission(); return; }
  if (difficultyInfo()) { chatLine("This game's asteroids are always on.", "sys"); return; }
  v = rocksMode(v);
  if (lobby.rocks === v) return;
  lobby.rocks = v;
  renderMission();
  chatLine(`You switched ${rocksLabel(v)}`, "sys");
  sayHello();
  if (running && window.DOSBox && window.DOSBox._wc_web_set_rocks) window.DOSBox._wc_web_set_rocks(ROCKS[v]);
}

// The exchange mode (src/cpu/wcnet_session.h, ExchangeMode): auto, low
// latency or high latency.  The host's page tells the others (for their
// lobby) and its own running game, whose frames carry the mode in force to
// every wingman's game.
const LATENCY = { auto: 0, low: 1, high: 2 };  // the game's numbers
const latencyMode = (v) => (Object.prototype.hasOwnProperty.call(LATENCY, v) ? v : "low");
const latencyLabel = (v) => (v === "auto" ? "the connection mode to auto (the host's game picks)" : `the connection to ${v} latency`);
function setLatency(v) {
  if (!isHost()) { chatLine("Only the host can set the connection mode.", "sys"); renderMission(); return; }
  v = latencyMode(v);
  if (lobby.latency === v) return;
  lobby.latency = v;
  renderMission();
  chatLine(`You set ${latencyLabel(v)}`, "sys");
  sayHello();
  if (running && window.DOSBox && window.DOSBox._wc_web_set_latency) window.DOSBox._wc_web_set_latency(LATENCY[v]);
}

// The frame rate in flight (WCFPS): Wing Commander's speed, since it moves
// everything a step a frame.  The host picks it before the launch; the
// host's game gives it to the wingmen's with each mission.  0: the game's
// default (its registry's `fps.def`); a number out of range is taken to the
// nearest end of it.
const lobbyFpsRange = () => { const g = gameById(lobbyGameId()); return (g && g.fps) || null; };
const fpsValue = (v) => {
  const r = lobbyFpsRange(); v = Math.round(Number(v));
  return r && v > 0 ? Math.min(r.max, Math.max(r.min, v)) : 0;
};
const fpsInForce = () => { const r = lobbyFpsRange(); return r ? (lobby.fps || r.def) : 0; };
function setFps(v) {
  if (!isHost()) { chatLine("Only the host can set the frame rate.", "sys"); renderMission(); return; }
  if (running) { chatLine("The frame rate is set before the launch.", "sys"); renderMission(); return; }
  v = fpsValue(v);
  if (!v || v === fpsInForce()) { renderMission(); return; }  // (shows what is in force)
  lobby.fps = v;
  renderMission();
  chatLine(`You set ${fpsInForce()} frames a second`, "sys");
  sayHello();
}

// Capital ships with three times their hull (WCCAPHULL=3 in every machine's
// game; the host's decides what is destroyed).  Set before the launch.
const CAP_HULL = 3;
const lobbyCapHull = () => { const g = gameById(lobbyGameId()); return !!(g && g.capHull); };
const capHullLabel = (on) => (on ? `capital ships ${CAP_HULL} times as tough (capship hull on)` : "capital ships as the game has them (capship hull off)");
function setCapHull(on) {
  if (!isHost()) { chatLine("Only the host can set the capital ships' hull.", "sys"); renderMission(); return; }
  if (running) { chatLine("The capital ships' hull is set before the launch.", "sys"); renderMission(); return; }
  on = !!on;
  if (lobby.capHull === on) return;
  lobby.capHull = on;
  renderMission();
  chatLine(`You set ${capHullLabel(on)}`, "sys");
  sayHello();
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
    if (p.id === 0) tags.push(g.solo ? "alone" : "host");
    if (me) tags.push("you");
    if (p.occupied && !me) tags.push(link === "up" ? "connected" : "connecting…");
    if (lobby.flying.has(p.id) || (me && running)) tags.push("flying");
    li.innerHTML = `<span class="dot ${p.occupied ? link : ""}"></span><span class="who">${esc(name)}</span><span class="tag">${tags.join(" · ")}</span>`;
    ul.appendChild(li);
  }
}

// The host's program is the room's.  A wingman's page follows it when its
// files can (followHost); when they cannot, this is said instead of flying.
// (The room's code is advisory: the game the room was made for.)
function wrongGame() {
  if (!lobby.game || isHost() || !source) return "";
  const theirs = gameById(lobby.games.get(0) || "");
  if (!theirs || theirs.id === source.game.id) return "";
  return `The host plays ${theirs.title}, and you have ${source.game.title} loaded: load ${theirs.title} above (step 1), or leave the room.`;
}
// A guest flies only once its host does (the user, 2026-11-05: "As a guest in a lobby--I believe if you fly first you
// kinda end up in single player mode...that shouldn't be allowed...guest should only be allowed to fly if host has
// already flied--guests can leave the lobby and start their own game if they want to fly alone"): its game would wait
// for a host that is not there, or fly on alone.  The host's hello says it flies (lobby.flying); a host that has said
// nothing GUEST_HELLO_MS after the guest came in (a native one: its game is the host, always in it) is not waited for.
const GUEST_HELLO_MS = 5000;
// The rooms this page has offered in the lobby (its Advertise, "/room": myRoom), for the page's life: Fly Alone is not offered
// in one of them (the user, 2026-11-05: "can the fly alone button not show up for rooms that the user clicked advertise
// in the lobby...we can store a small local hashtable in memory that recalls all room ids that have advertised")
const advertised = new Set();
const guestWaits = () => !!lobby.game && !soloRoom() && !isHost()
  && (lobby.names.has(0) ? !lobby.flying.has(0) : Date.now() - (lobby.joinedAt || 0) < GUEST_HELLO_MS);
// A host alone in its room may fly alone instead (the user: "a Fly Single Player button when they are in a empty
// lobby which doesn't post it to the channel and starts it as single player.  otherwise same as fly")
const aloneInRoom = () => !!lobby.game && !soloRoom() && isHost() && !lobby.game.players.some((p) => p.occupied && p.id !== lobby.game.selfId);
function updateActions() {
  const wrong = wrongGame();
  const ready = !!source && !!lobby.game && !running && !wrong;
  $("fly").disabled = !ready || guestWaits();
  // (in a room alone as its host, or in none yet, the lobby's alone: the user's "in a empty lobby")
  $("flySolo").hidden = !source || running || !!wrong || (lobby.game ? !aloneInRoom() : linkWaits() || $("join").disabled)
    || advertised.has(normalizeCode(lobby.game ? lobby.game.code : $("room").value.trim()));   // (offered in the lobby: not alone)
  $("advertise").hidden = !lobby.game || !lobbyReady() || !lobby.game.players.some((p) => !p.occupied);   // (in flight too)
  if (!running) {
    if (wrong) status(wrong);
    else if (!source && !lobby.game) status(linkWaits() ? `Load your game files (step 1) and you are in room ${$("room").value.trim()}; "Join room" takes a seat in it without them.` : "Load the game files and join a room first.");
    else if (!source) status(`Join succeeded; now load ${codeGame(lobby.game.code) ? codeGame(lobby.game.code).title : "the game files"} above.`);
    else if (!lobby.game) status("Game files ready; join a room to fly with friends.");
    else if (soloRoom()) { $("fly").textContent = "Fly"; status(`Room ${lobby.game.code} is yours alone: press Fly.`); }
    else if (isHost()) { $("fly").textContent = "Fly (starts everyone)"; status(`You host room ${lobby.game.code}.`); }
    else { $("fly").textContent = "Fly"; status(lobby.flying.has(0) ? "The host is already flying: press Fly to join the mission."
                                                : "Fly opens once the host flies. To fly alone, leave the room and make your own with Players at 1."); }
  }
  renderTopAction();
}

// The button at the top of the page, in view however the page is scrolled:
// Fly once there is a room to fly from (the same as the Fly button below),
// before that the step that comes next, and the full screen while the game
// runs.
function renderTopAction() {
  const b = $("topAction");
  const step = running ? "screen" : !source ? "files" : !lobby.game ? "room" : "fly";
  b.dataset.step = step;
  b.disabled = step === "fly" && $("fly").disabled;
  if (step === "room") b.disabled = seeking;
  b.textContent = { screen: "Full screen", files: "Load game files", room: "Join open game", fly: "Fly" }[step];
  b.title = { screen: "The game full screen; holding Esc leaves it",
              files: "Your own Wing Commander or Wing Commander II: a .zip of the game folder, or the GOG installer",
              room: "Joins a room with a free seat that a pilot in the lobby sits in, for a game this browser has (one at random). \"Join room\" in step 2 joins the code typed there, or makes that room",
              fly: $("status").textContent }[step];
}
$("topAction").addEventListener("click", () => {
  const step = $("topAction").dataset.step;
  if (step === "screen") goFullscreen();
  else if (step === "files") $("gamefile").click();
  else if (step === "room") { $("roomPanel").scrollIntoView({ block: "nearest", behavior: "smooth" }); void joinOpenGame(); }
  else $("fly").click();
});

// The doorbell: a pilot came into the room this page hosts and the host is not
// flying yet, so it is time to press Fly (the user, 2026-11-05: "would be
// nice to have a doorbell notif if someone shows up in a lobby you host, so
// you know to start the game").  A two-note chime made here (no file), and
// with the tab in the background its title says so until it is looked at.
// Only a new pilot rings, not one whose socket came back (player-rejoined).
let bellCtx = null, bellAt = -1e9, bellTitle = "";
function doorbell() {
  const now = performance.now();
  if (now - bellAt < 3000) return;
  bellAt = now;
  log("lobby: doorbell: a pilot came into your room");
  try {
    bellCtx = bellCtx || new (window.AudioContext || window.webkitAudioContext)();
    if (bellCtx.state === "suspended") void bellCtx.resume();
    const t0 = bellCtx.currentTime + 0.02;
    [[659.3, 0], [523.3, 0.42]].forEach(([f, at]) => {
      const out = bellCtx.createGain();
      out.connect(bellCtx.destination);
      out.gain.setValueAtTime(0.0001, t0 + at);
      out.gain.exponentialRampToValueAtTime(0.22, t0 + at + 0.01);
      out.gain.exponentialRampToValueAtTime(0.0001, t0 + at + 1.4);
      for (const [mult, level] of [[1, 1], [2.76, 0.28], [5.4, 0.1]]) {   // (a bell's partials)
        const o = bellCtx.createOscillator(), g = bellCtx.createGain();
        o.frequency.value = f * mult;
        g.gain.value = level;
        o.connect(g); g.connect(out);
        o.start(t0 + at); o.stop(t0 + at + 1.5);
      }
    });
    // (a context left running keeps an audio thread busy beside the game)
    const ctx = bellCtx;
    setTimeout(() => { if (performance.now() - bellAt >= 2500) void ctx.suspend(); }, 2600);
  } catch (e) { /* no sound here */ }
  if (document.hidden && !bellTitle) {
    bellTitle = document.title;
    document.title = "\u{1F514} A pilot joined your room";
  }
}
document.addEventListener("visibilitychange", () => {
  if (!document.hidden && bellTitle) { document.title = bellTitle; bellTitle = ""; }
});

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
        // Called as this page's given callsign: one of the two takes another.
        const was = myName();
        if (!$("callsign").value.trim() && typeof m.name === "string" && sameName(m.name, was) && callsignClash(ev.from, m.since, lobby.game.selfId)) {
          chatLine(`Another pilot here is ${esc(was)}: you are ${esc(myName())} now (the Callsign box takes one of your own).`, "sys");
        }
        if (typeof m.game === "string" && m.game !== (lobby.games.get(ev.from) || "")) {
          lobby.games.set(ev.from, m.game);
          if (ev.from === 0) { followHost(); renderMission(); }
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
        if (ev.from === 0 && !isHost() && typeof m.wingBumps === "string" && wingBumpsMode(m.wingBumps) !== lobby.wingBumps) {
          lobby.wingBumps = wingBumpsMode(m.wingBumps);
          chatLine(`${esc(m.name)} switched ${wingBumpsLabel(lobby.wingBumps)}`, "sys");
          renderMission();
        }
        if (ev.from === 0 && !isHost() && typeof m.difficulty === "string" && DIFFICULTIES.includes(m.difficulty)
            && m.difficulty !== lobby.difficulty) {
          lobby.difficulty = m.difficulty;
          chatLine(`${esc(m.name)} set the difficulty ${lobby.difficulty}`, "sys");
          renderMission();
        }
        if (ev.from === 0 && !isHost() && typeof m.latency === "string" && latencyMode(m.latency) !== lobby.latency) {
          lobby.latency = latencyMode(m.latency);
          chatLine(`${esc(m.name)} set ${latencyLabel(lobby.latency)}`, "sys");
          renderMission();
        }
        if (ev.from === 0 && !isHost() && typeof m.fps === "number" && fpsValue(m.fps) !== lobby.fps) {
          lobby.fps = fpsValue(m.fps);
          if (fpsInForce()) chatLine(`${esc(m.name)} set ${fpsInForce()} frames a second`, "sys");
          renderMission();
        }
        if (ev.from === 0) lobby.hostPrivate = m.private === true;
        if (ev.from === 0 && !isHost() && typeof m.capHull === "boolean" && m.capHull !== lobby.capHull) {
          lobby.capHull = m.capHull;
          if (lobbyCapHull()) chatLine(`${esc(m.name)} set ${capHullLabel(lobby.capHull)}`, "sys");
          renderMission();
        }
        voice.remote(ev.from, typeof m.voice === "string" ? m.voice : "off");
        if (!m.reply) sayHello(ev.from, true);
        renderRoster(); updateActions();
      } else if (m.t === "voice") {
        void voice.signal(ev.from, m.data);
      } else if (m.t === "chat") {
        chatLine(`<span class="name">${esc(m.name)}:</span> ${esc(m.text)}`);
      } else if (m.t === "start" && ev.from === 0 && !running) {
        if (typeof m.game === "string" && m.game) lobby.games.set(0, m.game);
        lobby.mission = parseMission(m.mission);
        if (typeof m.rocks === "string") lobby.rocks = rocksMode(m.rocks);
        if (typeof m.wingBumps === "string") lobby.wingBumps = wingBumpsMode(m.wingBumps);
        if (typeof m.latency === "string") lobby.latency = latencyMode(m.latency);
        if (typeof m.fps === "number") lobby.fps = fpsValue(m.fps);
        if (typeof m.capHull === "boolean") lobby.capHull = m.capHull;
        renderMission();
        chatLine(`${esc(lobby.names.get(0) || "The host")} started ${esc(missionLabel(lobby.mission))}`, "sys");
        lobby.flying.add(0); renderRoster();
        followHost();
        const theirs = gameById(lobby.games.get(0) || "");
        if (source && theirs && theirs.id !== source.game.id) status(`The host started ${theirs.title}, and you have ${source.game.title} loaded. Load ${theirs.title} above, then press Fly to join.`);
        else if (source) void start(false);
        else status("The host started the game. Load your game files above, then press Fly to join.");
      }
      break;
    }
    case "player-joined": case "player-rejoined": case "player-replaced":
      if (ev.type === "player-joined" && isHost() && !running && ev.playerId !== lobby.game.selfId) doorbell();
      lobby.links.set(ev.playerId, "down"); lobby.names.delete(ev.playerId); lobby.flying.delete(ev.playerId); lobby.games.delete(ev.playerId);
      voice.forget(ev.playerId);
      renderRoster(); updateActions(); break;   // (a host no longer alone: no Fly single player)
    case "player-left":
      if (ev.reason === "explicit-leave") {
        chatLine(`${esc(lobby.names.get(ev.playerId) || "Player " + (ev.playerId + 1))} left the room`, "sys");
        lobby.names.delete(ev.playerId); lobby.links.delete(ev.playerId); lobby.flying.delete(ev.playerId); lobby.games.delete(ev.playerId);
      }
      voice.forget(ev.playerId);
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

// The game a room's code names against what this browser holds.  The code
// is advisory (the host may switch programs in the room, and the pages
// follow the host's hello: followHost), but a browser with no files that
// run the code's game is not let in.  A click on SO1-4821 with Wing
// Commander II loaded switches to Special Operations 1 when it is there,
// or to a saved copy that has it; WC2-4821 with Wing Commander alone is
// refused.
function fitsRoom(code) {
  const theirs = codeGame(code);
  if (!theirs || !source || source.game.id === theirs.id) return true;
  const holder = holderOf(theirs.id);
  if (holder === "loaded") {
    setSource(source, theirs.id);
    chatLine(`Room ${esc(code)} is a ${esc(theirs.title)} room: switched to ${esc(theirs.title)}.`, "sys");
    return true;
  }
  if (holder) {
    switchTo(holder, theirs.id);
    chatLine(`Room ${esc(code)} is a ${esc(theirs.title)} room: switched to ${holder === "builtin" ? "it (built into this page)" : holder === "shelved" ? "the files dropped here" : "the saved copy that has it"}.`, "sys");
    return true;
  }
  status(`${code} is a ${theirs.title} room, and you have ${source.game.title} loaded. Load ${theirs.title} above (step 1) to join it.`);
  return false;
}

// create: false only joins a room that exists (a code clicked in the lobby).
async function joinRoom({ create = true } = {}) {
  if (lobby.game || $("join").disabled) return;
  // (A link's room that waits for the right game goes on waiting for it.)
  const viaLink = linkWaits();
  linked = false;
  const code = normalizeCode($("room").value.trim());
  $("room").value = code;
  if (!/^[A-Za-z0-9_-]{4,64}$/.test(code)) { status("Room codes are 4-64 letters, digits, - or _."); return; }
  if (hall.isLobby(code)) { status(`${code} is the public lobby (enter it above); a room to fly in needs a code of its own.`); return; }
  if (!fitsRoom(code)) { linked = viaLink; return; }
  $("join").disabled = true;
  const solo = create && seatCount() === 1;
  status(solo ? `Making room ${code} for one…` : `Joining room ${code}…`);
  try {
    const game = solo ? soloGame(code) : await P2PGame.connect({
      server: serverUrl(),
      code,
      // A room code gets said out loud: nobody who hears it may take a
      // seat somebody is sitting in.  A seat comes back only to the browser
      // tab that held it (its hidden resume token: reloading the page keeps
      // the seat); a tab that is gone leaves its seat taken until the room
      // dies, and the host makes a new room.  Tokenless "claims" would let
      // anyone with the code replace a player after 40 s of lobby silence,
      // which is every player in flight.
      // A room has two seats; Players 3 (beta) gives it a third, never
      // more: a third pilot gets a ship of the mission's wing, or one the
      // host's game makes (a second wingman, a second gunner), when they
      // come.  The count is 2 by default and not remembered.
      create: !create ? undefined
        : { maxPlayers: seatCount(), waitUntilFull: false, allowLateJoin: true,
            allowReconnect: true, allowReplacement: false, reconnectPolicy: "token-only" },
      storage: "session",
      storageKey: "wclobby-" + code,
      forceRelay: $("relay").checked,
    });
    lobby.game = game;
    lobby.joinedAt = Date.now();
    setTimeout(updateActions, GUEST_HELLO_MS + 100);   // (a host still silent then: a native one, not waited for)
    lobby.names.clear(); lobby.links.clear(); lobby.flying.clear(); lobby.games.clear();
    lobby.unsubscribe = game.onEvent(onLobbyEvent);
    // The address keeps no room: a link's code is for the join it brought,
    // not for every reload and bookmark after (rooms die; a pilot stuck in
    // a dead one's link could not get out).  "Copy link" makes the link.
    clearRoomFromAddress();
    $("lobby").hidden = false;
    $("leave").hidden = false;
    $("share").hidden = solo;
    $("chatForm").hidden = solo;
    $("lobbyHint").textContent = solo ? SOLO_HINT : ROOM_HINT;
    // (A room of one starts with the campaign as the game starts it, a room for more at the first mission.)
    if (game.selfId === 0) lobby.mission = parseMission(query.get("mission")) || (solo || (lobbyCampaign() || {}).noDefault ? null : { series: 1, mis: 0 });
    lobby.rocks = game.selfId === 0 ? rocksMode({ 0: "off", 2: "soft" }[query.get("rocks")] || query.get("rocks")) : "on";
    lobby.wingBumps = game.selfId === 0 ? wingBumpsMode({ 0: "game", 1: "harmless" }[query.get("bumps")] || query.get("bumps")) : "harmless";
    lobby.latency = game.selfId === 0 ? latencyMode(query.get("latency")) : "low";
    lobby.fps = game.selfId === 0 ? fpsValue(query.get("fps")) : 0;
    lobby.capHull = game.selfId === 0 && query.get("caphull") === "1";
    renderMission();
    for (const id of ["room", "seats", "server", "relay"]) $(id).disabled = true;
    if (solo) chatLine(`You are ${esc(myName())}, alone in room ${esc(code)}: Fly starts the game as it is, with its own wingman.`, "sys");
    else chatLine(`You are ${esc(myName())}, player ${game.selfId + 1} of ${game.maxPlayers} in room ${esc(code)}${game.selfId === 0 ? " (host)" : ""}. "Copy link" gives friends the way in.`, "sys");
    log(solo ? `lobby: room ${code} for one (no lobby server)` : `lobby: joined room ${code} as player ${game.selfId} of ${game.maxPlayers}`);
    renderRoster(); updateActions();
    $("chatInput").focus();
  } catch (e) {
    // (A link to a full or dead room is spent: the address loses the code.)
    clearRoomFromAddress();
    status(e && e.code === "room-not-found" ? `Room ${code} is not open any more: ask for another room in the lobby, or make one.`
      : e && e.code === "room-full" ? `Room ${code} is full: ask for another room in the lobby, or make one.`
      : `Could not join room ${code}: ${e && e.code ? e.code + ": " : ""}${e && e.message ? e.message : e}.${e && serverDown(e.code) ? " " + OTHER_SERVER : ""}`);
    $("join").disabled = false;
  }
}
function clearRoomFromAddress() {
  const url = new URL(location.href);
  for (const k of ["room", "callsign", "firstname", "lastname"]) url.searchParams.delete(k);
  history.replaceState(null, "", withServer(url));
}

// A room code clicked in the public lobby: the pilot wants to be in that
// room.  One they sit in alone (their own, made by "Join room" or by an
// advertisement) is left for it; one with company is not left behind by a
// click.  What came of it is said under the lobby's chat, where the click
// was, as well as in the status line.
async function joinFromLobby(code) {
  const tell = (text) => { status(text); hall.note(text); };
  if (running) { tell(`Reload the page to join ${code}: this page's game has been started.`); return; }
  if (lobby.game && lobby.game.code === code) { tell(`You are in room ${code} already (step 2).`); return; }
  let left = "";
  if (lobby.game) {
    const company = lobby.game.players.some((p) => p.occupied && p.id !== lobby.game.selfId);
    if (company) { tell(`You are in room ${lobby.game.code} with somebody: leave it (step 2) to join ${code}.`); return; }
    left = lobby.game.code;
    chatLine(`You left room ${esc(left)} for ${esc(code)}`, "sys");
    leaveRoom();
  }
  const typed = $("room").value;
  $("room").value = code;
  await joinRoom({ create: false });
  if (lobby.game) {
    tell(`You are in room ${code} (step 2): the host presses Fly when everyone is in.`);
    $("setup").scrollIntoView({ block: "nearest", behavior: "smooth" });
  } else {
    const why = $("status").textContent;
    $("room").value = typed; retagRoom();
    if (left) { await joinRoom(); if (lobby.game) status(why); }  // (back in the room of one's own)
    hall.note(why);  // (why not, said where the click was)
  }
}

// A room advertised in the public lobby: its code (which names the game),
// the mission and the free seats, "WC1-4821 Gimle 2, 1 seat free".  It is
// one of the pilot's lines there.
function offerText() {
  const g = lobby.game;
  const free = g.players.filter((p) => !p.occupied).length;
  const c = lobbyCampaign();
  const s = lobby.mission && c && c.series.find((e) => e.series === lobby.mission.series);
  const what = !lobbyFlies() ? "" : !lobby.mission ? (c && c.noneShort) || "campaign" : c && c.label ? c.label(lobby.mission, "short")
    : s && s.name ? `${s.name} ${lobby.mission.mis + 1}` : `ch ${lobby.mission.series} mission ${lobby.mission.mis + 1}`;
  const seats = free ? `${free} seat${free === 1 ? "" : "s"} free` : "full";
  // (The program, when the room's code was made for another: the code is advisory.)
  const prog = gameTag() && roomTag(g.code) !== gameTag() ? ` ${gameTag()}` : "";
  const full = `${g.code}${prog}${what ? " " + what : ""}, ${seats}`;
  return Array.from(full).length <= MAX_CHARS ? full : `${g.code} ${seats}`;
}
// The pilot's own room for the lobby (its "Advertise" and "/room"): the room
// the pilot is in, and otherwise the one of the room form, joined now; in
// flight, the room flown in (Fly advertises it while it has a seat free).
async function myRoom() {
  if (running) return lobby.game && !soloRoom() ? { code: lobby.game.code, offer: offerText() } : null;
  const alone = "A room of one (Players 1) is not offered in the lobby: nobody can join it.";
  if (!lobby.game && seatCount() === 1) { status(alone); return null; }
  if (!lobby.game) await joinRoom();
  if (soloRoom()) { status(alone); return null; }
  if (lobby.game) { advertised.add(normalizeCode(lobby.game.code)); updateActions(); }
  return lobby.game ? { code: lobby.game.code, offer: offerText() } : null;
}
$("advertise").addEventListener("click", () => { if (lobby.game) void hall.advertise(); });

// -- an open game ----------------------------------------------------------------

// "Join open game" (the top button while the pilot is in no room): the
// pilots in the lobby are asked for the room they sit in when it has a free
// seat (hall.findRooms), and one of those that this browser can fly in is
// joined, at random.  It never makes a room: with none, "No open game found"
// shows beside the button for a while ("Join room" in step 2 joins the code
// typed there, or makes that room).  A pilot who ticks Private is not asked
// about their room, nor is anyone in the room of a host who ticked it: only
// its code or its link brings somebody in.
// Private is not remembered between visits (the user, 2026-11-03: "users
// have become confused how others could not join using join open game when
// that button was accidentally ticked"); what an earlier page kept goes.
try { localStorage.removeItem(PRIVATE_KEY); } catch (e) { /* nothing kept */ }
$("private").addEventListener("change", () => sayHello());
// The room this page offers to a pilot looking for an open game, or null.
function openRoom() {
  const g = lobby.game;
  // (A room in flight is offered too while it has a free seat: whoever comes joins the mission in progress.)
  if (!g || privateOn() || lobby.hostPrivate) return null;
  const free = g.players.filter((p) => !p.occupied).length;
  return free > 0 ? { code: g.code, game: lobbyGameId(), free } : null;
}
// Can this browser fly in an offered room: the program it plays, and the
// game its code names (fitsRoom)?
function canFly(r) {
  const prog = gameById(r.game), tagged = codeGame(r.code);
  return !!prog && !!prog.multiplayer && !!holderOf(prog.id) && (!tagged || !!holderOf(tagged.id));
}
// A word beside the top button, fading after a while.
let topNoteTimer = null;
function topNote(text, why = "") {
  const n = $("topNote");
  n.textContent = text; n.title = why;
  n.classList.add("on");
  clearTimeout(topNoteTimer);
  topNoteTimer = setTimeout(() => n.classList.remove("on"), 5000);
}
async function joinOpenGame() {
  if (seeking || lobby.game || running || $("join").disabled) return;
  seeking = true;
  renderTopAction();
  try {
    status("Asking the pilots in the lobby for an open game…");
    const found = await hall.findRooms();
    const rooms = found ? found.rooms : [];
    const fit = rooms.filter(canFly);
    for (let i = fit.length - 1; i > 0; i--) { const j = Math.floor(Math.random() * (i + 1)); [fit[i], fit[j]] = [fit[j], fit[i]]; }
    const typed = $("room").value;
    for (const r of fit) {
      if (lobby.game || running) return;
      $("room").value = r.code;
      await joinRoom({ create: false });
      if (lobby.game) {
        // (Said under the lobby's chat as well: the status line is far below.)
        const text = `You are in room ${r.code}, an open game from the lobby.`;
        status(text); hall.note(text);
        return;
      }
    }
    if (lobby.game || running) return;
    $("room").value = typed; retagRoom();
    // (A pilot whose page is older than the question does not answer.)
    const silent = found ? found.asked - found.answered : 0;
    const text = !found ? "Not in the lobby: nobody to ask"
      : `No open game found${silent ? ` (${silent} pilot${silent === 1 ? "" : "s"} did not answer)` : ""}`;
    const why = !found ? "Join open game asks the pilots in the lobby (enter it above)."
      : fit.length ? "The open games were full by the time this page got there."
      : rooms.length ? "The open games in the lobby are for games this browser has no files for."
      : silent ? "A page older than Join open game does not answer: a reload brings the new one."
      : "Nobody in the lobby sits in a room with a free seat that is not private.";
    topNote(text, why);
    status(`${text}. ${why}`);
  } finally {
    seeking = false;
    renderTopAction();
  }
}

function leaveRoom() {
  voice.stopAll();
  if (lobby.unsubscribe) lobby.unsubscribe();
  if (lobby.game) { try { lobby.game.close(); } catch (e) { /* gone */ } }
  lobby.game = null; lobby.unsubscribe = null; lobby.adopted = false; lobby.backlog = []; lobby.hostPrivate = false;
  if (lobby.seatsBefore) { $("seats").value = lobby.seatsBefore; lobby.seatsBefore = null; }
  lobby.names.clear(); lobby.links.clear(); lobby.flying.clear(); lobby.games.clear();
  // (While the game runs, the button stays: the way out of it.)
  $("lobby").hidden = true; $("leave").hidden = !running; $("share").hidden = true; $("join").disabled = false;
  for (const id of ["room", "seats", "server", "relay"]) $(id).disabled = false;
  $("chatForm").hidden = false;
  $("roster").innerHTML = "";
  updateActions();
}

// "Leave room" always works.  A running game cannot be stopped, so leaving
// then reloads the page, once the pilot has said yes: the flight in progress
// is lost (the saved games are kept in this browser), and the others fly on
// without them.
let leavingForGood = false;  // (said yes already: the way out asks nothing more)
function leaveButton() {
  if (!running) { leaveRoom(); return; }
  const title = source && source.game ? source.game.title : "The game";
  if (!confirm(`${lobby.game ? `Leave room ${lobby.game.code}` : "Leave"}?\n\n${title} is running: leaving ends it here and reloads the page. ` +
               `The flight in progress is lost (saved games are kept in this browser)${lobby.game ? "; the others fly on without you" : ""}.`)) return;
  keepSaves();
  leavingForGood = true;
  leaveRoom();
  location.reload();
}
// A running game is work in progress: closing or reloading the tab asks
// first, as the browser does for a document not yet saved.
window.addEventListener("beforeunload", (ev) => { if (running && !leavingForGood) { ev.preventDefault(); ev.returnValue = ""; } });

$("setup").addEventListener("submit", (ev) => { ev.preventDefault(); void joinRoom(); });
// (Players: 1, 2 or 3, whatever is typed; three is in beta.)
$("seats").addEventListener("input", () => { $("seatsBeta").hidden = seatCount() !== 3; });
$("seats").addEventListener("change", () => { $("seats").value = seatCount(); $("seatsBeta").hidden = seatCount() !== 3; });
$("leave").addEventListener("click", leaveButton);
// The link to this room, to the clipboard (shown in the status line where
// the clipboard is out of reach).
$("share").addEventListener("click", async () => {
  if (!lobby.game) return;
  const link = roomLink(lobby.game.code);
  try { await navigator.clipboard.writeText(link); $("share").textContent = "Copied"; }
  catch (e) { status(`The link to room ${lobby.game.code}: ${link}`); $("share").textContent = "See below"; }
  setTimeout(() => { $("share").textContent = "Copy link"; }, 2500);
});
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
  const bumps = /^\/bumps(?:\s+(harmless|game|on|off))?$/i.exec(text);
  if (bumps) {
    if (bumps[1]) setWingBumps({ on: "game", off: "harmless" }[bumps[1].toLowerCase()] || bumps[1].toLowerCase());
    else chatLine(`${wingBumpsLabel(lobby.wingBumps)[0].toUpperCase()}${wingBumpsLabel(lobby.wingBumps).slice(1)}; the host switches it with /bumps harmless or /bumps game.`, "sys");
    return;
  }
  const lat = /^\/latency(?:\s+(auto|low|high))?$/i.exec(text);
  if (lat) {
    if (lat[1]) setLatency(lat[1].toLowerCase());
    else chatLine(`${linkState() || `The connection mode is set to ${lobby.latency}`}; the host sets it with /latency auto, /latency low or /latency high.`, "sys");
    return;
  }
  chatLine(`<span class="name">${esc(myName())}:</span> ${esc(text)}`);
  sendLobby({ t: "chat", name: myName(), text });
});
$("callsign").addEventListener("change", () => { if (lobby.game) { sayHello(); renderRoster(); } hall.announce(); });
$("collisions").addEventListener("change", () => {
  const [rocks, bumps] = $("collisions").value.split("-");
  setRocks(rocks);
  setWingBumps(bumps);
});
$("latency").addEventListener("change", () => setLatency($("latency").value));
$("difficulty").addEventListener("change", () => setDifficulty($("difficulty").value));
// A terse menu shows a word when closed and the whole of each choice
// (data-long) while open; its width stays the closed one's meanwhile.
for (const sel of document.querySelectorAll("select.terse")) {
  for (const o of sel.options) o.dataset.short = o.textContent;
  const open = () => {
    if (sel.dataset.open) return;
    sel.dataset.open = "1";
    if (sel.offsetWidth) sel.style.width = `${sel.offsetWidth}px`;
    for (const o of sel.options) o.textContent = o.dataset.long || o.dataset.short;
  };
  const close = () => {
    if (!sel.dataset.open) return;
    delete sel.dataset.open;
    for (const o of sel.options) o.textContent = o.dataset.short;
    sel.style.width = "";
  };
  sel.addEventListener("pointerdown", open);
  sel.addEventListener("keydown", open);
  sel.addEventListener("change", close);
  sel.addEventListener("blur", close);
}
// (A number input says "change" on Enter or leaving it; the box may still
// have the focus, which renderMission leaves alone.)
$("fps").addEventListener("change", () => { setFps($("fps").value); $("fps").value = String(fpsInForce()); });
$("capHull").addEventListener("change", () => setCapHull($("capHull").checked));
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
  if (!source || !lobby.game || running || guestWaits()) return;
  if (isHost()) sendLobby({ t: "start", game: source.game.id, mission: missionKey(lobby.mission), rocks: lobby.rocks, wingBumps: lobby.wingBumps, latency: lobby.latency, fps: lobby.fps, capHull: lobby.capHull });
  void start(true);
});
// Fly Alone: the empty room left for a room of one of the same code (soloGame: no lobby server, nobody can
// come in, no advertisement), the mission and the settings picked kept, and the flight started as Fly would
$("flySolo").addEventListener("click", async () => {
  if (!source || running || (lobby.game && !aloneInRoom())) return;
  const code = lobby.game ? lobby.game.code : normalizeCode($("room").value.trim()), seats = $("seats").value;
  const keep = lobby.game ? { mission: lobby.mission, rocks: lobby.rocks, wingBumps: lobby.wingBumps, latency: lobby.latency, fps: lobby.fps, capHull: lobby.capHull } : {};
  if (lobby.game) leaveRoom();
  $("room").value = code;
  $("seats").value = "1";
  await joinRoom();
  lobby.seatsBefore = seats;            // (the Players box as it was, back when this room of one is left)
  if (!soloRoom()) return;
  Object.assign(lobby, keep);
  renderMission();
  log(`lobby: ${code} flown single player: a room of one, nobody told`);
  void start(true);
});
$("fullscreen").addEventListener("click", () => goFullscreen());

// The picture's shape: the game's own (the registry's `screen`: TIE Fighter's 640 x 480, every row of it), for the game
// loaded or the room's, whichever is known (the user, 2026-11-06: "TIE Fighter is 640x480 so when not full screen I saw
// some horizontal rows missing... can we change canvas size when TF is selected or joined").
function gameLook() {
  const g = gameById(lobbyGameId()) || (source && gameById(source.game.id));
  const [w, h] = (g && g.screen) || [640, 400];
  const st = document.documentElement.style;
  st.setProperty("--screen-w", String(w));
  st.setProperty("--screen-h", String(h));
}
// Cinema: the picture as large as the window allows, without full screen (the user: "a cinema mode where non-fullscreen
// canvas gets scaled to fill the horizontal area of the browser window without actually going fullscreen"): the
// window's width, unless the window is too short for it (then its height decides, so the whole picture shows).
// Remembered in this browser.
const CINEMA_KEY = "wc-cinema";
function setCinema(on) {
  document.body.classList.toggle("cinema", on);
  $("cinema").checked = on;
  try { localStorage.setItem(CINEMA_KEY, on ? "1" : "0"); } catch (e) { /* this browser keeps nothing */ }
}
try { setCinema(localStorage.getItem(CINEMA_KEY) === "1"); } catch (e) { setCinema(false); }
$("cinema").addEventListener("change", () => {
  setCinema($("cinema").checked);
  if ($("cinema").checked) $("screen").scrollIntoView({ block: "start", behavior: "smooth" });
});
gameLook();
// Whether Fly goes full screen: each pilot's own, ticked unless unticked in
// this tab (the user, 2026-11-05: "The box should default to checked but
// should be remembered by the browser session"); the Full screen button works
// either way.
const FULL_START_KEY = "wc:fullStart";
try { if (sessionStorage.getItem(FULL_START_KEY) === "off") $("fullStart").checked = false; } catch (e) { /* ticked */ }
$("fullStart").addEventListener("change", () => { try { sessionStorage.setItem(FULL_START_KEY, $("fullStart").checked ? "on" : "off"); } catch (e) { /* not kept */ } });
// (#screen, the canvas and the touch controls: they come along into full screen)
const screenEl = $("screen");
$("fullStartLabel").hidden = !(screenEl.requestFullscreen || screenEl.webkitRequestFullscreen);   // (an iPhone has none)
function goFullscreen() {
  const req = screenEl.requestFullscreen || screenEl.webkitRequestFullscreen;
  if (!req) return;
  // Where the browser can (Chrome, Edge), ask for Esc in full screen: a tap
  // then goes to the game and only holding it leaves full screen.
  if (navigator.keyboard && navigator.keyboard.lock) navigator.keyboard.lock(["Escape"]).catch(() => { /* not allowed here */ });
  Promise.resolve(req.call(screenEl)).catch(() => { /* needs a click: the button stays */ });
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
  if (document.activeElement !== canvas && document.fullscreenElement !== screenEl) return;
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
// RetroVega's taunt hotkey, Alt+T, is a browser's own menu key outside full
// screen (Firefox's Tools): with the game in focus the browser is told to leave
// it alone, and the game still gets the key.  (Alt+B, the bookmarks, is left
// to the browser: the user's.)
window.addEventListener("keydown", (e) => {
  if (!running || !e.altKey || e.ctrlKey || e.metaKey || e.code !== "KeyT") return;
  if (document.activeElement !== canvas && document.fullscreenElement !== screenEl) return;
  e.preventDefault();
}, true);

async function start(fromGesture) {
  if (running) return;
  running = true;
  // A pad picked "built in" is the game port's joystick, which the game
  // looks for once, when it starts: the port goes into this launch's
  // configuration or into none (see below).
  joystickPort = controls.builtIn();
  emulatorStarted = true;
  // (The pilot stays in the public lobby while flying: flightOver.)  A room
  // with a seat free is advertised there as the flight begins (the user,
  // 2026-11-03: "If someone clicks play without full number of people in the
  // room can you auto-hit the advertise to the window. That way others can
  // join in at that time"), unless it is private: whoever comes joins the
  // mission in progress.
  if (!soloRoom() && lobby.game && lobby.game.players.some((p) => !p.occupied) && !privateOn() && !lobby.hostPrivate) void hall.advertise({ auto: true });
  // Voice on one side only: said here, so that nobody is surprised.
  const voiceWarning = voice.warning();
  if (voiceWarning) { chatLine(esc(voiceWarning), "sys"); log("voice: " + voiceWarning); }
  $("fly").disabled = true;
  renderTopAction();
  renderMission();  // (the mission and the frame rate are set for this flight)
  for (const el of ["gamefile", "exePicker", "callsign", "firstname", "lastname", "verbose", "hint"]) $(el).disabled = true;
  sourceButtons(false);
  try {
    const cfg = {
      code: lobby.game.code,
      callsign: $("callsign").value.trim(),
      lastname: $("lastname").value.trim(),
      firstname: gameInfo() && gameInfo().firstName ? $("firstname").value.trim() : "",
      players: String(lobby.game.maxPlayers || 2),   // (the game reads the room's own size; this agrees with it)
      server: serverUrl(),
      relay: $("relay").checked,
      verbose: $("verbose").checked,
    };
    status("Loading the emulator…");
    const { default: createDOSBox } = await import("./dosbox.js");
    // (WCNET_EXIT_STACK: should the game end by itself, its stack goes into the log.)
    const env = { WCNET_LOG: cfg.verbose ? "2" : "1", WCNET_EXIT_STACK: "1", SDL_EMSCRIPTEN_KEYBOARD_ELEMENT: "#canvas" };
    // (A room of one runs the game alone, as it is: soloRoom.)
    const solo = soloRoom();
    const together = source.game.multiplayer && !solo;
    if (together) {
      env.WCROOM = cfg.code; env.WCLOBBY = cfg.server; env.WCPLAYERS = cfg.players;
      if (cfg.relay) env.WCLOBBY_RELAY = "1";
    } else if (solo) {
      env.WCSOLO = "1";
      log(`${source.game.title} alone: the game as it is, the page's controller`);
    } else {
      log(`${source.game.title}: no multiplayer hooks for this game, running single-player`);
    }
    // The host's game decides and tells the wingmen's; theirs start the same.
    if (together) env.WCROCKS = String(ROCKS[lobby.rocks]);
    if (together) env.WCWINGBUMPS = String(WING_BUMPS[lobby.wingBumps]);
    if (together && isHost()) env.WCNET_MODE = lobby.latency;
    // (The wingmen's games take the host's with each mission; theirs agrees.)
    if (fpsInForce()) env.WCFPS = String(fpsInForce());
    if (together && lobbyCapHull() && lobby.capHull) env.WCCAPHULL = String(CAP_HULL);
    if (cfg.callsign && !solo) env.WCCALLSIGN = cfg.callsign;
    if (isTf()) env.WCNET_TIE_MOUSE = tfFlight();
    if (cfg.lastname && !solo) env.WCLASTNAME = cfg.lastname;
    if (cfg.firstname && !solo) env.WCFIRSTNAME = cfg.firstname;
    // A wingman's game calls the leader's ship by the leader's callsign (the
    // targeting computer, the comm menu): the roster has it.
    if (!isHost() && lobby.names.get(0)) env.WCHOSTCALLSIGN = lobby.names.get(0);
    if (!$("hint").checked) env.WCNET_NOHINT = "1";
    // The picked mission: in the hooks' environment, or on the game's own
    // command line (the registry says which).
    const campaign = gameInfo() && gameInfo().campaign;
    let missionArgs = "";
    if (lobby.mission && campaign && (together || (solo && campaign.alone && campaign.missionArgs))) {
      if (campaign.missionArgs) {
        missionArgs = " " + campaign.missionArgs(lobby.mission);
      } else if (campaign.env) {
        Object.assign(env, campaign.env(lobby.mission));
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
      lobbyGame: () => lobby.game,    // the transport adopts the connection the page is in when the game asks
      takeLobbyBacklog: () => { lobby.adopted = true; return lobby.backlog.splice(0); },
      onLobbyClosed: () => { chatLine("The game left the room", "sys"); leaveRoom(); },
      print: log,
      printErr: log,
      locateFile: (path) => path,
      onAbort: (what) => emulatorFailed(`abort: ${what}`),
      preRun: [() => { Object.assign(config.ENV, env); }],
      // The sprite accelerator asks for a bundle (src/hardware/rvgx.cpp, retrovega/docs/gx.md): it comes into the
      // emulator's memory whenever it comes, and the game draws its blocks until then.
      rvgxWant: (id, name) => {
        rvSets.fetchBundle(name).then((data) => {
          const at = config._malloc(data.length);
          config.HEAPU8.set(data, at);
          config._wc_web_rvgx_loaded(id, at, data.length);
          log(`sprite accelerator: ${name} in (${mb(data.length)})`);
        }).catch((e) => {
          log(`sprite accelerator: ${name}: ${e.message}`);
          config._wc_web_rvgx_loaded(id, 0, -1);
        });
      },
    };
    const Module = await createDOSBox(config);

    // A game built into the page comes from the site now.
    if (source.builtin && !source.files.length) {
      status(`Fetching ${source.game.title}…`);
      source.files = await readTarGz(gameInfo().builtin);
      log(`fetched ${source.files.length} files of ${source.game.title} (${mb(totalSize(source.files))}) from this site`);
    }
    status(`Installing ${source.game.title} (${source.files.length} files)…`);
    installFiles(Module.FS, GAME_ROOT, source.files);
    log(`installed ${source.files.length} files of ${source.game.title} under ${GAME_ROOT}`);
    // (a game's own settings made fit for the browser: the registry's `prepare`, TIE Fighter's music)
    if (gameInfo() && gameInfo().prepare) {
      try {
        const said = gameInfo().prepare(Module.FS, GAME_ROOT);
        if (said) log(said);
      } catch (e) {
        log(`preparing ${source.game.title}'s settings: ${e.message || e}`);
      }
    }
    // RetroVega's sprite sets (SHIPnn.RVH, retrovega/docs/engine.md "Flight")
    // are not installed here: a flight asks for the types it needs and the
    // page fetches their views and makes the sets then (rvSets.poll, below).
    // A program the page makes from the game (Special Operations 3): its
    // files come sealed from the site and open with the player's own copy.
    if (gameInfo() && gameInfo().addon === "so3") {
      const extra = await loadSo3(source.files, status);
      installFiles(Module.FS, GAME_ROOT, extra);
      log(`installed ${extra.length} files of the Culloden campaign`);
    }
    window.DOSBox = Module;
    restoreSaves(Module.FS);

    sayHello();
    renderRoster(); renderMission();
    status(together ? `Room ${cfg.code}: starting…` : `Starting ${source.game.title}…`);
    canvas.focus();
    // ?cmd=... runs another DOS command instead of the game (debugging aid).
    // RetroVega: ?res=1 the sharp sets; ?field=D its flights start in an
    // asteroid field of D rocks per (10,000 km)^3 (a stand-in until
    // the nav points carry their hazards).
    const isRv = gameInfo() && gameInfo().id === "rv";
    const field = isRv ? Math.max(0, Math.min(999, parseInt(query.get("field") || "0", 10) || 0)) : 0;
    const cmd = query.get("cmd") || source.game.run + missionArgs + (query.get("res") === "1" && isRv ? " /res 1" : "")
      + (field ? ` /field ${field}` : "");
    // The emulated CPU speed: the game's own good value (the registry), or
    // ?cycles=N; Ctrl+F11 / Ctrl+F12 still adjust it while playing.
    const cycles = Number(query.get("cycles")) || (gameInfo() && gameInfo().cycles) || 0;
    const args = ["-c", `mount c ${GAME_ROOT}`];
    // The game's disc, where it comes with one (the registry's image): drive D.
    if (gameInfo() && gameInfo().image) args.push("-c", `imgmount d ${GAME_ROOT}/${gameInfo().image} -t iso`);
    args.push("-c", "c:");
    if (cycles > 0) args.push("-c", `cycles=${Math.round(cycles)}`);
    // What the game's own setup expects of the machine (the registry), as a
    // configuration file: DOSBox's `config -set` rewrites the list of
    // startup commands it is itself run from, and the game's is lost.
    // The game port, for a built-in pad: counting (timed=false), since a
    // timed port's readings grow with the CPU's speed, which a flight
    // raises; the type stays "none", or DOSBox's own mapper would hold the
    // stick at rest every frame (the page puts it on the port, gamepad.js).
    // Any other launch's configuration is what it always was.
    const conf = ((gameInfo() && gameInfo().dosbox) || "") + (joystickPort ? "[joystick]\njoysticktype=none\ntimed=false\n" : "");
    if (conf) { Module.FS.writeFile("/game.conf", conf); args.unshift("-conf", "/game.conf"); }
    Module.callMain([...args, "-c", cmd]);
    if (gameInfo() && gameInfo().id === "rv") {
      rvSets.loadManifest().catch((e) => log(`rv/ships.json: ${e.message}`));
      setInterval(() => rvSets.poll(liveFS()), 300);
      setInterval(showFreeze, 5000);
    }
    setInterval(keepSaves, 4000);
    setInterval(showPerformance, 1000);
    for (const ev of ["pagehide", "visibilitychange"]) window.addEventListener(ev, keepSaves);
    $("fullscreen").hidden = !(screenEl.requestFullscreen || screenEl.webkitRequestFullscreen);   // (an iPhone has none)
    touch.show(true);
    if (fromGesture && $("fullStart").checked) goFullscreen();
    status(together
      ? `Room ${cfg.code}. "Copy link" (step 2) gives friends the way in.`
      : `${source.game.title} is running.`);
  } catch (e) {
    running = false;
    hall.reopen();
    log("failed: " + (e && e.stack ? e.stack : e));
    status("Failed to start: " + (e && e.message ? e.message : e));
    renderTopAction();
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
  const link = linkState();
  if (!alone && link) parts.push(link);
  const warn = speed < 0.93 ? "This computer cannot keep up with the emulated CPU: fewer cycles would run smoother (Ctrl+F11)."
             : load > 0.95 ? "The game needs more CPU than it is given: try ?env.WCFLIGHTCYCLES=20000 in the address."
             : (!alone && isHost() && wait > 10) ? "Another player's computer or connection is holding the game back." : "";
  el.innerHTML = esc(parts.join(" · ")) + (warn ? ` <span class="warn">${esc(warn)}</span>` : "");
}

// The running game's word on the connection (src/cpu/wcnet_hooks.cpp,
// wc_web_link): the exchange mode in force and the round trip, or "".
function linkState() {
  const M = window.DOSBox;
  if (!running || !M || !M._wc_web_link || !lobby.game || lobby.game.players.filter((p) => p.occupied).length < 2) return "";
  const mode = M._wc_web_link(0), setting = M._wc_web_link(1), rtt = M._wc_web_link(2);
  const name = mode === 2 ? "high-latency mode" : "low-latency mode";
  return `${name}${setting === 0 ? " (auto)" : ""}${rtt >= 0 ? `, round trip ${Math.round(rtt)} ms` : ""}`;
}

// -- controllers ---------------------------------------------------------------

// A game controller chosen in this window drives the running game's mouse and
// keyboard (web/gamepad.js); the registry says where the game's steering
// pointer rests and how far it reaches.
// Where a controller's stick rests the game's pointer and how far it moves
// it: the registry's figures (Wing Commander: a fixed point of the screen),
// or the running game's own (`fromGame`, Wing Commander II: the middle of
// the cockpit's window, another in every ship and every turret, and the
// distances at which its steps of turn begin; src/cpu/wcnet_hooks.cpp,
// steer_info).  The game's are in units of the mouse's range, 640 x 200.
function steeringPointer() {
  const p = (gameInfo() && gameInfo().pointer) || null;
  if (!p || !p.fromGame) return p;
  const M = running && window.DOSBox;
  if (!M || !M._wc_web_steer) return null;
  const [left, top, right, bottom] = [0, 1, 2, 3].map((i) => M._wc_web_steer(0, i));
  if (!(right > left && bottom > top && left >= 0 && top >= 0)) return null;
  const steps = (what, range) => { const s = []; for (let i = 0, v; (v = M._wc_web_steer(what, i)) >= 0; i++) s.push(v / range); return s; };
  return { x: (left + right) / 2 / 639, y: (top + bottom) / 2 / 199, rx: (right - left) / 2 / 639, ry: (bottom - top) / 2 / 199,
           stepsX: steps(1, 639), stepsY: steps(2, 199), edgeX: M._wc_web_steer(3, 0) / 639, edgeY: M._wc_web_steer(3, 1) / 199, top: M._wc_web_steer(3, 2) };
}

// The public lobby (web/hall.js): one for every game's pilots, WC-LOBBY (the
// user, 2026-11-03: "We have to have a unified lobby...splitting people in an
// almost abandoned service will not help it grow"; the roster tags each pilot
// with their game); ?hall=CODE is another one, for tests.
const lobbyName = query.get("hall") || undefined;
if (lobbyName) for (const el of document.querySelectorAll("code.lobbyName")) el.textContent = lobbyName + (el.dataset.n || "");
const hall = initHall({
  code: lobbyName,
  server: serverUrl,
  link: roomLink,
  name: () => $("callsign").value,
  given: () => given,
  clash: (seat, since, mine) => callsignClash(seat, since, mine),
  ready: lobbyReady,
  tag: () => gameTag(),
  title: (tag) => (gameByTag(tag) ? gameByTag(tag).title : tag),
  // (A room code is dimmed when nothing here runs its game: the loaded files or a saved copy.)
  fits: (tag) => !source || !gameByTag(tag) || !!holderOf(gameByTag(tag).id),
  onCode: (code) => void joinFromLobby(code),
  room: myRoom,
  openRoom,
  log,
});
window.__wcHall = hall;  // (for the page tests)

// The voice control, told of the room by the lobby code above.
const voice = initVoice({
  game: () => lobby.game,
  names: () => lobby.names,
  chatLine: (text, cls) => chatLine(esc(text), cls),
  log,
  announce: () => { if (lobby.game) sayHello(); },
  signal: (id, data) => sendLobby({ t: "voice", data }, id),
});
window.__wcVoiceLinks = voice.links;  // (for the page tests)
window.__wcVoiceReport = voice.report;

// The touch stick uses the same cockpit calibration as a controller, without sending a mouse click.
const touch = initTouch({ log, send: (name, down) => {
  const M = (running && window.DOSBox) || null;
  if (!M || !M._wc_web_key) return false;
  M._wc_web_key(KEY[name], down);
  return true;
}, inFlight: () => !!(running && window.DOSBox?._wc_web_in_flight?.()), steer: (x, y) => {
  const M = running && window.DOSBox, p = M && steeringPointer();
  if (!p || !M._wc_web_pointer) return false;
  const axis = (v, steps, reach, edge) => v === 0 ? 0 : steps ? stepped(v, steps, reach, edge, p.top) : v * reach;
  M._wc_web_pointer(p.x + axis(x, p.stepsX, p.rx, p.edgeX), p.y + axis(y, p.stepsY, p.ry, p.edgeY));
  return true;
} });

// TIE Fighter's flight controls (src/cpu/wcnet_tie_controls.cpp): the mouse points to aim, drags to steer (the
// game's own), or steers nothing (the keyboard alone, a joystick); the arrows always steer.  Remembered in this
// browser, given to the emulator at the launch (WCNET_TIE_MOUSE) and while flying (wc_web_tie_mouse).
const TF_FLIGHT = ["drag", "pointer", "keyboard", "joystick"];
const TF_FLIGHT_KEY = "wc-tf-flight";
const isTf = () => !!(gameInfo() && gameInfo().id === "tf");
function tfFlight() {
  let v = null;
  try { v = localStorage.getItem(TF_FLIGHT_KEY); } catch (e) { /* none */ }
  return TF_FLIGHT.includes(v) ? v : "pointer";
}
$("tfFlight").value = tfFlight();
$("tfFlight").addEventListener("change", (e) => {
  const v = TF_FLIGHT.includes(e.target.value) ? e.target.value : "pointer";
  try { localStorage.setItem(TF_FLIGHT_KEY, v); } catch (err) { /* this browser keeps nothing */ }
  const M = running && window.DOSBox;
  if (M && M._wc_web_tie_mouse) M._wc_web_tie_mouse(TF_FLIGHT.indexOf(v));
  log(`TIE Fighter's flight controls: ${e.target.selectedOptions[0].textContent}`);
});

// TIE Fighter's own meanings for a controller's buttons where Wing Commander's keys mean another thing there, or
// nothing (docs/tiefighter.md, step 7's open item; its flight keys, OpenTIE's user.c): Enter matches the target's
// speed, Tab is nothing (Backspace, full throttle, the nearest it has to an afterburner), N the missile boat's SLAM
// switch, A the target's attacker, G the goals, L the messages, C the flight recorder (the info rooms left out).
const TF_PAD = {
  missile: { keys: ["enter"], label: "Fire missile (TIE Fighter: match the target's speed; W picks the warheads)" },
  burner: { keys: ["backspace"], label: "Afterburner (TIE Fighter: full throttle)" },
  nav: { keys: [], label: "Navigation map (nothing in TIE Fighter: its map is an info room, not known to keep a shared flight going)" },
  auto: { keys: ["e"], label: "Autopilot (TIE Fighter: target your closest attacker)" },
  gunsel: { keys: ["x"], label: "Change guns (TIE Fighter: the lasers linked or not)" },
  lock: { keys: ["r"], label: "Lock target (TIE Fighter: the nearest enemy)" },
  comms: { keys: [], label: "Communications (nothing in TIE Fighter)" },
};
const controls = initControls({
  module: () => (running && window.DOSBox) || null,
  // (TIE Fighter: a controller steers through the emulator's stick, whatever the mouse does, rolls on the keypad's
  // Ins and Del as the emulator has it, and fires with the mouse's button, the game's own trigger)
  directStick: () => isTf(),
  rollKeys: () => (isTf() ? { left: "kp0", right: "kpperiod" } : null),
  actionKeys: (id) => (isTf() && TF_PAD[id] ? TF_PAD[id].keys : null),
  actionLabel: (id) => (isTf() && TF_PAD[id] ? TF_PAD[id].label : null),
  fireByMouse: () => isTf(),
  // (A controller button without a key of its own is the page's: push to talk.)
  onAction: (id, down) => { if (id === "ptt") voice.ptt(down); },
  pointer: steeringPointer,
  builtIn: () => !!(gameInfo() && gameInfo().joystick),
  joystickPort: () => (running ? joystickPort : null),
  log,
});

// -- go ------------------------------------------------------------------------

void initSources();
// A shared link (?room=CODE) puts a pilot with a game to fly straight into
// the room, as soon as the game files are there (setSource).  A page
// without them joins nothing by itself: whoever opens the link, or whatever
// does, has said nothing to the lobby server until a game is loaded or
// "Join room" is pressed.
function joinLinked() { if (linkWaits() && lobbyReady()) void joinRoom(); }
updateActions();
// The public lobby is where a pilot with a game to fly is unless it left:
// hall.auto says when not (?lobby=off stays out this once, for tests).  The
// page enters when the game files are there (setSource), and not before: a
// visitor who has none costs the lobby server nothing.
if (hall.auto({ link: !!query.get("room"), off: query.get("lobby") === "off" })) hall.enterWhenReady();
