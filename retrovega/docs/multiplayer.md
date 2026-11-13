# RetroVega shared flights: the design

How two or three players fly one RetroVega mission together on the `/dosbox` page (RV.EXE in our DOSBox, joined by
the multiplayer layer `src/cpu/wcnet_*`). `multiplayer-plan.md` is the survey that came before it: every entry point
against the layer's WC1 handlers, and the user's rulings. This file is what was built, and why it is shaped the way
it is. The native `/rv` page (the game compiled for the browser) has no layer and stays single-player.

## 1. The rules (the user's, 2026-11-02)

- As in Wing Commander: the host runs the mission and every NPC; every other human flies a wingman. No gunner or
  drone seats (RetroVega has no turrets). A third pilot gets a fighter made for the seat.
- A guest flies the host's mission whatever its own save holds. Its flight's results (the sortie and its splashes) go
  into its own save, which is written to its own bunk; its campaign position and its saved fighter do not move.
- Fate as in Wing Commander: one human's end (death, the carrier lost, a landing, the exit key) ends the flight for
  everyone. RetroVega's death screen (R fly it again, C carry on) comes up on every machine, and the host's key
  decides for the room.
- Each machine works out its own pilot's collisions, both shares; a guest bumping the host (or another human) does
  no damage, as in Wing Commander. NPC against NPC is the host's.
- 30 frames a second by default, one logic step a frame; the host's FPS box (8-60) slows the physics and the
  traffic.
- RetroVega keeps changing, so the layer knows it only through the interface below, checked at load.

## 2. The interface (ABI 2)

One source: `abi/rvabi.def`. `tools/rvabi.py` makes from it the game's `src/rvhooks.h` and `src/rvhook.asm`, and the
layer's `src/cpu/wcnet_rv.h` (`--write-layer`). `build.sh` runs `--check-layer` first: a stale layer header fails
the build, so the game and the emulator in one deploy always agree.

- **Thunks** at RVHOOK:0x20 + 5n, sites at fixed RVHOOK offsets, the fixed block at DS:0000-73FF. ABI 2 appends
  `restartChoice` (RVHOOK:00D4); nothing earlier moved.
- **The layer's data map:** `wcnet_rv.h` carries `WCNET_RV_DS(X)`, every fixed-block item whose name the layer's
  `wcnet_ds.def` has, with its offset. `load_ds` takes RV's offsets from it; `wcnet_ds.def` itself keeps its five
  Wing Commander columns (no second writer of that file). An item marked `nolayer` in `rvabi.def` is left out:
  `pilotRecords` and `campaignTable`, near pointers RV never sets, which the layer would follow. The other
  WC-shaped items RV does not use (`statusPilots`, `navPointState`, the comm buffers ...) are mapped to their
  reserved bytes, so a layer write lands in room kept for it, never at DS:0000.
- **The load check** (`wcnet_retrovega.cpp`, `rv_interface_check`): RVHOOK's magic, version, thunk and site counts;
  the puppet header's magic, version, slots, record sizes; and every word of the game's `rvPuppetDirectory` against
  the generated `kRvDirectory`. Any difference: the layer only watches (logs the thunks, passes the page's keys), no
  session is ever opened, the log says `RV ABI mismatch: ...` and the page shows that the game and the emulator do
  not match.
- **Published fields the layer writes:** `rvNetRole` (0 alone, 1 host, 2 guest), `rvPlayerCount`, and per slot
  `rvController` (CTL_PUPPET for a ship another machine flies), `rvFlags` bit `RV_FLAG_HUMAN` (32: a human flies
  it, on another machine), `rvNetId`, `rvLastWrite` (the frame its motion was last written).

## 3. The layer

- **Game traits, not game names.** `GameTraits` (`wcnet_game.h`) says what a game has: wing seats found through
  mission records (WC1, RV), seats made in a room of two (RV: a mission whose wingman was lost still seats the
  guest), gunner and drone seats (WC2), puppets marked in the game (RV), mission tables sent with the briefing (RV),
  the carrier-lost ending (RV), a victory call after every ending (RV), the shared restart choice (RV), the default
  frame rate (WC1 30 natively, RV 30), networking only when a room or peer is configured (RV). The session asks
  the traits; WC1's raw-address work (`despawn_slot_quietly`, the pilot names, the mission tree, credits) stays
  WC1's.
- **One dispatcher.** A compatible RV.EXE in a room goes through the common `check_hooks_slow`, bound by
  `load_rv_code` with the generated `RV_FN_*` / `RV_SITE_*` offsets. Not bound: `afterStartup`, `skipOrchestra`
  (it is RV's music call every frame), `afterHangarMission`, `runHangarMission`, `enterBarracks`, `cockpitPointer`,
  the probes nothing needs. A room of one (`WCSOLO`) or a mismatch keeps the old observer (`rv_check_hooks`).
- **Puppets** (`rv_reconcile_controllers`, every frame top, and at once when a ship is replayed): on the host, a
  ship a guest flies is CTL_PUPPET and HUMAN; on a guest, every ship the host owns is CTL_PUPPET (the AI does not
  run on a copy), the host's and other guests' ships HUMAN too; a guest's own ship is CTL_PLAYER. A ship the layer
  made a puppet goes back to CTL_AI when its human leaves, and every mark is cleared when the session ends.
- **The meeting point** is `missionStarting`, which RV calls once per flight before its briefing (or before the
  launch, for `/fly`). The host's session takes clients there and records the briefing: the mission (`currentSeries`
  = the campaign node, `currentMission` = the mission in it) and the compiled mission tables (`missionShipTable`,
  `navPointTable`, `playerMissionShip`), whose numbering a guest's own save could change (a lost wingman is left
  out). A guest's session writes all of that into the guest's fixed block, then returns to the game.
