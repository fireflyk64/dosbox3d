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
that say where everybody is (section 7), the pilots near each other seeing each other's ships (section 8); the nav
points' owners and the rest are the plan of section 6.

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
4. **Done (section 8) -- others in space:** a player in the same system within reach of another (the same nav point's sphere) appears
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
15 s.  The emulator's log (level 1; the overlay showed these until the user had it stopped, 2026-11-08: "it kinda
breaks immersion"; what the game shows is in section 11): "GUEST1 launched in Troy, 531 m from you (near)", "GUEST2 on autopilot in Troy, 680 m from
you", "GUEST1 is at a base", "GUEST1 jumped to Pyrenees", and every 15 s while somebody shares our system, "In Troy
with you: GUEST1 2,143 m, GUEST2 2,143 m".  Near = within 20,000 m.  The page: `maxSeats: 8` (the Players box
allows 8 for Privateer, 3 for every other game, as before), room codes PRV- and PRF-.  Measured: two native machines
over TCP, three in a local lobby room, a native host and a browser guest in a room of 8 (both ways, the page's
autopilot heard by the host), and on the live site a page hosting a room of 8 through the public lobby.

## 8. The others' ships (step 4)

How the game makes a ship, found by watching its calls (the key script's `PRIVWATCH=para:off,...` logs each call
of those places with its caller and arguments):

| place | what |
|---|---|
| seg099:0074 | `place(this, type far, cast near, appearance far, pos near, matrix near)`: spawn, the place and attitude set, the object added to the scene's list (`seg081:000E(ds:66CE, obj)`); returns the object.  Every ship at a launch and at a nav point comes through it (callers seg077:049C, the encounters; seg085:038D, the player's own) |
| seg111:07B1 | `spawn(scene ds:67F7, type far, appearance far, cast near)`: the type found or loaded (`seg111:0184`: `DATA/TYPES/<name>.IFF`), its factory (the type's second vtable, ds:2197: `seg053:064B`, a ship of 0x8F bytes), the appearance attached |
| seg053:064B | the ship's constructor: vtables at +2 (0x20EF) and +0x4C; what a near pointer to a ship points at is its +2 |
| ship +0x06 / +0x0E / +0x4C | its type / its cast name (8 bytes: "PLAYER", "MER_DP" a merchant, "MIL_DA" militia, "PIR_DA" a pirate: the AI's and side's file in DATA/AIDS) / its controller |
| vtable +0x8C | the attitude set from a matrix (copied to +0x23) |
| seg042:0944 | an AI controller's think (vtable 0x14A8), once a frame; returns AL 1 |
| player record +0x75 / +0x7E | the player's appearance ("CLUNKER") and type ("PLAYTYPE") |

The player ships: TARSTYPE / CLUNKER (Tarsus), ORIOTYPE / TUG (Orion), CENTTYPE / FIGHTER (Centurion), GLXYTYPE /
MERCHANT (Galaxy) -- the type an NPC of that ship is made as, and the appearance the player record names.

**Calls from the layer.**  At seg088:0B2E's entry (the player's controls: once a frame in flight, a root segment) the
registers are kept, the call's data put on the stack (SS is DGROUP there: the game passes stack addresses as near
pointers to seg099:0074 itself), the arguments pushed and a far return to that same entry, and the function
entered; back at the entry with the stack where it was left, its result is taken and everything given back
(TIE Fighter's way).  The cast name must outlive the call (the ship keeps a pointer to it): it is written into the
tail of the program's usage line at DGROUP:0074 ("..., or RF to run Righteous Fire.", which no flight reads).  Test
aids: `!privspawn=TARSTYPE:CLUNKER:MER_DP:dx:dy:dz` (a merchant's Tarsus beside us: the game made it, its target
display named it, its AI hailed "MAYBE WE CAN DO BUSINESS SOMETIME" and flew off), `!privpos=x:y:z`.

**Puppets** (src/cpu/wcnet_priv_session.cpp).  While another pilot shares our system within 60,000 m we send our
ship's state (`PrivState`: place, attitude, velocity from the last frames, appearance) at most 15 times a second,
best effort, relayed by the host.  A pilot whose state is fresh (5 s), in our system and within 30,000 m is a ship
in our game: made by `place` as the NPC type of its appearance, cast MER_DP, its AI held (its think returns at once),
its place and attitude written every frame from its last report carried on by its velocity (1 s at most).  Past
40,000 m or with its reports stopped it is parked far off (kept for its return); a new flight of ours (launch,
jump, load) forgets them all (the scene they were in is gone); a puppet the game destroyed is made again.  The
overlay says "GUEST1's Tarsus is in your space".  Measured: two native machines, the host stopped, the guest flying
out to 10 km and back: each saw the other's Tarsus, targeted as "Tarsus" at the right range throughout (3 minutes,
nothing amiss).  Open: shots between pilots (step 6), the cast (a merchant: shooting the other's ship counts against
the merchants for now), a jump with a puppet about (the starting Tarsus has no jump drive).

