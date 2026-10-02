# Wing Commander cooperative multiplayer (wcnet)

This DOSBox fork hooks the Wing Commander 1 executable (`WC.EXE`) so that two
pilots can fly a mission together: one machine is the server (the wing
leader, slot 0 of its own game) and the other flies the wingman.  This
document is the design reference for the code in `src/cpu/wcnet_*.cpp` and
records how the damage desync that motivated the rewrite was diagnosed.

Every game address below is written the way the IDA database (`wc.idb`,
exported as `WcMulti/wcmulti.idc`) names it, and can be reproduced with
`scripts/wcdis.py` (see "Reading the game" at the end).

## 1. Diagnosis: why damage was not coordinated

### 1.1 The game has two damage models, selected by slot

`do_damage(src, dst, quantity, vec)` is `ovr143:0A99` (reached through the
stub thunk `stub143:0084`, DOS `12D7:0084`).  Its flow:

1. `dseg:00BA` zero and `dst == 0` → the player is invulnerable, return.
2. Subtract the front or rear shield (`dseg:C430`, chosen by the sign of
   `dot(vec, front[dst])`, `ovr140:0924 vector_dot`).
3. Subtract the armor quadrant (`dseg:C85E`, chosen by `dot(vec, right[dst])`).
4. If armor is penetrated: with probability 1/100 destroy the ship outright
   (`delayedDespawn`, `ovr143:1F15`), otherwise apply *internal damage*
   (`ovr143:0EDB`) and if it returns 1 credit the kill (`ovr143:0A08`).

`ovr143:0EDB` starts with `or si,si / jnz`: **slot 0 takes the player path
`ovr143:1273`, every other slot takes the NPC path.**

| | Player model `ovr143:1273` | NPC model `ovr143:0EDB` |
|---|---|---|
| damage points per bolt hit | `max(quantity >> 4, 1)` | `max(quantity / 6, 1)` |
| per point | random system from a table, comm messages, HUD damage | random system 0..8 |
| instant death | system 2 from the front with probability 1/3, or 1/100 critical | system 2 from the front, always |
| hull | core hit points `dseg:C8B8` (4) → `missionStatus = 4` | `dseg:D22C` counter > ship max, or `delayedDespawn` inside `do_damage` |

So the same hit does very different things to "the ship in slot 0" and "the
same ship in slot 1".  In a two-player game each human's ship is slot 0 on
their own machine and an NPC slot on the other, and the old code replayed the
same `do_damage` call on both.  The server's copy of the wingman took NPC
damage and could be destroyed (from inside a replayed call, where the hooks
were inert, so the "ignore wingman death" logic never ran), while the client's
own copy took player damage.  Symmetrically the client's copy of the hero died
under the NPC model while the real hero was fine.  That is the "one player has
extra health" and "wingman explodes" behaviour.

### 1.2 Secondary problems found in the same pass

* The replay passed the shooter's *parent ship* as `src` even on the server,
  so the `entityType[src] == 8` (bolt) checks in `do_damage` never fired: no
  hit sounds, different taunt behaviour, and kill credit through a ship
  instead of a bolt.  (`ovr143:0BC6..0C1D`)
* Hooks were disabled for the whole duration of a replayed call, so nested
  `delayedDespawn`/spawn calls inside replayed damage were neither filtered
  nor synced.  Enemy deaths only stayed in sync because the RNG seed was
  replicated.
* The server still ran the wingman AI (`ovr163:160E`, dispatched from
  `ovr141:28D0`) for a human-flown slot: it fired guns (bolts that only
  existed on the server) and changed AI state on damage.
* Shield recharge (`ovr143:340E`) ticks on `frameCounter % rate`, so two
  machines with different frame phases drift by a point; nothing corrected
  that.
* The forwarded mission status was read after it had been zeroed, so a client
  landing told the server `Proceed`, and a client death reached the server
  while the client's own game was held in space by the status suppression.
* **Wingmen going KIA.**  The only write to the pilot-status array
  (`dseg:C260`) is `ovr144:02E9`, in a routine called solely from
  `outerSpawnExplosion` (`ovr143:1C37`): a pilot is KIA exactly when a
  piloted ship explodes.  The NPC-model damage above exploded the other
  human's ship copy, which is why wingmen came home KIA.  With ownership
  rules no machine explodes a remote player's ship, so the path is closed.