- **Seats:** a guest takes a wingman of the mission (`wing_slots_wc1`), else a ship made for the seat (the
  wingman's record spawned again, or the player's own when the mission has no wingman left).
- **Endings:** `missionEnded` goes out the moment the flight loop returns (before the landing show), and the score
  (`missionVictoryCalc`, after `logic_end`) after it, as in WC.EXE. Status 6, the carrier lost, is a shared ending
  (`GameState.EndCarrierLost`). A guest's landing carries the carrier it landed on (`PlayerEnd.landed_on_plus_one`),
  so the host's game shows and scores the same landing.
- **The restart choice:** RV calls `restartChoice(key)` on its death screen every pass (0: no key). Alone, the key
  is the answer. The host's R or C goes to every guest at once (`NetworkMessage.restart_choice`, the out-of-band
  category, read while no flight runs) and is its own answer; a guest's keys are not, and it gets the host's choice
  once it has come. A guest whose session is gone decides for itself again.
- **Pacing:** the layer paces RV a step a frame (`WCFPS`, default 30; the page's FPS box). RV leaves its own frame
  cap off when `rvNetRole` is not 0.

## 4. The page

- RetroVega is built into every page (since 2026-11-03, the user: "make the default site include the retrovega flag
  by default and default to that system if there are no other games"; `?retrovega` no longer changes anything): it is
  in every "Play" menu and the game of a page with no other. Every game's pilots meet in the one public lobby,
  `WC-LOBBY` (the user: "We have to have a unified lobby...splitting people in an almost abandoned service will not
  help it grow"); the roster tags each pilot with their game (`RV`, `WC1` ...), and a page that switches games stays.
- `web/gamefiles.js`'s `rv` entry flies together (no `noFlight`): the mission menu is "Campaign from the quarters"
  and every mission of the campaign, read from `rv-campaign.json` (`tools/rvdata.py` writes `RVNET.JSON` from the same
  packed records as `RVGAME.DAT`; `scripts/build-web.sh` puts it beside the page), so the menu follows the campaign.
  A picked mission is `rv /noquit /brief N`. The FPS box: 30 by default, 8-60. The room's asteroid choices are hidden
  (`ownRocks`: RetroVega's rocks are each machine's own); the wing bumps stay the host's choice.
- A room of one (Players 1: `WCSOLO`) is the game alone, as before, and has the mission menu too (the campaign's
  `alone`: the game takes the mission on its own command line), "Campaign from your quarters" picked to begin with:
  the game as it starts by itself. In a room of two or three (the user, 2026-11-03: "the first mission is
  freewheeling, but once a player has successfully played then they go in lockstep") the host's game never waits for a
  pilot who has not come: it flies, and whoever comes later joins the mission in progress. After a mission flown
  together, to its end or until the wingman died or landed, the host's game waits in front of the next briefing room
  ("Waiting on the other player...", and the same in the room's chat on the page) until that wingman's game is at its
  own, for as long as the wingman holds a seat in the room. A wingman who presses Fly first waits for the host's
  briefing ("Waiting for the host's mission"). Over TCP (`scripts/rvnet-duo.sh` without `ROOM=1`) a host alone waits
  for a first wingman, as before: the rigs start their pair together and there is no room to ask.
- An RV.EXE and an emulator of two interfaces: the page says so in its status line (the layer's "RV ABI mismatch"),
  and the game flies alone.
- `scripts/web-rv.mjs` (the deploy's RV smoke) flies two pages together: the menu's missions, the 30, the host's
  mission on both, each ship where its own page has it.

## 5. The game

Multiplayer appears in the logic only as who flies a ship (`rvController`, `RV_FLAG_HUMAN`), never as a role:

- **Collisions** (`ram`): a pair is this machine's to work out when its own pilot (CTL_PLAYER) is in it, or when no
  ship in it is another machine's (CTL_PUPPET). A guest's share of damage to the host's ship goes through
  `do_damage`, where the layer reports it as it reports a wingman's bolt hits.
- **Missiles:** a ship that fires a launcher publishes the target in `missileTarget[ship]` first (the player's lock,
  or for a dumbfire the chosen target; an AI's own target); `missile_launch` reads it there for every shooter, so a
  launch replayed on another machine homes and aims the same way.
- **Taunts** (the user, 2026-11-07: "Will a second player help? Do their taunts make it through?"; they had not: a
  guest's fell on its own copy of the ship, a puppet nobody thinks for, and the AI turned only on slot 0, the host's
  pilot): a taunt is written into the fixed block's `rvTaunt` (a byte per ship slot: the taunter's slot, 0xff none;
  this machine's pilot writes 0) and the AI that flies the ship takes it at its next thought, turning on that slot (logic.c
  `fighter_think`, as the old game's rule has it: docs/ai.md "The taunt"). On a guest the ship is a puppet, so the taunt waits there; the layer takes
  it (`rv_take_own_taunt`, each frame in `reconcile_puppets`) and sends it in the guest's frame (wc.proto `Taunt`: the
  ship's network id), and the host writes its own slots in (`rv_put_taunt`: the ship, the guest's ship as the
  taunter). So each pilot can draw enemies onto its own ship. Play test M10.11; rig scenario `taunt`.
- **The cloak** (the user, 2026-11-12: "The cloak status (cloak off or cloak on or cloak disrupted by afterburner or
  comms) should be sent across the wire. The game just needs to store it in a fixed 2 bit region"): the pilot's game
  keeps it in its own slot's `rvFlags` bits 6-7 (0 off, 1 on, 2 disrupted; with 1 also `RV_FLAG_UNSEEN`, 16, which no
  mind targets). The layer sends it with each of the ship's places (wc.proto `ShipUpdate.cloak`, absent when off: a
  guest's own update and its best-effort copy; the host's frame for every slot, so a guest's goes on to the others)
  and writes it on the ship's puppet in every other game (`rv_put_cloak`, only when it changes: a waiter's own unseen
  bit is left alone), where the ship is drawn half seen and the host's AI lets it go. Rig scenario `cloak`
  (PLAYERS=3 too).
- **Deaths:** `step_deaths` calls `delayedDespawn` when it first sees a ship dying, and that body starts the dying
  (shipAiState 9, the explosion's time); `despawn` ends it. The layer hears the kill as it did WC.EXE's.
- **The scene** (`step_scene`): a ship is held off-scene by its distance from the nearest human, not the local
  pilot's.
- **The autopilot** brings the human wingmen along with the AI wing and waits for no enemy near any human. Its
  three-second flyby is the engine's (`flight.c`), and it starts with `autoAnimation(type, 1, frames)`: the layer
  replays that on every guest at once, which shows the flyby around its own ship and never jumps itself. The host's
  jump ends with `autopilotFinished`, which ends every guest's flyby and takes each ship where the host's game put
  it. A flyby that ends without a jump (an enemy came near) sends `autoAnimation(0, 0, 0)` instead, and a guest's
  flyby whose end never comes stops two seconds past its own. The host's AI wing and escort targets close up on the
  host's ship for the flyby (`logic_form_up`), and a human wingman is put in its place there once as it begins (the
  player's way, its throttle at the player's speed): the frame with the flyby's start carries an autopilot event, on
  which a guest applies its own ship's place from the frame, and `autoAnimation_c` takes its speed to that throttle.
  Every machine holds its own pilot's stick through a flyby, so the human flies on beside them and comes out of the
  jump in formation (rig: 7,812 km apart before, 430 after; the user, 2026-11-03: "player 2 doesn't get the same
  forcing function for auto somehow and can't see the leader").
- **The pilots' fates:** a wingman flown by a human (`g_killed` 2) is no named pilot's death.
- **Splashes:** the local pilot's kills in the flight (`logic_splashes`), for the guest's save.
- **The player's fighter** is the one its mission record names (`missionShipTable[playerMissionShip]`), which is the
  save's own in a flight alone and the host's choice in a shared one: a guest takes it as its `g_camp.player_ship` when
  the host's mission comes (`flight_prepare`), so its cockpit and its sets are the host's fighter's too (they had
  been its own save's: a Schroedinger's cockpit in the Franklin missions, and no Franklin to be seen).
- **The flight's mission** (`game.c`): `flight_prepare` (once a flight) calls `missionStarting`; a guest then loads
  the mission the layer named, without compiling its own tables over the host's, and its campaign goes to that
  mission (`camp_follow`: the node and the mission there). After a guest's flight the debrief moves its campaign on
  as the host's moves (`camp_resolve` with the shared score, a named wingman's loss taken as the host takes it): the
  next mission's bar, barracks and briefing are the host's story's (the user, 2026-11-03: "For player 2 the
  characters in the bar and the barracks are still from mission 1--they never advance and the second player can't
  enjoy the full story"; it had stayed where it was, `camp_guest_sortie`). The sortie and the splashes go on the
  guest's own pilot record, and its bunk is written if it has one. A node the guest joins partway starts its tally
  there (the host's wins in it are not carried), so at that node's end the two may branch apart until the next
  shared mission takes the guest to the host's again. A retry discards the failed attempt, as alone.
- **Picked missions** (the page's menu): `/brief N`, the campaign at mission N, starting in the briefing room, as
  WC1's picked mission starts at its briefing.

## 5a. The base together (2026-11-05)

The user: "Users complain they don't know what the other player is doing. Can we see where the host (or guest) is?
... if the guest would otherwise wait we instead don't make the [briefing] clickable until the host has clicked in
it. Also lets add the chat functionality with 0 to the base scenes (except when typing save game names)", and "a sort
of echoy tinny speech clip audio and text when the host starts the mission... "All pilots report to briefing room"
and have it repeat every 45 seconds".

- **Where each pilot is.** The game writes its own place in `rvWhere` (1 bar, 2 crew lounge, 3 briefing, 4 quarters,
  5 locker, 6 deck: a room is its number + 1, set in `scene_enter`; 7 flying, 8 simulator, 9 debriefing, 10 shot
  down). The layer sends it in a `Presence` message (wc.proto, out of band like chat) when it changes and every 3 s;
  the host relays a guest's to the other guests with its player number; each machine writes the others' into
  `rvPeerWhere` (a byte a player number, the host 0). The rooms show it in the top right corner ("HOST: CREW LOUNGE",
  "GUEST: BRIEFING", two guests "GUEST 1", "GUEST 2"), below the chat's caption while one shows, at the bottom right
  in the briefing and on the deck (their own titles run across the top).
- **The briefing door.** The host's `Presence` says whether its mission has begun (`within_briefed_mission`: at its
  briefing, or flying it; at its briefing door its place reads as the briefing); a guest's layer keeps
  `rvBriefingLock` 1 while it has not, and the guest's briefing door does not open: its label reads "Briefing: host in
  the bar", a click says "The host chooses the mission: the briefing opens when the host goes in." The host's own is
  sent before its waits at the mission's start (`presence_tick(true)` in `on_mission_starting`). No `Presence` from a
  host (an older one) leaves the door open, as before.
- **A guest in the base is introduced at once.** The session was sought only at the game's start and at the briefing
  door, and a guest introduced itself (`send_connect`) only in `on_barracks`, which RetroVega reaches at the door: the
  host knew nothing of a guest in the bar. Now the layer seeks a session from its periodic tick while RetroVega is in
  its rooms, and a guest's tick introduces it; the host no longer marks a RetroVega guest as needing the mission's start
  state when it is adopted (it asks at its briefing door, and `send_briefing_state` brings it in then): the mark had the
  host wait for the frames of a guest still in the bar.
- **The call to the briefing.** A guest in the base (bar, lounge, quarters, locker) whose host's mission has begun reads
  "PA: All pilots report to the briefing room." across the top and hears the ship's address system (sounds.csv
  `pa_report`: a two-note chime and the voice, art/voice/pa_report.wav, made with Piper's en_US-norman voice, through a tinny band and a
  hall's echo: tools/sfx.py), again every 45 s while it stays out (`pa_tick`), and only while the host is in the
  briefing room or on the deck: once the host has launched the call stops and its line comes down (the user,
  2026-11-05: it "stays annoyingly during flight"). The card plays in flight only: started for the call and stopped
  after it, or before a flight.
- **Chat with 0 in the base.** The layer's 0 prompt (sdlmain) opens wherever a RetroVega session is, the rooms too
  (`wc_net_chat_ready`), except while the game takes typed text (`rvTyping`: a bunk's name). The line shows across the
  top of the other pilots' rooms, as in flight.

## 6. Checks

- **Play tests** (`tests/play/playtest.sh --dos`, last in the table): M6.7 ram-owner, M7.8 puppet-launch, M8.29
  scene-human, M8.30 human-seat-lost, M8.31 dying.
- **Two or three native emulators over TCP** (`scripts/rvnet-duo.sh [SECONDS]`, `SCENARIO=`, `PLAYERS=3`, `WCFPS=`;
  the checks in `scripts/rvnet-check.py` read the dumps with `wcnet_rv.h`'s offsets): `meet` (the guest asked for
  mission 3 and flies the host's 0 by the host's records; each has the other where it is; every copy of a host ship
  a puppet; as many ships on both; both at the host's frame rate), `retry`, `carrier`, `guestdeath` (the death
  screens, the host's R reaching the guest, the mission flown again and met again), `land` (the guest's landing is
  the host's, scored alike), `autopilot` (the host's flyby shown on the guest while the host's runs, then the guest taken along), `kill` (the host's kills explode and go on the
  guest), `combat` (the guest's hits go to the host and come back), `ram` (the guest's share of its collisions
  goes to the host), `PLAYERS=3` (a seat made for the third), `field` and `field1` (ENIGMA2's and ENIGMA1's
  asteroid fields flown by every pilot: each RVLOG's rocks), and `WCFPS=15`. One pair at a time; 40-55 s each.
  `SCENARIO=field1 PLAYERS=3 HOST_MISSION=0 GUEST_MISSION=0` is mission 1 flown by three: 30 checks (2026-11-06).
- **Three pilots in browser pages** (`scripts/web-smoke.sh rvtrio`, `scripts/web-rv-trio.mjs`; the user, 2026-11-06:
  "can we up the players to 3 and run a test where they do a mission 1 together"): a room of three seats, two wingmen
  joining by its code, the host's Fly starting all three on mission 1: one host and two guests, the host counts three,
  the same mission, every pilot's ship where it is in the other two games (guest to guest too), then the host's
  autopilot to the asteroid field short of nav 2: the three together, each reading the rocks in. 16 checks.
- **A mismatched interface** (`scripts/rvnet-abi.sh`): copies with the RVHOOK version, then a word of the directory,
  changed are refused and only watched.
- **Not covered by a rig:** a seat made from the player's own record (a mission whose wingman the host's save lost:
  every mission has one, and no switch loads a save at boot); the guest's save (`debrief_settle`: a guest whose
  campaign was never saved has no bunk to write).
- **Key-script aids** this added (`src/cpu/wcnet_hooks.cpp`): `T:!ds=NAME` dumps the data segment,
  `T:!land[=SLOT]` lands on that ship (or the first capital ship of ours), `T:waitkey=KEY` waits for the next
  flight tapping KEY (the death screen's R), `T:!ram=SLOT|npc` puts our ship nose to nose with that one; `T:!hit=QTY[:back]` (WC1's) lands a hit 100 km
  off our nose (or tail): RetroVega's `do_damage` takes the place a hit lands (`GameTraits.damageAtPlace`), not the
  way the blow travels; `T:!shoot=SLOT|npc|foe:QTY[:back]` hits another ship the same way (the hits a wingman
  takes: docs/damage.md); `T:!tail=SLOT|npc|foe[:KM]` puts our ship KM (1,500) behind that one, facing its way (a
  heat seeker's lock). `npc` is the first other ship, `foe` the first of another side.
