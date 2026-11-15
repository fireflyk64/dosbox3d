# Terminal Velocity in the multiplayer layer

The user's brief (2026-11-14): "We want to add multiplayer to the original
[dosgamesarchive 3dtv12] demo shareware.  Same idea as other dosbox executable.  Two ships flying together coop in
the single player missio[n].  Should also work for the full version too when they drag and drop.  Needs to work with
the mission launch etc. should work in the higher res mode with sound and controllers."

On the ships (the same day): "Could we optionally select the Ymir IceRider", "Ya or the other snow vehicle", "Would
be nice for the non demo to have it select any of the craft. Ideally each player could select a different craft. If
there are limits maybe one will have to select the same or a planet themed one", "Eg two that are the same.  Or else
if different then the wingman can be a planet themed one", "Maybe host chooses both craft instead of tv202 that way
there are max the same number of craft".

This document is the reference for what is built, what was found, and the plan (section 6).

## 1. The game

The shareware (3dtv12.zip, 3.9 MB, from dosgamesarchive.com) holds `INSTALL.EXE` and a split archive,
`TV12BBS._1`, `._2`, `._3` (with `TV12BBS.IFC`, the installer's script): the three pieces put end to end are a zip
(343 bytes of the installer's own header first, a `3DREALMS.BAT`) of the game's 41 files, which `INSTALL.EXE`
unpacks to `C:\TV`.  The page does the same in the browser (section 2).

* `TV.EXE` (29 KB, Watcom 16-bit): "Terminal Velocity Operating System (TVOS) 1.2": loads the sound driver
  (`SOUNDRV.COM`, a DIGPAK driver) and runs `GAME.EXE`; without `SETUP.CFG` it says "Please run setup first".
* `GAME.EXE` (620 KB): the game, a 32-bit LE program bound with the DOS/4G extender ("BW collection"; the LE header
  at file 0x290A4, its program's MZ at 0x26654, data pages from 0x43654): a code object of 0x4AC7F bytes at image
  0x10000 and a data object of 0xB7EE0 (40 KB of it initialised) at image 0x60000; entry 0x513E8.
* `SETUP.EXE`: the sound setup (DIGPAK's: the chosen card's driver copied to `SOUNDRV.COM`, e.g. `SB16.COM`) and the
  modem/network setup; "Save and launch TV" writes `SETUP.CFG` (`ScreenMode`, `FXDevice`, `MusicDevice`, volumes, the
  comm settings) and runs `TV.EXE`.  The game's music is digital (mixed into the DIGPAK driver's output): the Sound
  Blaster 16 plays both.
* `SHARE.POD` (5.4 MB, 748 files, "TV 1.2 Shareware") and `STARTUP.POD` (3.4 MB, 340 files, "Startup 1.1 Gold"): the
  game's archives (a count, an 80-byte comment, then 40-byte entries: a 32-byte name, size, offset).  Models are
  `MODELS\*.BIN`, levels `LEVELS\*.LVL` with `DATA\*.DEF` (what a level's objects are), textures `ART\*.RAW`.
* `VOX.INI`: the game's own settings, written by the game (text: a line of names, a line of values): `musicFlag`,
  `soundFlag`, `mixSpeed`, `joystickActive,xcenter,ycenter,throttleMin,throttleMax`, `gamePIXX,gamePIXY,...` (320,200;
  the SVGA mode 640,480), `difficulty,briefingFlag,cinemaFlag,showDemoFlag`, the stick's calibration, the keys
  (`weaponKey,fireKey,leftKey,...` as scan codes), the mouse's settings.

Measured natively (DOSBox, an S3, 16 MB, 30,000 cycles, a Sound Blaster 16 at 220/5/1/5): the main menu at 38 s;
Play Game, New Game, the episode ("Episode One: Tactical Strike", the shareware's), the difficulty, the pilot's name
and call sign, the story's page, then (briefings on) a slide show of the level's craft and targets, about a minute;
with `briefingFlag` and `cinemaFlag` 0 in `VOX.INI` the flight starts at 62 s.  The first level is `artic.lvl`, the
planet Ymir: 476 objects.  Cheats (the game's own, typed in flight): TRWARPn (to level n), TRIFIRn (weapons), TRIHOVR
(the hover pad), TRFRAME (a frame counter), and others (strings at data 0x60104 on).

## 2. The page

`web/gamefiles.js` registers `tv` (detected by `TV.EXE`, run as `tv`, rooms of up to 4, room codes TV-, 55,000 cycles (60,000 before the pixels were drawn natively),
the 640 x 480 picture, an S3 and a Sound Blaster 16 at 220/5/1/5, the game's own joystick offered "Built in", saved
games `SAVE*.DAT`, `SCORE.DAT` and `VOX.INI`).  A dropped `3dtv12.zip` is the installer and the split archive:
`expandSplitZips` puts `TV12BBS._1`, `._2`, `._3` end to end and reads the zip in them (`readZip` now takes a zip with
bytes before it, the installer's header), which leaves the game's 41 files.  The registered game's installed directory
is recognised the same way (by `TV.EXE`).

**On the site, free to play** (the user, 2026-11-15, as in docs/raptor.md section 4).
* `web/demos/3dtv12.zip` is the archive exactly as dosgamesarchive.com serves it (md5 72e9f023..., its ETag).
* Its VENDOR.DOC: "Everyone can -- and is encouraged! -- to copy, upload and generally pass around this Game without
  charging for it", every file unmodified, and "All advertising of the Program must include 'Apogee'". The page's words
  name Terminal Reality and 3D Realms (Apogee).
* The "Free to play now" row's Terminal Velocity button fetches it (or uses the copy kept), and the row links the full
  game on GOG: the Boosted Edition, which comes with the DOS original ("Terminal Velocity Legacy").
* `DEMO=1 scripts/web-tv.sh` flies two pages from the button.

`prepare` (at every launch, after the files are installed): `SOUNDRV.COM` from `SB16.COM` when there is none (what
SETUP.EXE's Sound Setup does for a Sound Blaster 16), `SETUP.CFG` as SETUP.EXE's "Save and launch TV" writes it when
there is none (TV.EXE asks for setup without one), and in `VOX.INI` (made with the game's defaults when there is none:
briefings, cinemas and demos on) the high-resolution mode (640,240) and the control: a pad picked "Built in" (the
launch puts the game port in) makes it the game's Gamepad control (2: the flight applies the stick only above 1)
calibrated as the game's own Calibrate measures DOSBox's counting port (centre 64, 0 to 128); without one, a Gamepad
control left from before goes back to the keyboard (the port is empty), the mouse is kept.  The Graphics Options at their best at every launch (the user, 2026-11-14: "Let's ensure detail is
maximum"): the sky's texture (`skyTextureFlag` 1), Image Quality Pentium (`perspectiveFlag` 1), Shadows Pentium
(`airShadowFlag` 2), Texture High Detail (`textureResolution` 1: 12 MB free wanted, the machine has `memsize=32`),
Debris Overkill (`partsFlag` 2) -- read from the options menu's code (0x31130: [0x64CE4], [0x64D54], [0x64D0C],
[0x64DDC]) and the settings writer's.  Their cost in flight is small: a dogfight with a wingman at 60,000 cycles 20.1
frames a second against 20.6 with the shareware's own settings; the briefing's 3D slides step more slowly.  The
picture is really 640 wide: the game draws 640 x 240 (its own 640 x 480 breaks its terrain), each row shown twice
(measured on a frame: 12 of 320 column pairs alike, all 240 row pairs).  No fog or view-distance setting exists in
1.2 (the menus, VOX.INI and the flight keys read: `-`/`=` size the view, the keypad's `-`/`+` a look angle): the fog
is each planet's own table (the level's `.FOG`, 16 steps of 256 colours) and the view's reach is fixed in the code
(objects within 5 of the game's 16-unit cells).  Measured natively with
the scripted stick: the calibration screen reads 128/0 at the stick's ends and 64 in the middle, and in flight the
ship turns with it, button 1 fires.

The room: the host's Host's craft and Wingmen's craft menus (the TV-202, the Ymir IceRider, the Ymir SnowScout, the
Hawk, the Gull, the level's own craft; remembered by the browser) become `WCTVLOOKS` on the host's game, which tells
the guests' (TvLooks).  No mission menu: the host plays the game as it comes and the others fly the host's level.

## 3. The program in memory (src/cpu/wcnet_tv.cpp)

DOS loads `GAME.EXE`; its extender loads the LE into extended memory, flat (every selector's base 0, paging off):
EIP and every pointer the program holds are linear addresses.  Measured: the data object at linear 0x22E000 (image +
0x1CE000) both times, the code object at 0x1E3000 once and 0x1E2000 the next (image + 0x1D3000 / + 0x1D2000): not
the same from run to run.  So the layer finds it by its code, every 500 ms for a minute after DOS starts `GAME.EXE`:

* `scripts/tvsig.py GAME.EXE > src/cpu/wcnet_tv_sig.h` makes the signatures: for each place the hooks use, a window
  of the code object's bytes around it, with the bytes the loader relocates (the LE's fixups) and the offsets of near
  calls masked out, each checked to match exactly once in the shareware's code; and for each data address, the
  32-bit operand at a fixed point of a window, which the loader has made the linear address the game uses.
* The search: the fire function's window (47 bytes kept in a run: the key) over all memory past 1 MB, then every
  other window within 512 KB of it.  Memory holds a second copy of the code as the file has it (found twice: the
  extender's read buffer), whose windows match too: of each window's matches the one nearest where the shareware's
  layout puts it is taken, and a copy is the program only when every data operand is relocated (past 1 MB).
* A build whose code is the same but for where things are is found the same way; any window not found leaves the
  game alone ("not a build the hooks know" in the log).
* An anchor can have variants (`VARIANTS` in tvsig.py: another window for the same place, from another program, with
  its own data offsets), tried when its first window is not there: the registered game's campaign loop is one
  (section 7).

The tool for the rest: `scripts/tvexe.py GAME.EXE info|image|dis ADDR N|str TEXT|xref ADDR|funcs|callers ADDR|func
ADDR` (the LE laid out at its image addresses with its fixups applied; Capstone).

The map (shareware image addresses; the hooks read them out of the code, these are for reading the code):

| what | where | notes |
|---|---|---|
| the level loop | 0x2CDF0 (level file name) | returns nonzero for a level completed; asks "Retry Mission (Y/N)" itself on a death |
| its frame | 0x2D0D7 | `call 0x11010` (keys and stick) at the top of each frame, then the world (0x33840, 0x17990, ...), the objects (0x241E0), the nav (0x3F8F0), the render (0x3C450) |
| the remote-ship place | 0x2D0FF | `cmp [netgame],0` then `call 0x4E840` in a net game: the other players put in the draw list |
| the campaign loop | 0x2EC30 | level index `[0xDDD74]` < count `[0xDDD78]`: `0x2CDF0(0xDC730 + index*0x28)`, the level files' names 0x28 bytes each |
| the objects | `[0x680EC]` pointer, `[0xDAFD4]` count | 0xB4 bytes each: +0 x y z, +0xC three angles, +0x18 type, +0x1C hit points, +0x20 idle (1) / awake (0), +0x28.. the angles' offsets for drawing, +0x64 state (0 idle, 100/101, 200 attacking, 300/301, 600 going down), +0x84 in this frame's near list |
| the types | `[0x680E8]` pointer | 0x244 bytes each: +0 the model, +4 the model awake, +8 the radius, +0xC the class (7 craft, 9 installation, 10 ground vehicle, 31 boss, 33 tree, 0 zones), then the model files' names and the briefing's words ("CRAFT", "Ymir", "SnowScout") |
| the objects' loop | 0x241E0 | each object with hit points: `0x29390` (into the draw list; 1 = too far) then `0x28630(obj)`, its update (the AI and mover of its class, a jump table at 0x2857C); the near ones listed at 0xBD720, `[0xDAFD0]` of them |
| a shot's hit on objects | 0x292B0 (pos, damage) | for each near object, `0x290F0` the test; hit: hit points -= damage, at 0 or less `0x28E70(obj, type)` (the next stage: some objects come back, e.g. a craft going down with its hit points filled again; the last frees it) |
| the projectiles | 0xA7CB4, 256 x 0x5C | made by `0x19080(posmat, weapon, damage, owner, kind)`: owner 0 the player's (slots 0..127), else the others' (128..255); `0x19340` flies them: the player's against the objects (0x292B0), the others' against the player (0x133C0) |
| the player | 0x7CA78 x y z, 0x7CA84 three angles, 0x7CA94 a 3x3 matrix (1.0 = 0x10000) | 0x7CF00 velocity, 0x7CEEC the angles' rates, 0x7CED8 shield (0xFFFF full), 0x7CEC4 the ship's model (ship.bin), 0x7CF24 the frame's time (16.16 s) |
| the keys | 0x7CDB8 + scan code | a byte each, down/up |
| a model put in the draw list | 0x1E640 (posang, model, scale) | the frame's list at 0xAFE48, 0x48 each, `[0xB464C]` long, 256 at most |
| a model loaded | 0x402E0 (name) | a handle; a name not in the archives ends the program |
| the ship's attitude | 0x81C94, a 3x3 float matrix (0x81C90 a word before it) | its columns the ship's right, up and forward in the world; the flight step (0x11CD0, from the input handler) turns it by the spin rates (0x64EB0.., the ship's own axes), then makes the angles at 0x7CA84 and the fixed-point matrix at 0x7CA94 from it: writing those two does nothing |
| the afterburner | weapon slot 22 ("FLY" on the HUD) | taken while the key at `[0x64D58]` (VOX.INI's weaponKey: 56, Left Alt) is held and its fuel `[0xA7A34]` is above 0 (0 at a start; a bunker's pickup gives 120, the cheat TRIBURN some) |
| the high-resolution mode | `[0x64CFC]`, `[0x64D00]` = 640, 240 | the Options menu's "Res: SVGA (Pentium)": the view drawn 640 x 240, doubled to the 640 x 480 screen (VOX.INI's gamePIXX,gamePIXY 640,240; 640,480 there draws the terrain broken); needs 3,000,000 bytes free, else 320 x 200 |

The game's own network play (modem, serial, IPX; up to 4 players, its own `multi%d.lvl` levels): a Doom-like
driver (`COMMIT.EXE`) behind a block whose pointer is at 0x113DF4 (+2 the command, 1 send / 2 receive, +4 the node,
+6 the length, +0x10 the data, a checksum word last); messages of 14 types (a jump table at 0x4DEDC): 1 a ship's
place, angles, speeds and shield (0x4CC30, every 0x2000 of frame time), 2 a shot (0x4E940: the fire call's
arguments), 3 a line, 6 damage to a player, 7 an object's state, 8 a death, 10 a hit on an object... The remote
ships are 0x70-byte records from 0x113C00 (x y z, angles, their rates, alive at +0x30, shield at +0x68), carried on
by their rates (0x4CEC0) and drawn by 0x4E840 with the draw-list call.  The layer does not use the game's net mode
(it turns off the level's goals and loads the net levels): it uses the same calls with the room's reports.

### The pixels drawn natively (src/cpu/wcnet_tv_span.cpp)

The user, 2026-11-14 (after the profile): "Please hook the textured pixel drawing loops and draw them in native code
or wasm".  53-67% of a frame's emulated instructions were the game's span routines, 0x2FAB0 (the cockpit's picture
doubled into the 640-wide frame) a fixed few percent more; they are now made by C++ twins (wasm in the browser):

* The polygon filler (0x3E720) calls, for each scanline, `0x49707` (`jmp [0x7CF14]`: the routine the polygon's kind
  set) with esi, edi its two edges (x 16.16 at +8, w at +0x10, u and v at +0x18 and +0x20, divided by w or not, by
  the routine) and ebx the row.  The layer watches that call (`span_call`); the twin of the routine the pointer
  names draws the scanline into the frame buffer (in conventional memory: the rows at 0x13990.., 640 bytes apart)
  and the call is stepped over: the filler reloads every register after it but ebp, which the routines keep.
* The twins (each the routine's transliteration: the same registers, branches, 32-bit wraps and carries, quirks
  included -- the unrolled routines draw three pixels where four are left at the end of the loop):

  | routine | twin | what it draws |
  |---|---|---|
  | 0x4732E, 0x44C97 (640 wide), 0x46552, 0x444FA | `persp<step, 64x64?, fog>` | perspective, a divide every 32 (16) pixels, a 64 x 64 or 256 x 256 texture, the depth fog's shade stepped (two formulas) through its table 0x944E8: the ground, the buildings |
  | 0x42F18 | `affine_lit` | u/w, v/w divided at the span's ends only, one shade (0x7CF38) through the light table 0x934E8 |
  | 0x480D4 | `affine_clear` | unlit, colour 0 not drawn; u and v stepped as one packed pair (add, adc, adc), read back from the stack's locals at odd offsets |
  | 0x48693 | `zbuf_lit` | the craft: u, v stepped into a strip (0x68590), then each pixel nearer than the depth buffer's (rows 0x928E8) drawn lit, its depth kept |
  | 0x49503 | `fill` | one colour, rep stos (as the routine counts its bytes) |
  | 0x42F13 | `none` | nothing (push, pop, ret) |
  | 0x2FAB0 (an entry) | `cockpit_copy` | the cockpit's 320-wide picture (0xDDDF8, its runs at 0xE2DF8) into the frame, each byte a word; eax, ecx, edx left as it leaves them, then its ret |
  | 0x3E720 (an entry; with 0x3E510, 0x3E6E0) | `poly` | the polygon filler (vertices, count): the vertices not yet projected projected (x 0x18, y 0x1C = k x / z + the centre: 0x7CF30.., the divides tried first), the edges into the 16-slot table at 0x104DA8 (0x3E510: one's-complement 16.16 divides by the rows' distance, the whole part at most 0x7FFF, stepped on to the first row's centre; the top and bottom rows and the counters at 0x105028..), then row by row the two edges at it (0x3E6E0: an edge past its bottom replaced by the next starting there), the row's span drawn by its routine's twin (called directly: no hook a row), the edges stepped; eax 0, ecx and edx the edge count, the flags of its `add esp, 8` |
  | 0x3C7E0 (an entry) | `height` | the terrain's height under (x, z) for its 48 callers (game logic: the craft, the shots, the ground's collisions): the cell's four heights from the 256 x 256 map ([0x104DA0]) << 15, interpolated on the half of the cell its diagonal cuts (the diagonal by the cell's parities: four cases); eax the height, ecx and edx as it leaves them, the flags of its `add esp, 0xc` |

  Not twinned (the game's own still): 0x431BE, 0x436F2, 0x43F46, 0x458D4, 0x460F5, 0x46D98, 0x48379 (see-through
  blended), 0x48A90, 0x49191, 0x4954A, 0x496BC: rare at the page's settings (the 10 s log counts them).
* The filler's polygon is the game's own when its span routine has no twin, it has more than 16 vertices, or a
  projection's divide would fault (all tried before anything is written).  A span twin that gives back at a row hands
  the polygon to the game's code there: the filler's frame made as its code has it at its span call (ebx, esi, edi,
  ebp pushed, the row and the first edge its locals; esi the first edge, edi and ebp the second, ebx the row), the
  CPU sent to the call (0x3E85D); the game's span routine draws the row and its filler goes on.
* Each routine is compared whole with the shareware's (`kSpans` in wcnet_tv_sig.h: every byte but the relocated
  operands, and each of those pointing where the shareware's layout puts its data, moved by one delta) before its
  twin is used.  A twin gives the call back to the game's code, before writing anything, where it cannot be exact:
  a divide the CPU would fault on, memory outside the game's RAM (conventional memory and extended memory from 1
  MB), a span over 4096 pixels, a segment not flat, the direction flag set.
* `WCNET_TV_SPANS`: unset or 1 the twins; 0 the game's own code; `check`: each twin runs first, as it runs in play,
  the memory it declared (the span's row, the depth buffer's row, the routines' variables 0x68520-0x689A0, the
  cockpit's whole picture), widened by 64 bytes either side, is kept as the twin left it and put back as it was, the
  game's code runs, and at its return all of it and the registers are compared (the entries' flags too).  For a
  polygon the memory kept is its vertices, the edge table and the filler's counters, the routines' variables, and
  its rows of the frame buffer (and of the depth buffer for the craft's spans) once its edges give them.  Measured,
  dogfights at 640 x 240 and the best detail: 2,819,588 and 2,087,754 calls of the first nine, 1,030,129 with the
  filler and the height (thousands of polygons, hundreds of thousands of heights a 10 s), at 320 x 200 771,856: 0
  differing in a byte, a register or a flag.  `WCNET_TV_RESUME_TEST=1` with the check tests the hand-over: the
  twin's whole polygon kept as what the game's code must end with, put back, the twin again handing over after 1-3
  rows, the game's code finishing it: 131,247 polygons handed over, 0 differed.  Every 10 s the log counts each
  twin's calls and pixels, the polygons and their spans, the calls given back, the routines not twinned.
* In the browser these places (the span call, the entries, the cap's idle below) are made in the CPU core
  itself, as natively (`wc_net_hook_exit` takes `tv_fast_hook` first): they wait on nothing, and going out to the main
  loop for each of some 2,000 spans a frame cost the page 10-15% of its time.
* The frame cap (`WCNET_TV_FPS`, default 60 frames a game second, 0 none): with its pixels native the game made 85
  and more frames a game second, past what a screen shows, each costing the browser its drawing.  A frame top reached
  sooner than 1/60 s after the last goes to an idle in the padding after the empty function 0x49710 (checked zero
  with the dispatch; `jmp $; jmp <frame top>` written there), where each slice's cycles are skipped as DOSBox's own
  HLT skips them (the interrupts still taken) until the time is up, then on to the frame top (its hooks ran before
  the idle: a flag keeps them from running twice).  The game's frame time counts the idle, as any wait.  Each second's
  report ends "the cap's idle N ms in M frames".
* Measured natively, solo at 60,000 cycles, the page's settings, no cap (frames a game second): the level's start
  21.3 -> 128.0, flying and fighting 39.5 -> 130.8 (the two flights part as the frame rates differ); two machines in
  co-op (each flying, the guest on the host's wing): ~20 -> 86 (host) and 56 (guest).  With the cap: 60 where the
  game can (dips to ~40 in the busiest fighting), ~450 ms of each second idle.
* Two browser pages on this 5-core machine (scripts/web-tv.sh, the same build; percent of real time, frames a wall
  second): the game's own drawing 100%, 21.1 and 27.8; the twins at 60,000 cycles with the cap 98% and 97%, 55.8 and
  54.7; at 55,000 cycles 100%, 53.8 and 55.1 -- the page's setting since.
* The filler and the height natively too (the user: "can we fix polygon edge stepping and terrain height lookup to
  also be hooked?"; they were ~16% and ~11% of what was left): natively at 60,000 cycles, uncapped, the level's start
  128 -> 150 frames a game second, flying and fighting 131 -> 160; two browser pages at 55,000 cycles 100% and 99%,
  57.4 and 57.0 frames a wall second (53.8 and 55.1 before).  What is left is game logic, spread out: the vector
  math (0x1D060, ~9%), the objects' visibility and loop (0x1E4E0, 0x241E0, 0x29390), none past 9%.

## 4. The rigs

* `scripts/web-tv.sh [seconds]`: two browser pages in a room of a local lobby server (the built page, the shareware's
  zip dropped into each, the host's craft menus, Fly on both, each page's key script from boot): "TV PAGE OK".

* `scripts/tvrun.sh [seconds] [key script]`: one machine, booted, routed into the first level (briefings off in its
  copy's `VOX.INI`): in flight at 62 s.  `COPY=1` a copy of the game per run, `ENV=` more environment.
* `scripts/tvnet-duo.sh [seconds] [host's script] [guest's script]`: two machines over TCP (`PORT`), each its copy;
  `GUEST_LATE` seconds between their starts (the guest flies the same route that much behind: the host is ahead of
  it on the screen).
* `WCNET_TV_PROF=1`: where the game's time goes (the program's place sampled every ~1000 instructions, the busiest
  logged every 10 s, the whole count in `WCNET_DUMP_DIR/tvprof-<s>.txt`); `scripts/tvprof.py GAME.EXE DIR FROM TO`
  groups it by function.  The 2026-11-14 profile (the worktree's `profile.md`), before the pixels were drawn
  natively: 53-67% of the game's instructions in its textured-span loops (0x42EE0-0x49710), ~10% the polygon
  filler; the frame rate follows the cycles (37.6 fps at 60,000, 82.2 at 120,000, flying straight); the browser keeps
  60,000 at 100% of real time, 120,000 at ~53%.  Since: "The pixels drawn natively" above.
  `scripts/web-tv.sh` prints each page's speed; `CYCLES=` there, `DETAIL=low` in tvrun.sh for the shareware's own
  Graphics Options.
* Key-script aids: `!tvpos` (the player), `!tvobjs[=range]` (the objects within that many world units: class, name,
  place, hit points, state), `!regs` (the CPU's registers and segment bases), `!dumplin=name:start:length`.
* The rigs' pilot (src/cpu/wcnet_tv_pilot.cpp): `!tvfly=obj:N | near:CLASS[:range] | partner[:right:up:back] |
  head:DEG:DEG | off` flies the ship by writing its attitude (eased turns, banked into them, over the hills to a
  ground target and down onto it), `!tvfire=1|hold|0` (fires when lined up within 140 units, or always),
  `!tvburn=SECONDS` (the afterburner's key held), `!tvfuel=N`, `!tvgod=1` (the shield kept full).
  `!tvfly=tun:N` flies into tunnel N's mouth the game's way (to a point 30 units short of it and 36 above, then a
  dive at it); `!tvtunnel=N` calls the game's tunnel entry at once; `!tvtunnelend` puts the ship at the tunnel's far
  end (the game's own way out takes it there); `!tvcraft` counts the level's craft (in the table, alive, awake = their
  update called in the last second, of those puppets) and its launchers (class 21) and generators (class 41).
* `SVGA=1 CYCLES=60000` for the rigs: the high-resolution mode in each copy (60+ frames a second natively).
* The video: `WCLAB=<dir>` in each machine's environment (HOSTENV/GUESTENV), then `scripts/tfnet-sheet.py` (contact
  sheets of both recordings lined up by the wall clock) and `scripts/tvnet-video.py` (tfnet-video.py's vertical
  video, host on top, with `--zoom FROM:TO:host|guest:FACTOR[:CX:CY]` punch-ins: a ship 30 world units off is a few
  pixels); both import walkthrough/labrec.py (the main tree's walkthrough/: a symlink in a worktree).  The first
  video (2026-11-14, the user: "two people blast a building and a flier and can see each other and one can see the
  other boost"): SVGA=1 CYCLES=60000 GUEST_LATE=1, both at bunker 408 with fire, then the host to object 5 with a 3 s
  burn and the guest on its wing (partner:2:0.5:7), the guest's 4 s burn past it, then the SnowScouts (near:7); the
  bunker blew at 75.5 s on both, the kills at 131-140 s told across (8 stages).
* The game's files: the shareware unpacked into `tvgame/` (untracked) and set up once (`SETUP.EXE`: Sound Setup,
  Sound Blaster 16, then Save and launch TV).

## 5. The sync design

Everybody in the room flies the same campaign level.  A pilot's ship is reported some 15 times a second (TvState:
x y z, the angles, their rates per second, the shield, the level); the others draw it at the frame's remote-ship
place (where the game's own net mode draws its players) with the draw-list call, carried on by its rates up to a
second past its report, as the model the room chose for that pilot.  A pilot's shots are the fire call's arguments
(TvShot), made again on the others' machines as their own player's shots: they fly and hit the same objects there.

The level's objects are the same on every machine (placed at the level's load, in the same order): an object is
named by its index (a level adds objects as it goes, a bunker's pickup: those can differ, the level's own come first).

* **Hits are told by the shooter.**  A pilot's own shot's hit on an object (0x292B0, the hook at its confirmation) is
  applied in its game and sent (TvHit: the object, the damage); every other machine applies it the game's way, the
  hit points less the damage and, at none left, the object's next stage (`0x28E70(obj, type)`, called at the frame's
  top).  A copy of another pilot's shot never applies its own hits (the hook answers "skip": the shot is spent, the
  hit counted, nothing applied).  So every machine sees the same hits and the same stages (a bunker blown open, a
  craft going down), its own explosions included; measured, a SnowScout shot down by the guest died on the host.
* **Moving objects have an owner.**  The host gives each moving object (a craft, a ground vehicle, anything seen
  moving) within 600 units of a pilot to the nearest pilot's machine (a fifth nearer to change hands, not twice in
  1.5 s; TvOwners on each change and every 2 s); the owner's game runs its AI and sends its place ten times a second
  (TvObjects), and the others' copies are puppets: their update skipped (the objects' loop's call jumped over) and
  their place written at each frame's top, carried on by their last two reports.  Static objects (installations,
  turrets) run in every game, at every machine's own pilot.
* **Who decides the owners.**  The host, while it is settled outdoors in our level (flying it, out of the tunnels, for
  2 s); while it is not (in a tunnel, dead, just out of a tunnel) the lowest seat that is (its TvOwners relayed by the
  host).  A pilot settled nowhere is given no objects and reports none; a pilot new to the place is sent the owners at
  once (it has forgotten them: its table is the place's as it was when it left).  Measured: the host into the tunnel,
  the guest "we decide the owners" and flying them all; the host out, "we decide the owners (7 of 161 ours)" 2 s
  later, the guest's word taken meanwhile.
* **New objects are made once.**  Three ways the game adds an object in flight: the level's reinforcements
  (0x23E40, from the objects' loop: every 4 s, while at most 1/2/4 hostiles by difficulty are awake near the player
  -- the count `[0xDAFC8]` made each frame from the objects within 5 of the game's 16-unit cells -- a craft of a
  type flagged at `+0xE4` put 48 units about the player), a launcher's craft (class 21, 0x27490: every 4 s once its
  last craft left the pad) and a generator's (class 41, 0x28350: its type's `+0x64`, every `+0x6C`).  Each game ran
  them for its own pilot, and a craft made in one game was fought by one pilot only.  Now: a launcher or a generator
  makes one only in the game of the pilot nearest it, and the reinforcements only in the game of the lowest seat
  among pilots whose awake squares overlap (11 cells: a pilot apart has its own, as alone); the others' turns are
  taken and skipped (the hook goes to the function's end).  The game that made one tells its slot and its whole
  0xB4-byte record (TvSpawn: no pointers in it -- copied from its type's first object, then 0x22F40's random AI
  values); the others write it at that slot (filling any gap with dead slots), owned by the maker's machine until
  the owners are decided.  Two games that made one each in the same slot at once: the lower seat's keeps it, the
  other moves its own to the table's end and tells it again.  A pilot who starts our level (a retry, late, out of a
  tunnel) is sent the made objects still alive with the objects gone.  The shareware's four levels have no
  launchers or generators (counted with `!tvcraft`): its new craft are the reinforcements; the launchers' and
  generators' gates are for the registered game, untested.  Measured: pilots apart, the guest's game made craft 476
  and 477 near its pilot and the host's wrote them, 94 craft in both; together, only the host's made them ("left to
  HOST's game") and every awake craft was flown by one machine, a puppet on the other.
* **An ally brings more hostiles.**  The user, 2026-11-14: "Were we able to up enemy spawns due to ally?".  The game
  that reinforces a group of N pilots reinforces it as N: its cap on the hostiles awake times N (the count `[0xDAFC8]`
  divided by N, rounded up, before its difficulty's test: a hook at 0x23E75) and its 4 s clock wound N times as fast
  (0x23EA6: `[0x680F4]` given N-1 more quarters of the frame time), each new craft put about the next pilot of the
  group in turn (moved by that pilot's offset from ours, as high above it as it was above ours).  Measured over 130 s
  of flight with the host hunting: alone none came; with a wingman 7, alternately about the host and the guest
  ("the level's reinforcements about GUEST"), the guest's game writing every one.
* **Enemies' shots are seen.**  The enemies fire through the same call (0x19080, owner 1, from the AI's fire at
  0x24A96, some 2.7 a second in a fight); a craft fires only on the machine that flies it (a puppet's update is not
  made), and its shots are told (TvShot with `owner`) to the pilots in our place and remade there as the enemy's: they
  fly there as here and can hit that pilot (a stray shot at the host hits a wingman in its path).  Their hits on
  objects, if any, are the firing machine's to tell (skipped like any remade shot's).  Measured: the guest on the
  host's wing, 93 and 246 enemy shots sent, 92 and 251 remade; the guest's shield (no god mode) 65535 down to 34815.
* **The tunnels.**  A tunnel's mouth is a hole in the ground (its entrance test 0x42490 from 0x42540: within a unit
  under the mouth's plane -- the ground's slope there -- inside 0.7 of its half sizes, diving more than 45 degrees);
  in it the object table is the tunnel's (the outdoor objects saved to enemy.sav/enemy2.sav, restored on the way
  out: the tunnel runs along z, out past `[0x64CF4]` 16-unit steps).  Each pilot reports its place (TvState.space: 0
  outdoors, 1 + the tunnel's number); shots, hits, objects, owners and the catch-up go only between pilots in the same
  place, and a pilot is drawn only where it is.  The others are told "X flew into the tunnel" / "came out of the
  tunnel" in the game's letters; in a tunnel each game flies the tunnel's craft itself.  Measured: the host dove into
  artic.lvl's tunnel by the game's own entrance and out; both pilots in it together, each drawing the other the whole
  512 units, the guest's 61 shots remade in the host's game, both out by the game's own exit; the host back outdoors
  told the 2 objects the guest had shot meanwhile.
* **The afterburner shows.**  A pilot whose afterburner fuel is going down reports `boost`; the others draw a flame
  behind its ship: a trail of three of the game's own `fireball.bin` (loaded once a level), smaller and smaller,
  flickering in size and place.

## 6. Where this stands, and what comes next

Done (step 1-2, 2026-11-14): the program found and watched, the room (TCP and the lobby's rooms), each pilot's ship
drawn in the others' games, shots remade.  Measured: two machines on one host over TCP, the guest 3 s behind on the
same route sees the host's TV-202 some 24 world units ahead.

Done (step 3, 2026-11-14): hits and stages through the shooter, moving objects' owners and puppets, the afterburner's
flame, the rigs' pilot, the high-resolution mode in the rigs.

**The mission flown together (step 4, 2026-11-14).**  A guest's campaign call of a level (the level function's
entry, 0x2CDF0, its return address the campaign's) is held, the overlay saying "Waiting for <host> to launch a
level", until the host's level is under way (TvState.in_level: from the level function's call to its return, briefing
and flight and the death's Retry question); then the guest flies the host's level: its index (ebx, `[0xDDD74]`, the
pushed name), the host's file name under that index, the host's difficulty (`[0x64D1C]`, which may decide the level's
objects).  Any pilot's level done (`[0x64CC8]`: the exit flown, or TRINEXT; the level loop then ends as completed and
the campaign goes on) ends everybody's level as done; a guest whose host is in another level for 3 s ends its own and
takes the host's at the campaign's next call.  The statistics screen after a level waits for Space.  A death: the game
asks "Retry Mission (Y/N)"; Y flies the same level again from its start, and a pilot who starts our level after us
(a retry, a late start) is told the objects already gone (TvDead: their hit points none in its game).  Level starts
are counted at the level function (a retry keeps the index): objects, owners and loaded models are forgotten at each.
Measured, two machines over TCP: the guest held then flying the host's artic.lvl 1.5 s after it; the host's exit
ending both levels, both then on artic3.lvl; the guest's death and retry with "1 of 1 objects gone already".

**The other pilots marked (2026-11-14, the user: "I can't identify the other player").**  A ship 8 world units off is some
25 pixels across in the 640 x 480 picture, the same TV-202 the player flies: the emulator's overlay marks every other
pilot in our level, in the cockpit's view (the view word 0; B cycles the outside cameras), with a cyan bracket round
its ship and its callsign and distance above, or a cyan diamond at the picture's edge the way to turn when it is off
the picture or behind.  The projection is the game's: the camera our ship's place and its fixed-point attitude matrix
(columns right, up, forward), 90 degrees across (the visibility test keeps |x| <= z), square pixels in the 640 x 480
mode; checked against the ship in captured frames (`!frame=`).  While a recording is made (WCLAB) each frame's
markers go into it as 'tvmark' events, and scripts/tvnet-video.py draws them (the recorder takes the game's picture,
before the overlay).  Measured, both ways with the other pilot holding fire: the guest's screen shows the bracketed
host and the host's bolts ahead of it, the host's screen the bracketed guest and the guest's bolts.

**The others drawn smoothly (2026-11-14, the user: "the other ship is kinda bouncing around ... visual smoothing of the
locations at the other end. We don't need to alter physics").**  A pilot's reports come some 15 a second, unevenly, and
its ship was drawn where the last report and its rates put it, jumping at each report; its angles were carried on by
the ship's own turn rates (about its own axes, not its angles': a banked turn wobbled).  Now the drawn place is carried
on each frame by the ship's velocity and its angles' rates (from its last two reports), then eased towards where the
latest report puts it by a share of the gap (time constant 0.12 s); a gap past 20 units (a level's start, a retry) is
closed at once.  The puppets (craft another machine flies) the same, by their last two reports.  Only what is drawn
and where puppets are put: the game's physics untouched.  Measured (WCNET_TV_SMOOTHLOG=1, 697 frames of the other
ship flying straight and turning): frame-to-frame change of its drawn speed median 2.47 -> 0.44 units/s, 95th
percentile 8.14 -> 2.08; of its heading's rate, 95th percentile 15,719 -> 2,141; drawn within 0.06 units of the
unsmoothed place (median; 0.42 at most).

**Talking (2026-11-14).**  In flight the 0 key (no key of the game's: 1 to 7 pick weapons) opens the emulator's comms
prompt; the line goes to the room (Chat, the host relaying) and shows on every screen in the game's own big letters,
"CALLSIGN: text", for 5 s (the message call 0x2CC00 its net game shows another player's line with); out of flight it
is the emulator's line over the picture.

**The craft (step 5, 2026-11-14).**  The host's choice for each seat (TvLooks); a model is looked up in the game's
archives first (the loader's own lookup, found by its call's offset in the loader: the loader ends the program on a
name it cannot find) and loaded once a level; one not there, and "planet", is drawn as the level's own craft (its first
craft type's model).  Measured: the host as an IceRider and the guest as a SnowScout in each other's games.

**How many hostiles (2026-11-14, the user: "does this increase the number of bad fliers around?").**  No more than
alone: the level's 92 craft are the same objects in every game, each awake one flown by one machine; awake at once,
1-3 near a pilot alone and near two pilots together (measured with `!tvcraft`); two pilots apart each wake their
own, as each would alone.  The reinforcements were the exception (each game made its own for its pilot: the guest's
game had 96 craft to the host's 93) until they were made once (above).

Next:

6. The registered game: done (section 7).

## 7. The registered game (GOG's installer)

The user, 2026-11-15: "I've purchased terminal velocity called setup_terminal_velocity_1.2_*.exe and I'd like to make
it so that if users drag and drop that into our site that it also works.  Try to reuse as much code from the demo as
possible, ideally all of it--but at least the logic maybe with different tables".

**What it is.**  `setup_terminal_velocity_1.2_(28045).exe` (86 MB, Inno Setup; the page's innoextract unpacks it) is the
CD-ROM edition installed: the same `TV.EXE`, `SETUP.CFG` and sound drivers as the shareware (byte for byte),
`STARTUP.POD` (3.9 MB), `CDROM.POD` (32 MB, 2,349 files: all the levels) in `SHARE.POD`'s place, the episodes' lists
(`SHARE.VOX`, `RETAIL1.VOX`, `RETAIL2.VOX`: Tactical Strike, Heavy Fire, The Mad God, nine, nine and eight levels;
`GEIGER.VOX` the hidden mission), `TVI\` (68 MB of cinematics) and `CDROM.INI` (one line, `.\TVI`: where they are).
GOG runs it with no CD drive, an S3, 16 MB and a Sound Blaster 16; nothing of the page's machine had to change.

**Its program.**  `GAME.EXE` (624,695 bytes; the shareware's 620,459) is another compile of the same source: the code
object 0x4B073 bytes (0x4AC7F), functions moved by -0x20 to +0x190, the data by -0x24, +0x6C or +0x8C.
`scripts/tvsigcheck.py SHAREWARE/GAME.EXE REGISTERED/GAME.EXE` tries it as the emulator tries memory:

* 30 of the 31 anchors' windows are in it once, byte for byte but for the relocated operands and the near calls'
  offsets, and every data operand read from them is a relocated one.  (One window, `remote_draw`, had kept the first
  byte of a call's offset that its end cut off; tvsig.py now masks a cut instruction's offset too.)
* The campaign loop is the one place compiled to another shape: the shareware's keeps the level's index in ebx (and
  in memory), the registered game's only in memory.  The anchor is now the loop's call of the level function, with a
  variant window from the registered program and its own offsets for the index, the count and the names; the level
  hook takes the index from memory in both (the shareware's loop stores ebx there before the call and takes it back
  after: writing ebx was never needed).
* Every natively drawn routine (the nine span routines, the filler, the height lookup, the cockpit copy, the
  dispatch) is in it whole.  They are now looked for (where the shareware's layout puts them, else the nearest copy
  of their bytes: the dispatch first, the rest by its move), and the data they use is placed by stretches: every
  relocated operand of the routines found gives (image address, delta), a new stretch where the delta changes (one
  for the shareware; two for the registered game, +0x6C up to 0x689A0 and +0x8C from 0x7CF14).

Nothing else changed: the hooks, the session and the twins are the shareware's code.  `src/cpu/wcnet_tv_sig.h` is made
from both programs (`scripts/tvsig.py SHAREWARE/GAME.EXE REGISTERED/GAME.EXE`), each anchor checked to be found once in
each.

**Measured** (native, `TVDIR=tvgame-reg`: the installer's files unpacked there):

* One machine, 640 x 240, `WCNET_TV_SPANS=check`: in flight in `artic.lvl` at 62 s by the shareware's route; 870,000
  routine calls compared with the game's own code in 40 s, 0 differed (the shareware beside it: the same).
* Two machines, the host choosing Episode Two (`45:down`), the guest Episode One: both fly `desert.lvl` (the host's
  level's name written under the guest's index, as before); shots, hits, stages and the level's reinforcements told
  both ways (objects 377-379 made by the host's game written in the guest's).
* The registered game's host and a shareware guest in episode one: `artic.lvl` together, 476 objects each, hits both
  ways.  With the host in Episode Two the shareware has no such level (loading it would end the program): the guest's
  game is held at its level call with the line "HOST flies desert.lvl, a level of the registered game: the shareware
  has episode one only" (`tv_level_listed`: the level's name is not in the shareware's one list).

**The craft** (the brief: "Would be nice for the non demo to have it select any of the craft"): twelve more looks after
the shareware's six, the other planets' craft by their briefings' names (the levels' `.DEF` files in CDROM.POD):
Crythanian Shockstar `ddcshp.bin`, WSF Razor-4 `afighter.bin`, TTA Sandstorm `dogg.bin`, TTA Dustdevil `pooh.bin`, TTA
Penetrator `deboss.bin`, OSO Phoenix `lavash.bin`, Ositsho Wedge `triship.bin`, Ositsho Quadram `volship.bin`,
Erigonean Shriek `litecr.bin`, CAF Attack Fighter `bugship.bin`, DTS-1 `podgun1.bin`, XI Coreguard `pincher.bin`.  The
page offers them while its game has `CDROM.POD`; a game without a model (the shareware beside the registered host)
draws the level's own craft, as before.

**The page**: the `tv` entry takes the installer as it takes the zip (detected by `TV.EXE`); `keep` holds `CDROM.INI`
and `VOX.INI` through the installer's junk filter (an `.ini` is otherwise GOG's own).  `ZIP=<the installer>
scripts/web-tv.sh` flies two pages from it; `GUEST_ZIP=` gives the guest another (the shareware).

**A model that is not there (found 2026-11-15, a crash).**  The check before a craft's model is loaded called the
loader's first function, the file's length (0x2AF30: directory, name).  That function asks the archives (0x499B0:
the path's length, -1 when no archive has it) and at -1 asks DOS for the file's size *without looking at whether DOS
found it*: for a file nowhere it returns what was on its stack, mostly not 0.  So a shareware guest of a registered
host flying a registered craft (`dogg.bin`) "found" the model, loaded it, and its game ended: "Unable to open model:
dogg.bin".  The page test had printed TV PAGE OK over it (every check a count made before the game ended; the picture
showed it).  Now the archives are asked themselves (`TV_FN_POD_LENGTH`: the call inside the length function, its
shape checked), -1 or 0 a model not there ("drawn as the level's own craft"); `scripts/web-tv.mjs` fails a run whose
games are not both flying at its end.  The same flaw was there for the shareware alone with a craft its archives
lack.