* **Missiles not locking for the wingman.**  A missile inherits the shooter's
  locked target from `dseg:C284[shooter]` at fire time
  (`ovr143:2C1F`, `fireGunFromShip`).  That byte is written by the shooter's
  own targeting code, so it only exists on the shooter's machine; the old
  code fired the client's missiles *on the server* using the server's byte
  for that slot (no lock), and the server's copy drives the missile.
  `WeaponFire.target` now carries the lock and the replaying side sets the
  byte before calling `fireGunFromShip`.
* The per-instruction hook read ~15 words of guest memory for every emulated
  instruction (`isExecutingOverlay` per hook).

## 2. Authority model

* **The server owns the mission.**  Spawns, despawns, the AI and damage to
  NPC ships happen on the server and are replayed on clients as events
  (`Spawn`, `Despawn`, `WeaponFire`, `Damage` with the RNG seed).
* **Each human's ship is authoritative on that human's machine**, because
  only there is it slot 0.  Damage to it is computed *locally* from that
  machine's own collision detection (victim side), never replayed.  The owner
  replicates the resulting `ShipHealth` snapshot every frame it changes (and
  every 30 frames regardless); everyone else writes the snapshot into their
  copy.  No machine ever runs `do_damage` or a despawn against a remote
  player's ship, at any nesting depth (`wcnet_events.cpp`,
  `on_do_damage_entry` / `intercept_despawn`).
* **NPC health is also replicated** by the server, applied after the frame's
  events have been replayed, so replay drift (recharge phase, RNG) is
  corrected every frame while cosmetic effects still come from the replay.
* Mission setup despawns every ship slot before spawning the mission; those
  calls run locally on every machine (they reset the slot's pilot to 0xff, its
  comm flags and cull status).  Only despawns of ships that exist are
  broadcast or, on a client, left to the server.
* **Asteroid and mine fields belong to each machine.**  The game never has
  more than twenty rocks or mines: overlay 168 keeps them around slot 0 while
  it is inside a field (`ovr168:0B86` tests every 16 frames whether the
  player is within a field's radius, `ovr168:0AA7` then adds rocks ahead and
  drops the ones left behind), so there are not enough for two pilots and
  nothing about them is replicated.  A field is a mission "ship" of type
  0x16 (asteroids) or 0x17 (mines): `outerSpawnShipEntity` only adds it to
  the nav point's field table (`ovr145:1183` -> `ovr168:0B40`, table at
  `dseg:CDF6`, count at `dseg:BFF6`).  Every machine sets a nav point up for
  itself when its own ship gets there (`seg001:2029`, by distance, calling
  `ovr145:098F`), which empties that table first, so the spawn hook lets
  field "ships" through on every machine instead of broadcasting them.
  Before, a client replayed the server's field spawn and then wiped it with
  its own nav point setup, whose spawn calls are suppressed: only the host
  ever saw a rock.  (The server also sent the garbage that call returns in
  AX as the field's "slot", and stamped a pilot byte from it.)
  What the rocks do is decided where they are:
  * a player's shots break that player's own rocks (bolt against rock never
    goes through `do_damage`: `ovr141:22CD` breaks it one time in three);
  * a rock or a mine blast hitting the machine's own ship is ordinary local
    damage to slot 0, replicated as that ship's health;
  * nothing a machine's rocks do touches another human's ship there;
  * a client's rock or mine hitting a ship the server owns is *reported*:
    the client's `do_damage` hook sends a `Damage` event (target, quantity,
    direction) when the source belongs to no ship, the server applies it
    with no source (`DamageJob::REPORTED`) and broadcasts it and any kill
    like its own.  The game only lets a rock it drew that frame hit a ship
    (`ovr141:2411` removes an unseen one quietly), so a report always is a
    collision that player watched.
  The server's AI runs where only the host's rocks exist, so an enemy near a
  wingman cannot steer around that wingman's rocks.
  A rock is as deadly as the game makes it, alone or not: a collision does
  `(closing speed)^2 / 2` (`ovr141:25AB`, speeds in units per frame, 250 kps
  being 30), a new rock is given 52 to 75 units (`ovr168:040F`, a table
  indexed by `dseg:BFF4`: 56 in the Hornet of Enyo) plus up to 15 less a
  random part, and a Hornet's front shield and armor are 85 together, so one
  head-on rock (1800 and 2592 were measured, the first with networking off)
  kills outright.  What multiplayer adds is that both pilots
  now fly through rocks and either one's death ends the mission for both.
