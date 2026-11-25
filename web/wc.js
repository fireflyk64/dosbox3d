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
import { readZip, extractInstaller, looksLikeInstaller, identifyGame, installFiles, expandSplitZips, expandInnerZips, expandCdImages,
         saveGame, loadGames, forgetGame, totalSize, gameById, gameByTag, programsIn, knownBuild, legacyOf, legacyAdvice, GAMES } from "./gamefiles.js";
import { newRoomCode, normalizeCode, roomTag, GAME_TAGS, MAX_CHARS, CALLSIGNS, pickCallsign, givesWay, sameName } from "./chatfilter.js";
import { initHall, OTHER_SERVER, serverDown } from "./hall.js";
import { createSetProvider, readTarGz } from "./rvsets.js";
import { buildFields, compareBuild, RELOAD_HOW } from "./build.js";
import { zipStore, isZip } from "./savezip.js";

const $ = (id) => document.getElementById(id);
const logEl = $("log");
const canvas = $("canvas");

const DEFAULT_SERVER = "https://pqrstuvw.xyz/lobbylink";
const DATA_URL = "wc.tar.gz";
const GAME_ROOT = "/game";
const LOBBY_MAGIC = [0x57, 0x43, 0x4c, 0x01]; // "WCL" + version; never a valid protobuf start
const query = new URLSearchParams(location.search);
// RetroVega, the registry's built-in game, is in every page's "Game" menu and
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
// The game's one line of help, "To transmit comms use the '0' key", is for
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
// ready says nothing there (the "Game" menu shows it); data-state tells the
// page tests what the text no longer does: "busy", "ready" or "error".
function sourceStatus(text, state) { const el = $("sourceStatus"); el.textContent = text; el.removeAttribute("title"); if (state) el.dataset.state = state; }
// ... with the advice for a Wing Commander that is not the release the hooks know (gamefiles.js, LEGACY): which
// one to download instead, and its store page.
function sourceAdvice(text, state, which) {
  sourceStatus(text, state);
  const a = legacyAdvice(which);
  if (!a) return;
  const link = document.createElement("a");
  link.href = a.link; link.target = "_blank"; link.rel = "noopener"; link.textContent = a.linkText;
  $("sourceStatus").append(" ", a.text, " ", link, ".");
}
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
// A room's code is only its name: the game travels with the room, from its host.  The box starts empty (the user,
// 2026-11-17: "when the user really doesn't bother to type a room code it should generate the room code based on the
// game"): Host with it empty makes a code of the picked game's tag and four digits (WC1-4821; said in the public
// lobby it tells who can join).  `autoCode` is the code the page itself put in the box (a room it made up or found):
// taken out again when that room is left or could not be had, so that no stale code names the next room.
let autoCode = "";
const roomPlaceholder = () => { $("room").placeholder = `${gameTag() || "WC"}-auto`; };
// The game a code is for, when it says: the tag of a registry game.
const codeGame = (code) => (GAME_TAGS.includes(roomTag(code)) ? gameByTag(roomTag(code)) : null);

// -- game files ----------------------------------------------------------------

// The selected source: { label, game, root, files } with files as
// [{path, data}] relative to the game directory (drive C); a game built into
// the page (`builtin`) has no files until it is flown (start fetches them).
let source = null;
let saved = [];
// The files dropped on this page while a built-in game is picked: still in
// the "Game" menu, as the saved copies are.
let shelved = null;

// A game directory can hold more than one program of the registry (Wing
// Commander's has The Secret Missions 2, Wing Commander II's the two Special
// Operations).  Which one runs: the one the player picks in the menu
// (`want`), else the host's in a room, else the one the code of the room one is in
// (or of a link's room) names, else the one picked the last time, else the game itself.
const PROGRAM_KEY = (base) => `wc:program:${base}`;
function setSource(s, want) {
  // What the page knows about a game is the registry's, also for a copy this
  // browser saved when the registry said something else.
  if (s && gameById(s.game.id)) {
    const base = gameById(gameById(s.game.id).partOf || s.game.id);
    const programs = programsIn(s.files);
    if (!programs.includes(base)) programs.unshift(base);
    const roomCode = lobby.game ? lobby.game.code : (linkWaits() ? $("room").value.trim() : "");
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
    // Ready: the "Game" menu says which, and the line under it speaks only
    // of what runs single-player.  (Where the files came from and how many
    // is the menu's tooltip: the panel stays a few lines, and Fly in view.)
    // (a Wing Commander of another release: which one to get, said with it)
    sourceAdvice(!s.game.multiplayer ? `${s.game.title}: the multiplayer hooks do not know this program, so it runs single-player.`
                 : !s.known ? `${s.game.title}: its ${gameById(s.game.id).detect[0]} is not the build the multiplayer hooks know: it runs single-player, and the public lobby stays closed.` : "", "ready",
                 s.game.multiplayer && !s.known ? legacyOf("", s.files) : "");
    $("programLabel").title = (s.builtin ? `${s.game.title} is built into this page: its files come from the site when you fly.`
                               : `From ${s.label}: ${s.files.length} files, ${mb(totalSize(s.files))}.`)
                              + " The menu has every program of the games this browser holds; in a room everyone plays the host's.";
  }
  $("source").classList.toggle("loaded", !!s);
  // The help under the game is the loaded game's own; so are its keys, where it has a list (RetroVega's flight).
  const keys = s && gameInfo() && gameInfo().keys;
  $("gameKeys").innerHTML = keys || "";
  $("tfFlightLabel").hidden = !isTie();
  gameLook();
  $("gameKeys").hidden = !keys;
  const hints = s && gameInfo() && gameInfo().campaign && gameInfo().campaign.hints;
  $("helpMission").textContent = (hints && hints.help) || (gameInfo() && gameInfo().help)
    || (s ? "The multiplayer hooks do not know this program: it runs single-player." : "Load the game files (step 1): the help is the game's own.");
  describeSaves();
  // (the Players box: as many seats as the game's rooms take)
  $("seats").min = String(minSeats());
  $("seats").max = String(maxSeats());
  if (Number($("seats").value) < minSeats()) $("seats").value = String(minSeats());
  if (Number($("seats").value) > maxSeats()) $("seats").value = String(maxSeats());
  // (Wing Commander II's people use a first name; the first game has none: the box is there for every game, optional,
  // the user's call, 2026-11-15)
  roomPlaceholder();
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
// The game this pilot picked last (the menu, or files dropped), in this browser.  It was the one a visit started
// with (2026-11-17) until the user's 2026-11-19 ruling (a visit starts with Wing Commander or RetroVega: the page's
// start, below); still kept.  A room joined or a link's game is not a pick.
const PICK_KEY = "wc:game";
const rememberPick = (id) => { try { localStorage.setItem(PICK_KEY, id); } catch (e) { /* not kept */ } };
// ?game=: a game's id, tag or everyday name (the user, 2026-11-15: "if they pass in a game=tv or game=tf or
// game=raptor or game=rv flag it defaults to those games"): "tf" is TIE Fighter, the CD's when this browser has it,
// else the 1994 demo.
function gameParam(v) {
  const k = String(v || "").trim().toLowerCase();
  if (!k) return null;
  const named = { raptor: "rap", retrovega: "rv", tie: "tf", tfd: "tiedemo", tiefighter: "tf", xw: "xwing", "x-wing": "xwing", privateer: "priv", terminalvelocity: "tv" };
  let id = named[k] || k;
  if (!gameById(id)) { const byTag = GAMES.find((g) => g.tag.toLowerCase() === k); id = byTag ? byTag.id : null; }
  if (id === "tf" && !holderOf("tf")) id = "tiedemo";
  return id && gameById(id) ? id : null;
}
// The game a link asks for: ?game=tf, or the short way, the game's name as the flag, ?tf=1 ?tv=1 ?rv=1 ?wc1=1 (the
// user, 2026-11-17: "if you go in with ?tf=1 or ?tv=1 or ?rv=1 or ?wc1=1 it forces those but otherwise it's entirely
// dropdown driven").  A room's link names its own game.
function askedGame() {
  if (query.get("room")) return null;
  const named = gameParam(query.get("game"));
  if (named) return named;
  for (const [k, v] of query) if (k !== "game" && (v === "1" || v === "")) { const id = gameParam(k); if (id) return id; }
  return null;
}
// The "Game" menu, the one place a game is picked (the user, 2026-11-17: "it now gets really confusing to figure out
// which game you've selected"; "the idea of installing demos can be abstracted away"): every program of every game
// this browser holds (the loaded files and the saved copies), by game, the loaded one selected; then the free games it
// has not got yet (DEMOS: fetched when picked, or when a room asks), then the games that need the player's own files
// (greyed: the drop box under the menu).  Picking a program of another saved copy switches to that copy.
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
  const more = (label, list, off) => {
    if (!list.length) return;
    const grp = document.createElement("optgroup"); grp.label = label;
    for (const g of list) { const o = document.createElement("option"); o.value = g.id; o.textContent = g.title; o.disabled = !!off; grp.appendChild(o); }
    sel.appendChild(grp);
  };
  more("Free: fetched when picked", DEMOS.filter((d) => !seen.has(d.id) && gameById(d.id)).map((d) => gameById(d.id)));
  more("Your own files: drop them below", GAMES.filter((g) => g.multiplayer && !g.partOf && !g.builtin && !seen.has(g.id) && !demoOf(g.id)), true);
  if (source) sel.value = source.game.id;
  $("programLabel").hidden = !sel.options.length;
  // Every game by name beside the menu, so that nobody has to open it to learn what there is (the user, 2026-11-17:
  // "people may not realize the set of games avail"): a click picks one as the menu does, the one picked is lit, a
  // game that needs the pilot's own files is dim until they are here.
  const row = $("gameList");
  row.innerHTML = "";
  const mine = source ? (gameById(source.game.id) || {}).partOf || source.game.id : "";
  // (the games that are here first, then the free ones, then those that wait for files)
  const rank = (g) => (seen.has(g.id) ? 0 : demoOf(g.id) ? 1 : 2);
  for (const g of GAMES.filter((x) => x.multiplayer && !x.partOf).sort((x, y) => rank(x) - rank(y))) {
    const here = seen.has(g.id), free = !here && !!demoOf(g.id);
    const b = document.createElement("button");
    b.type = "button"; b.className = "secondary" + (g.id === mine ? " on" : ""); b.textContent = g.short || g.title;
    b.dataset.game = g.id;
    b.disabled = !here && !free;
    b.title = here ? `${g.title}: pick it` : free ? `${g.title}: free, fetched when picked and kept in this browser` : `${g.title} needs your own files: drop them in the box below`;
    b.addEventListener("click", () => {
      if (running || g.id === mine) return;
      // (the program of it picked the last time, when it is here)
      let kept = null;
      try { kept = localStorage.getItem(PROGRAM_KEY(g.id)); } catch (e) { /* none */ }
      sel.value = kept && [...sel.options].some((o) => o.value === kept && !o.disabled) ? kept : g.id;
      sel.dispatchEvent(new Event("change"));
    });
    row.appendChild(b);
  }
}
// The public lobby is for a pilot with a game to fly: the page connects to
// it only while one is loaded that the hooks know.
const lobbyReady = () => !!(source && source.known);
$("program").addEventListener("change", () => {
  if (running) return;
  const id = $("program").value;
  rememberPick(id);
  const holder = holderOf(id);
  // (a free game this browser has not got: fetched; the menu back as it was if that fails)
  if (!holder) { const d = demoOf(id); if (d) void getGame(d).then((ok) => { if (!ok) renderPrograms(); }); else renderPrograms(); return; }
  const g = gameById(id);
  if (g) { try { localStorage.setItem(PROGRAM_KEY(g.partOf || g.id), id); } catch (e) { /* not kept */ } }
  switchTo(holder, id);  // (a program of another game this browser holds, or the page's own)
});
// The loaded game's tag for a room code ("WC1"), or "" (none, or a program
// the registry does not know).
const gameTag = () => (gameInfo() && gameInfo().tag) || "";

