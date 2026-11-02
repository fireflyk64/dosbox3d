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

## 4. The game

Multiplayer appears in the logic only as who flies a ship (`rvController`, `RV_FLAG_HUMAN`), never as a role:

- **Collisions** (`ram`): a pair is this machine's to work out when its own pilot (CTL_PLAYER) is in it, or when no
  ship in it is another machine's (CTL_PUPPET). A guest's share of damage to the host's ship goes through
  `do_damage`, where the layer reports it as it reports a wingman's bolt hits.
- **Missiles:** a ship that fires a launcher publishes the target in `missileTarget[ship]` first (the player's lock,
  or for a dumbfire the chosen target; an AI's own target); `missile_launch` reads it there for every shooter, so a
  launch replayed on another machine homes and aims the same way.
- **Deaths:** `step_deaths` calls `delayedDespawn` when it first sees a ship dying, and that body starts the dying
  (shipAiState 9, the explosion's time); `despawn` ends it. The layer hears the kill as it did WC.EXE's.
- **The scene** (`step_scene`): a ship is held off-scene by its distance from the nearest human, not the local
  pilot's.
- **The autopilot** brings the human wingmen along with the AI wing, waits for no enemy near any human, and calls
  `autoAnimation` / `autopilotFinished` around the jump, so a guest's ship is moved with the host's.
- **The pilots' fates:** a wingman flown by a human (`g_killed` 2) is no named pilot's death.
- **Splashes:** the local pilot's kills in the flight (`logic_splashes`), for the guest's save.
- **The player's fighter** is the one its mission record names (`missionShipTable[playerMissionShip]`), which is the
  save's own in a flight alone and the host's choice in a shared one.
- **The flight's mission** (`game.c`): `flight_prepare` (once a flight) calls `missionStarting`; a guest then loads
  the mission the layer named, without compiling its own tables over the host's. After a guest's flight the
  debrief scores nothing on its campaign: `camp_guest_sortie` adds the sortie and the splashes to the hero's record
  and writes the guest's own bunk (if it has one). A retry discards the failed attempt, as alone.
- **Picked missions** (the page's menu): `/brief N`, the campaign at mission N, starting in the briefing room, as
  WC1's picked mission starts at its briefing.

## 5. Checks

- **Play tests** (`tests/play/playtest.sh --dos`, last in the table): M6.7 ram-owner, M7.8 puppet-launch, M8.29
  scene-human, M8.30 human-seat-lost, M8.31 dying.
- **Two or three native emulators over TCP** (`scripts/rvnet-duo.sh [SECONDS]`, `SCENARIO=`, `PLAYERS=3`, `WCFPS=`;
  the checks in `scripts/rvnet-check.py` read the dumps with `wcnet_rv.h`'s offsets): `meet` (the guest asked for
  mission 3 and flies the host's 0 by the host's records; each has the other where it is; every copy of a host ship
  a puppet; as many ships on both; both at the host's frame rate), `retry`, `carrier`, `guestdeath` (the death
  screens, the host's R reaching the guest, the mission flown again and met again), `land` (the guest's landing is
  the host's, scored alike), `autopilot` (the guest taken along), `kill` (the host's kills explode and go on the
  guest), `combat` (the guest's hits go to the host and come back), `PLAYERS=3` (a seat made for the third), and
  `WCFPS=15`. One pair at a time; 40-55 s each.
- **A mismatched interface** (`scripts/rvnet-abi.sh`): copies with the RVHOOK version, then a word of the directory,
  changed are refused and only watched.
- **Not covered by a rig:** a seat made from the player's own record (a mission whose wingman the host's save lost:
  every mission has one, and no switch loads a save at boot); the guest's save (`camp_guest_sortie`: a guest whose
  campaign was never saved has no bunk to write).
- **Key-script aids** this added (`src/cpu/wcnet_hooks.cpp`): `T:!ds=NAME` dumps the data segment,
  `T:!land[=SLOT]` lands on that ship (or the first capital ship of ours), `T:waitkey=KEY` waits for the next
  flight tapping KEY (the death screen's R).
