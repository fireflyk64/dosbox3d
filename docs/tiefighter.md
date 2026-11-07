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

### How the game computes, and what it shares with the Wing Commander hooks

* **The program.**  Z_TIE__.EXE is a 32-bit protected-mode program under
  DOS/4GW (Watcom C: arguments in eax, edx, ebx, ecx), one flat address
  space, paging off.  Wing Commander I and II are 16-bit real-mode programs
  with overlays, their data found by segment.
* **Its arithmetic is integer and fixed point throughout** (OpenTIE's
  math2.c: "Fixed-point arithmetic utilities"; no float or double in the
  flight's core, math2, trig2, dynamix, move, collide, paiman, transfm2;
  OpenTIE's few floats are its own port's renderer).  Positions are 32-bit
  integers, 40,960 to a km; angles 16-bit, 65,536 to a turn (heading in the
  x-y plane from +y towards +x, pitch from +z with 0x4000 level); direction
  vectors (the move vector, the forward/side/up axes) Q15 in 16 bits; speeds
  16-bit (a T/F's top 225, 3,600 in hyperspace); damage 16-bit (a bolt's
  collision radius); time in ticks, 236 to a second, a frame's share in
  `frameticks` (4 at 59 frames a second); trigonometry by tables
  (`trig2_ctop`, cartesian to polar); a 16-bit shift-register random
  (`math2_getrandom`, 0x2357 at start, the backdrop's seed the mission's).
  The same mission is exactly the same on two machines only with the same
  inputs at the same frame times, and the frame rate varies (each frame
  moves things by its `frameticks`): so the sync is by owners and reports
  (section 5), not a lockstep of frames.
* **What it shares with the Wing Commander layer**: the transports (TCP for
  the rigs, the lobby's rooms over WebRTC, the host relaying, `WCNET_LAG` and
  the rest of the simulated link), the protobuf envelope (wc.proto's
  NetworkMessage, with TIE Fighter's own TieHello, TieFrame, TieCraft,
  TieShot, TieHit, TiePresence and TieOwners), the per-instruction hook
  dispatcher (`wc_net_watch` by 16-bit CS:IP for Wing Commander,
  `wc_net_watch32` by the low word of a 32-bit EIP for TIE Fighter), the key
  script and screenshots of the rigs (with TIE Fighter's own items), the
  line the emulator draws over the picture (its notices), the keyboard and
  mouse filters (its flight controls plug into them), the logging, the
  browser page (rooms, the public lobby, chat, voice, controllers, the
  mission menu's generic hooks) and the release gate.
* **What is its own** (src/cpu/wcnet_tie*.cpp): the program found in memory
  after DOS4GW.EXE loads it (by its bytes, not its file); its address map
  (OpenTIE's, each checked in the program); its session (TieNet; the Wing
  Commander session, `g_session`, stays NULL); its sync (each craft flown by
  one owner, reported every frame near, every 10th within 10 km, in turn far
  off; puppets written before the move; far ones flown by each machine's own
  AI); the calls into the program (a trampoline of CPU state only, nothing
  written into its code); its flight controls (mickeys added at the mouse
  driver); the shuttle's cockpit made from the gunboat's files.  None of the
  Wing Commander session's machinery fits it: that session paces the game to
  a fixed frame rate and has the wingmen's games take the host's frames, in
  16-bit games of a few ships within a sphere; TIE Fighter has its own clock,
  a varying frame rate, 32-bit code and dozens of craft spread over tens of
  kilometres.

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
6. **Done, first version: lasers and damage** (section 7, step 6): the shooter's machine decides its hits
   (`collide_laserhitcraft` there), the craft's owner applies them
   (`collide_damagecraft`); a puppet takes no damage of this machine's own
   (collisions included); deaths and spawns from the owner and the host.
6a. **Done: seen from both cockpits, once the guns are in step** (the user,
   2026-11-06): a gunboat mission, the two players flying side by side, one
   a little ahead so that it is just on the other's screen; one fires
   lasers, the other ions.  Screenshots: the ions on the laser player's
   screen, and both players' fire on the ion player's.
6b. **Done: laser fire near and far** (section 7, step 6b; the user,
   2026-11-06: far fire "simulated by both clients but damage is entirely
   done by the host"; "have the AIs control them and let them diverge a bit,
   but smoothly snap to the right place over ten+ seconds between updates";
   near, "the owner of the laser should apply its damage... allows for
   precision aiming"): far craft flown by each machine's own AI, their shots
   for show, their places pulled to the owner's over the time between
   reports; a hit shown on the shooter's machine at once, applied by the
   target's owner for good, the damage in the reports.
7. **Done: the controls the user asked for** (2026-11-06, section 7, step 7): the arrow keys steering
   as RetroVega's do, a pointer-steer mode like RetroVega's mouse beside the
   game's own drag-to-steer (the 90s way, for those who want it), and a
   selector between them; all of it in the emulator, fed as the mickeys the
   game reads.
7a. **Done: all four selectable from the page's menu** (the user, 2026-11-06:
   "Make sure the joystick, keyboard mouse and alternative mouse (point to
   aim like a joystick; middle doesn't move just like wc1 and retrovega
   mouse) also are selectable from web menu"): the joystick (a gamepad as
   the stick), the keyboard (the arrows), the game's own mouse (drag to
   steer), and the alternative mouse (point to aim: the middle is still,
   the turn grows with the pointer's distance from it, as in WC1 and
   RetroVega).
