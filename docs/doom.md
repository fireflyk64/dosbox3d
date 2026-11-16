# DOOM, played together

The user, 2026-11-15: "Let's try the same coop multiplayer with the mother of all demos. The doom demo. See if it
functions". And, the same hour: "Make it on a separate branch and don't push the website yet until I've had some time
to qc it": the work is on the branch `doom`, not merged, not deployed.

## 1. What it does

* **DOOM's own co-operative game**, for two to four marines, over the page's rooms.
  * Everybody is in the same level at once. The monsters, doors, keys and switches are shared; each marine has their
    own health, armour, weapons and ammunition.
  * A marine who dies presses Space and is back at the level's start with a pistol. The level ends for everybody when
    anyone reaches the exit.
  * `T` talks to the others (the game's own chat). Bullets hurt the other marines (the game's own rule).
* **Nothing of the game is hooked.** DOOM has a network game of its own; the emulator is its network driver
  (section 2). The lockstep, the start-up arbitration, the consistency check ("consistency failure") are DOOM's.
* **The start.** When the host presses Fly every player's game starts. Each launcher waits on a text screen until
  everybody who is in the room is there; then the level begins for all at the same moment.
  * Enter on the host's screen starts at once with those who are ready; with nobody ready it plays alone.
  * Esc on anybody's screen plays alone.
* **The host's skill and level** (the room's mission menu: `-skill`, `-warp`) are everybody's: the host's command line
  is given to every player's game.