* **The host can switch the fields off** (`WCROCKS=0`, the lobby's
  "Asteroids and mines" box, `/rocks off` in the lobby chat or in the comms
  prompt; `Session::request_rocks`).  Off, the spawn hook answers a field
  "ship" with no slot on every machine, so a nav point has no field and the
  autopilot flies through.  The server's choice travels in the briefing
  message (before a client sets up its first nav point) and in `Frame.rocks`
  with the start state and whenever it changes in flight; at that frame each
  machine runs the game's own `ovr168:01AE` to drop its rocks, or registers
  the current nav point's fields again (`outerSpawnShipEntity` for each field
  ship of `dseg:C178`'s nav point).
* A hit or a kill names its shooter only when a ship is behind it; a rock's
  or a mine's slot number means nothing on another machine.
* The server skips the AI think function for a human-flown slot and refuses
  AI fire from it; the set-speed AI is skipped as before.
* Positions of a player's ship come from its owner every frame; the server
  broadcasts all positions.
* **Pilot identity of a human's ship.**  The comms code names and draws a
  speaker from the ship's pilot byte (`dseg:D1A2`): 0-7 are the eight named
  wingmen (name, face, phrase table and the talk-to-wingman menu index), 8 is
  the player, 9-12 enemy aces.  A player's own ship carries 8, which names
  nobody on another machine, so health snapshots never overwrite that byte
  for a human's ship (`write_health(..., keepPilot)`): on the server the
  client's body keeps the pilot the mission setup gave the wingman slot (the
  mission's wingman), and the client's body for the host wears the pilot the
  server reports for that spawn (`Spawn.pilot`, stamped when the spawn event
  leaves, after the mission setup has run), or `WCHOSTPILOT=0..7` to pick
  another named pilot.  The replayed spawn of an NPC gets its pilot the same
  way.  Before this, both machines saw "(null)" with a random face and a
  garbage orders menu for the other human.
* **Shared fate.**  Any player's ending (landed, died, ejected, quit) ends
  the mission for everyone with that same status.  A client reports its own
  ending with `PlayerEnd`; the server adopts the status, its main loop exits,
  and its `MissionEnd` frame carries the status to every other client.
  Nobody's ship is despawned for this, so no pilot is ever marked KIA by it.
* **After a death the same mission is flown again.**  `runHangarMission`
  returns nonzero after a landing or an ejection (the game's main loop then
  calls it again: rec room, barracks, next briefing) and zero after a death,
  the loss of the carrier or a quit, which sends the main loop back to its
  title call.  In a forced-mission game (`MIS` / `SERIES`, the lobby's
  mission menu) that title call is where the hook starts the mission
  (`afterStartup`), and it now starts the mission most recently flown on that
  machine rather than the one the session began with; `run_campaign` returns
  into the main loop's own test (`afterHangarMission`), so the first mission
  behaves like every later one.  Both machines get there (shared fate), the
  client asks for the briefing and the server answers when its own restart
  reaches the mission start.  Before, a death in the first mission dropped
  into the game's new-game routine, which begins in the simulator, and later
  deaths restarted the session's first mission.  A game started from the
  barracks (no forced mission) keeps the original behaviour: back to the
  title, and the save games.
* **Funerals.**  The scripts hold a funeral for each named pilot whose word in
  `dseg:C260` (eight words, `series * 4 + mission` of his death, 0 = alive)
  names the mission just flown.  Both sides zero all eight at every mission
  start and end (`liven_everyone`; it used to leave the last two and a half
  untouched), so nobody is mourned who is alive on another machine.
* Clients fire their own guns immediately (`FireJob::PREDICT`) and tag the
  event with `client_seq`; the server replays it, and the echo lets the client
  map the missile slot instead of firing twice.

## 3. Frame exchange

Unchanged lockstep from the original design.  At `seg001:20E3` (top of
`main_loop`) the client sends its `ShipUpdate` (+ events, + health) and blocks
for the server's frame; the server blocks for one message per client in the
mission, merges, and sends one frame to each.  The server sends exactly one
frame per client message so the two sides never drift in message count; the
autopilot and mission-end flushes (`exchange(true)`) still consume one client
message each.  Events are replayed through the trampoline before the game
simulates the frame; the client applies received health snapshots when the
trampoline drains (`on_trampoline_idle`).

