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
  drawn into each game's frame.
* **Shared damage, the killer's money.**
  * Each pilot's damage to the wave's enemies and to the ground's buildings and bridges is told to the other and taken
    off the same enemy or tile there.
  * Each game kills it, explodes it and drops its bonus. The money goes only to the pilot whose damage killed it.
* **"GG for both."** A pilot shot down blows the other pilot's ship up too, through the game's own death and its
  death screen.
* Rooms of 2 (`maxSeats`); room codes start `RAP-`.

## 2. The program (src/cpu/wcnet_rap.cpp)

* **RAP.EXE.** 32-bit, DOS/4GW Professional, Watcom C compiled `-od` (no optimisation: the code follows the source).
  * The extender loads its LE into extended memory, flat.
  * The game's 1.2 source has been published under the GPL (github.com/skynettx/dosraptor). It was the map: the names
    below are its own, the addresses the shareware's image.
* **Finding it.** `scripts/rapsig.py RAP.EXE > src/cpu/wcnet_rap_sig.h` makes 23 code windows: relocated bytes and
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

The data:
* `playerx/y` 0x61474/8 and `playerpic` 0x61480; `curship` 0x64214 (the ship's frames as archive items);
  `draw_player` 0x64334; `plr.score` 0x642C8.
* `p_objs` 0x70CAC (energy `p_objs[16]->num`); `cur_game` 0x61484 and `game_wave` 0x61488; `cur_diff` 0x61704 and
  `curplr_diff` 0x614BC.
* The enemy list: `first_enemy`/`last_enemy` 0x6FBB8/0x6FAF8; the shots: `first_shots`/`last_shots` 0x6BBE8/0x699B8.
* The tiles: `hits` 0x68EFC and `tdead` 0x68468 (a SHORT per map spot, 9 × 150).
* `keyboard[]` 0x7DF84; the archives' entries `filedesc[f].item` 0x7C5C4 (0xA0 per archive, 0x28 per entry,
  +0x10 its memory).

**Damage.** An enemy's damage is measured each frame:
* its hits less than we left them, told as (its index in the wave's script, damage);
* the other's damage is taken off at the frame's top, kept up to 4 s for an enemy not here yet;
* our ship crashing into it, in ENEMY_Think, is told at its kill.

A tile's damage is measured only across SHOTS_Think. A tile's explosion damages its neighbours in TILE_Think, the same
on both machines, and must not be told twice.

**Drawing.** The game's sprite format is a GFX_PIC header, then segments {x, y, offset, length} and their pixels,
ending at offset −1. Sprites are drawn as GFX_PutSprite's clipped path does, within the map (x 16..304). A frame not in
memory here is drawn as our ship's current frame.

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

## 5. The rigs

* `scripts/raprun.sh [seconds] [key script]`: one machine, a copy of the game with its SETUP.INI. The route from boot
  is New Mission at 41 s, the name and call sign 45-50, the difficulty as offered 53, the hangar's Fly Mission 57-59,
  the Bravo sector 63, in the wave at ~64 s.
* `scripts/rapnet-duo.sh [seconds] [host's script] [guest's]`: two machines over TCP, both routed into the wave.
* `scripts/web-rap.sh [seconds]`: two browser pages in a room of a local lobby, the zip dropped into each. Ends
  "RAP PAGE OK".
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

## 7. Open

* The registered game (more sectors: FILE0002-4.GLB) is untested; none at hand. Its RAP.EXE is the same source built
  with REG_VERSION: the windows should find it if the hooked functions are unchanged. A build whose windows do not all
  match is left alone (the log says so).
* The other's ship has no engine flames or shadow.
* Enemies' shots aim at each machine's own pilot. A kamikaze chases its own machine's pilot.
* No pad built in (the keyboard and the mouse).
* A pilot who chooses a sector while the other is already in a wave waits for the other's next launch.
