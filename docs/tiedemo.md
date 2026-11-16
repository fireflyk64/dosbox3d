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
* **Where** (the user, 2026-11-15: point it at archive.org instead of the ad-laden page, "the download link would be
  ideal so they don't get distracted and play without our multiplayer patch", then "can we switch to using
  https://archive.org/cors/... since that can make it so we don't need to confuse users ... Then they can just play it
  directly. We should also save it in that users cache"):
  * **The Internet Archive** (https://archive.org/details/StarWarsTIEFighter_1020): `TIE_Fighter_Preview__Dodge_Neon.zip`
    (1,734,898 bytes, md5 d736eb5a...), the first release's two floppies as folders, `disk1/` and `disk2/` (dated
    4 December 1993; 48 files). Its CORS address, https://archive.org/cors/StarWarsTIEFighter_1020/TIE_Fighter_Preview__Dodge_Neon.zip,
    answers any page's origin (`access-control-allow-origin` echoed): the page fetches it itself (section 3).
  * DOS Games Archive's `tiedemo.zip` (1,586,575 bytes) is a later repack (January 1994: 46 files, the floppies as
    zips of their own, `tiedem1.zip`, `tiedem2.zip`) without the showroom. Dropped, it works as before.
  * The game's files are the same in both, TIEMISS.EXE byte for byte; the first release adds NEON.EXE and NEON.LIB
    (the Dodge Neon's showroom) and its own TIEDEMO.BAT and INSTALL.EXE.
* **Not hosted here.** Its readable files state no terms of distribution (unlike the Apogee shareware the page offers
  itself), so the page fetches it from the Internet Archive, once, for each browser.
* **The programs.** `TIEDEMO.BAT` runs `demo` (DEMO.EXE: the opening), `tiemiss demo >foo` (TIEMISS.EXE: the mission,
  DEMO.TIE) and `demo final` (the ending).
  * The first release's runs `neon` first (NEON.EXE, the Dodge Neon's showroom: "Cars are like friends", the car's
    features as buttons, "Go to game" and "Exit to DOS"); its exit code is the key: Esc (27) quits to DOS, anything
    else goes on to the game; after the ending it goes back to the showroom.
  * **Never to the DOS prompt** (the user, 2026-11-15: "I tried tie demo...and hit esc or something and quit to dos
    prompt...which wasnt helpful", then, of Esc pressed to leave the browser's full screen: "we definitely need to trap
    that esc"). The showroom's exit code 27 (Esc, or "Exit to DOS") is changed to 'G' as the program ends
    (wcnet_tiedemo.cpp `tdm_program_ended`, from game.cpp's program end): the batch file goes on to the game. In
    TIEMISS.EXE Esc ends the program at once, mid-flight (no question asked): the layer keeps the key from it
    (`tdm_key`, the page's and the native key filter), with a line in the log at most every 10 s.
  * The showroom's introduction ("Say hello to Neon", the car driving in, the buttons one by one) took some 9 s
    during which it heard no key or click. The user: "there may be a time wait there--can we jump the clock ahead so
    people can click whenever?" It is timed by one routine (NEON.EXE is packed: found running, at load+0BB8:001B),
    `wait(n)`, which polls the timer chip until n units of 6 x 256 of its counts (1.3 ms each) have passed; for the
    showroom's first 15 s the layer returns from it at once (wcnet_tiedemo.cpp `at_neon_wait`, its bytes checked
    first): the menu is up within a second and takes a click or a key at once. (A trap found on the way: TIEMISS.EXE
    is loaded into the PSP the showroom had; any other program's load now ends the showroom's watch.)
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

* **Step 1.** The "Free to play now" row has a "TIE Fighter demo" button beside Raptor and Terminal Velocity (web/wc.js
  `DEMOS`): the first click fetches the Internet Archive's zip from its CORS address and reads it as a dropped file;
  the installed demo is kept in the browser (IndexedDB, as every game dropped), and the button uses that copy from
  then on: nothing is fetched again. Dropping either zip on the box works too.
* **Unpacking.** `expandSplitZips` (web/gamefiles.js) reads `tiedem1.zip` and `tiedem2.zip` inside a dropped zip (DOS
  Games Archive's) and unpacks `TIEDEMO.001`/`.002` with `unpackPvm`, wherever they are (the Internet Archive's:
  `disk1/`, `disk2/`).
  * The floppies' own files are left out (the volumes, TIEDEMO.INF, DISK.ID, INSTALL.EXE, DETECT.EXE: in either disk's
    folder) but for READ.ME and FLIGHT.HLP.
  * The folders are the installer's: the game's, RESOURCE, CP. It takes about half a second.
* **Settings.** The registry's `tiedemo` (tag TFD, detected by TIEMISS.EXE, run as `tiedemo`): `tdPrepare` writes what
  the installer writes when there is none.
  * `LANDRU.CFG`: `adlib_w` (the AdLib's music, a Sound Blaster at 220 for the effects and the voices: ADLIB_W.IMS),
    `FAST`, `ems`, `diff`.
  * `OPTIONS.CFG`: the installer's FASTYES.CFG.
  * The machine: an S3, 16 MB, EMS, a Sound Blaster 16 at 220/7/1; 30,000 cycles.
* **Multiplayer** (`multiplayer: true`, rooms of up to 3): sections 4 and 5.
* `scripts/web-tiedemo.mjs PAGE_URL ZIP|fetch [seconds] [key script]`: one page; `fetch` clicks the button (once
  fetched, then after a reload the kept copy, nothing fetched), else the zip is dropped; a room of one flown (the
  showroom's G at 3 s, Esc at 10 s skips the opening; 8 s for DOS Games Archive's), the canvas every 10 s. Ends
  "TIEDEMO PAGE OK". scripts/web-tdm.sh and tdmnet-duo.sh take the same opening (`OPENING`), web-tdm.sh's `ZIP=fetch`
  the button.

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
* paiman_boardmaneuver's entry (0BBC:17FE; its first 12 bytes checked once): a boarding craft's dock and transfer.
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
  offset>:<byte>` (a byte written), `!tdmglue=dx:dy:dz` (object 0 held beside the player), `!tdmpose` (our craft's
  heading, pitch and roll, its nose, speed and target, and the shots made: laser_createprojectile's calls, ours apart).

## 5. Flown together (src/cpu/wcnet_tiedemo_session.cpp): the CD port's session, ported

The user, 2026-11-15: "Please fix all the things for tie demo so the games sync. Please borrow as much technique as
possible and ideally code from the tie fighter cd version for similar gameplay".

The session is the CD port's (wcnet_tie_session.cpp, docs/tiefighter.md sections 5 to 7). Its flight code is carried
over with the demo's addresses and 16-bit calls (`fill_craft`, `write_puppet`, `decide_owners`, `show_hit`, the
shots, hits and kills, the room's end), with its own messages: TieFrame, TieCraft, TieShot, TieHit, TieEvent,
TieOwners. Only the room's hello and the launch are the demo's own (TdmHello, TdmState).

* **The room.** The host is seat 0; rooms of up to 3 (T/F Alpha's craft). The session lives from the page's start (the
  page says WCGAME=tiedemo; natively from the showroom or TIEMISS.EXE's load) to the emulator's end, not with
  TIEMISS.EXE: a program ended and run again (the showroom between flights, or by hand from the prompt) stays in the
  room.
* **The launch.** Each machine's program waits at create_createmission's entry, after the opening, until every pilot
  of the room is there (a guest still in the opening after two minutes is left out). The host then says go with its
  random seeds.
  * A guest who comes to its launch while the host flies waits for the host's next launch (the end of the host's
    flight: the campaign's next mission, or the same one again). The guest's picture says "Waiting for HOST's next
    launch (Esc: fly alone)"; the host's log that the guest waits. Esc there flies that one mission alone; the room's
    next launch takes the guest in again.
  * Over the picture only these waits ("Waiting for the others to launch", "Waiting for HOST to launch"); the room's
    other notices and the campaign's (a mission's brief, its end, the next) go to the log alone (the user,
    2026-11-15: "We need to minimize the added output text using the overlay in the game"). The radio lines are the
    game's own messages.
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
* **Which craft a message names: its identity, not its slot** (2026-11-15, the user: "I was chasing Gold 5 and shooting
  it. It just kept running forward and wouldn't be damaged... I must have hit it a hundred times or two", and "I would
  get randomly blast animations on my ship here and there--they weren't actually hurting me but they didn't originate
  from anywhere--seems like some kind of desync").
  * The slots part as the mission goes on: a craft dies a frame or two later on one machine, its explosion holds its
    slot a while, and the next arrival takes another slot there. Measured, two machines through the demo's own mission
    (the rebels destroyed as they came, `!tdmcraft` on both every 20 s): the same from the start, apart from 2
    minutes on (BETA 3 in slot 6 on the guest, 8 on the host, for minutes; GOLD 5 in a slot the host had empty).
    Every message named craft by slot, so a hit on GOLD 5 went to another craft or to none on its owner's machine
    (never applied: it took hits forever, shown at once here and never fatal), its owner's reports went to another
    craft, and the shots its machine made came out of whatever held that slot here, their flashes on our craft.
  * Now each craft has an identity, its flight group and the order of its arrival in it (a group's craft are made one
    after another, the same way everywhere): `kIdent | fg << 8 | arrival` (src/cpu/wcnet_tie_ident.h's `TieIdents`,
    the CD's session's too since the user asked for one mechanism: "We should have the same mechanisms for both"). Every message names craft by it (TieCraft,
    TieShot, TieHit, TieEvent, the frame's own craft and target, the owners' table: TieOwners.craft beside .owner),
    each machine finding its own slot (`local`). The slots are scanned at the frame's top and at a hooked call's entry
    (`scan_idents`, never from the clock's tick: a craft half made has its type and not yet its group); a new object in
    a slot is a new craft, nothing of the last one's kept there (its owner the host's until the host says, its
    report, a kill under way); one gone from its slot without its death sent is an exit, sent with its identity.
  * A craft here that the host's table has gone without for 4 s (arrived only here, or gone there unseen here) is
    flown here: held for an owner who never reports it, it would fly on straight forever.
  * A bolt made here from another machine's report that hits a craft we fly passes: no flash, no hit (its shooter's
    machine says whether it hit, and its word brings the hit and its flash). Before, every such bolt flashed on our
    craft (the damage skipped), hits the shooter's machine never saw among them. The CD's session the same.
  * Measured after (the same run, by identity): every craft on one machine was on the other, the same identity within
    0.07 km of its owner's place near the pilots (up to 1.8 km for the far craft our AI flies between sparse reports),
    with the slots apart as before.
* **Dockings, as the CD's session makes them** (2026-11-16, the user, of the CD's: "we must fix all these not synced
  items ... only the host can ever own ships that are doing docking operations"; and before: "We should have the same
  mechanisms for both"). DEMO.TIE boards too: LAMBDA 3 (Board to Take, order 13, from 30 s), BLUE and LAMBDA (13 after
  their first). The boarding maneuver is the maneuver table's 18th (DS-image 0x43004: 30 far pointers to the run
  functions, then 30 to the inits; the CD's MODE_Board is 18 too): 0BBC:17FE, the CD's routine in 16 bits. Its
  CraftData fields: +0x5D the maneuver, +0x5E its step (0 to the dock point, 1 aligning, docked within 16 units: "has
  docked with"; 2 its delay, +0x60's 32-bit timer; at 0 the transfer; 3 backing off), +0x2E its target, +0x26 its
  order (the transfer's kind: 0x1C to 0x1F only, deliver, take, exchange, capture: no pick-up, no reload in this build),
  +0x8E, +0x92, +0x96 the push to the dock point, +0xAF the dock flags (a captured craft's 0x80 | its captor's group).
  The AI's context is the planner's globals, which pai_updateplaneai (091E:018C) sets inline: DS:D008 the craft, BF8C
  its record, D028 its flight group, D024 its leader, BF94 the leader's record, D02E its order's entry.
  * The owner sends its dock and its transfer (TieEvent kinds 6 and 7, as the CD's); a copy another machine flies is
    kept from both (the call skipped at its entry, AX 0).
  * The others make the same call on their copy (`tdm_queue_call(TDM_BOARD)`, no arguments): the six globals set as the
    planner sets them and put back after, the copy's target, order and step written first; a dock in two calls (the
    first finds the dock point, the copy is put there, the second docks).
  * The host flies every craft whose group has a boarding order (12 to 17), one boarding now or captured, and the craft
    such a craft is boarding (at once).
  * `!tdmcraft` shows a boarding (`board<step>><target>`) and the dock flags (`dock<flags>`).
  * Measured (`scripts/tdmnet-duo.sh`, 340 s, the pilots idle; the two Prowlers that shot LAMBDA 3 down in the first
    run killed at the start with `!tdmw`): LAMBDA 3 came out of hyperspace, flew to the yard's CREW QRTRS at the
    game's pace (33 a tick), docked some 3 minutes later (sent; the guest made it at its first call) and took the
    cargo 40 s after (order 0x1D, sent; the guest's copy made it, its step then 3); it then left through hyperspace on
    both machines. Its place on the two machines within 0.1 km throughout. A held copy here runs no maneuver (unlike
    the CD's, whose maneuvers run every tick): the guest's copy showed no boarding of its own.
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
* **A guest who arrives once the host flies** waits for the host's next launch (section 5, told so over its picture;
  Esc flies alone meanwhile): a long flight of the host's is a long wait.
* **The flash of a hit applied from another machine** is drawn where the stand-in bolt is (the shooter's bolt's
  place): the CD port's impact offset globals are not known in this build.
* **The campaign's balance** (section 8) is set from measured speeds, hulls, shields and torpedo damage and from the
  AI's own flights, not yet from people flying it: the shuttle run's timing, the bomber's escorts and the escort's
  Y-wings may want a turn once flown.
* **A guest who joins mid-campaign** flies from the host's next launch, whichever mission that is.

## 8. The campaign (src/cpu/wcnet_tiedemo_campaign.cpp, scripts/tdmmissions.py)

The user, 2026-11-15: "Can players win the mission? if so can you have them deploy again as 3 Tie interceptors using
the tie fighter cockpits from the prowler corvettes and have to attack a series of about 12 fleeing shuttles while
being targeted by x-wing and y-wings. The player should have time to deal with the shuttles as long as they don't
entirely focus on the x-wings. Then another mission where they fly a tie bomber and have to take out a number of
correlean corvettes while being dogged by a few y-wings and eventually some x-wings. The last one should be a escrot
mission for the prowlers flying as the classic tie fighter where a series of Y-wings are trying to take down the
corvettes with 1 or 2 x-wings for cover. The players must deal with them before they get into torpedo range. Finally
when that is done they may notice a distant shuttle trying to make an escape run. If they destroy the shuttle then a
series of freighters guarded by corvettes arrives." Then: "the mission shifts can be done in the protocol layer rather
than in the game code. In fact the mission table could be just shifted around since there aren't save games".

**How the demo's flight ends** (read in TIEMISS.EXE):
* `main` (0000:0000) flies the mission (0090:0006) again and again until a flight's status (DS:614C) is 3, "the
  mission over"; then TIEMISS exits and TIEDEMO.BAT plays the ending.
* A flight's frames run until the end flag (DS:614D) is set. A death (status 0), a capture (1) or a rescue after an
  eject (2) ends it, and `main` flies the mission again: the demo restarts after a death. `Q` then `Space` ejects.
* Winning does not end a flight: score_checkobjective (125A:0006) marks the primary goals complete (DS:614F = 1),
  says so and shows the header's win lines; the pilot flies on.
* The goals are this build's, older than the CD's: a flight group's primary goal state (DS:C492 + 0x30 per group) is
  0 open, 1 met (never looked at again) or 0xFF failed (the game then shows the header's loss lines); a goal's amount
  is the condition's amount as it stands (0 all, 1 three quarters, 2 half, 3 a quarter, 4 at least one, 5 all but
  one), not passed through the CD's percentcon. A "survived" goal is met at its first look and never fails after.

**The mission table, shifted** (the user's way): create_createmission reads DEMO.TIE whole into memory: the 456-byte
header (three counts, then OpenTIE's EMissionStruct: the time limit, the win and loss lines) at DS:C280, the flight
groups (OpenTIE's EFGStruct, 292 bytes, at most 48) at the mission segment's 0xD0, the radio messages (90 bytes, at
most 10) at its 0x3790. At 0FC2:0136, just after the read and before the craft are made, the layer writes the mission
to fly over that copy (the guest's seat and the host's seeds are written next, into it). No file is changed.

**The missions** (scripts/tdmmissions.py describes them; `build` writes src/cpu/wcnet_tiedemo_missions.h, `decode`
reads a mission file back):
1. **The Ambush**: the demo's own, as read (T/F Alpha). Its primary goals are every rebel group from the yard's fall on,
   each arriving as the one before it dies: with every rebel destroyed the moment it came (`!tdmrebels`), the game
   said "primary goals complete" 8 minutes in, and the next mission followed.
2. **The Shuttles Run** (T/I Alpha): four flights of three shuttles, a minute apart, each crossing ahead of Alpha to
   a jump some 19 km on (about 5 minutes at the shuttle's 66 m/s, measured); two of each flight must die (goal: half
   destroyed). Two X-wings and two Y-wings attack Alpha from the start, two more of each come with the third and fourth
   flights. The interceptor (110 m/s) outruns them all.
3. **Corvette Hunt** (T/B Alpha, proton torpedoes: `W` arms them): four rebel corvettes patrolling 7 to 10 km out;
   three Y-wings attack Alpha from the start, two X-wings come 20 s after the first corvette dies, two more once half
   are gone. Measured first: a corvette has 5,000 hull behind 10,000 of shields, a torpedo does some 1,300 to 2,500,
   and the T/B carries four (two a launcher): twelve torpedoes for four corvettes could not do it. So Alpha flies
   with twice the warheads and the corvettes with half their shields (a flight group's status byte, +0x34, TIE95's:
   1 and 4, measured in this build), some five torpedoes to a corvette.
4. **Escort the Prowlers** (T/F Alpha): three Prowlers on a slow loop; three flights of Y-wings (eight torpedoes each,
   two, three and three of them), each with one or two X-wings for cover, each coming when the one before is
   destroyed, from 9 to 11 km out. A Prowler lost fails the mission (the layer's rule: this build's "survived" goal is
   met at its first look and never fails). Then the shuttle KESTREL runs, far off (a bonus); if it dies, two pairs of
   freighters and two corvettes arrive for it (the freighters a bonus too). Alpha's two AI wingmen took the first
   flight on 2.5 to 4 km out, before it fired.
   * Speeds measured in flight: the world moves 0.44 m/s per unit of a craft's speed: the T/I 110 m/s, the T/F and
     the X-wing 99, the T/B and the Y-wing 79, the shuttle 66.

**The radio, and the Thrawn of it.** The user, 2026-11-15: "make sure to add some nice flavor text conversations
during the mission. I wonder if this can be connected to the whole Thrawnverse fan fiction scene through a few tactical
callouts of mainstream plot elements." TIE Fighter's own later battles bring in Grand Admiral Thrawn, so the missions
are his: his flagship the Chimaera and Captain Pellaeon give the orders, and the callouts are his best-known ways: the
X-wings read as a decoy, the corvettes' gunners known to watch the bow, a Noghri team aboard the Prowlers, the officers'
escape and the convoy that comes back for them foreseen ("they never leave their own"), and on a failed shuttle run
"Thrawn forgives an honest mistake. Once." Each mission has the most the game holds, ten radio lines (60 characters:
the message bar), the briefing (the emulator's line over the picture, wrapped) and the header's win and loss lines.
* The lines are the game's own messages, shown on its message bar with its sound. This build ignores a message's delay
  byte (score_checkobjective shows a message as soon as its conditions hold: 125A:02E4), so a line meant for later
  (`after`: seconds past the flight's start or a flight group's first craft) is written with never-true conditions
  and the layer makes both true when its time comes (wcnet_tiedemo_campaign.cpp `release_timed`); the game then shows
  it at its next look. Lines tied to an event (an arrival, a death) are the game's own triggers.
* The game ends a line with a full stop unless it ends with "!" or "?": the builder leaves a written final "." to it.

**The end of a flight, and the next** (the layer's, per mission: Mission's end rule):
* Won: a few seconds after the primary goals are complete (Escort the Prowlers: once its tail, the shuttle and what
  comes for it, has been quiet for 25 s, 10 minutes at most) the layer sets the end flag.
* Failed: a primary goal failed (0xFF), or a protected craft dying: the end flag 8 s later; the same mission next.
* At 0090:04FB (the frames over, the status not yet looked at) the status is written: 3 once the last mission is won
  (TIEMISS exits, the ending plays), else 2, so that `main` flies again: the next mission or the same one. A pilot's
  own end (status 3, not ended by the layer, the room or the clock) is left alone: the program ends as it did. (Esc,
  the game's own way out, is kept from it since 2026-11-15: section 1.)
* In a room the host decides: its go carries the mission (TdmState.mission), its verdict on each flight the next one
  (TdmState.verdict/next/over). A guest's program waits at 0090:04FB for the host's verdict on the flight it flew
  (the launch's hold, the PSP's `jmp $`), 20 s at most.
  * The host's verdict also ends a guest's flight (3 s later, as a pilot down does): the host's frames simply stop when
    its flight ends, never saying so. The guest keeps the verdict when it comes, and ends its flight by its own clock:
    when the campaign is over the host's program ends at once, and the room with it, before the guest's flight does.
  * A hold let go jumps back to its place, and the dispatcher does not look again at an IP a hook has just set: the
    "released" flag that was to stop the hook holding again at once was left set, and the next flight's launch went
    without the room (the demo alone never launched twice). It is now cleared at the mission read (and the verdict's
    at the next flight's first frame) in any case.
* The cockpit's warhead counter is drawn only when its count changes, and never for a craft without warheads: the T/F
  of Escort the Prowlers kept the T/B's 2-2 from Corvette Hunt. The layer empties our craft's warheads (CraftData
  +0x114: 6-byte weapon slots, +0 the type, 0x14 a laser and 0x18 a torpedo launcher, +2 the count) a few frames
  before any flight's end it decides, so the counter shows 0-0 when the next begins.

**Test aids** (key script): `!tdmmission=<n>` the next flight's mission (the flight under way ended), `!tdmwin` the
primary goals marked complete, `!tdmrebels` the rebels' craft destroyed one at a time as they come (the game's own
damage), `!tdmcraft` every craft logged (flight group, type, place, speed, hull, shields, warheads, goal state),
`!tdmcd` our craft's data in hex; in the environment `TDM_MISSION=<n>` starts the campaign there and
`TDM_MISSION_FILE=<path>` flies that mission file instead of the later mission (a variant from the builder's classes,
no rebuild). scripts/tdmcampaign-duo.sh flies the campaign on two machines with scripted wins; scripts/web-tdm.sh
with a host script holding `!tdmwin` checks two pages go on to the next mission.

**Measured (2026-11-15):**
* Alone: the four missions one after another with scripted wins, then the program's end and the demo's ending.
* Two machines (CAMPAIGN DUO OK): missions 2 to 4 launched together by the host's go (launches 1, 2, 3), the guest in
  its own craft of Alpha in each, the host's verdicts taken, both programs ending together after the last; 30 of the
  other's shots made each way.
* The bomber's torpedoes cross the room: the host's two, remade on the guest's machine (type 24, the launcher's).
* A pilot down: both flights end, the guest waits for the host's verdict ("the same mission"), and both launch it
  again together with new seeds.

## 9. The flight's keys (src/cpu/wcnet_tiedemo_controls.cpp)

The user, 2026-11-15: "the keyboard suffers some of the same problems as TIECD, so I think that will also need to be
improved accordingly". The CD port's keyboard (docs/tiefighter.md step 7), fitted to this build's own input code.

**How the demo steers** (read from TIEMISS.EXE: the poll at image 0x3654, the stick at 0x3190):
* The mouse's motion counters (INT 33h function 0Bh) are a frame's stick: 128 to a mickey across, 64 up and down
  (DS:CD70, DS:CD76, clamped to 191 and 127 mickeys), nothing within 2,048 across and 1,536 up and down.
* The arrows come one a key event from the BIOS's buffer (`getch`: codes 1 to 4 in DS:BF96), each a push of the stick
  that grows while the same arrow repeats: across 2,112, then 4,096 more an event up to 12,288 (half the mouse's
  most); up and down 1,600, then 1,536 more up to 8,192. Held, the keyboard's own repeat gave one push, nothing for
  half a second, then the rest. Measured, a T/F: Left held, the nose still for 0.55 s, then turning, and turning on
  after it was let go.
* Space confirms what the onboard computer offers (pstate+0x20, DS:CD98: 1 the target it suggests, DS:CD9A, 2
  another; the flight's key switch, image 0x1458C) and fires nothing; Ins and Del do nothing; the left button fires.
* The right button held is the X-roll (the stick across rolls), but only once it has been held 78 frame ticks (0.33 s:
  pstate+0x2C, DS:CDA4, counted against last frame's buttons at pstate+0x2A, image 0x143C5); until then it is masked
  out, and let go sooner it is a click, which picks a target.

**Now, in flight** (the layer's, the game unchanged; `tdm_key` passes every key but Esc to `tdm_controls_key`):
* The arrows are taken from the game and steer by RetroVega's ramp (WC1's), as mickeys added at the frame's top
  (`tdm_controls_frame`, from 0090:06CE): a notch further every other frame, 1 to 9, the opposite arrow or letting go
  0, full in some 0.3 s. The ramp runs between the game's own first push and its arrows' most (17 to 96 mickeys
  across, 25 to 127 up and down); Shift with an arrow is the whole stick at once (191 across: the mouse's sharpest
  turn). Up dives, as a stick pushed forward.
* The keypad's Ins and Del roll on the same ramp: the right button pressed and marked as held long enough already
  (the two words above written), so the roll starts at once and letting go is no click; the stick across, to 69
  mickeys (some 80 degrees a second in a T/F, near the CD port's keyboard roll); Shift the whole stick. Nothing up and
  down while rolling (the X-roll's up and down is the throttle).
* Space fires: the mouse's left button, held while Space is, unless DS:CD98 is set: then Space is the game's.
* Q is not passed to the game (the user, 2026-11-15: "Q as eject is punishing--can we disable that key"): the game's
  Q offers the flight's end and its Space confirms it, an eject, and Space is the trigger here, so a Q pressed by
  mistake and the next shot ended the flight. Measured: Q then Space twice, the flight ended each time; now it flies
  on and the Space fires.
* The mouse and a controller as the CD port has them (the user, 2026-11-15: "point to aim mouse mode should be default.
  They can select drag to aim just like on cd version. also joystick should be like cd version too"): the page's
  Flight choice beside Controller, shown for the demo too (`isTie` in web/wc.js), given at the launch
  (WCNET_TIE_MOUSE) and while flying (`wc_web_tie_mouse`); F12 in flight cycles aim, aim banking, drag, the keyboard.
  The mode, the pointer and the page's pad are wcnet_tie_controls.cpp's (`tie_controls_aim`: the pointer's and the
  pad's mickeys, each game's least and most: the demo's 17 to 191 across, 25 to 127 up and down; the CD's frame makes
  its own from the same function); the demo's frame adds them, the keys' over them.
  * **Point to aim** (the default): the pointer moves the ring (drawn round the gunsight's middle, (159.5, 76.5) of
    the 320 x 200 picture, measured on a frame; not drawn within the middle's dead zone, the CD's rule), its distance
    from the middle is the stick. The game's bank into a
    turn is left out: user_inputforplane's `sub [roll],turn` for a turn that is not the X-roll's (image 0x15818,
    load+1411:1708, its 21 bytes through the function's `retf` checked once) is stepped over to its way out
    (1411:1718), as the CD port steps over its own. The right button held with the mouse moved across rolls, once the
    game takes the button as held (the 0.33 s above: a shorter press stays the game's click, a target).
  * **Point to aim, banking**: the same with the game's bank. **Drag to steer**: the game's own (1994). **Keyboard
    alone** and **joystick**: the mouse steers nothing.
  * **A controller** steers through the emulator's stick (`wc_web_stick`) whatever the mouse's mode, rolls on Ins and
    Del (the keys above), fires with the mouse's left button; its buttons: Backspace full throttle, X single, dual or
    quad fire, R the nearest enemy fighter, W the torpedoes (web/wc.js `TFD_PAD`). The page's controller code steers
    so only "in flight" (`wc_web_in_flight`), which counted Wing Commander's flights alone: the CD's never, so on the
    CD a pad moved the pointer as in a menu and, in the joystick's mode, steered nothing. Both TIE games' flights
    count now. A pad at rest says (0, 0) every frame: its stick steers only while pushed, or the pointer would be held
    still.

**Measured** (native, `!tdmpose`: our craft's attitude, nose, target and the shots made):
* A T/F, Left held 1 s: turning from the first frame, the ramp full in some 0.4 s; Right held 1 s brought the nose
  back to within a degree. Shift+Left 0.5 s: some 33 degrees.
* Ins held 1 s: rolled 59 degrees, the nose and heading unmoved (82 degrees a second at full); a 0.1 s tap 0.4
  degree, the target unchanged.
* Space held 1 s: four shots of ours.
* The T/I (The Shuttles Run) and the T/B (Corvette Hunt): Left 1 s then Right 1 s back to within a degree (the CD's
  sign bug, a stick read unsigned for craft of other turn rates, is not in this build's), Up dives, Down climbs, Ins
  rolls.
* The mouse's modes, natively (the pointer pushed half way right 1 s with `!mickey=150:0`): point to aim turned the nose
  some 25 degrees, its roll as the turn about the craft's own axes leaves it (+8.7 degrees in the game's angles), aim
  banking the same turn with the game's bank (-13 degrees: the bank 22 degrees more), drag a twitch (one motion); the
  pointer back to the middle, the turn stopped; the right button with the pointer across rolled from 0.33 s on, the
  heading unmoved. F12: aim, aim banking, drag, the keyboard, aim.
* In the browser (scripts/web-tdm-flight.mjs, one page, a stand-in gamepad: TDM FLIGHT OK): the Flight choice shown with
  the demo, point to aim by default; the pointer pushed right turned the nose 85 degrees in 3 s, the ring drawn right
  of the gunsight (centred before); joystick: the pad's stick right 2 s turned it 67 degrees, the mouse pushed nothing;
  point to aim again with the pad at rest: the mouse turned it 54 degrees in 2 s.
* The CD after the shared code (scripts/tfnet-duo.sh): the pointer pushed 3 s turned it 59 degrees, roll 0, stopped
  when back; the Right arrow as before.
