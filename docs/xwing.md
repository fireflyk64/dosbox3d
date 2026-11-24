# X-Wing Collector's CD-ROM (1994)

The user, 2026-11-23: "I have xwingcd folder with the xwing collector cd ... The goal is to add the multiplayer in the
same way as tie fighter with the upres the same as tie demo ... let users drag their xwing collector cd into the
system and play multiplayer coop mission. With 640x400". Then: "I don't have the gog installer yet ... let's just work
with cd and zip ... Let's allow 1-4 players coop if possible", and the first flight to share: a new pilot, Tour of Duty
I, Ackbar's briefing, "OP 1: Destroy Imperial Convoy", "flight group 1 of 3 and click on space to launch. Then you get
a xwing hyperspace out and hyperspace in".

Branch `xwing`, worktree `~/dev/xwing`. The code: src/cpu/wcnet_xw.cpp (the program found, its places, calls into
it), wcnet_xw_session.cpp (the room), web/gamefiles.js (`xwing`). docs/tiedemo.md is the guide to the session this one
is made from.

## 1. The game

* **The programs.** `XWINGCD.BAT` runs `BWING.EXE` (292 KB: a 16-bit real-mode program, Microsoft C, .RTLink/Plus
  overlays) with `BFRONT.OVL` (595 KB, the front end: registration, the concourse, briefings, debriefings) and
  `BFLIGHT.OVL` (258 KB, the flight; "Final V1.2 10/06/93"). EMS for its cockpits, music and sound (one handle of 128
  pages; without EMS: "Not Enough Memory!"), LANDRU.CFG and iMUSE as TIE Fighter's.
* **The engine is the TIE Fighter demo's**, an earlier build (its READ.ME: "X-Wing CD uses a new graphics system,
  similar to the one used in TIE Fighter"). Lined up function by function with TIEMISS.EXE (section 3) most of the
  flight is the same code instruction for instruction: the frame's calls in the same order, the same collision,
  damage, projectile and move routines, the same renderer segment. So the TIE demo's session, its rules and its 640x400
  renderer are the ones to port, not new work.
