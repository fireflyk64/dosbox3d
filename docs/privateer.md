# Wing Commander: Privateer in the multiplayer layer

The user's brief (2026-11-07):

> Many people yearn to play privateer multiplayer.  Unlike the other games privateer is very loosely coupled
> multiplayer.  Players need to be able to go wherever.  If they are somewhat near the same nav point, then they
> should spawn for each other.  We should allow up to 8 players in the same game.  We need to be sure not to break
> existing games like tie fighter retro vega or wing commander 1 or 2.  Since privateer operates so differently it
> will need to have the owner of the navpoint be a) someone who is present and b) of the present people someone with
> a mission objective at that nav point and host is the tie breaker.  That means if the host and 2 guests arrive at
> the nav point... the host's mission will fire first... once they all leave and if they all return then a guest
> will be chosen by order to own the nav point and simulate, and eventually all will have simulated the mission and
> the host will own it again.  If a player is on their own at a nav point, then it's all single player.  Basically
> whenever a player hits A or otherwise every 15 seconds players will broadcast their system and coordinates.  If
> they are close by they show up for each other and the lower player (or mission player if someone has a mission
> near by) will dictate the host.  The setup_wing_commander_privateer_1.01_cs_music_fix_(77119).exe is the file
> with all the privateer files -- people can purchase it from gog.com per usual.  It will need to extract and allow
> people to play together.  Privateer will also need a keyboard fix -- it didn't have the keyboard rampup that
> retrovega and wc1 and 2 did... so when you hit the key a bit it just hops over beyond the target, making it
> frustrating to aim possibly.  Note that if a player ejects cargo it should be tractorable by another character.
> Also note that if a steltek drone is following a player it should spawn but not more than 1.  And it should target
> the lowest player number in that region.
>
> Again it is imperative we do not disrupt the gameplay of other genres and types of games on this website since
> this one is pretty niche.

This document is the reference for what is built, what was found in the program, and the plan.  Status: the page
takes GOG's installer and runs Privateer and Righteous Fire, the arrow keys ramped (section 4), in rooms of up to 8
that say where everybody is (section 7); the pilots in each other's space and the rest are the plan of section 6.

## 1. The game as installed