## 4. Modules

| file | role |
|---|---|
| `wcnet_memory.h/.cpp` | data-segment map with IDA names, typed accessors, `ShipHealthState` |
| `wcnet_code.h` | stub/overlay/root addresses of every hooked function and location |
| `wcnet_vm.h/.cpp` | interception helpers (`call_arg16`, `return_from_call`), `GameCall` thunks, the `Trampoline` job queue |
| `wcnet_transport.h/.cpp` | TCP framing, `Connection` with per-category queues |
| `wcnet_entities.h/.cpp` | `NetworkShipId`, client `EntityMap`, server `SpawnRegistry` |
| `wcnet_events.h/.cpp` | interception policy for damage/fire/spawn/despawn/AI, replay jobs |
| `wcnet_session.h/.cpp` | `ServerSession` / `ClientSession`: lifecycle, handshake, frame exchange, health replication, chat |
| `wcnet_hooks.cpp` | the per-instruction dispatcher (IP bitmap prefilter) |
| `wc_net.h` | the public API used by the CPU core, the shell command and the GUI |

### Running game code

`GameCall(seg, off).arg(..).invoke()` writes a far-call thunk into an unused
error string in the data segment (`dseg:0395`) and jumps to it with the
current CS:IP pushed as the return address.  The `Trampoline` is a 6-byte stub
(`push bp / mov bp,sp / nop / pop bp / retf`) at `dseg:0187+101`; its `nop` is
a hook point.  `run_instead_of_current_call()` is used at a function entry so
the stub's final `retf` returns to the intercepted function's caller;
`run_before_current_instruction()` pushes CS:IP first.  Each `VmJob::start()`
issues one `GameCall`; `finish()` runs when it returns (this is how the slot a
spawn used is learned).  `return_from_call(ax)` skips an intercepted function
by popping its far return address, the Borland cdecl caller cleans up the
arguments.

### Hook points (`wcnet_code.h`)

| hook | where | purpose |
|---|---|---|
| `do_damage` | `ovr143:0A99` | damage policy + broadcast |
| `fireGunFromShip` | `ovr143:2978` | fire policy, prediction, broadcast |
| `delayedDespawn` / `despawn` | `ovr143:1F15` / `ovr140:1C16` | despawn policy + broadcast |
| `outerSpawnShipEntity` | `ovr145:115D` | spawn broadcast, slot registry; asteroid and mine fields run locally |
| AI think / set speed | `ovr163:160E` / `ovr143:0874` | suppress for human-flown slots |
| mission starting / ended / score | `ovr161:0470` / `04DD` / `0251` | briefing handshake, mission end sync |
| main loop top, status checks | `seg001:20E3`, `20F4`, `2108`, `20F2` | frame exchange, client mission-status policy |
| autopilot | `ovr133:0003`, `ovr133:05C9`, `seg001:1695` | camera replication, client key lockout |
| barracks, startup | `ovr150:105D`, `seg001:0512`, `seg001:04F2` | connect handshake, direct mission start |

## 5. Protocol additions (`src/wc.proto`)

* `ShipHealth` inside `ShipUpdate.health`: shields, shield max, armor
  quadrants, damage points, core HP, hull counter, state byte, gun damage,
  engine flag, gun energy (the arrays are listed in the proto comments).
* `WeaponFire.client_seq` for predicted client shots, `WeaponFire.target`
  for the shooter's missile lock.
* `Frame.player_end` (`PlayerEnd`) for a client leaving the mission.
* `Spawn.pilot`: the slot's pilot byte once the mission setup has run.
* `Frame.rocks` and `ServerSendBriefingStart.rocks`: asteroid and mine fields
  on (1) or off (0), the server's choice.
* A client's frame may carry `Damage` events as well as `WeaponFire`: hits by
  that client's own rocks or mines on a ship the server owns (no shooter, no
  seed).  The server ignores one that names a human's ship, a missile or an
  empty slot.

Old peers are not wire-compatible with these semantics (a client that does
not replicate health would be trusted as authoritative for a ship it never
damages), so run the same build on both machines.

## 6. Running and testing

