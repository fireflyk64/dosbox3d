// Bringing your own game files: everything stays in the browser.
//
//   readZip(bytes)              -> [{path, data}] from a .zip (stored/deflate)
//   extractInstaller(file, cb)  -> [{path, data}] from an Inno Setup / GOG
//                                  installer, using innoextract compiled to
//                                  WebAssembly in a worker (inno-worker.js)
//   identifyGame(files)         -> which game this is and how to run it
//   saveGame / loadGame / forgetGame: IndexedDB cache so the files only have
//                                  to be dropped once per browser
//
// The game registry is the only game-specific knowledge here: a game is
// recognised by an executable, run by a DOS command, and flagged when the
// emulator's multiplayer hooks apply to it (the five programs of Wing
// Commander 1 and 2: src/cpu/wcnet_game.cpp).
// `tag` is the game's name in a room code (WC1-4821; web/chatfilter.js):
// everyone in a room runs the same program, and the code says which.
// `partOf`: a program that comes in another game's directory (The Secret
// Missions 2 in Wing Commander's, the two Special Operations in Wing
// Commander II's): the directory is recognised by the game's own executable
// and the page lets the player choose among the programs in it
// (programsIn).
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
// speed from this, while in flight the hooks hold 30 frames a second with
// 24000 cycles: WCFPS and WCFLIGHTCYCLES in the environment), and `pointer`,
// for a controller's stick (web/gamepad.js): where the mouse pointer rests
// when the game steers by it and how far it reaches, as fractions of the
// mouse range.  Both games park the pointer in the middle of the cockpit's
// window, which is another rectangle in every ship (318,52 of 640x200 in
// Wing Commander's Hornet, 34 up in its Rapier) and in every turret, and
// turn in steps by its distance from there: `fromGame` asks the running
// game for both (web/wc.js, steeringPointer).  Fixed figures {x, y, rx, ry}
// are the other form, for a game the hooks know nothing about.
// `firstName`: the game's people use the pilot's first name, and the page
// asks for one.  `help`: the line under the game for a game with no
// campaign to pick from.
// `dosbox`: what the game's own setup expects of the machine, as the text of
// a DOSBox configuration file.
// `build`: how the hooks tell the executable they know from another of the
// same name (kGames in src/cpu/wcnet_game.cpp, whose figures these are): a
// string at an offset of the program's data segment, the segment given in
// paragraphs from the start of the image (or, `rvhook`, read from the
// image's own RVHOOK header, as RetroVega has it).  The page asks the file
// the same question (knownBuild): only a pilot whose game the hooks will fly
// is let into the public lobby.
// `fps`: the frame rates the host may fly at, { def, min, max } in whole
// frames a second (the hooks pace the flight: WCFPS; a game that paces
// itself has none).
// `capHull`: the hooks can give capital ships three times their hull (the
// room's "Capship hull" box: WCCAPHULL).
// `joystick`: the game reads a joystick on the game port itself, its own
// way (double clicks and all), and the hooks know its joystick state: the
// controller picker offers each pad "built in" too (web/gamepad.js).
// `noFlight`: no missions to pick and no shared flight (the page hides
// its mission and flight options).
// `campaign.manifest`: the campaign's series come from that file beside
// the page, made by the game's own build (RetroVega's rv-campaign.json):
// wc.js fills `series` from it.  `ownRocks`: the game's asteroids are its
// own, every machine's (the room's asteroid choices do not apply).
// `builtin`: a game the site itself serves (ours to give away), as a
// .tar.gz beside the page: it is in every browser's "Game" menu with no
// files dropped, fetched when it is flown.
// `image`: a CD image among the game's files (GOG's game.gog, the disc as
// raw sectors), mounted as drive D before the game runs (imgmount, an
// ISO).  `keep`: files of the game that the installer-junk rule would drop
// by their extension (TIE Fighter's IMUSE.INI).
// `disc(files)`: the game's CD wherever the dropped files have it (X-Wing: an image of any name, or the disc's files
// in a folder): { image: path } or { folder: path } among the game's files, mounted as drive D; null: none found.
// `noRoomOptions`: the room's Conn and Collide choices are Wing Commander's (the frame exchange's mode, the asteroid
// fields and wing bumps): none of them for this game, so the page leaves them out.  `screen`: the picture's shape on
// the page, [width, height] (640 x 400 when none: Wing Commander's 320 x 200 doubled; TIE Fighter flies at 640 x 480,
// whose rows a 400-row canvas would drop).  `stretch`: whatever the emulator's picture measures (320 x 200 doubled),
// it fills that shape, full screen too (none now: Privateer had 4:3 on 2026-11-22, as a monitor of 1993 showed its 200
// rows, until the user found the 200 rows' stretch over 480 uneven: "it looks worse because of the 4:3 stretch. We
// should keep pixel ratios").
// `prepare(FS, root)`: the game's own settings made fit for the browser after its files are installed (the copy in
// the emulator's memory only; what it did, for the log).

import { unpackPvm } from "./pvm.js";

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

