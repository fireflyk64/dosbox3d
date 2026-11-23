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
`refs`, `xref`, `str`).  It runs paged, in chained mode 13h (320x200, double-scanned to 400 lines, pixels 1.2 times as tall
as they are wide on the 4:3 monitors of its day; measured 2026-11-22, this said Mode X before; the key script's
`!frame=` is what the window showed).  Saved games are files named by the player: `<name>.SAV` (Privateer) and `<name>.PRS` (Righteous Fire).

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

**A controller (2026-11-21).**  The user: "I tried using the joystick but I was given 3 bad options... if I selected
keyboard in the options menu the joystick was inert. If I select mouse, the joystick drove around the blue dot and so I
could press right for a bit and my ship would be truning right...but then to start turning left I would have to push my
joystick all the way past center to left...which was way too slow to steer. And if I selected "joystick" as the option
my ship was spinning around like absolute crazy--I think the joystick option is actually the right one but it wouldn't
let me control at all".  Why: the page never counted Privateer's flight as a flight (`wc_web_in_flight`), so a pad moved
the mouse as in a menu (the pointer keeps its place: the mouse mode turned until it was pushed back past the middle),
the keyboard mode reads no mouse, and the joystick mode read an empty game port: every reading timed out, the stick
read hard over (the record +131064 / -131064) and the ship spun.  The options' three buttons (seg037:0004, 0018,
002C) write the mode's three flags (ds:13EF joystick, 13F0 mouse, 13F1 keyboard) and nothing else.  Now the layer also
watches seg088:0BBD, where the modes' readings meet (not when ds:1142 or ds:23B6 is set: the frame then reads no
controls), and:
- a pad that is not built in (and a phone's touch stick) is a flight's stick (`wc_web_stick`, said every 16 ms, let go
  after 250 ms unsaid): it writes pitch and yaw, the stop being the stick at its end, in any mode; an axis an arrow is
  held on stays the arrow's (the keyboard's mode); its roll is the keypad's 7 (left) and 9 (right), ramped here in the
  mouse's mode, which reads no keys;