Server: `WCPORT=13255 src/dosbox -c "mount c wc" -c c: -c wc` (or
`runwc.sh <dospath>`).  Client: same with `WCHOST=<server>`.  The `WCNET`
shell command still starts/stops the server or connects.  Useful environment:

| variable | meaning |
|---|---|
| `WCNET_LOG=0..3` | log verbosity (1 default; 2 per event; 3 full protocol) |
| `MIS=<n> SERIES=<n>` | jump straight into a campaign mission (series 1.., mission 0.. within it; setting either variable is enough, so `MIS=0 SERIES=1` forces the first mission) |
| `WCCALLSIGN`, `WCLASTNAME` | override the pilot identity |
| `WCHOSTPILOT=0..7` | on a client: which of the eight named pilots the host's ship appears as (default: the one the server's mission setup gave that slot, i.e. the mission's wingman) |
| `WCROCKS=0` | on the host: fly without asteroid and mine fields (everyone follows the host; `/rocks on` / `/rocks off` in the comms prompt switch it in flight) |
| `WCNET_AUTOKEYS=1` | test aid: press Enter through the briefing, then `A` (autopilot) once in space |
| `WCNET=0` | fly alone: no server, no room, the game's own wingman (a single-player control for experiments) |
| `WCNET_KEYSCRIPT="8:c,9.5:1,10:@after"` | test aid: once in space, at each emulated second press that key (letter, digit, `enter`, `esc`, `space`, held 150 ms) or, for `@tag`, dump the data segment to `WCNET_DUMP_DIR/tag.bin` and log the comm line, VDU text, mission, pilot bytes and KIA words; `!status=N` ends the mission as the game would (1 landed, 4 died, ...), `!kill=SLOT` destroys a ship on the server through the broadcast path of a real kill, `!poke=HEXOFF:HEXBYTE` writes the data segment, `!mouse=X:Y` puts the mouse pointer at those fractions of its range, `!button=N:1` / `N:0` presses and releases a mouse button, `+key` / `-key` hold and release a key, `!pos=X:Y:Z` moves the machine's own ship, `!face=SLOT` turns it towards a slot (`npc`: the first ship no human flies, `rock`: one of its field's rocks), `!rock=SLOT` puts one of its rocks or mines on that ship so the game's collision code finds them touching (`!rock=0`: dead ahead of its own guns instead), `!rocks=1` / `!rocks=0` switches the fields as the host's lobby option does, and every `@tag` line also lists the field state (`fields N inside|outside rocks <slots>`), and `wait` holds the script until the next mission's first frame and restarts the clock there; `WCNET_AUTOKEYS` then only handles briefings and debriefings |
| `WCNET_SKIPBARRACKS=1` | test aid: never stop in the rec room or the barracks, so a scripted run goes from a debriefing straight to the next briefing |
| `WCNET_WATCH=C260:16` | debugging aid: log every change of that part of the data segment (hex offset, length) with the address of the instruction after the write |
| `WCNET_AUDIOLOG=1` | test aid: every 500 ms log each mixer channel's mean level and the number of OPL register writes (stderr, same clock as the key script); `WCNET_WAVE=1` also records the mixed output into DOSBox's capture directory |
| `WCNET_DUMP_FRAME=<n>`, `WCNET_DUMP_FILE` | dump the data segment at in-flight frame n (and on SIGUSR1) |

`scripts/wcnet-smoke.sh [seconds] [mission] [series]` starts a headless
server and client (dummy SDL drivers), lets them fly mission 1 with
`WCNET_AUTOKEYS`, and prints both logs.  It exercises the handshake, the
mission start state, spawn mapping, autopilot, enemy fire/damage replay and
the client's local damage model.  It does not exercise chat, landing, or a
client death (nothing shoots back headless for long enough).

The default build needs no special flags (`include/setup.h` no longer uses
dynamic exception specifications); `make -j8` after `./configure`.

## 6a. The browser build

The same code runs in a web browser (`scripts/build-web.sh`, see the
top-level README for the workflow).  What differs:

* **Blocking becomes suspension.**  Emscripten
  [Asyncify](https://emscripten.org/docs/porting/asyncify.html) instruments
  the whole program, so `Connection::recv`, `Listener::accept` and
  `LobbyHub::join` can wait for the network from inside
  `CPU_Core_Normal_Run` exactly as they do natively: the wasm stack is
  unwound, the page's event loop runs (delivering WebRTC and lobby events,
  keyboard input, audio), and the stack is rewound when the awaited message
  arrives.  `Normal_Loop` also yields to the browser every ~10 ms
  (`emscripten_sleep`, `EM_ASYNCIFY` in `src/dosbox.cpp`).