## 9. Nav point owners and the others' encounter ships (step 5)

**The owner.**  Every 500 ms the host looks at each nav point somebody flies at (a pilot's `nav` in its presence:
the sphere it is within, read at the frame from the system's sphere list, ds:66C2) and names its owner: of those
present, the ones with a mission there (their presence's `missions`, a bit a sphere: not yet filled, section 12),
else all present; of those, the lowest seat that has not owned the point this round; everyone having had it, a new
round.  The owner stays while it stays; a point left by all is vacant, and the next comers choose again.  The
decisions go out as `PrivOwners` (every 3 s, and at once on a change); a game that has no word yet spawns nothing
while another pilot is already there, the host's word first.  Alone, a game is single-player as it is.

**What an owner's game does.**  Its wings spawn as they would alone (seg077:0265, the encounter spawn, is let
through); a game that does not own the point returns from that spawn with nothing made ("nav 1's wings are pilot
1's to spawn, not ours").  Ten times a second the owner sends every ship in its scene list within 60,000 m of itself
that is not a puppet -- `PrivScene`: for each, the game's object address (its id), its type, appearance and cast
names, place, attitude, velocity (from its last two reports), alive, and its four armour quadrants -- to everybody,
best effort.

**The others' games** make each such ship as a puppet of its own kind (`spawn` with the owner's type, appearance
and cast, the AI held, placed every frame from the last report carried on by its velocity for a second at most),
12 at once at most (`kNpcCap`: a list has a cap), parked far off when 40,000 m away or when the reports stop for 3 s
(a parked ship of the same kind is taken over by the next one), killed by the game's own death (section 10) when
the owner says it is no longer alive, and forgotten at a new flight.  Measured natively, two machines, the host at
nav 1 first: the host owned it and its pirates came; the guest, arriving, made none of its own and saw the host's
("pilot 1's TALNWIMP (TALMIL, PIR_DA) is ds:9B4A here"), the pirate as the host flew it.

## 10. Shots and damage (step 6)

The shooter's game decides the hit (seg053:1206, the game's hit on a ship: the ship at [bp+0xA], the quadrant at
[bp+0xC], the damage at [bp-6]); when the ship hit is a puppet (another pilot's ship or one of its encounter ships)
the hit is sent to its owner (`PrivHit`: owner, id, quadrant, damage; the host relays), and the owner's game applies
it to the real ship by the game's own calls at its next frame (shields, then armour, then the display when the ship
is ours, then whether it is intact, then its death: a chain of calls each at a frame's entry, section 3).  The
puppet's armour follows the owner's reports (the scene's four quadrants, and the state's for a pilot's ship), so a
hit shows on both sides.  A ship's guns fired (seg120:0004, the weapons record's ship at +0xA) by a ship of an
owner's scene is reported (`PrivShot`, at most one a ship per 120 ms, best effort) and its puppet fires its guns in
the other games (the control record's fire bit, set for a frame), so that the others see the shots.  Ship-on-ship
bumps with a puppet do no damage (everybody launches from the same pad).  Measured: the guest's puppet of the host's
pirate killed the guest's Tarsus with hits forwarded to the host, 121 shots replicated.

## 11. Talking: the room's chat on the comm screen, and faces

A line typed at the 0 key's chat prompt (the layer's, as in every game) goes to the room (`Chat`); in a game where the
sender's ship is present it is said by that ship: the sender's callsign and the line written into the ship's own
copy of its cast (the cast's name at +0xA, 11 characters, and the text over the three RECV message sets' strings),
its pilot's line set to the first id of the neutral set with the timer at 1 (`priv_ship_say`, at the frame: the
game's comm screen then shows the face, the name and the line, and the line's own speech if any is not played).
Without the sender's ship about, the line goes to the emulator's chat line as before.