* **The CD.** The game looks for `\BWING.EXE` on a drive and reads `RESOURCE\`, `MISSION\` (the missions: `.XWI`, with
  `.BRF` briefings), `CLASSIC\`, `TALK\` and `CP\` (the cockpits) from it; the disc's top holds every file of the
  installed game too. The installed game is 59 files (pilots `*.PLT`, `OPTIONS.CFG`, `SETMUSE.INI`, the sound drivers).
* **GOG's "X-Wing Special Edition"** (not seen here; its listing from gogdb.org): the 1994 game's folder, "Star Wars
  - X-Wing CD (1994)", is the installed game's files with `game.gog` (the disc's image, 90 MB) beside them.
* **The files here** (`~/dev/dosbox3d/xwingcd`, untracked, the user's): `XWING/` the installed game, `xwing.iso` the
  disc (78 MB), `XWNOCP/` and `XWINGCD.bak/` an installed game with the disc's files in `CD/`. `XWNOCP/CD`'s seven
  forward cockpits (`CP\*12*.LFD`) are 2,199-byte blanks and its OPTIONS.CFG has the cockpit off: flown from it the
  game shows no cockpit (measured in stock DOSBox 0.74 too). The rigs fly `XWING/` with `xwing.iso`.
* **The front end's way to a flight** (measured; the pointer must be MOVED onto a spot before the click):
  registration (a roster; a new pilot: click the name box, type, Enter), the door on the left to the concourse; there
  Historical Combat (top middle), Pilot Proving Ground (top left), the tour's desk (top right: "New Tour" for a pilot
  with tours to fly, "View TOD Cutscenes" for one who has flown them all, as the TOPACE pilots have), Tech Room, Film
  Room, Register; a briefing's door on the right, then Pilot Assignment ("Flight Group 1 of 3": the roster's pilots as
  wingmen), a click on the hangar's view of space, the launch scenes (Space skips), the hyperspace jump, the flight.

## 2. The page (web/gamefiles.js `xwing`, tag XW)

* **What is dropped**: GOG's installer or a zip of its folder (`game.gog` beside the game); a zip of an installed
  game with the disc's files in a folder (any name: the folder holding `MISSION\*.XWI`) or the disc's image in it;
  or the disc's image alone, an `.iso` (`expandCdImages`: the files at its top are the installed game's, put beside
  it). `xwDisc` finds the disc among the files: the largest `.iso`/`.gog`/`.bin`/`.img` of 10 MB and more, mounted with
  `imgmount d ... -t iso`, else the folder, mounted with `mount d ... -t cdrom`.
* **Settings** (`xwPrepare`): LANDRU.CFG `fast`, `ems`, `diff`; SETMUSE.INI's two drivers (its first 28 bytes, 14 a
  name) made FM4.MDR and SB16.WDR whatever card the copy was set up for; OPTIONS.CFG the installer's FASTYES.CFG with
  the cockpit on when there is none. The machine: an S3, 16 MB, EMS, a Sound Blaster 16 at 220/7/1, 30,000 cycles.
* **Kept in the browser**: the pilots (`*.PLT`), the high scores (`*.HGH`), OPTIONS.CFG.
* `node scripts/web-xwing.mjs PAGE_URL ISO|ZIP [seconds]`: one page, the game dropped, a room of one flown from boot
  to Tour of Duty I's first mission. Ends "XWING PAGE OK".

## 3. The program in memory (src/cpu/wcnet_xw.cpp)

**Finding it.** DOS loads BWING.EXE (load segment 01A2 under our DOS); BFLIGHT.OVL is read when a mission is flown, to
a fixed place: its file is 1,855 far pointers to relocate (segments from 0920), then the code, file offset F at linear
(load << 4) + F + 0x7500. So the flight's places are paragraphs from the load segment, as the root's are. The front
end's overlay has the same place between flights: each hooked place's first bytes are checked when it comes up. DGROUP
is paragraph 3F1C (DS 40BE).

**Lined up with the TIE demo** (the scratch tools `xalign.py TIESEG:OFF XSEG:OFF` and `xmap.py`: both programs
disassembled, the instruction sequences matched, the numbers that differ paired; 140 functions found by following
the far calls from the frame):

| | TIE demo | X-Wing CD |
|---|---|---|
| the flight (makes the mission, runs the frames) | 0090:0006 | 0920:0006 |
| the frame | 0090:06CE | 0920:06AE |
| create_createmission(path) | 0FC2:000E | 1722:000A |
| ... its mission file read, the first random number next | 0FC2:0136 | 1722:00D0 |
| move_moveobjects | 054A:0000 | 0DFD:000A |
| laser_createprojectile(shooter, hardpoint, type) | 1338:0B16 | 19E8:0B0E |
| collide_laserhitcraft(bolt, target, offset) | 05B3:1B18 | 0E65:1A84 |
| collide_damagecraft(target, component, attacker, side) | 05B3:1CF8 | 0E65:1C52 |
| collide_updatekills(shooter, victim, 1) | 05B3:2CA8 | 0E65:2C16 |
| the frames over, the status not yet looked at | 0090:04FB | 0920:04B8 |
| the AI's planner | 091E:018C | 11B8:011E (another planner: scripts) |
| the renderer's segment | 1CCA | 222D |

**The frame** (0920:06AE): the wait for 4 ticks (DS:BF46; a frame's ticks DS:C802, the rate C948), then the user's
input (1AC1:0004), the clock (0920:144C), the flight groups (1722:0BEA), the AI (11B8:011E), the weapons (19E8:0000),
the dynamics (1145:000E), the picture (0920:0826), the collisions (0E65:0008), the moves (0DFD:000A), the animations
(1E43:05B0), 1722:2754 (the goals, judged once the status is 3), the messages (0C8A:04CA).

**The data** (DS offsets; wcnet_xw.h):
* The objects at DS:9886, 116 of 0x54 bytes: the demo's record less its first word (this build's objects have no id
  number). +0 category (0 a craft, 1 a projectile, 5 an explosion), +1 genus, +2 type (the mission files' craft
  types: 1 X-W, 2 Y-W, 3 A-W, 4 T/F ...; 0 a free slot), +4 x y z (int32), +0x10 the frame before's, +0x1C heading,
  pitch, roll, +0x24 speed, +0x28 radius (a bolt's damage), +0x2A the death's timer, +0x2C age, +0x2E a bolt's
  shooter, +0x31 side, +0x36 the move vector, +0x3E forward, side, up, +0x50 the craft data (near, then DS).
* Craft 0 to 27, projectiles above (made from slot 115 down).
* A craft's data (0x170 bytes, from DS:7046): +5 its flight group, +6 its leader, +0x24/+0x26 the planner's reload and
  countdown (the planner runs a craft whose countdown is 0), +0x3C its attacker, +0x41 its flight flag, +0x42 its
  maneuver (0x12 boarding), +0x94 its hull's damage against +0x98, +0x9B its systems, +0xB6 and +0xB8 its shields.
* The player: DS:BFAC its object (near), BFAE its craft, BFB0 its object number, BFB2 its flight group, BFB8 its
  target. The random generator: its state DS:5ECA (two bytes), its last number 5EC8. The flight's status DS:6F5A, its
  end flag 6F5B.
* The mission as read: the file's header at DS:C72C (0xCE bytes: the flight groups' count at C7F6), the flight groups
  in the mission segment (load + 3E3E) from 0xE8, the file's own records of 0x94 bytes (+0x32 type, +0x38 craft in a
  wave, +0x44 arrival by hyperspace, +0x82 the player's craft in it, 1-based, +0x86 its order, +0x8E its goal). Each
  flight group's status at DS:C80E, 20 bytes: what its craft did and had done to them, which the goals are judged by.
* The mission's path: create_createmission's argument, DS:0C96 ("d:\mission\defect.xwi").

**The mission is made twice.** The flight's start makes it (0920:011D), and when the player's flight group arrives
through hyperspace flies the jump as a scene (the frames run with DS:E62E set) and makes it again (0920:0197). The
room's launch is the last making: the first passes as it is, and where the game would not make it again it is sent
back to (0920:0193).

**The places watched**: create_createmission's entry and its mission read, 0920:019F (after the first making), the
frame, move_moveobjects (the others' craft written in; our calls made), laser_createprojectile, collide_laserhitcraft,
collide_damagecraft, 0920:04B8 (the flight over), PSP:005C (the launch's idle, a `jmp $`). Calls into the program as
the demo's (`xw_queue_call`): CPU state only, no byte of the game written.

**Research aids** (key script): `!xwcraft` (every craft: slot, identity, flight group, type, place, hull, shields),
`!xwpose`, `!xwr=<DS offset>`, `!xww=<DS offset>:<byte>`, `!xwkill=<object>`, `!xwahead=<object>:<units>[:<seconds>]`
(that craft held ahead of our nose: 8,000 units is gunnery range; 1,200 is inside a freighter). `WCNET_EMS_TRACE=1`
lists the EMS calls.

## 4. Flown together (src/cpu/wcnet_tiedemo_session.cpp, built for X-Wing as wcnet_xw_session.cpp)

The TIE demo's session (docs/tiedemo.md section 5), the same source file compiled twice (the user: "we should ideally
use the same code to do it, so the fixes are shared"): wcnet_tdm_game.h gives each build its game (`XG(tie, xwing)` for
a place or an offset, `TDM_NS` for the object's fields by name, `#ifdef TDM_XWING` around what is one game's own);
the wrapper defines TDM_XWING and includes the source; the shared names are renamed for it (scripts/t640-names.sh).
The flight's keys likewise (wcnet_tiedemo_controls.cpp as wcnet_xw_controls.cpp). So: who
flies a craft (the host most; a guest those near its nose or its target), the rates, puppets, shots made with the
game's own laser_createprojectile, hits decided by the shooter's machine and applied by the owner's, deaths, kills
credited, craft named by flight group and arrival. What is X-Wing's own:

* **Rooms of up to 4.** The host is seat 0.
* **The launch.** Every pilot goes through its own front end (its own pilot file, any mission it likes) to the click
  into space. Its launch then waits at the mission's last making until every pilot of the room is there (two minutes
  at most for one who is not); the host says go with its random seed, its mission file's path and how many pilots
  fly. A guest's game makes the HOST's mission: the path is written over its own (its own drive letter kept).
* **The seats.** The mission's player flight group is made as large as the pilots flying (its craft count raised, on
  every machine alike: Tour of Duty I's first mission has two X-wings in Red, a room of three flies three). The host
  flies the craft the mission gives the player; seat 1 the group's first craft the host does not fly, seat 2 the
  next.
* **No id numbers**: a slot's new object is told by what the slot held at the last look (empty; an explosion and now
  a craft; a craft of another type or flight group).
* **The flight's end.** A pilot down, or the host's flight over (its jump home too: the host's frames stop), ends
  every pilot's flight 3 s later.
* **The result is the host's.** A boarding runs only where the boarder's AI runs (the host flies every craft whose
  flight group has a boarding order), so a guest's game would never see the corvette taken. Every pilot sends its
  flight groups' status every 2 s; in flight each count of it (bytes 3 to 13 of a group's 20: they only grow) is
  taken from whoever has the larger, so a boarding, a capture or an identification counted on one machine is counted
  on all; and a guest's game is given the host's whole table as its own flight ends, before the goals are judged
  (0920:04B8).
