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

(Step 7: not yet.)

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
* A build whose code is the same but for where things are (the registered game, if it is so) is found the same way;
  any window not found leaves the game alone ("not a build the hooks know" in the log).  No registered copy was at
  hand: untested.

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

The game's own network play (modem, serial, IPX; up to 4 players, its own `multi%d.lvl` levels): a Doom-like
driver (`COMMIT.EXE`) behind a block whose pointer is at 0x113DF4 (+2 the command, 1 send / 2 receive, +4 the node,
+6 the length, +0x10 the data, a checksum word last); messages of 14 types (a jump table at 0x4DEDC): 1 a ship's
place, angles, speeds and shield (0x4CC30, every 0x2000 of frame time), 2 a shot (0x4E940: the fire call's
arguments), 3 a line, 6 damage to a player, 7 an object's state, 8 a death, 10 a hit on an object... The remote
ships are 0x70-byte records from 0x113C00 (x y z, angles, their rates, alive at +0x30, shield at +0x68), carried on
by their rates (0x4CEC0) and drawn by 0x4E840 with the draw-list call.  The layer does not use the game's net mode
(it turns off the level's goals and loads the net levels): it uses the same calls with the room's reports.

## 4. The rigs

* `scripts/tvrun.sh [seconds] [key script]`: one machine, booted, routed into the first level (briefings off in its
  copy's `VOX.INI`): in flight at 62 s.  `COPY=1` a copy of the game per run, `ENV=` more environment.
* `scripts/tvnet-duo.sh [seconds] [host's script] [guest's script]`: two machines over TCP (`PORT`), each its copy;
  `GUEST_LATE` seconds between their starts (the guest flies the same route that much behind: the host is ahead of
  it on the screen).
* Key-script aids: `!tvpos` (the player), `!tvobjs[=range]` (the objects within that many world units: class, name,
  place, hit points, state), `!regs` (the CPU's registers and segment bases), `!dumplin=name:start:length`.
* The game's files: the shareware unpacked into `tvgame/` (untracked) and set up once (`SETUP.EXE`: Sound Setup,
  Sound Blaster 16, then Save and launch TV).

## 5. The sync design

Everybody in the room flies the same campaign level.  A pilot's ship is reported some 15 times a second (TvState:
x y z, the angles, their rates per second, the shield, the level); the others draw it at the frame's remote-ship
place (where the game's own net mode draws its players) with the draw-list call, carried on by its rates up to a
second past its report, as the model the room chose for that pilot.  A pilot's shots are the fire call's arguments
(TvShot), made again on the others' machines as their own player's shots: they fly and hit the same objects there.

The level's objects are the same on every machine (placed at the level's load, in the same order): an object is
named by its index.  Next (section 6): who flies which awake object (the nearest pilot's machine), the others' copies
puppets (their update skipped, their place from the owner's reports), hits and deaths through the owner.

## 6. Where this stands, and what comes next

Done (step 1-2, 2026-11-14): the program found and watched, the room (TCP and the lobby's rooms), each pilot's ship
drawn in the others' games, shots remade.  Measured: two machines on one host over TCP, the guest 3 s behind on the
same route sees the host's TV-202 some 24 world units ahead.

Next:

3. The objects: owners (the nearest pilot's machine flies an awake object), puppets, hits sent to the owner, deaths.
4. The level: a guest flies the host's level (the campaign loop's call held until the host flies, its index set);
   the host's level done ends everybody's; a pilot's death and "Retry Mission" rejoin the level as it stands.
5. The craft: the host picks each pilot's craft (the TV-202, a level's own, any craft in the game's archives); a
   model not in this level's set and not loadable falls back to the level's own craft.
6. The page: the shareware's zip (the split archive), the registered game's files, `SOUNDRV.COM`, `SETUP.CFG`,
   `VOX.INI` (640x480, briefings, the joystick), the room's options.