// The Secret Missions 2, the Crusade campaign of SM2.EXE: nine series of two
// missions (10 and up do not exist; 8 and 9 are the two ways it ends).  The
// note is the system and the wing each mission's own record names, read
// from the running game two seconds into the flight.
const SM2_SERIES = [
  { series: 1, missions: ["Firekka, Alpha Wing", "Firekka, Kappa Wing"] },
  { series: 2, missions: ["Firekka, Epsilon Wing", "Firekka, Rho Wing"] },
  { series: 3, missions: ["Firekka, Delta Wing", "Firekka, Chi Wing"] },
  { series: 4, missions: ["Corsair, Beta Wing", "Corsair, Gamma Wing"] },
  { series: 5, missions: ["Firekka, Psi Wing", "Firekka, Theta Wing"] },
  { series: 6, missions: ["Corsair, Sigma Wing", "Corsair, Mu Wing"] },
  { series: 7, missions: ["Corsair, Omicron Wing", "Corsair, Omega Wing"] },
  { series: 8, missions: ["Charon, Iota Wing", "Charon, Upsilon Wing"] },
  { series: 9, missions: ["Charon, Iota Wing", "Charon, Upsilon Wing"] },
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

// The Special Operations are Wing Commander II's program built again, each
// with five series of four missions of its own (surveyed like the game's:
// every mission flown by two in the direct mode, the host's log saying what
// the second player is).
const SO1_SERIES = [
  { series: 1, missions: ["with Stingray", DRONE, GUNNER, GUNNER] },
  { series: 2, missions: [DRONE, DRONE, GUNNER, GUNNER] },
  { series: 3, missions: [GUNNER, GUNNER, "with Hobbes", "with Hobbes"] },
  { series: 4, missions: [GUNNER, GUNNER, GUNNER, GUNNER] },
  { series: 5, missions: [GUNNER, GUNNER, GUNNER, GUNNER] },
];
const SO2_SERIES = [
  { series: 1, missions: [DRONE, DRONE, GUNNER, GUNNER] },
  { series: 2, missions: [GUNNER, "with Stingray", GUNNER, GUNNER] },
  { series: 3, missions: [GUNNER, DRONE, DRONE, DRONE] },
  { series: 4, missions: [DRONE, DRONE, DRONE, DRONE] },
  { series: 5, missions: [GUNNER, GUNNER, "with Maniac", DRONE] },
];
// Special Operations 3, the Culloden campaign (so3/story.md): our own
// missions on Special Operations 2's program, in its twenty slots.  The
// second player flies the named wingman.  Series 5's first mission is the
// ending the campaign script turns to when a mission is lost; its second
// and third are second chances that rejoin the line.
const SO3_SERIES = [
  { series: 1, missions: ["Shakedown, with Maniac", "Hand-me-downs, with Doomsday", "The relay, with Stingray", "Prize, with Maniac"] },
  { series: 2, missions: ["First patrol, with Maniac", "Scramble, with Stingray", "Beacon, with Toro", "Valkyrie, with Maniac"] },
  { series: 3, missions: ["The traders, with Maniac", "Theft, with Maniac", "Rendezvous, with Maniac", "Jump point, with Maniac"] },
  { series: 4, missions: ["Countersign, with Maniac", "Ceremony, with Maniac", "The yard, with Maniac", "Duel, with Maniac"] },
  { series: 5, missions: ["The assault (the raid called off), with Stingray", "Picket line (a second chance), with Downtown", "Blown (a second chance), with Maniac", "The last patrol, with Maniac"] },
];

// What the page says under the mission menu, in a line: with a mission
// picked (`forced`), and for the barracks, to the host and to a wingman.
// The whole story (`help`) is under the game, folded: Fly stays in view.
const WC1_HINTS = {
  forced: "Everyone flies it from a fresh start with the callsigns entered above.",
  host: "The barracks: your save game and your walk to the briefing decide the mission; wingmen get it in the briefing room.",
  wing: "The host flies from the barracks: walk into the briefing room after the host has started, and you get the host's mission.",
  help: "From the barracks (no mission picked): the host's save game and walk to the briefing room decide the mission; a wingman walks into the briefing room on their own ship after the host has started, and gets the host's mission there. With a mission picked, everyone flies it from a fresh start with the callsigns entered above. The second player flies the mission's wingman; a third gets a second copy of that wingman, which the host's game launches for them (where the mission has more wingmen of its own, one of those)." };
const WC2_HINTS = {
  forced: "Everyone starts in the barracks at that point of the story: click \"Fly mission\" there.",
  host: "The barracks: your saved game decides the mission; click \"Fly mission\" and wingmen get your mission.",
  wing: "The host flies from the barracks: click \"Fly mission\" and you get the host's mission.",
  help: "Everyone starts in the barracks, with the story at the mission picked or at the host's saved game: click the door the game calls \"Fly mission\" (point at a door and it is named), and the briefing plays first; wingmen get the host's place in the story, the briefing and the mission. Wingmen fly the mission's wing; when the wing is short of ships, the host's game launches another copy of the wingman's for whoever is left, so two or three can fly. In a mission flown alone the second player is the gunner if the ship has turrets (starting in the rear turret; F2 and F3 are the side turrets, F4 the rear one, F1 the pilot's view) and a third the next turret, and otherwise each gets a copy of the leader's own ship to fly. Only when no ship can be made does a player ride as a drone behind the leader, as its copilot: Up and Down shift the leader's shields to the rear and the front, Space puts shields into the guns, Enter guns into the weakest shield, + and - set the leader's cruising speed, and its gauges show the leader's. Nothing sees or hits the drone; 0, then /chase and Enter, flies it free." };
// "Origin s<series> m<mission>" on the command line of Wing Commander II's
// programs puts the story at that mission and starts in the barracks.
const wc2Mission = (m) => `Origin s${m.series} m${m.mis}`;

// Wing Commander moves everything by a step a frame, so its frame rate is
// its speed: some missions were made for a slower one (the host's choice).
const WC1_FPS = { def: 20, min: 8, max: 60 };
// RetroVega too, at 30 as it runs alone (the user: "the default should be 30
// as the real game is"; fewer make it slower and easier, and lighter on the
// link).
const RV_FPS = { def: 30, min: 8, max: 60 };
// A picked RetroVega mission (its series a campaign node, from
// rv-campaign.json): everyone starts in the briefing room with the campaign
// at that mission, as Wing Commander's picked mission starts at its briefing.
const rvMission = (m) => {
  const s = RV_CAMPAIGN.series.find((e) => e.series === m.series);
  const n = s && s.packed ? s.packed[m.mis] : undefined;
  return Number.isInteger(n) ? `/brief ${n}` : "";
};
const RV_HINTS = {
  forced: "Everyone starts in the briefing room for it, the campaign at that mission; your saves stay as they were.",
  host: "Your campaign decides: walk into the briefing room and your wingmen get your mission there.",
  wing: "The host's campaign decides: walk into the briefing room and you get the host's mission there.",
  // (a room of one: alone below)
  alone: "You start in your quarters with a new campaign; your saved games are in the bunk.",
  aloneForced: "You start in the briefing room for it, the campaign at that mission; your saves stay as they were.",
  help: "RetroVega is a campaign of 40 missions: you start in your quarters aboard the TCS Vigilance and walk the ship, bar, briefing room and deck, saving in the bunk, then fly from the deck. Flown together, the host's game runs the mission and each other pilot flies one of its wingmen (a third gets a fighter of their own). A wingman flies the host's mission whatever their own save holds; their sortie and their kills go into their own save, their campaign stays where it was. When anyone dies, or the carrier is lost, everyone's flight ends, and the host's R (fly it again) or C (carry on) decides for everyone. A pilot's bumps into another pilot do no harm. The host's Fly starts everybody's game. Arrow keys steer the pointer, Tab jumps between the things you can use, Enter uses one." };
// (series: filled from rv-campaign.json by web/wc.js before the menu is made.  alone: a room of one may pick a
// mission too, since the game takes it on its own command line; the user, 2026-11-03: "We'll need a mission picker
// (or start at campaign) like the other games".  start: the menu's first entry, the campaign as the game starts it.)
const RV_CAMPAIGN = { series: [], manifest: "rv-campaign.json", missionArgs: rvMission, hints: RV_HINTS, alone: true,
                      start: { room: "Campaign from the quarters (the host's save game decides)",
                               alone: "Campaign from your quarters (your saves are in the bunk)" } };

// (TIE Fighter's menu words) The TEXT resources of a LucasArts LFD file (an RMAP of 16-byte heads, type, name, size, then the bodies): each body a
// count and that many records, a record a length and its strings, NUL-separated.
function lfdTexts(data) {
  const out = [];
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  for (let i = 0; i + 16 <= data.length;) {
    const type = String.fromCharCode(data[i], data[i + 1], data[i + 2], data[i + 3]);
    const size = dv.getUint32(i + 12, true);
    const end = Math.min(data.length, i + 16 + size);
    if (type === "TEXT" && size >= 2) {
      const records = [];
      let p = i + 16;
      const n = dv.getUint16(p, true);
      p += 2;
      for (let k = 0; k < n && p + 2 <= end; k++) {
        const len = dv.getUint16(p, true);
        const strs = [];
        let s = "";
        for (let q = p + 2; q < Math.min(end, p + 2 + len); q++) {
          if (data[q] === 0) { if (s) strs.push(s); s = ""; } else s += String.fromCharCode(data[q]);
        }
        if (s) strs.push(s);
        records.push(strs);
        p += 2 + len;
      }
      out.push(records);
    }
    i = end;
  }
  return out;
}

// TIE Fighter's missions for the room's menu (docs/tiefighter.md, step 8): the files of its MISSION directory.
// Series 1..7 are the Combat Chamber's craft in its own order (RESOURCE/SHIP1..7.LFD), four missions each; series
// 11..23 the tour's battles 1..13 (RESOURCE/BATTLE1..13.LFD).  A picked mission reaches the game as its file's name
// (WCNET_TIE_MISSION at the launch, wc_web_tie_mission while it runs): the hooks fly it whenever a pilot launches a
// Combat Chamber mission, briefing and all (src/cpu/wcnet_tie_session.cpp).  None picked: the Combat Chamber's own,
// the host's pick there flown by everybody.  The page words the menu from the game's own files when it has them
// (tfMissionTexts: each craft's and battle's name and each mission's first sentence).
const TF_CHAMBER = [["f", "T/F"], ["i", "T/I"], ["b", "T/B"], ["a", "T/A"], ["g", "Gunboat"], ["d", "T/D"], ["m", "Missile Boat"]];
const TF_CRAFT = Object.fromEntries(TF_CHAMBER);
const TF_BATTLES = [
  ["b1m1fm", "b1m2fm", "b1m3bm", "b1m4im", "b1m5gm", "b1m6gm"],
  ["b2m1fw", "b2m2bw", "b2m3iw", "b2m4gw", "b2m5fw"],
  ["b3m1bm", "b3m2bm", "b3m3fm", "b3m4gm", "b3m5bm", "b3m6gm"],
  ["b4m1fm", "b4m2bm", "b4m3bm", "b4m4im", "b4m5gm"],
  ["b5m1iw", "b5m2gw", "b5m3gw", "b5m4aw", "b5m5gw"],
  ["b6m1aw", "b6m2aw", "b6m3gw", "b6m4gw"],
  ["b7m1am", "b7m2am", "b7m3aw", "b7m4dw", "b7m5dm"],
  ["b8m1am", "b8m2dm", "b8m3dm", "b8m4dm", "b8m5dm", "b8m6dm"],
  ["b9m1dw", "b9m2dw", "b9m3dw", "b9m4dw", "b9m5dw", "b9m6dw"],
  ["b10m1gw", "b10m2gw", "b10m3am", "b10m4mm", "b10m5mm", "b10m6mw"],
  ["b11m1mw", "b11m2mw", "b11m3mw", "b11m4mw", "b11m5mw", "b11m6mw", "b11m7mw"],
  ["b12m1am", "b12m2am", "b12m3gm", "b12m4mm", "b12m5mm", "b12m6mm", "b12m7mm"],
  ["b13m1mm", "b13m2mm", "b13m3mm", "b13m4mm", "b13m5aw", "b13m6aw", "b13m7aw", "b13m8aw"],
];
// (the craft a mission file is flown in: the Combat Chamber's second letter, a battle's next to last)
const tfCraftOf = (file) => TF_CRAFT[file[0] === "h" ? file[1] : file[file.length - 2]] || "";
const TF_CAMPAIGN = {
  series: [
    // (the T/F row in a room is the Z-95 challenge: src/cpu/wcnet_tie_session.cpp, step 14)
    ...TF_CHAMBER.map(([l, craft], k) => ({ series: k + 1, group: "Combat Chamber", name: k === 0 ? "Z-95 challenge" : craft,
      files: [1, 2, 3, 4].map((n) => `h${l}${n}${n < 3 ? "w" : "m"}`), missions: [] })),
    ...TF_BATTLES.map((files, k) => ({ series: 11 + k, group: `Battle ${k + 1}`, name: `B${k + 1}M`, files, missions: [] })),
  ],
  noDefault: true,       // (a room for more starts with none picked: the Combat Chamber's own)
  noneLabel: "the Combat Chamber's own (the host's pick there)",
  noneShort: "Combat Chamber",
  live: "wc_web_tie_mission",
  file: (m) => { const s = TF_CAMPAIGN.series.find((e) => e.series === m.series); return (s && s.files[m.mis]) || ""; },
  env: (m) => ({ WCNET_TIE_MISSION: TF_CAMPAIGN.file(m) }),
  label(m, form) {
    const s = this.series.find((e) => e.series === m.series);
    const file = s && s.files[m.mis];
    if (!file) return "no such mission";
    const chamber = file[0] === "h";
    const note = s.missions[m.mis] || "";
    if (form === "short") return chamber ? `${s.name} ${m.mis + 1}` : `B${s.series - 10}M${m.mis + 1}`;
    if (form === "menu") return `${chamber ? s.name + " " : "Mission "}${m.mis + 1}${chamber ? "" : " (" + tfCraftOf(file) + ")"}${note ? ": " + note : ""}`;
    return chamber ? `the Combat Chamber's ${s.name} mission ${m.mis + 1}` : `battle ${s.series - 10}${s.title ? " (" + s.title + ")" : ""}, mission ${m.mis + 1}, in a ${tfCraftOf(file)}`;
  },
  start: { room: "None: the Combat Chamber's own (the host's pick there)" },
  hints: {
    host: "No mission picked: launch from the Combat Chamber and a guest's launch there flies yours. Pick one here to fly a battle's mission together; you may change it between flights.",
    wing: "No mission picked: launch any Combat Chamber mission while the host is briefed or flying to fly the host's.",
    forced: "Launching any Combat Chamber mission briefs and flies this one, for everybody (the tour's own battles stay each pilot's).",
  },
};
// The game's own words for the menu, from its files ([{path, data}]): a mission's first sentence, a battle's name
// ("AFTERMATH OF HOTH" as "Aftermath of Hoth").  Each SHIPn.LFD and BATTLEn.LFD holds one TEXT resource: its craft's or
// battle's name, then a record of its mission files and a record of each one's description, in that order.
TF_CAMPAIGN.fromFiles = function (files) {
  const byPath = new Map(files.map((f) => [f.path.toUpperCase(), f.data]));
  const sentence = (lines) => {
    const t = lines.join(" ").replace(/\s+/g, " ").replace(/(\w)- (\w)/g, "$1$2").trim();   // (a word broken at a line's end joined)
    const m = /^.*?[.!?](?=\s|$)/.exec(t);
    const s = m ? m[0] : t;
    return s.length > 80 ? s.slice(0, 79).replace(/\s+\S*$/, "") + "…" : s;
  };
  const small = new Set(["of", "the", "a", "an", "and", "for", "at", "on", "in", "to"]);
  // (only a name in capitals is cased again: "T/D Technology" stays; "MYLOK IV" keeps its numeral)
  const titled = (t) => (t !== t.toUpperCase() ? t : t.toLowerCase().split(" ").map((w, i) => (/^[ivx]+$/.test(w) ? w.toUpperCase()
    : i && small.has(w) ? w : w.charAt(0).toUpperCase() + w.slice(1))).join(" "));
  let worded = 0;
  for (const s of this.series) {
    const file = s.series <= 7 ? `RESOURCE/SHIP${s.series}.LFD` : `RESOURCE/BATTLE${s.series - 10}.LFD`;
    const data = byPath.get(file);
    if (!(data instanceof Uint8Array)) continue;
    const recs = (lfdTexts(data)[0] || []);
    const at = recs.findIndex((r) => r.length && r.every((x) => s.files.includes(x.toLowerCase())));
    if (at < 0) continue;
    const list = recs[at].map((x) => x.toLowerCase());
    for (let i = 0; i < s.files.length; i++) {
      const k = list.indexOf(s.files[i]);
      const desc = k >= 0 && recs[at + 1 + k];
      if (desc && desc.length) s.missions[i] = sentence(desc);
    }
    if (s.series > 10 && recs[1] && recs[1][0]) s.title = titled(recs[1][0]);
    worded++;
  }
  // The T/F row in a room is the Z-95 challenge, each mission flown from another's file (the gunboat's 1, 3 and 4,
  // the T/I's 2): its words those missions', in two Z-95 Headhunters against the Empire's craft.
  const source = [[5, 0], [2, 1], [5, 2], [5, 3]];
  const tf = this.series.find((e) => e.series === 1);
  for (let i = 0; i < source.length && tf; i++) {
    const from = this.series.find((e) => e.series === source[i][0]);
    const note = from && from.missions[source[i][1]];
    tf.missions[i] = "two Z-95s against the Empire" + (note ? ": " + note : "");
  }
  return worded;
};

// TIE Fighter's music in the browser (the user: "I don't get any music"): iMUSE plays it through the driver IMUSE.INI
// names in its first 14 bytes, GOG's GMIDI.MDR, General MIDI to an MPU-401, which the browser build has no synthesizer
// for: the flight was silent but for its effects.  The Sound Blaster 16 it emulates has an OPL3 too, and iMUSE's own
// FM4.MDR (on the game's disc) plays the music there, as Wing Commander's does.  Measured natively: GMIDI.MDR no OPL
// write in a minute of the opening, FM4.MDR some 500 to 1,500 every 6 s and the FM channel sounding.  The digital
// driver (the next 14 bytes, SB16.WDR) is left as it is.
function tfMusicOnFm(FS, root) {
  const path = `${root}/IMUSE.INI`;
  let data;
  try { data = FS.readFile(path); } catch (e) { return "TIE Fighter: no IMUSE.INI (its music as the game chooses)"; }
  const name = String.fromCharCode(...data.subarray(0, 14)).replace(/\0.*$/, "");
  if (/^FM[24]\.MDR$/i.test(name)) return "";
  const out = new Uint8Array(data);
  out.fill(0, 0, 14);
  for (let i = 0; i < 7; i++) out[i] = "FM4.MDR".charCodeAt(i);
  FS.writeFile(path, out);
  return `TIE Fighter's music: ${name || "none"} (General MIDI: no synthesizer in the browser) changed to FM4.MDR, the Sound Blaster's OPL3`;
}

// A file of a CD image among the game's files (ISO 9660: 2048-byte sectors, or raw 2352-byte ones as GOG's game.gog
// holds them), by its path ("RESOURCE/ICONS640.ICO", any case): its bytes, or null.
function readFromImage(FS, imagePath, wanted) {
  let stream;
  try { stream = FS.open(imagePath, "r"); } catch (e) { return null; }
  try {
    const at = (pos, len) => { const b = new Uint8Array(len); FS.read(stream, b, 0, len, pos); return b; };
    let ss = 0, hdr = 0;
    for (const [s, h] of [[2048, 0], [2352, 16]]) {
      const p = at(16 * s + h, 6);
      if (p[0] === 1 && String.fromCharCode(...p.subarray(1, 6)) === "CD001") { ss = s; hdr = h; break; }
    }
    if (!ss) return null;
    const read = (sector, size) => {
      const out = new Uint8Array(size);
      for (let k = 0; k * 2048 < size; k++) out.set(at((sector + k) * ss + hdr, Math.min(2048, size - k * 2048)), k * 2048);
      return out;
    };
    const u32 = (d, i) => (d[i] | d[i + 1] << 8 | d[i + 2] << 16 | d[i + 3] << 24) >>> 0;
    const pvd = read(16, 2048);
    let extent = u32(pvd, 158), size = u32(pvd, 166);     // (the root directory's record, at 156)
    const parts = wanted.toUpperCase().split("/");
    for (let n = 0; n < parts.length; n++) {
      const d = read(extent, size);
      let found = null;
      for (let i = 0; i < d.length;) {
        if (!d[i]) { i = (Math.floor(i / 2048) + 1) * 2048; continue; }     // (a record never spans sectors)
        const name = String.fromCharCode(...d.subarray(i + 33, i + 33 + d[i + 32])).replace(/;\d+$/, "").toUpperCase();
        if (name === parts[n]) { found = { extent: u32(d, i + 2), size: u32(d, i + 10), dir: (d[i + 25] & 2) !== 0 }; break; }
        i += d[i];
      }
      if (!found || found.dir !== (n < parts.length - 1)) return null;
      extent = found.extent;
      size = found.size;
    }
    return read(extent, size);
  } catch (e) {
    return null;
  } finally {
    FS.close(stream);
  }
}

// The in-flight map's icons (RESOURCE\ICONS640.ICO, MAPICONS.ICO): the installer-junk rule dropped .ICO files, so the
// map asked for the CD (the user, 2026-11-15: "for the inflight map in tie fighter hitting m immediately crashed the
// game on the missing file RESOURCE\icons640.ico and it asked me to insert the CD").  Kept now (`keep`); a copy of the
// game kept in this browser before that gets them from the disc, where they are the same bytes.
const TF_ICONS = ["ICONS640.ICO", "MAPICONS.ICO"];
function tfIconsFromDisc(FS, root) {
  const named = (dir, want) => { try { return FS.readdir(dir).find((n) => n.toUpperCase() === want) || null; } catch (e) { return null; } };
  const res = named(root, "RESOURCE"), image = named(root, "GAME.GOG");
  if (!res || !image) return "";
  const got = [];
  for (const name of TF_ICONS) {
    if (named(`${root}/${res}`, name)) continue;
    const data = readFromImage(FS, `${root}/${image}`, `RESOURCE/${name}`);
    if (!data) continue;
    FS.writeFile(`${root}/${res}/${name}`, data);
    got.push(name);
  }
  return got.length ? `TIE Fighter: ${got.join(" and ")} copied from the disc (the map's icons, missing from this browser's copy)` : "";
}
function tfPrepare(FS, root) {
  return [tfMusicOnFm(FS, root), tfIconsFromDisc(FS, root)].filter(Boolean).join("; ");
}

// Privateer's music made the Sound Blaster's (the user, 2026-11-09: "also zero music here").  SOUND.CFG's first line is
// the music card and the speech card (PRCD.EXE, ovr392:05BB: "%d %d", then each card's four settings): music card 1
// or 3 loads the "roland" driver (General MIDI: GOG's file says 3, for Windows' own synthesizer; there is none in the
// browser: no music at all), 2 the "adlib" one with the settings of the second line, the Sound Blaster's OPL (measured
// natively: the title's music at once, where 3 was silent).  The speech card and its line are kept.
function privMusicOnFm(FS, root) {
  const path = `${root}/SOUND.CFG`;
  let text;
  try { text = new TextDecoder().decode(FS.readFile(path)); } catch (e) { return "Privateer: no SOUND.CFG (its music as the game chooses)"; }
  const lines = text.split(/\r?\n/);
  const first = (lines[0] || "").trim().split(/\s+/);
  if (first[0] === "2") return "";
  const speech = first[1] || "1";
  const out = [`2 ${speech}`, "904 -1 -1 -1", (lines[2] || "544 5 1 -1").trim(), (lines[3] || "0 0").trim(), ""].join("\r\n");
  FS.writeFile(path, new TextEncoder().encode(out));
  return `Privateer's music: card ${first[0] || "none"} (General MIDI: no synthesizer in the browser) changed to 2, the Sound Blaster's FM`;
}

// A joystick's calibration as Origin's library keeps it (stick A's file JOYA.DAT, little-endian words; Armada's has a
// word before it and the throttle's five after, Privateer's the reads' timeout): each axis's lowest, centre and highest
// readings (x, y), its 32 steps a side, each step's size (x above, x below, y above, y below), and the dead band's top
// and bottom.  The game makes a reading above the top ceil((reading - top) / size) steps, below the bottom
// floor((reading - bottom) / size), between them none.  Calibrated in the game, the band is a share of each side
// (Armada 10%; GOG's Privateer file 30%: a built-in stick did nothing until a third of its travel, measured).  The
// page writes these with a pad picked "Built in" (opts.joystick), so the game needs no calibrating (the user,
// 2026-11-23: "Ideally the OS itself handles joystick calibration so the ingame calibration seems to be too much work
// for plyaers these days. Can we bypass that unless they want to *recalibrate*"), with no band of the game's: the
// page's dead zone is the one ("we need to reduce the default deadzone in wing commander privateer and this to 4%").
// The game's own calibration (Armada's Options, JOY 1) still recalibrates, its band with it.
function joyCalibration(words) {
  const bytes = new Uint8Array(words.length * 2);
  words.forEach((w, i) => { bytes[2 * i] = w & 0xff; bytes[2 * i + 1] = w >> 8; });
  return bytes;
}
// Privateer reads the port by counting (DOSBox's untimed port: 64 + 64 x reads, 0 to 128; the emulator moves a stick
// off the centre by one read at least), so 2 reads a step: 32 steps from the centre to each end
const PRIV_JOYA = joyCalibration([0, 0, 64, 64, 128, 128, 32, 32, 2, 2, 2, 2, 64, 64, 64, 64, 140]);
// Armada times the port; the emulator gives it the readings itself, exact (src/cpu/wcnet_armada.cpp joy_read): 0, 321
// the centre, 642, 10 a step, the band 320 to 322 (its first word, 11, and the throttle's zeros as the game writes them)
const ARMADA_JOYA = joyCalibration([11, 0, 0, 321, 321, 642, 642, 32, 32, 10, 10, 10, 10, 322, 322, 320, 320, 0, 0, 0, 0, 0]);
function privPrepare(FS, root, opts = {}) {
  const said = [privMusicOnFm(FS, root)].filter(Boolean);
  if (opts.joystick) {
    FS.writeFile(`${root}/JOYA.DAT`, PRIV_JOYA);
    said.push("JOYA.DAT: the built-in stick calibrated for the page's port, no dead band of the game's");
  }
  return said.join("; ");
}
function armadaPrepare(FS, root, opts = {}) {
  if (!opts.joystick) return "";
  FS.writeFile(`${root}/JOYA.DAT`, ARMADA_JOYA);
  return "Armada's JOYA.DAT: the built-in stick calibrated (no calibration in the game's Options needed), no dead band of the game's";
}

// Wing Commander's sound card is the word after "v" in WINGCMDR.CFG (CRUSADE.CFG for SM2.EXE): "a904" the AdLib at
// 388h, the Sound Blaster's FM, as GOG's Legacy Edition has it ("v a904 z"); "r" the Roland MT-32, as the 35th
// Anniversary Edition's DOS game has it ("v r z": GOG's own DOSBox there plays it through a synthesizer; the browser
// has none: no music at all).  A Roland setting is changed to the Legacy Edition's line, byte for byte.
function wc1MusicOnFm(FS, root) {
  const said = [];
  for (const name of ["WINGCMDR.CFG", "CRUSADE.CFG"]) {
    const path = `${root}/${name}`;
    let text;
    try { text = new TextDecoder().decode(FS.readFile(path)); } catch (e) { continue; }
    if (!text.replace(/\0/g, "").trim().split(/\s+/).includes("r")) continue;
    FS.writeFile(path, new TextEncoder().encode("v a904 z\r\n\0"));
    said.push(name);
  }
  return said.length ? `Wing Commander's music: the Roland MT-32 of ${said.join(" and ")} (no synthesizer in the browser) changed to the AdLib, the Sound Blaster's FM` : "";
}

// Terminal Velocity's shareware (3dtv12.zip, 3D Realms' BBS release) holds an installer, INSTALL.EXE, and the game in
// pieces, TV12BBS._1, ._2, ._3: put end to end they are a zip (the installer's own header before it) of the game's
// files, which INSTALL.EXE unpacks to C:\TV.  expandSplitZips does the same in the browser: every set of pieces
// <name>._1, ._2, ... (any case, the same directory) that reads as a zip is replaced by the files in it.
export async function expandSplitZips(files) {
  const groups = new Map();
  for (const f of files) {
    const m = /^(.*)\._(\d+)$/.exec(f.path);
    if (!m) continue;
    const key = m[1].toLowerCase();
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push({ n: Number(m[2]), f });
  }
  let out = files;
  for (const [, parts] of groups) {
    parts.sort((a, b) => a.n - b.n);
    if (parts[0].n !== 1 || parts.some((p, i) => p.n !== i + 1)) continue;
    const size = parts.reduce((t, p) => t + p.f.data.length, 0);
    const whole = new Uint8Array(size);
    let at = 0;
    for (const p of parts) { whole.set(p.f.data, at); at += p.f.data.length; }
    let inside;
    try { inside = await readZip(whole); } catch (e) { continue; }
    const dirOf = parts[0].f.path.includes("/") ? parts[0].f.path.slice(0, parts[0].f.path.lastIndexOf("/") + 1) : "";
    const drop = new Set(parts.map((p) => p.f));
    out = out.filter((f) => !drop.has(f)).concat(inside.map((f) => ({ path: dirOf + f.path, data: f.data })));
  }
  // (Raptor's shareware, 1rap12.zip: its installer and RAPSW12.SHR, one zip of the game's files: replaced the same way)
  for (const f of out.slice()) {
    if (!/\.shr$/i.test(f.path)) continue;
    let inside;
    try { inside = await readZip(f.data); } catch (e) { continue; }
    const dirOf = f.path.includes("/") ? f.path.slice(0, f.path.lastIndexOf("/") + 1) : "";
    out = out.filter((g) => g !== f).concat(inside.map((g) => ({ path: dirOf + g.path, data: g.data })));
  }
  // (the TIE Fighter demo: its two floppies, whose TIEDEMO.001 and TIEDEMO.002 hold the game's files compressed, as
  // folders (the Internet Archive's TIE_Fighter_Preview__Dodge_Neon.zip: disk1/, disk2/, the first release with the
  // Dodge Neon's showroom) or as zips of their own (DOS Games Archive's tiedemo.zip: tiedem1.zip, tiedem2.zip, a
  // later repack without it); unpacked as its installer does (web/pvm.js), the floppies' own files left out but for
  // the READ.ME and the FLIGHT.HLP)
  for (const f of out.slice()) {
    if (!/(^|\/)tiedem\d\.zip$/i.test(f.path)) continue;
    let inside;
    try { inside = await readZip(f.data); } catch (e) { continue; }
    const dirOf = f.path.includes("/") ? f.path.slice(0, f.path.lastIndexOf("/") + 1) : "";
    out = out.filter((g) => g !== f).concat(inside.map((g) => ({ path: dirOf + g.path, data: g.data })));
  }
  const v1 = out.find((f) => /(^|\/)tiedemo\.001$/i.test(f.path)), v2 = out.find((f) => /(^|\/)tiedemo\.002$/i.test(f.path));
  if (v1 && v2) {
    const dirOf = v1.path.includes("/") ? v1.path.slice(0, v1.path.lastIndexOf("/") + 1) : "";
    const dir2 = v2.path.includes("/") ? v2.path.slice(0, v2.path.lastIndexOf("/") + 1) : "";
    const files = unpackPvm([v1.data, v2.data], dirOf);
    const floppy = /(^|\/)(tiedemo\.00\d|tiedemo\.inf|disk\.id|install\.exe|detect\.exe)$/i;
    const inDir = (f, d) => f.path.startsWith(d) && !f.path.slice(d.length).includes("/");    // (directly in it)
    out = out.filter((f) => !floppy.test(f.path) || !(inDir(f, dirOf) || inDir(f, dir2))).concat(files);
  }
  return out;
}

// The TIE Fighter demo's own settings, as its installer writes them when there are none (docs/tiedemo.md):
// LANDRU.CFG, the sound (the music on the AdLib, the effects and the voices on a Sound Blaster at 220: "adlib_w", its
// ADLIB_W.IMS), the machine's speed (FAST), "ems" and "diff"; OPTIONS.CFG, the in-flight options of a FAST machine with
// the digitized sound on (the installer's FASTYES.CFG).
function tdPrepare(FS, root) {
  const said = [];
  const has = (name) => { try { FS.readFile(`${root}/${name}`); return true; } catch (e) { return false; } };
  if (!has("LANDRU.CFG")) {
    FS.writeFile(`${root}/LANDRU.CFG`, new TextEncoder().encode("adlib_w\r\nFAST\r\nems\r\ndiff\r\n"));
    said.push("LANDRU.CFG (the AdLib's music, the Sound Blaster's effects, a fast machine)");
  }
  if (!has("OPTIONS.CFG") && has("FASTYES.CFG")) {
    FS.writeFile(`${root}/OPTIONS.CFG`, FS.readFile(`${root}/FASTYES.CFG`));
    said.push("OPTIONS.CFG (the installer's FASTYES.CFG)");
  }
  return said.length ? `The TIE Fighter demo's settings, as its installer writes them: ${said.join(", ")}` : "";
}

// X-Wing Collector's CD-ROM (docs/xwing.md).  Its CD: an image among the game's files (GOG's game.gog, an .iso, a
// .bin of raw sectors: the largest such file of 10 MB and more), or the disc's own files in a folder (the one that
// holds MISSION\*.XWI: a copy of the CD beside the installed game, or the CD's own top, where the installed game's
// files are too).
function xwDisc(files) {
  let image = null;
  for (const f of files) {
    if (!/\.(iso|gog|bin|img)$/i.test(f.path) || f.path.split("/").length > 2) continue;
    const size = f.size !== undefined ? f.size : (f.data ? f.data.length : 0);
    if (size >= 10e6 && (!image || size > image.size)) image = { path: f.path, size };
  }
  if (image) return { image: image.path };
  let folder = null;
  for (const f of files) {
    const m = /^(?:(.*)\/)?mission\/[^/]+\.xwi$/i.exec(f.path);
    if (m && (folder === null || (m[1] || "").length < folder.length)) folder = m[1] || "";
  }
  return folder === null ? null : { folder };
}
// Its settings, as its installer leaves them for a fast machine with a Sound Blaster 16: LANDRU.CFG ("fast", "ems",
// "diff"); SETMUSE.INI's two drivers (the file's first 28 bytes: the music's driver, then the digitized sound's, 14
// bytes each) FM4.MDR, the OPL3's, and SB16.WDR, whatever card the copy was set up for (a General MIDI or a Roland
// has no synthesizer here; SETMUSE.NON, the installer's blank, when there is no SETMUSE.INI); OPTIONS.CFG when there
// is none (FASTYES.CFG with the cockpit on: its byte 10).
function xwPrepare(FS, root) {
  const said = [];
  const named = (want) => { try { return FS.readdir(root).find((n) => n.toUpperCase() === want) || null; } catch (e) { return null; } };
  const read = (want) => { const n = named(want); try { return n ? FS.readFile(`${root}/${n}`) : null; } catch (e) { return null; } };
  const write = (want, data) => FS.writeFile(`${root}/${named(want) || want}`, data);
  const landru = "fast\r\nems\r\ndiff\r\n";
  const had = read("LANDRU.CFG");
  if (!had || new TextDecoder("latin1").decode(had) !== landru) {
    write("LANDRU.CFG", new TextEncoder().encode(landru));
    said.push("LANDRU.CFG (a fast machine, EMS)");
  }
  const muse = read("SETMUSE.INI") || read("SETMUSE.NON");
  if (muse && muse.length >= 28) {
    const out = muse.slice();
    const put = (at, name) => { for (let i = 0; i < 14; i++) out[at + i] = i < name.length ? name.charCodeAt(i) : 0; };
    const was = (at) => { let t = ""; for (let i = 0; i < 14 && muse[at + i]; i++) t += String.fromCharCode(muse[at + i]); return t; };
    if (was(0) !== "FM4.MDR" || was(14) !== "SB16.WDR" || !named("SETMUSE.INI")) {
      put(0, "FM4.MDR");
      put(14, "SB16.WDR");
      write("SETMUSE.INI", out);
      said.push(`SETMUSE.INI (the music ${was(0) || "none"} -> FM4.MDR, the sound ${was(14) || "none"} -> SB16.WDR)`);
    }
  }
  if (!named("OPTIONS.CFG")) {
    const fast = read("FASTYES.CFG");
    if (fast && fast.length > 10) {
      const out = fast.slice();
      out[10] = 1;
      write("OPTIONS.CFG", out);
      said.push("OPTIONS.CFG (the installer's FASTYES.CFG)");
    }
  }
  return said.length ? `X-Wing's settings: ${said.join(", ")}` : "";
}

// Terminal Velocity's own settings made for the browser at each launch (src/cpu/wcnet_tv.cpp; docs/terminalvelocity.md):
// the Sound Blaster 16's DIGPAK driver as SOUNDRV.COM (SETUP.EXE's Sound Setup copies the card's driver there; TV.EXE
// loads it, the music and the effects both through it), SETUP.CFG as SETUP.EXE's "Save and launch TV" writes it (TV.EXE
// asks for setup without one), and in VOX.INI (the game's settings, written by the game) the high-resolution mode (the
// game's own "Res: SVGA": 640 x 240 drawn on a 640 x 480 screen; 640,480 there breaks the terrain) and the control:
// with a pad picked "Built in" (opts.joystick: the game port has it), the game's Gamepad control (2) calibrated to
// DOSBox's port as the game's own Calibrate measures it (centre 64, 0 to 128); else a Gamepad control left from an
// earlier launch goes back to the keyboard (the port is empty), the mouse kept.
const TV_SETUP_CFG = [";Setup File for TV", ";Version 1.2", "", "", "[Screen Modes]", "ScreenMode = 0", "", "", "[Sound Setup]",
  "FXDevice = 0", "MusicDevice = 0", "FXVolume = 128", "MusicVolume = 128", "", "", "[Comm Setup]", "ComPort = 2", "IrqNumber = ~",
  "UartAddress = ~", "PortSpeed = 9600", "ToneDial = 1", "SocketNumber = ~", "NumberPlayers = 2", 'InitString = "ATZ"',
  'HangupString = "ATH0"', 'DialoutString = ""', "",
  ...Array.from({ length: 10 }, (_, i) => `CommbatMacro#${i} = "No Macro #${i + 1}"`), "",
  ...Array.from({ length: 10 }, (_, i) => [`PhoneName#${i} = ""`, `PhoneNumber#${i} = ""`]).flat(), ""].join("\r\n");
const TV_VOX_INI = ["musicFlag", "1", "soundFlag", "1", "mixSpeed", "11025", "skyTextureFlag", "1",
  "joystickActive,xcenter,ycenter,throttleMin,throttleMax", "0,0,0,0,0", "perspectiveFlag,airShadowFlag", "1,2",
  "gamePIXX,gamePIXY,gameX1,gameY1,gameX2,gameY2", "640,240,0,0,0,0", "localBusVideo", "1", "aspectRatio,flipVideo", "0,0",
  "textureResolution", "1", "difficulty,briefingFlag,cinemaFlag,showDemoFlag", "1,1,1,1",
  "xStickMin,yStickMin,xStickMax,yStickMax", "0,0,0,0", "notebookStickFlag", "0", "stereoFlag,musicVolume,soundVolume",
  "0,65536,65536", "weaponKey,fireKey,leftKey,rightKey,upKey,downKey,throtUp,throtDown", "56,57,75,77,72,80,30,44",
  "buttonFire,buttonWeapon,buttonThrottleUp,buttonThrottleDown", "1,2,8,4", "boomZoom", "65536",
  "autoLevel,mouseControl,rollLeftKey,rollRightKey,partsFlag,mouseFlip", "1,0,71,73,1,1",
  "mouseSenseX,mouseSenseY,mouseMotion,mouseLeftButton,mouseRightButton", "8,16,1,0,1", ""].join("\r\n");
function tvPrepare(FS, root, opts = {}) {
  const said = [];
  const read = (name) => { try { return FS.readFile(`${root}/${name}`); } catch (e) { return null; } };
  const has = (name) => read(name) !== null;
  if (!has("SOUNDRV.COM")) {
    const sb = read("SB16.COM");
    if (sb) { FS.writeFile(`${root}/SOUNDRV.COM`, sb); said.push("the Sound Blaster 16's driver as SOUNDRV.COM"); }
  }
  if (!has("SETUP.CFG")) { FS.writeFile(`${root}/SETUP.CFG`, new TextEncoder().encode(TV_SETUP_CFG)); said.push("SETUP.CFG"); }
  // (the registered game's cinematics without the line that says where they are: an archive that lost CDROM.INI)
  if (has("CDROM.POD") && !has("CDROM.INI")) {
    let tvi = false;
    try { tvi = FS.readdir(`${root}/TVI`).some((n) => /\.TVI$/i.test(n)); } catch (e) { /* no cinematics */ }
    if (tvi) { FS.writeFile(`${root}/CDROM.INI`, new TextEncoder().encode(".\\TVI")); said.push("CDROM.INI (the cinematics in TVI)"); }
  }
  const vox = read("VOX.INI");
  const lines = new TextDecoder().decode(vox || new TextEncoder().encode(TV_VOX_INI)).split(/\r?\n/);
  const set = (head, fn) => {
    const i = lines.findIndex((l) => l.startsWith(head));
    if (i >= 0 && i + 1 < lines.length) lines[i + 1] = fn(lines[i + 1].split(",")).join(",");
  };
  set("gamePIXX,", (v) => { v[0] = "640"; v[1] = "240"; return v; });
  // the Graphics Options at their best (the user: "Let's ensure detail is maximum"): the sky's texture, Image Quality
  // Pentium (perspective), Shadows Pentium, Texture High Detail (12 MB free: the machine has 32); but Debris Normal
  // (the user, 2026-11-16: "can you set debris to be normal but keep shadows max", "max debris looks bad")
  set("skyTextureFlag", () => ["1"]);
  set("perspectiveFlag,", () => ["1", "2"]);
  set("textureResolution", () => ["1"]);
  set("autoLevel,", (v) => { if (v.length > 4) v[4] = "1"; return v; });
  let control = "";
  set("joystickActive,", (v) => {
    if (opts.joystick) { control = "the gamepad (built in)"; return ["2", "64", "64", v[3] || "0", v[4] || "0"]; }
    if (Number(v[0]) >= 2) { control = "the keyboard (no pad built in)"; v[0] = "0"; }
    return v;
  });
  if (opts.joystick) set("xStickMin,", () => ["0", "0", "128", "128"]);
  FS.writeFile(`${root}/VOX.INI`, new TextEncoder().encode(lines.join("\r\n")));
  said.push(`VOX.INI: 640 x 480, the best detail but Debris Normal${control ? ", " + control : ""}${vox ? "" : " (made)"}`);
  return `Terminal Velocity's settings: ${said.join("; ")}`;
}

// Raptor's own settings made for the browser at each launch (src/cpu/wcnet_rap.cpp; docs/raptor.md): SETUP.INI as
// SETUP.EXE writes it (RAP.EXE will not start without one) when there is none: the Sound Blaster for the music (its FM)
// and the effects, at the machine's 220, IRQ 7, DMA 1; the control the keyboard and the mouse.
const RAP_SETUP_INI = ["[Setup]", "Detail=1", "Control=0", "[Music]", "CardType=5", "BasePort=220", "Irq=7", "Dma=1",
  "Volume=127", "[SoundFX]", "CardType=5", "BasePort=220", "Irq=7", "Dma=1", "Channels=4", "Volume=127", ""].join("\r\n");
function rapPrepare(FS, root) {
  let ini = null;
  try { ini = new TextDecoder().decode(FS.readFile(`${root}/SETUP.INI`)); } catch (e) { /* none: made */ }
  if (ini === null) {
    FS.writeFile(`${root}/SETUP.INI`, new TextEncoder().encode(RAP_SETUP_INI));
    return "Raptor's settings: SETUP.INI (the Sound Blaster for the music and the effects, the keyboard and the mouse)";
  }
  // A SETUP.INI that came with the game (Steam's 1994 Classic Edition: the music on General MIDI at port 330, which
  // the browser has no synthesizer for; the effects on the Sound Blaster): its [Music] and [SoundFX] cards made the
  // machine's Sound Blaster (220, IRQ 7, DMA 1), everything else (the keys, the volumes, the detail) kept.
  const lines = ini.split(/\r?\n/);
  const section = (name) => {
    const a = lines.findIndex((l) => l.trim().toLowerCase() === `[${name.toLowerCase()}]`);
    if (a < 0) return null;
    let b = lines.findIndex((l, i) => i > a && /^\s*\[/.test(l));
    if (b < 0) b = lines.length;
    return [a, b];
  };
  const value = (name, key) => {
    const r = section(name);
    if (!r) return null;
    for (let i = r[0] + 1; i < r[1]; i++) { const m = /^\s*(\w+)\s*=\s*(.*?)\s*$/.exec(lines[i]); if (m && m[1].toLowerCase() === key.toLowerCase()) return m[2]; }
    return null;
  };
  const put = (name, key, v) => {
    let r = section(name);
    if (!r) { lines.push(`[${name}]`); r = [lines.length - 1, lines.length]; }
    for (let i = r[0] + 1; i < r[1]; i++) { if (new RegExp(`^\\s*${key}\\s*=`, "i").test(lines[i])) { lines[i] = `${key}=${v}`; return; } }
    lines.splice(r[0] + 1, 0, `${key}=${v}`);
  };
  let said = "";
  for (const name of ["Music", "SoundFX"]) {
    if (value(name, "CardType") === "5" && value(name, "BasePort") === "220" && value(name, "Irq") === "7" && value(name, "Dma") === "1") continue;
    put(name, "CardType", "5"); put(name, "BasePort", "220"); put(name, "Irq", "7"); put(name, "Dma", "1");
    said += (said ? " and " : "") + (name === "Music" ? "the music" : "the effects");
  }
  if (!said) return "";
  FS.writeFile(`${root}/SETUP.INI`, new TextEncoder().encode(lines.join("\r\n")));
  return `Raptor's settings: ${said} on the Sound Blaster (the SETUP.INI that came with the game had another card)`;
}

const armadaMachine = {
  multiplayer: true, ipx: true, cycles: 25000, noFlight: true, noRoomOptions: true, joystick: true, prepare: armadaPrepare,
  dosbox: "[dosbox]\nmachine=svga_s3\nmemsize=16\n[cpu]\ncore=normal\n[dos]\nxms=true\nems=true\numb=true\n[sblaster]\nsbtype=sb16\nirq=7\ndma=1\nhdma=5\n",
  keys: "<b>Armada flight:</b> steer with the arrow keys (a tap turns a little; held, they build to the full turn in a third of a second; <kbd>Shift</kbd> with an arrow turns at full at once) or hold the right mouse button and drag (the further the drag, the faster the turn; let go to centre it; dragging up raises the nose unless <b>Invert mouse</b> is ticked). Fire guns with <kbd>Space</kbd> or the left button while steering with the mouse. <kbd>Enter</kbd> fires a missile, <kbd>Tab</kbd> is the afterburner, and the keypad’s <kbd>+</kbd> and <kbd>−</kbd> change speed. For a joystick or gamepad, pick its <b>Built in</b> entry under Controller before Fly: it is calibrated already (<kbd>Alt</kbd>+<kbd>O</kbd>, <b>JOY 1</b> recalibrates it if you want). <b>Strategy map:</b> the game’s own keys press its buttons: <kbd>F1</kbd> Menu, <kbd>F2</kbd> Move, <kbd>F3</kbd> Catalog, <kbd>F4</kbd> Dock, <kbd>F5</kbd> Planet menu, <kbd>F6</kbd> Move resources, <kbd>F7</kbd> Information, <kbd>F8</kbd> Execute (it asks first); in a dialog <kbd>C</kbd> is Cancel, <kbd>D</kbd> Done, and <kbd>A</kbd> All when moving a squadron. The arrow keys move the map. <kbd>+</kbd> and <kbd>−</kbd> (the main keys, not the keypad’s) work the arrows of a dialog’s top row (other rows by clicking): in Move resources a ton onto or off the ship, in Dock one fighter docked or undocked, moving a squadron one more going or staying; hold them to keep going. <b>High resolution (2x)</b> draws the flight at 640×400 and the strategy map one level out, four times as much of the sector at once; the mouse wheel steps between the game’s own map, one level out and further out still (nine times the game’s own), about the pointer (<kbd>`</kbd> flips high resolution).",
};

export const GAMES = [
  { id: "wc1", tag: "WC1", title: "Wing Commander", detect: ["WC.EXE"], run: "wc", multiplayer: true,
    build: { para: 0x1231, off: 0x0187, text: "Loading WING COMMANDER" },
    saves: ["GAMEDAT/SAVEGAME.WLD"], cycles: 3630, fps: WC1_FPS, capHull: true, joystick: true, prepare: wc1MusicOnFm,
    pointer: { fromGame: true },
    campaign: { series: WC1_SERIES, hints: WC1_HINTS } },
  // SM2.EXE is the game's last build, with the Crusade campaign (it can
  // load the other two from a saved game), and keeps its saved games in a
  // file of its own.  The hooks know it as they know WC.EXE; a picked
  // mission is one of Crusade's.
  { id: "wc1sm2", tag: "SM2", partOf: "wc1", title: "Wing Commander: The Secret Missions 2", detect: ["SM2.EXE"], run: "sm2", multiplayer: true,
    build: { para: 0x11E8, off: 0x0181, text: "Loading WING COMMANDER" },
    saves: ["GAMEDAT/CRUSADE.WLD"], cycles: 3630, fps: WC1_FPS, capHull: true, joystick: true, prepare: wc1MusicOnFm,
    pointer: { fromGame: true },
    campaign: { series: SM2_SERIES, hints: WC1_HINTS } },
  // A picked mission of Wing Commander II starts in the barracks (a
  // different room from base to base: the door that flies the mission is
  // not always in the same place).
  // "loadfix -34" is how GOG starts it, and it matters: loaded lower in
  // memory the game jumps through a null pointer in some in-flight scenes
  // (after the first autopilot of series 2 mission 2, at the start of
  // others) and hangs.
  // Its sound setup (wc2.cfg, "c25": a Sound Blaster at 220, IRQ 5) is GOG's,
  // and so must the emulated card's be: with DOSBox's IRQ 7 the first spoken
  // line never ends, and the game waits for it for ever.
  { id: "wc2", tag: "WC2", title: "Wing Commander II", detect: ["WC2.EXE"], run: "loadfix -34 wc2", multiplayer: true,
    build: { para: 0x1976, off: 0x8D65, text: "Origin" },
    saves: ["GAMEDAT/SAVEGAME.WC2"], cycles: 8000, joystick: true,
    firstName: true,
    pointer: { fromGame: true },
    dosbox: "[sblaster]\nirq=5\n",
    campaign: { series: WC2_SERIES, missionArgs: wc2Mission, hints: WC2_HINTS } },
  // The Special Operations are programs of their own in the same directory,
  // started the way GOG's menu starts them.
  { id: "wc2so1", tag: "SO1", partOf: "wc2", title: "Wing Commander II: Special Operations 1", detect: ["SO1.EXE"], run: "loadfix -34 so1", multiplayer: true,
    build: { para: 0x1989, off: 0x0258, text: "Loading WC2 - SPECIAL OPERATIONS 1" },
    saves: ["GAMEDAT/SAVEGAME.SO1"], cycles: 8000, joystick: true,
    firstName: true,
    pointer: { fromGame: true },
    dosbox: "[sblaster]\nirq=5\n",
    campaign: { series: SO1_SERIES, missionArgs: wc2Mission, hints: WC2_HINTS } },
  { id: "wc2so2", tag: "SO2", partOf: "wc2", title: "Wing Commander II: Special Operations 2", detect: ["SO2.EXE"], run: "loadfix -34 so2", multiplayer: true,
    build: { para: 0x1924, off: 0x025A, text: "Loading WC2 - SPECIAL OPERATIONS 2" },
    saves: ["GAMEDAT/SAVEGAME.SO2"], cycles: 8000, joystick: true,
    firstName: true,
    pointer: { fromGame: true },
    dosbox: "[sblaster]\nirq=5\n",
    campaign: { series: SO2_SERIES, missionArgs: wc2Mission, hints: WC2_HINTS } },
  // Special Operations 3 is not in anyone's game directory: the page makes
  // it from Special Operations 2 (its program with the data-file number
  // changed) and the sealed package web/so3/ (so3/README.md), which opens
  // with a key derived from the player's own SERIES.S02.  The program is
  // SO2.EXE's build, so the hooks' test is SO2.EXE's.
  { id: "wc2so3", tag: "SO3", partOf: "wc2", title: "Wing Commander II: Special Operations 3 (Culloden)", detect: ["SO2.EXE"], run: "loadfix -34 so3", multiplayer: true,
    addon: "so3",
    build: { para: 0x1924, off: 0x025A, text: "Loading WC2 - SPECIAL OPERATIONS 2" },
    saves: ["GAMEDAT/SAVEGAME.SO3"], cycles: 8000, joystick: true,
    firstName: true,
    pointer: { fromGame: true },
    dosbox: "[sblaster]\nirq=5\n",
    campaign: { series: SO3_SERIES, missionArgs: wc2Mission, hints: WC2_HINTS } },
  // RetroVega (retrovega/ in dosbox3d): our own game in the Vega Strike
  // universe, open data, built into the page.  It wants 32 MB of XMS, VESA
  // 640x400 and the Sound Blaster's OPL3.  Its saved games are the eight
  // bunks.  Flown together as Wing Commander is (retrovega/docs/multiplayer.md).
  { id: "rv", tag: "RV", title: "RetroVega", detect: ["RV.EXE"], run: "rv /noquit", multiplayer: true,
    builtin: "retrovega.tar.gz", fps: RV_FPS, campaign: RV_CAMPAIGN, ownRocks: true,
    build: { rvhook: true, off: 0x0400, text: "Loading RETROVEGA" },
    saves: ["BUNK01.SAV", "BUNK02.SAV", "BUNK03.SAV", "BUNK04.SAV", "BUNK05.SAV", "BUNK06.SAV", "BUNK07.SAV", "BUNK08.SAV",
            "RVSETUP.CFG"],   // (its settings: the talk speed)
    // (its difficulty, the bunk menu's DIFFICULTY row, the page's Difficulty menu in its Collide's place: RVSETUP.CFG's
    // ninth byte, read by the game as a flight begins; asteroids always on, wing bumps always harmless; the user,
    // 2026-11-07: "For RetroVega remove the current options from the dropdown and just have easy medium hard. player
    // collisions should always be off and asteroids should always be on with full damage")
    difficulty: { file: "RVSETUP.CFG", at: 8, magic: "RVCFG1", values: { easy: 1, medium: 0, hard: 2 },
                  fresh: [0x52, 0x56, 0x43, 0x46, 0x47, 0x31, 1, 0, 0, 0] },
    // (50000: the flight's frame costs that many; RV.EXE halts while it waits for its next tick, so a
    // machine spends only what the game uses; the browser's emulator does ~70000 on a 2016 desktop core)
    cycles: 50000,
    pointer: { fromGame: true },       // (the middle of the cockpit's opening, another in every ship: wc_web_steer)
    // the flight's keys, under the picture (the game listed them on a VDU till the user had them moved out of it)
    keys: "<b>RetroVega in flight:</b> the mouse or the arrow keys steer (a tap turns a little, held they build to the full turn in a third of a second; <kbd>Shift</kbd> with an arrow turns at full at once), <kbd>Q</kbd> and <kbd>E</kbd> (or <kbd>,</kbd> and <kbd>.</kbd>, or the keypad's <kbd>Ins</kbd> and <kbd>Del</kbd>; a controller's roll axis too) roll (the same way), <kbd>Space</kbd> or the left button fires the guns, <kbd>Enter</kbd> a missile, <kbd>Tab</kbd> (or the keypad's <kbd>*</kbd>) the afterburner; <kbd>+</kbd> and <kbd>-</kbd> set the speed, <kbd>F</kbd> or <kbd>\\</kbd> full, <kbd>S</kbd> or <kbd>Backspace</kbd> stop; <kbd>T</kbd> the next target in front of you, <kbd>Y</kbd> the nearest enemy, <kbd>L</kbd> locks the target; <kbd>G</kbd> picks the guns, <kbd>W</kbd> the missile (a seeker's lock: an amber octagon sets off from the bottom corner farther from the target and chases its box, white once it holds on it), <kbd>D</kbd> the damage readout; <kbd>N</kbd> the nav points (a white cross marks the one chosen), <kbd>A</kbd> the autopilot; <kbd>`</kbd> or <kbd>~</kbd> the ships between high detail and blocks, at once; <kbd>C</kbd> the comm menu: <kbd>1</kbd> your wingman (fly in formation, break formation, attack your target, help you out, return to base), <kbd>2</kbd> your target (taunt an enemy; with the carrier as your target, request landing within 25,000 km, granted once you have flown a nav point or finished the mission, then come within 2,000 km of it, afterburner off, to land), <kbd>9</kbd> never mind; at once: <kbd>C</kbd> three times quickly (or <kbd>Alt</kbd>+<kbd>T</kbd>) taunts your target; <kbd>Alt</kbd>+<kbd>C</kbd> cloaks a Schroedinger (half seen, no enemy finds you, the shields down; the afterburner or talking shows you while it lasts, a shot or <kbd>Alt</kbd>+<kbd>C</kbd> ends it); flying with others, <kbd>0</kbd> talks to them (Shift does not). Home with every nav point flown and no enemy about, <b>PRESS FIRE TO LAND</b> shows under the gunsight: the trigger lands you; <kbd>R</kbd> the radar's range; <kbd>F5</kbd> the outside view, <kbd>F1</kbd> the cockpit. Low over a planet you fly in its air, the more the lower (none above 6,000 km): the ship sinks when slow, keeps its speed in level flight, loses it in a climb and gains it in a dive; near the ground the throttle alone will not carry it straight up, the afterburner will; the wings level themselves when you do not roll; fast and low, the hull glows. The simulator in the bar (click its chair) is Rlaan after Rlaan, on your own: <kbd>Esc</kbd> or <kbd>Caps Lock</kbd> leaves it.",
    dosbox: "[dosbox]\nmemsize=63\n[dos]\nxms=true\nems=false\numb=true\n[sblaster]\nsbtype=sb16\noplmode=opl3\n" },
  // TIE Fighter (LucasArts, 1995: the Collector's CD-ROM as GOG sells it).
  // TIE.EXE starts Z_TIE__.EXE, a 32-bit program under DOS/4GW; the disc
  // (game.gog: the voices, and the check the game makes before anything
  // else) must be in drive D, or the game quits at once.  Its 594 MB go into
  // the browser's cache in parts (saveGame).  The machine is the one GOG's
  // own configuration asks for (an S3 card for its 640x480 flight, 16 MB,
  // EMS for the extender, a Sound Blaster 16 on IRQ 7).  Single-player
  // until its hooks are built (docs/tiefighter.md).
  // Rooms of up to 4 (`maxSeats`; the Players box shows 3 and 4 as beta): every pilot takes a craft of the player's
  // flight group, the group made as big as the room (src/cpu/wcnet_tie_session.cpp).
  { id: "tf", tag: "TF", title: "TIE Fighter", detect: ["TIE.EXE"], run: "tie", multiplayer: true, maxSeats: 4,
    // (cycles: DOSBox's automatic ones up to 60,000, the user: "the sound was very choppy at the base until I hit
    // CNTRL-F11 three times"; measured in headless Chrome, a fixed 60,000 ran the concourse at 75% of real time and the
    // flight at 86 to 88%, the sound starved; automatic, 100% everywhere at some 32,000 to 55,000)
    image: "game.gog", keep: ["IMUSE.INI", ...TF_ICONS], cycles: 60000, cyclesAuto: true, campaign: TF_CAMPAIGN, noRoomOptions: true, screen: [640, 480], prepare: tfPrepare,
    // (the launcher's own words: the hooks find the game's program in memory by its bytes, not by this file)
    build: { text: "Loading TIE-Fighter CD" },
    help: "TIE Fighter flown together, up to 4 pilots (3 and 4 in beta): everybody in the room flies the same mission, each in a craft of the player's flight group (or the craft that pairs with it, for a mission flown alone), and sees the others where they really are. The craft near a pilot are flown by that pilot's machine, the rest by the host's; every shot is seen by everybody, a hit is decided by the shooter's machine and applied by the machine flying the craft hit, and a kill counts for the pilot who made it. A pilot down or the host's flight over ends the flight for everybody, mission failed: Fly Mission Again at the debriefing. A callsign with DEMO in it gives that pilot's craft to a wingman that flies itself (stays clear of mines and capital ships, attacks what attacks the other pilot or what it targets, else keeps to its wing): two-player tests with one person. The host decides: a guest's launch waits until the host flies, and the ship's PA calls a guest to the briefing. In a room the Combat Chamber's T/F row is the Z-95 challenge: two Z-95 Headhunters, hopelessly outgunned, flying the gunboat's and the T/I's missions against the Empire's own craft, the radio speaking for the other side in the game's own lines and voices. The host may pick a mission for everybody in the room's menu, any of the Combat Chamber's and any battle's: launching any Combat Chamber mission then flies it, briefing and all (the host may change it between flights). With none picked, a guest's Combat Chamber launch flies the host's (the page's log says where the others are; over the game only the PA's call, the pilots' words and a launch waiting for the host). The tour's own battles stay each pilot's own. Space skips the opening; at the registration desk type a pilot's name (or click one) and take the door; in flight the mouse is the stick and its button (or Space) fires, Backspace is full throttle, and in flight 0 opens the comms prompt (Enter sends the line to the room). What is done and what comes next: docs/tiefighter.md.",
    // the flight's controls as the emulator gives them (src/cpu/wcnet_tie_controls.cpp; the "Flight" choice beside Controller)
    keys: "<b>TIE Fighter in flight:</b> the arrow keys steer (a tap turns a little, held they build to the full turn in a third of a second; <kbd>Shift</kbd> with an arrow turns at full at once; <kbd>Up</kbd> dives as a stick pushed forward), the keypad's <kbd>Ins</kbd> and <kbd>Del</kbd> roll. The mouse, as the <b>Flight</b> choice beside Controller has it: <i>point to aim</i> (a ring shows the pointer; the middle is still, the further the pointer the faster the turn, as in Wing Commander and RetroVega; the craft turns and climbs without banking, and the right button held with the mouse moved across rolls), <i>point to aim, banking</i> (the same with the game's own bank into each turn), <i>drag to steer</i> (the game's own, 1994), or nothing (the keyboard alone, or a joystick: a controller picked above, which also rolls and fires); <kbd>F12</kbd> in flight switches between the first four. <kbd>Space</kbd> or the left button fires (Space is still the game's when it asks for it: <kbd>Shift</kbd>+<kbd>S</kbd> then <kbd>Space</kbd> calls reinforcements); <kbd>0</kbd> opens the comms prompt in flight, <kbd>Enter</kbd> sends; <kbd>W</kbd> (held a moment) changes weapon, <kbd>Backspace</kbd> full throttle, <kbd>\\</kbd> none, <kbd>[</kbd> and <kbd>]</kbd> a third and two thirds; <kbd>T</kbd> and <kbd>R</kbd> target; <kbd>Esc</kbd> the game's options.",
    dosbox: "[dosbox]\nmachine=svga_s3\nmemsize=16\n[dos]\nxms=true\nems=true\numb=true\n[sblaster]\nsbtype=sb16\nirq=7\ndma=1\nhdma=5\n" },
  // Armada uses NetBIOS over IPX. GOG includes Novell's DOS driver beside both programs.
  { ...armadaMachine, id: "armada", tag: "ARM", title: "Wing Commander: Armada", short: "Armada",
    detect: ["ARMADA.EXE"], build: { text: "Loading Wing Commander Armada...." }, run: "armada", maxSeats: 2,
    // (the saved games: SAVE_1A#.ARM Armada mode, SAVE_1C#.ARM the campaign, slot #; kept by this browser, the page's
    // Download and Load buttons; the user, 2026-11-23: "there's no "download saves" button so I can't retrieve it")
    saves: ["SAVE_*.ARM"],
    netCommand: "netbios.exe", gather: [{ name: /^NETBIOS\.EXE$/i }],
    help: "Each player loads their GOG Armada installer or a zip of the installed DOS folder with NETBIOS.EXE. Host a room for two and share its code or link. After Fly starts both games, skip the intro with Esc and choose Multi Player, then Net Play. Accept the default group with Y and enter a different callsign on each machine. In Armada’s lobby the host clicks Create and picks A for strategy or B for Battle. The guest clicks Join, then the host clicks Play. For Battle, each pilot picks a ship and clicks Select, then Commit.",
  },
  { ...armadaMachine, id: "armadapg", tag: "PG", partOf: "armada", title: "Armada: Proving Grounds", short: "Proving Grounds",
    detect: ["PG.EXE"], build: { text: "Loading Wing Commander Armada::Proving Grounds" }, run: "pg", minSeats: 2, maxSeats: 6,
    help: "Load the Armada files and select Proving Grounds in the Game menu. Host a room for two to six and share its code or link. Press Fly, then choose IPX Net Play and enter different callsigns. Choose P when creating a Proving Grounds arena. During ship selection, T changes teams.",
  },
  // Wing Commander: Privateer (Origin, 1993: the CD-ROM edition as GOG sells it, Righteous Fire with it).  PRIV.EXE is
  // Origin's loader, PRCD.EXE the game (a real-mode program like the Wing Commanders, its world in EMS); the disc
  // (GAME.GOG: PRIV.TRE and RF.TRE, all of the game's data) must be in drive D.  The machine is the one GOG's own
  // configuration asks for (dosboxPRIV.conf: an S3, 16 MB, EMS and XMS, a Sound Blaster 16 on IRQ 5).  Saved games
  // are named by the player: *.SAV (Privateer) and *.PRS (Righteous Fire), kept by the page whatever their names.
  // Flown together loosely (docs/privateer.md: rooms of up to 8, `maxSeats`, everybody's own career, presence and the
  // others' ships near by so far;
  // src/cpu/wcnet_priv_session.cpp); the emulator ramps its arrow keys (src/cpu/wcnet_priv_controls.cpp).
  { id: "priv", tag: "PRV", title: "Wing Commander: Privateer", short: "Privateer", detect: ["PRCD.EXE"], run: "priv", multiplayer: true, maxSeats: 8,
    image: "GAME.GOG", cycles: 30000, saves: ["*.SAV"], noFlight: true, noRoomOptions: true, prepare: privPrepare, joystick: true,
    build: { para: 0x6E4D, off: 0x0074, text: "Type PRIV to run Privateer" },
    help: "Privateer flown together loosely, rooms of up to 8: everybody plays their own career, goes where they like and keeps their own saved games. Each pilot's system and place goes to the room when they launch, land, jump, engage the autopilot (A), and every 15 s; a line over the game says who is in your system and how far. Near each other you see each other's ships as the ships they fly, and a nav point's encounter is run by one pilot's game and shown to the others: everybody fights the same pirates, your hits count, and a wing about to spawn gets up to 3 more ships when several of you are there. A mission's nav points wait for the pilot who has the mission, and what flies with a pilot (an escort's charge) is seen by all. Talking: in flight press 0, type, Enter: the line shows on the others' message bar under your face (the Face box above, in the room), said by your ship when it is near; at a base use the room's chat beside this panel. Cargo you expel can be tractored by the others. Esc skips the opening; NEW registers a pilot (a name and a callsign, Enter after each); click your ship on the landing pad to launch. Save in a base with the Quine 4000's SAVE: the page keeps your saved games in this browser.",
    keys: "<b>Privateer in flight:</b> <kbd>`</kbd> flips the high-resolution picture (unless Voice is on push to talk, whose key it is); the arrow keys steer (a tap turns a little, held they build to the full turn in a third of a second; <kbd>Shift</kbd> with an arrow turns at full at once), <kbd>Home</kbd> and <kbd>PgUp</kbd> (the keypad's <kbd>7</kbd> and <kbd>9</kbd>) roll the same way; <kbd>Space</kbd> fires the guns, <kbd>Enter</kbd> the weapon picked with <kbd>W</kbd> (a missile, or the tractor beam), <kbd>Tab</kbd> the afterburner; <kbd>+</kbd> and <kbd>-</kbd> set the speed, <kbd>Backspace</kbd> stops; <kbd>T</kbd> targets, <kbd>N</kbd> the nav map, <kbd>A</kbd> the autopilot, <kbd>J</kbd> jumps, <kbd>X</kbd> expels your cargo, <kbd>C</kbd> the comms; <kbd>Alt</kbd>+<kbd>O</kbd> the options, where the mouse or a joystick can be the stick instead (the ramp is the keyboard's). A controller picked above steers in any of them; picked “Built in” before the launch, it is the game's own joystick. <b>One ship, crewed</b> (the host's Ships choice): the host flies; a guest in a turret turns it with the stick and fires with <kbd>Space</kbd>, and from any crew seat <kbd>S</kbd> evens the four shields, <kbd>Enter</kbd> moves shield power to the guns and <kbd>Tab</kbd> gun power to the shields (nothing made or lost); <kbd>F1</kbd> is the cockpit, where <kbd>C</kbd> works the comms for the ship, <kbd>F5</kbd> or <kbd>F6</kbd> back to the turret. A guest launches after the host does.",
    dosbox: "[dosbox]\nmachine=svga_s3\nmemsize=16\n[dos]\nxms=true\nems=true\numb=true\n[sblaster]\nsbtype=sb16\nirq=5\ndma=1\nhdma=5\n" },
  // Righteous Fire: the same program and disc, started as GOG's menu starts it ("priv r"), its saved games *.PRS.
  { id: "privrf", tag: "PRF", partOf: "priv", title: "Wing Commander: Privateer - Righteous Fire", detect: ["PRCD.EXE"], run: "priv r", multiplayer: true, maxSeats: 8,
    image: "GAME.GOG", cycles: 30000, saves: ["*.PRS"], noFlight: true, noRoomOptions: true, prepare: privPrepare, joystick: true,
    build: { para: 0x6E4D, off: 0x0074, text: "Type PRIV to run Privateer" },
    help: "Righteous Fire, Privateer's sequel campaign, flown together the way Privateer is (rooms of up to 8, each pilot its own career, where everybody is told). It starts from a pilot of its own (a new one, or a Privateer pilot who finished the game). The page keeps your saved games in this browser.",
    dosbox: "[dosbox]\nmachine=svga_s3\nmemsize=16\n[dos]\nxms=true\nems=true\numb=true\n[sblaster]\nsbtype=sb16\nirq=5\ndma=1\nhdma=5\n" },
  // Terminal Velocity (Terminal Reality / 3D Realms, 1995): the shareware zip as 3D Realms released it (its split
  // archive unpacked by expandSplitZips), the game's installed directory, or the registered game as GOG sells it
  // (setup_terminal_velocity_1.2_*.exe, the CD-ROM edition: three episodes in CDROM.POD, the cinematics in TVI\, where
  // CDROM.INI says they are: kept though an .ini; its GAME.EXE another compile of the same code, found by the same
  // signatures and one variant, scripts/tvsig.py).  TV.EXE loads the sound driver and runs
  // GAME.EXE, a 32-bit program bound with DOS/4G that the hooks find in memory by its code (src/cpu/wcnet_tv.cpp).
  // Flown together: the host's level for everybody, each pilot's ship in the others' games as the host's Craft menus
  // choose, hits and kills and the level's craft shared (docs/terminalvelocity.md).  The machine of its own setup: an
  // S3 for its 640 x 480 mode, a Sound Blaster 16 at 220, IRQ 5.  55,000 cycles: with its pixels drawn natively
  // (wcnet_tv_span.cpp) and its frames capped at 60 a second, two pages on one 5-core machine keep 100% of real time
  // at 54-55 frames a second (60,000: 97-98%, the same frames a wall second).
  { id: "tv", tag: "TV", title: "Terminal Velocity", detect: ["TV.EXE"], run: "tv", multiplayer: true, maxSeats: 4,
    build: { text: "Terminal Velocity Operating System" },
    cycles: 55000, screen: [640, 480], noFlight: true, noRoomOptions: true, joystick: true, prepare: tvPrepare,
    keep: ["CDROM.INI", "VOX.INI"],
    // (what the game reads, wherever the archive has it: the program proper, the archives, the episodes' lists, the
    // sound drivers, the settings; the registered game's cinematics belong in TVI\)
    gather: [{ name: /^(GAME|SETUP|COMMIT)\.EXE$/i }, { name: /\.(POD|VOX|COM|ADV)$/i }, { name: /^(SETUP\.CFG|VOX\.INI|CDROM\.INI)$/i },
             { name: /^SAVE.*\.DAT$|^SCORE\.DAT$/i }, { name: /\.TVI$/i, to: "TVI/" }],
    saves: ["SAVE*.DAT", "SCORE.DAT", "VOX.INI"],
    help: "Terminal Velocity flown together: the host plays the game as it comes (New Game, an episode, a difficulty) and everybody else flies the host's level with them: a guest's game waits for the host's launch, then flies the same level at the same difficulty. The shareware (episode one) and the registered game (all three episodes: drop GOG's installer or a zip of the game's folder) fly together in episode one; a shareware pilot waits out a level of the other episodes. Everybody sees everybody else's ship (the host picks each pilot's craft in the room: the TV-202, a craft of the game's own, or the level's own); every hit and kill counts in every game, and the level's craft are flown by the nearest pilot's machine. Whoever flies the exit ends the level for everybody (the statistics: Space for the next level). A pilot who dies can retry: the level is flown again as it stands, what was destroyed stays destroyed. The afterburner (held Alt, once you have fuel for it) shows to the others as a flame. Pick a controller \"Built in\" (above) for the game's own gamepad control.",
    keys: "<b>Terminal Velocity in flight:</b> the arrow keys steer, <kbd>A</kbd> and <kbd>Z</kbd> set the speed, <kbd>Space</kbd> fires, <kbd>1</kbd> to <kbd>7</kbd> pick a weapon, <kbd>Alt</kbd> held the afterburner (when you have picked one up), the keypad's <kbd>7</kbd> and <kbd>9</kbd> roll; <kbd>B</kbd> changes the view; <kbd>0</kbd> opens the comms prompt (<kbd>Enter</kbd> sends the line to the others' screens); <kbd>Esc</kbd> the game's menu. A controller picked \"Built in\": the stick steers, button 1 fires, button 2 the afterburner, buttons 3 and 4 the speed.",
    dosbox: "[dosbox]\nmachine=svga_s3\nmemsize=32\n[dos]\nxms=true\nems=true\numb=true\n[sblaster]\nsbtype=sb16\nirq=5\ndma=1\nhdma=5\n" },
  // Raptor: Call of the Shadows (Cygnus Studios / Apogee, 1994): the shareware zip as Apogee released it (1rap12.zip:
  // its installer and RAPSW12.SHR, unpacked by expandSplitZips) or the game's installed directory.  The registered game
  // (Steam's 1994 Classic Edition: its folder zipped, "Browse local files"; a Steam *backup* is chunks without file
  // names, no use) is another build of 1.2 that the same signatures find (scripts/rapsig.py checks both programs):
  // three sectors, FILE0002 to FILE0004.GLB; it will not start without its FILE_ID.DIZ.  RAP.EXE is a
  // 32-bit program bound with DOS/4GW that the hooks find in memory by its code (src/cpu/wcnet_rap.cpp).  Flown
  // together, up to four pilots: the host's wave for all, launched together; each pilot's own ship, weapons, money and
  // shop; the enemies' and the ground's damage shared, the money the killer's, each game's bonuses its own pilot's; a
  // pilot shot down the end for all; each ship's stripes its seat's colour (docs/raptor.md).  The game runs 23 frames a second by its own clock.
  { id: "rap", tag: "RAP", title: "Raptor: Call of the Shadows", short: "Raptor", detect: ["RAP.EXE"], run: "rap", multiplayer: true, maxSeats: 4,
    build: { text: "ENEMY_Think() - Invalid ANIMTYPE1" },
    cycles: 40000, screen: [320, 200], noFlight: true, noRoomOptions: true, prepare: rapPrepare,
    // (the game's archives -- FILE0000 and FILE0001 the shareware's, FILE0002 to FILE0004 the registered game's other
    // sectors -- FILE_ID.DIZ, which the registered game will not start without ("Critical file FILE_ID.DIZ missing
    // or modified"), the setup and the saved pilots, wherever the archive has them)
    keep: ["SETUP.INI"],
    gather: [{ name: /^FILE\d{4}\.GLB$/i }, { name: /^FILE_ID\.DIZ$/i }, { name: /^SETUP\.(EXE|INI)$/i }, { name: /^CHAR\d*\.FIL$/i }],
    saves: ["CHAR*.FIL", "SETUP.INI"],
    help: "Raptor flown together: each pilot plays a game of their own (a new pilot or a saved one, their own ship, weapons, money and shop), and the waves are flown together: when a pilot chooses a sector at the ship computer (the hangar's Fly Mission), their game waits there for the other, then both fly the host's wave. Both ships are on both screens (the other pilot's with blue engine flames, yours with the game's own, each casting its shadow); the enemies are the same, and what either pilot shoots down is shot down in both games: the money goes to whoever did it, and each pilot picks up their own bonuses. If either pilot is shot down, the mission is over for both. The shareware (the Bravo sector) and the registered game (all three sectors: a zip of the game's folder, from Steam's Browse local files) fly Bravo together; a shareware pilot cannot follow into the other two (Esc there flies their own wave alone).",
    keys: "<b>Raptor:</b> the arrow keys fly the ship (the mouse is for its menus), <kbd>Ctrl</kbd> fires, <kbd>Alt</kbd> fires the special weapon, <kbd>Space</kbd> picks the next special weapon, the right <kbd>Shift</kbd> the mega bomb; <kbd>Esc</kbd> aborts the mission (the other pilot flies on alone), <kbd>P</kbd> pauses. A controller picked above: the left stick or the D-pad flies the ship, A fires, B the special weapon, X the next special, Y the mega bomb, Start pauses, Back is Esc; in the menus the stick moves the pointer and A clicks (the D-pad and Start, Enter, work them as the keys do).",
    dosbox: "[dosbox]\nmachine=svga_s3\nmemsize=16\n[dos]\nxms=true\nems=true\numb=true\n[sblaster]\nsbtype=sb16\nirq=7\ndma=1\nhdma=5\n" },
  // The TIE Fighter demo (LucasArts, early 1994: one mission, flown in a build from before the game's release, given
  // away to advertise the Dodge Neon): its two floppies (the Internet Archive's TIE_Fighter_Preview__Dodge_Neon.zip,
  // the step 1 button's, fetched from its CORS address; or DOS Games Archive's tiedemo.zip dropped), unpacked by
  // expandSplitZips and web/pvm.js as its installer
  // does, or its installed directory.  The first release opens in the Neon's showroom (NEON.EXE: its introduction run
  // ahead by the layer, wcnet_tiedemo.cpp).  TIEDEMO.BAT runs the opening
  // (DEMO.EXE), the mission (TIEMISS.EXE: a 16-bit real-mode program, EMS for its music, sound and cockpits) and the
  // ending.  Flown together (src/cpu/wcnet_tiedemo*.cpp: the CD version's session, ported): launched together, each
  // pilot in its own craft of T/F Alpha (rooms of up to 3), the battle shared (each craft flown by one machine, the
  // others' copies following its reports), shots, hits and deaths shared, a pilot down the end for all.  After the
  // demo's own mission, three more (src/cpu/wcnet_tiedemo_campaign.cpp: written over the mission the game reads, the
  // layer's; the game's files unchanged): docs/tiedemo.md section 8.
  { id: "tiedemo", tag: "TFD", title: "TIE Fighter (the 1994 demo)", short: "TIE Fighter demo", detect: ["TIEMISS.EXE"], run: "tiedemo", multiplayer: true, maxSeats: 3,
    build: { text: "TIE Fighter Demo V1.0 12/04/93" },
    cycles: 30000, screen: [640, 400], noFlight: true, noRoomOptions: true, prepare: tdPrepare,
    saves: ["OPTIONS.CFG"],
    help: "The TIE Fighter demo of early 1994, the one given away with the Dodge Neon: a single mission (T/F Alpha against the Rebel repair yard at Remitik, the Rebels fleeing Hoth after), in a build from before the game's release. Here it is the first of four: win it and you fly on, the same game with new missions written in by this page: The Shuttles Run (TIE interceptors after a dozen fleeing shuttles, X-wings and Y-wings in the way), Corvette Hunt (TIE bombers with proton torpedoes against four corvettes) and Escort the Prowlers (TIE fighters against waves of Y-wings, and whatever comes after). A mission failed or a pilot down: that mission again. It opens in the Dodge Neon's showroom (click Go to game, or press any key but Esc: Esc there leaves the demo), then the opening plays (Esc skips it), then the missions, then the ending, and back to the showroom. The TIE Fighter demo button under step 1 fetches it from the Internet Archive once (TIE_Fighter_Preview__Dodge_Neon.zip, the preview's two floppies, 1.7 MB), installs it as its installer does and keeps it in this browser; its zip dropped on step 1 works too. Flown together, up to 3 pilots: everybody's mission starts at the same moment (the host's game waits at the end of the opening until everybody has got there), each pilot flies their own TIE fighter of T/F Alpha, and the battle is one battle: the same Rebels in the same places on every screen, every shot seen by everybody, every hit counted where it lands, a craft destroyed for one destroyed for all. If a pilot is shot down, the flight ends for everybody a moment later.",
    keys: "<b>TIE Fighter demo in flight:</b> the arrow keys steer (a tap turns a little, held they build to the game's own keyboard turn in a third of a second; <kbd>Shift</kbd> with an arrow is the whole stick at once, the mouse's sharpest turn; <kbd>Up</kbd> dives as a stick pushed forward), the keypad's <kbd>Ins</kbd> and <kbd>Del</kbd> roll. The mouse, as the <b>Flight</b> choice beside Controller has it: <i>point to aim</i> (the default: a ring shows the pointer; the middle is still, the further the pointer the faster the turn; the craft turns and climbs without banking, and the right button held with the mouse moved across rolls), <i>point to aim, banking</i> (the same with the game's own bank into each turn), <i>drag to steer</i> (the game's own, 1994: the right button held a moment with the mouse moved across rolls, a click picks a target), or nothing (the keyboard alone, or a joystick: a controller picked above, which also rolls and fires); <kbd>F12</kbd> in flight switches between the first four. <kbd>Space</kbd> or the left button fires; <kbd>+</kbd> and <kbd>-</kbd> the throttle, <kbd>Backspace</kbd> full, <kbd>\\</kbd> none, <kbd>[</kbd> and <kbd>]</kbd> a third and two thirds; <kbd>X</kbd> single, dual or quad fire, <kbd>F9</kbd> the lasers' recharge rate; <kbd>R</kbd> the nearest enemy fighter, <kbd>T</kbd> and <kbd>Y</kbd> the next and previous target, <kbd>U</kbd> the ship in the sight; <kbd>W</kbd> the bomber's proton torpedoes (and back to the lasers); <kbd>Space</kbd> is still the game's when the onboard computer suggests something (it approves); <kbd>Ctrl</kbd> the threat display; the keypad's digits look around. <kbd>Esc</kbd> is kept from the game (it would end the demo at once, back to DOS), so it only leaves full screen.",
    dosbox: "[dosbox]\nmachine=svga_s3\nmemsize=16\n[dos]\nxms=true\nems=true\numb=true\n[sblaster]\nsbtype=sb16\nirq=7\ndma=1\nhdma=5\n" },
  // X-Wing Collector's CD-ROM (LucasArts, 1994): the installed game with its CD (GOG's "X-Wing Special Edition": the
  // 1994 game's folder with game.gog, the disc; a zip of the installed folder with the CD's files or an image of it
  // inside; or the CD's image alone, whose top holds the installed game's files too: expandCdImages).  BWING.EXE is
  // a 16-bit real-mode program with overlays (BFRONT.OVL the front end, BFLIGHT.OVL the flight: the TIE Fighter demo's
  // engine in an earlier build), EMS; it looks for its CD on a drive (\BWING.EXE there).  Flown together
  // (src/cpu/wcnet_xw*.cpp, the TIE demo's session ported; docs/xwing.md): rooms of up to 4, every pilot through
  // its own front end to a mission, launched together into the HOST's mission, each in its own craft of the
  // player's flight group (made as large as the pilots flying), the battle shared, the host's flight over the end
  // for all, each pilot's result the host's.
  { id: "xwing", tag: "XW", title: "X-Wing (Collector's CD-ROM)", short: "X-Wing CD", detect: ["BWING.EXE"], run: "bwing", multiplayer: true, maxSeats: 4,
    build: { text: "Exiting X-Wing CD" },
    cycles: 30000, screen: [640, 400], noFlight: true, noRoomOptions: true, prepare: xwPrepare, disc: xwDisc,
    saves: ["*.PLT", "*.HGH", "OPTIONS.CFG"],
    help: "X-Wing (the 1994 Collector's CD-ROM) flown together, up to four pilots. Each pilot plays their own game: register a pilot, walk to Tour of Duty, Historical Combat or the Proving Ground, sit through the briefing and click out into space. Your launch then waits for the others; when everybody is there you all fly the HOST's mission, each in your own craft of the same flight group (wingmen are added when the mission has too few). Kills, damage and the mission's goals are shared; when the host hyperspaces home or is shot down the flight ends for everybody, and each pilot's debriefing shows the same result. The page keeps your pilots in this browser.",
    keys: "<b>X-Wing in flight:</b> the arrow keys steer (a tap turns a little, held they build to full in a third of a second; <kbd>Shift</kbd> with an arrow is the whole stick at once; <kbd>Up</kbd> dives as a stick pushed forward), the keypad's <kbd>Ins</kbd> and <kbd>Del</kbd> roll, <kbd>Space</kbd> fires (and confirms what the game asks you to confirm); the mouse steers as the Flight choice says (point to aim by default; <kbd>F12</kbd> changes it), its left button fires; <kbd>+</kbd> <kbd>-</kbd> <kbd>[</kbd> <kbd>]</kbd> <kbd>Backspace</kbd> <kbd>\\</kbd> set the throttle, <kbd>R</kbd> targets the nearest enemy, <kbd>T</kbd> <kbd>Y</kbd> the next and the one before, <kbd>W</kbd> picks the weapon, <kbd>X</kbd> links the cannons, <kbd>S</kbd> sets the shields, <kbd>F9</kbd> <kbd>F10</kbd> the lasers' and shields' recharge, <kbd>M</kbd> the map, <kbd>B</kbd> the goals, <kbd>.</kbd> the cockpit on and off, <kbd>H</kbd> hyperspace home once the work is done, <kbd>Esc</kbd> the options.",
    dosbox: "[dosbox]\nmachine=svga_s3\nmemsize=16\n[dos]\nxms=true\nems=true\numb=true\n[sblaster]\nsbtype=sb16\nirq=7\ndma=1\nhdma=5\n" },
];

// The registry entry of a game id, or null (a game picked by its executable).
export const gameById = (id) => GAMES.find((g) => g.id === id) || null;
// ... and of a room code's tag ("WC2").
export const gameByTag = (tag) => GAMES.find((g) => g.tag === tag) || null;
// The registry's programs in a game directory (its files, paths relative to
// it): the game itself first, then the add-ons that came with it.
export function programsIn(files) {
  const top = new Set(files.filter((f) => !f.path.includes("/")).map((f) => f.path.toUpperCase()));
  return GAMES.filter((g) => g.detect.some((d) => top.has(d))).sort((a, b) => (a.partOf ? 1 : 0) - (b.partOf ? 1 : 0));
}

// Is the game's executable, among a game directory's files, the build the
// hooks know?  Their own test (game_program_loaded in
// src/cpu/wcnet_game.cpp) made on the file instead of the loaded program:
// the image begins after the MZ header, whose length in paragraphs is the
// word at byte 8, and a string is the same bytes in both.
export function knownBuild(game, files) {
  const sig = game && game.build;
  if (!sig) return false;
  const exe = files.find((f) => !f.path.includes("/") && game.detect.includes(f.path.toUpperCase()));
  const d = exe && exe.data;
  if (!d || d.length < 28 || d[0] !== 0x4d || d[1] !== 0x5a) return false;
  if (sig.para === undefined && !sig.rvhook) {
    // (no data segment to go by: the words anywhere in the file)
    const want = Array.from(sig.text, (ch) => ch.charCodeAt(0));
    for (let i = 0; i + want.length <= d.length; i++) {
      let k = 0;
      while (k < want.length && d[i + k] === want[k]) k++;
      if (k === want.length) return true;
    }
    return false;
  }
  const image = (d[8] | (d[9] << 8)) * 16;
  let para = sig.para;
  if (sig.rvhook) {
    // "RVHOOK" opens the image, and its word at 8 is the data segment's paragraph
    if (image + 10 > d.length || String.fromCharCode(...d.subarray(image, image + 6)) !== "RVHOOK") return false;
    para = d[image + 8] | (d[image + 9] << 8);
  }
  const at = image + para * 16 + sig.off;
  if (at + sig.text.length > d.length) return false;
  for (let i = 0; i < sig.text.length; i++) if (d[at + i] !== sig.text.charCodeAt(i)) return false;
  return true;
}

// The Wing Commanders GOG sells since 2026-11-05 are packs, "Wing Commander: 35th Anniversary Edition" and "Wing
// Commander II: Vengeance of the Kilrathi: 35th Anniversary Edition" (installers of 1.1 GB and 850 MB), each with the
// release sold until then beside it in the buyer's library: "Wing Commander - Legacy Edition" (an installer of 34 MB)
// and "Wing Commander II - Legacy Edition" (27 MB).  An Anniversary Edition's installer holds the Legacy Edition's
// game, byte for byte, in its DOS/ directory (beside a Windows version, Kilrathi_Saga/, and a patched one, WCAT/):
// the page unpacks only that (inno-worker.js), so either installer is the game.  The old "Wing Commander 1+2" store
// page is gone.  A Wing Commander that is neither gets the advice below (the user, 2026-11-15: "If you detect a
// strange version can you instruct the user to download Legacy Edition from GOG instead", then, with the
// Anniversary installers: "Ideally we just make them work").
export const LEGACY = {
  wc1: { title: "Wing Commander", legacy: "Wing Commander - Legacy Edition",
         pack: "Wing Commander: 35th Anniversary Edition", url: "https://www.gog.com/en/game/wing_commander_35th_anniversary_edition" },
  wc2: { title: "Wing Commander II", legacy: "Wing Commander II - Legacy Edition",
         pack: "Wing Commander II: 35th Anniversary Edition", url: "https://www.gog.com/en/game/wing_commander_ii_vengeance_of_the_kilrathi_35th_anniversary_edition" },
};
// Which of the two a dropped file is, by its name and by what is in it: "wc1", "wc2" or "" (neither: the later Wing
// Commanders and Privateer are games of their own).  For a file the page could not make a known game of.
export function legacyOf(name, files = []) {
  const names = new Set(files.map((f) => base(f.path.replace(/\\/g, "/")).toUpperCase()));
  if (["WC2.EXE", "SO1.EXE", "SO2.EXE", "ALTSHIPS.000", "ARTDATA.V00"].some((n) => names.has(n))) return "wc2";
  if (["WC.EXE", "SM2.EXE"].some((n) => names.has(n)) || (names.has("CAMP.000") && names.has("BRIEFING.000"))) return "wc1";
  const n = String(name || "").toLowerCase();
  if (!/wing[\s_.-]*commander/.test(n) || /commander[\s_.-]*(iii|iv|[345])(?![a-z\d])|privateer|armada|academy|prophecy|saga/.test(n)) return "";
  return /commander[a-z]*[\s_.-]*(ii|2)(?![a-z\d])|vengeance/.test(n) ? "wc2" : "wc1";
}
// What to tell the pilot: the words, and the store page they name.
export function legacyAdvice(which) {
  const l = LEGACY[which];
  return l ? { text: `This page flies GOG's release: drop the installer of ${l.pack}, or of "${l.legacy}" (which comes with it), as you downloaded it from your GOG library.`,
               link: l.url, linkText: `${l.title} on GOG` } : null;
}

// What an installer leaves beside a game that the game never reads: its folders (GOG's own DOSBox, the
// redistributables, GOG's support files, the installer's temporary files), GOG's metadata files (goggame-*, the
// launcher's webcache.zip), documents and shortcuts (manuals as PDF, .lnk, .url) and Windows libraries (.dll: no DOS
// program loads one).  Everything else under the game's folder is kept, whatever its name: a game's own files can
// look like an installer's (TIE Fighter's map icons are RESOURCE\ICONS640.ICO and MAPICONS.ICO; the user,
// 2026-11-15: "I suspect all files are actually needed that are there in the first place").  Measured over every
// installer the page takes (GOG's TIE Fighter, Wing Commander 1.01 and its 35th Anniversary Edition, Wing Commander
// II's, Privateer, Terminal Velocity, Steam's Raptor): what this leaves out under the game's folder is manuals, GOG's
// metadata and DOSBox alone.
const JUNK_DIRS = new Set(["dosbox", "__redist", "__support", "commonappdata", "tmp", "__macosx"]);
const JUNK_EXT = new Set(["pdf", "lnk", "url", "dll"]);
const JUNK_NAME = /^(goggame-\d+\.(ico|hashdb|info|script|dll)|webcache\.zip|goglog\.ini)$/i;

// -- zip ---------------------------------------------------------------------

export async function readZip(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let eocd = -1;
  for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 65558); i--) {
    if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error("not a zip file (no end-of-central-directory record)");
  const count = dv.getUint16(eocd + 10, true);
  const cdSize = dv.getUint32(eocd + 12, true);
  let off = dv.getUint32(eocd + 16, true);
  if (off === 0xffffffff) throw new Error("zip64 archives are not supported");
  // (a zip with bytes before it, an installer's header or a stub: its offsets count from the zip's own start)
  const shift = eocd - cdSize - off > 0 ? eocd - cdSize - off : 0;
  off += shift;
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
    const local = dv.getUint32(off + 42, true) + shift;
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

// (the installer: a File, read by the worker where it is, or its bytes)
export function extractInstaller(file, onLog) {
  const blob = file instanceof Blob ? file : new Blob([file]);
  const detect = [...new Set(GAMES.filter((g) => !g.partOf).flatMap((g) => g.detect))];
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL("./inno-worker.js", import.meta.url), { type: "module" });
    worker.onmessage = (ev) => {
      const m = ev.data;
      if (m.type === "log") onLog?.(m.line);
      else if (m.type === "done") { worker.terminate(); resolve(m.files.map((f) => ({ path: f.path, data: new Uint8Array(f.data) }))); }
      else if (m.type === "error") { worker.terminate(); reject(new Error(m.message)); }
    };
    worker.onerror = (e) => { worker.terminate(); reject(new Error("installer extraction failed: " + (e.message || e))); };
    worker.postMessage({ type: "extract", blob, detect });
  });
}