async function useFiles(files, label, { persist = true } = {}) {
  // (a game in pieces, Terminal Velocity's shareware: its split archive unpacked)
  files = await expandSplitZips(files);
  files = expandCdImages(files);      // (a CD's image: the game's own files at its top put beside it)
  let id = identifyGame(files);
  if (!id.game) {
    // (no game at the archive's own level: a zip inside it that holds one, a backup wrapped once more)
    const more = await expandInnerZips(files);
    if (more !== files) { files = more; id = identifyGame(files); }
  }
  if (id.game && id.gathered && id.gathered.length) {
    log(`${id.game.title}: ${id.gathered.length} of its files found elsewhere in ${label} and put with the program: ${id.gathered.slice(0, 12).join(", ")}${id.gathered.length > 12 ? ", ..." : ""}`);
  }
  if (!id.game) {
    $("pickerLabel").hidden = false;
    const sel = $("exePicker");
    sel.innerHTML = "";
    for (const c of id.candidates) { const o = document.createElement("option"); o.value = c; o.textContent = c; sel.appendChild(o); }
    // (a Wing Commander by its name or its data files, but none of its programs: another release)
    const which = legacyOf(label, files);
    const said = () => sourceAdvice(id.candidates.length
      ? (which ? `${label}: not a release of ${which === "wc2" ? "Wing Commander II" : "Wing Commander"} this page knows (${files.length} files).`
               : `${label}: no game I recognise (${files.length} files). Pick the executable to run.`)
      : `${label}: no DOS executable found among ${files.length} files.`, "error", which);
    said();
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
    if (id.candidates.length) { sel.onchange(); if (which) said(); }   // (the advice stays in view over the picked program's line)
    return;
  }
  $("pickerLabel").hidden = true;
  setSource({ label, game: id.game, root: id.root, files: id.files });
  if (persist) {
    const ok = await saveGame({ id: id.game.id, title: id.game.title, run: id.game.run, multiplayer: id.game.multiplayer, label, files: id.files, when: Date.now() });
    if (ok) { await refreshSaved(); log(`kept ${id.game.title} in this browser for next time`); }
  }
}

// Is the file an ISO 9660 image (its volume descriptor at sector 16: 2048-byte sectors, or raw 2352-byte ones)?
async function isCdImage(file) {
  for (const at of [16 * 2048, 16 * 2352 + 16]) {
    if (file.size < at + 6) continue;
    const b = new Uint8Array(await file.slice(at, at + 6).arrayBuffer());
    if (b[0] === 1 && String.fromCharCode(...b.subarray(1, 6)) === "CD001") return true;
  }
  return false;
}

async function handleFile(file) {
  try {
    sourceButtons(false);
    sourceStatus(`Reading ${file.name} (${mb(file.size)})…`, "busy");
    // (what it is by its first bytes: a .zip is read whole, an installer where it is, a piece at a time)
    const head = new Uint8Array(await file.slice(0, 4).arrayBuffer());
    let files;
    if (head[0] === 0x50 && head[1] === 0x4b) {
      files = await readZip(new Uint8Array(await file.arrayBuffer()));
      log(`read ${files.length} files from ${file.name}`);
    } else if (looksLikeInstaller(head)) {
      sourceStatus(`Unpacking the installer ${file.name} in your browser… (innoextract)`);
      files = await extractInstaller(file, log);
    } else if (await isCdImage(file)) {
      // (a CD's image by itself: X-Wing's .iso.  Its top's files are the game's: expandCdImages)
      files = [{ path: file.name.replace(/[^A-Za-z0-9._-]/g, "_"), data: new Uint8Array(await file.arrayBuffer()) }];
      log(`${file.name}: a CD's image (${mb(file.size)})`);
    } else {
      throw new Error(`${file.name} is neither a .zip, a Windows installer nor a CD's image`);
    }
    await useFiles(files, file.name);
  } catch (e) {
    log("game files: " + (e && e.message ? e.message : e));
    const which = legacyOf(file.name);
    sourceAdvice(`Could not use ${file.name}: ${e && e.message ? e.message : e}.` +
                 (which ? "" : ` A .zip of the installed game folder always works; for a GOG installer you can also unpack it with innoextract and zip the result.`), "error", which);
  } finally {
    sourceButtons(true);
  }
}

// --- RetroVega's ship sets, fetched when a flight asks for them ---
//
// RetroVega's ship sets and asteroid rocks, fetched and made when the game asks (RVWANT.TXT: retrovega/web/rvsets.js)
const rvSets = createSetProvider({ base: "rv/", root: GAME_ROOT, status, log });

// The buttons that change the game files: off while a file is being read
// and once the game runs.
function sourceButtons(on) {
  for (const el of [$("useServer"), $("program"), ...$("savedList").querySelectorAll("button")]) el.disabled = !on;
  if (on) renderPrograms(); else for (const el of $("gameList").querySelectorAll("button")) el.disabled = true;
}
// The games anyone can play at once, the "Game" menu's free ones: RetroVega (built into the page), the shareware this
// site offers (web/demos: the archives exactly as Apogee and 3D Realms released them, their VENDOR.DOC letting anyone
// pass them around unchanged and free of charge), and the TIE Fighter demo from the Internet Archive (its CORS address:
// the Dodge Neon preview's two floppies, 1.7 MB).  A copy this browser kept is used as it is (nothing fetched again);
// else the archive is fetched and read as a dropped file is (and kept): when it is picked in the menu, when a room's
// link, a room found or a room's host plays it.
const DEMOS = [
  { id: "rv", title: "RetroVega", builtin: true },
  { id: "rap", file: "1rap12.zip", title: "Raptor's shareware" },
  { id: "tv", file: "3dtv12.zip", title: "Terminal Velocity's shareware" },
  { id: "tiedemo", file: "TIE_Fighter_Preview__Dodge_Neon.zip", title: "the TIE Fighter demo", from: "the Internet Archive",
    url: "https://archive.org/cors/StarWarsTIEFighter_1020/TIE_Fighter_Preview__Dodge_Neon.zip" },
];
const demoOf = (id) => DEMOS.find((d) => d.id === id) || null;
// The game picked wherever this browser has it, else fetched (one fetch at a time for each): true once it is the
// source.
const fetching = new Map();   // game id -> its fetch under way
async function getGame(d) {
  if (d.builtin) { useBuiltin(d.id); return true; }
  const holder = holderOf(d.id);
  if (holder) { switchTo(holder, d.id); return runs(source, d.id); }
  if (!fetching.has(d.id)) {
    fetching.set(d.id, (async () => {
      let file;
      try {
        sourceButtons(false);
        sourceStatus(`Fetching ${d.title} (${d.file}) from ${d.from || "this site"}…`, "busy");
        const r = await fetch(d.url || "demos/" + d.file);
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        file = new File([await r.blob()], d.file, { type: "application/zip" });
      } catch (e) {
        sourceStatus(`Could not fetch ${d.title}: ${e && e.message ? e.message : e}`, "error");
        sourceButtons(true);
        return;
      }
      await handleFile(file);
    })().finally(() => fetching.delete(d.id)));
  }
  await fetching.get(d.id);
  return runs(source, d.id);
}

// (Said with its date and its file: a game that is simply there on the
// next visit is the copy this browser kept, not one the site has.)
const useSavedCopy = (g, want) => setSource({ label: `the copy this browser saved${g.when ? " on " + new Date(g.when).toLocaleDateString() : ""} (${g.label})`, saved: true, savedId: g.id,
  game: { id: g.id, title: g.title, run: g.run, multiplayer: g.multiplayer }, root: "", files: g.files }, want);

// The copies this browser kept, side by side on one line: each with its
// size and a "forget" for one that is wrong or damaged.  (Playing one is
// the "Game" menu's business: it lists their programs.)
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
  // (files the pilot brings are a pick, as the menu's are)
  const brought = (f) => { if (f) void handleFile(f).then(() => { if (source) rememberPick(source.game.id); }); };
  // A file dragged over the page lights the box; dropped anywhere on the page (the box too) it is taken as the box
  // takes it -- a drop that missed the box would have the browser open the file in the page's place.  Not once the
  // game runs (the box's input is disabled then: the drop is only kept from the browser).
  const hasFiles = (e) => !!e.dataTransfer && Array.from(e.dataTransfer.types || []).includes("Files");
  let dragDepth = 0;
  const dragEnd = () => { dragDepth = 0; document.body.classList.remove("dragfile"); drop.classList.remove("over"); };
  window.addEventListener("dragenter", (e) => { if (hasFiles(e)) { dragDepth++; document.body.classList.add("dragfile"); } });
  window.addEventListener("dragleave", (e) => { if (hasFiles(e) && --dragDepth <= 0) dragEnd(); });
  window.addEventListener("dragover", (e) => { if (hasFiles(e)) e.preventDefault(); });
  window.addEventListener("drop", (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    dragEnd();
    if (!$("gamefile").disabled) brought(e.dataTransfer.files && e.dataTransfer.files[0]);
  });
  $("gamefileButton").addEventListener("click", () => $("gamefile").click());
  $("gamefile").addEventListener("change", (e) => { brought(e.target.files && e.target.files[0]); e.target.value = ""; });

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
  // (a game dropped or fetched while the saved copies and the server were asked is the player's pick: no default over
  // it.  On the site the server's question can take a second or more, and a zip dropped as the page opened was
  // replaced by the page's own game)
  if (source || fetching.size) return;
  // (A link to a room for one game picks that game among the saved copies.)
  // (Nothing saved and no server copy: the game built into the page, so
  // that everybody can fly, and meet in the lobby, with nothing to bring.)
  const wanted = codeGame(lobby.game ? lobby.game.code : $("room").value.trim());
  const fits = wanted && saved.find((g) => programsIn(g.files).includes(wanted));
  // (?game=: the page opened for a game, "rv", "tv", "raptor", "tf" (the CD's when this browser has it, else the demo):
  // that one, wherever this browser has it, a free one fetched; a room's link names its own)
  const asked = askedGame();
  if (asked && holderOf(asked)) { switchTo(holderOf(asked), asked); return; }
  if (asked && demoOf(asked)) { void getGame(demoOf(asked)); return; }
  // (then a room's link's game; then Wing Commander where this browser has it, else RetroVega, the page's own: the
  // user, 2026-11-19, of a visit that opened on TIE Fighter, the game picked the last time: "Make the normal dosbox
  // entry point default to wing commander if installed or retro Vega if not.  Only tf should default to tie fighter".
  // The game picked last is no longer a visit's first; any other game is a link's (?tf=1, the site's /tf) or the
  // menu's)
  const home = !wanted && ["wc1", "wc2"].find((id) => holderOf(id));
  if (wanted && builtins().includes(wanted)) useBuiltin(wanted.id);
  else if (fits) useSavedCopy(fits);
  else if (home) switchTo(holderOf(home), home);
  else if (serverCopy) $("useServer").click();
  else if (builtins().length) useBuiltin(builtins()[0].id);
  else if (saved.length) useSavedCopy(saved[0]);
  // (a link's game that needs the pilot's own files: said, with whatever is picked meanwhile)
  if (asked && source && source.game.id !== asked) sourceStatus(`${gameById(asked).title} needs your own files: drop them below. ${source.game.title} is picked meanwhile.`);
}

// -- save games ----------------------------------------------------------------

