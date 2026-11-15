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

* **Finding it.** DOS loads TIEMISS.EXE for TIEDEMO.BAT, so the layer knows its load segment from the start
  (`tdm_program_loaded`, beside Privateer's and Raptor's in wcnet_game.cpp). The build is checked by "TIE Fighter Demo
  V1.0 12/04/93" at image offset 0x30A70.
  * The places are image paragraphs from the load segment (01A2 under our DOS, PSP 0192).
  * DGROUP is image paragraph 0x421F (DS 43C1): measured by sampling DS in flight, 97% of the samples.
* **The overlays.** Image paragraph 3727 is the overlay area: zeros in the file, code at run time, a different overlay
  at 20 s and 26 s of the same flight. The HUD's code is there (3727:15A5 the counter, 3727:1623 the mission clock,
  3727:17AB the speed), so nothing is hooked there without checking its bytes first.
* **The frame.** 0090:06CE is the flight's frame, called from 0090:0006's loop:
  * ... 0090:07CC call 05B3:0000;
  * 0090:07D1 call 054A:0000: every object moved;
  * **0090:07D6** call 1768:058E: next, the picture. Watched: our place for "after the moves, before the picture".
* **The objects.** 054A:0000 loops over 0x74 (116) slots of 0x58 bytes from DS:67CA (`si` the record, DS:8FB0 it too).
  * +0x00 its number; +0x04 its type (a byte: 0 a free slot; 4 the T/F, 15 the corvette, 11 the repair yard's
    platforms, 1 the X-wing ...);
  * +0x06 x, +0x0A y, +0x0E z (int32; 054A:05DC adds the frame's motion, DS:5356/535A/535E, kept within ±0x01000000);
  * +0x12 x, y, z the frame before;
  * +0x1E three angle words (the platforms at 0x2000, 0x6000, 0xA000, 0xE000); +0x26 the speed (the T/F's full 225,
    100 on the HUD);
  * +0x34 its flight group (Alpha's craft alike);
  * +0x3A its forward unit vector (3 words, 0x7FFF one: the player's (0, 0.926, -0.376) flying along its motion),
    +0x42 forward again, +0x48 right, +0x4E up;
  * +0x54 its craft data (a far pointer).
* **The player.** DS:CD78 is the player's object (a near pointer: object 1 at the start, DS:6822), DS:CD7A its craft
  data. T/F Alpha is objects 0, 1 and 2.
* **The HUD's state** is at DS:E078: the speed at DS:E07A, the mission clock in seconds at DS:E084, from the minutes
  and seconds bytes at DS:C074/C075.
* **Shown by experiment** (`!tdmglue=dx:dy:dz`: object 0 given the player's record, offset, every frame at 0090:07D6):
  a craft whose record is written there is drawn where it says. Held 3,000 units ahead it sat in the gunsight, its
  back to us; 400 to the right, it moved there.

The research aids (the environment's):
* `WCNET_TDM_PROF=1`: where the CPU is and the DS, sampled, every 5 s.
* `WCNET_TDM_WATCH=<linear>:<bytes>`: every write to those bytes and the instruction after it.
* The key script's `!tdmrec=<object>` (a record logged), `!tdmglue=dx:dy:dz`.

## 5. Flown together (src/cpu/wcnet_tiedemo_session.cpp): the first step

* **The room.** As Raptor's: the host is seat 0, a guest says hello (TdmHello, field 90), the host answers with its
  seat (rooms of up to 3: T/F Alpha's three craft).
* **The state.** Every frame of its flight (else twice a second) a pilot tells its craft (TdmState, field 91): its
  record's place, the frame before's, its angles and speed (+0x06..+0x27), and its attitude's vectors (+0x3A..+0x53).
  The host relays a guest's to the other guests.
* **Each pilot flies the mission's own player craft on its own machine.** The others are put in T/F Alpha's other craft
  every frame at 0090:07D6, after the game has moved everything: the last record told, carried on by its own motion
  (the frame before's place to this one's) for the frames since.
* **Places.** At its flight's first frame a guest (seat m ≥ 1) takes its own place: its craft and Alpha's craft m - 1
  change places and attitudes. The host flies Alpha 1, the first guest Alpha 2, a second Alpha 3.
  * Each machine then puts the host in the craft whose place a guest took, and any other seat s in craft s - 1, where
    it began.
* **Not shared yet.** Each machine's enemies, shots, damage and the wingmen's AI are its own. The craft another pilot is
  put in is still flown by the game's AI between our writes (its guns too).

## 6. Measured (2026-11-15)

* **Natively** (20,000 cycles, `8:esc`): the opening, then the mission's cockpit by 12 s: "T/F ALPHA, ENGAGE X-WINGS",
  "X-WING RED 1 HAS BEEN DESTROYED", the clock from 42:05.
* **In the browser:** the zip installed in 0.6 s, the flight by 20 s. TIEDEMO PAGE OK.
* **Two native machines over TCP** (`scripts/tdmnet-duo.sh`, 30,000 cycles, about 59 frames a second each):
  * the guest in Alpha 2's place;
  * each machine putting the other in its flight group every frame, the other's craft at the very place the other's
    own machine has it (10240, -7611, 107844 on both; 8240, -6005, 105688 on both);
  * the host stopped, the guest 6,000 units ahead: the guest's TIE on the host's screen, upper right of the canopy.
* **Two browser pages through a local lobby** (`scripts/web-tdm.sh`): TDM PAGE OK.
  * Both found TIEMISS.EXE and flew; the guest in Alpha 2's place.
  * Each page put the other in its flight group (1,624 and 1,880 frames in 40 s).
  * 37.7 and 43.6 frames a second; the guest's TIE on the host's canvas as natively.

## 7. Next

* **The enemies shared:** the host's machine flies them, the guests' copies follow its reports (the CD port's
  ownership, docs/tiefighter.md section 5).
* **The other pilots' craft held from the AI:** no guns, no turns of its own between our writes. The craft data's flags
  (+0xA4 is tested at 3727:157F).
* **Shots and hits shared;** a pilot's death.
* **The launch together:** a guest's flight begins with the host's.