// -- which game --------------------------------------------------------------

const base = (p) => p.slice(p.lastIndexOf("/") + 1);
const dir = (p) => (p.includes("/") ? p.slice(0, p.lastIndexOf("/")) : "");
const ext = (p) => { const b = base(p); return b.includes(".") ? b.slice(b.lastIndexOf(".") + 1).toLowerCase() : ""; };

// (the files a game needs whatever their extension: the registry's `keep`)
const KEEP = new Set(GAMES.flatMap((g) => g.keep || []).map((n) => n.toUpperCase()));
function isJunk(path) {
  const parts = path.split("/");
  if (parts.slice(0, -1).some((d) => JUNK_DIRS.has(d.toLowerCase()))) return true;
  if (KEEP.has(base(path).toUpperCase())) return false;
  return JUNK_EXT.has(ext(path)) || JUNK_NAME.test(base(path));
}

// Finds the game among the files: the known executable closest to the top
// decides the game and its directory; everything under that directory
// (minus installer junk) becomes drive C.
export function identifyGame(files) {
  const normalized = files.map((f) => ({ path: f.path.replace(/\\/g, "/").replace(/^\.?\/+/, ""), data: f.data }));
  let best = null;
  for (const game of GAMES) {
    if (game.partOf) continue;
    for (const f of normalized) {
      if (isJunk(f.path)) continue;
      if (game.detect.includes(base(f.path).toUpperCase())) {
        // (the build the hooks know before another program of its name: GOG's Anniversary Editions hold the DOS
        // game and a Windows one, WC2.EXE both, as deep)
        const depth = f.path.split("/").length;
        const known = knownBuild(game, [{ path: base(f.path), data: f.data }]);
        if (!best || (known && !best.known) || (known === best.known && depth < best.depth)) best = { game, root: dir(f.path), depth, known };
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
  const gathered = gatherFiles(best.game, best.root, normalized, gameFiles);
  return { game: best.game, root: best.root, files: gameFiles, gathered };
}

// A game whose files are not all beside its program (the user, 2026-11-15: "very loose on finding the .EXE and
// supporting files so that if users compress one level deeper or shallower or with different subfolder names or from
// their original DOS backups it still works ... a find/search sort of situation to ground ourselves in whatever zip
// is passed").  The program is already found wherever it is, its folder the game's; here every file the game wants
// by name (the registry's `gather`: a pattern for the name, and the game's folder it belongs in, "" its top) that is
// not where the game looks is searched for through the whole archive, any folder, any case, and put there: of
// several, the one nearest the program (under its folder first, then the longest shared path, then the shallowest).
// Returns what was moved, for the page's log.
function gatherFiles(game, root, all, gameFiles) {
  const rules = game.gather || [];
  if (!rules.length) return [];
  const have = new Set(gameFiles.map((f) => f.path.toUpperCase()));
  const rootParts = root ? root.split("/") : [];
  const near = (path) => {
    const parts = dir(path) ? dir(path).split("/") : [];
    let shared = 0;
    while (shared < parts.length && shared < rootParts.length && parts[shared].toUpperCase() === rootParts[shared].toUpperCase()) shared++;
    // (under the program's folder: best; else the more of the path shared the better, the shallower the better)
    return [shared === rootParts.length ? 0 : 1, -shared, parts.length];
  };
  const better = (a, b) => { const x = near(a), y = near(b); for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] < y[i]; return false; };
  const picked = new Map();     // (where it belongs, upper case) -> the file
  for (const f of all) {
    const parts = f.path.split("/");
    if (parts.slice(0, -1).some((d) => JUNK_DIRS.has(d.toLowerCase()))) continue;
    const name = base(f.path);
    const rule = rules.find((r) => r.name.test(name));
    if (!rule) continue;
    const to = (rule.to || "") + name;
    if (have.has(to.toUpperCase())) continue;
    const was = picked.get(to.toUpperCase());
    if (!was || better(f.path, was.from)) picked.set(to.toUpperCase(), { to, from: f.path, data: f.data });
  }
  const out = [];
  for (const g of picked.values()) {
    gameFiles.push({ path: g.to, data: g.data });
    out.push(`${g.from} -> ${g.to}`);
  }
  return out;
}

// Zips inside the dropped archive (a backup of a backup, a download wrapped once more): each that reads as a zip and
// holds a program the registry knows is replaced by its files, in a folder of its name.  For an archive no game was
// found in; the shareware's own inner zips (other games' adverts) hold no such program and stay as they are.
export async function expandInnerZips(files) {
  const known = new Set(GAMES.flatMap((g) => g.detect));
  let out = files, n = 0;
  for (const f of files) {
    if (ext(f.path) !== "zip" || n >= 8) continue;
    let inside;
    try { inside = await readZip(f.data); } catch (e) { continue; }
    inside = await expandSplitZips(inside);
    if (!inside.some((g) => known.has(base(g.path.replace(/\\/g, "/")).toUpperCase()))) continue;
    const at = f.path.replace(/\\/g, "/").replace(/\.zip$/i, "") + "/";
    out = out.filter((g) => g !== f).concat(inside.map((g) => ({ path: at + g.path.replace(/\\/g, "/"), data: g.data })));
    n++;
  }
  return out;
}

// A CD's image dropped by itself, or beside nothing that is a game (an .iso of the X-Wing Collector's CD: the disc's top
// holds the installed game's own files): the files at the image's top put beside it, when one of them is a program
// the registry knows that comes with a disc (`disc`).  ISO 9660, 2048-byte sectors or raw 2352-byte ones.
export function expandCdImages(files) {
  const known = new Set(GAMES.filter((g) => g.disc).flatMap((g) => g.detect));
  let out = files;
  for (const f of files) {
    const d = f.data;
    if (!d || d.length < 2e6) continue;
    let ss = 0, hdr = 0;
    for (const [s, h] of [[2048, 0], [2352, 16]]) {
      const p = 16 * s + h;
      if (d[p] === 1 && String.fromCharCode(...d.subarray(p + 1, p + 6)) === "CD001") { ss = s; hdr = h; break; }
    }
    if (!ss) continue;
    const read = (sector, size) => {
      const o = new Uint8Array(size);
      for (let k = 0; k * 2048 < size; k++) {
        const from = (sector + k) * ss + hdr, n = Math.min(2048, size - k * 2048);
        if (from + n > d.length) break;
        o.set(d.subarray(from, from + n), k * 2048);
      }
      return o;
    };
    const u32 = (b, i) => (b[i] | b[i + 1] << 8 | b[i + 2] << 16 | b[i + 3] << 24) >>> 0;
    const pvd = read(16, 2048);
    const rootDir = read(u32(pvd, 158), Math.min(u32(pvd, 166), 1 << 20));
    const top = [];
    for (let i = 0; i < rootDir.length;) {
      if (!rootDir[i]) { i = (Math.floor(i / 2048) + 1) * 2048; continue; }
      const name = String.fromCharCode(...rootDir.subarray(i + 33, i + 33 + rootDir[i + 32])).replace(/;\d+$/, "").replace(/\.$/, "").toUpperCase();
      if (!(rootDir[i + 25] & 2) && name.length > 1) top.push({ name, extent: u32(rootDir, i + 2), size: u32(rootDir, i + 10) });
      i += rootDir[i];
    }
    if (!top.some((e) => known.has(e.name))) continue;
    const at = dir(f.path.replace(/\\/g, "/"));
    const prefix = at ? at + "/" : "";
    const have = new Set(out.map((g) => g.path.replace(/\\/g, "/").toUpperCase()));
    const added = top.filter((e) => !have.has((prefix + e.name).toUpperCase()) && e.size < 64e6).map((e) => ({ path: prefix + e.name, data: read(e.extent, e.size) }));
    out = out.concat(added);
  }
  return out;
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
//
// A game is one record in "games" (its files inline), but a browser holds a
// record of some 127 MB at most (Chrome: "the serialized keys and/or value
// are too large"), and TIE Fighter's disc image alone is 594 MB.  So a big
// file (PART_FROM and up), and every file once the record's inline files
// reach INLINE_BUDGET, is kept out of the record, in "parts": pieces of
// PART_SIZE under the keys <game id>/<path>#<n>, the record holding
// { parts, size } in their place.  Records of the first version (every file
// inline) read as they are.

// The terrain branch's playtest copy of the page (fireflyk64.github.io/terrain/) shares the site's storage with
// /dosbox: it keeps its games in a database of its own, as its saves and made sets (web/wc.js saveKey, retrovega/web/rvsets.js).
const DB = typeof location !== "undefined" && /^\/terrain\//.test(location.pathname) ? "wc-web-terrain" : "wc-web";   // (no location: node's tests)
const PART_SIZE = 32 * 1048576, PART_FROM = 8 * 1048576, INLINE_BUDGET = 64 * 1048576;
function openDb() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB, 2);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains("games")) db.createObjectStore("games", { keyPath: "id" });
      if (!db.objectStoreNames.contains("parts")) db.createObjectStore("parts", { keyPath: "id" });
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}
function tx(db, mode, fn, stores = ["games"]) {
  return new Promise((resolve, reject) => {
    const t = db.transaction(stores, mode);
    const r = fn(...stores.map((n) => t.objectStore(n)));
    t.oncomplete = () => resolve(r && r.result);
    t.onerror = () => reject(t.error);
    t.onabort = () => reject(t.error);
  });
}
const partKey = (id, path, n) => `${id}/${path}#${n}`;
const partRange = (id) => IDBKeyRange.bound(`${id}/`, `${id}/\uffff`);
export async function saveGame(record) {
  try {
    const db = await openDb();
    let inline = 0;
    const files = [], parts = [];
    for (const f of record.files) {
      const size = f.data.byteLength;
      if (size >= PART_FROM || inline + size > INLINE_BUDGET) {
        let n = 0;
        for (let at = 0; at < size; at += PART_SIZE, n++) parts.push({ id: partKey(record.id, f.path, n), data: f.data.slice(at, Math.min(size, at + PART_SIZE)) });
        files.push({ path: f.path, parts: n, size });
      } else {
        inline += size;
        files.push(f);
      }
    }
    await tx(db, "readwrite", (games, store) => {
      store.delete(partRange(record.id));
      for (const p of parts) store.put(p);
      games.put({ ...record, files });
    }, ["games", "parts"]);
    db.close();
    return true;
  } catch (e) { console.warn("cannot save the game files in this browser:", e); return false; }
}
export async function loadGames() {
  try {
    const db = await openDb();
    const all = (await tx(db, "readonly", (s) => s.getAll())) || [];
    for (const g of all) {
      if (!g.files.some((f) => f.parts)) continue;
      const parts = (await tx(db, "readonly", (s) => s.getAll(partRange(g.id)), ["parts"])) || [];
      const byId = new Map(parts.map((p) => [p.id, p.data]));
      g.files = g.files.map((f) => {
        if (!f.parts) return f;
        const data = new Uint8Array(f.size);
        let at = 0;
        for (let n = 0; n < f.parts; n++) {
          const piece = byId.get(partKey(g.id, f.path, n));
          if (!piece) throw new Error(`${g.title}: ${f.path} is missing part ${n} in this browser's cache`);
          data.set(new Uint8Array(piece), at); at += piece.byteLength;
        }
        return { path: f.path, data };
      });
    }
    db.close();
    return all;
  } catch (e) { console.warn("cannot read the saved games in this browser:", e); return []; }
}
export async function forgetGame(id) {
  try {
    const db = await openDb();
    await tx(db, "readwrite", (games, parts) => { parts.delete(partRange(id)); games.delete(id); }, ["games", "parts"]);
    db.close();
  } catch (e) { /* ignore */ }
}

export function totalSize(files) { return files.reduce((n, f) => n + f.data.byteLength, 0); }
