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
3. **Next: notices through DOSBox**, drawn by the emulator over the picture,
   nothing patched into the game (the user, 2026-11-06): who is in the room
   and where each pilot is (registration, concourse, briefing, flying), and
   a call to action when the host leaves the bridge for a flight ("the host
   is flying: launch the same mission").  The first lines are in (the
   overlay's line); the places need the front end's scene number.
4. **The mission everybody flies**: the host's choice carried to the guests
   (the Combat Chamber's missions first: no story to walk), the guest's game
   put on the same mission, and a guest that launches another told so.
5. **Ownership by distance, and the variable rates** (section 5): the AI
   craft owned by the machine whose pilot is nearest within the lasers'
   reach, the host's otherwise; near craft every frame, far ones a few a
   frame (every 10 to 20 s each), all within the best-effort channel's
   16,000 bytes a message.
6. **Lasers and damage**: the shooter's machine decides its hits
   (`collide_laserhitcraft` there), the craft's owner applies them
   (`collide_damagecraft`); a puppet takes no damage of this machine's own
   (collisions included); deaths and spawns from the owner and the host.
7. **The controls the user asked for** (2026-11-06): the arrow keys steering
   as RetroVega's do, a pointer-steer mode like RetroVega's mouse beside the
   game's own drag-to-steer (the 90s way, for those who want it), and a
   selector between them; all of it in the emulator, fed as the mickeys the
   game reads.
8. **The page**: the room tag TF, `multiplayer: true`, a mission picker.

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
* **Measured** (two native games over TCP on one machine, `tfduo.sh` in the
  scratchpad: the same front-end script on both, the guest 2 s later; the
  guest at full throttle climbing and turning past the host with forty
  `!mickey=40:-12`; reports four times a second stamped with the wall
  clock, joined by `tfjoin.py`):

  | | samples | place, median / worst | attitude, worst |
  |---|---|---|---|
  | the host's copy of the guest | 70 | 7 / 51 units | 28 of 65,536 (0.16 degrees) |
  | the guest's copy of the host (still) | 70 | 0 / 0 | 0 |

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
  every pilot's craft: step 5.  Not yet tried over a lobby room.