* **A craft's side** goes with its report (a captured craft changes sides where its captor's AI runs), and its
  **warheads**: the craft data's weapon slots are 4 bytes each from +0x112, six of them, the count the byte at +1
  (measured: a torpedo from each of an X-wing's launchers, slots 4 and 5, 3 to 2).
* **The flight's keys** are the demo's: the arrows steer by a ramp (a tap a little, full in a third of a second,
  Shift the whole stick), the keypad's Ins and Del roll, Space fires and confirms, F12 changes what the mouse does
  (point to aim by default). X-Wing's input is the demo's code (its poll lined up: 657 of 700 instructions): what
  Space confirms at DS:BFC7, the right button's hold at DS:BFD0 / BFD2; the gunsight's middle is read from the
  projection's centre, each cockpit's own. Measured: Left held 1 s turned the nose 50 degrees and Right 1 s brought
  it back to within 4; Ins rolled 62 degrees with the heading unmoved; Space 1 s made four shots.
* **The trench run flown by two** (Historical Combat, the B-wing's sixth; `ROUTE=trench scripts/xwnet-duo.sh`): both
  pilots over the Death Star's surface in B-wings at 640x400, each seeing the other and the TIE that attacks.

**Measured (2026-11-23, natively over TCP, scripts/xwnet-duo.sh: each machine from boot to Tour of Duty I's first
mission by the route's keys and clicks):**
* Two machines launched together with the host's seed, the guest in craft 1 of Red, the host craft 2; all ten craft
  at the same places on both (tables a second apart, the far freighters within the frame's move).
* Gunnery: each pilot held a freighter 8,000 units ahead and fired 24 s; each machine made the other's shots, the
  hits on the other machine's craft were applied there, and both freighters were destroyed on both ("FREIGHTER ????
  HAS BEEN DESTROYED" on both pictures).
* The host's jump home (H, Space): the guest's flight ended, its goals judged by the host's status, both back aboard
  with the debriefing.
* Three machines (PLAYERS=3): Red made three craft, each pilot its own, the tables the same on all three.

## 5. The flight at 640x400 (src/cpu/wcnet_tie640*.cpp built for BFLIGHT.OVL: wcnet_xw640*.cpp)

The TIE demo's 640x400 flight (docs/tie640.md): the game's scene section run once at twice the scale, every call of the
renderer's lower half taken down and replayed by hand-written tracers for each quarter of the picture, a hand-written
scanline pass, the game's 320x200 (cockpit, instruments, text) doubled behind it with the cockpit's edge smoothed.

* **One source, built twice.** X-Wing's renderer is the demo's at other addresses (lined up in a chain over the whole
  segment: 16,025 instructions paired, 478 variables). `XA(tie, xwing)` gives each build its place or variable
  (wcnet_t640_game.h); wcnet_xw640*.cpp define T640_XWING and include their twins; the shared names tie640_... are
  xw640_... there (scripts/t640-names.sh). The VGA's side asks whichever game's picture is on.
* **What X-Wing's build does another way** (found by listing the aligned instructions whose constants differ, and by
  the call-by-call check):
  * the scanline pass's work lists and run tables are in a block it asks for each frame (its segment at DS:4E5B),
    not a fixed segment; the rows' table is at DS:D3C8, the code segment's row offsets at CS:D400;
  * a sprite's box is grown by 4 pixels, not 2; 6Fh flat objects at most, not 77h;
  * a marking's record holds 16 layers (colours' indices 6 bits, tables of 64 at CS:0A0F and 0A6B), the demo's 8;
    the layers are the top 16 object numbers;
  * a shaded face cut by the near plane is not made flat (the demo's 1CCA:5D18 is not there): the edge that closes
    a cut face is its own code (23CF:5FA1 to 6740; `close_cut_face_xwing`): the lights are the words before the two
    cut points, stepped along the closing edge; an edge wholly beside the window is one running record (right of
    the window for a shaded face only);
  * **its divisions are not rounded**: where the demo's tracers call a by-cases rounding division (1CCA:F59D, and
    F6F0), X-Wing's call a plain 32-bit one (23CF:F875, F897). An edge that comes in from just beside the window
    came in a row off: this was the seam at a quarter's side (the user's "cut line"). Found by a face whose two
    lists differed by one running record at x = the window's width;
  * the near plane's clip is in two places, not four; the static objects' records are 14 bytes.
* **Checked** (`WCNET_T640_TRACECHECK=1`, each call of the game's tracer against the hand-written one on a copy of
  the memory; Tour of Duty I's first mission, a freighter held ahead): the line tracer the same in 68,246 of 68,246
  calls, the sprites' row pass in 6,616 of 6,616, flat polygons in 3,478 of 3,478, the face tracer in 26,446 of
  26,446 (a freighter 9,000 units ahead) and 18,696 of 18,696 (150 units ahead: faces cut by the near plane).
  `WCNET_T640_CHECK=3 WCNET_T640_SPR1X=0` (every frame in quarters by the game's own code, then in one pass, the
  sprites the game's way): 1,863 frames, the game's state never differs and no pixel does.
* **The gunsight** is drawn at the finer grid (docs/tie640.md): its one-pixel strokes one fine pixel wide.
* **Sprites** are the game's own art as it draws it at 320, each pixel four (docs/tie640.md: one run of the scaler a
  sprite; the user: "I like the idea of drawing sprites with 2x2 pixels instead of upres since the original art is
  meant for lower scale anyway").
* **Speed, against the host's clock** (the user: "the interesting number is the minimum framerate over 30 seconds of
  gameplay"; `!wallfps`, and a line every 30 s of flight). On the page (headless Chrome), a freighter held 12,000
  units ahead under fire, then destroyed: the fewest frames in a second 43, the mean 62, the emulator at 99% of real
  time; the game's own 320x200 picture in the same scene: 16 at the fewest, the mean 43. Natively 58 at the fewest.
  Two pages in a room: 56 to 62 at the fewest (30 once, both pages on one machine).
* **Seen**: the cockpit, the view without it, the look-around views, the in-flight map, fire, a kill, the rescue
  scenes after a death.
* `WCNET_TIE640=0` keeps the game's own picture; `WCNET_T640_NATIVE=0` draws the quarters by the game's own code
  (four times the scene's work: 32 frames a second where the one pass makes 62).

## 6. Open

* Not flown at 640x400 yet: the proving ground's gates.
* Dockings as events (the TIE CD's kinds 6 and 7): X-Wing's AI is script-driven and its boarding routine is not
  found (the craft data's +0x3A is the docking's minutes, maneuver 0x12 the boarding); today the boarder's machine
  counts it and the counts are merged.
* The comms prompt.
* The mission a guest sees briefed is its own pick, the mission flown the host's.
