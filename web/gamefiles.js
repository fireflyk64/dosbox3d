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
// .tar.gz beside the page: it is in every browser's "Play" menu with no
// files dropped, fetched when it is flown.
// `image`: a CD image among the game's files (GOG's game.gog, the disc as
// raw sectors), mounted as drive D before the game runs (imgmount, an
// ISO).  `keep`: files of the game that the installer-junk rule would drop
// by their extension (TIE Fighter's IMUSE.INI).
// `noRoomOptions`: the room's Conn and Collide choices are Wing Commander's (the frame exchange's mode, the asteroid
// fields and wing bumps): none of them for this game, so the page leaves them out.  `screen`: the picture's shape on
// the page, [width, height] (640 x 400 when none: Wing Commander's 320 x 200 doubled; TIE Fighter flies at 640 x 480,
// whose rows a 400-row canvas would drop).
// `prepare(FS, root)`: the game's own settings made fit for the browser after its files are installed (the copy in
// the emulator's memory only; what it did, for the log).

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

export const GAMES = [
  { id: "wc1", tag: "WC1", title: "Wing Commander", detect: ["WC.EXE"], run: "wc", multiplayer: true,
    build: { para: 0x1231, off: 0x0187, text: "Loading WING COMMANDER" },
    saves: ["GAMEDAT/SAVEGAME.WLD"], cycles: 3630, fps: WC1_FPS, capHull: true, joystick: true,
    pointer: { fromGame: true },
    campaign: { series: WC1_SERIES, hints: WC1_HINTS } },
  // SM2.EXE is the game's last build, with the Crusade campaign (it can
  // load the other two from a saved game), and keeps its saved games in a
  // file of its own.  The hooks know it as they know WC.EXE; a picked
  // mission is one of Crusade's.
  { id: "wc1sm2", tag: "SM2", partOf: "wc1", title: "Wing Commander: The Secret Missions 2", detect: ["SM2.EXE"], run: "sm2", multiplayer: true,
    build: { para: 0x11E8, off: 0x0181, text: "Loading WING COMMANDER" },
    saves: ["GAMEDAT/CRUSADE.WLD"], cycles: 3630, fps: WC1_FPS, capHull: true, joystick: true,
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
    keys: "<b>RetroVega in flight:</b> the mouse or the arrow keys steer (a tap turns a little, held they build to the full turn in a third of a second; <kbd>Shift</kbd> with an arrow turns at full at once), <kbd>Q</kbd> and <kbd>E</kbd> (or <kbd>,</kbd> and <kbd>.</kbd>, or the keypad's <kbd>Ins</kbd> and <kbd>Del</kbd>; a controller's roll axis too) roll (the same way), <kbd>Space</kbd> or the left button fires the guns, <kbd>Enter</kbd> a missile, <kbd>Tab</kbd> (or the keypad's <kbd>*</kbd>) the afterburner; <kbd>+</kbd> and <kbd>-</kbd> set the speed, <kbd>F</kbd> or <kbd>\\</kbd> full, <kbd>S</kbd> or <kbd>Backspace</kbd> stop; <kbd>T</kbd> the next target in front of you, <kbd>Y</kbd> the nearest enemy, <kbd>L</kbd> locks the target; <kbd>G</kbd> picks the guns, <kbd>W</kbd> the missile, <kbd>D</kbd> the damage readout; <kbd>N</kbd> the nav points (a white cross marks the one chosen), <kbd>A</kbd> the autopilot; <kbd>`</kbd> or <kbd>~</kbd> the ships between high detail and blocks, at once; <kbd>C</kbd> the comm menu: <kbd>1</kbd> your wingman (fly in formation, break formation, attack your target, help you out, return to base), <kbd>2</kbd> your target (taunt an enemy; with the carrier as your target, request landing within 25,000 km, granted once you have flown a nav point or finished the mission, then come within 2,000 km of it, afterburner off, to land), <kbd>9</kbd> never mind; at once: <kbd>C</kbd> three times quickly (or <kbd>Alt</kbd>+<kbd>T</kbd>) taunts your target; flying with others, <kbd>0</kbd> (or a tap of <kbd>Shift</kbd>) talks to them. Home with every nav point flown and no enemy about, <b>PRESS FIRE TO LAND</b> shows under the gunsight: the trigger lands you; <kbd>R</kbd> the radar's range; <kbd>F5</kbd> the outside view, <kbd>F1</kbd> the cockpit. The simulator in the bar (click its chair) is Rlaan after Rlaan, on your own: <kbd>Esc</kbd> or <kbd>Caps Lock</kbd> leaves it.",
    dosbox: "[dosbox]\nmemsize=63\n[dos]\nxms=true\nems=false\numb=true\n[sblaster]\nsbtype=sb16\noplmode=opl3\n" },
  // TIE Fighter (LucasArts, 1995: the Collector's CD-ROM as GOG sells it).
  // TIE.EXE starts Z_TIE__.EXE, a 32-bit program under DOS/4GW; the disc
  // (game.gog: the voices, and the check the game makes before anything
  // else) must be in drive D, or the game quits at once.  Its 594 MB go into
  // the browser's cache in parts (saveGame).  The machine is the one GOG's
  // own configuration asks for (an S3 card for its 640x480 flight, 16 MB,
  // EMS for the extender, a Sound Blaster 16 on IRQ 7).  Single-player
  // until its hooks are built (docs/tiefighter.md).
  { id: "tf", tag: "TF", title: "TIE Fighter", detect: ["TIE.EXE"], run: "tie", multiplayer: true,
    image: "game.gog", keep: ["IMUSE.INI"], cycles: 60000, campaign: TF_CAMPAIGN, noRoomOptions: true, screen: [640, 480], prepare: tfMusicOnFm,
    // (the launcher's own words: the hooks find the game's program in memory by its bytes, not by this file)
    build: { text: "Loading TIE-Fighter CD" },
    help: "TIE Fighter flown together: everybody in the room flies the same mission, each in a craft of the player's flight group (or the craft that pairs with it, for a mission flown alone), and sees the others where they really are. The craft near a pilot are flown by that pilot's machine, the rest by the host's; every shot is seen by everybody, a hit is decided by the shooter's machine and applied by the machine flying the craft hit, and a kill counts for the pilot who made it. A pilot down or the host's flight over ends the flight for everybody, mission failed: Fly Mission Again at the debriefing. The host decides: a guest's launch waits until the host flies, and the ship's PA calls a guest to the briefing. In a room the Combat Chamber's T/F row is the Z-95 challenge: two Z-95 Headhunters, hopelessly outgunned, flying the gunboat's and the T/I's missions against the Empire's own craft, the radio speaking for the other side in the game's own lines and voices. The host may pick a mission for everybody in the room's menu, any of the Combat Chamber's and any battle's: launching any Combat Chamber mission then flies it, briefing and all (the host may change it between flights). With none picked, a guest's Combat Chamber launch flies the host's; the line over the game says where the others are and when the host launches. The tour's own battles stay each pilot's own. Space skips the opening; at the registration desk type a pilot's name (or click one) and take the door; in flight the mouse is the stick and its button fires, Backspace is full throttle. What is done and what comes next: docs/tiefighter.md.",
    // the flight's controls as the emulator gives them (src/cpu/wcnet_tie_controls.cpp; the "Flight" choice beside Controller)
    keys: "<b>TIE Fighter in flight:</b> the arrow keys steer (a tap turns a little, held they build to the full turn in a third of a second; <kbd>Shift</kbd> with an arrow turns at full at once; <kbd>Up</kbd> dives as a stick pushed forward), the keypad's <kbd>Ins</kbd> and <kbd>Del</kbd> roll. The mouse, as the <b>Flight</b> choice beside Controller has it: <i>point to aim</i> (a ring shows the pointer; the middle is still, the further the pointer the faster the turn, as in Wing Commander and RetroVega), <i>drag to steer</i> (the game's own, 1994), or nothing (the keyboard alone, or a joystick: a controller picked above, which also rolls and fires); <kbd>F12</kbd> in flight switches between the first three. The left button fires; <kbd>W</kbd> (held a moment) changes weapon, <kbd>Backspace</kbd> full throttle, <kbd>\\</kbd> none, <kbd>[</kbd> and <kbd>]</kbd> a third and two thirds; <kbd>T</kbd> and <kbd>R</kbd> target; <kbd>Esc</kbd> the game's options.",
    dosbox: "[dosbox]\nmachine=svga_s3\nmemsize=16\n[dos]\nxms=true\nems=true\numb=true\n[sblaster]\nsbtype=sb16\nirq=7\ndma=1\nhdma=5\n" },
  // Wing Commander: Privateer (Origin, 1993: the CD-ROM edition as GOG sells it, Righteous Fire with it).  PRIV.EXE is
  // Origin's loader, PRCD.EXE the game (a real-mode program like the Wing Commanders, its world in EMS); the disc
  // (GAME.GOG: PRIV.TRE and RF.TRE, all of the game's data) must be in drive D.  The machine is the one GOG's own
  // configuration asks for (dosboxPRIV.conf: an S3, 16 MB, EMS and XMS, a Sound Blaster 16 on IRQ 5).  Saved games
  // are named by the player: *.SAV (Privateer) and *.PRS (Righteous Fire), kept by the page whatever their names.
  // Flown together loosely (docs/privateer.md: rooms of up to 8, `maxSeats`, everybody's own career, presence so far;
  // src/cpu/wcnet_priv_session.cpp); the emulator ramps its arrow keys (src/cpu/wcnet_priv_controls.cpp).
  { id: "priv", tag: "PRV", title: "Wing Commander: Privateer", detect: ["PRCD.EXE"], run: "priv", multiplayer: true, maxSeats: 8,
    image: "GAME.GOG", cycles: 30000, saves: ["*.SAV"], noFlight: true, noRoomOptions: true,
    build: { para: 0x6E4D, off: 0x0074, text: "Type PRIV to run Privateer" },
    help: "Privateer flown together loosely, rooms of up to 8: everybody plays their own career, goes where they like and keeps their own saved games, and the room tells everybody where the others are: each pilot's system and place is sent when it engages the autopilot (A), jumps, launches or lands, and every 15 s, and a line over the game says who is in your system and how far (in the game's metres, as its Range readout gives them). Seeing each other in space, sharing a nav point's fights, cargo and the Steltek drone come next (docs/privateer.md). Esc skips the opening; NEW registers a pilot (type a name and a callsign, Enter after each); click your ship on the landing pad to launch. Save in a base with the Quine 4000's SAVE: the page keeps your saved games in this browser.",
    keys: "<b>Privateer in flight:</b> the arrow keys steer (a tap turns a little, held they build to the full turn in a third of a second; <kbd>Shift</kbd> with an arrow turns at full at once), <kbd>Home</kbd> and <kbd>PgUp</kbd> (the keypad's <kbd>7</kbd> and <kbd>9</kbd>) roll the same way; <kbd>Space</kbd> fires the guns, <kbd>Enter</kbd> the weapon picked with <kbd>W</kbd> (a missile, or the tractor beam), <kbd>Tab</kbd> the afterburner; <kbd>+</kbd> and <kbd>-</kbd> set the speed, <kbd>Backspace</kbd> stops; <kbd>T</kbd> targets, <kbd>N</kbd> the nav map, <kbd>A</kbd> the autopilot, <kbd>J</kbd> jumps, <kbd>X</kbd> expels your cargo, <kbd>C</kbd> the comms; <kbd>Alt</kbd>+<kbd>O</kbd> the options, where the mouse or a joystick can be the stick instead (the ramp is the keyboard's).",
    dosbox: "[dosbox]\nmachine=svga_s3\nmemsize=16\n[dos]\nxms=true\nems=true\numb=true\n[sblaster]\nsbtype=sb16\nirq=5\ndma=1\nhdma=5\n" },
  // Righteous Fire: the same program and disc, started as GOG's menu starts it ("priv r"), its saved games *.PRS.
  { id: "privrf", tag: "PRF", partOf: "priv", title: "Wing Commander: Privateer - Righteous Fire", detect: ["PRCD.EXE"], run: "priv r", multiplayer: true, maxSeats: 8,
    image: "GAME.GOG", cycles: 30000, saves: ["*.PRS"], noFlight: true, noRoomOptions: true,
    build: { para: 0x6E4D, off: 0x0074, text: "Type PRIV to run Privateer" },
    help: "Righteous Fire, Privateer's sequel campaign, flown together the way Privateer is (rooms of up to 8, each pilot its own career, where everybody is told). It starts from a pilot of its own (a new one, or a Privateer pilot who finished the game). The page keeps your saved games in this browser.",
    dosbox: "[dosbox]\nmachine=svga_s3\nmemsize=16\n[dos]\nxms=true\nems=true\numb=true\n[sblaster]\nsbtype=sb16\nirq=5\ndma=1\nhdma=5\n" },
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

// (the files a game needs whatever their extension: the registry's `keep`)
const KEEP = new Set(GAMES.flatMap((g) => g.keep || []).map((n) => n.toUpperCase()));
function isJunk(path) {
  const parts = path.split("/");
  if (parts.slice(0, -1).some((d) => JUNK_DIRS.has(d.toLowerCase()))) return true;
  if (KEEP.has(base(path).toUpperCase())) return false;
  return JUNK_EXT.has(ext(path));
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
//
// A game is one record in "games" (its files inline), but a browser holds a
// record of some 127 MB at most (Chrome: "the serialized keys and/or value
// are too large"), and TIE Fighter's disc image alone is 594 MB.  So a big
// file (PART_FROM and up), and every file once the record's inline files
// reach INLINE_BUDGET, is kept out of the record, in "parts": pieces of
// PART_SIZE under the keys <game id>/<path>#<n>, the record holding
// { parts, size } in their place.  Records of the first version (every file
// inline) read as they are.

const DB = "wc-web";
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
