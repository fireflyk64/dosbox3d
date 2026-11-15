# The TIE Fighter demo (1994)

The user, 2026-11-15:
* "Is there a tie fighter demo by Nissan that we could work into the tie fighter side of things?"
* Then: "Ok begin the tie fighter neon demo port. For now have a link to the download location and have it so the
  players click download and drag into the installer. See if it work once you find the new hooks for the tie demo".

## 1. The demo

* **What it is.** LucasArts' playable demo of early 1994: one mission, in a build from before the game's release. It was
  given away to advertise the Dodge Neon (not a Nissan), and on a magazine's floppies.
  * Its mission is T/F Alpha at the Rebel repair yard near Remitik, then the Rebels fleeing Hoth. The clock starts at
    42 minutes.
  * TIEMISS.EXE says "TIE Fighter Demo V1.0 12/04/93".
* **Where.** DOS Games Archive: `tiedemo.zip` (1,586,575 bytes), the zips of its two floppies, `tiedem1.zip` and
  `tiedem2.zip`. Its page is https://www.dosgamesarchive.com/file/star-wars-tie-fighter/tiedemo (the file itself:
  image.dosgamesarchive.com/games/tiedemo.zip).
* **Not hosted here.** Its readable files state no terms of distribution (unlike the Apogee shareware the page offers
  itself), so the page links the archive and the player brings the zip.
* **The programs.** `TIEDEMO.BAT` runs `demo` (DEMO.EXE: the opening), `tiemiss demo >foo` (TIEMISS.EXE: the mission,
  DEMO.TIE) and `demo final` (the ending).
  * TIEMISS.EXE is a 16-bit real-mode program (Microsoft C, 1990 run-time) with overlays in the EXE. It uses EMS for
    its music, its digitized sound and its cockpits, never XMS.
  * Not the 32-bit DOS/4GW program of the CD version (docs/tiefighter.md): none of that port's addresses or hooks apply.

## 2. The floppies, unpacked (web/pvm.js)

* `TIEDEMO.001` and `TIEDEMO.002` are one stream of 46 files, each compressed with PKWARE's Data Compression Library
  ("implode"), end to end, split across the two disks.
* The first disk's file ends with the installer's directory (INSTALIT: from `\x05[PVM]`):
  * a 55-byte record per file: its offset on its disk, its name, its compressed size, its CRC-32, its disk, its size,
    its folder;
  * then the folders: a length byte and the path, `[INSTALLATIONDIRECTORY]`, `...\RESOURCE`, `...\CP` (the bytes after
    a path's length are left-overs of a longer name: "CP" reads "CPSOURCE");
  * then the disks' names and volume labels (TIEDEM_1, TIEDEM_2).
* The first disk's data stops where the directory begins (1,260,520 bytes). A file that runs past it (BLAST.LFD) goes on
  at the start of the second disk's file.
* `explode()` is zlib's contrib/blast.c (Mark Adler's decoder of the PKWARE format) in JavaScript.
* Checked against the installer itself, run in our DOSBox:
  * the 33 files it installed before it asked for disk 2 are byte for byte the same, and so are the first 86,016
    bytes of BLAST.LFD;
  * every one of the 46 files comes out at its directory's CRC-32 (the unpacker checks them all).
* The installer's disk 2 prompt was not passed headless; the directory's CRCs stand in for it.
* `node scripts/tiedemo-unpack.mjs DISK1_DIR DISK2_DIR OUT_DIR` unpacks the floppies on the command line (the rigs'
  game folder).

## 3. The page

* **Step 1.** The "Free to play now" row has a "TIE Fighter demo" link to DOS Games Archive's page. Its line says to
  download `tiedemo.zip` and drop it on the box.
* **Unpacking.** `expandSplitZips` (web/gamefiles.js) reads `tiedem1.zip` and `tiedem2.zip` inside the dropped zip and
  unpacks `TIEDEMO.001`/`.002` with `unpackPvm`.
  * The floppies' own files are left out (the volumes, TIEDEMO.INF, DISK.ID, INSTALL.EXE, DETECT.EXE) but for READ.ME
    and FLIGHT.HLP.
  * The folders are the installer's: the game's, RESOURCE, CP. It takes about half a second.