- the joystick mode with no stick on the port steers by the arrows, ramped (the spin gone);
- each pad is offered "Built in" too (the registry's `joystick`): the launch then puts stick A on the port
  (`joysticktype=none`, `timed=false`, as for Wing Commander), and when it comes onto the port the layer picks the
  options' JOYSTICK, as its button would; a mode picked later stays the pilot's.  The game reads it its own way: GOG's
  JOYA.DAT has it calibrated 0 / 64 / 128, the untimed port's readings, but with a dead band of 45..83 around the 64:
  measured (`!joy=x:0`, the record's yaw), nothing until 32% of the stick, then a jump, the stop at 80%.  So with a pad
  built in the page writes its own JOYA.DAT (`PRIV_JOYA` in web/gamefiles.js, `JOY=1 COPY=1 scripts/privrun.sh`): the
  band 64..64, 2 reads a step, and the emulator moves a stick off the centre by a read at least (`port_off_centre`,
  wcnet_hooks.cpp): the page's dead zone (4%) is the only one (the user, 2026-11-23: "we need to reduce the default
  deadzone in wing commander privateer and this to 4% --- I think 5% is too much dead air").  Measured: 5% of the
  stick turns 16 of the stop's 256, 10% 24, 20% 48, 40% 104, 60% 152, 80% 208, 100% 256.  Measured: button 1 the guns, both buttons a
  missile, button 2 with the stick sideways rolls and forward / back sets the speed (SET 150 -> 300); at a base the stick
  moves the game's pointer.
Measured natively (`!privstick=x:y`, the page's stick; `JOY=1 scripts/privrun.sh` and `!joy=`, the built-in pad; the
ship's matrix each second): half stick turns 40-47 degrees a second in the keyboard's, the mouse's and the empty
joystick's modes, built in 0.6 of the stick 46-52; at the middle each stops within the second; the arrows still steer
in the keyboard's and the empty joystick's.  The Pointer choice is now in the Controller list (the user: "the "Shown a
ring in flight" should just be Kbd + mouse (shown) (which is the default) and Keyboard + mouse instead of a whole
sepratae menu"): "Kbd + mouse (shown)" and "Keyboard + mouse" (the game's own: none), also changed while flying
(`wc_web_priv_pointer`).

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
its pilot's line set to the first id of the neutral set with the timer at 0 and the hail's flags (`priv_ship_say`,
at the frame), and the ship's AI -- held, as a puppet's is -- let think for 60 frames (its guns kept quiet
meanwhile): the line is played only as the pilot's own think runs, and then the game's comm line under the cockpit
shows "GUEST1: HELLO FROM THE GUEST" for some 3 s (measured on two machines, both ways; with the think held nothing
showed though the sets were written).  Without the sender's ship about, the line goes to the emulator's chat line
as before.

The face is the pilot's choice: the page's Face box (Privateer only, remembered by the browser, changeable at every
launch; `WCPRIVFACE=<cast>:<head>`): a merchant (MER_DP), a militia pilot (MIL_AA), a Confed officer (CON_SA) or a
hunter (BOU_AA), each a woman (head 33 and over: the page sends 47 for the merchant, 33 for the others) or a man
(-1 or under 33); another machine's cast is taken only from those four.  A puppet is made with that cast.

**Where a line shows** (the user: "the lines of written text should go as text in the hud not in the vdu itself").
The game's comm line is one object, ds:67E4 (seg098): 01F9 takes a line (the speaker, the line, its kind), 03A0 runs
it every frame -- it makes the string "NAME: text" (02C8, +0xC) and its time (+0xF, 0x333) -- and the comm screen's
mode (ovr359) draws the speaker's face, copying the string to the cockpit's message bar under the screens.  The
speaker's comm record is its controller + 8 (seg042:0AA0): +2 the cast, +8 the face picture's frame, +0xA its voice.
The game picks the frame the first time the face is drawn (seg096:0168 -> seg097:007F): a random one of the cast's
frames (the cast's +2 first, +4 count; the last +6 of them the other head -- in all four casts the first a man, the
other a woman) and with it the voice, the cast's portrait id (+0xE, +0x21 for the other head).  With a voice, Sound
Blaster and SPEECH.PAK (the CD game's, ds:13EB and ds:6930 set) a line is played as the recorded speech of its kind,
another sentence than ours, the bar's text left out and the line ended when the speech does; and as the voice was
picked in the middle of our first line, that line ended at once (03A0 found a voice and no speech playing) and its
face never showed.  So `priv_ship_say` sets the frame itself (the first head, or the other's first frame for a woman)
and the voice to none (0xFF) before every line: from the first line on, the face on the comm screen and the text on
the message bar together for the line's 3-4 s (measured, one machine and two: "GUEST1: HELLO FROM GUEST ONE" under a
hunter's face, "HOST: HELLO FROM THE HOST" under a Confed officer's, both ways).  The text goes over the three RECV
sets' strings with their size kept (the rest spaces: a later, longer line is not cut to an earlier one's length;
measured 87/111/108 bytes, unchanged after "HI"); the bar shows a 48-character line whole.  Aids: `PRIVCOMMLOG=1`
(the comm object every change, the speaker's frame and voice), `PRIVSAYFACE=<head>` for `!privsay`,
`!privwatchctl=<offset>:<seconds>` (the instruction that changes a word of the last spawned ship's controller, with
the code trace; it found seg097:0126 writing the voice).

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

## 13. Cargo pods (step 7)

**The game's pod.**  X in flight expels the hold as one pod (seg036:0C90, from the hold's list: one pod a unit for
cargo of kind 0x29, else one pod of all; a ship's death makes pods the same way): type CDBRTYPE, appearance from the
commodity (RAWMAT for iron and uranium), name "CARGO" (dseg:13AA), vtable 0x1584; a ship's layout up to the alive
flag at +0x20 (the place at +0x14), then at +0x4B its cargo list -- a word, the first node (far) at +2, the last at +6;
a node {next far, record far}; a record 0x28 bytes at +0x10 of a 0x38-byte far block that begins with the commodity's
name ("Iron", 12 bytes), a word -3 and the block's own segment: the record's +0 points back at its block, +4 the
pod's type name, +0xD its appearance's, +0x16 the category, +0x17 the kind, +0x18 the units -- and at +0x56 the units
of all its records.  A pod drifts with the ship's velocity and a push, and lives some minutes (a counter at +2).  The
tractor beam (7,500 at the dealer's Repair and Upgrade; the new Tarsus's Shield Generator 1 sells for the same) is a
weapon of the W key's cycle ("W: Tractor beam", with the missiles; G cycles the guns alone) fired with Enter: held
on a pod 120 m ahead, the pod is drawn in within some seconds, its records into the hold, the pod deleted (measured
alone: the pod gone from the scene list; !privaim=120:1 had put the ship facing it, the matrix's second row being
"forward").

**Shared.**  Every pilot's `PrivScene` (the owner's with its encounter ships; anybody's with pods alone, nothing sent
when there is none) carries each pod of its scene within 60,000 m that is not a puppet and not taken: its id, type,
appearance, cast "CARGO", place and velocity, `pod`, every record's 0x38-byte block (`cargo`) and the `units`.  The
others make it by the game's own place as any encounter ship (within 30,000 m, the memory reserve kept at 256 bytes
a pod), then give it its cargo: for each record the game's far malloc (seg000:089D, a call for the block and one for
the node), the block written with its own address in the record's +0 and the word before it, the node at the list's
tail, the units at +0x56; placed every frame from the reports, no AI to hold, no armour, never killed: parked far off
when its owner stops reporting it (taken, expired, or the owner gone).  A pod puppet gone from our scene while it
was here and fresh -- our tractor beam took it (the game moved its records into our hold and deleted it) -- is told
to its owner (`PrivPodTaken`, reliable, relayed to the owner), who puts its own pod far off (never deleted: the game
lets it expire there) and reports it no more; the other pilots' puppets of it go stale and are parked.  So the first
to tractor a pod has it; two tractoring the same pod within the same instant would both have it (loose coupling).
Measured, two native machines: the guest's pod of 2 iron and 1 uranium made on the host with both records in the
host's own far heap ("Iron" 2, "Uranium" 1, 3 units), following the pod's drift.

**Base screens** (Achilles; the pad's "Main Concourse" at 0.93, 0.50): the concourse's Hangar (0.15, 0.45), Ship
Dealer (0.50, 0.45), Merchants' Guild (0.70, 0.35), Bar (0.70, 0.50), Mission Computer (0.80, 0.55), Mercenaries'
Guild (0.90, 0.30), Commodity Exchange (0.90, 0.50); a booth is left by a click at the screen's bottom (0.5, 0.96).
The exchange: BUY (0.24, 0.40) then the item picture (0.18, 0.22) a unit; SELL (0.12, 0.40); Next Item (0.39, 0.15),
Previous (0.39, 0.29); 2,000 credits to begin with, iron 46, uranium 403.  The dealer: Repair and Upgrade (0.60,
0.22), Purchase Orion / Centurion / Galaxy, Main Concourse (0.39, 0.08); its upgrade screen: SELL (0.15, 0.96), BUY
(0.30, 0.96), the next item (0.43, 0.80) and the previous (0.43, 0.68), the item picture (0.18, 0.72) buys or sells,
"Ship Dealer" (0.04, 0.29) leaves, the Software Booth (0.50, 0.25) sells maps.  For sale: Laser 1,000, Cargo
Expansion 5,000, ECM 3/2/1 30,000/15,000/5,000, Repair Droid 30,000, Jump Drive 10,000, Tractor Beam 7,500, Tungsten
Armor 1,500, Plasteel Armor 1,000, Afterburner 3,000, Engine Upgrade 1 10,000, Shield Generator 2/1 30,000/10,000, FF
Missile 100.  The new Tarsus sells its Laser for 750, Plasteel Armor 500, Shield Generator 1 7,500, 5 DF Missiles 10
each, Missile Launcher 7,500: a jump drive and a tractor beam are had for the shields and the launcher (the user's
recipe, "sell your mk2 iris and get a mk1 iris", read for this version).  Test aids: `!privscene` (every object of
the scene list: ships, pods with their records, the rest with their bytes), `!privaim=<metres>:<variant>` (our ship
put that far from the nearest pod, facing it), `!privkill`, `!privhold`.

**The video** (the user: "record the system in action to a video").  Two native machines recorded (`WCLAB=<dir>`
through privnet.sh's HOSTENV / GUESTENV), the host having sold its shields for a tractor beam, the guest having
bought three iron; both launch from Achilles, the guest's turn (left held 3 s) carries it across the host's nose at
some 100 m while its "HELLO THERE HOST" sits under the host's cockpit, the host's "WELCOME TO TROY" under the
guest's; the guest stops and expels the pod (X), the host turns to it (`!privturn=pod:40:24`), closes at 72 kps
(`equals` thrice) holding Enter, and the beam draws the pod in; the guest's game puts its own pod away.  Cut by
scripts/tfnet-video.py (1080x1920, the host above, the guest below, the two recordings lined up by their wall
clocks) into 28 s: `scratchpad/privateer-together.mp4`.  Frames of the game's comm line are found in a recording
with scratchpad/commline.py (the light box at the picture's bottom).

## 14. Mission objectives, the Steltek drone, the face (were open; section 16)

Mission objectives, the drone and the face on the comm screen were open here; they are done (section 16 for the
missions and the drone, section 11 for the face).  The search that came first: the Mission Computer (concourse 0.80,
0.55; the palm plate 0.72, 0.82; arrows 0.25 / 0.34, 0.82; Accept 0.47, 0.81; out at the bottom) gives missions such
as "ATTACK MISSION ... Retro ships at Nav 7" in Troy and rings the nav point in orange on the map; the sphere records
(ds:66C2) carry no mark of it -- the objectives are the accepted missions' own (section 16).

**A jump with puppets about** (the user's recipe read for this version: sell the Missile Launcher, 7,500, and the
Plasteel Armor, 500 -- not the laser or the shields, the launch point's encounter kills an unarmed Tarsus -- and buy
the Jump Drive, 10,000).  Found so far, alone and on two machines: the nav map (N) selects a nav point by its
numbered button at the bottom (the "1" button at 0.115, 0.95; a click on the map itself did not), Esc closes it, A
then flies the autopilot's cutscene and drops the ship 11 km from it ("Navpoint near"); the map's Nav 1 is sphere
id 0, where the jump object sits (JUMPTYPE, appearance JUMP, name "JMPPYRI", at 30,000, 40,000, 20,000 m in Troy);
the autopilot refuses while an encounter is about (`!privpeace`, a test aid, kills every ship of the scene but ours
and the puppets; the guest's puppets of them die with the owner's reports).  J pressed from 3-8 km out five times did
not jump: the ship flies past the point after the drop, so it must be steered into the sphere first (the next try:
`!privpos` beside it, world units = metres x 256).  On the two-machine run the host's flight ended of itself at
119 s, its heap at 16 KB and no error reported -- a death between frames, most likely (the jump point's own wing);
untested beyond that.

## 15. Stability: the game's memory, our calls, the limits (2026-11-08)

The user: "Highest priority is any stability issues.  We need to either solve them or limit the number of players such
that they won't happen."  Everything here was measured natively with rooms of up to 8 (scripts/privtests/).

**The game's near heap** is its own allocator (seg142:012B) in its 64 KB data segment: from the end of its static data
(`[0163]`, 6C04) to `0xFFF6 - _stklen` (`ds:620A` = 0x1000: the stack EFF6-FFF6), some 33.8 KB of which 17 KB are free
as a flight begins.  Nothing grows it, and a request no block can meet is its error 1303 and the end of the program.
More conventional or upper memory (fewer drivers, high memory) does not reach it -- it is inside the data segment --
and helps only the far heap and the overlays, which never ran short.  The only bytes that could be moved to it are
the stack's never-used bottom: painted at the program's load (`PRIVSTACK=1`), the stack never came within 956 bytes
of the heap's end through intro, menus, booths, conversations, autopilot, fights and deaths; ~500 of those (3% of the
heap) are not worth a silent corruption should a rare path go deeper, so it stays as it is.

**Watched** (every 1,000 instructions, the allocator's lock byte ds:4001 respected; the per-second log line): the
heap's free and its lowest this second and ever, the largest block, our puppets, the allocations (1,000-2,000 a
second in a fight, the largest 142 bytes, 1,593 once), the stack's headroom at each allocation (the game's own deepest
1,404 bytes over the heap's end; ours, inside our calls, never below 2,700), the scene's ships, the system; every
error the game reports (seg050:0006) with the heap's state; our own ship's death.

**Found and fixed:**
1. *Cargo multiplied.*  A ship dying in the owner's game died as a puppet in every other game too, each dropped its
   own pods and shared them: one death's cargo some N x N times in every game (the owner's heap went to 3,240).  A
   puppet's death now skips the game's cargo ejection (seg053:0C0B -> seg036:1692; the dying ship's allocation in DI,
   our pointer 2 bytes in): the owner's real pods are the only ones, shared.
2. *Puppets took local damage.*  A hit on another's ship was sent to its owner and applied here as well: under fire
   puppets died, exploded and were made again over and over.  A puppet takes no damage here; its owner's game
   decides.  A pilot's own destruction is told to the room (`PrivState.destroyed`, reliable) and its puppets die
   with it.
3. *Calls outliving their object.*  Each queued call of ours (spawns, the hit chain's calls with a ship's far damage
   model, a pod's cargo) carries a guard -- its ship or pod still of its kind, in the scene, its model the same --
   checked as it starts; a failed one is cancelled cleanly.  The queue is flushed at every flight change: a landing,
   a death, a new flight, a jump (a jump now forgets the puppets too: the game rebuilds the scene).
4. *More puppets than held AIs.*  The held-AI list had room for 16; 7 pilots and 12 encounter puppets need 19.  Now 32,
   and a puppet whose AI cannot be held is taken away again.

**The memory governor** (`govern_memory`, every frame).  A puppet is made only while the last 5 s' low stays 8,000
over its cost (600 a ship, 200 a pod), one at a time, 2 s apart, none within 10 s of a give-back; encounter ships
first (co-op: an enemy not shown still shoots).  Below 6,000 one puppet a second is given back (four a second below
4,500): moved far off, its AI let go, killed -- the game's own death path frees it, without cargo -- parked ones first,
then the other pilots, then encounter ships, the farthest first; pods are never chosen (a pod's death frees nothing).
And the game's own encounter first: when the owner's game is about to spawn one of its wings while our puppets have
squeezed its heap (the last 2 s' low under 9,000), the wing waits -- the game calls its spawn again every frame -- and
a puppet is given back; with none of ours left the wing spawns whatever the heap says, as alone.

**Measured** (8 players at Achilles' nav point, everyone firing guns and missiles at the nearest pirate; the owner
is the host).  Before the fixes, against 4 added pirates: guests 7,900-11,000 free at their lowest, the owner
3,240-4,760; the same fight with the owner alone: 6,672-7,714.  After:

| added to the natural encounter (3) | guests' lowest | the owner's lowest | the owner shows of the 7 others |
|---|---|---|---|
| 4 pirates (three runs) | 9,400-11,500 | 4,884-5,614 | 5-6 through the fight |
| 3 pirates, co-op | 10,250 | 5,302 | 2-7 |
| 5 pirates, co-op | 6,380 | 3,834 | none at the peak |
| 12 pirates, co-op | 6,650-8,570 | its game ran out (1303) | none |

The binding limit is the owner's game: a fighting AI ship costs it about 1 KB, a held puppet anywhere 450-850 bytes
(a ship type not yet loaded some 300 more, once).  With all 8 together the natural encounter and about 3 more ships
are safe, 5 more the edge; 12 more is more fighting AI than the game itself ever puts in one place, and its own heap
cannot hold them whatever we do.  Players spread over several nav points each have their own owner and encounter.
4 players: guests >= 11,160, the owner >= 5,276.  The game's own encounters (2-6 ships) are within this; anything
that adds enemies for a bigger room must keep under it.

**The jump test** (scripts/privtests/jump.sh).  Three pilots in Troy; one buys a jump drive (credits from the rig's
`!privcredits`, its laser kept), jumps Troy -> Pyrenees -> Troy 15 s after each arrival: 6 jumps, then J does nothing
(the fuel, the ship 163 m from the jump point); a second jumps with its third trip out and stays.  The host, 2 km
from the jump point, held at 17,830 bytes the whole time (the jumper's puppet parked and taken up again, never made
again); the jumper's heap at its arrivals 17,314 / 18,140 / 18,278 in Troy and 18,318 / 15,526 / 15,540 in Pyrenees
(the encounter it cleared there): no drift; the two in Pyrenees saw each other and its nav 0 passed to the second
when the first jumped home.  No error, no death, the program never restarted.  A jump is made from within 500 m of
the jump point (1,500 m: nothing); the jump points are scene objects (JUMPTYPE, named JMP<system>: Troy's JMPPYRI at
30,000, 40,000, 20,000 m; Pyrenees' JMPTROY at 25,000, 45,000, 15,000 m), and an arrival is beside the one back.

**The rigs' aids** (key scripts only, never the page): `!privcredits=N` (the credits, a 32-bit global at ds:655A),
`!privfind=V` (where a value lies in the data segment), `!privrelease`, `!privpeace`, `!privturn=pirate|ship|pod:
deg/s:secs`, `!privpos=x:y:z` (world units, 256 a metre), `!privscene`, `!privgonav=<sphere>|m<seat>[:<metres>]`
(into a nav sphere, or a pilot's first mission nav point), `!privarmor` (our damage model's words), `!privhit=<quadrant>:
<damage>` (a forwarded hit on ourselves), `!privsay=<name>:<text>` with `PRIVSAYFACE`, `!privwatchctl=<offset>:<secs>`;
`PRIVSTACK=1`, `PRIVCOMMLOG=1`, `PRIVMISSIONS=<sphere,...>`.  The tests: `scripts/privtests/load.sh` (PLAYERS,
PIRATES), `scripts/privtests/jump.sh`, `scripts/privtests/ships.sh` (PLAYERS: the four ships bought and outfitted) and
`scripts/privtests/mission.sh` (MODE=first|reserved: a mission's nav point with helpers), `scripts/privtests/escort.sh`
(the Oxford escort from a save, two machines).  More aids: `!privcredits` (read), `!privfollow=<cast>:<metres>:<secs>
[:<sphere>]`, `!privgopilot=<seat>:<metres>`, `!privkillhostile`; privrun.sh's `SAVE=<file>` (a save into the copied
game directory; the title's LOAD at 0.38, 0.94, the name at 0.31, 0.405, into the box at 0.31, 0.345, LOAD at 0.85,
0.345).  The per-second log's "pirates" counts every hostile cast (pirates, Kilrathi, Retros, hunters), as
`!privturn=pirate` turns on any of them; the missions line gives each objective's state byte.

**Cast names** (were open: every ship we spawned pointed its cast name at one shared scratch, so all read the last
written): now 16 slots of 9 bytes in a block taken from the game's own near heap at the program's load, a cast a slot,
the oldest slot taken over when all are used.

## 16. Co-op, missions, escorts, the drone, the four ships (2026-11-08)

The user: "See how many random encounters can spawn in addition to the 8 players.  This is mostly coop not PvP and
if no one can spawn when players are present then it may be boring"; "ensure that escort missions like Oxford
function with several helpers.  Also grant enough money for a test rig to buy each of the four ships and make sure
that the multiple players at the same nav works".

**The game asks once.**  A nav point's wings spawn as the player's ship enters its sphere: seg077:0265(wing far) once
a wing (measured: a refused wing is never asked for again on that visit).  So the owner is decided at that call by
every machine alike: the host's word when it has one, else the same rule (`choose_owner`) over who is there by
presences and fresh positions; `PrivOwners` carries each nav point's owner and who has had it this round (`done`).
The wing valve no longer refuses a wing for memory: our puppets are given back to make room (`make_room_for_wing`).

**Pilots flying together.**  A game deciding at the wing's call with no word from the host yet counts as present the
pilots inside the sphere by their fresh reports and, since 2026-11-09, those within 5 km of its own ship
(`pilots_with_us`): a formation crossing a sphere's edge together otherwise saw itself alone in every game and each
spawned the encounter (measured with four pilots arriving at Nav 4 together: the host's pirate wing and guest 3's).
After: four pilots gathered 4.5 km out and entering together -- every machine chose pilot 1, one wing.  (The nav
spheres' radii, now in the log: 12,000 m, the bases' 17,000 m.)

**Co-op extras** (more enemies for more pilots, made by the game itself).  As a hostile wing (cast PIR, KIL, RET, BOU)
is about to spawn in the owner's game, its ship array (wing +0x3B, near; the count at +0x53, 8 words at most) is
replaced by a larger one from the game's own allocator and the count raised: +1 for 2-3 pilots present or within
20 km of the sphere, +2 for 4-5, +3 for 6-8 (`extras_for`), as many as the heap affords (its last low minus 8,000,
1,200 bytes a ship).  The game then makes them as its own -- formation, orders, attack (measured: a pirate wing of
2 got 2 more and they attacked; at nav 6 the heap allowed 1); the old array is freed (seg000:03BA) at the next frame.
Ships placed by our layer instead (spawned and angered) only idled: no orders.

**Missions.**  The player record (ds:66B2) +0x5D holds 4 near pointers to the accepted missions; a mission's
objectives are at +0x2C (far, 5-byte entries: a far pointer and a byte; the count at +0x2E); an objective whose byte
2 is 9 names a nav point, byte 3 the map's Nav N (sphere N-1) -- read at the frame in flight (`missions_refresh`,
checked against the nav map's rings), sent in the presence (`missions`) whenever they change.  The owner rule prefers
a mission holder present.  Two more rules for missions (scripts/privtests/mission.sh):
- **Reserved:** a nav point that is a mission's of a pilot flying in the system is left to that pilot's game while
  it is not there (`kOwnerReserved`): a helper arriving first spawns nothing ("a mission's of a pilot not here yet"),
  since a mission's ships are made by its holder's game alone and a refused wing is lost.
- **The holder spawns:** at its own mission's nav point a holder's game spawns its wings even when another pilot
  owns it (its mission not yet known when that one came): the nav's own encounter may then stand twice, the lesser
  harm.
Measured, three machines (the holder's mission taken at the Mission Computer, PRIVMISSIONS=2 fixing the nav point:
the list is random): reserved -- the helper there first spawned nothing, the holder's wing (two merchants) came with
it and showed on the helper; first -- the host's own encounter and the holder's wings both, shown everywhere.

**Escorts: every game's own ships are shown.**  Every game now sends the others all of its own ships within 60 km of
it (not puppets), wherever it is -- not only the owner of a nav point its encounter.  A game that does not own the
nav point has no others there (its wings were refused; measured, a guest's scene was its puppets alone), so what it
sends is what came with it: an escort's ship, a mission's ship met at a nav point, pursuers.  The holder's game flies
them; the others show them as puppets and send their hits to it.  Measured: the holder's own merchant (an escort's
stand-in) shown on both helpers at Achilles, a nav point it did not own.  The Oxford escorts (the plot's third series,
S3MA / S3MC / S3MD: "Meet with ... at Oxford system, Nav 3 [or 1] and escort ... safely to Oxford") are reached only
late in the plot and were not flown; they are mission ships at a mission nav point, which these two rules cover.

**The Oxford escort, flown with a helper** (2026-11-09; the user's save OXFORD01.SAV with Masterson's series open,
loaded by both machines; scripts/privtests/escort.sh).  The holder takes S3MA from Masterson in Oxford's Library (the
pad's "Train to Oxford University" at 0.95, 0.45; the campus's Library at its top right, 0.88, 0.20; his lines skipped
with Esc, the hand's Accept): "Meet with Hunter Toth's ship at Oxford system, Nav 3 and escort him safely to Oxford",
objectives E0 (Nav 3), D5, D3 (Nav 4, Oxford; each entry's own byte turns 1 as it is reached).  The mission plays as
the user described it: at Nav 3 (sphere 2) the Drayman "toth" waits (the mission makes it itself: no wing call); the
talk is the comm (C), its list -- the base and the nearest ship -- "2" for Toth, and his menus in turn: "Hey, I'm no
Retro." / "Sir, I'm here to escort you to Oxford." / "Don't worry.  You're in good hands." (TOTH.IFF's PLOT sets);
"Of that I'm sure", and two Retros (talrelig, cast RETRO1) come ("Hunter Toth!  You are hereby sentenced to death for
your heresy!"); when they are gone, three more; then the autopilot home, Toth following at his own pace (some 165 m/s:
the autopilot leaves him 3 minutes behind), and the landing zone lands you.  Measured, two machines: the holder owned
Nav 3 (its mission's), the helper's game spawned nothing there; Toth and every Retro shown on the helper as the
holder's (its hostile count 2, 3, 0 with the holder's); the helper's hit on a Retro (175) applied in the holder's game;
Toth home, the holder landed, paid: 189,187 credits to 199,137 (10,000 less the landing's 50).  No error, no death.
The fight itself was the rig's weak part: its gunnery is a turn toward the nearest hostile and a held trigger, which
sprayed the helper (13 hits sent) and, left to it, the second wave killed Toth in 12 s and then the holder; so the
waves are ended by `!privkillhostile` (the game's own death for every hostile ship of the holder's game) after the
helper's real fight -- a won fight's stand-in.  Rig lessons: Oxford's own hostiles keep the autopilot from flying
(`!privpeace` first), a ship drifting into a base's landing zone cannot take the autopilot, the comm's list offers the
nearest ship only (with the helper beside the holder "2" was the helper: it joins after the talk, `!privgopilot`),
key names: `equals` for the throttle, W selects a missile, Enter fires it.  Two players who both take the mission each
get their own Toth and Retros (each game's mission makes its own; each is shown to the other): not flown.

**The Steltek drone.**  A drone (DRONTYPE, appearance DRONE, cast SDRONE) is shared by the game that runs it like any
own ship; one a place: a drone reported by a lower seat within 30 km of our own makes ours its puppet (`drone_adopt`).
Each second the game running it aims it at the lowest seat present within 30 km, by replacing the old target's ship
pointer and damage-model segment in its AI plan (far, the controller's +2; the words found in its maneuver records)
-- measured on pirates retargeted at the guest: hits on the guest 1 before, 51 after.

**The four ships** (scripts/privtests/ships.sh; 500,000 credits from the rig's `!privcredits`).  At Achilles' Ship
Dealer (concourse 0.50, 0.45) Purchase Orion (0.32, 0.71), Centurion (0.10, 0.35), Galaxy (0.67, 0.64): the
salesman's twelve-line pitch (Esc skips a line), then a hand, Accept (0.5, 0.9); the new ship waits on the pad.  A new
ship comes bare: no shields, no armour (its damage model all zero; the Tarsus 250 a quadrant), and the game's own rule
(seg053:1366 -> seg101:0C53: a quadrant's armour at 0 or less after a hit is death) kills it at the first hit through
-- measured, one forwarded hit of 20 killed a bare Orion; in a test of 8 bare ships 6 died at the launch's first
pirate.  So the rig outfits it as a pilot must: Repair and Upgrade (0.60, 0.22), the list's down arrow (0.43, 0.80)
to Tungsten Armor (the Orion's and Galaxy's item 8, the Centurion's 7) and then the best shield generator (the
Orion's 5, item 17; the Centurion's and Galaxy's 3, items 14 and 16), each bought by a click on its picture (0.18,
0.72); a gun needs a mount chosen (not bought).  The puppet of a pilot's ship is made by its appearance (TUG an
Orion, FIGHTER a Centurion, MERCHANT a Galaxy, CLUNKER a Tarsus).

Measured with the outfitted ships (no error, no death, the program never restarted anywhere):

| test | the owner's lowest free | the guests' lowest | shown |
|---|---|---|---|
| 4 pilots (Tarsus, Orion, Centurion, Galaxy), 3 pirates added | 7,540 | 11,734 | each the other three, as their kinds |
| 8 pilots (two of each), 3 pirates added | 6,434 | 9,924 | each machine all 7 others, as their kinds |
| load.sh again after "every game's own ships shown", 8 Tarsus, 3 pirates | 6,048 | 10,878 | 7 pilots and 6 encounter ships on every guest |

The last is no worse than before the change (section 15: the owner 5,302, guests 10,250): with nobody's escort about,
a game that does not own the nav point has nothing of its own to send.

**The video** (2026-11-09, `scratchpad/privateer-coop.mp4`, 44 s, cut by scripts/tfnet-video.py from the host's and
guest 1's recordings).  Four pilots from Achilles (faces: the host a Confed officer, woman; guest 1 a hunter, man):
guest 1's "HELLO FROM GUEST ONE" on the host's message bar under the hunter's face, the host's "WELCOME TO TROY
PILOTS" on guest 1's under the officer's; a Steltek drone made beside the group goes after pilot 1, the lowest seat
(the host's target screen, 1-4 km); the four gather outside Nav 1 and enter together, the host's game spawns its pirate
wing of 2 with 2 more for the four, and all fly it (the four Talons destroyed in 35 s).  The take's script:
scratchpad/vidtake.sh (`!privgonav` gather at 13.5 km, enter at 11 km, then 300 m; `!privturn=pirate`; the drone by
`!privspawn=DRONTYPE:DRONE:SDRONE`, removed by `!privpeace`).

## 17. Missions with several helpers; the page's fixes (2026-11-09)

The user: "now test with some of those defend nav point missions and a few other missions with multiple helpers"; on
the page: "I also don't see an option to select comm portrait", "0 also doesn't seem to do comms", "also zero music
here", "a random drayman I met had a male face but female voice".

**The same missions every run.**  The Mission Computer's offers come from the random numbers, seeded by the clock;
`WCNET_BOOTCLOCK=HH:MM:SS` (a rig's: src/ints/bios.cpp) boots at that time of 2026-01-01, and the lists are then the
same each run (measured twice, Oxford with the user's save and Achilles with a new pilot).  At 12:00:00 Oxford deals
2 ATTACK (CMF A), 3 CARGO (Troy), 4 DEFEND BASE ("Oxford in Oxford system seeks reinforcements ... strong Kilrathi
blockade", 3,890), 5 CARGO (Padre), 6 PATROL (Perry); Achilles deals 1 DEFEND BASE (Retros at Helen, 6,539), 2 DEFEND
BASE (Hector, 6,351), 3 and 4 CARGO (Newcastle), 5 ATTACK (Retros at Nav 5, 5,765), 6 SCOUT (Nav 7, 2,976).
Objectives (the missions line gives each entry's state byte, 1 when met): D0 a nav point to visit, D6 a nav point to
clear, E2/NN the base to land at for the pay (01 Achilles, 02 Oxford).  The Retros of these missions are cast COM_AF,
COM_DF, COM_SF ("This attitude not possible" but hostile): now hostile to the co-op extras and the rigs' aids too.

**Measured** (no error, no death unless said, the program never restarted):
- scripts/privtests/defend.sh: Oxford's defence, four machines from the user's save, the host the holder, three
  helpers.  The two Dralthi (KIL_AF) come with the launch, in the holder's game (its mission's nav point: the helpers'
  games refused their wings there and showed the holder's two, then their deaths); the attack ended (the rig's
  stand-in), every pilot docked: paid 189,187 -> 193,027 credits (3,890 less the landing's 50).
- scripts/privtests/missions.sh: two holders and two helpers, new pilots at Achilles: the host takes the scout and
  Helen's defence, guest 1 the attack at Nav 5; together to Nav 5, Nav 7, Helen.  Paid: the host 2,000 -> 11,465 (the
  scout 2,976 and the defence 6,539), guest 1 2,000 -> 7,715 (the attack 5,765); each nav point owned by its holder,
  the others' wings refused there; every mission ship, and its death, on all four machines; the helpers' hits (9 sent)
  applied in the holders' games.
- The escort (section 16) with one helper; the Oxford defence's helpers before the fix below, likewise.

**Found and fixed: dead ships kept alive in the others' games.**  A ship that died in its owner's game left the scene
before a report said it was dead, so the others kept its puppet: parked when the reports stopped, but alive and
hostile in their game for good.  Helen's defence ("when calm returns") was never met with another pilot's long-dead
Retro so kept in the holder's game (measured: the Helen Retro killed, the objective still 0; alone, met at once).  Now
a ship sent in the last 2 s and gone from its owner's scene is told dead (the receivers kill its puppet: "destroyed, as
its owner says"; a death told of a ship never shown makes nothing), and an encounter puppet with no word for 20 s is
given back (the game's own death, no cargo).  With the fix the defence was met (2,000 -> 11,465 above).

**The rigs' fighting.**  `!privturn=hunt` leads the nearest hostile by its speed, holds the guns when lined up within
2 km and nothing friendly is in the line, fires a missile every 4 s within 4 km and the afterburner beyond 1.5 km --
still no match for a Dralthi; `!privturn=stalk` puts the ship 350 m behind the nearest hostile (a test of what a hit
does), but by a base it carried helpers into the landing zone, which landed them (a defended base's attackers fight by
it: the holder hunting one landed too); so the fights are ended by `!privkillhostile` in the holder's game, after the
pilots' own shots.  At a base the autopilot will not fly ("Destination: Oxford"): the rigs dock by `!privgonav=<base
sphere>:0`.

**The page** (live): the Face box shows in a Privateer room (it sat in the room's row of choices, hidden for a game the
room does not fly); a TIE Fighter room code turns PRV when Privateer is picked (the codes' pattern lacked TF); the 0
key opens the chat prompt in flight (a Shift tap did too until 2026-11-10) (it opened only for Wing Commander's session; the room test types a
line and the native host hears it); the music plays (GOG's SOUND.CFG names music card 3, the "roland" driver: General
MIDI for Windows' synthesizer, none in the browser; the page's `prepare` makes it card 2, "adlib", the Sound Blaster's
OPL at 0x388: measured, the title's music at once); the help says how to talk.  Cast names: a slot is given to another
cast only when no ship of the scene points at it (a ship reads its cast's name again later: a slot taken from under it
gave it another cast's), else the puppet is not made.  The drayman the user met with a man's face and a woman's voice:
not one of our puppets with a chat line (those have no voice) nor a pilot's (those fly the four player ships); a ship
of the game's own, or another's encounter ship given another cast's name by the old slots.

## 18. A play test's batch (2026-11-20)

The user, after flying a room of two on the page: "the mouse cursor was black when I selected mouse play from the
alt-o options menu...it still steered the ship but was not available"; the guest's view of the host's fight (shields,
gunfire, missiles), ramming, enemies ignoring the guest, hits landing on the wrong ship, and friendly fire.  One commit
an item.

**The mouse's pointer.**  The game draws its own pointer (its shape blitter, seg191; DOSBox's mouse driver never draws
one): a cross with serifs in palette index 0x0F on an outline of 0.  0x0F is white on the bases' screens (58, 58, 59)
but nearly black on the options screen over a flight (Alt+O: 7, 6, 6), and in flight the game shows no pointer at all,
though the mouse flies the ship by where it is (seg088:02E5: the game's mouse x, ds:6A7E, 640 across, less the middle's
ds:3C5B = 320, over 10 is the turn; its y, ds:6A80, less ds:3C5D = 100, over 3 the pitch).  Measured by the video
memory and palette dumped on each screen (`!privvga=<name>`: DOSBox's chained 320 x 200, a pixel at
`linear[((a & ~3) << 2) + (a & 3)]`).  Now, by the page's Pointer choice (Privateer only, remembered by the browser,
`WCPRIVPOINTER=ring|game`; since 2026-11-21 the Controller list's "Kbd + mouse (shown)" and "Keyboard + mouse", section
4): "shown" (the default) draws TIE Fighter's amber ring where the mouse is while the flight
runs with the mouse flying (not over the options screen or the nav map: the frame stops there), and on a screen over a
flight where 0x0F is dark redraws the game's pointer white (its 0x0F pixels in the 7 x 7 round the cross's middle at
the mouse; the options screen's own 46 pixels of 0x0F, in its buttons' lettering, stay); "as the game has it" leaves
both as the game has them (the user: "if that's how the original has it then many purists may want it that way").
Measured, one machine each way: the options screen's pointer white over the grid, the MOUSE button and INVULNERABILITY
with "shown", black with "game"; in flight the ring at the mouse's place at (0.7, 0.3) and (0.3, 0.7) of the picture,
none with "game".

**Shields on the others' screens.**  A ship's damage model holds a quadrant's shield as an int32 at +0x31 + 6q, its
armour's int16 just after it (seg101:0CF0 takes a hit off the shield: the damage times the model's +0xA; 25,600 full on
a Talon and a Tarsus).  Only the armour went in the reports, and a pilot's ship sent neither, so a puppet's shields
stood full in every other game whatever its owner's game said (and a pilot's armour too): the target display showed the
guest a pirate untouched while the host's game had its rear shield down.  Now `PrivNpc.shield` and `PrivState.armor`,
`.shield` carry them, written on the puppet every frame (a report without them, an older build's, leaves the puppet's
own).  Measured, two machines, the host's pirate shot by the guest (scripts in the scratchpad: !privdmg, every live ship's
shields and armour at the next frame): the host's 25600 2734 25600 516 / armour 80 63 100 23, the guest's view of it
25600 2734 25600 962 / 80 63 100 23 (a report later); at the start 25600 x 4 / 80 80 100 90 on both.

**Explosions from nowhere** (the user: "sometimes I'd arrive at a nav point and get random explosions--I'm worried the
multiplayer somehow deleted craft that were overloading the system and treated them as explosions"; "one time when I
heard it though my pursuant was still following me").  The game's dead-ship handler, seg053:0A0B(this, scene, ship),
runs only for a ship whose alive byte is 0: its death begun (an explosion, the kill's credit at 0B6F when the pilot was
angry at us, its comm line stopped, the cargo) and, when its time is out, the explosion's sound -- 0x1B by seg246:089D
with no place given (0A5D) -- and its removal from the scene; a dead ship stays in the scene about a second (alive 0).
Measured with the sound recorded (WCLAB): a ship killed 50 km off is as loud as one 2 km off.  And the layer killed
ships far off: every puppet it gives back (the memory governor's, which also takes encounter ships near us when it
must; one with no word for 20 s; one whose AI cannot be held) is moved far off and killed so that the game frees it.
So when another pilot left (an autopilot, a jump, out of reach), 20 s later each of its ships we showed exploded
loudly in our game; and a ship left behind in its owner's game (its scene made anew at a jump) was reported dead and
exploded where we showed it.  Now:
- a puppet given back (`release_obj`: its object in the session's `quiet` list) dies without the sound (0A5D skipped
  to 0A73) and without a kill credited to us (0B6F to 0B9C);
- the owner's game notes its own ships' deaths (the dead-ship handler's entry, and a report of one not alive); a ship
  sent in the last 2 s and gone from its scene without one is reported `gone` (`PrivNpc.gone`), and the others give it
  back quietly; a dead one, as before, dies in place where it is shown (sound and all), or quietly where it was parked
  far off.
Measured, two machines (the guest's sound recorded): the guest's give-back of a puppet (!privrelease) silent; the
host out of reach (`!privgonav`), its two merchants given back on the guest 20 s later, silent (each a boom before);
one of the host's merchants taken out of its scene without a death (the test aid `!privunscene`, the dead-ship handler's
own removal) -- "gone from its owner's game: given back quietly" on the guest, silent; the host's pirate killed ten
seconds later -- "destroyed, as its owner says", the explosion heard.

**The shooter's game decides a hit; a copy's shot is harmless** (the user: the guest "didn't seem to take damage or
maybe took little"; "sometimes I got a very good attack on the enemy as the guest but the full whallop went to my
ally, almost as if the numbers were mixed up").  A ship of another machine's fires in our game when its owner says so
(`PrivShot`, section 10), so that we see its shots -- and those copies hit for real: in the host's game the guest's own
merchant's copied bolts struck the host's pirates on top of the hits the guest's game sent (measured, 32 a bolt
applied twice), and a copy can strike the host's own ship; in the guest's game the copies of the pirate's shots struck
the guest while the host's game sent its own hits on the guest's puppet.  The hit routine's hitter ([bp+0xE]) now gives
the ship behind each hit -- a ship itself in a bump, else its shooter by the game's own "who fired" (the vtable's
+0x14): a laser bolt's (vtable 0x2DE8) +0x25, the other bolt's (0x2F22) and a missile's (0x3160) +0x4C, the program's
only three projectile classes (a scan of its vtables) -- and `priv_session_hit` decides what our game keeps: a shot by
a puppet (a copy) nothing, and nothing is sent; a shot by our own ship or one of our game's ships on a puppet nothing
here, sent to its owner (`PrivHit.by_pilot` says whether our own ship fired it); anything else as the game has it.
Measured, two machines, the guest stalking the host's pirate (level 3 logs every hit with its shooter): the guest's
three missiles sent and applied in the host's game (the pirate destroyed by them), the host's militia's crossfire on
the guest's puppet (a missile and three bolts) sent to the guest and taken there once.

**Rams** (the user: "if the guest crashed into the ship it would take damage but the guest didn't seem to take damage
or maybe took little.  Moreover if the guest crashed into the host both would take damage but I thought the option was
that friend collisions were ignored or much reduced").  A bump with any puppet was nothing anywhere, so a guest ramming
the host's pirate came off free.  Now a ram is decided by the game whose pilot's ship it is (its own place is the true
one there): our ship and another machine's encounter ship -- ours takes its share here, the other's share goes to its
owner (the same pair in the owner's game, its ship and our puppet: nothing); two pilots' ships: nothing anywhere (the
pad they all launch from); our game's ships and another's encounter ships: nothing.  Measured, two machines (the test
aid `!privturn=ram`: our ship put 40 m ahead of the nearest hostile, nose to nose, every 2 s): three rams of the host's
pirate by the guest -- the guest's ship took 43+14, 46+17, 44+13 there, the pirate's 57+18, 61+23, 57+17 went to the
host's game and were applied (its rear shield 25,600 to 392, armour 100 to 63) and shown on the guest; then the guest
put 12 m from the host three times (`!privgopilot`): every bump between the two ships kept 0 on both machines, their
shields and armour full.  (No ram between pilots hurt before either; what the user saw was likely the copies' shots of
the previous item or the pirate's.)

**Enemies turn on the pilot who provokes them** (the user: "the enemy wouldn't really react to fire or comms from the
guest, so the guest was basically just pummeling the enemy without any risk to thsmelves").  The game angers a ship at
the player -- its pilot (the plan's +0x23) gets +0x19 bit 1 and +0x1A one more -- at a hit of the player's (seg053:1157)
and at a hostile line of the player's comm (measured: "Live or die.  Your choice." to a pirate, +0x19 FC to FE, +0x1A 0
to 1), in the guest's game on the guest's copy of the host's ship as on any ship; in the host's game, where the ship
flies, nothing of it happened.  Now every game watches the pilots of the ships it shows for others: a rise is told to
the ship's owner (`PrivHit.provoke`, damage 0, once in 2 s a ship), as is every hit of our own ship's
(`PrivHit.by_pilot`); the owner's game turns the ship on that pilot's ship (its AI's target replaced in its plan, as the
drone's is re-aimed: `turn_on_pilot`, once in 3 s a ship), and only from a target it has -- caught between maneuvers
with none, it is tried again each frame for 6 s; a ship after nobody (a merchant at peace) stays as it is, and its AI
may turn back later as it likes.  Measured, two machines (the guest a Confed officer's ship, so the host's pirate
starts on the host; `!privretargetpirates=1` makes sure): the guest's missile hit, "our PIR_DA turns on pilot 2's ship
(it was after ds:B924, the host): its pilot's shot", its AI's target word replaced; the pirate hit the guest's ship 0
times between the host's retarget (30 s) and the turn (39 s), 21 times after, each sent to the guest and taken there.
A taunt by the guest (comm, the pirate, "Live or die"): its anger 0 -> 257 on the guest, told, the host's game had it
("provoked by pilot 2") -- that pirate, spawned by a rig and without orders, held no target in its plan for the whole
time and was left as it was; an encounter's pirates on an attack run have one.

**Gunfire seen, within the game's memory** (the user: the guest "didn't see any gun fire at all in the arena from the
other ship"; then "I am worried we will overload the free slots with gun slots"; "the bolts should only be duplicated
when both players are in the same arena of sorts"; "a simpler rule could be that the weapons are only manifested on the
guest's computer if the one firing the laser had a ship that is on the guest's computer targeted.  I suspect the AI
always does that.  I suppose we should also fire for the actual player cannons"; "since players don't always target who
they mean").  Found: no copy ever fired.  A ship's guns fire in the game's weapons update, seg125:0158(weapons far,
scene, control record far): its AI branch (038A) makes the guns of the groups the record's +0x1E bits 3-6 name active
(each gun's +0x35) and, with bit 2, fires them through seg120:0004 (per gun: active, its group, its refire time, its
energy); an AI ship's think fills the record and calls the update, so a puppet (its think held) with the fire bit set
fired nothing (measured: a held pirate, 3 s, no bolt), and seg120:0004 called alone fired nothing (no gun active).  Now
a copy's shot is a call of the update for the puppet (its weapons record the ship's +0x4E, far; the scene ds:66C6; its
record the controller's +2) with the record's bits set for it -- fire and the groups the original fired -- and let go
after (measured: a held pirate, six calls, 36 bolts in the scene).  The shooter's game sends with each shot
(`PrivShot`) the groups its AI's record had (pirates fire 0x1C: groups 0 and 1; a pilot's ship sends none, and its copy
fires groups 0 and 1) and its target as the receivers know ships (`target_seat`, `target_id`: a pilot's ship, an
encounter ship by its owner and id; an AI's by its plan, a pilot's by its targeting system); a pilot's own guns are
sent too (id 0).  A receiver copies a shot when the shooter is within 3 km of its ship, and then: a pilot's always; an
AI ship's when its target is a ship shown here (ours, or one we show) -- its target not known, when its nose is within
20 degrees of a pilot's ship; and only while the copies' bolts alive in its scene are under 24 and its heap's last 2 s
stayed over 9,000.  A bolt is some 82-116 bytes of the near heap and lives a second or two.  Measured, two machines (the
host fighting its pirate, the guest 200 m off facing it): on the guest 71 copies fired, 9 left out; the copies' bolts
in its scene 5, 14, 20 (the host's ship's), 15 (the pirate's), never past 24; its heap's low 14,128 at the most bolts
(17,400 without); their hits on the pirate's puppet harmless there.

**Friendly fire, the host's choice** (the user: "I wonder if friendly fire off unless the guest and host had targeted
each other would be more friendly and easier to collaborate during a fight (could be an option similar to no damage from
friend collisions)").  The page's Friendly fire (Privateer, a room only, the host's counts as Collide's does; remembered
by the browser; `WCPRIVFF=targeted|always|never`, sent to the guests in the host's hello, `PrivHello.friendly_fire`):
a shot of our own ship's on a pilot's ship is sent to it (and hurts) only always, or -- the default -- when the two
pilots target each other.  The player's target is the targeting system's: a far object at the ship's +0x5A (its key
handler seg109:0A60 takes T, L, I), the ship targeted at its +0xC (its cheat key kills [+0xC]; measured: the host's
Tarsus there while the target display showed it); each pilot's state says whose ship it targets
(`PrivState.target_seat`).  The game targets the nearest ship by itself -- measured: two pilots launched together had
each other targeted at once, nobody pressing T -- so only a target taken with the T key counts (the one held within a
second of a T press, for as long as it stays on it).  Rams between pilots never hurt (above).  Measured, two machines,
the guest 350 m behind the host firing (the test aid `!privturn=stalkship`): before either pressed T, "our shot on pilot
1's ship does nothing (... not targeting each other)", the host whole; both pressed T at 40 s, each "on pilot N's ship",
the guest's missiles hit and were taken (the host's rear armour 250 to 202); with the host's "never", the guest's shots
did nothing even with both targeting.

**Found by the 8-pilot load test: the far heap broken by retargets (error 1407).**  With the batch above, the owner's
game reported error 1407 (seg143:0602: Origin's far heap check, seg143:054E, finding a block whose back link is not
the block before) and its flight ended, twice, seconds after its pirates were turned on pilots.  The retarget (the
drone's since 2026-11-08, `turn_on_pilot` now) scanned 1 KB from the controller's +2 and wrote every word equal to the
old target's ship or its model's segment -- but the controller's +2 is the control record, a 4-paragraph block (60
bytes of data: the stick, the fire bits, the pilot at +0x23; measured, its header FFFC), often the last block of its
heap region: the writes (measured at +740..+894) landed in the blocks after it and past the region's end marker,
headers' links among them.  Now the blocks after the record are walked as the heap's own walker does (the size at
seg:0000, negative in use; the next block's back link at +2 must be this one; free blocks stepped over; 1 KB at most),
and the target is a ship whose pointer and whose model's far pointer stand in one such block's data, written there
alone.  The controller and the pilot hold no pointer to the target, and the AI's data is not always after the record
(measured: the record the region's last block), so a ship often has no target found and is not turned: the enemy's
turn on a provoking pilot works only sometimes (the AI's own target field is still to be found in its think,
seg042:0944).  Also put back: the dead-ship handler's 0B6F block for a given-back puppet (a pilot angry at the player
taken out of the game's lists, seg026:0079 and seg013:0748; it was skipped as a kill's credit).  Measured after: the
8-pilot load test (8 Tarsus at Achilles' nav point, 3 pirates added, everyone firing) -- no error, death or restart on
any machine; the owner's lowest free 5,532 (as before), the guests' 7,738-8,660 (some 2-3 KB under before: the copied
gunfire), no guest gave anything back.  Test aid `!privplan` (the last spawned ship's AI blocks and the ships in them).

**No writes into the AI's memory** (the user, 2026-11-21: "This seems like a dangerous approach ... it's clearly caused
several bugs and who knows how many others"; "can we go with a simpler more minimal design ... within the games rules";
"I think we should still send it over the wire as proto"; "maybe rate limit it.  Perhaps only the first hit?").  Taken
back: the turn of an enemy on a provoking pilot and the Steltek drone's re-aim at the lowest pilot, both by writing the
AI's target (with the test aids that did it).  Every ship chooses its targets by the game's own rules, among all the
ships of its game, the other pilots' ships among them (as a merchant's, by the default face, pirates go for them:
measured, the host's pirate hit the guest's merchant-cast ship 25 times without any help).  What stays: a provocation
(our pilot's first hit or taunt of another's ship: its copy's pilot's anger rose here, read only) is told to the owner
(`PrivHit.provoke`), once a ship, and logged there ("pilot 2 angered our PIR_DA ... in its game").  The game's own
reaction to a hit is the player's alone (the anger code compares the shooter with ds:66B4), so a safe turn needs the
AI's own target field -- found in its think, seg042:0944, written only on a first provocation of a healthy ship by a
pilot whose ship is shown and whole: the next step.  (An AI ship firing at another AI ship, to see whether the game makes
it retaliate: not shown yet -- the trial's aim hit once in 22 s.)  Measured after: a two-machine fight, the guest's
missiles applied on the host, the pirate's hits on the guest taken there, the provocation logged on the host, no error.

**A provoked ship turned on the pilot, the game's own way** (the user: "lets do the safe target setting"; "only when a
guest specifically does something to a target and the target is in good health"; "Perhaps only the first hit?").  The
AI's think (seg042:0944) has its control record give it the pilot (near, the record's +0x23; the pilot's +0 its own
ship) and runs it (seg013:00AE); the pilot's +0x13 is its brain, whose +0x12 is the ship it is after -- copied each
think to ds:0E42 for the maneuvers (seg014:0B04) and chosen by the brain itself among the scene's ships (seg014:0538-
0760: candidates by distance and by its relation, seg014:1C83 -- to the player the pilot's own attitude, brain+0x6, which
the player's hits and taunts change; to any other ship the factions' table at ds:65B6 alone, 0 hostile, 2 neutral, 4 the
same faction).  It is always written through the game's setter of kept pointers, seg083:01AF(field, ship): the old
ship let go (seg083:0194), the new stored and the field registered in the game's list (ds:67FD: a count, a far array
of 200; full is its error 6281), cleared by the game when that ship is destroyed -- the bolts' shooters, the missiles',
the comm's speaker are kept the same way.  Measured: the player's hits on a merchant made its brain's +0x12 the
player's ship, the list's count 21 -> 22, and back to 0 / 21 when it gave up.  So a provocation (`PrivHit.provoke`, the
pilot's first hit or taunt of the ship there) is acted on at the owner's frame by calling that setter with the pilot's
ship (`turns_frame`), once a ship and pilot a flight, and only when our ship is an AI ship whose brain we can vouch for
(controller vtable 0x14A8, its pilot's +0 the ship), alive with every armour quadrant whole, the pilot's ship shown,
alive and within 5 km, the kept pointers under 180.  Per-ship anger exists for the player alone, so the brain chooses
again at its next think by the game's rules: a pirate stays on a pilot who is near and hostile to it by the factions,
and goes back to a nearer or angrier one (the host) otherwise; a ship friendly to the pilot's faction (militia to a
merchant's face) lets it go.  Measured, two machines: a guest's provocation from 2.5 km of the host's pirate (test aid
`!privprovoke`) -- "our PIR_DA turns on pilot 2's ship (... it was after PLAYER ...)", the brain's target the guest's
ship, then the host's again at its next look (the host beside it had shot it); the guest close, firing (copies of its
shots hitting the pirate in the host's game, harmless): the pirate's brain on the guest's ship from 42 s to the end,
its hits on the guest from 47 s (none before).  Test aid `!privbrains` (every AI ship's target and the kept pointers).

**A review of the batch before a showing (2026-11-21;** the user: "I'm very concerned that the crash was just papered
over and not fixed").  Read against the program: the scan that wrote the far heap is gone with every caller (no place
of the layer writes a block it found by searching; the writers left are fields at fixed offsets of objects checked by
their vtables), and the game's setter is what the commit says -- seg083:01AF(field, ship), near, the caller's stack
(its retf plain, as our call place needs): the old pointer let go (0194 -> 0121, the field's entry taken out of the
list), the new stored and the field's far address added (0073; the 201st is error 6281); a destroyed ship's fields
cleared by 00AD from seg030:0022.  The pilot has one kind of brain (seg013:00AE: none is the game's error 6021), so the
field is the same in every AI ship.  Three things of the copied gunfire put right:
- the shot's target sent with it was still `plan_target`, the walk of the far heap's blocks after the control record
  (read only, but up to a million reads for every gun an AI fired, and a target found only sometimes, so most AI shots
  fell to the aim cone): now the brain's target, the field the game keeps -- the user's rule as said;
- the weapons update launches a missile on the record's bit 1 (seg125:048C -> seg120:03E5, by lock state a held
  puppet does not keep), and the copy's write left that bit as it stood (a puppet let think for a comm line may leave
  it set): now cleared for the call, a copy fires guns alone;
- two more limits a copy leaves room in: the game's kept pointers (every bolt keeps its shooter there: no copy from
  140 of the 200) and our own queue of calls (16: no copy while 8 wait, so a hit to apply or a ship to make is never
  dropped behind a burst).
Measured after: the 8-pilot load test (3 pirates added) -- no error, death or restart on any machine, the owner's
lowest free 5,688, the guests' 7,844-8,514, five provoked pirates turned by the setter, the kept pointers at most 96
(the owner's), no call dropped for a full queue; two pilots and 5 pirates added -- no error, the kept pointers at most
108 (the owner's, its own ships' bolts).

**The bolt collector** (the user, 2026-11-21: "Could we have a garbage collector that cleared active gunshots randomly
if the limit exceeded 190? ... if we're already at 190 problems abound"; "favor other-machine bolts to remove and then
seconadrily non-player bolts ... but in the end the limit must be respected no matter whose bolt").  A bolt's update
(the laser's seg090:02EC, the other's seg091:0C27: the vtable's +0x24) keeps it while its age is within its type's life
and its alive byte (our pointer's +0x20) is set; the game clears that byte itself when a bolt hits (seg090:01D2) and
then takes it out and destroys it, its shooter let go of the kept pointers (seg090:035A).  So the collector only clears
the byte (`priv_bolts_retire`); nothing of ours frees or unlinks.  Each frame (`bolts_gc`), from 170 of the 200 kept
pointers on, enough bolts to come down to 160: other machines' copies first, then our game's other ships', our own
ship's last, at random within each; and the same when the near heap has under 4,000 free.  The marks are 170 and 4,000
(not the 190 first asked): they are read once a frame, and a frame of many guns adds entries before the next look (below).  Test settings: `WCPRIVBOLTGC=<n>|off` (the mark),
`WCPRIVCOPYSTRESS=1` (the copies' own caps lifted).  `scripts/privtests/bolts.sh`: PLAYERS (8) load the user's Oxford
save (a Centurion each), launch, are kept together and hold their guns, the copies' caps lifted.  Measured:
- the 8-pilot load test with the mark at 50: 3,700-5,900 bolts retired a machine, no error, the most kept 63;
- 8 Centurions, the collector off: no error, the most kept 159 -- the list is not what gives first: the near heap is
  (lows 4,300-5,300), and the memory governor then shows fewer pilots' ships (3 of 7), so fewer copies fire.  The
  200 were not reached by any fight tried; with the copies' real caps (24 bolts, none under 9,000 free) far less;
- 8 Centurions, the mark at 120: held at 126 at most on every machine, 445-667 bolts retired each (all copies);
- 8 Centurions and the 8-pilot load test as shipped (180): no error; the collector never ran in the load test.
- **the limit reached** (the user: "can you change refire rate and energy usage of laser for this test so we
  artificially get there"; "only have 4 centurions with rapid fire lasers just to test"): `WCPRIVGUNSTRESS=<n>` (the
  rig's GUNS=) divides every gun's refire delay (the gun's +0x2B; its guns a list at the weapons record's +0xE) and
  zeroes its energy (+0x33), and under WCPRIVCOPYSTRESS the memory governor gives no ship back.  4 Centurions, guns 6
  times as fast, the collector off: the host's game ended with error 6281 (the list 176 at the frame before, the heap
  622).  The same with the collector at 180 / 2,500: no error, but the heap down to 58 bytes; at 170 / 4,000, as
  shipped: no error, the list 175 at most, the heap's low 1,846, 1,623 bolts retired on the host (all copies).  None
  of the test settings is on unless its variable is set.
- **the soak** (the user: "The lowest free memory is kinda low.  Could our gc idea be leaving trash.  Should we run it
  20x as long and see"): `scripts/privtests/boltsoak.sh`, the rapid-fire stress for 22 minutes, the guns held in 20
  bursts of 40 s with 20 s of rest, the list and the heap read at each rest.  With the marks above and 8 bolts over the
  shortfall a run: no trash (the host back to 14 kept pointers and 17,276 free at every rest), but one guest's game
  ended at 10 minutes with error 1303 -- 800 bytes free, the largest block 134.  So the collector now reads the heap's
  largest free block too (under 1,500 counts as low) and, when low, retires enough bolts to come back to 6,500 free.
  Measured after, the same 22 minutes: no error on any of the 4 machines; the host at 14 kept pointers and 17,352
  free at each of its 20 rests; 24,600-29,100 bolts retired a machine (all copies); the worst second's free 1,182-
  1,910; the list 178 at most.  The 8-pilot load test as shipped: no error, the collector never ran (the owner's
  largest block never under 7,070).

## 19. The game's own frame rate (2026-11-22)

The user, on the high-resolution picture's plan: "we need the full framerate"; "I think it's 20 or 30 fps back then";
on what was found: "the joystick thing seems like a major waste--- probably setup incorrectly or something".

**The cap.**  The flight's loop (seg075:062B) starts a frame when more than 20 ticks of the game's 600 Hz clock
(ds:3636) have passed since the last: 21 ticks, 28.57 frames a second.  The frame's time (ds:277C ticks, ds:2768
seconds, both 24.8, at most 140 ticks) scales what moves, a gun's refire (seg125:00DE) and a bolt's life
(seg090:0300): a faster frame rate is no faster game.

**What kept it under the cap.**  Not the drawing (an eighth of a fight's frame): the game port.  Once a frame
seg108:0028 -> seg133:008E reads every input device, and the port's turn, seg133:0081, is the one reader that does
not first ask whether a stick is there (seg235:035C).  Without one the timeout stays at its uncalibrated 16,383
reads (seg235:037B; JOYA.DAT, which makes it 140, is loaded only for a stick found at the start: ovr397:0991): 704,000
of the 1,051,000 cycles a frame has at 30,000 a millisecond, every frame, whatever the controls.  It is the game's
and no setting's: GOG's own joystick section, joysticktype=none and a directory without JOYA.DAT all read 16,383
times (GOG's cycles=max hides it); with a stick, 140 times and 28.6 frames a second.

**The fix** (`empty_port_poll`): at seg133:0081, with no stick found at the start (ds:6A96 both 0), the call returns,
having made the copy of the reading that it makes (ds:6ABC, 6AC0; the reading itself, ds:6A8A, is the start's: an
empty port always reads the same).  With a stick nothing changes.  `WCPRIVEMPTYPORT=game` keeps the game's poll.

Measured at 30,000 cycles, natively:

| | the game's poll | fixed |
|---|---|---|
| one pilot, empty space | 28.6 | 28.6 |
| one pilot, a fight | 21.7 | 28.57 |
| eight pilots (load.sh) | 17.2-19.8 | 25.6-27.9 |
| the port's state in DGROUP | | the same, byte for byte |

Frame rate costs no near heap (the user: "it's just sleeping less-that should not consume additional memory"): one
fight at 13.9, 17.6, 22.3 and 28.6 frames a second averaged 14,077-14,586 bytes free with no trend; what a frame
allocates it frees.  load.sh both ways: no errors, the guests' lows 6,730-8,374 before and 8,210-8,694 after, the
host's 6,530 and 5,454.

## 20. The high-resolution picture (2026-11-22, off by default)

The user: "our audience wants an uprezzed version of privateer, selectable from the play menu (will be on by default
when it's ready but now a checkbox that's off by default) ... for things like 2d art this means drawing 2x2 pixels
instead of 1x1, but for the in-flight screen it gives an opportunity to draw larger versions of the sprites"; "the
ratio should be identical to what the original players of privateer saw"; "lets keep them blocky but use the highest
res art available in the game"; "backtick would be very useful"; "lasers at very high resolution ... Target brackets
and hud lines should also be uprezzed to only draw single pixel wide"; "the actual resolution of the cockpits can
remain low", their edge smooth.

**How the game draws a flight.**  Every pixel of the scene and the instruments goes into one buffer in memory through
the graphics library's routines; seg205:0002 then copies a list of spans (x, y, count; FFFF ends), the cockpit's
window and screens, to the screen; the cockpit was drawn to the screen once.  The game runs paged: linear A0000 is
memory of its own (that buffer is there, BBBE:0004), and the screen is linear 100000 (FFFF:0010), physical A0000.

| entry | what | arguments after the bitmap record |
|---|---|---|
| seg180:0008 | fill | x0, y0, x1, y1, colour |
| seg181:000A | clear to the clip | colour |
| seg182:0008 | copy (the source's record first) | the destination's record |
| seg183:0004 | ellipse | x, y, rx, ry, colour |
| seg184:0002 | line | x0, y0, x1, y1, colour |
| seg185:000A | pixel | x, y, colour |
| seg186:0000 | rectangle | x0, y0, x1, y1, colour |
| seg187:0004 | shape in one colour | x, y, shapes far, index, colour |
| seg188:0008 | shape | x, y, shapes far, index |
| seg189:0008 | shape scaled, translucent | x, y, shapes far, index, sx, sy, tables far, first colour, count |
| seg193:0006 | shape scaled | x, y, shapes far, index, sx, sy (8.8, negative mirrors) |
| seg202:0ECE | shape turned and scaled | x, y, shapes far, index, degrees, scale 8.8, flags (10h mirror x, 20h y) |
| seg116:000E | polygon through a table | corners, corners far (22 bytes each), table far |

A bitmap record is segment, row table, clip x0 y0 x1 y1.  A shape is mirrored first, then turned clockwise, in the
game's own pixels (no correction for their shape).

**The picture** (src/cpu/wcnet_priv_hires.cpp).  Each of those calls on the flight's buffer is drawn again at its
entry, from its arguments, at 640 x 480: the ships from their whole art sampled at the fine pixels (blocky close up),
the lasers' triangles from the corners the game computes to 1/256 of a pixel and then rounds (seg091:05D7, 0621),
lines, rectangles and the crosshair a fine pixel wide, the rest in blocks (a game pixel is 2 wide and 2 or 3 tall).
At the span copy that picture is frozen; at the VGA's frame it shows wherever the screen still holds what was copied,
with the spans' edge smoothed; everything else is the game's own pixel.  The game's memory is never written.  The
hooks run inside the CPU's core (wc_net_hook_exit in the browser): they read only by checked reads and never wait.

`WCPRIVHIRES=1` at the launch, `wc_web_priv_hires` while flying (the page's "High resolution (2x)"), the ` key flips.

Still to do before it is the default: the landing and takeoff scenes' bases from the flight's larger art
(LTOBASES.PAK's small copies have it 1.6-2.5 times as large in APPEARNC for six of ten), the radar's rings as lines,
the other cockpits and views gone through by eye.

## 21. Fights at the full frame rate: native twins, the wait idled (2026-11-22)

The user: "we need the full framerate--this may require some routines that are hooked by dosbox just similarly to
retrovega has it"; "lets use that oxford savegame as a good stress test for a fight--I'm worried that's where we get
serious lag and need to optimize drawing routines".

**The Oxford stress** (the user's save, a Centurion at Oxford; scratch rigs: launch, Oxford's own traffic, 0-8 more
pirates, guns held, a missile every 3 s): at the page's 30,000 cycles a fight of five more fell to 19-21 frames a
second.  The profile of it (the emulated cycles by routine, 18 s): drawing a sixth; over half the collision tests' and
the AI's vector maths -- seg039:00D4 a pair's set-up, seg115:02DD a length (seg115:0133 / 0217 to bring a vector into
range, seg095:0247 its root, seg128:02F6 a 20-step bit-by-bit square root: 13% alone), seg095:02DC an absolute value,
seg128:0296 / 0390 / 0450, seg115:0962.  And in quiet space two thirds of the emulated CPU spun in the frame wait
(seg075:062B).

**The twins** (src/cpu/wcnet_priv_native.cpp): twelve routines -- those ten and the two plain copies, seg181:000A (a
bitmap's clip filled) and seg205:0002 (the spans to the screen).  At a routine's entry the twin works out everything
it would leave: what it writes, all its registers (the high halves too), and its own stack below the stack pointer --
its pushes, locals, return addresses and its callees' frames, because the game reads such stack later (poisoning that
stack after each call in a scratch build made a run diverge within 20 s; poisoning the flags did not: no caller reads
them, and they are not made).  It writes all that and returns as the routine's retf.  A routine whose code is not the
file's (DOS's relocations taken off), memory not mapped, operands overlapping, the game's own error, a loop that would
not end, more than the twin's caps: the game's own code runs.  WCPRIVTWINS=0 the game's code, =check the game's code
with the twin's answer compared at each return.  The wait: while its clock has not moved on, the slice's remaining
cycles are skipped, as a hlt's are (WCPRIVIDLE=0: the spin).

**How they were checked.**  Each twin against the executable's own bytes run by unicorn on random inputs and random
stack (scratch twindiff.py, in design/privateer-hires/research): every register, every byte written, the 128 bytes of
stack below the entry -- millions of calls, none differing; two bugs planted on purpose were each caught at once.  In
the game, the check: 15 million calls in Oxford fights compared, none differing.

**Measured**, natively at 30,000 cycles:

| | the game's code | the twins |
|---|---|---|
| Oxford, 5 pirates more, solo | 26.5 (19 the worst second) | 28.6 (28) |
| Oxford, 8 more, solo | 25.7 (18) | 28.6 (27) |
| Oxford, 7 more, a room of two (the host) | 26.9 (13) | 28.6 (28) |
| eight pilots, load.sh | 25.6-27.9 | 27.8-28.6, no errors |

**The near heap at the full rate.**  The user doubted that frames could cost memory ("it's just sleeping less").  An
ordinary fight's use does not change with the rate (section 19).  But with seven pirates more than the game itself
puts there, solo, the game's own code at the full rate -- given 60,000 cycles, no twins -- ran out (error 1303 at 51 s;
the same fight at 26 frames a second did not): the AI's shots are per frame, and a heap of 17 KB holds only so many
bolts.  In a room the bolt collector (section 18) holds it: the room of two above, its host's heap never under 3,100.
Solo is the game as it is (section on rooms of one): nothing more is done there, and the game alone never fields that
many.

## 22. Privateer as it was, and the picture's code unloaded (2026-11-22)

The user: "is there a way on the website to run privateer without high res code loaded at all in case it causes an
issue?"

* Until the high-resolution picture is first wanted (the page's box, WCPRIVHIRES=1, or the ` key) none of its code
  runs: its places are not watched at all.  The box ticked during a flight or the key pressed watches them from the
  next instruction, and the picture follows a frame later.
* `?privclassic=1` on the page (WCPRIVCLASSIC=1 natively): Privateer as it was before this day's work -- no
  high-resolution picture (the box greyed out, the ` key inert), no native twins, the frame wait spun and the empty
  game port polled as the game does.  The 4:3 shape stays (the page's).  Measured: an Oxford-type fight at 24 frames a
  second (the twins' 28.6), the picture 640 x 400, none of the picture's places watched.

## 23. The radar as line art, and every cockpit and view looked at (2026-11-22)

The plan's step 5 and 6.  The radar's art is one shape a frame, drawn by segment 0x13AE (the radar's: it draws that
shape, the blips as pixels and some lines, nothing else, in every probed frame).  In the high-resolution picture its
most common colour (the screen's black) stays in blocks and each other colour is line art: a pixel joined to its
same-coloured neighbours by one-pixel lines (the two-tone rings stay two rings; joining any colours made a mesh of
them, thinning the bitmap made combs), and a pixel in no chain of its own colour (the side gauge's two-tone bar, a
line's end) reaches halfway to each neighbour of another ink, so that bar is a line and not dots.

Looked at with the picture on and off (scratch rig views.sh: a new pilot buys the ship, outfits it, launches, two
pirates, guns fired, each view key, the nav map): the Tarsus, Orion, Centurion and Galaxy, F1-F4 (front, left, right,
rear), F7 (the missile camera's notice), F8 and F9 (outside cameras), the nav map.  The radar is the same art in all
four; the window's smoothed edge follows the Centurion's and Galaxy's curved canopies; the red flash of a hit (the
palette's colour 0) shows in the picture as in the game's (paired !shot / !frame every quarter second).

## 24. The landing and takeoff scenes' bases from the flight's art (2026-11-22)

The plan's step 7.  The scenes (MIDGAMES\LANDINGS.IFF, TAKEOFFS.IFF) do not go through the flight's frame: they draw
with the plain shape routine (seg188), unscaled, into a buffer of their own, and copy it to the screen with their own
code (a trace of a takeoff: 98% of the time in a wait loop, the drawing in seg188, no seg205 copy).  So in the
high-resolution picture they were the game's picture made twice as large.  Their base is one of LTOBASES.PAK's ten
small pictures; six are small copies of a base's flight art (APPEARNC\*.IFF, its SHAP: four quarters laid over each
other), 1.6 to 2.75 times as large there:

| LTOBASES | small | flight art |
|---|---|---|
| 1 | 132 x 79 | REFINE 219 x 132 |
| 2, 4 | 83 x 67 | ROIDBASE 209 x 184 (Achilles) |
| 3 | 84 x 70 | AGRIC 182 x 170 (Helen) |
| 6 | 89 x 72 | DETROIT 184 x 150 |
| 7 | 141 x 81 | DERLCT 225 x 128 |

(0 and 8 are other planets than the flight art's, 5 and 9 other stations: theirs stay the game's.  The pairs were
matched by eye with design/privateer-hires/research/bases.py; each size is checked as the files are read, a pair not as
known left out with a line saying so.)  The first time a scene draws a shape of a small base's size (the high-resolution
picture wanted), PRIV.TRE is read through the drive that has it (the CD image, drive D: DOSBox's own file objects, not
the game's handles): LTOBASES.PAK and the five APPEARNC files ("the landing and takeoff bases: 6 of 6 from the flight's
art").  A shape the scene draws is matched by its decoded pixels; its place is noted, and at the compose, if 90 in 100
of that small base's own pixels are on the screen where it was drawn, each of those pixels shows the flight's art over
the same box (nearest, blocky: the user's ruling), and anything else (the ship passing in front, the screen's edge)
stays the game's.  Measured: Achilles' takeoff and landing (ROIDBASE, also cut off by the screen's edge) and Helen's
landing (AGRIC) show the flight's art in place, colours right, the Tarsus in front of it.

shape_of's run-length decoding is now decode_rows, a template over the bytes it reads (the game's memory, or a file's).


## 25. The mouse's pitch, and the page's "Invert mouse" (2026-11-22)

The user: "The mouse is inverted.  Is there a way to change it back. An option or toggle".  The game's mouse flying
(the options' MOUSE, Alt+O) turns toward the pointer left and right but pitches as a flight stick: seg088:02E5 writes
the control record's yaw from -(x - ds:3C5B) / 10 (negated at 0327) and its pitch from (y - ds:3C5D) / 3, not negated
(0373..03C7), and +0x100 pitch climbs (the keyboard's Down).  So the pointer above the middle, the mouse pushed up,
dives.  The game has no setting for it (no "invert" among its strings).  With button 2 held the mouse's x rolls
(03CF) and its y changes the speed (seg088:0909, pushed up faster): neither is touched.

Now the page's "Invert mouse" box (Privateer only, beside High resolution, remembered by the browser, unticked by
default; `WCPRIVMOUSEINVERT=1` at the launch, `wc_web_priv_mouse_invert` while flying): unticked, at seg088:0BBD (where
the modes' readings meet, section 4) the mouse mode's pitch is turned the other way up, the record's +6 only, so the
pointer above the middle climbs and the ship steers toward the pointer both ways (the ring, section 18, is where the
nose goes); ticked, the game's own.  The page's stick (a pad, the touch stick) writes its own pitch over it as before.
The first flight frame says the choice in the log ("the mouse pushed up raises the nose" or "the mouse inverted, as the
game has it").  The rig aid `!privmode=keyboard|mouse|joystick` writes the options' three flags as its buttons do.

Measured, a Tarsus out of Achilles, the options' MOUSE, the pointer at the middle then 40 of 200 above it for two
seconds, then 40 below (the forward row's z from DGROUP each second): unticked the nose went -6 -> +10 degrees, then
down to -51; ticked (the game's own) +0 -> -77.

## 26. Even pixels, round rings, and the space alone (2026-11-22)

The user, after the first evening of it: "the radar is very bumpy and not circular. Same with the targeting reticle.
And the pixel art in the cockpit is not exactly 2:1 and there's some strange aliasing around"; then "it looks worse
because of the 4:3 stretch. We should keep pixel ratios"; "Can we have a third toggle mode where reticle and cockpit
are not high rez but sprites in space are?"; "Target boxes are but aiming and all other things are not in that third
mode".

**Square pixels, 2 x 2.**  Sections 20 to 25 drew H at 640 x 480: 200 rows over 480 made each game row 2 or 3 rows tall
in turn, an uneven staircase on every edge, and the page's 4:3 (section 20's `stretch`) did the same to the game's own
picture.  Now H is 640 x 400, every game pixel a block of 2 x 2 (`kSy` = 2 where 2.4 was: the sprites' scaling, the
lasers, the bases' overlay, the window's edge; `fine_y` = 2y + 1), and the page shows Privateer at 16:10 as the other
games, full screen contained.  The art was drawn for a 4:3 monitor's tall pixels (round bodies 1.22 times as wide as
tall): it shows as in any DOSBox without aspect correction.  The registry's `stretch` stays for a game that wants it.

**Round rings.**  The radar's rings and the crosshair's circle are fitted: each colour's pixels split into 8-connected
pieces; in each, the upright ellipse about the piece's middle that most of its pixels lie on (height to width 0.6 to
1.6, the band of 1.2 pixels holding the most), then fitted to the pixels near it by least squares (A du^2 + B dv^2 +
C du + D dv = 1, four rounds, the pixels within a pixel and then 0.8 of the last); a ring when its radius is 2.5 pixels
at least and points along it have its pixels within a pixel 80 times in 100.  (The ring's pixels' box was taken at
first: the crosshair's ticks lie on its axes, and the box made its circle of 9 an ellipse of 9 by 10 that most of its
pixels missed -- gaps and a wobble: the user, "The targeting reticle is still wobbly back and forth instead of a
beautiful circle like the radar"; "we need a better reticle or it looks like a cheap uprez".  Fitted, it is a circle of
8.95 about the art's middle.)  It is drawn one fine pixel wide along that ellipse, in order round it (an
L's corner left out), only where the game's ring has a pixel within one: its gaps stay, and where two rings touch a
shared pixel of the other's colour carries it over.  What is left of the piece is looked at again (the radar's inner
ring, then its middle ring), and the rest is joined as before.  Worked out once a shape (its hash), 16 kept.

**Three levels.**  0 off; 2 all of it; 1 the space alone: the scaled and turned shapes (the ships, bases, planets),
the lasers and the target box (its lines come from one place, para 0x2903: found by logging each drawing routine's
callers in a fight) at 640 x 400, and everything else the game's own pixels -- the crosshair and radar as blocks, every
other line and ellipse the game's pixels stepped as blocks (line_blocks), the cockpit window's edge block for block (no
smoothing).  The page: "High resolution (2x)" and beside it "Hi-res cockpit" (ticked by default, greyed while the first
is unticked; remembered); `WCPRIVHIRES=1` (all) or `=space`, `wc_web_priv_hires(level)` while flying.  The ` key cycles
all -> the space alone -> off -> all.  The nav map is drawn outside the flight's buffer: the game's own at every level.

Looked at (scratch levels.sh: the Tarsus, two pirates, one targeted, guns firing; the picture at each level and the nav
map): at the space alone the crosshair and radar are the game's blocks, the target box a fine pixel wide, the Talon
from its art; at all, the rings round, the radar's middle and side rings too.

## 27. A base's wait for a key idled (2026-11-22)

The user: "the sound was kind scratchy in the base"; asked to choose, "less emulated CPU at bases only--- maybe we can
do some optimizations if we need to for the base drawing"; "lets evaluate if animations can keep up"; "I remember they
were fairly low framerate to begin with"; and of a bigger sound buffer, "delay isn't good for realtime fighting".

**Measured first.**  `WCMIXERSTATS=1` (the mixer's callbacks: dropouts, stretched, overflows, clipped samples, the
peak; a measuring aid) in a headless Chrome at Achilles' concourse: dropouts 0 to 2 a minute with the high-resolution
picture on, off and `?privclassic=1` alike, nothing clipped (the base's music peaking near 5,000) -- but the page's main
thread busy 96 to 100 in 100 of the time in all three (Chrome's TaskDuration over each 5 s), `?privclassic=1` falling
behind (0.95 to 0.98 of real time): no room for a slower machine's hiccup, which starves the sound.  The screen there
changes on the game's clock: 10 times a second in the concourse, 5 to 6 in the bar, the same at 30,000, 15,000, 10,000
and 6,000 cycles (a scratch counter of changes of the VGA's 64,000 bytes).  A flat cut of the cycles outside flight
made a launch 2 s longer (7.2 s from the click at 10,000 against 5.0 at 30,000: the flight's load), so not that.

**The loop.**  Counted by vector (a scratch counter in CPU_Interrupt): at a base the game calls DOS's "flush the
keyboard's buffer" (INT 21h AH=0Ch) 62,000 times a second, each one of its protected mode's trips to DOS through its
memory manager (as many VCPI calls, INT 67h DE0x): the whole of the emulated CPU goes round that loop.  The menus wait
the same way.  Now DOS's handler tells the layer of each call (src/dos/dos.cpp -> priv_dos_keyboard_poll, Privateer
only, outside a flight): one within 0.1 ms of the last (the loop going round, not a key read in the middle of work)
ends the millisecond's slice, as a hlt does.  The loop goes round about twice a millisecond; the drawing between its
calls, a load and the timer's music keep all their cycles.  `WCPRIVIDLE=0` (as for the flight's wait) and the game as
it was leave the loop as it is.

Measured natively at the concourse and the bar: 25,400 of the 30,000 cycles a millisecond skipped, and the screen's
changes the same with and without it, second by second (8, 2, 7.8, 3.4, ... 9.6, 10.2, 7.2, 5, 7.6, 5.6, 6.4).

## 28. The user's font and the target's ship in straight lines (2026-11-22/23)

The user: "Can you (with the highest res option) also uprez the font letters etc. the shapes should be the same but not
as thick. Meaning a 5 should still trace like a led clock"; tries were rejected ("lets back these out: they do not meet
the quality bar") -- the game's letters joined thin, the X11 6x10 and 5x8 and the Unbroken font in the game's boxes, a
straight-line ship -- and then: "I hand made a font--can we try this with the uprezzed wireframes--but we need to
ensure the wireframes are symmetric exactly".

**The letters.**  The text in flight is one routine's (seg221, para 0x6108, fixed code): each letter a silhouette of
the game's 3 x 4 font whose index is its character.  The user's font (scripts/privfont/: its metrics, atlas, README,
REVISION; 70 characters, twice the game's, one-pixel strokes, each letter in the game's own box and advance) is written
into src/cpu/wcnet_priv_font.h by scripts/privfont.py, each entry with the game's glyph it was drawn from (checked: all
70 the game's, pixel for pixel).  With the cockpit in high resolution (level 2) a letter is drawn from it at twice the
game's place (the capitals' top 2 (y - 3), y the silhouette's anchor, its baseline row), in the game's colour -- only
when the game draws exactly that glyph; any other stays the game's blocks.

**The ship.**  The target display draws the ship in parts, each a 52 x 52 shape in the same frame: the hull's left and
right halves (one-sided, 112 pixels each), its middle (a mirror image of itself), each shield's arcs.  The routine is
in an overlay whose segment the overlay manager chooses (E613 in one run, elsewhere in the next), so its calls are known
by their place: the return's offset (039A, 043B, 04C2, 054A) and the code before it (`push word [bp-1Ch]; lea
ax,[bp-68h]; push ax; call far 0008:`).  A part is drawn in straight lines: each colour's pixels a graph (a diagonal
link only where no corner of the colour joins the pair), chains between ends and junctions simplified (Douglas and
Peucker within 0.75 of a pixel: a staircase of any slope one segment, a corner kept), drawn one fine pixel wide between
the pixels' middles; a pixel in a square of four of its colour a solid part, the game's block.  Symmetry, exactly: a
part all on the right of the frame's middle is drawn as its mirror image's drawing, mirrored back pixel for pixel (so
the hull's halves are mirrors whatever the lines' rounding did); a part that is a mirror image of itself has its left
half copied onto its right wherever the art is the same as its mirror about a pixel, the copy taking the art's colour
there.  Pictures (line_art: over 30 in 100 of the pixels in squares of four) and shapes over 64 x 64 stay the game's.

Measured (scratch comm.sh, levels.sh: a targeted Talon): every pixel of the drawn diagram has a mirror partner of its
colour about the frame's middle (1,084 with its shields down, 1,468 with them up; the hull's 628), in runs where the
overlay sat at different segments.

**Every ship, and straight lines through junctions (2026-11-23).**  The user: "the talon's leading edge wings are
straight, but the system drew it as a subtle curve. Can they be straight. Can you render all the high rez wireframes
from the game and see how they turn out and if others need fixing?"  The Talon's wing edge is a 45-degree staircase
met half-way by an inner stroke: the chains broke at that junction and each half took its own slope (2:3, then 7:8).
Now chains of a colour that meet at a pixel and together simplify to no corner there are joined, the straightest pair
first, until none is left; a chain ending where a joined line passes through ends on that line (its nearest point
within 1.5 pixels), and junction pixels are no longer dotted.  A scratch rig (ship1.sh: each type spawned ahead and
targeted, the display at both levels; GLXYTYPE:MERCHANT, ORIOTYPE:TUG -- GALAXY1 and SCOUT, wrong appearances, made the
game's own errors 6142 and 6482 with the high resolution off too) drew all sixteen.  Found and fixed: the Kilrathi
ships' frames are 69 wide (the size limit is now 96); the halves are not each other's mirrors (the drone's left half
carries the middle column, the Gothri's right), so the parts are now gathered into one picture as the game lays them
(wire_gather) and drawn whole at the next other drawing (wire_flush), each part's pixels judged line art on its own
(a part that is not -- a band of colours, a picture -- the game's blocks there); a line along a middle column is drawn
in both its fine columns (the one way it mirrors itself).  Measured: every ship's drawing has each pixel's mirror of its
colour, except where the game's own art has none (the Broadsword's one pixel, the Tarsus's offset cockpit).

**The user's notes on every ship (2026-11-23).**  "Paradign should have every pod be rounded / Gothri outside wings
should be rounded / Stilettos back engines should be flat and the front bright wing stabilizers should be at high res
too / Tarsis front cockpit should connect through the 2 weapon pods. There shouldn't be gaps / ... dralthi outer hull
should be smooth too and rounded".  Made, first in a scratch prototype on the dumped pictures (wire2.py), then here:
- no part is judged a picture any more: the Stiletto's stabilisers, the Dralthi's, Gladius's and Kamekh's bands are
  lines now;
- a piece of four pixels or fewer touching a solid part (orthogonally) is solid too: the Stiletto's engines, a yellow
  block with an orange row at its end, are flat blocks;
- a pod is a ring: for each colour, a region of other pixels (4-connected) that touches no edge, of 200 pixels at
  most; its loop the colour's pixels about it (a solid part's not counted); refused if two corners of the loop's
  bounding box are its own (a box: the Tarsus's cells stay square) or if it runs along a part kept as blocks; an
  upright ellipse fitted (least squares about the box's middle) with radii of 1.5 to 14 pixels and every loop pixel
  within 0.8 of it is drawn instead, its pixels no chain's;
- a line's end (a pixel with no more than one link of its colour) reaches half-way to each touching pixel of another
  colour (a diagonal one only where no corner pixel of a third colour comes between): the Tarsus's cockpit bar meets
  its weapon pods.
Curves through the Gothri's and the Dralthi's hull corners were tried (a bulge test, Catmull-Rom through the points)
and taken out: "the gothri and dralthi were acceptable before and this edit made it way worse, please revert".  The
Talon's front (its canopy and engines, one part of 94 pixels) stays the game's blocks: "I rather liked the blocky front
of the talon", "a special exemption for talon is fine--everything else is ok".  A part is known by its frame and its
lit pixels, not their colours (those are its damage): kWireBlockParts holds that part's FNV-1a hash (C7EFE729, the
same in both Talons).  Measured with ship1.sh, all sixteen: no game errors; every drawing's pixels have their mirror of
the colour (but the Broadsword's 4 and the Tarsus's offset cockpit, as the game's art); the thirteen ships the user saw
and passed are pixel for pixel as they saw them.  (The rig's lock sometimes takes another object -- a cargo pod at
4.9 km -- instead of the spawned Paradigm: its own picture with the resolution off shows the same; it is retried.)

**The letters a row higher, and the cockpits' lettering that is art (2026-11-23).**  The user: "A couple more displays
in the cockpit need the new font like AUTO and E for fuel.... ideally KPS would be shifted up by one pixel so it doesn't
get below the bevel / not sure hwo the other ship cockpits look--they may have their own quirks"; then "Can you shift
the font I provided up by the equivalent pixels then. That way it's a data change", "It was an accident I made them
lower".  The font's metrics say `"shift_up": 1` (scripts/privfont/font-replacement-metrics.json); privfont.py writes it
as kPrivGlyphShiftUp and every letter is drawn that many 2x rows above the game's letter's top (in the Tarsus's KPS box
a black row above the letters and one below, where the game's letters, and the user's at the game's place, met its
bevel; SET one above and three below).

AUTO and the gauges' letters are no text: each is part of a picture of its own, drawn by root code -- AUTO by 130E,
the energy gauge (its E) by 131A, the Centurion's fuel gauge (its F) by 1356 -- and its letters are the art's own, each
cockpit's different: the Tarsus's AUTO 16 x 4 (a round A and O), the Centurion's 20 x 4 (square letters), the Orion's
21 x 5, the Galaxy's 25 x 5 between red dashes; the E 3 x 5 in the Tarsus, Orion and Galaxy, 4 x 5 in the Centurion,
two blues (its corners the darker).  kArtWords lists them: the code, the letters' exact pattern ('#' the letters, of
one or two colours; '.' the panel, one colour; no letter colour just round it), the text, and the user's glyph row for
each fine row.  Found in a picture from that code, the pattern's pixels are drawn as the panel and the user's glyphs
over them, side by side with the font's two-pixel gaps, centred on the art's letters, in the colour of the art's
letter's stem; not found (another picture, a cockpit not seen), the game's blocks.  In a five-row letter the user's
eight-row capitals are centred (the A's slant kept whole), but for the E, whose two stems are drawn a row longer each.
AUTO, E and F are drawn in the space-alone level as the game's (hires_cockpit).  Measured with a scratch rig
(cockpit.sh: the Tarsus of a new game, the user's Centurion from OXFORD01.SAV, an Orion and a Galaxy bought at
Achilles as privtests/ships.sh buys them): every AUTO, E and F in the user's letters, the panel's red marks and dashes
as they were, no game errors.  (The Orion's and Galaxy's own-ship display, overlay code, stays the game's blocks.)

**Two floods cut short (2026-11-23).**  The ring finder grew each region of other pixels only to 201 and the small
pieces only to five, leaving the rest of a big region (the background) or of a long line unmarked; later starts inside
them made pieces of their own, so chance holes got rings (the Broadsword's side pods, a ring round the Centurion's
canopy) and chance bits of a line became solid blocks (canopies of the Gothri, Stiletto, Centurion), and a change
anywhere in a picture moved them: leaving the Centurion's nose pixels out took the ring off its canopy.  Both now grow
the whole region or piece (at most the picture) before judging it.  Thirteen ships drew differently (thin lines where
the chance blocks were); the user, shown them side by side with the live ones: "Take the fixes".  The Python prototype
(scratch wire2.py) had the same cut.

**The Centurion's nose (2026-11-23).**  The user: "centurion should have a smoother curve towards the front part of the
cockpit not a harsh triangle", "centurion just needs to pull the nose triangle towards the center so it's more sloped",
"so the slope starts at the wings I believe", "maybe as two slopes--one to the lower horizontal bar, then another one
to the corner of the wing that touches the body".  kWireEdits: a part (by its mask hash, as kWireBlockParts) drawn with
runs of its pixels left out (the left side's, mirrored about the art's middle) and the table's own straight lines in
their place, each side's in the colour of its own pixel at a sample place (one left out: its damage shows), drawn
before the mirror's copy.  The Centurion's middle part (92E829FB, 82 pixels: the nose and its tip) loses each side's
column 25 from row 25 to 34 and the steps below it to the tip, and gets two slopes: from the wing's corner (25, 24) to
the lower bar's end (27, 40), and from there to the tip (29, 46).  Shown with and without (scratch cent-nose2.png), the
user: "Yes, ship it".  With the floods whole, the edit changes the nose and nothing else.
