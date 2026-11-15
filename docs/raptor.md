# Raptor: Call of the Shadows, flown together

The user, 2026-11-14: "Ok next challenge: add support for the raptor call of the shadows demo (and full version). Same
deal: we want two planes flying together but each gets its own powerups. If either dies then it's GG for both. Each
can have their own upgrades, etc. This should be designed in a way that does not interfere with extant code".

## 1. What it does

* **Each pilot's game is its own.**
  * A new pilot or a saved one: their own ship, weapons, money, shop, saved pilots (`CHAR*.FIL`).
  * The bonuses the enemies drop are each game's own, for its own pilot to pick up ("each gets its own powerups").
* **The wave is flown together.**
  * A pilot who chooses a sector at the ship computer (the hangar's Fly Mission) waits there, its game idle, until the
    other is there too. Then both fly the host's wave: its sector, its wave and its difficulty's enemies (the guest's
    set to the host's before the map loads).
  * Esc during the wait flies that wave alone.
* **Kept in step.** Each frame's wait holds a machine that is more than two frames ahead of the other. The enemies
  come by the scroll, the same on both machines.
* **Both ships on both screens.** The other pilot's ship and shots (sprites, the turret's lines, the lasers' beams) are
  drawn into each game's frame, the ship with its engines' flames and its shadow (section 2's "The other ship's flames
  and shadow").  The user, 2026-11-14: "Also flames and shadow. Ideally other players flames are slightly different
  shape or color": its flames are pale blue and narrow to a point; ours are the game's own.
* **Shared damage, the killer's money.**
  * Each pilot's damage to the wave's enemies and to the ground's buildings and bridges is told to the other and taken
    off the same enemy or tile there.
  * Each game kills it, explodes it and drops its bonus. The money goes only to the pilot whose damage killed it.
* **"GG for both."** A pilot shot down blows the other pilot's ship up too, through the game's own death and its
  death screen.
* Rooms of 2 (`maxSeats`); room codes start `RAP-`.
* **A controller** picked in the page's Controller menu flies the ship (the user, 2026-11-14: "Please add gamepad support
  if selected on the menu"): section 4's "A controller".

## 2. The program (src/cpu/wcnet_rap.cpp)

* **RAP.EXE.** 32-bit, DOS/4GW Professional, Watcom C compiled `-od` (no optimisation: the code follows the source).
  * The extender loads its LE into extended memory, flat.
  * The game's 1.2 source has been published under the GPL (github.com/skynettx/dosraptor). It was the map: the names
    below are its own, the addresses the shareware's image.
* **Finding it.** `scripts/rapsig.py RAP.EXE > src/cpu/wcnet_rap_sig.h` makes 25 code windows: relocated bytes and
  outside calls masked, each checked to match once. The data addresses come from their operands.
  * It reuses `scripts/tvsig.py`'s `window()` and `matches()` and `scripts/tvexe.py`'s LE loader.
  * The search runs as Terminal Velocity's does: Do_Game's window first, then the others within 512 KB. A copy is the
    program only when all its operands point past 1 MB.

| place | where (shareware image) | what |
|---|---|---|
| Do_Game | 0x11128 | a wave begun (`srand(1024 * game_wave[cur_game])`) |
| its loop's top | 0x112A4 | each frame: our damage to the enemies measured and told, the other's taken off, our death told |
| SHOTS_Think's call | 0x11642 / +5 | the tiles' hits before and after our shots: our damage to the ground |
| the draw point | 0x11775 | `ANIMS_DisplayHigh` next: the other's ship and shots drawn natively into `displaybuffer` |
| the frame's wait | 0x11839 | `FRAME_COUNT - local_cnt < 3` over (70 a second: 23.3 frames a second): held while ahead |
| the loop's end | 0x11ABB | the wave over (its end, an abort, a death) |
| ENEMY_Add's call | 0x1845C / +5 | the wave's next enemy: its index in the wave's script (`cur_enemy - csprite`, 0x18 bytes each) noted for its slot (`ships[30]`, 0xBC bytes, `hits` at +0x50) |
| ENEMY_Think's kill | 0x18F81 | `hits <= 0`: `plr.score += curlib->money` skipped (to 0x18F8D) for a kill the other's damage made |
| TILE_Think's kill | 0x12D7C | a tile's `hits < 0`: `plr.score += money[spot]` skipped for the other's |
| WIN_MainLoop | 0x257F4 | the sector chosen: the launch (held, the wave set) |
| the code's first bytes | 0x10003 | never run (`int 3; jmp`, nops): the launch's idle written there (`jmp $; jmp <the launch>`) |
| SHADOW_Add | 0x172BF | (not watched: its window gives `shads`, `num_shadows` and MAX_SHADOWS, the cmp's byte) |
| FLAME_InitShades | 0x19760 | (not watched: its window gives `palette`) |

The data:
* `playerx/y` 0x61474/8 and `playerpic` 0x61480; `curship` 0x64214 (the ship's frames as archive items);
  `draw_player` 0x64334; `plr.score` 0x642C8.
* `p_objs` 0x70CAC (energy `p_objs[16]->num`); `cur_game` 0x61484 and `game_wave` 0x61488; `cur_diff` 0x61704 and
  `curplr_diff` 0x614BC.
* The enemy list: `first_enemy`/`last_enemy` 0x6FBB8/0x6FAF8; the shots: `first_shots`/`last_shots` 0x6BBE8/0x699B8.
* The tiles: `hits` 0x68EFC and `tdead` 0x68468 (a SHORT per map spot, 9 × 150).
* `keyboard[]` 0x7DF84; the archives' entries `filedesc[f].item` 0x7C5C4 (0xA0 per archive, 0x28 per entry,
  +0x10 its memory).
* The air shadows: `shads` 0x6DE44 (12 bytes each: item, x, y), `num_shadows` 0x616F0, at most 50; `o_engine` 0x613D4
  (8 dwords: the engines' spread at each bank); `palette` 0x64340 (a pointer to the 768 bytes, 0..63 each).

**Damage.** An enemy's damage is measured each frame:
* its hits less than we left them, told as (its index in the wave's script, damage);
* the other's damage is taken off at the frame's top, kept up to 4 s for an enemy not here yet;
* our ship crashing into it, in ENEMY_Think, is told at its kill.

A tile's damage is measured only across SHOTS_Think. A tile's explosion damages its neighbours in TILE_Think, the same
on both machines, and must not be told twice.

**Drawing.** The game's sprite format is a GFX_PIC header, then segments {x, y, offset, length} and their pixels,
ending at offset −1. Sprites are drawn as GFX_PutSprite's clipped path does, within the map (x 16..304). A frame not in
memory here is drawn as our ship's current frame.

**The other ship's flames and shadow.**
* *Its shadow* is put among the game's own air shadows after SHOTS_Think's call (`shads [num_shadows]`, as SHADOW_Add
  puts ours, within MAX_SHADOWS). SHADOW_DisplaySky draws it with the others: in the game's 3D perspective, under the
  enemies in the air, only at the detail that draws shadows.
* *Its flames* are drawn natively just before its ship, where FLAME_Down puts ours: two flames under the ship, at the
  ship's middle −/+ `o_engine [its bank]` (RapState carries the bank), 8 or 12 rows by `gl_cnt`'s parity.
* The game's flame lightens each pixel through 8 light tables, the brightest at the ship. The other's takes each pixel
  towards a pale blue (30, 46, 63 of 63) instead: 81% of the way in the row at the ship, 10% in the last. Its last third
  is 2 pixels wide, not 4.
* Its 8 tables are made from the game's `palette` as GFX_MakeLightTable makes the game's (GFX_Remap: the nearest colour
  by the sum of the differences). They use colours 0..239 (240 on rotate), made again whenever the palette changes.

## 3. The room (src/cpu/wcnet_rap_session.cpp)

Messages from field 80 of `NetworkMessage` (src/wc.proto):
* `RapHello`: the seat.
* `RapState`: every frame of a wave, else ten times a second. Where the pilot is (the launch and its serial, the run,
  the wave and its frame), its ship (place, frame item, shown), its shots.
* `RapHits`: (enemy index | 0x40000000 for a tile's map spot, damage) pairs.
* `RapDead`.

The host relays a guest's messages to the other guests, as Terminal Velocity's does. A run is the host's launch serial:
messages of another run are ignored.

## 4. The page

`web/gamefiles.js` registers `rap`:
* detected by `RAP.EXE`; known by the string "ENEMY_Think() - Invalid ANIMTYPE1";
* 40,000 cycles, 320 × 200, an S3, a Sound Blaster 16 at 220/7/1/5;
* saves `CHAR*.FIL` and `SETUP.INI`.

The shareware zip (`1rap12.zip`: INSTALL.EXE and RAPSW12.SHR, a zip of the game) is unpacked by `expandSplitZips`:
any `*.SHR` that reads as a zip. `rapPrepare` writes `SETUP.INI` as SETUP.EXE does when there is none (RAP.EXE will not
start without one): the Sound Blaster for the music (its FM) and the effects, the keyboard and the mouse.
`web/chatfilter.js` knows the tag RAP. The overlay's "0 key" chat hint is not shown (Raptor has no chat line).

**On the site, free to play.** The user, 2026-11-15: "Can you include the exact copy of both demo versions on the
website since they have redistribution rights. That way people can get retrovega and the demos right away. Then they
can link to gog for the full version".
* `web/demos/1rap12.zip` is the archive exactly as it came (sha256 7d6b062d...); `scripts/build-web.sh` copies `web/demos`
  into the page's `demos/`.
* Its VENDOR.DOC: "Everyone can -- and is encouraged! -- to copy, upload and generally pass around this Program without
  charging for it", every file unmodified, and "All advertising of the Program must include 'Apogee' in the
  description". The page's words name Cygnus Studios and Apogee.
* Step 1's "Free to play now" row (`useDemo` in web/wc.js, `DEMOS`): its Raptor button uses the copy this browser kept,
  or else fetches `demos/1rap12.zip` and reads it as a dropped file (and keeps it).
* The row links the archive and the full game. Raptor is not sold on GOG: the link is Steam's "Raptor: Call of the
  Shadows (1994 Classic Edition)" (app 358360, the DOS original).
* `scripts/web-demos.mjs PAGE_URL` checks it (DEMOS PAGE OK), and `DEMO=1 scripts/web-rap.sh` flies two pages from the
  button.

**A controller.** A pad picked in the Controller menu (or picked up by a press of its buttons) plays Raptor through its
keyboard, the control the page's SETUP.INI gives it.
* Raptor's own joystick (SETUP's Control 2) is no more than its arrows (INPUT.C: full speed past a third of the stick).
  It costs the mouse in the menus and asks for a three-press calibration in text at every start, so no "Built in" choice
  is offered.
* *The stick and the D-pad are the arrow keys* (`host.stickKeys` in web/gamepad.js):
  * the stick in a wave, past 30% of its travel (`wc_web_in_flight` is 1 while Do_Game runs: `rap_in_wave`);
  * the D-pad everywhere (the menus take the arrows too);
  * in the menus the stick moves the pointer and A clicks, as for every game.
* *The buttons are Raptor's keys* (`RAP_PAD` in web/wc.js):
  * A: Ctrl (fire);
  * B: Alt (the special weapon);
  * X: Space (the next special);
  * Y: the right Shift (the mega bomb);
  * Start: Enter in the menus, P (pause) in a wave;
  * Back: Esc.

## 5. The rigs

* `scripts/raprun.sh [seconds] [key script]`: one machine, a copy of the game with its SETUP.INI. The route from boot
  is New Mission at 41 s, the name and call sign 45-50, the difficulty as offered 53, the hangar's Fly Mission 57-59,
  the Bravo sector 63, in the wave at ~64 s.
* `scripts/rapnet-duo.sh [seconds] [host's script] [guest's]`: two machines over TCP, both routed into the wave.
* `scripts/web-rap.sh [seconds]`: two browser pages in a room of a local lobby, the zip dropped into each. Ends
  "RAP PAGE OK". `PAD=1`: the guest flies by a simulated standard gamepad picked in its Controller menu. In the wave the
  stick left, the stick right and the D-pad up must move its ship that way, and A held must fire.
* Aids:
  * `!rapenergy=N` (our energy);
  * `!rapgod=1` (kept full);
  * `!rapshot=<frame>:<name>` (the game's frame at that frame of the wave, as a PPM: two machines' pictures compared
    frame for frame);
  * the key script's `+ctrl` (fire) and `shift`.
* Each second the log has: frames, the wave, the ship, energy, money, damage told and taken, kills by the other, the
  run, tiles destroyed, the other's ships drawn.

## 6. Measured (2026-11-14)

* **Two native machines over TCP:**
  * At the same wave frames (300, 600, 900, 1200) the same enemies and scenery on both screens, each showing both
    ships.
  * The host firing, the guest not: 25 enemies dying on the guest's screen by the host's damage. Money: host 18,300,
    guest 10,800 (its own only).
  * The bridge the host destroyed destroyed on both (12 tiles each).
  * The host blown up (`!rapenergy=0`): the guest blown up, both at the game's death screen.
  * The launch held for the one there first; Esc there flies alone.
* **Two browser pages through a local lobby:** RAP PAGE OK.
  * Each drew the other's ship 1,852 frames; 309 damage told, 307 taken.
  * Both at 100% of real time, 23.3 frames a second (the game's own rate).

* **Flames and shadow, two native machines:** the frames at the same wave frames show the other's ship with its blue,
  narrowing flames (12 rows and 8 by the parity) and its shadow, below and to the right of it in the game's
  perspective; our own ship has the game's flames.
* **A controller, two browser pages (`PAD=1`):** the guest's ship at (144, 160), the stick left took it to x 5, the stick
  right to x 282, the D-pad up to y 0; A held fired (its damage told). RAP PAGE OK: both pages at 100% of real time,
  23.3 frames a second.

## 7. Open

* The registered game: done (section 8).
* Enemies' shots aim at each machine's own pilot. A kamikaze chases its own machine's pilot.
* No "Built in" pad choice (the game's own joystick, Control 2): a controller plays through the keyboard, which steers
  the same.
* A pilot who chooses a sector while the other is already in a wave waits for the other's next launch.

## 8. The registered game (Steam's 1994 Classic Edition)

The user, 2026-11-15: "I need help figuring out how to allow players to play the full raptor call of the shadows
together. It is not available on gog so players will have to look elsewhere. I did steam show files and zipped it up as
steam_version_of_raptor.zip and I did raptor_steam_backup.zip through the UI where it said export files ... for now
lets make sure the full edition also works like the demo so that players can indeed look at the demo be like 'I wanna
buy this' and go to steam to buy it and then actually play multiplayer".

**The two archives.**
* `steam_version_of_raptor.zip` (Steam: right-click the game, Manage, Browse local files; the folder zipped) is the
  game: `Raptor Call of the Shadows/Raptor - Call of the Shadows/` with `RAP.EXE` (474,359 bytes), `FILE0000.GLB` and
  `FILE0001.GLB` (byte for byte the shareware's), `FILE0002.GLB` to `FILE0004.GLB` (the Tango sector and the Outer
  Regions), `FILE_ID.DIZ`, `SETUP.EXE`, `SETUP.INI`, and Steam's own DOSBox beside it (`Dosbox/`, `RAPTOR.CONF`:
  left out by the page).  This is what the page takes.
* `raptor_steam_backup.zip` (Steam's Backup game files) is no use: `sku.sis`, a `.csm` index and a `.csd` of
  compressed chunks named by their hashes; the files' names and which chunks make them are in a manifest Steam
  fetches, not in the backup.

**Its program** is another build of 1.2: functions from `ENEMY_Think` on up to 0x24 lower, the data 0x10 higher.
`scripts/rapsig.py SHAREWARE/RAP.EXE REGISTERED/RAP.EXE` now tries every window against the second program too.  23
of the 25 were in it as they were; `frame_top` and `game_end` were not, and not because the code differs: their
windows started inside an instruction, so the bytes were read wrongly from there and a call's offset in each was kept
as code.  Both now start at an instruction (the generator refuses a window whose reading does not come to its place),
and all 25 match both programs once; the hooks, the session and the drawing are untouched.

**It will not start without `FILE_ID.DIZ`** ("Critical file FILE_ID.DIZ missing or modified"): the page keeps it
(and looks for it anywhere in the archive, below).

**A shareware pilot and the other sectors.**  The guest takes the host's sector before its map loads; a sector whose
files this game lacks (Tango: `FILE0002.GLB`; the Outer Regions: `FILE0003.GLB`, `FILE0004.GLB`; asked of DOS in the
game's directory) is not taken: the launch goes on waiting with "HOST flies the Tango sector, of the registered game:
the shareware has Bravo only (Esc: fly alone)".  Bravo is flown together by both.

**The page.**  `rapPrepare` now reads a `SETUP.INI` that came with the game: Steam's has the music on General MIDI
(`CardType=8`, port 330: the browser has no synthesizer), so `[Music]` and `[SoundFX]` are made the machine's Sound
Blaster (220, IRQ 7, DMA 1), the keys and the rest kept.  The words under "Free to play now" say how to get the
folder out of Steam.

**The loose finder** (the user, the same day: "very loose on finding the .EXE and supporting files so that if users
compress one level deeper or shallower or with different subfolder names or from their original DOS backups it still
works ... a find/search sort of situation to ground ourselves in whatever zip is passed").  `identifyGame` already
found the program at any depth, any case, its folder the game's.  Now (`web/gamefiles.js`):
* `gatherFiles`: each file the game wants by name (the registry's `gather`: for Raptor the `FILEnnnn.GLB`, `FILE_ID.DIZ`,
  `SETUP.EXE`/`.INI`, the saved pilots; for Terminal Velocity `GAME.EXE`, the `.POD`, `.VOX`, the drivers, the settings,
  and `*.TVI` into `TVI\`) that is not where the game looks is searched for through the whole archive and put there:
  of several, the one under the program's folder, else the one sharing most of its path, else the shallowest.  The
  page's log says what was moved.
* `expandInnerZips`: an archive with no game at its own level but a zip inside that holds a known program is opened
  one level further.
* Terminal Velocity's `CDROM.INI` is written when the cinematics are there without it.
* `node scripts/web-gather.mjs STEAM_RAPTOR.zip TV_FOLDER`: nine layouts (at the top, four folders deep, lower case, the
  program in one folder and its files in three others, a file twice, a zip in a zip; TV a level deeper and scattered):
  GATHER OK.

**Measured** (native, `RAPDIR=~/dev/rap-reg`): two registered machines, Bravo and (`SECTOR=tango`) Tango: "everybody at
the launch", "the wave begins together", damage told both ways, each ship on the other's screen (both screens
captured at the same moment with the ships flown apart: two ships on each, the other's with the pale-blue flames).
A registered host and a shareware guest: Bravo together; Tango: the guest held with the line above while the host
flies.