GOG's installer (152 MB, Inno Setup 5.6.2) unpacks to the game's own files and `GAME.GOG`, an ISO 9660 image of the
CD (157 MB: `PRIV.TRE` 89 MB and `RF.TRE` 73 MB, the two campaigns' data, plus fonts and a picture).  GOG runs it with
`imgmount d game.gog -t iso` and a menu that starts `priv.exe` (Privateer) or `priv r` (Righteous Fire)
(`__support/app/dosboxPRIV.conf`: machine svga_s3, memsize 16, cycles max, a Sound Blaster 16 on IRQ 5, EMS, XMS
and UMBs on).  `PRIV.CFG` names the archives (`=D:priv.tre`, `=D:rf.tre`); `SOUND.CFG` the sound card.

The programs: `PRIV.EXE` (8 KB) is Origin's loader: it installs JEMM, Origin's own EMS manager, when the machine has
none (DOSBox has: it uses DOSBox's), and runs `PRCD.EXE` through DOS.  `PRCD.EXE` (937,760 bytes) is the game: a
real-mode Borland C++ 1991 overlay program, as the Wing Commanders are (454 segments, 183 overlays, DGROUP at
paragraph 0x6E4D of the image), so `scripts/wcexe.py -e <game>/PRCD.EXE ...` disassembles it (`segs`, `dis`,
`refs`, `xref`, `str`).  It runs in Mode X (unchained 320x200: the key script's `!shot=` reads mode 13h only; use
`!frame=`).  Saved games are files named by the player: `<name>.SAV` (Privateer) and `<name>.PRS` (Righteous Fire).

The archives (`TRE`: a count, then entries of 74 bytes: a flag byte, a 65-byte path, offset, length) hold EA IFF
files: `DATA/SECTORS/QUADRANT.IFF` (the four quadrants, each system's name, position and bases),
`DATA/SECTORS/SECTORS.IFF` (per system: `SCTR` with its nav spheres `SPHR` and its encounter script `SCRP`, cast
and all), `DATA/MISSIONS/*.IFF` (the plot's missions, S0MA..S7MB), `DATA/TYPES/*.IFF` (every ship and object type:
`DRONTYPE.IFF` the Steltek drone, `CPODTYPE.IFF` the cargo pod).

## 2. The page

`web/gamefiles.js` registers `priv` (detected by `PRCD.EXE`, run as `priv`) and `privrf` (Righteous Fire, the same
directory, `priv r`), `image: "GAME.GOG"` (drive D), GOG's machine as `dosbox`, 30,000 cycles (natively 28 frames a
second in flight at that), single-player (`multiplayer: false`, `noFlight`) until there is a session.  Saved games:
`saves: ["*.SAV"]` (`*.PRS` for Righteous Fire): a registry `saves` entry may now be a pattern, and the page keeps
every matching file at the game directory's top (web/wc.js, `saveFiles`); games with plain names are as before.

## 3. The program in memory (src/cpu/wcnet_priv.cpp)

DOS loads `PRCD.EXE` for `PRIV.EXE`, so the layer knows where it is at once (`priv_program_loaded`): it checks
"Type PRIV to run Privateer" at DGROUP:0074 (the build GOG sells) and reads the command line for Righteous Fire.
Measured: loaded at 02A0, DGROUP 70ED, in our DOSBox with GOG's machine.  `g_game` stays `GAME_NONE` and the Wing
Commander layer's `DS` 0, as for TIE Fighter: nothing of Wing Commander's hooks applies; the dispatcher sends a hit
on a watched IP to `priv_check_hooks` while Privateer runs (wcnet_hooks.cpp), and `priv_watch_list` adds its places
to the table.  Its own test aids (key script): `!privds=<name>` (DGROUP to a file), `!privtrace=<name>:<seconds>`
(every instruction's CS:IP counted for that long: two traces, with and without a key held, found the controls).

What is known so far:

| place | what |
|---|---|
| seg226 (para 61E8) | the int 15h hook (AH=4Fh, the keyboard's scan codes): calls the far handler at dseg:4FDC |
| seg229:000E | that handler: the held-key table `dseg:69DE + scan code` (1 held), the key event queue (seg229:01E7 in, 023F out to dseg:6A45) |
| seg088:0B2E | the player's controls, once a frame (see section 4) |
| seg087:000D | clears the control record |
| dseg:13EF/13F0/13F1 | the control mode: joystick, mouse, keyboard |
| dseg:66B4 | the player's ship object (a near pointer: objects live in DGROUP's heap); 0 at a base |
| ship +0x00 | its vtable (0x20EF for the class of the player's and three others in Troy) |
| ship +0x06 | its type (a near pointer: "PLAYTYPE" for the player's) |
| ship +0x14 | its place: int32 x, y, z, world units (z up) |
| ship +0x21 | its attitude: 3x3 int32, 1.0 = 1 << 24 (an odd offset: the class is packed) |
| dseg:66B2 | the player's record in flight ("PLAYTYPE", "PLAYER"); 0 at a base |
| record +0x54 | a near pointer to the system's name (the game's copy: "Troy"), what the destination display shows |
| dseg:CA74 | the player's controller (vtable dseg:2A06, "PLAYPLAN"; its per-frame method seg088:0C62 calls the controls) |

Units: the world's are 256 to the game's metre (the destination display's "Range: 70515m" is the distance in world
units >> 8, by the game's own approximate magnitude, seg128:0296).  At 150 KPS the ship covers some 34,300 units a
second.  A nav sphere's radius in SECTORS.IFF is in metres (12,000 and 17,000 for nav points, 2,000 for a jump
point or a base).  The 19-byte `SPHR` records: an id, the system's id (QUADRANT.IFF's: Troy 0x3A), int32 x, y, z,
the radius (int16), then kind (2 a jump point, 1 a base or planet), what (a jump point's destination system),
the nav point's number.  Troy's Achilles is at 15,360,000, 0, -10,240,000, where a new pilot launches.

The world's objects live mostly in EMS (pages mapped at D000): the cockpit's gauges there are C++ objects in Origin's
own heap (paragraph blocks with a size and back link), "KPS" and "SET" with their numbers.

## 4. The arrow keys (src/cpu/wcnet_priv_controls.cpp)

The game reads its controls once a frame in seg088:0B2E: the control record (a far pointer at the controller's +2)
is cleared (pitch +0x06, roll +0x0A, yaw +0x0E, int32 each, 0x100 = the stick at its stop), then the mode's routine
fills it: the joystick's (seg088:0AEA), the mouse's (seg088:0B0C) or the keyboard's (seg088:0439).  The keyboard's
reads the held-key table and gives a held key the stop at once: Down +0x100 pitch, Up -0x100, Left +0x100 yaw, Right
-0x100, Home (keypad 7) -0x100 roll, PgUp (keypad 9) +0x100.  A tap turns at the full rate for its frames: the nose
jumps past the target.

The layer watches seg088:0BAD, the instruction after the keyboard routine's call (a root segment: a fixed place),
and writes the record again from the same table, ramped as Wing Commander's and RetroVega's arrows are: an axis held
builds from a ninth of the stop to the stop in a third of a second (a ninth more every 37 ms), Shift with it gives
the stop at once, the opposite key or letting go puts it to nothing.  Measured (28 frames a second): a held Right
gives -28, -28, -56, -85, ... -256 from the tenth frame on, where the game gave -256 from the first; a tap of 150 ms
-28, -28, -56, -85 (the game: four frames of -256).  Only the keyboard mode is touched (the mouse and joystick are
analog already).  `WCNET_PRIV_RAMP=0` leaves the game's own.

## 5. The rigs

* `scripts/privnet.sh [seconds] [host's extra] [guests' extra]`: a host and guests (`PLAYERS=2..8`), each flown by
  privrun.sh's route, over TCP or a local lobby room (`LOBBY=1`, with build-lobby's emulator: `DOSBOX=`).
* `scripts/web-priv.sh`: the page's test (`ROOM=1`: a native host's room and the page its guest; `SEATS=8` with
  web-priv.mjs alone: the page hosting a room of that many, the lobby server's limit).
* `scripts/privrun.sh [seconds] [extra key script]`: one machine headless, booted, the opening skipped (Esc at 4 s),
  a new pilot (NEW at 0.14, 0.94; "joe" / "ace"), launched from Achilles's pad (the Tarsus at 0.45, 0.6, clicked at
  16 s): in flight in Troy at ~22 s.  `RF=1` Righteous Fire, `NOROUTE=1` from boot only, `PRIVDIR=` the game
  (default `privgame/`: the installer's top files and `GAME.GOG`).

## 6. The plan

The user's design, step by step; each step committed, tested and deployed as it is done, nothing of it reaching the
other games (Privateer's module is its own: `wcnet_priv*.cpp`, its messages its own, its page entry its own).

1. **Done:** the page runs it (installer, disc, saves), the arrows ramped.
2. **The map of the world in memory:** the player's system, position, attitude and speed; the nav points of the
   system and the one nearest; the objects in space (ships, cargo, missiles) and how the game makes and removes
   them; the player's missions and their nav points (the mission computer's list); the Steltek drone.
3. **Done (section 7) -- presence (rooms of up to 8):** a Privateer room; every player broadcasts its system, nav point and position
   when it engages the autopilot (A), jumps, launches or lands, and every 15 s otherwise; the overlay lists who is
   where ("ACE in Troy near Achilles, 4,000 km from you").
4. **Others in space:** a player in the same system within reach of another (the same nav point's sphere) appears
   as a ship of its own type in the other's game, flown by its own machine (place, attitude, speed sent many times a
   second while near, as RetroVega and TIE Fighter do).
5. **Nav point ownership:** the players present at a nav point have one owner, who simulates what happens there
   (the encounter, the mission's ships) and sends it; the others' games show it and do not make their own.  The
   owner: of those present, one with a mission objective at that nav point, the host first; when all have left and
   come back, the next of them by order, until each has had its mission there, then the host again.  Alone at a nav
   point, a player's game is single-player as it is.
6. **Shots and damage:** as TIE Fighter: the shooter's machine decides a hit, the owner of the ship hit applies it.
7. **Cargo:** a pod a player expels is in every present game, and the first to tractor it has it.
8. **The Steltek drone:** following a player, it is made at most once at a nav point (by its owner), and it goes
   for the lowest-numbered player there.

## 7. Presence (src/cpu/wcnet_priv_session.cpp)

A session of its own, as TIE Fighter's: the host is player 0 (the room's, or the TCP listener), a guest says hello
and is given its seat and the room's seats (`PrivHello`); the host relays a guest's messages to the other guests.
Where a pilot is (`PrivPresence`: flying, its system's name, its place, why) goes out when it engages the autopilot
(the A key in flight), jumps (the system's name changes), launches or lands (ds:66B4 set or cleared), and every
15 s.  The overlay: "GUEST1 launched in Troy, 531 m from you (near)", "GUEST2 on autopilot in Troy, 680 m from
you", "GUEST1 is at a base", "GUEST1 jumped to Pyrenees", and every 15 s while somebody shares our system, "In Troy
with you: GUEST1 2,143 m, GUEST2 2,143 m".  Near = within 20,000 m.  The page: `maxSeats: 8` (the Players box
allows 8 for Privateer, 3 for every other game, as before), room codes PRV- and PRF-.  Measured: two native machines
over TCP, three in a local lobby room, a native host and a browser guest in a room of 8 (both ways, the page's
autopilot heard by the host), and on the live site a page hosting a room of 8 through the public lobby.