* **Settings.** The registry's `tiedemo` (tag TFD, detected by TIEMISS.EXE, run as `tiedemo`): `tdPrepare` writes what
  the installer writes when there is none.
  * `LANDRU.CFG`: `adlib_w` (the AdLib's music, a Sound Blaster at 220 for the effects and the voices: ADLIB_W.IMS),
    `FAST`, `ems`, `diff`.
  * `OPTIONS.CFG`: the installer's FASTYES.CFG.
  * The machine: an S3, 16 MB, EMS, a Sound Blaster 16 at 220/7/1; 30,000 cycles.
* **Multiplayer** (`multiplayer: true`, rooms of up to 3): sections 4 and 5.
* `scripts/web-tiedemo.mjs PAGE_URL ZIP [seconds] [key script]`: one page, the zip dropped, a room of one flown (Esc
  at 8 s skips the opening), the canvas every 10 s. Ends "TIEDEMO PAGE OK".

## 4. The program in memory (src/cpu/wcnet_tiedemo.cpp)

The demo is an earlier build of the TIE Fighter the CD port knows (docs/tiefighter.md), compiled 16-bit.
* **Its records are the CD's.** The object record is OpenTIE's `FlightObject`, the CD's layout byte for byte: 88 bytes,
  116 of them. So the CD port's design and much of its code apply.
* **Its functions follow OpenTIE's C.** The frame calls them in the CD's order, and a module's functions come in its
  source file's order (Microsoft C emits them so). That is how each was named.

**Finding it.** DOS loads TIEMISS.EXE for TIEDEMO.BAT, so the layer knows its load segment from the start
(`tdm_program_loaded`, beside Privateer's and Raptor's in wcnet_game.cpp).
* The build is checked by "TIE Fighter Demo V1.0 12/04/93" at image offset 0x30A70.
* The places are image paragraphs from the load segment (01A2 under our DOS, PSP 0192).
* DGROUP is image paragraph 0x421F (DS 43C1).
* Image paragraph 3727 is the overlay area (zeros in the file, swapped in flight: the HUD's code): nothing is hooked
  there.

**The frame**, `tie_doframe`, 0090:06CE (the CD's in OpenTIE's tie.c):

| demo | the CD's function |
|---|---|
| 0090:06FA | the wait until 4 ticks have passed (DS:C464; `frameticks` DS:CE44, `framerate` DS:CE48) |
| 1411:000E | user_userinterface |
| 0090:146E | tie_updatetime (DS:614D `mission.end_flag`: the frame does nothing once it is set) |
| 0FC2:0E04 | create_updatefgstatus |
| 091E:018C | pai_updateplaneai (28 craft; a craft's planner runs when its CraftData +0x2C is <= 0) |
| 1338:0006 | laser_weaponsfire |
| 089D:0002 | dynamix_planedynamics |
| 0090:0836 | tie_updatescreen (the picture) |
| 0FC2:2726 | create_checkdebris |
| 05B3:0000 | collide_collisions |
| 054A:0000 | move_moveobjects |
| 1768:058E | anim_updateanimation |
| 125A:0006 | score_checkobjective |
| 03B1:0640 | msg_messageupdate |

**The other functions used.** Microsoft C's far cdecl: the arguments on the stack, the caller pops them.
* `create_createmission`, 0FC2:000E: called from the flight program's start (0090:0155). It opens the mission file,
  reads its header (DS:C280), its flight groups (at the mission segment's 0xD0, 0x124 each; the segment in DS:5890),
  messages and goals. Its first random number is at 0FC2:0136, then it makes the craft.
* `laser_createprojectile(shooter, hardpoint, type)`, 1338:0B16 (genus 5 a player's bolt, 6 another's).
* `collide_laserhitcraft(bolt, target, offset)`, 05B3:1B18.
* `collide_damagecraft(target, component, attacker, side)`, 05B3:1CF8: an attacker of -1 is a collision.
* `collide_updatekills(shooter, victim, 1)`, 05B3:2CA8.
* `collide_makeobjectexplosion(obj, species)`, 05B3:25D6. A dying craft becomes species 0x40 or 0x41: this build has
  one explosion path, the CD has 127 to 130.

**The data** (DS offsets):
* The objects at DS:67CA, 116 of 0x58 bytes: +2 category, +3 genus, +4 ship_idx (0 a free slot), +6 x y z (int32),
  +0x12 the frame before's, +0x1E heading, pitch, roll, +0x26 current_speed, +0x2A collision radius, +0x2C death
  timer, +0x2E age, +0x30 self_idx (a bolt's shooter), +0x33 side, +0x35 flight group, +0x3A move vector, +0x42
  forward, +0x48 side, +0x4E up, +0x54 the craft data (near; a bolt's: its warhead record, DS:BF9C + 4 per slot past
  28).
* Craft 0 to 27, projectiles from 28 (`NUM_CRAFTS` 0x1C).
* `pstate` at DS:CD78: +0 the player's object, +2 its craft, +4 its number (DS:CD7C), +0xC its target (DS:CD84).
* `math2_randomseed` DS:5372 (0x2357 at first), the second generator's state DS:1A70.
* The mission's status DS:614C and end flag DS:614D: set, the flight ends at once (TIEMISS.EXE exits).
* A craft's data (CraftData, 0x18C bytes, the CD's shorter): +0x2C the planner's countdown (the CD's +0x2A), +0x40 its
  attacker, +0x5C its flight flag (the CD's +0x60), +0x9E its hull's damage (the CD's +0xA2) against +0xA2 (the CD's
  +0xA6: 1,000 a T/F), +0xA8 its systems, +0xC2 and +0xC4 its shields.
* A flight group's record: +0x33 its craft, +0x42 the player's craft in it (1-based). DEMO.TIE's Alpha has 3 craft, the
  player its second: object 1, Alpha's first and third objects 0 and 2.

**The places watched** (`tdm_watch_list`; each IP's CS checked):
* create_createmission's entry: the launch.
* 0FC2:0136: the seat and the seeds.
* tie_doframe's entry: the frame's top.
* move_moveobjects's entry: the others' craft written in; our calls made.
* laser_createprojectile, collide_laserhitcraft, collide_damagecraft: their entries.
* PSP:005C: the launch's idle.

**Calls into the program** (`tdm_queue_call`, the CD's `tie_queue_call` in 16 bits).
* At move_moveobjects's entry the CPU's state is kept. The arguments are pushed, then the place's own CS:IP as the far
  return address, and CS:IP is set at the function.
* Its `retf` comes back to the place (SP checked). The place's hook gives the state back, the lazy flags too, and makes
  the next call or lets the frame go on.
* `tdm_skip_call` returns from a function at its entry (the far return address popped, AX set).
* No byte of the game is written; the launch's idle is a `jmp $` in the PSP's first FCB, which the program never uses.

**Research aids** (the environment's):
* `WCNET_TDM_PROF=1`: where the CPU is and the DS, sampled, every 5 s.
* `WCNET_TDM_WATCH=<linear>:<bytes>`: every write to those bytes and the instruction after it.
* The key script's `!tdmrec=<object>` (a record logged), `!tdmr=<DS offset>[/...]` (words logged), `!tdmw=<DS
  offset>:<byte>` (a byte written), `!tdmglue=dx:dy:dz` (object 0 held beside the player).

## 5. Flown together (src/cpu/wcnet_tiedemo_session.cpp): the CD port's session, ported

The user, 2026-11-15: "Please fix all the things for tie demo so the games sync. Please borrow as much technique as
possible and ideally code from the tie fighter cd version for similar gameplay".

The session is the CD port's (wcnet_tie_session.cpp, docs/tiefighter.md sections 5 to 7). Its flight code is carried
over with the demo's addresses and 16-bit calls (`fill_craft`, `write_puppet`, `decide_owners`, `show_hit`, the
shots, hits and kills, the room's end), with its own messages: TieFrame, TieCraft, TieShot, TieHit, TieEvent,
TieOwners. Only the room's hello and the launch are the demo's own (TdmHello, TdmState).

* **The room.** The host is seat 0; rooms of up to 3 (T/F Alpha's craft).
* **The launch.** Each machine's program waits at create_createmission's entry, after the opening, until every pilot
  of the room is there (a guest still in the opening after two minutes is left out). The host then says go with its
  random seeds.
* **The seat.** At 0FC2:0136, the mission read, a guest writes the host's seeds and its own craft into the player's
  flight group: seat 1 the group's first craft the host does not fly, seat 2 the next (the CD's player-craft number).
  The mission's own setup then makes the same craft in the same object slots on every machine. The host flies object
  1, the first guest object 0, a second object 2, and they start together.
* **Who flies a craft** (the CD's step 5, the user's rule for the CD version).
  * The host owns most. A guest gets a craft within the lasers' reach of it (2.5 km) and within 45 degrees of its
    nose, or its target within 4 km, unless the host targets it. Each pilot owns its own craft.
  * The owner's game flies it. The others write its reports into their game at move_moveobjects's entry and hold
    their AI off it (the planner's countdown, CraftData +0x2C, kept full).
  * A craft beyond 5.5 km of our pilot is flown by our AI between its owner's sparse reports, pulled to them smoothly.
* **Rates.** Our own craft every frame. The craft we fly for the others: within 2.5 km of another pilot every frame,
  within 10 km every 10th frame, the rest one at a time every 30 frames.
* **Shots.** A shot of a craft we fly, near another pilot, goes with the next frame. The others make it from that
  craft with the game's own `laser_createprojectile`. A craft another machine flies does not shoot here.
* **Hits.** Decided by the shooter's machine (its `collide_laserhitcraft`).
  * A hit on another machine's craft is sent and applied by the owner with the game's own call and a stand-in bolt.
  * This game damages only the craft it flies (`collide_damagecraft`'s entry), and shows its own hits on the others'
    craft at once; the owner's figures follow.
* **Deaths.** An owner's craft dying is sent; the others kill their copy with the game's own damage (a collision's).
  The kill is credited to the pilot whose hit began it (`collide_updatekills`). A craft gone without dying is gone
  everywhere.
* **The room's end** (the user's rule for the CD version): a pilot down, or the host's flight over, ends everybody's
  flight 3 s later, through the mission's end flag.
* **Left out of the CD's.** The front end (the demo has none), the mission picker, the pairings, the radio voices, and
  the inspection and reinforcement events (not in this mission).

## 6. Measured (2026-11-15)

* **The first step's** (each machine's enemies its own, the others as wingmen):
  * natively, two machines at 59 frames a second, the other's craft at the very place its machine had it;
  * two browser pages: TDM PAGE OK.
* **Synced, two native machines over TCP** (`scripts/tdmnet-duo.sh`, 30,000 cycles):
  * **The launch and the seats.** Both held at the mission's start, then launched together; the guest with the host's
    seeds, flying craft 1 of Alpha (object 0), the host object 1, the same slots on both.
  * **The craft.** Each machine has the other's own craft where its owner does (8240, 31,548, 90,092 on the guest;
    8240, 31,486, 90,118 on the host, 17 ms apart).
    * The enemies the same on both. The guest took the craft near its nose (an X-wing, the corvette) and gave them
      back.
    * An X-wing that died on one machine was killed on the other.
  * **The guns, 100 s, both firing.**
    * Each machine made the other's shots (64 and 63).
    * The guest's 5 hits on craft the host flew were applied on the host (stand-in bolts, the game's own call).
    * The corvette, flown by the guest's machine, hit an X-wing the host flew six times: its hull's damage 400, 800 ...
      2,000 of 2,100 on the host, and the host's screen said "X-WING RED 2 HAS BEEN DESTROYED".
  * **A pilot down** (the guest's craft's death started with `!tdmw=67F6:3c`). "You are down" on the guest, "GUEST is
    down" on the host. The host blew up its copy, and both flights ended 3 s later.
  * **Frame rates.** 17 to 21 ms a frame (about 50 to 58 a second) with the session.
* **Synced, two browser pages through a local lobby** (`scripts/web-tdm.sh`, both firing): TDM PAGE OK.
  * The launch together, the guest in craft 1 with the host's seeds.
  * Each page flying the other's craft from its reports for the whole 51 s.
  * Each page made the other pilot's 64 shots.
  * 55 and 54 frames a second.

## 7. Open

* **A kill credited across machines** has not been seen yet: it needs a pilot's own shot to finish a craft another
  machine flies.
* **A guest who arrives once the host flies** waits for the host's next launch: the demo has one mission, and the host
  launches again only after its flight ends.
* **The flash of a hit applied from another machine** is drawn where the stand-in bolt is (the shooter's bolt's
  place): the CD port's impact offset globals are not known in this build.
