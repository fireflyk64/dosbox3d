# TIE Fighter (1995 Collector's CD-ROM) in the multiplayer layer

The user's brief (2026-11-06): extend the layer to a new game altogether,
TIE Fighter as GOG sells it (`setup_star_wars_tie_fighter_collectors_cd`),
the way Wing Commander is flown: the player buys the game, drops the
installer into the page, and the emulator's hooks drive the game's memory.
TIE Fighter brings challenges of its own: no spheres of action, so a mission
has many craft at once; the near ones are small and precise shots count, so
a jerking enemy is no fun; there are lasers everywhere.  The brief's design:
far craft updated a few at a time, every ten to twenty seconds; a guest
takes control of the craft near its ship or in its sights unless the host
has them, and arbitrates its own lasers' hits; far-off shots are left to
each machine, only what is within the lasers' reach (some 2.5 km) kept in
step.  Nothing of this may change Wing Commander or RetroVega.

This document is the reference for what is built, what was found, and the
plan.  Status: the page takes the installer and the emulator finds and
watches the program; no shared flight yet (section 6).

## 1. The game as installed

GOG's installer (424 MB, Inno Setup 5.6.2) unpacks to 698 MB: the game's
own files (resources, missions, the cockpits for 320x200 and 640x480, the
cutscenes' streams) and `game.gog`, a raw image of the CD (594 MB, 2352-byte
sectors, one data track: `game.ins` is its cue sheet).  The CD holds the
same files again plus `VOICE/` (1,325 files, 445 MB, the in-flight voices),
and the game quits at once without it ("ERROR #3: You must insert
TIE-Fighter CD"): the disc must be in drive D.  GOG runs it with
`imgmount d game.gog -t iso` and `tie.exe` (`__support/app/dosbox_tiecd*.conf`:
machine svga_s3, memsize 16, cycles fixed 60000, SB16 on IRQ 7, EMS on).

The programs: `TIE.EXE` (31 KB, a 16-bit Watcom launcher) runs
`Z_TIE2__.EXE`, which runs `DOS4GW.EXE Z_TIE__.EXE`: the game is
`Z_TIE__.EXE`, a 32-bit LE program under the DOS/4GW extender (1 MB: a code
object of 0xAAE83 bytes at image address 0x10000 and a data object of
0x46470 at 0xC0000; entry 0x7B4E4).  `IMUSE.EXE` is the music server.
`TOPACE10.TFR` and `TOPACE7.TFR` (GOG's `__support/save/`, and the CD) are
pilots with the campaign done, for a start past the training.

## 2. The page

`web/gamefiles.js` registers `tf`: detected by `TIE.EXE`, run as `tie`,
`image: "game.gog"` (mounted as drive D by `imgmount ... -t iso` before the
game: web/wc.js), `keep: ["IMUSE.INI"]` (the installer-junk rule drops
`.ini` files otherwise), the machine GOG's configuration asks for, 60,000
cycles, `noFlight` and `multiplayer: false` until there is a session.

The browser's cache (IndexedDB) refused the game in one record: Chrome
keeps a record under some 127 MB ("the serialized keys and/or value are too
large", measured: max 133,169,152 bytes).  So `saveGame` now writes big files
(8 MB and up, and everything past 64 MB of inline files) as 32 MB parts in a
second store, "parts", and `loadGames` reads them back (database version 2;
version 1 records, every file inline, read as they are).  Measured in
headless Chrome: the installer unpacks in the browser in 13 s (1.4 GB of JS
heap at the peak), the save takes 7 s, the reload finds "TIE Fighter
(685.3 MB)".  Blobs were tried first: headless Chrome's IndexedDB failed to
write a 600 MB Blob ("Failed to write blobs").

The browser emulator had no CD image support at all (`drive_iso.cpp`,
`cdrom_image.cpp` and the `imgmount -t iso` path were left out under
`EMSCRIPTEN`, over a worry about SDL mutexes that the SDL port's no-op
mutexes answer); `dos_mscdex.cpp`'s ISO case was behind the same guard, so
`imgmount` said "no ISO/CUE image" with the file intact.  All of it is
compiled in now; no CD audio is ever played (data tracks only).

## 3. The program in memory (src/cpu/wcnet_tie.cpp)

DOS never tells the layer where DOS/4GW put the program (the extender loads
the LE itself, into extended memory), so when DOS loads `DOS4GW.EXE` the
layer looks for it every 250 ms for a minute: the data object's rank table
(20000, 50000, 100000, 250000, 500000 at image 0xC1E10) and resource path
(`RESOURCE\` at 0xC1C98), tried at every dword of memory above 1 MB with
the fault-free reads (`mem_readd_checked`), then the code object by the
first bytes of `tie_updatetime` below it (`53 51 52 56 57 55 8b 2d`) and of
`laser_createprojectile`.  Found: the code at linear 0x1FA000 and the data at
0x2A5000 in our DOSBox (16 MB): the objects are packed page-aligned, 0xAB000
apart, not the LE header's 0xB0000, hence the measured gap.  Paging is off
and the selectors flat (CS base 0): EIP and every pointer the program holds
are linear addresses, and image address A is at `tie_linear(A)`.

The hooks' per-instruction test knew only 16-bit IPs (`wc_net_watch`); a
second table, `wc_net_watch32`, holds the low words of 32-bit EIPs and the
dispatcher calls `tie_check_hooks` on a hit (the full address compared
there).  A 16-bit game never has an EIP past 0xFFFF, so Wing Commander and
RetroVega take the same path as before.

The addresses come from OpenTIE (https://github.com/elyosh/OpenTIE, GPL-3),
a decompilation-faithful C port of this very build with the retail
addresses in its comments (`// GLOBAL: TIE95 0x...`, `// FUNCTION: TIE95
0x...`; its Landru front-end engine is https://github.com/elyosh/landru).
Only the facts are used here (addresses and layouts), no code: this layer is
GPL-2.  Checked against `Z_TIE__.EXE` (file offset of image A: code
0x3D800 + A - 0x10000, data 0xE8800 + A - 0xC0000): the rank table, the
resource path, `spec_data[69]` ("X-W" at 0xC7AB4), the function prologues.

What is watched now, and logged once a second (`WCNET_LOG=1`):

| image address | what | notes |
|---|---|---|
| 0x577F4 | `tie_updatetime` | once a frame: the clock |
| 0x5CDA0 | `user_inputforplane` | the player's controls, once a frame |
| 0x32448 | `move_moveobjects` | every object moved, once a frame |
| 0x2E0E4 | `laser_createprojectile` | a bolt or warhead fired |
| 0x1433C | `collide_laserhitcraft` | a bolt meets a craft |
| 0x148F0 | `collide_damagecraft` | damage applied |
| 0x17BF8 | `create_createcraft` | a craft made (10 at the first Combat Chamber mission's start) |
| 0x16B8C | `create_createmission` | the mission set up |

The flight's data (all from OpenTIE; sizes in bytes):

* `FlightObject objects[120]` at 0xE38BC, 88 each: craft in slots 0..31,
  projectiles 32..79, explosions and debris after.  +0x03 genus (0 fighter,
  1 transport, 3 freighter, 4 starship, 5 platform, 6 player projectile,
  7 NPC projectile, 8 mine, 13 explosion), +0x04 ship type (0: free slot),
  +0x06/0x0A/0x0E int32 x/y/z, +0x12.. the previous frame's, +0x1E heading,
  +0x20 pitch (0x4000 level), +0x22 roll (16-bit angles), +0x26 speed
  (0..3600), +0x2A collision radius, +0x2C death timer, +0x2E age, +0x30 own
  index, +0x33 side, +0x35 flight group, +0x3A.. move vector, +0x42.. the
  axes, +0x54 pointer to the CraftData (linear).
* `CraftData crafts[32]` at 0xE6394, 0x21A each: +0x2C target, +0x42
  attacker, +0x61 AI manoeuvre, +0x6A max speed, +0x9E throttle (65,500 =
  100%), +0xA2 hull damage, +0xA4 hull strength, +0xA6 hull max, +0xCA/+0xCC
  front/rear shield, +0xCF is the player's, +0xD3 laser types, +0xDC laser
  cooldowns, +0xE1 warhead types, +0x142 sixteen weapon slots.
* `pstate` at 0xEB158: +0x08 the player's object slot, +0x0A flight group,
  +0x12 the target's slot.  `frameticks` 0xEB712 (4 ms ticks the last frame
  took), `framerate` 0xEB70C (236 / frameticks), `maingameflag` 0xEB763,
  `hyperspaceflag` 0xEB75D.  Projectiles: a FlightObject whose craft
  pointer is an 8-byte warhead record; speed = the shooter's + the type's
  (`projectilevelocity[]` 0xC545C), life in ticks (`projectilelife[]`
  0xC548C: lasers 2 to 5 ticks of 236/s... i.e. seconds x 236).
* Units: 236 ticks a mission second; the clock runs at 250 Hz (4 ms ticks),
  so a frame of 4 ticks is 59 "fps".  Waypoint = km x 160, world = waypoint
  x 256: 1 km = 40,960 units.  The front end's pointer: `mouse_cursor_x/y`
  int16 at 0xD2BE6/0xD2BE8 (Landru's own, moved by the mouse's mickeys).
* `fg_array[48]` 0xDF6CC (the mission's flight groups, 0x124 each, the .TIE
  file's records in place; limits 48 flight groups, 32 craft at once),
  `inflight_unlimited/invulnerable/collision` int8 at 0xC1553..55 (the Esc
  menu's switches), `math2_randomseed` 0xC5710.

Measured in the first Combat Chamber mission (TIE Fighter, mission 1): 60
frames a second at 4 ticks each, `maingameflag` 1, 10 objects live, the
player's object #0 of type 5 (T/F) at (0, 0, 0), pitch 0x4000, speed 0;
`create_createmission` once, `create_createcraft` ten times, the input and
move functions once a frame.

## 4. Driving the game headless (the rigs)

`tests/run.sh`-style runs with the key script, from boot
(`WCNET_KEYSCRIPT_FROM=boot`), the game's GOG configuration and the disc
mounted (`tf.conf` in the scratchpad: machine svga_s3, memsize 16, cycles
60000, ems on; `imgmount d game.gog -t iso`; no `exit` after `tie.exe`, so
a text-mode `!shot` can read an error).  The key script's aids made for
this: `!shot` now reads text modes (as `.txt`) and 256-colour VESA modes
(the flight's 640x480), `!dumplin=name:hexaddr:hexlen` writes linear memory,
`!tiecursor=x:y` puts the front end's pointer on its 320x200 screen (the
engine moves it by mickeys, so the driver's absolute position means nothing
to it: a `!mouse` lands anywhere), `WCNET_TIE_TRACE=addr,...` logs the
registers at image addresses (the Watcom `int386` thunk at 0xB6F0B is where
every software interrupt goes: `eax` the function).

The route to a flight, in the 320x200 front end (OpenTIE's `register.c`,
`mainmenu.c`, `combat.c`, `brief.c` give the click rectangles): Space skips
the logo, the crawl and the cutscene (Esc opens the options pad instead,
whose GAME button restarts the title); the registration desk lists the
pilots at (75..127, 102..174), a pilot is clicked there (TOPACE10 at
100,105) and the door at (240..320, 80..150) entered; the concourse: tour
of duty (14..170, 28..64) when a battle is on, new tour (82..128, 64..90),
tech room (220..260, 64..90), film room (272..304, 68..94), registration
(0..34, 110..146), training (44..82, 96..126), Combat Chamber (130..168,
86..106); the Combat Chamber's "Enter Briefing" at (268, 148); the briefing
map's "Enter Mission" at (242, 186).  A flight starts 62 s after boot at
60,000 cycles.

In flight (measured): Backspace is full throttle (not Backslash: the
ship then ran into the container ring 8,192 units ahead and died,
`collide_damagecraft(0, 0xFFFF, 1)`), T targets, and the mouse is the
stick: its mickeys turn the ship (the script's `!mouse` moves, relative)
and its left button fires; the keyboard arrows do not steer and Space did
not fire in the Combat Chamber.  A shot is `laser_createprojectile(eax =
the shooter's object, edx = the hardpoint 0 or 1, ebx = the projectile type
0x8C, ecx = 0 or 1)`; the bolt is the object in slot 32 on, genus 6 (the
player's projectile), type 140, speed 900 from rest, its move vector the
nose (Q15), a death timer of 664 ticks, its craft pointer the warhead record
(`warheads[]` at 0xE61FC, 8 bytes each), not a CraftData.  A hit is
`collide_laserhitcraft(eax = the bolt's object, edx = the target's, ebx,
ecx = 2, 2)` and then `collide_damagecraft(eax = the target's object, edx =
the component, ebx = the bolt's object, ecx = 0)`; a collision's damage has
ebx 0xFFFF for the source.  The first Combat Chamber
mission as the tables show it: eight containers (genus 3, type 29, radius
7,996, side 2) in a ring 8,192 units out, a platform (type 61) far off;
`crafts[0]` is the player's (`is_player_craft` 1, throttle 65,535 at full,
max speed 225).

## 5. The sync design (the brief, as the layer would do it)

To be built on the facts above; the Wing Commander session
(`wcnet_session.cpp`) is the model, with these differences:

* **Ownership by distance, not by slot.**  Every craft has an owner; the
  host owns everything to begin with.  A guest owns its own craft and takes
  over any craft within the lasers' reach of it (2.5 km = 102,400 units) or
  in its sights, unless the host, or another guest, has it within reach too:
  the nearer machine owns it, ties to the host.  Ownership moves by a
  message (the old owner's last state goes with it), never more than once a
  second for the same craft, so a dogfight does not flicker between owners.
  The owner's game flies the craft (its AI, its moves, its damage); the
  others set its position, attitude and speed from the owner's reports and
  keep its AI quiet (as RetroVega's puppets: a flag the game honours, or
  the AI's manoeuvre byte at CraftData +0x61 held).
* **Far craft a few at a time.**  Owned craft beyond anyone's reach are
  reported in a round-robin, a handful a frame, so each is refreshed every
  ten to twenty seconds (the brief); within reach of any player, every frame
  (the host paces the flight as Wing Commander's does, WCFPS; the game's own
  60 fps at 4 ticks is the native rate).  A report is the object's position,
  the previous position (its move vector), heading/pitch/roll and speed; the
  receiver writes them and lets `move_moveobjects` carry the craft on.
* **Lasers local, hits arbitrated by the shooter.**  Every machine fires and
  flies its own bolts (`laser_createprojectile` as the game does it); a bolt
  of a far-off craft is not sent at all.  A player's bolt is sent as an
  event (shooter, type, position, vector) so the others see it; its hit on
  a craft is decided on the shooter's machine (`collide_laserhitcraft` there)
  and sent as a damage event, which the craft's owner applies through
  `collide_damagecraft`; the non-owners drop their own hit (the brief: the
  guest arbitrates its lasers' hits).  AI shooters' hits are decided by
  their owner.
* **Events from the host.**  Spawns (`create_createcraft`: flight groups
  arriving, waves), deaths, hyperspace in and out, mission goals and
  messages come from the host, as Wing Commander's.  The guests' games run
  the same mission file with the same seed (`math2_randomseed`), so the
  flight groups and their orders match from the start.
* **The guest's seat.**  The mission's player craft is the host's; a guest
  flies another craft of the player's flight group (the game's
  `PlayerCraft` field, 1-based, says which craft of the group is the
  player's: the guest's game is told a different one, and the host's game
  sees the guest's craft as an AI wingman it does not own).  A flight group
  of one gets a craft added by the host (`create_createcraft`).

## 6. Where this stands, and what comes next

The step guide.  Each step is done when it is measured; its notes go in
section 7 as it is worked on.

1. **Done: the flight's controls from the test aids** (section 4): the mouse
   is the stick, `!mickey=dx:dy` gives the rigs a sustained turn.
2. **Done: the transport, positions reflected** (section 7, step 2): a
   session of TIE Fighter's own, the seats in the player's flight group,
   every pilot's craft sent each frame and written into the others' games.
3. **Done: notices through DOSBox**, drawn by the emulator over the picture,
   nothing patched into the game (the user, 2026-11-06): where each pilot
   is, launches and flights' ends, and a call to action standing while the
   host is at a briefing or flying and a guest is not (section 7, step 3).
4. **Done for the Combat Chamber: the mission everybody flies** (section 7,
   step 4): a guest launching from the Combat Chamber flies the host's
   mission whatever it picked.  Battles (the tour's) are still each pilot's
   own: a guest is told when the two fly apart.
5. **Done, first version: ownership and the variable rates** (section 7,
   step 5): the host flies most craft; a guest flies those within the
   lasers' reach and in its view, or its target, unless the host has it
   targeted (the user's rule); near craft every frame, within 10 km every
   10th, far ones in turn every 10 to 20 s.
6. **Next: lasers and damage**: the shooter's machine decides its hits
   (`collide_laserhitcraft` there), the craft's owner applies them
   (`collide_damagecraft`); a puppet takes no damage of this machine's own
   (collisions included); deaths and spawns from the owner and the host.
6a. **Seen from both cockpits, once the guns are in step** (the user,
   2026-11-06): a gunboat mission, the two players flying side by side, one
   a little ahead so that it is just on the other's screen; one fires
   lasers, the other ions.  Screenshots: the ions on the laser player's
   screen, and both players' fire on the ion player's.
7. **The controls the user asked for** (2026-11-06): the arrow keys steering
   as RetroVega's do, a pointer-steer mode like RetroVega's mouse beside the
   game's own drag-to-steer (the 90s way, for those who want it), and a
   selector between them; all of it in the emulator, fed as the mickeys the
   game reads.
8. **The page**: the room tag TF, `multiplayer: true`, a mission picker.
9. **Missions flown alone, flown by two** (the user, 2026-11-06).  In 37 of
   the 105 mission files the player's flight group is a single craft (read
   from `MISSION/*.TIE`: the group with a player-craft number, +0x42, and a
   count of 1, +0x33).  Today the second pilot gets a second craft of the same
   type; the user's pairings instead, the first player's craft kept (the
   mission's goals may need it), the second's chosen to fit:

   | the mission's craft | flown by two | its solo missions |
   |---|---|---|
   | T/A | T/A + T/A | HA1W |
   | T/D | T/D + T/A | B9M2..B9M6, HD1W, HD2W, HD3M |
   | T/I | T/F + T/F; T/I + T/F if a T/F is too slow for the mission's timing (to be judged mission by mission) | B5M1 (the minefield, unshielded), HI1W, HI3M |
   | gunboat | gunboat + a standard Imperial shuttle (not the escort shuttle; the gunboat's cockpit for the shuttle for now) | B10M1, B10M2, HG1W, HG3M |
   | T/F | T/F + T/F | B1M1, HF1W, HF3M, HF4M, TRAIN |
   | missile boat | missile boat + gunboat, or missile boat + a standard Imperial shuttle depending on the difficulty | B10M5, B10M6, B11M1..B11M7, B12M6, B13M1, B13M4, HM1W, HM2W |
   | T/B | T/B + T/F: one flies the torpedo work, the other point defense | B3M1, HB1W |

   How: a flight group has one craft type (+0x32), so a mixed pair is a
   second flight group for the second pilot, a copy of the player's record
   with the other type and a count of 1 (the mission's 48 at most), the
   goals left on the first; T/I to two T/F changes the group's type and
   count.  The cockpit follows the craft the pilot flies; the shuttle has
   none of its own (the gunboat's).

## 7. Notes, step by step

### Step 2: the transport, positions reflected (2026-11-06)

`src/cpu/wcnet_tie_session.cpp`.  The Wing Commander session is not used:
`g_session` stays NULL for TIE Fighter, whose session is started from its
own tick once the program is found and a room or peer is named (`WCROOM`,
`WCHOST`/`WCPORT`; none: the game plays alone).  The transports are the
layer's: a lobby room's WebRTC data channels in the browser and natively
(lobbylink: a reliable ordered channel and a best-effort one, unordered with
no retransmits, 16,000 bytes a message at most), or TCP natively for the
rigs.  Messages (`src/wc.proto`): `TieHello` (a guest's callsign; the host's
answer gives the guest its seat and the room's seats) and `TieFrame` (a
pilot's craft, `TieCraft`: object slot, place, heading/pitch/roll, speed,
move vector, the nine axis words, type).

* **The seats.**  At `create_createmission`'s entry the player's flight
  group (the record with a player-craft number, +0x42) gets as many craft
  as the room has seats (+0x33), and a guest's game is told it flies the
  next craft of the group that is not the host's.  The mission's own setup
  then makes everybody's craft in the same object slots on every machine:
  measured, the Combat Chamber's first mission puts the host's T/F in
  object 0 and the guest's in object 1 on both, the guest starting at
  (872, -849, 0) beside the host at the origin.
* **Sending.**  At the top of each flight frame (`tie_updatetime`'s entry)
  the sockets are read and our own craft sent, best effort, with a reliable
  copy once a second; the host relays a guest's frames to the other guests
  with its seat.
* **Writing.**  At `move_moveobjects`'s entry, after this machine's AI,
  weapons and dynamics have run and before its move, every puppet is
  written: its place from a report not yet written, its attitude, speed,
  move vector and axes every frame.  The move integrates a craft along its
  move vector (Q15) at its `current_speed`, so between reports the game's
  own motion carries the puppet (the dead reckoning a sparser rate will need).
  The local AI is held off a puppet by keeping its planner countdown
  (CraftData +0x2A, `ai_update_rate_copy`) full: `pai_updateplaneai` runs a
  craft only when it has run out, and with it go the AI's shots.
* **Measured** (two native games on one machine, `scripts/tfnet-duo.sh`:
  the same front-end script on both, the guest 2 s later; the guest at full
  throttle climbing and turning past the host with forty `!mickey=40:-12`;
  reports four times a second stamped with the wall clock, joined by
  `scripts/tfnet-join.py`; the example in the rig's header):

  | link | the copy | samples | place, median / worst | attitude, worst |
  |---|---|---|---|---|
  | TCP | the host's of the guest | 70 | 7 / 51 units | 28 of 65,536 (0.16 degrees) |
  | TCP | the guest's of the host (still) | 70 | 0 / 0 | 0 |
  | lobby room (WebRTC) | the host's of the guest | 82 | 8 / 68 units | 25 (0.14 degrees) |
  | lobby room (WebRTC) | the guest's of the host (still) | 82 | 0 / 0 | 0 |
  | room, the guest a browser page | the native host's of the page's | 330 | 18 / 111 units | 27 (0.15 degrees) |
  | room, the guest a browser page | the page's of the host (still) | 315 | 0 / 0 | 0 |

  The room is a local lobby server's (`LOBBY=1`), its game data over
  lobbylink's WebRTC data channels natively, as the page has them; both
  emulators kept 60 frames a second (the guest dipped to 47 by the container
  ring, as it does alone: the game's own drawing).  The browser page
  (`scripts/web-tf.sh`: a native host and Chrome as the guest; two pages at
  once would not fit the sandbox's 8 GB beside each other's 685 MB game)
  took GOG's installer, joined the room, and flew at 59 to 61 frames a
  second.  Two things the browser rig taught: the page's game has no pilot
  (GOG's `TOPACE10.TFR` is in `__support/save`, which the page leaves out as
  installer junk), so the guest types a name at the registration desk as a
  player does; and a site that serves `wc.tar.gz` makes a fresh page "ready"
  with Wing Commander before the installer is unpacked, so the rig serves
  the site as deployed, without it.

  A T/F is some 1,300 units across: the copy is a fraction of a frame
  behind.  The guest's craft is on the host's screen where its report puts
  it (passing the right canopy strut).
* **Found on the way.**  The session first polled its sockets at every
  periodic tick (every 999 instructions: some 60 an emulated ms at 60,000
  cycles), a `select()` each, and the host's emulator ran at about 85% of
  real time (its flight began 8 s of wall time after the guest's although it
  started 2 s earlier); polled once a frame and every 4 ms otherwise, both
  run at 60 frames a second and their flights begin the rig's 2 s apart.
  A report read at any moment shows a puppet's pitch stepped toward level by
  this machine's dynamics: between a frame's dynamics and its move a puppet
  is this machine's own, so the report is taken at the top of a frame, what
  the last frame drew.
* **Open.**  A puppet still collides and takes damage on the machine that
  only shows it (both pilots flew into the container ring once and each
  died on both machines): step 6.  The rates are one report a frame for
  every pilot's craft: step 5.

### Step 3: notices through DOSBox (2026-11-06)

* **Where each pilot is: the front end's scene.**  `shellext_Get_Cur_Scene`
  (0x6621B) reads `mov eax,[0x356F8]; mov ax,[eax+0x14]`: the scene head's
  pointer at image 0xF56F8 (the address is a fixup into the data object),
  the scene at +0x14 of it (`tie_scene()`).  Walked from boot: 6 to 80 the
  opening, 100 the registration desk, 110 the concourse, 130 and 131 the
  Combat Chamber, 133 its briefing map, 3 its flight (the main-game flag
  1); OpenTIE's `SCENE_*` names the rest (160 the tour desk, 179 to 183 the
  briefing, 190 to 192 the debriefing, 2 a training flight, 4 a battle's).
* **The mission a front end launches** is named in shipext's
  `mission_name`, 14 bytes at image 0xFB3E8 (`shipext_Set_Mission_Name`
  0x822B4; the Combat Chamber fills it from the craft and mission chosen,
  `shipext_Get_Combat_Mission_Name` 0x8149C, as soon as its screen comes up;
  the tour desk from the battle's; the mission file's name is made from it
  before the mission loads).  Its forms: `H` + the craft's letter (F T/F,
  I T/I, B T/B, A T/A, D T/D, G the gunboat, M the missile boat) + the
  number + W or M (`hf1w`: the Combat Chamber's T/F mission 1); `B<n>M<m>..`
  a battle's; `train` the training course (`MISSION/*.TIE`).
* **Presence** (`TiePresence`: seat, callsign, scene, flying, mission), sent
  when it changes and every 3 s, the host relaying a guest's to the others
  with its seat.  On another pilot's launch the overlay's line says so
  ("GUEST launched: the Combat Chamber's T/F mission 1"), likewise its
  flight's end and, to a guest, another guest's arrival with where it is.
* **The call to action** (to a guest): while the host is at a briefing or
  flying and the guest is not flying, "HOST is at the briefing for <the
  mission>: launch it to fly together" or "HOST is flying <the mission>:
  ...", said again every 2 s so it stands, gone once the guest flies or the
  host is back.  The Wing Commander comms hint is not shown over TIE
  Fighter: it has no chat line yet.
* **Measured**: two native games, the guest 25 s behind (`GUEST_LAG=25
  scripts/tfnet-duo.sh`), the window's pictures dumped with the key
  script's new `!frame=` (the overlay is drawn after the game's picture:
  `!shot` reads video memory and never has it) and read back by matching
  the overlay font's glyphs: the guest's screen at the host's briefing and
  in its flight carried the two lines above (the line wraps at the picture's
  edge), the guest's own flight none; the host's carried the guest's
  launch.

### Step 4: the mission everybody flies (2026-11-06)

* **Where.**  The mission file's name is made from the front end's
  `mission_name` (image 0xFB3E8) by `shipext_Get_Mission_Path` (0x82504:
  `"mission/" + mission_name + ".tie"`), called when the briefing map loads
  the mission and again when the flight does.  At its entry a guest's
  `mission_name` gets the host's (a 14-byte memory write), when the guest's
  own is a Combat Chamber mission (`H...`), the host's is too, and the host
  is at a briefing or flying (its presence says so).  The guest is told:
  "Flying HOST's mission, the Combat Chamber's T/F mission 1 (you had picked
  the Combat Chamber's T/F mission 2)".  A battle's file belongs to the
  pilot's tour record (its battle and mission, what it unlocks), so it is
  not swapped; a guest launching anything else while the host flies is told
  the two fly apart.
* **Frames carry their mission** (`TieFrame.mission`): a pilot flying
  another mission is relayed but its craft are not written into this
  game's objects.
* **Measured** (`GUEST_LAG=6`, the guest clicking the Combat Chamber's
  "next mission" arrow, (135,167), then the briefing): the guest's front end
  showed `hf2w`, the swap came as its briefing loaded, it was briefed and
  flew `hf1w` as the player group's second craft, and its craft on the host
  was 12 units off (median of 100, worst 48).
* **The Combat Chamber's buttons** (OpenTIE `combat.c`, ids 1..6 = rects
  0..5 at 320x200): 1 (32,142)-(54,156) the craft back, 2 (124,142)-(146,156)
  the craft on, 3 (22,160)-(44,174) the mission back, 4 (124,160)-(146,174)
  the mission on, 5 (254,142)-(280,158) "Enter Briefing", 6 (10,176)-(38,200)
  out.

### Step 5: ownership and the variable rates (2026-11-06)

* **Who flies a craft.**  The host keeps an owner for each craft slot (0 to
  31: craft, not debris) and sends the table (`TieOwners`) on a change and
  every 2 s; a pilot's own craft is its own.  The rule is the user's: "host
  should own most craft except the ones both within range and on the guests
  viewport or specifically targeted by guest and not targeted by the host".
  A guest gets a craft within the lasers' reach of it (2.5 km, 102,400
  units) and within 45 degrees of its nose (the move vector, +0x3A: a TIE
  craft does not slide), or one it has targeted within 4 km; a craft the
  host has targeted (pstate +0x12) stays the host's (read as applying to
  both cases, in the spirit of the brief's "unless the host has them").  A
  craft leaves a guest only clearly (1.25 times the reach, 60 degrees off
  the nose, the target dropped) and changes owner at most once a second; of
  two guests, the one that targets it first, then the nearer.  An earlier
  rule (the nearest pilot within reach) was replaced the same day.
* **Rates.**  Each machine sends its own craft every frame, and the craft
  it flies for the others by their distance from the nearest other pilot:
  within 2.5 km every frame, within 10 km every 10th frame, the rest one at
  a time every 30 frames in turn (so each far craft every 10 to 20 s; a
  frame with a far craft goes reliably, its next report being far off).
  Frames name the sender's craft and target (`own`, `target`).
* **Puppets.**  This machine's AI is held off every craft another flies;
  a craft it takes over has its AI released (the countdown set to 0: it
  plans at once).  A report is written only from the craft's owner by this
  machine's table, and only into a slot holding the same ship.  The puppet
  table holds protobuf messages and had been cleared with `memset`: cleared
  field by field now.
* **Measured** (the Combat Chamber's T/F mission 2, HF2W: a battle under way
  from the start, 13 craft; the host at full throttle ahead, the guest
  turning away; `scripts/tfnet-join.py ... all`):

  | the copies | near (2.5 km) | mid (10 km) |
  |---|---|---|
  | the guest's of the host's 12 craft | 711 samples, 28 units median (the worst a flight's first frame), attitude within 78 | 1,295 samples, exact (slow or still ships), reports 89 ms old |
  | the host's of the guest's 5 craft | 393 samples, 31 units median, worst 318, attitude within 277 | none |

  The guest took the 5 craft ahead of it at the start and gave them back
  one by one as it turned away.  The whole battle sits within 10 km, most
  of it within 2.5 km: about 23 KB a second from the host when 4 craft are
  near the guest, about 50 KB a second when 24 are (sending every craft
  every frame).
* **Frame rates.**  Unchanged by the session: HF2W ran at 53 to 58 frames a
  second on both machines, 53 to 60 alone.  HF3M runs at about 33 alone at
  60,000 cycles (the game's own cost, measured with and without a session
  and with and without the added craft): it looked like a slowdown until
  the solo baseline was taken in the same mission.  The 32-bit hook table
  goes by an EIP's low word, so each place's word matches some ten other
  addresses: 2,600 to 3,600 such calls a second, nothing to slow anything.
* **Open.**  Lasers are each machine's own and damage is not shared (step
  6): a craft killed on one machine flies on, as a puppet, on the other.
  Mid and far rates were barely exercised (one battle within 10 km); a
  mission over a larger space is the test for them.
