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
* **Single-player for now** (`multiplayer: false`).
* `scripts/web-tiedemo.mjs PAGE_URL ZIP [seconds] [key script]`: one page, the zip dropped, a room of one flown (Esc
  at 8 s skips the opening), the canvas every 10 s. Ends "TIEDEMO PAGE OK".

## 4. Measured (2026-11-15)

* **Natively** (20,000 cycles, `8:esc`): the opening, then the mission's cockpit by 12 s: "T/F ALPHA, ENGAGE X-WINGS",
  "X-WING RED 1 HAS BEEN DESTROYED", the clock from 42:05.
* **In the browser:** the zip installed in 0.6 s, the flight by 20 s. TIEDEMO PAGE OK.

## 5. Next: the hooks

The flight program (TIEMISS.EXE) is found in memory and watched, as the other games' programs are:
* its frame loop;
* its table of craft (each one's place, attitude, speed, type, flight group);
* the player's craft;
* then two pilots in one mission, each the other's wingman in T/F Alpha.