// A game's registry entry lists the files it keeps its saved games in (Wing
// Commander: one file holding all eight bunks).  This browser keeps a copy in
// local storage, keyed by game: it is put back into the game directory at
// every start and refreshed whenever the running game changes the file.
const gameInfo = () => (source && gameById(source.game.id)) || null;
const isArmada = () => source?.game.id === "armada" || source?.game.id === "armadapg";
// A room's seats at most: 3, or as many as the game's registry entry allows (`maxSeats`: Privateer's rooms of up to 8,
// each pilot flying its own career: docs/privateer.md).
const maxSeats = () => (gameInfo() && gameInfo().maxSeats) || 3;
const PRIV_CREWS = ["own", "crew"];
const minSeats = () => (gameInfo() && gameInfo().minSeats) || 1;
// (the terrain branch's playtest copy at /terrain/ keeps its own: its RetroVega's saved game is another version's,
// and the site's storage is shared with /dosbox)
const SAVE_PREFIX = /^\/terrain\//.test(location.pathname) ? "wcsave-terrain:" : "wcsave:";
const saveKey = (rel) => `${SAVE_PREFIX}${source.game.id}:${rel.toUpperCase()}`;
// A registry entry's saves may be patterns ("*.SAV": Privateer names its saved games after what the player types):
// then they are the files at the top of the game directory that match, and the copies of such files this browser
// keeps.  A game with names only (every other) gets its list as it is.
const savePatterns = () => { const g = gameInfo(); return (g && g.saves) || []; };
const isSavePattern = (rel) => rel.includes("*");
const savesByPattern = () => savePatterns().some(isSavePattern);
const savePatternRe = (pat) => new RegExp("^" + pat.toUpperCase().replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*") + "$");
function keptSaveNames() {
  const prefix = source ? `${SAVE_PREFIX}${source.game.id}:` : null;
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

// Every save game there is to download, newest first (the user, 2026-11-23: "I can only download 8 of my save games. If
// we can't get more then at least sort by date so I can get the newest ones"): the running game's file, or this
// browser's copy, or the game's own; dated by this browser's copy, or the file's own time.  At most 200.
function saveEntries() {
  const FS = liveFS();
  const out = [];
  for (const rel of saveFiles().slice(0, 200)) {
    const kept = storedSave(rel);
    const pristine = source.files.find((f) => f.path.toUpperCase() === rel.toUpperCase());
    const live = FS && readSave(FS, rel);
    const data = live || (kept && kept.data) || (pristine && pristine.data);
    if (!data) continue;
    let when = kept ? kept.when : 0;
    if (live) {
      try { const r = resolveSave(FS, rel); when = Math.max(when, FS.stat(r.path).mtime.getTime()); } catch (e) { /* undated */ }
    }
    out.push({ rel, name: baseName(rel), data, when });
  }
  return out.sort((a, b) => b.when - a.when || a.name.localeCompare(b.name));
}
function downloadBytes(data, name) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([data], { type: "application/octet-stream" }));
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
}
function listSaves() {
  const list = $("saveList");
  list.textContent = "";
  const entries = saveEntries();
  list.hidden = entries.length < 2;
  for (const e of entries) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "link";
    b.textContent = `${e.name}${e.when ? ` (${new Date(e.when).toLocaleString()})` : ""}`;
    b.title = `Download ${e.name} alone`;
    b.addEventListener("click", () => downloadBytes(e.data, e.name));
    const li = document.createElement("li");
    li.appendChild(b);
    list.appendChild(li);
  }
}

function describeSaves() {
  const files = saveFiles();
  $("saves").hidden = savePatterns().length === 0;
  if (!savePatterns().length) return;
  listSaves();
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
  const entries = saveEntries();
  if (!entries.length) { saveStatus("Nothing to download: this copy of the game has no save file yet."); return; }
  // (one file as itself; several as one zip, newest first: a page may start only so many downloads at once)
  if (entries.length === 1) { downloadBytes(entries[0].data, entries[0].name); return; }
  const day = new Date().toISOString().slice(0, 10);
  downloadBytes(zipStore(entries.map((e) => ({ name: e.name, data: e.data, when: e.when }))), `${source.game.id}-saves-${day}.zip`);
  saveStatus(`${entries.length} save games in one zip file (restore takes the zip, or any of its files).`);
});
$("saveFile").addEventListener("change", async (e) => {
  const picked = [...(e.target.files || [])].slice(0, 200);
  e.target.value = "";
  // (several files at once, and a zip of them: the download's own, or any; each restored as the one file it is)
  for (const file of picked) {
    const bytes = new Uint8Array(await file.arrayBuffer());
    if (!isZip(bytes)) { await restoreSave(file.name, bytes); continue; }
    let inner = [];
    try { inner = await readZip(bytes); } catch (err) { saveStatus(`${file.name}: ${err.message}`); continue; }
    for (const f of inner.slice(0, 200)) await restoreSave(baseName(f.path), f.data);
  }
});
async function restoreSave(fileName, data) {
  const file = { name: fileName };
  const files = saveFiles();
  if (!savePatterns().length) return;
  // (a game whose saves are patterns takes any file of a matching name, as itself)
  const byName = savesByPattern() && savePatterns().filter(isSavePattern).some((p) => savePatternRe(p).test(file.name.toUpperCase()));
  if (savesByPattern() && !byName && !files.some((p) => baseName(p).toUpperCase() === file.name.toUpperCase())) {
    saveStatus(`${file.name} is not one of this game's saved games (${savePatterns().join(", ")}).`);
    return;
  }
  const rel = byName ? file.name.toUpperCase() : files.find((p) => baseName(p).toUpperCase() === file.name.toUpperCase()) || files[0];
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
}

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
      const name = s.name || s.id.charAt(0) + s.id.slice(1).toLowerCase();
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
  // (a game the room does not fly, Privateer's: the row for its Face box alone -- and Full screen -- the room's own
  // choices hidden; the Face box was hidden with the whole row before: the user, 2026-11-09, "I also don't see an
  // option to select comm portrait"); Armada's for its High resolution and Invert mouse boxes)
  $("missionRow").hidden = !lobbyFlies() && !isPriv() && !isTvRoom() && !isArmada();
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
  $("mission").parentElement.hidden = (one && !pickAlone) || !lobbyFlies();
  $("mission").parentElement.title = one ? "The mission to start at, or the campaign as the game starts it" : "The mission the host picked for everyone";
  // (a game with none of the room's options, TIE Fighter's: no Conn, no Collide)
  const noRoomOptions = !!(gameById(lobbyGameId()) || {}).noRoomOptions;
  for (const id of ["latency", "collisions"]) $(id).parentElement.hidden = one || noRoomOptions;
  gameLook();
  // (a game with a difficulty, RetroVega: its Difficulty menu in Collide's place, asteroids on and wing bumps harmless,
  // in a room of one too: the game's bunk menu or this, either)
  const diff = (gameById(lobbyGameId()) || {}).difficulty;
  $("difficultyLabel").hidden = !diff || noRoomOptions;
  // (Privateer: Face, yours on the others' comm screens, in the Conn and Collide choices' place)
  $("privFaceLabel").hidden = !isPriv();
  $("privHiresLabel").hidden = $("privHiresCockpitLabel").hidden = !isPriv();
  $("privMouseLabel").hidden = !isPriv() && !isArmada();
  $("privMouseLabel").title = isArmada() ? ARM_MOUSE_TITLE : PRIV_MOUSE_TITLE;
  $("armHiresLabel").hidden = !isArmada();
  $("privFFLabel").hidden = !isPriv() || one || lobby.privCrew === "crew";     // (one ship: nobody to hit)
  $("privCrewLabel").hidden = !isPriv() || one;
  $("privCrew").value = lobby.privCrew;
  $("privCrew").disabled = !isHost() || running;
  $("privFF").disabled = !isHost();       // (the host's counts, as Collide's)
  // (Terminal Velocity: the craft each pilot flies as in the others' games, the host's choice)
  $("tvCraftLabel").hidden = $("tvCraftGuestLabel").hidden = !isTvRoom() || one;
  $("tvCraftHost").disabled = $("tvCraftGuest").disabled = !isHost() || running;
  tvCraftOffer();
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
const followFetched = new Set();   // (the free games fetched for the room's host: one try a room)
function followHost() {
  if (isHost() || !source) return;
  const theirs = lobby.games.get(0);
  if (!theirs || theirs === source.game.id) return;
  const g = gameById(theirs);
  const who = esc(lobby.names.get(0) || "The host");
  // (a free game this browser has not got: fetched, once for a room; setSource then takes the host's program)
  const d = !holderOf(theirs) && demoOf(theirs);
  if (d) {
    if (followFetched.has(theirs)) return;
    followFetched.add(theirs);
    chatLine(`${who} plays ${esc(g.title)}: fetching it…`, "sys");
    void getGame(d).then((ok) => {
      if (ok) chatLine(`Switched to ${esc(g.title)}.`, "sys");
      else chatLine(`${esc(g.title)} could not be fetched: pick it in the Game menu to try again, or leave the room.`, "sys");
      updateActions();
    });
    return;
  }
  switchTo(holderOf(theirs), theirs);
  if (source && source.game.id === theirs) chatLine(`${who} plays ${esc(g ? g.title : theirs)}: switched to it.`, "sys");
  else chatLine(`${who} plays ${esc(g ? g.title : theirs)}, which this browser has no files for: drop them in the Game panel to fly along.`, "sys");
}

// -- the room ------------------------------------------------------------------

const lobby = { game: null, names: new Map(), links: new Map(), flying: new Set(), unsubscribe: null, mission: null,
                // which game each player has loaded (a registry id), from their hello
                games: new Map(),
                // each player's page against this one's build (build.js compareBuild), from their hello
                builds: new Map(),
                // asteroid and mine fields, "on", "soft" (a rock does a
                // fraction of its damage) or "off": the host's choice, for everybody
                rocks: "on",
                // two wingmen's ships touching: "harmless" (the default) or
                // "game" (the game's collision damage): the host's choice
                wingBumps: "harmless",
                // a game's difficulty (its registry's `difficulty`: RetroVega's), "easy", "medium" or "hard": the
                // host's, as its own game has it (the settings file the game and the menu both write)
                difficulty: "medium",
                // Privateer's room: "own" (everybody flies their own ship) or "crew" (the host flies, the others man
                // its ship's turrets: docs/privateer.md section 30): the host's choice
                privCrew: "own",
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
// The seats of a room this page makes: 1, 2 (the default) or 3; a game's registry may allow more (maxSeats).
// (Privateer's one ship, crewed: the pilot and a gunner a turret, 3 at most)
const seatCount = () => Math.min(isPriv() && lobby.privCrew === "crew" ? 3 : maxSeats(), Math.max(minSeats(), Math.round(Number($("seats").value)) || 2));
// A phone or a tablet makes a room of one to begin with (the user, 2026-11-03,
// after a host on a phone was dropped at random: "default the number of
// people per room in mobile to 1..that way mobile players can try the game
// but have to specifically try multiplayer to get it"): a device whose
// pointers are all coarse, a finger (a touch-screen laptop or a tablet with
// a trackpad has a fine one too, and keyboards).  Players 2 or 3 is a
// choice there; a room's link or "Join open game" still joins a room.
const phoneLike = () => { try { return matchMedia("(pointer: coarse)").matches && !matchMedia("(any-pointer: fine)").matches; } catch (e) { return false; } };
if (phoneLike()) $("seats").value = "1";
// On a phone the page is a long column and the next thing to press was hard to find (the user, 2026-11-19: "there's a
// lot to scroll through. Can there be a "Fly Solo" button that starts a 1 player room and joins it and then scrolls
// down to the game part ... a flashing button for next steps ... and scrolling down when you click it to the next
// thing... eg host tie fighter scrolls down to the room which has the glowing fly game. and click that and it scrolls
// down to the controls").  So there: Fly Solo at the top; the next button to press pulses (renderTopAction, the
// page's styles' html.phone .next-step); a room joined scrolls to its Fly button, the game started to its picture
// and the touch controls under it.
const PHONE = phoneLike();
document.documentElement.classList.toggle("phone", PHONE);
let roomScroll = true;      // (false while Fly Solo makes its room: the one scroll is the game's)
function scrollToFly() { if (PHONE && roomScroll) $("fly").scrollIntoView({ block: "center", behavior: "smooth" }); }
// (the page grows as the game starts, the room's lines and the touch controls coming in: the picture is brought to the
// window's top again once they have, unless the pilot has scrolled meanwhile)
function scrollToGame() {
  if (!PHONE) return;
  let mine = -1;
  const go = () => {
    if (mine >= 0 && Math.abs(scrollY - mine) > 40) return;     // (scrolled by hand since: left there)
    $("screen").scrollIntoView({ block: "start", behavior: "auto" });
    mine = scrollY;
  };
  requestAnimationFrame(go);
  for (const ms of [300, 900, 2000]) setTimeout(go, ms);
}

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
  privCrew: isHost() && isPriv() ? lobby.privCrew : undefined,
  latency: isHost() ? lobby.latency : undefined, fps: isHost() ? lobby.fps : undefined,
  capHull: isHost() ? lobby.capHull : undefined, private: isHost() ? privateOn() : undefined, ...buildFields() }, to);