8. **Done: the page**: the room tag TF, `multiplayer: true`, a mission picker
   (the room's menu: every Combat Chamber mission and every battle's,
   `web/gamefiles.js`'s `TF_CAMPAIGN`); its help says what flying together is
   (brought up to date in step 15).
9. **Done: missions flown alone, flown by two** (the user, 2026-11-06; section 7, step 9).  In 37 of
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
   | missile boat | missile boat + a standard Imperial shuttle (the user, later: "otherwise the gunboat could do some ion cannon shenanigans and the designers probably didn't think of that") | B10M5, B10M6, B11M1..B11M7, B12M6, B13M1, B13M4, HM1W, HM2W |
   | T/B | T/B + T/F: one flies the torpedo work, the other point defense | B3M1, HB1W |

   How: a flight group has one craft type (+0x32), so a mixed pair is a
   second flight group for the second pilot, a copy of the player's record
   with the other type and a count of 1 (the mission's 48 at most), the
   goals left on the first; T/I to two T/F changes the group's type and
   count.  The cockpit follows the craft the pilot flies; the shuttle has
   none of its own.
   **Done: the shuttle's cockpit** (section 7, step 9), made by the
   emulator from the player's own gunboat cockpit: an A of struts instead
   of the gunboat's V, every instrument working; **and the pairings**: the
   guests' own flight group in the pairing's craft.

10. **Done: three pilots** (section 7, step 10; the user, 2026-11-06: "add another item for three
   player like in the other game. The third should be same as the guest in
   all cases. I guess more players also makes sense but we will probably cap
   it at 3 for now").  The page's Players box already stops at 3 (beta) for
   every game, and the session already gives the player's flight group a
   craft a seat.  To do: in a pairing (step 9) the third pilot flies the same
   craft as the second, in the second's flight group; a three-machine rig and
   its measurement.

11. **Done: the page for TIE Fighter** (the user, 2026-11-06: "for tie
   fighter we shouldn't have the asteroids section. that may let
   fullscreen be on the same line... tie fighter is 640x480 so when not full
   screen I saw some horizontal rows missing... can we change canvas size
   when TF is selected or joined. It would also be nice to have a cinema
   mode"; section 7, step 11): no Conn or Collide for TIE Fighter, the
   picture 640 x 480, a Cinema checkbox.

12. **Done: a room's mission ending** (the user, 2026-11-06: "test what
   happens with campaign missions... we don't want capture or recover
   scenarios... it should go back to the mission as if you failed it if
   either of you dies or if you fail it"; "it just delivers you to the
   debrief room with mission failed and you can quickly try again with your
   friends"; section 7, step 12): a pilot down or the host's flight over
   ends everybody's flight, mission failed, Fly Mission Again at the
   debriefing.

13. **Done: the host decides, the guest waits** (the user, 2026-11-07:
   "It's also sort of strange that the guest can enter the mission first
   without knowing what mission the host will choose"; "it might be simpler
   if the host did it and guest had to wait or didn't have the option like
   retro vega until the host is in"; "send a message to the other player (or
   also play a loudspeaker sound we can copy from retrovega in dosbox
   itself) every 20 seconds to guide them to the briefing room"; section 7,
   step 13): a guest's launch waits until the host flies; the ship's PA
   calls a guest to the briefing every 20 s.

14. **First version done: the Z-95 challenge** (the user, 2026-11-07; section
   7, step 14): in a room the Combat Chamber's T/F row flies the gunboat's
   missions that need no ion cannon and the T/I's where they would, in two
   Z-95 Headhunters against the Empire's own craft (every craft mirrored),
   the cockpit kitbashed from the gunboat's by code; the missions' lines that
   speak for the Empire's side swapped for the game's own lines that fit the
   mirrored sides, each line in its own voice (the bank made from the
   CD's).  To do: the briefings' words.
15. **Under way: long mission tests with an ally** (the user, 2026-11-07: "Do some comprehensive mission
   tests with the ally and make sure the networking works, especially the
   guest handoffs...and that well aimed shots at tie fighters work. The
   challenge missions are a good place to start as well as some of the other
   missions"): two machines through whole missions (the four challenges
   first, then others of the chamber's rows and a battle), the guest's
   handoffs of near craft (owner changes as craft come and go within its
   5 km), aimed shots at T/Fs from both machines landing (the shooter's hits,
   the damage in the reports, the kill on both), the room's ending each way.

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
* **Open then, closed since.**  A puppet still collided and took damage on
  the machine that only shows it (both pilots flew into the container ring
  once and each died on both machines): closed in step 6 (a puppet takes no
  damage of this machine's own, collisions included).  The rates were one
  report a frame for every pilot's craft: step 5's.

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
* **Open then, closed since.**  Lasers were each machine's own and damage
  not shared: closed in step 6.  Mid and far rates were barely exercised (one
  battle within 10 km); step 15's long tests measure them.

### Step 6: the guns (research, 2026-11-06)

* **The calls.**  A shot is `laser_createprojectile(eax shooter object,
  edx hardpoint, ebx projectile type, ecx)` (0x2E0E4; type 0x8C a T/F's
  laser); the bolt is an object from slot 32 with its shooter in `self_idx`
  (+0x30).  A bolt meeting a craft is `collide_laserhitcraft(eax bolt, edx
  target, ebx hit offset)` (0x1433C), which goes on to
  `collide_damagecraft(eax target, edx component, ebx attacker, ecx shield
  side)` (0x148F0).  The damage comes from the attacker object itself: its
  ship type (a laser's, an ion's, a warhead's) and its collision radius
  (OpenTIE `collide.c`: `damage = collision_radius`, a quarter against a
  freighter, a sixteenth against a starship or platform); -1 is a collision
  (0x20000), 0x3800 and up a static object.
* **The design** (section 5): the shooter's machine decides its bolts' hits
  and the target's owner applies them; far shots are each machine's own.
  1. A player's shot is sent (shooter, hardpoint, type) and made on the
     other machines from that player's craft there, by calling
     `laser_createprojectile`: the bolt is seen, and flies from where the
     craft is.  An AI craft's shots likewise from its owner, within the
     lasers' reach of another pilot.
  2. On the shooter's machine a hit by its own bolt (its player's, or an AI
     craft's it flies) on a craft another machine owns is sent to that
     owner (target, component, the bolt's type and radius, the shooter) and
     not applied there; the owner applies it by calling
     `collide_laserhitcraft` with a stand-in bolt (a free projectile slot
     given the bolt's type, radius and shooter), so the game's own damage,
     shields, components, kill credit and messages follow.
  3. A bolt made from another machine's report hits nothing here (its
     shooter's machine decides), and a craft this machine does not own
     takes no damage from this machine's collisions.
* **What the emulator needs.**  A call into the 32-bit program from a
  hook: at a hook's place (a function's entry the game reaches every frame,
  `move_moveobjects`), the registers saved, the place itself pushed as the
  return address, the arguments in `eax`, `edx`, `ebx`, `ecx` (Watcom's
  register convention), EIP set to the function; when it returns to the
  place the hook sees itself come back, restores the registers and lets the
  game go on (or makes the next call).  And an early return: at a
  function's entry EIP set to the return address on the stack and ESP past
  it (these functions take their arguments in registers), so a hit the
  game was about to apply is not.  Both are CPU state at a hook, no code
  changed (the Wing Commander layer's trampoline does the same in 16 bits).

### Step 6: the guns, built and measured (2026-11-06)

* **The trampoline** (`wcnet_tie.cpp`, `tie_queue_call`): at
  `move_moveobjects`'s entry the registers are kept, the place is pushed as
  the return address, the arguments go in `eax`, `edx`, `ebx`, `ecx`, and
  EIP is set to the function; its `ret` comes back to the place, whose hook
  gives the state back (the lazy flags too), tells the call's `done`, and
  makes the next call, the frame going on after the last.  A `before` step
  runs with the arguments in place (it writes what the call needs at that
  moment, may change an argument, may call it off).  `tie_skip_call`
  returns from a function at its entry.  No byte of the game is written:
  the core loads its fetch address after the hook.
* **Shots.**  `laser_createprojectile`'s entry: a shot of a craft this
  machine flies, within 5 km of another pilot, goes with the next frame;
  the others make it from that craft with the same call (hardpoint, type).
  A craft another machine flies does not shoot here (0xFFFF, "no slot").
* **Hits.**  `collide_laserhitcraft`'s entry: a hit by a bolt whose shooter
  (+0x30) this machine flies, on a craft another flies, goes to that machine
  reliably (the bolt's type, genus, radius, shooter, segment); the flash
  still shows here.  The owner applies it with the game's own call and a
  stand-in bolt in a free projectile slot (the segment in `laserx..` and
  `laserxold..`, 0xEAB90 to 0xEABD8).  `collide_damagecraft`'s entry: this
  game damages only the craft it flies, and from a bolt only one whose
  shooter it flies (collisions included); its own calls go through.
* **Deaths.**  A craft whose death timer runs (+0x2C) is reported dying;
  the others kill their copy with the game's own damage (a collision's,
  called until its death is under way, at most 12 times), so its explosion
  and the mission's count of it are theirs too.  A craft whose owner is
  out of the flight is this machine's.
* **Measured** (HF1W, the containers ahead): the host's eight laser hits on
  a container the guest flew applied on the guest (its hull 100 to 800 of
  750), the container's death followed on the host; each side's shots made
  on the other.
* **Step 6a, the user's picture** (HG2W, the Combat Chamber's gunboat
  mission 2: the player's group four gunboats; HG1W, a training mission,
  holds the gunboat still).  The guest holds W 0.4 s (a 150 ms tap during
  the hyperspace arrival did nothing): its shots become type 142 from
  hardpoints 2 and 3, the ion cannons, made on the host as 142.  The host
  fires lasers (140), made on the guest as 140.  The guest flies ahead along
  +x, some 8,650 units to the side; from the host's 80 s it is within 30
  degrees of the host's nose.  The laser player's screen shows the guest's
  cyan ion bolts ahead and to the right; the ion player's shows the host's
  laser streak coming up from the left and its own ions going away.
* **Done in step 15.**  Kill credit: a kill the shooter's machine did not
  apply was not scored there; now the owner's dying reports name the shooter
  whose hit began the death and that machine calls the game's own
  `collide_updatekills` (step 15).  The impact flash of an applied hit sat
  where the bolt's segment ends; now at the shooter's impact (OpenTIE has the
  collision test's offset now: `collidexoff` 0xEAB7C, step 15).  By design: a
  shot of a craft far from every other pilot is not sent (the brief: far
  shots are each game's own).

### Step 7: the flight controls (2026-11-06)

* **How TIE steers** (OpenTIE `feinput.c`): a frame's mouse motion is the
  stick, 128 to a mickey across and 64 up and down, with nothing below
  2,048 (17 mickeys a frame across and 33 up and down are the least that
  turn; 191 and 127 the most); a joystick is 120 and 50 to a count.  So the
  emulator steers by adding mickeys once a frame at its top
  (`Mouse_AddMickeys`, no sensitivity, the pointer left where it is), and
  by taking things from the game: keys at the keyboard
  (`wc_net_key_filter`), the host mouse's motion at the mouse driver
  (`Mouse_CursorMoved`).  Every piece answers only with TIE Fighter found.
* **The arrows** steer by RetroVega's ramp (WC1's): a step every other
  frame, 1 to 9 then 8 and 9, Shift 9 at once, the opposite arrow or
  letting go 0; Up dives as a stick pushed forward.  TIE's own Left and
  Right roll in flight: taken from it; the keypad's Ins and Del roll
  instead, passed to the game as its own arrows (TIE's keyboard roll, a
  step a press and a repeat).  Q, E, comma and period are TIE's own keys.
* **The mouse**, `WCNET_TIE_MOUSE`: `pointer` (the default) moves a pointer
  the emulator draws (an amber ring over the gunsight) and its distance from
  the middle is the stick, held until it moves (above the middle climbs);
  `drag` is the game's own; `keyboard` and `joystick` let it steer nothing.
  F12 in flight cycles pointer, drag, keyboard.  A joystick's stick comes
  from the page (`wc_web_stick`, -1..1, right and climbing positive; one that
  stops saying for half a second is the middle).
* **The page**: a "Flight" choice beside Controller with TIE Fighter loaded
  (point to aim, drag to steer, keyboard alone, joystick / gamepad),
  remembered in the browser, given at the launch and while flying
  (`wc_web_tie_mouse`).  A controller steers TIE through the emulator's
  stick whatever the mouse's mode, rolls on Ins and Del and fires with the
  mouse's button, TIE's trigger.  The flight's keys are under the game.
* **Measured** (the Combat Chamber's first mission, from rest, the nose's
  direction a second before and after): the Right arrow turned the nose
  right as the game's own mouse motion to the right did (+0.38 against
  +0.22 across), Up dived as its motion forward did (-0.23 against -0.11),
  Down climbed (+0.28), Left turned left (-0.36); the pointer left right of
  the middle held a turn of 50 degrees a second, stopped with the pointer
  back; in the keyboard's mode eight bursts of mouse motion did nothing.  In
  the browser the choice showed with the game loaded, defaulted to the
  pointer, and the page's guest held its turn with the pointer pushed aside.
* **Done in step 15.**  The joystick's buttons beyond fire, roll and the
  keys were WC's mapping (target T, weapon W, speed = and -: TIE's own keys
  for those), the afterburner and missile buttons meaning nothing in TIE.
  Now a game may give an action its own keys and name (`host.actionKeys`,
  `host.actionLabel` in `web/gamepad.js`), TIE's from its flight keys
  (OpenTIE's `user.c`): the afterburner Backspace (full throttle), Fire
  missile Enter named for what it is there (match the target's speed; W picks
  the warheads), Autopilot E (your closest attacker), Change guns X (the
  lasers linked or not), Lock target R (the nearest enemy); Communications
  and the navigation map nothing (C is the flight recorder, the map an info
  room not known to keep a shared flight going).

### Step 9: the Imperial shuttle's cockpit (2026-11-06)

The user: "modify the gunboat cockpit to work for the imperial shuttle... widen
the viewscreen a bit so it looks more like a shuttle. Make sure instruments are
active too"; "Gunboat cockpit should remain same. Shuttle cockpit should
originate from gunboat. Make sure that the edits are done with instructions and
code so we don't include the gunboat cockpit art in the distribution"; "the two
strut bars should curve in rather than out making the edge of an A character
rather than the V that we see", with the stolen shuttle's cockpit in Return of
the Jedi (seen from outside) as the reference.

* **Why there is none.**  A craft's cockpit is `<cockpit dir><its spec's
  internal name>.INT` (`panel_loadpaneldata`); the shuttle's spec (16, species
  17, the mission files' craft type 17) has an empty name, so the game would ask
  for `CP640\.INT`.  The tables (image addresses): `spec_data` 0xC7AB4, 0xEC a
  record, +0x2C the name; `species_table` 0xCBBC4, 0x16 a record, +1 the load
  flags, +0x13 the spec; `speciesconvert` 0xC1834; the player state's +0x0A the
  player's flight group, +0x0E its spec.  The data object is 0x28800 below its
  file offset in Z_TIE__.EXE.
* **When.**  `fediskio_loadspecies` (0x218D8, watched now) runs after
  `create_loadmission` has read the mission and marked its craft types (load
  flags 0x10) and set the player's spec, before the craft and the cockpit load.
  At its entry, when the player's spec is the shuttle's, the emulator names it
  SHUTTLE and makes the files.  `WCNET_TIE_FLY_AS=<craft type>` changes the
  player's flight group's type there first (+0x32), marks the species and sets
  the player's spec as the loader would: how the rigs fly a shuttle (and the
  first half of the pairings).
* **The files** (`src/cpu/wcnet_tie_cockpit.cpp`), written beside the game's on
  its drive (the local drive's host path; the drive's cache emptied after):
  `CP640\SHUTTLE.INT` and `CP320\SHUTTLE.INT`, the gunboat's description with
  one change, its forward view `SHUTFWD` instead of `GUNBT12`.  Every other view,
  the 95 instruments' places and the parts (`GUNBOATP.PNL`) are the gunboat's own
  files, read where they are: the gauges, scopes and displays work as in the
  gunboat.  The game's files are not touched.
* **The view's formats.**  An INT: 28 views of 36 bytes (flags, a 9-byte name,
  x, y, width, depth, y offset, a title), 95 instruments of 6 bytes from 0x3F0,
  the parts' name.  A view's LFD: PANL, MASK, PLTT, read in that order.  PANL is
  the game's sprite (0..250 a run of 1 to 4 of the base color plus op>>2, 251 a
  new base, 252 a run of a color and the next in turn, 253 a run of 1 to 256 of
  one color, 254 the row's end, 255 the end; color 253 is left out), drawn once
  when the view comes up.  MASK: for each row of the 3-D view's depth (325 of
  480 forward), a signed state (not negative: space shows first) and the runs,
  the state turning at each (past 255: 0, then the rest less 256; past 511: 0,
  0, the rest less 512; a run of exactly 256 has no code, so one pixel moves).
  The 3-D view is copied in only where the mask says space.
* **The change, all code** (the gunboat's art never in our files): the struts
  are the longest stretch of rows, each side, where a hidden run with space on
  both sides moves smoothly (rows 117..243 and 120..268 at 640x480); a line is
  fitted to their middles and widths.  Out: their bands (9 pixels more each
  side), from the top down, the scopes' disks kept (instruments 0 and 1), below
  the stretch the strut's own run while it is a strut's width; small hidden
  pieces left near the bands cleared.  In: two legs of an A, mirrored, leaning
  in 122 pixels over 282 rows at 640x480 (the user's first look), pushed out
  until each one's outer edge is just past its side of the screen on the last
  row where space shows there (row 313: the user, "push the struts out so the
  bottom right of the strut is just beyond the right of the screen"; "symmetric
  both at the edge of the screen mirrored"), each row a row of the strut that
  leans the same way, the strut's length
  stretched over the leg's and its runs narrowed by the slants, drawn only where
  space shows (so behind the dashboard and the scopes); a frame bar 24 rows deep
  along the top, each column a strut's row on its side, the scopes hanging from
  it.  The same code makes the 320x200 view from the gunboat's own (its
  figures scaled).
* **The targeting display**'s mask is chosen by the spec's number in the
  program (the gunboat's 15 has its own; the shuttle would get the T/F's), so
  while a shuttle flies the emulator writes the gunboat's 480 bytes (0xC6C63)
  over the default's (0xC66C3), and its 200 (0xC628B over 0xC61C3) at 320x200,
  and puts them back when another craft flies.
* **Measured** (the gunboat's mission 2, HG2W, flown as a shuttle,
  `WCNET_TIE_FLY_AS=17`): the cockpit as made, the scopes' contacts, the
  throttle, speed, clock and the laser, shield and engine bars changing, the
  warhead counters, the targeting display's picture of the target inside the
  gunboat's frame.  The side views are the gunboat's (its V).
* **Open.**  The side views' struts.  (The second pilot's own flight group in
  the pairing's craft: done, step 9's pairings below.)

### Step 6b: laser fire near and far (2026-11-06)

* **What it was** (the user asked to verify it): every craft another machine
  flies had its AI held here at any distance, so it neither planned nor
  fired, and its own shots were cancelled here; a craft's shots were made on
  the others only when within 5 km of another pilot.  A far battle on the
  host was blank on a guest: in battle 1 mission 2, a guest 7.6 km from the
  platform's fight and facing it saw no shot at all (0 made in the 27 s it
  watched).  Near, the shooter's machine decided its hits and the target's
  owner applied them; the shooter's copy took no damage and nothing carried
  shields or hull, so a guest's shots at the host's craft left its shields
  at 100% on the guest until it died.
* **Far now** (the user's design): another machine's craft beyond 5.5 km of
  our pilot is flown by this machine's AI (released at once: CraftData +0x2A
  0), held again within 5 km (there its owner's shots come over).  Its shots
  are made here for show (laser_createprojectile not cancelled), their hits
  flash but damage nothing here (collide_damagecraft is skipped for a target
  not ours and for a bolt whose shooter is not ours).  Its owner's reports
  pull its place in, the error spread over the time to its next report as the
  last one came (1 to 15 s: some 15 s for the far ones reported in turn, a
  second within 10 km); its attitude and speed stay our AI's.  A pilot's own
  craft is never flown by AI, however far.  `WCNET_TIE_FAR_AI=0` holds them as
  before.
* **Near now**: a report within 10 m of where the copy is is written as it is
  (a near craft, reported every frame); a larger error is blended in at 300
  m/s at most.  The reports carry each craft's damage (CraftData +0xA2 the
  hull's, +0xCA and +0xCC the shields, +0xAE the systems), written into the
  copies.  A hit of a bolt of ours on another machine's craft is shown here at
  once, by the game's own sums (collide_damagecraft: the bolt's radius as the
  damage, a quarter on a freighter, a sixteenth on a starship or platform; the
  struck side's shield first, then the hull), but never a death, a system or
  an ion bolt's overflow (the owner's), nor a starship's part; the owner's
  figures wait 600 ms after it, and the owner applies the hit for good as
  before.  A craft near a guest's crosshairs is the guest's (step 5): its
  place is never written from the network there, so aiming at it is the
  game's own motion.
* **Measured.**  Far (battle 1 mission 2, `!tieface=9` facing the platform
  from 7.6 km, throttle cut): 305 far shots made on the guest while it
  watched (the picture: green bolts by the platform); up to 14 far craft
  flown by its AI; their places 90 m off their owner's reports on average,
  the worst 2.9 km at a hyperspace arrival.  The first version blended every
  far report over 10 s: with reports six times a second within 10 km the
  error settled near 760 m; blending over the time between reports brought it
  to 90 m.  Near (the Combat Chamber's first mission, the host's target a
  container, `!tietarget=5`, the guest firing at it): 9 hits sent, 9 shown on
  the guest, 8 applied by the host (the last came after its death); with 150
  ms each way (`WCNET_LAG=150`) the guest showed each hit 170 to 190 ms
  before the host applied it, the two copies' hull the same after
  (100..700 of 750).
* **A bug found on the way**: a craft whose AI was held through its
  hyperspace arrival never died on the other machines.  The game lets a craft
  die only with its flight flag (CraftData +0x60) clear; an arrival sets it to
  6 and the craft's own AI clears it at the arrival's end, which a held copy's
  never did, so the kill's damage calls did nothing and began again with each
  dying report (359 on a guest in one run).  The flag (not a death's, 3 or 4)
  is now cleared before each kill call: the four Y-wings the host saw die died
  once each on the guest.
* **Aids**: `!tieface=<obj>` turns our craft to face an object;
  `!tietarget=<obj>` makes it our target.

### Step 9: the pairings (2026-11-06)

* **Where.**  At `fediskio_loadspecies`'s entry (the mission read, its craft
  not yet loaded), on every machine alike, before the cockpit's step.  A
  mission whose player flight group is one craft (its +0x33) gets the user's
  pairing by its craft type (+0x32): T/I (6): the group becomes T/Fs (5), and
  the seats give each pilot one, as for a group of the same craft; T/B (7) +
  T/F, T/D (9) + T/A (8), gunboat (16) + shuttle (17), missile boat (12) +
  shuttle: the player's group copied as a second one at the end of the table
  (the header's count +1) in the guests' craft, one craft for each seat after
  the host's (a third pilot flies the same as the second: "The third should be
  same as the guest in all cases"), no waves, its start points and route (the
  first 12 waypoints in use) 0.4 km to the side (64; 160 waypoint units a km),
  its place reference as create_loadmission sets it (FGStatus 0xDEDCC, 0x30 a
  group, +6 = 0x8000), its craft marked to be loaded.  A guest flies its own
  craft of it: the original group's player flag cleared, the copy's set to the
  guest's seat, the player state's group and spec moved there (a shuttle so
  gets its cockpit).  T/F and T/A pair with themselves: the seats as before.
  The goals, orders and arrival are the player's group's.
* **A mission that begins with a hyperspace jump** (the Combat Chamber's
  gunboat missions) is read from its file a second time after the jump, its
  craft not loaded again: the copy vanished and the guest flew the host's
  gunboat.  The pairing chosen at the loading is made again at
  `create_createmission` when it is gone.
* **Measured** (two machines, the room's mission picked): the gunboat's
  mission 3 (`hg3m`): the host a gunboat (obj 0), the guest a shuttle (obj 2,
  flight group 28, seen as a shuttle on the host) in the cockpit made from the
  gunboat's, through the jump and after; the T/B's mission 1 (`hb1w`): the
  guest a T/F (flight group 19); battle 5 mission 1 (`b5m1iw`, the minefield):
  both T/Fs side by side, both killed by the mines after a minute and each
  death seen once on the other machine.
* **Open.**  The copy's goals are the player's group's (usually none); the
  T/I's timing judgement (the user: "unless there's a timing aspect where T/F
  is too slow").  (Three machines: measured, step 10.)

### Step 11: the page's picture and options for TIE Fighter (2026-11-06)

* The registry's `screen`: a game's picture shape on the page (TIE Fighter
  640 x 480; the others 640 x 400, Wing Commander's 320 x 200 doubled), set as
  CSS variables for the game loaded or the room's.  A 640 x 400 canvas had
  dropped 80 of the flight's 480 rows.
* `noRoomOptions`: no Conn (the frame exchange's mode) or Collide (asteroids,
  wing bumps) for TIE Fighter, whose hooks read neither; Full screen then
  sits on the mission's line.
* Cinema, a checkbox beside Full screen, kept in the browser: the picture as
  large as the window allows without full screen (the window's width, or its
  height when the window is too short, so the whole picture shows), centred,
  the log below.
* Measured in Chrome at 1280 x 720 (scripts/web-tf-guest.mjs): the canvas 640
  x 480 on the page, Conn and Collide hidden, Full screen on the mission's
  line; cinema 885 x 664, centred.

### Step 12: a room's mission ending (2026-11-06)

* **What the game does.**  A room's missions are all launched from the
  Combat Chamber (step 8 swaps a battle's file in there), mission mode 1: the
  pilot record keeps only the chamber's best score for the craft and mission
  picked (`fediskio_updatepilotrecord`); killed, captured and rescued (the
  player's status, RUNTIME_MissionState 0xEB29C +0x248: 0, 1, 2) are recorded
  only for the tour's battles (mode 4).  Within a flight a death sets status
  0, an ejection 1 or 2 by whether friends are near (`user_isrescued`), and
  each machine's game went on alone: the other pilots flew on.
* **The room's rule now.**  A pilot's own craft dying (killed or ejected) or
  the host's flight over ends every pilot's flight 3 s later the way the
  game's own quit does: the end flag (+0x249) the flight's loop runs until,
  the status 3 (the mission ended).  Our own death's status becomes 3 at
  once, so nobody is killed, captured or rescued in a room's flight.  The
  debriefing then says the primary mission was a failure (unless its goals
  were met: the host's successful exit ends the guests' flights the same way
  and they are debriefed on the goals) and offers Fly Mission Again; leaving
  goes back to the Combat Chamber.  Only with another pilot flying.
* **Measured** (the Combat Chamber's first mission, the guest at full
  throttle into the container ring): the guest's status 2 (rescued) made 3,
  the host's flight ended 3 s later with a notice; both at the debriefing
  officer, "The primary mission was a failure", Fly Mission Again (the
  guest's Fly Mission Again put it back in the mission at once).  The guest's
  first answer had a third page, not read.

### Step 10: three pilots (2026-11-06)

* Nothing new was needed in the session: the host's hello gives the room's
  size, the seats give the player's flight group a craft for each pilot, the
  pairing gives the guests' group a craft for each seat after the host's
  (the third the same as the second), and the host relays each guest's
  frames to the other.  The page's Players box stops at 3 (beta) for every
  game.  `scripts/tfnet-duo.sh` takes `PLAYERS=3` (a second guest, GUEST2).
* **Measured.**  The Combat Chamber's first mission: the host, the guest and
  the third pilot fly craft 1, 2 and 3 of the player's group (objects 0, 1,
  2), each seen as a T/F on the other two machines, a guest's craft on the
  other guest reported 14 to 28 ms before (through the host).  The gunboat's
  mission 3: the host a gunboat (object 0), the two guests shuttles 1 and 2 of
  the guests' group (objects 2 and 3), each seen in its craft on the others,
  reports 16 to 20 ms old.

### The music in the browser (2026-11-07)

The user: "I don't get any music for some reason".  GOG's IMUSE.INI names the
music driver in its first 14 bytes, GMIDI.MDR (General MIDI to an MPU-401;
GOG's own DOSBox sends it to Windows' synthesizer), the digital driver in the
next 14 (SB16.WDR); the browser build has no MIDI synthesizer.  The Sound
Blaster 16 it emulates has an OPL3, and iMUSE's FM4.MDR (on the game's disc)
plays the music there: the page writes FM4.MDR into its installed copy (the
registry's `prepare`, web/gamefiles.js `tfMusicOnFm`).  Measured natively
(`WCNET_AUDIOLOG=1`): GMIDI.MDR no OPL write in a minute of the opening;
FM4.MDR 250 to 1,500 OPL writes every 6 s and the FM channel sounding.

### Step 13: the host decides, the guest waits (2026-11-07)

* **Where.**  The Combat Chamber asks for the mission's file (at
  `shipext_Get_Mission_Path`) as its screen opens, the briefing as it loads,
  the flight as it starts.  Holding a guest at the first froze the chamber's
  screen black (the front end's palette is black while it loads, so the
  emulator's line could not be seen either), so a guest may look at the
  chamber and its briefing freely, and what waits is the launch:
* **A guest's click on a launch** while the host is not flying is not given to
  the game (the mouse driver's press and its release, `wc_net_mouse_button`):
  the briefing's and the debriefing officer's bar, its launch button (map.c's
  `map_rect`, 225..257 x 172..194: Enter Mission, Fly Mission Again), the
  briefing room's door to the mission (brief.c, 190..248 x 118..138), the
  debriefing room's Fly Again (debrief.c, the room's left 70 columns).  A
  notice says why: "HOST has not launched yet: you take off together once HOST
  is flying".
* **Any other way into a flight** (a key, say) waits where the program asks for
  the file as the flight starts: the call is made again once a time slice
  (`tie_repeat_call`: back to the caller's `E8`, the slice given up as a HLT
  does; made again on every instruction it had the emulator fall behind real
  time) until the host flies.  Esc flies alone.  A tour battle the guest
  launches is its own: never held.
* **The ship's PA**: while the host is briefed or flying and a guest is at
  neither, the emulator plays RetroVega's `pa_report` (a two-note chime and
  "All pilots, report to the briefing room." in our own synthesized voice:
  retrovega/tools/sfx.py, made into src/cpu/wcnet_tie_pa.h at 11,025 Hz by
  scripts/tf-pa-sound.py) on a mixer channel of its own, every 20 s, with the
  line "PA: All pilots, report to the briefing room." and where the host is.
* **Measured** (two machines, the host briefed at 75 s and flying at 88 s):
  the guest's Enter Mission at 62 s held, the notice over its briefing map;
  its click at 95 s took off as the host group's second craft.  A guest left on
  the concourse heard the PA (its channel 4.5 s) at the host's briefing and
  every 20 s while the host flew.

### Step 14: the Z-95 challenge (2026-11-07)

The user: "replace all tie fighter missions in the combat trainer with the
gunboat missions that don't need ion cannons or the tie interceptor missions
if they do, and the ship you fly is the z95 headhunter but do a substitution of
imperial equivalents of each rebel craft etc, which has a cockpit that's a bit
like a gunboat but it has a curvey line and the long nose you can see. We'll
have to kitbash some of the cockpits to achieve that. ideally we use the line
'They're flying z-95 headhunters they're brave' or something from one of the
other missions. These are meant to be challenge missions since the z-95 is
hopelessly underpowered but there will be two of them", with a picture of an
X-wing-like cockpit (an arched canopy, a long nose).

* **The missions.**  The goals read from the files (a flight group's goal
  conditions at +0x9E, +0xA0, +0xA4: 4 capture, 6 board and 8 disable need ion
  cannons): HG1W's primary goals destroy mines (a capture only secondary),
  HG2W's disable two craft, HG3M's are dogfights, HG4M an escort.  So the T/F
  row's four are HG1W, HI2W (the T/I's mission 2, for HG2W), HG3M and HG4M.  A
  mission file begins with a version word (-1), then the 456-byte header (the
  flight groups', messages' and goals' counts), the 292-byte flight groups,
  the 90-byte radio messages.
* **When the challenge is flown.**  In a room (a session), at
  `shipext_Get_Mission_Path`: a T/F mission's file (a room's pick, or the
  chamber's own) maps to its source at any of a launch's calls; with none
  picked, in the chamber, the pilot record in memory (0xFABC8) says the row
  (+0x66, 0 the T/F's) and its mission (+0x67): the chamber asks for the file
  as its screen opens and again at Enter Briefing, by when the name is already
  the source's.  A guest takes the host's word (TiePresence.challenge).
* **The craft.**  Before the craft load (`fediskio_loadspecies`), on every
  machine: each flight group's craft mirrored, the Rebels' by the Empire's and
  the Empire's by the Rebels' (X-wing T/I, Y-wing T/B, A-wing T/I, B-wing
  gunboat, T-wing T/I, Z-95 and R-41 T/F; T/F Z-95, T/I A-wing, T/B Y-wing, T/A
  X-wing, T/D, missile boat and gunboat B-wing; the corvettes the Carrack, the
  Nebulon-Bs the strike cruiser, the Calamari cruiser the Star Destroyer, the
  light one the Victory, and back; pirates', containers, transports, platforms
  and mines as they are), the IFF of each group kept, the player's group
  Z-95s (the second pilot's too: the seats), each new craft marked to be
  loaded; made again at `create_createmission` after the opening jump.
* **The line.**  The user remembered "They're flying Z-95 Headhunters!
  They're brave..."; no line of the game's says so (the closest: "Tau 2 to
  Tau 1, there are Z-95 Headhunters right behind us!" in B13M3, "Outstanding,
  Mu 1! That was some pretty brave flying!" in B11M3), and the line it was is
  HA2W's "How pathetic! They must really wish to die flying those Z-95s!"
  (the user, 2026-11-07: "for a challenge mission it's great").  So the
  challenge's Imperials say that one, its words read from HA2W and its voice
  from HA2W's bank (below), as a radio message of the mission's own (radiomsg
  0xE2F24, 90 bytes: the text, two triggers "always", the delay five polls of
  1,180 ticks: 25 s in; at 10 s HG3M's fell under a new craft alert and was
  never seen: a message fired with others shows only if nothing replaces it)
  where the mission has one of its 16 free and does not say it already
  (HG4M; HG3M's first line is it); again when the mission is read again after
  its opening jump (HG4M's: the line was lost there before).  With none free
  (HG1W, HI2W) the emulator's notice says "They're flying Z-95 Headhunters!
  They're brave...", unvoiced.  In the enemy lines' color (below).
* **The missions' lines** (the user: "just replace them with those other side
  lines.... where it talks about the weak z-95 or others where they talk about
  rebel scum. lots of lines can sub in... find extant lines").  Each radio
  message that speaks for the Empire's side gets another mission's line,
  read from the player's own mission file at the launch (the program holds
  only which mission and message, `kChallengeLines`): the player's side
  speaks of Imperial craft as the enemy (lines of the battles against Zaarin's
  and Harkov's traitors, the Rebels' own in B10M1 and B12M6), the enemy of the
  Z-95s (HA2W's "How pathetic! They must really wish to die flying those
  Z-95s!", B2M4's sparing its warheads, HM1W's Z-95s launched, HM4M's "you and
  your wingman are toast").  HG1W 2 (13, 14), HI2W 7 (0..5, 15), HG3M 2 (0, 1),
  HG4M 2 (0, 2); the neutral ones stay (training, shields, the duel's taunts).
  A message's first character is its color: none the command's red, '1' green
  (the missions' own enemy speakers: HG3M's challenger, B12M6's Rebels), '2'
  blue, '3' yellow; a foe's line takes '1', a friend's none, as each mission's
  own lines do.  Applied again when a jump reads the mission again.  Two
  lines fired in one poll (HG3M's 0 and 1) show one: the game's own timing.
* **The voices.**  The game's voice bank is `VOICE\<letter>M<n>`, the letter
  the Combat Chamber's craft row (`FIBAGDM`) and n its mission, from the pilot
  record at each launch (voice_id_a 0xD4190, voice_id_b 0xD418E), its clips
  going to the mission's messages in order.  In a room, at the craft load
  (before `create_createmission` loads the bank): a chamber mission's file
  `h<letter><n>` sets them (a room's pick played the chamber row's voices over
  its lines before).
* **The challenge's voices** (the user, 2026-11-07: "it would be nice to have
  the audio to match the text in the challenge. Can we make a script to
  duplicate them in? Especially the z95 line"; "ideally all radio";
  `src/cpu/wcnet_tie_voice.cpp`).  A bank (`VOICE\<name>\<name>.LFD`, on the
  CD only) is an RMAP resource: a directory of VOIC entries (a name, a size),
  then each clip, its entry again and a Creative Voice File.  The game gives
  the clips to the slots that have text in order: radio messages 0..15, then
  the two win messages and the loss message (OpenTIE's fsfx_loadvoicelfd).
  The developers named each clip for its slot (`MAKEVOCE.BAT` on the CD):
  `<name>r<k>` is message k - 1 (now and then a letter after), `w1`, `w2`,
  `l1`; checked for the 17 missions the challenge uses.  A chamber mission
  `h<row><n>` flies `<ROW>M<n>`, a battle's `b<b>m<m>` `<b>M<m>`.  At the
  challenge's launch the emulator makes its bank from the player's CD: for
  each slot with text the clip of the mission its line is from (the other
  mission's for a line replaced, its own for the rest), then its own win and
  loss clips; written to the game's drive as `Z95VOICE.LFD`.  The voice ids
  are then G and Z (4 and `'Z' - '1'`), naming `VOICE\GMZ\GMZ.LFD`, and
  `DOS_OpenFile`'s open of that name, on any drive, is the made file
  (`tie_voice_open`; every other open, and every other game, untouched).  A
  clip not found is a tenth of a second of silence, so the rest keep their
  slots; with no bank read (no CD) the challenge flies without voices, as
  before (the letter 7, past the table, names no bank).  Measured, HG4M on two
  machines, the host's sound recorded (WCLAB) and each clip looked for in it
  (normalized cross-correlation at 4 kHz): 9 clips, 3 of other missions';
  slot 0's B12M3 clip at 74.3 s (0.64), slot 1's own 6 s later (0.63), the
  Z-95 line's HA2W clip at 100.4 s (0.46; its words on the screen at 105 s,
  green), slot 2's B12M6 clip at 108.2 s (0.70); the replaced own clip of
  slot 0 and the lines not fired at the floor (0.08..0.09).
* **The cockpit.**  The Z-95's spec (13) is named XWING with the R-41's: when
  the player flies one it is renamed Z95, and Z95.INT and Z95FWD.LFD are made
  from the gunboat's (the shuttle's way, step 9): the struts out, an arched
  canopy frame (an ellipse's upper half over the top, its legs down to the side
  consoles, each pixel a strut's row across its width) and a long nose from
  the dashboard's middle towards the horizon in the struts' own greys (the
  panel's palette by luminance: lit along its top, a ridge seam, panel lines,
  seams across).  The nose's bottom sits on the dashboard (the user:
  "translate the long polygon downward so the bottom is flush with the top of
  the VDU display's bevel"): its lower corners' last row of space in the
  mask, row 277 of 480.  The targeting display gets the gunboat's mask.
* **The parts.**  A parts file (the INT names it at 0x62A, the count after) is
  the instruments' sprites one after another, PANL opcodes, each row ended by
  254, each sprite by 255.  The Z-95's own, Z95P.PNL, is the gunboat's with
  sprites 30..33 (the shield display's picture of the player's craft from
  behind, red, yellow, green, white by the hull; 19x14 at 640, 9x9 at 320)
  drawn as a Z-95's back (the user: "should be a z-95 shape if possible, or
  else just squashed somehow like a back view --8[]8--": wings with a gun at
  each tip, an engine each side, the canopy on top), each in its own colors
  by brightness.  No ion cannons (the user: "removing the top 2 icons from the
  crosshair since z-95 does not have ions"): the four gun lights around the
  targeting box are painted into the forward view itself (masked as panel),
  the game lighting one over each for weapon groups 0..3 (instruments 37..40,
  `panel_updatelasers`); Z95FWD.LFD opens the upper two (39 and 40's places)
  to space, and the box's missile-mode states (sprites 35..37, the lights
  drawn in) lose them in Z95P.PNL.  The Z-95 has two weapon groups.
* **The page.**  The room's menu calls the T/F row "Z-95 challenge 1..4: two
  Z-95s against the Empire", with the source missions' words.
* **Measured** (two machines): T/F mission 1 from the chamber flew HG1W (6 of
  its 13 groups mirrored, the line as a notice), a room's pick of T/F mission 3
  flew HG3M (25 of 28 mirrored), both pilots in Z-95s and the Z-95 cockpit,
  through the jump and after.
* **Open.**  The briefings still speak of the Empire's side; the side views
  are the gunboat's.

### Step 15: long mission tests with an ally (2026-11-07)

The user: "Do some comprehensive mission tests with the ally and make sure the
networking works, especially the guest handoffs...and that well aimed shots at
tie fighters work. The challenge missions are a good place to start as well as
some of the other missions"; then "Let's finish production using it and do the
things in the docs".

* **The rig.**  `scripts/tfnet-duo.sh`'s two machines through the front end
  into the room's pick, each flown by a scripted pilot from its launch on
  (the key script): full throttle (Backspace) and every 2 s the nearest
  moving enemy of the Empire's fighters targeted (`!tietarget=nearest:5-9`;
  the game's R took a T/F yet to launch, sitting still, and a container),
  every 0.2 s the craft turned to where a laser fired now meets the target
  (`!tieface=target:1.0`: the game's own reckoning, `collide_targetinrange`,
  a speed s covering s x 4660 / 256 world units a second, a bolt its
  shooter's speed plus 1,000) with the trigger held within 1 km (held from
  afar at full throttle the Z-95's lasers ran dry in 24 s before reaching);
  a click every 10 s (Fly Mission Again after a death): `scripts/tfnet-aim.sh`.
  Each machine's wall reports summed over the run (`scripts/tfnet-sum.py`):
  shots sent and made, hits sent and applied, owner changes, kills, kills
  credited; the commands at the top of `scripts/tfnet-duo.sh`.
* **Measured** (challenge missions, the native build, the two machines on
  one computer, 3.5 minutes each):
  * HI2W: every shot of each pilot made on the other (327 of the host's, 245
    of the guest's), 79 to 117 owner changes a run (the guest taking the
    craft near it and giving them back), an AI T/F's linked fire decided on
    the host and applied by the guest (four hits, 540 then 770 a hit past its
    shields: the game's own damage), the guest down, the flight over for
    both, Fly Mission Again flying both again, five times a run; the guest's
    hits on the host's craft 20 and 28 a run, every one applied.
  * HG3M: an AI fighter whose AI ran on the guest (handed to it) killing the
    host, its hits decided on the guest and applied by the host, its kill
    credited to nobody; the guest's two hits on a T/B the host flies applied
    there, the second beginning its death, the kill credited to the guest
    (below).
  * HG4M: the radio's voices (step 14).
  * HI1W (the T/I row, flown by two): an inspection; its B-wings had not come
    in four minutes, nothing to shoot.
  * B2M1FW (a battle's mission as the room's pick, two T/Fs against Rebel
    fighters, 33 flight groups): 46 owner changes; the guest's 19 hits on
    craft the host flies, 18 applied (the 19th's target dead by then: "not
    ours to apply"), the host's 5 on the guest's, 5; most of them an AI
    craft's, flown on one machine, hitting a craft flown on the other.  The
    far tier at last: the host sent 23,559 near reports, 5,563 mid and 80 far;
    the guest flew up to 14 far craft by its own AI, 299 of their shots shown
    there, 3,539 of the host's reports pulled them in, the error a second
    604 units at the median (15 m); now and then 4 to 16 km (a mission's
    trigger firing a little apart on the two machines: a craft launched or
    arrived at another moment), pulled in over the time to its next report as
    the user's far rule has it.  The host down at 171 s, the room's flight over
    for both, both flying it again.
* **Kill credit** (step 6's open item).  The owner of a craft keeps the
  shooter whose hit, applied there, began its death (the death timer running
  after the stand-in bolt's `collide_laserhitcraft`); its dying reports carry
  it (`TieCraft.killer`), and the shooter's machine, killing its copy, first
  calls the game's own `collide_updatekills(our craft, the victim, 1)` (TIE95
  0x16218): the craft's kills by species, the player's, the congratulations,
  a friendly kill's message, as for a kill of its own.  A death of the
  owner's own (its AI's shot, a collision) names none.
* **The impact flash** (step 6's open item).  OpenTIE now has the collision
  test's offset (`collidexoff` 0xEAB7C, y 0xEAB78, z 0xEAB74): the shooter's
  machine sends it with the hit and the owner writes it before its call, so
  the bolt becomes its flash where it met the craft on the shooter's machine.
  Measured, HI2W: every applied hit's flash at the shooter's impact to the
  unit, 264 to 453 units from a fighter's middle, 1,529 to 2,388 from the
  container's.
* **The page.**  The help said "in its first stage... the shots, the damage
  still runs on each machine on its own": now what flying together is.  A
  controller's buttons mean TIE's own things in TIE (step 7's open item).
* **Seen, the game's own.**  Two lines fired in one poll show one (HG3M's 0
  and 1) but both their voices play; the game's R targets craft yet to
  launch.  The machine running both games and the rig at once, a guest's
  frame rate fell to 12 to 16 at the busiest moments (60 alone): the
  computer's load, not the session's (step 5's frame-rate note).