The face is the pilot's choice: the page's Face box (Privateer only, remembered by the browser, changeable at every
launch; `WCPRIVFACE=<cast>:<portrait>`): a merchant woman (MER_DP, portrait 14), a merchant man (47), a militia
pilot (MIL_AA), a Confed officer (CON_SA), a hunter (BOU_AA).  A puppet is made with that cast; the portrait is the
cast copy's +0xE, which the game sets again at its own hails (a merchant's a woman's or a man's at random: a ship
made with 14 was 47 at its next hail), so the one asked for is written there at every line said (one of the cast's
own only: a foreign id crashes the game, 1510-FFFF).  Measured, two native machines: each line in the other's game's
own comm line ("GUEST1: HELLO FROM THE GUEST" under the cockpit, 4 s).  **Open:** the face on the comm screen.  A
spawned merchant with its AI running showed one for a line of ours once; a puppet (its AI held) never has, nor with
its AI let think for 60 frames, nor with the two more flags the game's own hail functions set (seg013:0A60..:
+0x1D |= 0x10 and ds:0E23 = 1).  Something else of the hail's remains to be found (a code trace through a pirate's
taunt against one through our say).

## 12. Limits: the game's near heap

The game's near heap is its own allocator (seg142:012B, not Borland's): a ring of free blocks from ds:3FF8 (a
block: its size at +0 with bit 0 for used, the block before at +2, the ring at +4/+6), first fit, nothing growing
it; a request with no block big enough is its error 1303 (seg050:0006, every code logged by the layer: "the game
reports error 1303"), and the program ends -- PRIV.EXE starts it again at its title.  `priv_near_heap` walks the
ring (free bytes, the largest block, the count; in the log's line every second).  Measured at Achilles: 17,284
bytes free as the flight begins, 15,202 once it settles; a Tarsus of ours with what its AI allocates some 850 bytes
(12 merchants spawned one by one, 16,966 to 6,698, the game running on at 13 ships with 6 KB free; 30 spawned 3 s
apart, the game ended at the tenth ship).  So a puppet -- a pilot's ship or an encounter ship of another's -- is
made only while the heap would keep 6 KB after it (`kNearReserve`, for the game's own wings, missiles and pods),
the pilots' ships first; what cannot be made is not shown ("no room in the game's memory for a pilot's ship ...:
not shown"), the mission going on with the ships that fit (the user: "It's ok if the limit is less than 8").  At
Achilles that is some nine ships of ours.  Nothing of ours is ever deleted: the game's own destructor (seg053:3540,
the vtable's second entry through the thunk 3BA7) on a ship of ours ended the game in every way tried (alone: its
error 0404; after the scene removal: the heap ring corrupted; with the owner word cleared: ended, no report), so a
ship out of reach is parked far off and reused by the next of its kind, and the test aid `!privdel=<mode>` stays
for another look.  The far heap (the EMS window: types, appearances, casts) has not been measured.