* **The transport is JavaScript.**  `src/wclobby_web.js` implements the
  `wclobby.h` C API (the one `wcnet_lobby.cpp` uses) on top of lobbylink's
  browser client (`lobbylink/clients/ts`), with the same generation /
  hangup semantics as the Rust crate, so browser and native players can
  share a room.  Blocking API calls use `Asyncify.handleSleep`; the state
  machine lives in JS and rewinds the wasm from event handlers only (never
  from inside a call made by compiled code).
* **No TCP.**  `init_network` refuses to start without `WCROOM`.  Only the
  room-code transport exists in a browser.
* **The simple CPU core runs the hooks too.**  The Emscripten build defaults
  to `core=simple` (`src/dosbox.cpp`), so `CPU_Core_Simple_Run` calls
  `wc_net_check_cpu_hooks()` before every instruction exactly like the
  normal core; the hooks are core-independent.
* **Frames are pushed as whole-screen updates.**  The chat overlay in
  `GFX_EndUpdate` reports the entire frame as changed; the surface output
  (the browser's renderer) only pushes changed runs, so that table has to
  be right (it was inverted before, which the texture output natively never
  noticed).
* **The game files come from the player.**  `web/gamefiles.js` unpacks a
  `.zip` (browser `DecompressionStream`) or an Inno Setup / GOG installer
  (innoextract compiled to wasm, run in `web/inno-worker.js`), recognises
  the game by its executable, installs the game directory under `/game` in
  the memory file system and caches it in IndexedDB.  Only Wing Commander 1
  gets `WCROOM` (the hooks are for `WC.EXE`); other games run single-player.
* **The page joins the room first** (roster, chat, host-driven start) and
  DOSBox adopts that `P2PGame` (`Module.lobbyGame`) in `wclobby_connect`
  instead of connecting again, which would supersede the page's session.
  Lobby chat and presence are reliable messages prefixed with
  `57 43 4C 01`; a protobuf message never starts with 0x57 (wire type 7),
  so `wclobby_web.js` drops them before they reach the game.
* **Lobby traffic shares the data channels**: the page's roster, chat and
  "start" messages begin with `WCL\x01`, which no protobuf message can, and
  both the browser transport (`src/wclobby_web.js`) and the native one
  (`src/cpu/wcnet_lobby.cpp`) drop them before the game sees a byte, so a
  native DOSBox can join a room a browser hosts.
* **Configuration is the environment**, set by `web/wc.js` from the page's
  form (`Module.ENV` in `preRun`): `WCROOM`, `WCLOBBY`, `WCPLAYERS`,
  `WCCALLSIGN`, `WCLASTNAME`, `WCLOBBY_RELAY`, `WCNET_LOG`, plus anything
  given as `?env.NAME=value` (so `MIS`, `SERIES` and `WCNET_AUTOKEYS` work
  for testing, as in `scripts/web-smoke.sh`).
* **Asteroids and mines** are the host's lobby option (a box beside the
  mission menu, `/rocks on` and `/rocks off` in the lobby chat, `?rocks=0` in
  the host's link).  The page tells the other pages (`rocks` in its `hello`
  and `start` messages), starts its game with `WCROCKS`, and while flying
  calls `wc_web_set_rocks`; the host's game then tells every wingman's game
  through the game protocol.  A page in full screen has no chat: leave full
  screen, or type `/rocks off` in the game's own comms prompt (`0`).
* **Save games.**  A game's registry entry (`web/gamefiles.js`) lists its
  save files (Wing Commander: `GAMEDAT/SAVEGAME.WLD`, all eight bunks).  The
  page keeps a copy in local storage, writes it back into the game directory
  at every start, re-reads the file every few seconds while the game runs and
  stores it when it changed, offers it as a download, and can restore it from
  a file of the same size.
* **Speed.**  The registry also gives the game's `cycles` (3630 for Wing
  Commander: DOSBox's default of 3000 raised twice with Ctrl+F12), passed as
  a `cycles=` command before the game starts; `?cycles=N` overrides it.