* **Nobody joins a game under way** (DOOM has no way in). A player who comes late waits at the launcher for the host's
  next start (the host's page reloaded, Fly again).
* **A player who leaves** is told to the others the way DOOM tells it ("Player 2 left the game"), and they play on.
  * A page closed or reloaded says so as it goes: the others hear in 0.1 s.
  * A browser that dies is found by its link's end, some 16 s later; the others stand still until then.
* **A player who stops answering** (a browser tab in the background stops its emulator) stops everybody: DOOM waits.
  After 12 s the others' pictures say whom they wait for; the game goes on where it was when that player is back.
* **The same files for everybody**: the program and the WADs beside it, names and sizes. A player with others is not
  taken, and both screens say why.
* Rooms of up to 4 (`maxSeats`); room codes start `DOOM-`. Alone (Fly Alone, a room of one) it is DOOM as it is: its
  title screen, its menus, saved games kept by the page (`DOOMSAV*.DSG`).

## 2. The driver (src/cpu/wcnet_doom.cpp, wcnet_doomnet.asm)

* **How DOOM plays a network game.** A driver program stays in memory under it. The two share a block of memory,
  `doomcom_t` (DOOM's d_net.h): a command, a node number, one packet, and how many players there are and which one
  this machine is. DOOM is run with `-net <the block's address>` and calls the block's interrupt for every packet:
  "send" and "get". id's own drivers were IPXSETUP.EXE and SERSETUP.EXE; others wrote theirs to the same interface.
* **DOOMNET.COM** is ours: 1,132 bytes on the emulator's Z: drive (`doom_setup`, called where DOS makes its programs).
  * `src/cpu/wcnet_doomnet.asm`, assembled by `scripts/doomnet-asm.sh` (nasm) into `wcnet_doomnet.h`, checked in.
  * It does what a DOS program must: keeps the block's memory, gives the rest back, waits with the machine running,
    runs the game (EXEC), leaves.
  * Everything else is the emulator's. The launcher calls it by the emulator's callback instruction (`FE 38 nn nn`,
    the number written into the file when it is made), function in AX:
    * 0: at its start (its buffers' places). The program is found: its own first argument if that names one
      ("doomnet doom2 ..."), else DOOM.EXE, else DOOM2.EXE. The rest of the arguments are the game's.
    * 1: again and again while it waits, with any key pressed. The answer: wait, run the game, leave.
    * 2: the game is over.
* **The block.** Filled for the game started (section 3): `id` 0x12345678, `intnum` 64h, `numnodes` and `numplayers`
  the players, `consoleplayer` ours, `ticdup` 1, `extratics` 1 (every packet repeats the tic before: one lost packet
  costs nothing). The vector 64h is the emulator's callback while the game runs, and what it was after.
* **The interrupt.** DOOM's node 0 is itself; the other players are its nodes 1.. in the order of their numbers.
  * Send: the packet (`datalength` bytes of `data`) to `remotenode`'s player.
  * Get: the next packet in, with its player's node; or node -1.
* **Alone** (no room, a room of one, Esc): the game is run with its arguments and no `-net`.
* The log every 5 s of a network game: the tics made (35 a second is DOOM's own rate), every other player's seen, the
  packets.

## 3. The room (src/cpu/wcnet_doom_session.cpp)

* The host is seat 0 (the room's, or a rig's TCP listener); a guest opens a stream to it. Messages: wc.proto fields
  100 to 103.
* **DoomHello**, once a second: a launcher's callsign, whether it waits ("ready"), its program and WADs. The host's
  answer: the player's seat, the game under way if any, why a player is not taken.
* **DoomStart**, the host's: DOOM's players in order (the host is DOOM's "key player", number 0; the guests follow by
  seat), their callsigns, the host's command line.
  * Sent when every seat held in the room is ready and has been for 1.5 s; or on Enter.
  * A rig over TCP has no room to ask: it waits for as many guests as `WCPLAYERS` says.
* **DoomPacket**: DOOM's own bytes (its `doomdata_t`, checksum and all), from a player to a player, best effort
  (DOOM asks again for what is lost, as it did over IPX). The host passes on what goes between two guests.
* **DoomGone**: a player's game is over. Its machine's own word when its DOOM ends, the host's when its link ends.
  * The others' games are given DOOM's own exit packet from that player, made here (the exit flag, the checksum as
    DOOM computes it), again every second: one given during DOOM's start-up arbitration is not looked at.
  * A guest whose host is gone has nobody left (every packet came through the host): all the others are gone.
* **The waiting notice** (wcnet_doom.cpp): a player not heard for 12 s, and not gone, is named over the picture.
  `WCNET_DOOM_WAITNOTE=<ms>` for a rig.

## 4. The page

* **The registry** (web/gamefiles.js, `doom`): detected by DOOM.EXE, the build known by "Doomcom buffer invalid!" in
  it; `run` is `doomnet`.
* **The shareware archive**, doom19s.zip as id released it: DEICE.EXE and the pieces DOOMS_19.1 and DOOMS_19.2,
  which joined are a self-extracting zip (15,769 bytes of program, then the zip). `expandSplitZips` now also tries
  pieces named `<name>.1`, `.2`, ...
* **DEFAULT.CFG** (`doomPrepare`): made when there is none (the Sound Blaster for the music and the effects at 220,
  IRQ 7, DMA 1; eight channels; the mouse; the picture at full width). One that came with the game keeps everything
  but the sound cards and the joystick (DOOM calibrates a joystick at every start, and would wait there).
* **The mission menu** (`DOOM_CAMPAIGN`): the five skills, each with the nine levels of episode one. None picked: the
  game's own start, E1M1 at Hurt me plenty. The room's offer in the lobby: "DOOM-4821 E1M3 Ultra-Violence, 2 seats
  free".
* **The mouse**: a click on the picture captures the pointer (`[sdl] autolock=true` in the game's configuration);
  Esc gives it back.
* **A controller** picked in the Controller menu: the stick and the D-pad are the arrows; the buttons are DOOM's keys
  (`DOOM_PAD` in web/wc.js: fire, open, run, sidestep, the map, weapons).
* **Leaving**: a DOOM page closes its room connection on `pagehide`.
* **Free to play now**: a DOOM button; the archive byte for byte in web/demos (2,450,688 bytes, sha256 cacf0142...).
* 50,000 cycles. The picture is 320 x 200, shown 640 x 400.

## 5. The rigs

* `scripts/doomrun.sh [seconds] [key script]`: one machine, headless. `PROG=` (default `doomnet`), `ENV=`.
* `scripts/doomnet-duo.sh [seconds] [host's keys] [guest's] ...`: two to four machines over TCP (`PLAYERS=`), the
  host's `ARGS=`.
* `scripts/web-doom.sh [seconds]`: two to four browser pages in a room of a local lobby server; ends "DOOM PAGE OK".
  `PLAYERS=`, `MISSION=4/2`, `LEAVE_AT=`, `LATE=`, `MOUSE=1`, `DEMO=1`.
* Key-script aids: `!doomtext` (the text screen into the log: DOOM's start-up lines, an error's), `!frame=<name>`
  (the picture: `!shot=` reads DOOM's planar video memory wrongly).
* A held `left` turns half round in 1.6 s; a held `up` crosses the first room in 1.6 s.

## 6. Measured (2026-11-15)

* **Native, two machines over TCP**: 35.0 tics a second on both, each seeing the other's.
  * Three minutes over a simulated link of 40 ms, 20 ms jitter and 5% loss: 35.0 throughout, no consistency failure.
* **Native, three machines** (the guests' packets passed on by the host): 35.0 on all three.
* **A machine killed**: "PLAYER 1 LEFT THE GAME" on the other's screen.
* **A machine stopped for 9 s**: the other's tics stood still, the notice showed, 35.0 again after.
* **Two browser pages** (headless Chrome, one 5-core machine, 50,000 cycles): the emulator at 100% of real time,
  35.0 tics a wall second on both; both marines on both screens.
* **Three browser pages**: 35.0 on all; the host's pick (`-skill 4 -warp 1 3`) run by all three; a page closed: the
  other two told in 0.1 s (16 s before the page said so itself), 35.0 after.
* **The mouse** in a page: a click captured the pointer, the mouse moved across turned the view.
* **A page come late** (the game of two under way): its launcher waited ("HOSTMARINE's game is under way: waiting for
  the next one"), the game went on at 35.0.
* **Alone in a page** (a room of one): the title screen and its demo, a new game from the game's own menu, the page's
  keys walking and firing (DOOM SOLO OK). The free-demo button: fetched once, kept, the game of two started from it
  (DEMOS PAGE OK, `DEMO=1`).
* **Players with other files** (an extra WAD beside the guest's game): not taken; both screens said why.
* **The picture and the sound** at 50,000 cycles: 35 new pictures a second turning on the spot in E1M1 (DOOM's own
  limit; the same at 70,000). The music and the pistol recorded (WCLAB): the level rose from 1,100 to 2,860 while
  firing.
* **Raptor** on the same build, two pages: RAP PAGE OK (nothing of the other games changed).

## 7. Open

* **The registered game, DOOM II**: not tried (no copy here). The driver is the same for any program of DOOM's
  engine with the 1.9 interface; `doomnet doom2` runs DOOM2.EXE. The page's registry has DOOM.EXE only, and its
  mission menu episode one only.
* **Saved games in a network game**: DOOM can load one for all (`-loadgame`, the block's `savegame`); not offered.
* **Three and four players**: a packet between two guests goes through the host, two links instead of one. The
  room's pages are all linked to each other; the guests could talk directly.
* **Deathmatch**: the host's `-deathmatch` on the command line would reach everybody; no menu for it.
* **The picture's shape**: 640 x 400 on the page, as the other 320 x 200 games; DOOM was drawn for 4:3.
* **A late player** has to wait for the host's page to be reloaded and flown again.