// Another page in the room is another version (build.js): a line once a seat and a verdict, nothing when the same.  A
// page from before builds were compared hears nothing of ours but the chat: it is asked there, that pilot alone.
function buildNote(seat, m, rel) {
  const who = esc(typeof m.name === "string" ? m.name : `Player ${seat + 1}`);
  if (rel === "newer") chatLine(`${who} has a newer version of this page: ${RELOAD_HOW}.`, "sys");
  else if (rel === "older" || rel === "none") chatLine(`${who}'s page is out of date${rel === "none" ? " (asked to reload)" : ""}.`, "sys");
  else if (rel === "site") chatLine(`${who} is on another page (${esc(m.site)}): fly from one page.`, "sys");
  if (rel === "none") sendLobby({ t: "chat", name: myName(), text: `(automatic) Your page is out of date: please ${RELOAD_HOW}.` }, seat);
}
// Why seat's game and this page's would not match (build.js), plain text, or ""
function buildApart(seat) {
  const rel = lobby.builds.get(seat), who = lobby.names.get(seat) || `Player ${seat + 1}`;
  return rel === "newer" ? `This page is out of date: ${RELOAD_HOW}.`
    : rel === "older" || rel === "none" ? `${who}'s page is out of date.`
    : rel === "site" ? `${who} is on another page.` : "";
}

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
  return `The host plays ${theirs.title}, and you have ${source.game.title} loaded: load ${theirs.title} (the Game panel), or leave the room.`;
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
// A game's series: Wing Commander and Wing Commander II with their mission disks, TIE Fighter and its demo; every other
// game is its own (Privateer too: another kind of game).  The room panel's "Join open ..." keeps to the series of the
// game picked (the user, 2026-11-17: "restrict to...that series at least (eg can join TFD or TF, can join WC1 or WC2)
// but not willy nilly"); the top button's takes any.
const SERIES = { wc1: "wc", wc2: "wc", tf: "tie", tiedemo: "tie" };     // (X-Wing is a series of its own)
const SERIES_NAMES = { wc: "Wing Commander", tie: "TIE Fighter" };
function seriesOf(id) {
  const g = gameById(id);
  if (!g) return id || "";
  const base = g.partOf || g.id;
  return SERIES[base] || base;
}
function seriesName(id) {
  const g = gameById(id), base = g && gameById(g.partOf || g.id);
  return SERIES_NAMES[seriesOf(id)] || (base ? base.short || base.title : "");
}
// The name on the Host button: the game picked, in a word or two.
const shortTitle = () => { const g = gameInfo(); return g ? g.short || g.title : source ? source.game.title : ""; };
// This page's flights would not match the room's (build.js): it is an older build than a pilot's here, or, a guest, its
// host's page is another build.  The reason, for the status line (and Fly held till it is gone), or ""
function buildWrong() {
  if (!lobby.game) return "";
  for (const [seat, rel] of lobby.builds) if (rel === "newer") return buildApart(seat);
  return isHost() ? "" : buildApart(0);
}
function updateActions() {
  const wrong = wrongGame() || buildWrong();
  $("join").textContent = source ? `Host ${shortTitle()}` : "Host a room";
  const one = seatCount() === 1;
  $("join").title = `Makes a room${source ? " of " + source.game.title : ""} and puts you in it as its host: under the code typed in the box, or one made for the game (${gameTag() || "WC"}-4821). ${one ? "With Players at 1 it is yours alone, said to nobody" : "It is offered in the lobby, for anybody to join, unless Private (below) is ticked: then only its code or its link brings somebody in"}`;
  $("findRoom").hidden = $("joinCode").hidden = !!lobby.game;
  const series = source ? seriesName(source.game.id) : "";
  $("findRoom").textContent = series ? `Join open ${series}` : "Join open game";
  $("findRoom").title = `Joins a room with a free seat that a pilot in the lobby sits in, of ${series ? "the " + series + " games" : "the game picked"} only (the game picked first): the Game menu then says which. The button at the top of the page takes a room of any game. It never makes a room: Host does`;
  const needsPeers = minSeats() > 1;
  const ready = !!source && !!lobby.game && !running && !wrong && !(needsPeers && soloRoom());
  $("fly").disabled = !ready || guestWaits();
  // (in a room alone as its host, or in none yet, the lobby's alone: the user's "in a empty lobby")
  $("flySolo").hidden = !source || needsPeers || running || !!wrong || (lobby.game ? !aloneInRoom() : linkWaits() || $("join").disabled)
    || (advertised.has(normalizeCode(lobby.game ? lobby.game.code : $("room").value.trim()))   // (offered in the lobby by hand: not alone;
        && !hostOffered.has(normalizeCode(lobby.game ? lobby.game.code : $("room").value.trim())));  // by Host itself: nobody may come)
  $("advertise").hidden = !lobby.game || !lobbyReady() || !lobby.game.players.some((p) => !p.occupied);   // (in flight too)
  if (!running) {
    if (wrong) status(wrong);
    else if (!source && !lobby.game) status(linkWaits() ? `Load your game files (step 1) and you are in room ${$("room").value.trim()}; "Join" takes a seat in it without them.` : "Pick a game (the Game menu), then host a room or join one.");
    else if (!source) status(`Join succeeded; now load ${codeGame(lobby.game.code) ? codeGame(lobby.game.code).title : "the game files"} above.`);
    else if (!lobby.game) status(`${source.game.title} is picked: host a room, join an open game, or join a friend's by its code.`);
    else if (soloRoom()) { $("fly").textContent = "Fly"; status(needsPeers ? `Leave this room and host a room for at least ${minSeats()} players.` : `Room ${lobby.game.code} is yours alone: press Fly.`); }
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
const FIND_TITLE = "Joins a room with a free seat that a pilot in the lobby sits in, of any game this browser has or can fetch: one of the game picked in the Game menu when there is one, then one of its series, then any other (the Game menu then says which). It never makes a room: Host does";
function renderTopAction() {
  const b = $("topAction");
  const step = running ? "screen" : !source ? "files" : !lobby.game ? "room" : "fly";
  b.dataset.step = step;
  b.disabled = step === "fly" && $("fly").disabled;
  if (step === "room") b.disabled = seeking;
  $("findRoom").disabled = $("joinCode").disabled = seeking || $("join").disabled;
  b.textContent = { screen: "Full screen", files: "Load game files", room: "Join open game", fly: "Fly" }[step];
  // (a phone: Fly Solo beside it until there is a room; the next button to press pulses, none once the game runs)
  const solo = $("topSolo");
  solo.hidden = !PHONE || running || !source || minSeats() > 1 || !!lobby.game;
  solo.disabled = seeking || $("join").disabled;
  const next = running ? null : !lobby.game ? (solo.hidden ? null : solo) : $("fly").disabled ? null : $("fly");
  solo.classList.toggle("next-step", next === solo);
  $("fly").classList.toggle("next-step", next === $("fly"));
  b.classList.toggle("next-step", next === $("fly") && step === "fly");
  b.title = { screen: "The game full screen; holding Esc leaves it",
              files: "Load a .zip of your game folder or a supported GOG installer",
              room: FIND_TITLE,
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
        const rel = compareBuild(m);
        if (lobby.builds.get(ev.from) !== rel) {
          lobby.builds.set(ev.from, rel);
          buildNote(ev.from, m, rel);
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
        // (a host's hello without the word -- another game's, an older page's -- is everyone their own)
        if (ev.from === 0 && !isHost() && (PRIV_CREWS.includes(m.privCrew) ? m.privCrew : "own") !== lobby.privCrew) {
          lobby.privCrew = PRIV_CREWS.includes(m.privCrew) ? m.privCrew : "own";
          // (said when the host's word changes; "their own" only after a "crewed": a guest's own last room as a host
          // may have been one, which is no news of this room)
          if (lobby.privCrew === "crew") chatLine(`${esc(m.name)} set the room to one ship, crewed: ${esc(m.name)} flies and you man a turret. Launch after ${esc(m.name)} does.`, "sys");
          else if (lobby.privCrewTold) chatLine(`${esc(m.name)} set the room to everyone their own ship.`, "sys");
          lobby.privCrewTold = lobby.privCrew === "crew";
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
        if (PRIV_CREWS.includes(m.privCrew)) lobby.privCrew = m.privCrew;
        renderMission();
        chatLine(`${esc(lobby.names.get(0) || "The host")} started ${esc(missionLabel(lobby.mission))}`, "sys");
        lobby.flying.add(0); renderRoster();
        followHost();
        const theirs = gameById(lobby.games.get(0) || ""), apart = buildApart(0);
        if (apart) status(apart);
        else if (source && theirs && theirs.id !== source.game.id) status(`The host started ${theirs.title}, and you have ${source.game.title} loaded. Load ${theirs.title} above, then press Fly to join.`);
        else if (source) void start(false);
        else status("The host started the game. Load your game files above, then press Fly to join.");
      }
      break;
    }
    case "player-joined": case "player-rejoined": case "player-replaced":
      if (ev.type === "player-joined" && isHost() && !running && ev.playerId !== lobby.game.selfId) doorbell();
      lobby.links.set(ev.playerId, "down"); lobby.names.delete(ev.playerId); lobby.flying.delete(ev.playerId); lobby.games.delete(ev.playerId); lobby.builds.delete(ev.playerId);
      voice.forget(ev.playerId);
      renderRoster(); updateActions(); break;   // (a host no longer alone: no Fly single player)
    case "player-left":
      if (ev.reason === "explicit-leave") {
        chatLine(`${esc(lobby.names.get(ev.playerId) || "Player " + (ev.playerId + 1))} left the room`, "sys");
        lobby.names.delete(ev.playerId); lobby.links.delete(ev.playerId); lobby.flying.delete(ev.playerId); lobby.games.delete(ev.playerId); lobby.builds.delete(ev.playerId);
      }
      voice.forget(ev.playerId);
      renderRoster(); updateActions(); break;

    case "peer-state":
      lobby.links.set(ev.playerId, ev.state === "connected" ? "up" : (ev.state === "failed" || ev.state === "closed") ? "down" : (lobby.links.get(ev.playerId) || "down"));
      if (ev.state === "connected") sayHello(ev.playerId, false);
      renderRoster(); break;
    case "signaling-closed":
      // (the connection gone, not the room ended: made again unless the game runs with it: resumeRoom)
      if (ev.code === "connection-lost" && !running) { chatLine("Lost the lobby server's connection: making it again…", "sys"); void resumeRoom(); }
      else chatLine(`Lost the lobby server (${esc(ev.code)}): nobody new can join, existing links stay up`, "sys");
      break;
    case "lobby-error":
      if (ev.code === "invalid-message" && /ping/.test(ev.message || "")) break;      // (the server's answer to roomBeat's line)
      chatLine(`Lobby error ${esc(ev.code)}: ${esc(ev.message)}`, "sys"); break;
  }
}

// A room's connection to the lobby server, kept and made again (the user, 2026-11-19: "I just hosted a game on my
// browser--I waited like 20 minutes and tried to join on another browser... but it said the game was not available. Is
// there some kind of hidden timeout in the system? I had a stable connection the whole time").  There were two.  A
// room's connection says nothing while nobody comes or goes, and what stands between the page and the server (its web
// server's proxy, a router) closes a connection silent for some minutes; the page then said so in the room's chat and
// did nothing more.  And the server ends a room five minutes after its last connection went.  So a host waiting alone
// had no room some ten minutes on.
//   * Every 25 s a room's page says a line to the server (roomBeat; the lobby chat's page has done so from the start,
//     hall.js).  The server has no such line and answers that it does not know it (onLobbyEvent leaves the answer
//     out).  The beat comes from a worker's clock: a page in a hidden tab has its own timers slowed to one a minute.
//   * A connection lost all the same (the network gone a while, the computer asleep) is made again while no game runs
//     with it (resumeRoom): the same seat by the tab's token, the room made anew by its host if the server has ended
//     it, tried again every 20 s.  With a game running the links between the players stay up as before and nobody new
//     can join: the emulator holds that connection.
const ROOM_BEAT_MS = 25000;
function roomBeat() {
  const g = lobby.game;
  try { if (g && !g.solo && g.ws && g.ws.readyState === WebSocket.OPEN) g.ws.send('{"type":"ping"}'); } catch (e) { /* its close is told */ }
}
try {
  const clock = new Worker(URL.createObjectURL(new Blob([`setInterval(() => postMessage(0), ${ROOM_BEAT_MS});`], { type: "text/javascript" })));
  clock.onmessage = roomBeat;
} catch (e) { setInterval(roomBeat, ROOM_BEAT_MS); }
let resuming = false;
async function resumeRoom() {
  const old = lobby.game;
  if (!old || old.solo || resuming || running) return;
  resuming = true;
  const code = old.code, key = "wclobby-" + code;
  try {
    const game = await P2PGame.connect({
      server: serverUrl(), code,
      create: old.selfId !== 0 ? undefined
        : { maxPlayers: old.maxPlayers, waitUntilFull: false, allowLateJoin: true, allowReconnect: true, allowReplacement: false, reconnectPolicy: "token-only" },
      storage: "session", storageKey: key, forceRelay: $("relay").checked,
    });
    if (lobby.game !== old || running) { try { game.close(); } catch (e) { /* gone */ } return; }     // (left, or flying, meanwhile)
    if (lobby.unsubscribe) lobby.unsubscribe();
    try { old.close(); } catch (e) { /* gone */ }
    try { sessionStorage.setItem(key, game.resumeToken); } catch (e) { /* (the old one's close forgot the seat's token: the new one's) */ }
    lobby.game = game;
    lobby.adopted = false; lobby.backlog = [];
    lobby.links.clear();
    lobby.unsubscribe = game.onEvent(onLobbyEvent);
    chatLine(`The lobby server's connection is made again: room ${esc(code)} is open${game.selfId !== old.selfId ? `, and you are player ${game.selfId + 1} of it now` : ""}.`, "sys");
    log(`lobby: room ${code}: the connection made again as player ${game.selfId} of ${game.maxPlayers}`);
    renderRoster(); updateActions();
    // (a room Host offered in the lobby is offered again: the server may have ended the one that was said)
    if (isHost() && hostOffered.has(normalizeCode(code)) && !privateOn() && game.players.some((p) => !p.occupied)) void hall.advertise({ auto: true });
  } catch (e) {
    if (lobby.game === old && !running && e && (e.code === "room-not-found" || e.code === "room-full")) {
      chatLine(`Room ${esc(code)} is not open any more (${esc(e.code)}): leave it, and host or join another.`, "sys");
    } else if (lobby.game === old && !running) {
      chatLine(`The lobby server's connection could not be made again (${esc((e && e.code) || (e && e.message) || "no answer")}): trying again in 20 s.`, "sys");
      setTimeout(() => { if (lobby.game === old) void resumeRoom(); }, 20000);
    }
  } finally {
    resuming = false;
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
  const d = demoOf(theirs.id);
  if (d) {
    // (a free game: fetched now; a room's link joins once it is there, setSource's joinLinked)
    status(`${code} is a ${theirs.title} room: fetching ${d.title} first…`);
    void getGame(d);
    return false;
  }
  status(`${code} is a ${theirs.title} room, and you have ${source.game.title} loaded. Drop its files in the Game panel to join it.`);
  return false;
}

// create: false only joins a room that exists (Join with a code typed, a code clicked in the lobby, a room found);
// otherwise the room is made when it is not there (Host, a link).  `game`: the program the room is known to play (a
// room found in the lobby): switched to, whatever the code says.
async function joinRoom({ create = true, game = null } = {}) {
  if (lobby.game || $("join").disabled) return;
  // (A link's room that waits for the right game goes on waiting for it.)
  const viaLink = linkWaits();
  linked = false;
  let code = normalizeCode($("room").value.trim());
  // (no code typed: a room made here gets one of the picked game's)
  if (!code && create) { code = newRoomCode(gameTag()); autoCode = code; }
  if (!code) { status("Type the code of the room to join (a friend's, or one said in the lobby). Host makes a room of your own."); $("room").focus(); return; }
  $("room").value = code;
  if (!/^[A-Za-z0-9_-]{4,64}$/.test(code)) { status("Room codes are 4-64 letters, digits, - or _."); return; }
  if (hall.isLobby(code)) { status(`${code} is the public lobby (enter it above); a room to fly in needs a code of its own.`); return; }
  // (The code's prefix is a hint of the game for whoever joins by it, a link's too.  A room made here is of the game
  // picked, whatever its code says: a code left in the box switched a host back to another game.)
  if (game) { if (source && source.game.id !== game && holderOf(game)) switchTo(holderOf(game), game); }
  else if ((viaLink || !create) && !fitsRoom(code)) { linked = viaLink; return; }
  $("join").disabled = true;
  renderTopAction();
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
    else chatLine(`You are ${esc(myName())}, player ${game.selfId + 1} of ${game.maxPlayers} in room ${esc(code)}${game.selfId === 0 ? " (host)" : ""}. "Copy link" to play with friends.`, "sys");
    log(solo ? `lobby: room ${code} for one (no lobby server)` : `lobby: joined room ${code} as player ${game.selfId} of ${game.maxPlayers}`);
    renderRoster(); updateActions();
    if (PHONE) scrollToFly();           // (and no focus in the chat's box: it would raise the phone's keyboard)
    else $("chatInput").focus();
  } catch (e) {
    // (A link to a full or dead room is spent: the address loses the code.)
    clearRoomFromAddress();
    status(e && e.code === "room-not-found" ? `Room ${code} is not open${create ? " any more" : ""}: ask for another room in the lobby, or host one.`
      : e && e.code === "room-full" ? `Room ${code} is full: ask for another room in the lobby, or host one.`
      : `Could not join room ${code}: ${e && e.code ? e.code + ": " : ""}${e && e.message ? e.message : e}.${e && serverDown(e.code) ? " " + OTHER_SERVER : ""}`);
    $("join").disabled = false;
    if ($("room").value === autoCode) { $("room").value = ""; autoCode = ""; }
    renderTopAction();
  }
}
function clearRoomFromAddress() {
  const url = new URL(location.href);
  for (const k of ["room", "callsign", "firstname", "lastname"]) url.searchParams.delete(k);
  history.replaceState(null, "", withServer(url));
}

// A room code clicked in the public lobby: the pilot wants to be in that
// room.  One they sit in alone (their own, made by Host or by an
// advertisement) is left for it; one with company is not left behind by a
// click.  What came of it is said under the lobby's chat, where the click
// was, as well as in the status line.
async function joinFromLobby(code) {
  const tell = (text) => { status(text); hall.note(text); };
  if (running) { tell(`Reload the page to join ${code}: this page's game has been started.`); return; }
  if (lobby.game && lobby.game.code === code) { tell(`You are in room ${code} already (step 2).`); return; }
  let left = "", leftMade = false;
  if (lobby.game) {
    const company = lobby.game.players.some((p) => p.occupied && p.id !== lobby.game.selfId);
    if (company) { tell(`You are in room ${lobby.game.code} with somebody: leave it (step 2) to join ${code}.`); return; }
    left = lobby.game.code;
    leftMade = $("room").value === autoCode;
    chatLine(`You left room ${esc(left)} for ${esc(code)}`, "sys");
    leaveRoom();
  }
  // (a room of a free game this browser has not got: the game fetched first)
  const theirs = codeGame(code), d = theirs && demoOf(theirs.id);
  if (d && !holderOf(theirs.id)) {
    tell(`Fetching ${d.title} for room ${code}…`);
    if (!(await getGame(d))) { tell(`Could not get ${d.title} for room ${code}.`); return; }
  }
  const typed = $("room").value;
  $("room").value = code;
  await joinRoom({ create: false });
  if (lobby.game) {
    autoCode = code;
    tell(`You are in room ${code} (step 2): the host presses Fly when everyone is in.`);
    $("setup").scrollIntoView({ block: "nearest", behavior: "smooth" });
  } else {
    const why = $("status").textContent;
    $("room").value = typed;
    if (left) { $("room").value = left; await joinRoom(); if (lobby.game) { status(why); if (leftMade) autoCode = left; } }  // (back in the room of one's own)
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
// shows beside the button for a while (Host in step 2 makes one; Join joins the code
// typed there).  A pilot who ticks Private is not asked
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
// Can this browser fly in an offered room: the program it plays is here, or free to fetch?  (The room's code is
// only its name.)
function canFly(r) {
  const prog = gameById(r.game);
  return !!prog && !!prog.multiplayer && (!!holderOf(prog.id) || !!demoOf(prog.id));
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
async function joinOpenGame({ series = false } = {}) {
  if (seeking || lobby.game || running || $("join").disabled) return;
  seeking = true;
  renderTopAction();
  // The game picked comes first (the user, 2026-11-17: "the join prefers the game they want but if none is available
  // they join any room where they have the install files available (eg tie demo) or they have it actually installed"):
  // its open rooms in a random order, then every other game's this browser has or can fetch.  A game is fetched only
  // when its room's turn comes.  With no room joined the game picked is the pick again.
  // `series`: the room panel's button, which keeps to the series of the game picked; the top button takes any game,
  // the picked one's first, then its series'.
  const mine = source ? source.game.id : "";
  const kin = (r) => seriesOf(r.game) === seriesOf(mine);
  const within = series && mine ? seriesName(mine) : "";
  const typed = $("room").value;
  try {
    // (a pilot who had left the lobby comes back for this: its pilots are the ones asked)
    if (!hall.inside() && lobbyReady()) await hall.enter();
    status("Asking the pilots in the lobby for an open game…");
    const found = await hall.findRooms();
    const rooms = found ? found.rooms : [];
    const able = rooms.filter(canFly);
    const fit = within ? able.filter(kin) : able;
    for (let i = fit.length - 1; i > 0; i--) { const j = Math.floor(Math.random() * (i + 1)); [fit[i], fit[j]] = [fit[j], fit[i]]; }
    const rank = (r) => (r.game === mine ? 0 : kin(r) ? 1 : 2);
    fit.sort((a, b) => rank(a) - rank(b));
    for (const r of fit) {
      if (lobby.game || running) return;
      const g = gameById(r.game), d = demoOf(r.game);
      if (!holderOf(r.game)) {
        status(`Fetching ${g.title} for the open room ${r.code}…`);
        if (!d || !(await getGame(d))) continue;
        if (lobby.game || running) return;
      }
      $("room").value = r.code;
      await joinRoom({ create: false, game: r.game });
      if (lobby.game) {
        autoCode = r.code;
        // (Said under the lobby's chat as well: the status line is far below.)
        const was = gameById(mine);
        const text = r.game === mine || !was ? `You are in room ${r.code}, an open ${g.title} room from the lobby.`
          : `No open ${was.title} room: you are in room ${r.code}, an open ${g.title} room (the Game menu says ${g.title} now).`;
        status(text); hall.note(text);
        // (a game other than the one picked: said in the room's chat too, where it stays)
        if (r.game !== mine && was) chatLine(esc(text), "sys");
        if (r.game !== mine && was) topNote(`No open ${was.short || was.title} room: joined ${g.short || g.title}`, text);
        return;
      }
    }
    if (lobby.game || running) return;
    $("room").value = typed;
    if (mine && source && source.game.id !== mine && holderOf(mine)) switchTo(holderOf(mine), mine);
    // (A pilot whose page is older than the question does not answer.)
    const silent = found ? found.asked - found.answered : 0;
    const text = !found ? "Not in the lobby: nobody to ask"
      : `No open ${within ? within + " " : ""}game found${silent ? ` (${silent} pilot${silent === 1 ? "" : "s"} did not answer)` : ""}`;
    const why = !found ? "Join open game asks the pilots in the lobby (enter it above)."
      : fit.length ? "The open games were full by the time this page got there."
      : within && able.length ? `The open games in the lobby are other games: "Join open game" at the top of the page takes any.`
      : rooms.length ? "The open games in the lobby are for games this browser has no files for."
      : silent ? "A page older than Join open game does not answer: a reload brings the new one."
      : "Nobody in the lobby sits in a room with a free seat that is not private: Host makes one, and Advertise offers it there.";
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
  lobby.names.clear(); lobby.links.clear(); lobby.flying.clear(); lobby.games.clear(); lobby.builds.clear();
  // (While the game runs, the button stays: the way out of it.)
  $("lobby").hidden = true; $("leave").hidden = !running; $("share").hidden = true; $("join").disabled = false;
  for (const id of ["room", "seats", "server", "relay"]) $(id).disabled = false;
  // (a code the page put in the box goes with its room; one the pilot typed stays)
  if ($("room").value === autoCode) $("room").value = "";
  autoCode = "";
  followFetched.clear();
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

// Host makes the room (or sits down in it, when the code typed is a room that is there); Join, and Enter in the code's
// box, only join a room that exists; Join open game asks the lobby.
// Host offers its room in the lobby, as Advertise does, unless Private is ticked (the user, 2026-11-17: "the
// non-private one advertises in the lobby unless players was bumped down to 1"; then "there's already a private
// checkbox. Forget the host private. It should advertise unless they check that private box").  A room of one is
// offered to nobody; a room that was there already (the code typed was somebody's) is its host's to offer.  Fly Alone
// stays for a room the button offered (hostOffered), as nobody may come.  (?advertise=off: the page tests' rooms, said
// by hand.)
const hostOffered = new Set();
async function hostRoom() {
  if (lobby.game || $("join").disabled) return;
  await joinRoom();
  if (!lobby.game || soloRoom() || privateOn() || !isHost() || query.get("advertise") === "off") return;
  const code = lobby.game.code;
  hostOffered.add(normalizeCode(code));
  const offered = await hall.advertise();
  updateActions();
  const text = offered ? `You host room ${code}, offered in the lobby: Fly when somebody joins, or Fly Alone.`
    : `You host room ${code}. It could not be said in the lobby just now (Advertise there says it again; "Copy link" for friends).`;
  status(text); hall.note(text);
}
$("join").addEventListener("click", () => void hostRoom());
$("setup").addEventListener("submit", (ev) => { ev.preventDefault(); void joinRoom({ create: false }); });
$("findRoom").addEventListener("click", () => void joinOpenGame({ series: true }));
// (Players: 1, 2 or 3, whatever is typed; three is in beta.)
$("seats").addEventListener("input", () => { $("seatsBeta").hidden = seatCount() < 3; if (!lobby.game) updateActions(); });
$("seats").addEventListener("change", () => { $("seats").value = seatCount(); $("seatsBeta").hidden = seatCount() < 3; });
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
  if (!source || !lobby.game || running || $("fly").disabled) return;
  if (isHost()) for (const [seat, rel] of lobby.builds) if (rel !== "same") chatLine(`${esc(lobby.names.get(seat) || `Player ${seat + 1}`)} won't fly along (${rel === "site" ? "another page" : rel === "newer" ? "this page is out of date" : "their page is out of date"}).`, "sys");
  if (isHost()) sendLobby({ t: "start", game: source.game.id, mission: missionKey(lobby.mission), rocks: lobby.rocks, wingBumps: lobby.wingBumps, latency: lobby.latency, fps: lobby.fps, capHull: lobby.capHull, privCrew: isPriv() ? lobby.privCrew : undefined });
  void start(true);
});
// Fly Alone: the empty room left for a room of one of the same code (soloGame: no lobby server, nobody can
// come in, no advertisement), the mission and the settings picked kept, and the flight started as Fly would
async function flyAlone() {
  if (!source || minSeats() > 1 || running || (lobby.game && !aloneInRoom())) return;
  const code = lobby.game ? lobby.game.code : normalizeCode($("room").value.trim()), seats = $("seats").value;
  const keep = lobby.game ? { mission: lobby.mission, rocks: lobby.rocks, wingBumps: lobby.wingBumps, latency: lobby.latency, fps: lobby.fps, capHull: lobby.capHull } : {};
  const made = !!code && code === autoCode;
  if (lobby.game) leaveRoom();
  $("room").value = code;
  $("seats").value = "1";
  roomScroll = false;
  try { await joinRoom(); } finally { roomScroll = true; }
  if (made && lobby.game) autoCode = code;
  lobby.seatsBefore = seats;            // (the Players box as it was, back when this room of one is left)
  if (!soloRoom()) return;
  Object.assign(lobby, keep);
  renderMission();
  log(`lobby: ${lobby.game.code} flown single player: a room of one, nobody told`);
  void start(true);
}
$("flySolo").addEventListener("click", () => void flyAlone());
// (the top's Fly Solo, a phone's: the same, from the top of the page; the game's start scrolls down to it)
$("topSolo").addEventListener("click", () => void flyAlone());
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
  // (the user, 2026-11-22, of Privateer: "The ratio should be ideantical to what the original players of privateer saw")
  document.documentElement.classList.toggle("stretch", !!(g && g.stretch));
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
// Privateer: the face the others see when you talk (the user, 2026-11-08: "ideally they could select from any of the
// faces in a dropdown ... browser should remember but they can change every time they launch again"): a cast of the
// game's and a portrait id, src/cpu/wcnet_priv_session.cpp.
const PRIV_FACE_KEY = "wc:privFace";
try { const f = localStorage.getItem(PRIV_FACE_KEY); if (f && [...$("privFace").options].some((o) => o.value === f)) $("privFace").value = f; } catch (e) { /* the first */ }
$("privFace").addEventListener("change", () => { try { localStorage.setItem(PRIV_FACE_KEY, $("privFace").value); } catch (e) { /* not kept */ } });
// Privateer at 640 x 400, each game pixel 2 x 2 (the user, 2026-11-22: "selectable from the play menu (will be on by
// default when it's ready but now a checkbox that's off by default)"; then "we can tick it for now by default"): each
// pilot's own screen, WCPRIVHIRES at the launch and wc_web_priv_hires while flying; the ` key cycles it in the emulator
// (src/cpu/wcnet_priv_hires.cpp).  "Hi-res cockpit" (the user, 2026-11-22: "Can we have a third toggle mode where
// reticle and cockpit are not high rez but sprites in space are?"; "Target boxes are but aiming and all other things are
// not in that third mode"): unticked, the space alone -- the ships, the lasers, the target box -- the crosshair, the
// radar and the cockpit the game's own.  The level: 0 off, 1 the space alone, 2 all.
const PRIV_HIRES_KEY = "wc:privHires", PRIV_HIRES_COCKPIT_KEY = "wc:privHiresCockpit";
// (ticked unless this browser unticked it: the user, 2026-11-22, "we can tick it for now by default")
try { $("privHires").checked = localStorage.getItem(PRIV_HIRES_KEY) !== "0"; } catch (e) { $("privHires").checked = true; }
try { $("privHiresCockpit").checked = localStorage.getItem(PRIV_HIRES_COCKPIT_KEY) !== "0"; } catch (e) { $("privHiresCockpit").checked = true; }
// ?privclassic=1: Privateer exactly as it was before that work (the user, 2026-11-22: "is there a way on the website to
// run privateer without high res code loaded at all in case it causes an issue?"): the boxes greyed out, WCPRIVCLASSIC
const PRIV_CLASSIC = new URLSearchParams(location.search).get("privclassic") === "1";
// ?renderer=software: the emulator's picture put on the canvas through its 2D context instead of WebGL (SDL's own
// renderers: auto, opengl, software).  The user, 2026-11-23, testing on a Mac: white flashes in full screen only; the
// emulator's own frames have none (measured, every frame of a 250 s flight), so the browser's full-screen WebGL is
// the suspect, and this is the other way to show the same frames.
const RENDERER = ["software", "opengl"].includes(query.get("renderer")) ? query.get("renderer") : "";
if (PRIV_CLASSIC) {
  $("privHires").disabled = true;
  $("privHires").checked = false;
  $("privHiresLabel").title = "Off for this visit: the page was opened with ?privclassic=1, Privateer as it was (no high resolution, nor its speed-ups). Open it without that to have them.";
}
const privHiresLevel = () => (PRIV_CLASSIC || !$("privHires").checked ? 0 : $("privHiresCockpit").checked ? 2 : 1);
const privHiresBoxes = () => { $("privHiresCockpit").disabled = PRIV_CLASSIC || !$("privHires").checked; };
privHiresBoxes();
for (const [id, key] of [["privHires", PRIV_HIRES_KEY], ["privHiresCockpit", PRIV_HIRES_COCKPIT_KEY]]) {
  $(id).addEventListener("change", () => {
    try { localStorage.setItem(key, $(id).checked ? "1" : "0"); } catch (e) { /* not kept */ }
    privHiresBoxes();
    if (PRIV_CLASSIC) return;
    const M = running && window.DOSBox;
    if (M && M._wc_web_priv_hires && isPriv()) M._wc_web_priv_hires(privHiresLevel());
  });
}
// (Privateer's pointer: a ring in flight and a white one on the options screen, or the game's own: none, and black.
// Picked in the Controller list since 2026-11-21, the user: "the "Shown a ring in flight" should just be Kbd + mouse
// (shown) (which is the default) and Keyboard + mouse instead of a whole sepratae menu"; given to the emulator at the
// launch, WCPRIVPOINTER, and while flying, wc_web_priv_pointer)
const PRIV_POINTER_KEY = "wc:privPointer";
let privPointer = "ring";
try { const v = localStorage.getItem(PRIV_POINTER_KEY); if (v === "ring" || v === "game") privPointer = v; } catch (e) { /* the first */ }
function setPrivPointer(v) {
  privPointer = v === "game" ? "game" : "ring";
  try { localStorage.setItem(PRIV_POINTER_KEY, privPointer); } catch (e) { /* not kept */ }
  const M = running && window.DOSBox;
  if (M && M._wc_web_priv_pointer && isPriv()) M._wc_web_priv_pointer(privPointer === "ring" ? 1 : 0);
}
// (Privateer's mouse flying: the game's pitch is a flight stick's, the mouse pushed up dives, with no setting for it;
// unticked, the default, the emulator turns it the other way up (the user, 2026-11-22: "The mouse is inverted. Is
// there a way to change it back. An option or toggle"): WCPRIVMOUSEINVERT at the launch, wc_web_priv_mouse_invert while
// flying, src/cpu/wcnet_priv_controls.cpp)
const PRIV_MOUSE_KEY = "wc:privMouseInvert";
try { $("privMouseInvert").checked = localStorage.getItem(PRIV_MOUSE_KEY) === "1"; } catch (e) { /* the first */ }
$("privMouseInvert").addEventListener("change", () => {
  const on = $("privMouseInvert").checked;
  try { localStorage.setItem(PRIV_MOUSE_KEY, on ? "1" : "0"); } catch (e) { /* not kept */ }
  const M = running && window.DOSBox;
  if (M && M._wc_web_priv_mouse_invert && isPriv()) M._wc_web_priv_mouse_invert(on ? 1 : 0);
});
// The same box for Armada's right-button drag (the user, 2026-11-22: "mouse (both inverted and otherwise--probably
// default non-inverted like WC1)"): unticked a drag up raises the nose; ticked as a flight stick, the arrows' way
const PRIV_MOUSE_TITLE = $("privMouseLabel").title;
const ARM_MOUSE_TITLE = "Armada's mouse steering (hold the right button and drag). Unticked: dragging up raises the nose. Ticked: as a flight stick, the arrow keys' way (dragging up, the nose goes down). Works while flying too; remembered in this browser";
// Armada at 640 x 400 (the user, 2026-11-22: "lets figure out how to double the screen x and y for armada"; "It should
// be toggleable from the browser too"): off unless ticked, WCARMHIRES at the launch and wc_web_arm_hires while flying;
// the ` key flips it in the emulator (src/cpu/wcnet_armada_hires.cpp)
const ARM_HIRES_KEY = "wc:armHires";
try { $("armHires").checked = localStorage.getItem(ARM_HIRES_KEY) === "1"; } catch (e) { /* off */ }
// (one level: the space at 640 x 400, the cockpit and reticle the game's own. The user, 2026-11-24, of the hi-res
// cockpit, reticle and displays: "I think for now we should remove the code to make the very high resolution displays
// as they are going very slow")
const armHiresLevel = () => ($("armHires").checked ? 1 : 0);
$("armHires").addEventListener("change", () => {
  try { localStorage.setItem(ARM_HIRES_KEY, $("armHires").checked ? "1" : "0"); } catch (e) { /* not kept */ }
  const M = running && window.DOSBox;
  if (M && M._wc_web_arm_hires && isArmada()) M._wc_web_arm_hires(armHiresLevel());
});
// Privateer's room, the host's choice (src/cpu/wcnet_priv_session.cpp, crew_from_env; docs/privateer.md section 30; the
// user, 2026-11-24: "launch privateer in a different mode where player two (for Orion) or three (for galaxy) flies in
// the turret of the associated ship", "a game mode set in the lobby"): everybody their own ship, or the host's ship
// with the others in its turrets.  The host's last choice is this browser's next room's.
const PRIV_CREW_KEY = "wc:privCrew";
try { const v = localStorage.getItem(PRIV_CREW_KEY); if (PRIV_CREWS.includes(v)) lobby.privCrew = v; } catch (e) { /* the first */ }
$("privCrew").addEventListener("change", () => {
  if (!isHost()) { $("privCrew").value = lobby.privCrew; return; }
  lobby.privCrew = PRIV_CREWS.includes($("privCrew").value) ? $("privCrew").value : "own";
  try { localStorage.setItem(PRIV_CREW_KEY, lobby.privCrew); } catch (e) { /* not kept */ }
  if (lobby.game) {
    chatLine(lobby.privCrew === "crew" ? "One ship, crewed: you fly, the others man your turrets (up to 3 in the room). They launch after you do."
      : "Everyone their own ship.", "sys");
    sayHello();
  }
  renderMission();
});
// (Privateer's friendly fire between pilots, the host's counts: src/cpu/wcnet_priv_session.cpp, ff_from_env)
const PRIV_FF_KEY = "wc:privFF";
try { const v = localStorage.getItem(PRIV_FF_KEY); if (["targeted", "always", "never"].includes(v)) $("privFF").value = v; } catch (e) { /* the first */ }
$("privFF").addEventListener("change", () => { try { localStorage.setItem(PRIV_FF_KEY, $("privFF").value); } catch (e) { /* not kept */ } });
// Terminal Velocity's craft menus: the registered game's craft of its other planets (the models in its CDROM.POD, the
// names its briefings'; src/cpu/wcnet_tv_session.cpp's table) after the shareware's, offered while the game here is
// the registered one (tvCraftOffer); remembered in this browser
const TV_FULL_CRAFT = [["ddcshp.bin", "Crythanian Shockstar"], ["afighter.bin", "WSF Razor-4"], ["dogg.bin", "TTA Sandstorm"],
  ["pooh.bin", "TTA Dustdevil"], ["deboss.bin", "TTA Penetrator"], ["lavash.bin", "OSO Phoenix"], ["triship.bin", "Ositsho Wedge"],
  ["volship.bin", "Ositsho Quadram"], ["litecr.bin", "Erigonean Shriek"], ["bugship.bin", "CAF Attack Fighter"],
  ["podgun1.bin", "DTS-1"], ["pincher.bin", "XI Coreguard"]];
for (const id of ["tvCraftHost", "tvCraftGuest"]) {
  for (const [value, text] of TV_FULL_CRAFT) {
    const o = document.createElement("option");
    o.value = value; o.textContent = text; o.dataset.full = "1"; o.hidden = true;
    $(id).append(o);
  }
}
function tvCraftOffer() {
  const full = !!(source && source.game.id === "tv" && source.files.some((f) => /(^|\/)CDROM\.POD$/i.test(f.path)));
  for (const id of ["tvCraftHost", "tvCraftGuest"]) {
    for (const o of $(id).options) if (o.dataset.full) o.hidden = o.disabled = !full;
    if ($(id).selectedOptions[0] && $(id).selectedOptions[0].disabled) $(id).value = "ship.bin";
  }
}
for (const id of ["tvCraftHost", "tvCraftGuest"]) {
  const key = "wc:" + id;
  try { const v = localStorage.getItem(key); if (v && [...$(id).options].some((o) => o.value === v)) $(id).value = v; } catch (e) { /* the first */ }
  $(id).addEventListener("change", () => { try { localStorage.setItem(key, $(id).value); } catch (e) { /* not kept */ } });
}
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

// Armada uses the mouse in menus and has no mouse flight of its own. A right-button drag steers it as a stick would:
// the emulator writes the drag into the game's control packet (wcnet_armada.cpp), the further from where the button
// went down the faster the turn (64 px the full turn, as an arrow key held), centred when the button is let go. The
// left button with it fires the guns (Space). An emulator without that steers by the keypad's arrows instead.
let armadaDrag = null;
const armadaMouseKeys = new Set();
const ARMADA_DRAG_FULL = 64, ARMADA_DRAG_DEAD = 3;
const armadaDragAxis = (d) => (Math.abs(d) <= ARMADA_DRAG_DEAD ? 0
  : Math.sign(d) * Math.min(1, (Math.abs(d) - ARMADA_DRAG_DEAD) / (ARMADA_DRAG_FULL - ARMADA_DRAG_DEAD)));
function armadaMouseInput() {
  const send = window.DOSBox?._wc_web_key;
  if (!send) return;
  const steer = window.DOSBox?._wc_web_arm_steer;
  const keys = [];
  // (the game's pitch is a flight stick's: its Up arrow dives; a drag up raises the nose unless Invert mouse is ticked)
  const pitch = $("privMouseInvert").checked ? 1 : -1;
  if (armadaDrag) {
    if (!steer) {
      if (armadaDrag.x < -12) keys.push("kp4");
      if (armadaDrag.x > 12) keys.push("kp6");
      if (pitch * armadaDrag.y < -12) keys.push("kp8");
      if (pitch * armadaDrag.y > 12) keys.push("kp2");
    }
    if (armadaDrag.fire) keys.push("space");
  }
  if (steer) steer(armadaDrag ? armadaDragAxis(armadaDrag.x) : 0, armadaDrag ? pitch * armadaDragAxis(armadaDrag.y) : 0, armadaDrag ? 1 : 0);
  for (const key of armadaMouseKeys) if (!keys.includes(key)) send(KEY[key], 0);
  for (const key of keys) if (!armadaMouseKeys.has(key)) send(KEY[key], 1);
  armadaMouseKeys.clear();
  for (const key of keys) armadaMouseKeys.add(key);
}
function endArmadaDrag() {
  if (!armadaDrag) return;
  armadaDrag = null;
  armadaMouseInput();
}
canvas.addEventListener("mousedown", (e) => {
  if (running && isArmada() && e.button === 2) armadaDrag = { x: 0, y: 0, fire: false };
  if (armadaDrag && e.button === 0) {
    armadaDrag.fire = true;
    armadaMouseInput();
  }
});
window.addEventListener("mousemove", (e) => {
  if (!armadaDrag) return;
  if (!(e.buttons & 2)) { endArmadaDrag(); return; }
  armadaDrag.x = Math.max(-64, Math.min(64, armadaDrag.x + e.movementX));
  armadaDrag.y = Math.max(-64, Math.min(64, armadaDrag.y + e.movementY));
  armadaDrag.fire = !!(e.buttons & 1);
  armadaMouseInput();
});
window.addEventListener("mouseup", (e) => {
  if (e.button === 2) endArmadaDrag();
  if (armadaDrag && e.button === 0) {
    armadaDrag.fire = false;
    armadaMouseInput();
  }
});
window.addEventListener("blur", endArmadaDrag);
canvas.addEventListener("blur", endArmadaDrag);

// RetroVega's taunt hotkey, Alt+T, is a browser's own menu key outside full
// screen (Firefox's Tools): with the game in focus the browser is told to leave
// it alone, and the game still gets the key.  (Alt+B, the bookmarks, is left
// to the browser: the user's.)
window.addEventListener("keydown", (e) => {
  if (!running || !e.altKey || e.ctrlKey || e.metaKey || e.code !== "KeyT") return;
  if (document.activeElement !== canvas && document.fullscreenElement !== screenEl) return;
  e.preventDefault();
}, true);

// The game's sound, started by the pilot's own touch (the user, 2026-11-19: "I'm no longer hearing sound happening on
// my mobile...  I can hear sound on youtube").  A browser lets a page's sound begin only from a touch or a key.  The
// emulator makes its sound some seconds after Fly was pressed, files and program first, and then asks the browser to
// start it from a timer: Chrome takes that (a page touched once is trusted), an iPhone's Safari does not, and in flight
// the page keeps the picture's touches from the emulator, so no later touch started it either.  So the page makes the
// sound's context itself, in the touch that starts the game (Fly, Fly Solo, the top's button, a touch of the picture),
// hands it to the emulator (the module's SDL2.audioContext, which its audio code takes when there is one), and starts
// it again at any touch or key that finds it stopped.  And an iPhone's ringer switch silences a page's sound, though
// not a video's (YouTube plays on): where the browser has an audio session (iOS 17) the page says its sound is
// playback, while Voice is off (a microphone's session is the system's to choose).
let gameAudio = null;
function unlockAudio(make) {
  try {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!gameAudio && make && AC) gameAudio = new AC();
    const sdl = window.DOSBox && window.DOSBox.SDL2 && window.DOSBox.SDL2.audioContext;
    for (const ctx of [gameAudio, sdl]) if (ctx && ctx.state === "suspended") void ctx.resume();
    if (PHONE && navigator.audioSession) navigator.audioSession.type = $("voice").value === "off" ? "playback" : "auto";
  } catch (e) { /* no sound is no reason to stop */ }
}
for (const ev of ["pointerdown", "pointerup", "touchend", "click", "keydown"]) {
  window.addEventListener(ev, (e) => unlockAudio(!!(e.target && e.target.closest && e.target.closest("#topAction, #topSolo, #fly, #flySolo, #screen"))),
    { capture: true, passive: true });
}

async function start(fromGesture) {
  if (running) return;
  running = true;
  scrollToGame();
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
  for (const el of ["gamefile", "gamefileButton", "exePicker", "callsign", "firstname", "lastname", "verbose", "hint"]) $(el).disabled = true;
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
    env.WCGAME = source.game.id;     // (the game this run is for: the TIE demo's room lives from the boot on, wcnet_tiedemo.cpp)
    if (isTie()) env.WCNET_TIE_MOUSE = tfFlight();
    // (each flight starts with the lasers' recharge one step up, wcnet_tie_controls.cpp: a phone or a tablet has no F9,
    // and on a computer too: the user, 2026-11-19, "would be useful on desktop web too not just mobile")
    if (isTie()) env.WCNET_TIE_TOUCH = "1";
    if (isPriv()) env.WCPRIVFACE = $("privFace").value;
    if (isPriv()) env.WCPRIVPOINTER = privPointer;
    if (isPriv()) env.WCPRIVMOUSEINVERT = $("privMouseInvert").checked ? "1" : "0";
    if (isPriv() && privHiresLevel()) env.WCPRIVHIRES = privHiresLevel() === 2 ? "1" : "space";
    if (isArmada() && armHiresLevel()) env.WCARMHIRES = "1";
    if (isPriv() && PRIV_CLASSIC) env.WCPRIVCLASSIC = "1";
    if (isPriv() && together) env.WCPRIVFF = $("privFF").value;
    if (isPriv() && together && lobby.privCrew === "crew") env.WCPRIVCREW = "1";
    if (isTv() && together && isHost()) env.WCTVLOOKS = `${$("tvCraftHost").value},${$("tvCraftGuest").value}`;
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
      ...(gameAudio ? { SDL2: { audioContext: gameAudio } } : {}),     // (the sound's context, made in the pilot's touch: unlockAudio)
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
        const said = gameInfo().prepare(Module.FS, GAME_ROOT, { joystick: joystickPort });
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
    // the nav points carry their hazards); ?nogx=1 the machine without the
    // sprite accelerator (RV.EXE /nogx: it draws everything itself, a
    // planet too).
    const isRv = gameInfo() && gameInfo().id === "rv";
    const field = isRv ? Math.max(0, Math.min(999, parseInt(query.get("field") || "0", 10) || 0)) : 0;
    const cmd = query.get("cmd") || source.game.run + missionArgs + (query.get("res") === "1" && isRv ? " /res 1" : "")
      + (field ? ` /field ${field}` : "") + (query.get("nogx") === "1" && isRv ? " /nogx" : "");
    // The emulated CPU speed: the game's own good value (the registry), or
    // ?cycles=N; Ctrl+F11 / Ctrl+F12 still adjust it while playing.  A game
    // whose registry says `cyclesAuto` (or ?cycles=max) gets DOSBox's own
    // automatic cycles, up to its value: as many as this computer runs in
    // real time with room left for the browser's sound and picture.
    const askedCycles = query.get("cycles");
    const cycles = Number(askedCycles) || (gameInfo() && gameInfo().cycles) || 0;
    const autoCycles = askedCycles === "max" || (!Number(askedCycles) && !!(gameInfo() && gameInfo().cyclesAuto));
    const args = ["-c", `mount c ${GAME_ROOT}`];
    // The game's disc, where it comes with one (the registry's image): drive D.
    if (gameInfo() && gameInfo().image) args.push("-c", `imgmount d ${GAME_ROOT}/${gameInfo().image} -t iso`);
    // ... or wherever these files have it (the registry's disc: an image of any name, or the disc's files in a folder)
    if (gameInfo() && gameInfo().disc) {
      const disc = gameInfo().disc(source.files);
      if (disc && disc.image) args.push("-c", `imgmount d "${GAME_ROOT}/${disc.image}" -t iso`);
      else if (disc) args.push("-c", `mount d "${GAME_ROOT}${disc.folder ? "/" + disc.folder : ""}" -t cdrom`);
      else log(`${gameInfo().title}: no CD among its files (an image, or the disc's folders): the game will ask for it`);
    }
    args.push("-c", "c:");
    if (autoCycles) args.push("-c", `cycles=max${cycles > 0 ? ` limit ${Math.round(cycles)}` : ""}`);
    else if (cycles > 0) args.push("-c", `cycles=${Math.round(cycles)}`);
    // What the game's own setup expects of the machine (the registry), as a
    // configuration file: DOSBox's `config -set` rewrites the list of
    // startup commands it is itself run from, and the game's is lost.
    // The game port, for a built-in pad: counting (timed=false), since a
    // timed port's readings grow with the CPU's speed, which a flight
    // raises; the type stays "none", or DOSBox's own mapper would hold the
    // stick at rest every frame (the page puts it on the port, gamepad.js).
    // Any other launch's configuration is what it always was.
    const game = gameInfo();
    const ipx = together && game && game.ipx;
    const conf = (game && game.dosbox || "") + (ipx ? "[ipx]\nipx=true\n" : "")
      + (joystickPort ? "[joystick]\njoysticktype=none\ntimed=false\n" : "")
      + (RENDERER ? `[sdl]\nrenderer=${RENDERER}\n` : "");
    if (conf) { Module.FS.writeFile("/game.conf", conf); args.unshift("-conf", "/game.conf"); }
    if (ipx && game.netCommand) args.push("-c", game.netCommand);
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
    scrollToGame();
    if (fromGesture && $("fullStart").checked) goFullscreen();
    status(together
      ? `Room ${cfg.code}. "Copy link" (step 2) to play with friends.`
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
  // (TIE Fighter and its demo have no steering pointer: the emulator steers them by a stick, the controller's way,
  // right and climbing positive; the thumb up climbs, as the pointer's games do.  Until 2026-11-19 the touch stick
  // looked for a pointer there, found none and steered nothing: the user, "On mobile turning joystick does nothing")
  if (isTie() || isPriv()) {
    const T = running && window.DOSBox;
    if (!T || !T._wc_web_stick) return false;
    T._wc_web_stick(x, -y);
    return true;
  }
  const M = running && window.DOSBox, p = M && steeringPointer();
  if (!p || !M._wc_web_pointer) return false;
  const axis = (v, steps, reach, edge) => v === 0 ? 0 : steps ? stepped(v, steps, reach, edge, p.top) : v * reach;
  M._wc_web_pointer(p.x + axis(x, p.stepsX, p.rx, p.edgeX), p.y + axis(y, p.stepsY, p.ry, p.edgeY));
  return true;
}, remap: (id) => (isTie() && id === "touchNav"
  // (TIE Fighter has no navigation map, and a phone no F9: the user, "F9 simply isn't on mobile ... the one button for
  // laser power up/down for now")
  ? { key: "f9", label: "Lasers", title: "The lasers' recharge rate (F9): each press a step up, to the most, then none, then up again" } : null) });

// TIE Fighter's flight controls (src/cpu/wcnet_tie_controls.cpp): the mouse points to aim, drags to steer (the
// game's own), or steers nothing (the keyboard alone, a joystick); the arrows always steer.  Remembered in this
// browser, given to the emulator at the launch (WCNET_TIE_MOUSE) and while flying (wc_web_tie_mouse).
const TF_FLIGHT = ["drag", "pointer", "keyboard", "joystick", "pointerbank"];   // (the emulator's mode numbers)
const TF_FLIGHT_KEY = "wc-tf-flight";
const isTf = () => !!(gameInfo() && gameInfo().id === "tf");
// (TIE Fighter or its 1994 demo: the same Flight choice and controller, the user: "point to aim mouse mode should be
// default. They can select drag to aim just like on cd version. also joystick should be like cd version too")
const isTfd = () => !!(gameInfo() && gameInfo().id === "tiedemo");
// (X-Wing Collector's CD: the demo's engine, the same flight keys and Flight choice: wcnet_tiedemo_controls.cpp built for it)
const isXw = () => !!(gameInfo() && gameInfo().id === "xwing");
const isTie = () => isTf() || isTfd() || isXw();
const isPriv = () => !!(gameInfo() && (gameInfo().id === "priv" || gameInfo().id === "privrf"));
const isTv = () => !!(gameInfo() && gameInfo().id === "tv");
const isRap = () => !!(gameInfo() && gameInfo().id === "rap");
// (the room's game, the loaded one's or the lobby's)
function isTvRoom() { return lobbyGameId() === "tv" || isTv(); }
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
  log(`${isXw() ? "X-Wing's" : isTfd() ? "The TIE Fighter demo's" : "TIE Fighter's"} flight controls: ${e.target.selectedOptions[0].textContent}`);
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
// The TIE Fighter demo's (its flight keys, the page's keys line): Backspace full throttle, X single, dual or quad fire,
// R the nearest enemy fighter, W the bomber's torpedoes (and back to the lasers: the trigger fires either); no
// autopilot, map or comms.
const TFD_PAD = {
  missile: { keys: ["w"], label: "Fire missile (the TIE Fighter demo: W picks the torpedoes, or the lasers again; the trigger fires them)" },
  burner: { keys: ["backspace"], label: "Afterburner (the TIE Fighter demo: full throttle)" },
  nav: { keys: [], label: "Navigation map (nothing in the TIE Fighter demo)" },
  auto: { keys: [], label: "Autopilot (nothing in the TIE Fighter demo)" },
  gunsel: { keys: ["x"], label: "Change guns (the TIE Fighter demo: single, dual or quad fire)" },
  lock: { keys: ["r"], label: "Lock target (the TIE Fighter demo: the nearest enemy fighter)" },
  comms: { keys: [], label: "Communications (nothing in the TIE Fighter demo)" },
};
// X-Wing's: W the torpedoes (or the lasers again), Backspace full throttle, M the map, X the cannons linked, R the
// nearest enemy fighter.
const XW_PAD = {
  missile: { keys: ["w"], label: "Fire missile (X-Wing: W picks the torpedoes, or the lasers again; the trigger fires them)" },
  burner: { keys: ["backspace"], label: "Afterburner (X-Wing: full throttle)" },
  nav: { keys: ["m"], label: "Navigation map (X-Wing: the in-flight map)" },
  auto: { keys: [], label: "Autopilot (nothing in X-Wing)" },
  gunsel: { keys: ["x"], label: "Change guns (X-Wing: the cannons linked or not)" },
  lock: { keys: ["r"], label: "Lock target (X-Wing: the nearest enemy fighter)" },
  comms: { keys: [], label: "Communications (nothing in X-Wing)" },
};
const tiePad = () => (isXw() ? XW_PAD : isTfd() ? TFD_PAD : TF_PAD);
// Raptor's keys for a controller's buttons (the user: "Please add gamepad support if selected on the menu"): Ctrl fires,
// Alt the special weapon, Space picks the next special, the right Shift the mega bomb (its own keys, SETUP.INI's
// defaults); Start is Enter in its menus and P (pause) in a wave, Back is Esc.  The stick and the D-pad are its arrows
// (stickKeys below): the game's own joystick is no more than its arrows (full speed past a third of the stick, in its
// INPUT.C), and its menus take the arrows too.
const RAP_PAD = {
  guns: { keys: ["leftctrl"], label: "Fire guns (Raptor: fire; in its menus a click)" },
  missile: { keys: ["leftalt"], label: "Fire missile (Raptor: the special weapon)" },
  target: { keys: ["space"], label: "Change target (Raptor: the next special weapon)" },
  weapon: { keys: ["rightshift"], label: "Change weapon (Raptor: the mega bomb)" },
  faster: { keys: [], label: "Speed up (nothing in Raptor)" },
  slower: { keys: [], label: "Slow down (nothing in Raptor)" },
  burner: { keys: [], label: "Afterburner (nothing in Raptor)" },
  nav: { keys: ["esc"], label: "Navigation map (Raptor: Esc: out of a menu; in a wave it asks to abort the mission)" },
  auto: { keys: ["enter"], flight: ["p"], label: "Autopilot (Raptor: Enter in its menus, pause in a wave)" },
  up: { keys: [], label: "Up (Raptor: the D-pad and the stick fly the ship)" },
  down: { keys: [], label: "Down (Raptor: the D-pad and the stick fly the ship)" },
  gunsel: { keys: [], label: "Change guns (nothing in Raptor)" },
  lock: { keys: [], label: "Lock target (nothing in Raptor)" },
  comms: { keys: [], label: "Communications (nothing in Raptor)" },
};
const rapKeys = (id) => {
  const k = RAP_PAD[id];
  if (!k) return null;
  const M = running && window.DOSBox;
  return k.flight && M && M._wc_web_in_flight && M._wc_web_in_flight() ? k.flight : k.keys;
};
const controls = initControls({
  module: () => (running && window.DOSBox) || null,
  // (TIE Fighter: a controller steers through the emulator's stick, whatever the mouse does, rolls on the keypad's
  // Ins and Del as the emulator has it, and fires with the mouse's button, the game's own trigger)
  // (Privateer too since 2026-11-21: the stick steers whatever the game's options say, the keyboard, the mouse, or the
  // joystick with none on the port, wcnet_priv_controls.cpp; the user, "If I select mouse, the joystick drove around the
  // blue dot ... way too slow to steer"; its roll keys are the keypad's 7 and 9)
  directStick: () => isTie() || isPriv(),
  rollKeys: () => (isTie() ? { left: "kp0", right: "kpperiod" } : isPriv() ? { left: "kp7", right: "kp9" } : null),
  actionKeys: (id) => {
    if (isArmada() && id === "faster") return ["kpplus"];
    if (isArmada() && id === "slower") return ["kpminus"];
    return isTie() && tiePad()[id] ? tiePad()[id].keys : isRap() ? rapKeys(id) : null;
  },
  actionLabel: (id) => (isTie() && tiePad()[id] ? tiePad()[id].label : isRap() && RAP_PAD[id] ? RAP_PAD[id].label : null),
  // (Raptor: the stick in a wave and the D-pad everywhere are its arrow keys, the stick past 30% of its travel)
  stickKeys: () => (isRap() ? { left: "left", right: "right", up: "up", down: "down", at: 0.3 } : null),
  fireByMouse: () => isTie(),
  // (A controller button without a key of its own is the page's: push to talk.)
  onAction: (id, down) => { if (id === "ptt") voice.ptt(down); },
  pointer: steeringPointer,
  builtIn: () => !!(gameInfo() && gameInfo().joystick),
  builtInHint: () => (isPriv()
    ? "Built in: Privateer flies this pad as the joystick on its game port, its own way (the options' JOYSTICK is picked for you as the flight starts): button 1 fires the guns, both buttons a missile; hold button 2 and move the stick to roll and change speed. At a base the stick moves the pointer and button 1 clicks. The other buttons still press their keys."
    : isArmada() ? "Built in: calibrate JOY 1 in Armada’s Options before entering net play. The game reads the stick and trigger through its joystick port." : null),
  noneChoices: () => (isPriv()
    ? [{ value: "none", label: "Kbd + mouse (shown)", picked: privPointer === "ring" }, { value: "none-game", label: "Keyboard + mouse", picked: privPointer === "game" }]
    : null),
  pickNone: (v) => { if (isPriv()) setPrivPointer(v === "none-game" ? "game" : "ring"); },
  joystickPort: () => (running ? joystickPort : null),
  log,
});

// -- go ------------------------------------------------------------------------

void initSources();
// A shared link (?room=CODE) puts a pilot with a game to fly straight into
// the room, as soon as the game files are there (setSource).  A page
// without them joins nothing by itself: whoever opens the link, or whatever
// does, has said nothing to the lobby server until a game is loaded or
// Host or Join is pressed.
function joinLinked() { if (linkWaits() && lobbyReady()) void joinRoom(); }
updateActions();
// The public lobby is where a pilot with a game to fly is unless it left:
// hall.auto says when not (?lobby=off stays out this once, for tests).  The
// page enters when the game files are there (setSource), and not before: a
// visitor who has none costs the lobby server nothing.
if (hall.auto({ link: !!query.get("room"), off: query.get("lobby") === "off" })) hall.enterWhenReady();