* **Caps Lock is Esc.**  Browsers keep Esc for leaving full screen, so the
  page swallows Caps Lock while the canvas has the keyboard and injects a
  150 ms Esc press instead (`wc_web_tap_escape`); a lone key-up counts as a
  tap, because a Mac reports Caps Lock as key-down when it locks and key-up
  when it unlocks.  Where the Keyboard Lock API exists (Chrome, Edge) the
  page also asks for Esc in full screen: a tap reaches the game, holding it
  leaves.
* **Controllers** (`web/gamepad.js`).  The lobby has a controller picker
  and a table of buttons and axes; a chosen controller is presented to the
  game as its mouse and keyboard.  Wing Commander steers by its mouse
  pointer: it parks the pointer at 318,52 of the 640x200 mouse range (the
  middle of the cockpit view) and turns towards it, right of the neutral
  point to turn right, above it to raise the nose (the registry's `pointer`
  entry).  In flight the sticks set the pointer around that point (left
  stick turn and pitch, right stick pitch and, through the roll keys, roll;
  each axis can be inverted and pitch is by default, so pulling back climbs);
  the pointer is re-sent every 200 ms because the game re-centres it itself.
  Outside flight the stick moves the pointer like a mouse and the Fire guns
  button is also the click.  Buttons hold keys: A Space (guns), B Enter
  (missile), X `T`, Y `W`, L1 `+`, R1 `-`, Back `N`, Start `A`, and the left
  trigger Tab (afterburner) only past 90% of its travel, releasing below
  80%.  The page queues keys, the pointer and the click through
  `wc_web_key`, `wc_web_pointer` and `wc_web_mouse_button`; the emulator
  applies them in its async tick, and `wc_web_in_flight` tells the page which
  of the two modes applies.  The choice of controller is per window (session
  storage, announced to other windows over a `BroadcastChannel` so a
  controller taken by one window is not picked up by the next); the mapping
  is shared (local storage).  The page reads the controller from a timer, so
  a window that is visible but not focused keeps flying: two windows side by
  side are a split screen.  The DOS game-port joystick is off in this build
  (`joysticktype=none`), or a gamepad would reach the game twice.
* **The lobby server checks the page's origin**; the public server accepts
  only its own host.  Serve the page from there or run a lobby server with
  `--allowed-origin` for the page's origin.

## 7. Known limitations and follow-ups

* The exchange is still blocking lockstep: both games run at the pace of the
  slower machine plus one round trip.  The next step for smoothness is to
  pipeline it (send frame N, consume the server's frame N-1) with velocity
  extrapolation for remote ships; `ServerSession::exchange` and
  `ClientSession::on_frame_top` are the only places that would change.
* A server with no connected client blocks at the start of every frame until
  one connects (original behaviour); the same happens if the only client
  drops mid-mission.
* A client that died cannot rejoin the mission in progress; it gets the next
  briefing.
* Only two client slots (network ids 1 and 3) are allowed, as before.
* Rocks and mines are per machine (section 2): two pilots in the same field
  do not see the same rocks, and one can watch the other fly through a rock
  unharmed.
* Bolts fired by a remote ship are spawned on the client one frame later
  from the replicated position, so a fast-turning ship's shots can appear
  slightly off.

## 8. Reading the game

`scripts/wcdis.py` resolves any of the address forms used in this document
and disassembles with symbols from the IDC export:

```
scripts/wcdis.py sym do_damage           # ovr143:0A99, stub143:0084, DOS 12D7:0084
scripts/wcdis.py dis do_damage           # symbolized listing until the next name
scripts/wcdis.py dis ovr143:0EDB 0x300   # explicit range
scripts/wcdis.py callers stub143:0084    # every far call site, root image and overlays
scripts/wcdis.py entries stub143         # the stub's thunk table
scripts/wcdis.py names dseg              # all named data
```

Facts the tool encodes that are easy to get wrong by hand: overlay code is at
`FBOV + 0x10 + stub.fileofs` in `WC.EXE`; far-call segment operands inside
overlay code are `8 * segment-index` (index N is IDA's `segNNN` or
`stubNNN`), patched by the overlay loader; IDA segments may start unaligned
(`seg001` starts at 0x3BE8 with paragraph 0x3BE), so offsets are relative to
`paragraph * 16`; DOS segment = IDA segment + 0x1A2.
