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

`web/gamefiles.js` registers `tv` (detected by `TV.EXE`, run as `tv`, rooms of up to 4, room codes TV-, 60,000 cycles,
the 640 x 480 picture, an S3 and a Sound Blaster 16 at 220/5/1/5, the game's own joystick offered "Built in", saved
games `SAVE*.DAT`, `SCORE.DAT` and `VOX.INI`).  A dropped `3dtv12.zip` is the installer and the split archive:
`expandSplitZips` puts `TV12BBS._1`, `._2`, `._3` end to end and reads the zip in them (`readZip` now takes a zip with
bytes before it, the installer's header), which leaves the game's 41 files.  The registered game's installed directory
is recognised the same way (by `TV.EXE`).

`prepare` (at every launch, after the files are installed): `SOUNDRV.COM` from `SB16.COM` when there is none (what
SETUP.EXE's Sound Setup does for a Sound Blaster 16), `SETUP.CFG` as SETUP.EXE's "Save and launch TV" writes it when
there is none (TV.EXE asks for setup without one), and in `VOX.INI` (made with the game's defaults when there is none:
briefings, cinemas and demos on) the high-resolution mode (640,240) and the control: a pad picked "Built in" (the
launch puts the game port in) makes it the game's Gamepad control (2: the flight applies the stick only above 1)
calibrated as the game's own Calibrate measures DOSBox's counting port (centre 64, 0 to 128); without one, a Gamepad
control left from before goes back to the keyboard (the port is empty), the mouse is kept.  Measured natively with
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

## 4. The rigs

* `scripts/web-tv.sh [seconds]`: two browser pages in a room of a local lobby server (the built page, the shareware's
  zip dropped into each, the host's craft menus, Fly on both, each page's key script from boot): "TV PAGE OK".

* `scripts/tvrun.sh [seconds] [key script]`: one machine, booted, routed into the first level (briefings off in its
  copy's `VOX.INI`): in flight at 62 s.  `COPY=1` a copy of the game per run, `ENV=` more environment.
* `scripts/tvnet-duo.sh [seconds] [host's script] [guest's script]`: two machines over TCP (`PORT`), each its copy;
  `GUEST_LATE` seconds between their starts (the guest flies the same route that much behind: the host is ahead of
  it on the screen).
* Key-script aids: `!tvpos` (the player), `!tvobjs[=range]` (the objects within that many world units: class, name,
  place, hit points, state), `!regs` (the CPU's registers and segment bases), `!dumplin=name:start:length`.
* The rigs' pilot (src/cpu/wcnet_tv_pilot.cpp): `!tvfly=obj:N | near:CLASS[:range] | partner[:right:up:back] |
  head:DEG:DEG | off` flies the ship by writing its attitude (eased turns, banked into them, over the hills to a
  ground target and down onto it), `!tvfire=1|hold|0` (fires when lined up within 140 units, or always),
  `!tvburn=SECONDS` (the afterburner's key held), `!tvfuel=N`, `!tvgod=1` (the shield kept full).
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

**Talking (2026-11-14).**  In flight the 0 key (no key of the game's: 1 to 7 pick weapons) opens the emulator's comms
prompt; the line goes to the room (Chat, the host relaying) and shows on every screen in the game's own big letters,
"CALLSIGN: text", for 5 s (the message call 0x2CC00 its net game shows another player's line with); out of flight it
is the emulator's line over the picture.

**The craft (step 5, 2026-11-14).**  The host's choice for each seat (TvLooks); a model is looked up in the game's
archives first (the loader's own lookup, found by its call's offset in the loader: the loader ends the program on a
name it cannot find) and loaded once a level; one not there, and "planet", is drawn as the level's own craft (its first
craft type's model).  Measured: the host as an IceRider and the guest as a SnowScout in each other's games.

Next:

6. The registered game: its GAME.EXE untested (none at hand); a build whose windows do not all match is left alone.
