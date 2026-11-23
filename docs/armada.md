# Wing Commander: Armada in the browser

Open [DOSBox](https://fireflyk64.github.io/dosbox/?game=armada) and drop your GOG offline installer into **Game**. A zip of the installed DOS directory works too. Include `ARMADA.EXE`, `NETBIOS.EXE`, and the game data. `PG.EXE` and `PG.TRE` add Proving Grounds. Each pilot loads their own copy, which the browser keeps locally.

## Armada for two players

1. Pick **Wing Commander: Armada**. Host a two-player room and send its link or code. Once both players are in, the host presses **Fly** to start both copies.
2. In each game, skip the intro with **Esc**. Choose **Multi Player**, then **Net Play**. Press **Y** to accept the default group name and enter separate callsigns. Wait for the in-game lobby to finish initializing.
3. The host clicks **Create**. For the strategy game, enter **A**, then **C** for Confederation or **K** for Kilrathi. Press **Enter** after each answer. For an immediate dogfight, enter **B** and press **Enter**.
4. The guest picks the host's game in the in-game list and clicks **Join**. Once it shows two players, the host clicks **Play**.
5. In strategy mode, the host chooses the sector size. Both players click **Begin** to open their maps. Click **Execute** and confirm **Yes** when ready to end the turn. The game advances after both players confirm. In Battle, each pilot chooses a ship, clicks **Select**, then **Commit** to launch.

## Proving Grounds arena

1. Pick **Armada: Proving Grounds** in the same Game menu. Host a room for two to six players and share its code or link. Press **Fly** when everyone is in.
2. At the title, press **Space** if needed to show the menu. Choose **IPX Net Play** and enter separate callsigns. In the in-game lobby, the host clicks **Create**, enters **P**, and chooses an asteroid density. **N** starts an arena without asteroids. Press **Enter** after each answer.
3. Each guest selects the host's game and clicks **Join**. The host clicks **Play**. Each pilot picks a ship, clicks **Select**, and clicks **Commit**. **T** changes teams on the ship selection screen.

## Controls

- **Keyboard.** Steer with the arrow keys or numeric keypad. A tap turns a little; held, a key builds to the full turn in a third of a second, as in Wing Commander and Privateer. **Shift** with an arrow turns at full at once. **Space** fires guns, **Enter** fires missiles, **Tab** applies afterburners, and the keypad **+** and **−** change speed.
- **Mouse.** Click normally in the menus and on the strategy map. In flight, hold the right button and drag to steer, as a stick would: the further the drag from where the button went down, the faster the turn (64 px is the full turn of an arrow key held). Release the right button to center the steering. Press the left button while steering to fire guns. Dragging up raises the nose; tick **Invert mouse** to steer as a flight stick instead (dragging up, the nose goes down, the arrow keys' way).
- **Joystick or gamepad.** Connect the controller, press a button so the browser sees it, and pick its **Built in** entry under **Controller** before pressing **Fly**. Calibrate **JOY 1** in the game's Options before entering the in-game network lobby. In Armada, **Alt+O** opens Options from the main menu; **JOY 1** asks for the stick at the upper left, the lower right and the centre, each with the button. The page's port counts its readings (`timed=false`), so a calibration holds at any emulated speed, the high resolution's included. [Origin's Armada Play Guide](https://download.wcnews.com/files/arma/Armada%20-%20Play%20Guide%20-%20Text.pdf)

- **High resolution (2x).** Tick it in the room to fly at 640×400 (your own screen only). **Hi-res cockpit** (ticked by default) adds the cockpit: the window's edge smooth, the reticle, the target's brackets and the displays' lines one pixel wide. Unticked, only the space is fine (ships, lasers, explosions, the target's brackets). <kbd>`</kbd> goes through all, the space alone and off while flying. Off unless ticked.

## Network and checks

`ARMADA.EXE` uses the bundled Novell `NETBIOS.EXE` driver over IPX. The browser launcher enables DOSBox IPX for multiplayer and runs the driver before Armada. `PG.EXE` uses IPX directly. Each emulator has its own IPX node, and the room host forwards guest IPX datagrams over the LobbyLink data channels. Armada and Proving Grounds handle their own matchmaking, turns, and combat.

`scripts/armadanet-duo.sh` runs the native emulator pair over TCP. `scripts/web-armada.mjs` runs the browser build against a LobbyLink server. Set `PLAYWRIGHT_DIR` to a directory with Playwright installed, then pass the page URL, the lobby server URL, and the GOG installer. A fourth argument can pass the guest a zip instead. Set `MODE=strategy` or `MODE=arcade` for those routes. The script saves each player's game screens in `SHOT_DIR`.

## The arrow keys and the mouse (src/cpu/wcnet_armada.cpp)

Armada turns at full rate the moment an arrow key goes down, so a tap overshoots the target. Privateer had the same fault. Every flight frame the game builds its pilot's control packet. That packet is the few bytes it sends to the other machines, and every machine steers that pilot's ship from them. Byte 3 is the stick's left and right and byte 4 its up and down, each a 7-bit signed number; the joystick's reading goes in first. Then a loop over the key bindings checks the game's held-key table. Each held key bound to a stick action writes the stop at once: Down +30 in byte 4, Up −30, Right +30 in byte 3, Left −30 (Proving Grounds uses 20).

The emulator watches those four writes and the loop's end. At the end it writes the bytes again, ramped the way Wing Commander's and Privateer's arrows are. A held key starts at a ninth of the stop and adds a ninth every 37 ms, reaching the stop in a third of a second. Shift with the key gives the stop at once. Letting go, or pressing the opposite key, starts the ramp again. Each machine sends the packet after this rewrite, so every machine in a room flies the same ramped turn. Only the keys' writes change; a joystick's reading is the game's own. `WCNET_ARM_RAMP=0` keeps the game's original keys.

| Program | Code segment | Writes A, B, C, D | Loop end | Held keys | Stop |
|---|---|---|---|---|---|
| ARMADA.EXE | 2584 | 14D0, 14EC, 1508, 1524 | 1689 | DS:8E56 | 30 |
| ARMADACD.EXE | 2587 | same | 1689 | DS:8ED6 | 30 |
| PG.EXE | 1B68 | 0E2A, 0E46, 0E62, 0E7E | 0FE3 | DS:7B1C | 20 |
| PGCD.EXE | 1B6C | same | 0FE3 | DS:7C2C | 20 |

Segments count from the load segment. When a program loads, the emulator checks the bytes at each of these places; a build that differs keeps the game's own keys.

Measured natively at 25,000 cycles (about 19 frames a second), holding Right in a solo Gauntlet flight with the ship stopped:

- **Stock game:** 30 from the first frame. A 0.1 s tap moved the view about 70 px in one frame, and a hold about 60 px per 0.1 s from the start.
- **Ramped:** 3, 6, 13, 16, 23, 26, then 30 from the seventh frame. A tap gave 3 and 6, about 12 px of view. A hold built through about 6, 12, 40 and 56 px per 0.1 s to the same full rate.
- **Proving Grounds** (two machines over TCP, in the arena): 2, 4, 8, 11, 15, 17, then 20, for both Right and Up.

When an arrow is let go, the log says how it ramped ("Armada: Right held 0.85 s, 16 frames, ramped +3 to +30"); `scripts/web-armada.mjs` checks that line for the guest's Right.

**The mouse.** Armada uses the mouse only in its menus. In flight, the page's right-button drag steers through the same place, as a stick would. The page sends the drag's offset from where the button went down (`wc_web_arm_steer`, −1 to 1 each way, right and down positive; 3 px dead, full at 64 px). At the loop's end the emulator writes it into the packet in proportion to the keys' stop, on any axis that no key and no joystick moves this frame. Letting go of the button centres it. The left button with the right fires the guns (Space). As with the keys, the packet carries the turn to the other machines. Measured natively with the `!armsteer=x:y` key-script aid: a quarter drag wrote 8 and turned the view about 12 px per 0.1 s, about a fifth of the full turn; a full drag wrote 30; an arrow pressed during a drag ramped its own axis while the drag held the other.


## Twice the resolution (src/cpu/wcnet_armada_hires.cpp)

The user, 2026-11-22: "lets figure out how to double the screen x and y for armada. This is a polygon renderer so it should be scalable"; "It should be toggleable from the browser too"; "We will need to change clip distance so that sub pixel in 320 times will show up when size of a 640 pixel"; and, of the frame rate, "I want to make sure the uprez doesn't change game behaivor at all".

**How Armada draws a flight.** ARMADA.EXE runs in V86 mode under JEMM.OVL, Origin's own memory manager (16-bit protected mode at selector 18h maps EMS-like pages; a 32-bit helper at 98h copies pages and writes the changed dwords of a frame to mode 13h). Every flight frame the frame function (seg2B4B) draws into a 320x200 back buffer, a bitmap record (segment, row table, clip rectangle) at DS:[621A], in one section:

| Offset from the section's top (2B4B:061A) | What |
|---|---|
| +00..+0E | the buffer cleared to the frame's colour |
| +11 | the display list (DS:85A4) emptied |
| +25 | setup: every point projected through the view (DS:SI+8, copied to DS:83BA: centre x, y; scale x, y in 8.8), the list built |
| +3B | the list sorted by depth (a radix sort) |
| +51 | the list drawn: each entry's own draw (polygons through the library's filler, 22-byte corners already in pixels) |
| +57..+80 | an optional overlay, then the cockpit and instruments |

The CD build and both Proving Grounds programs have the same section at their own places (each found by its bytes and checked at the first frame; a program that differs keeps its own picture).

**The four quarters.** With the picture on, the section runs four times first, each with the view made a quarter's of a 640x400 picture (centre 2c less the quarter's corner, scale doubled, the clip rectangle likewise), and each time the buffer is kept as that quarter. Then it runs once exactly as the game runs it, its view and its state, so everything the frame leaves behind is the game's own. That pass's scene is kept, the buffer filled with colour FF (neither cockpit draws it), and the cockpit drawn over it as usual: where FF is left the 640x400 scene shows, anywhere else the game's pixel doubled. The buffer then gets the game's scene back under the cockpit, so the screen's memory holds exactly the game's frame. The picture is shown through the VGA override (Privateer's seam), from the first frame drawn so until the screen shows anything else (a menu, the end of a battle).

**What a draw changes is put back.** Some draws change their own object each time they run, which four extra passes would multiply: the star field moves its stars by the camera's motion (a resource of 12-byte positions), keeps its last camera place, and adjusts its count of stars by the frame rate; an explosion piece counts its frame on (+32). Before each pass after the first these are as they were before the frame, so only the game's own pass changes them. Found by comparing the whole emulated memory after passes 0, 1 and 2 (`!armside`) and by reading every kind of draw met (`WCARMHIRESDRAWS=1` logs each new one): what still differs between passes is the picture, per-vertex screen positions (rewritten by the game's pass), scratch, stacks, page tables and interrupt-time state (the music driver, the network, the timer).

**Drawn here in the quarters.** The far-off sprites (the sun, the planets) are drawn by the game at a fixed size: in the quarters each pixel is drawn 2x2. The library's turned-and-scaled shapes (explosions) are drawn here too, at the doubled scale and clipped to the quarter (the game's skipped one whose middle lay in another quarter: a straight cut down the picture).

**The draw range.** Armada draws nothing beyond DS:8646 (1,980 of its 5 m units, 9.9 km): a fighter comes in at 16 pixels wide. For the quarters it is 16 times as far (`WCARMHIRESFAR`), so ships come in when they are a few fine pixels; the game's own pass, targeting and the HUD keep the game's range.

**The frame rate is the game's.** Armada holds its own frame time: after each frame it waits for 375 units of its timer (about 58 ms here, 17.1 frames a second), and in a room the simulation's rate is the session's. The four quarters take about three times the scene's work, so while the picture is 640x400 the emulated CPU gets three times the cycles (`WCARMHIRESCYCLES`; the game on a faster PC, waiting longer), and that wait is skipped ahead 0.05 ms at a time instead of spun (less than its timer's unit: it ends on the same unit). Measured in a two-machine battle with a missile, guns and the kill: 17.1 frames a second throughout, the passes 18-20 ms of the 58.3 ms frame (27 at the explosion); on the page (headless Chrome here) 17.1 frames a second and 1.00 emulated second a second for both pilots, in Armada and in Proving Grounds.

**Three levels, as Privateer's** (the user, 2026-11-22: "the same three options as in priv. One with higher res everything. One with just high rez laser and fighters but cockpit low Rez and one all low Rez"; "Then we can use the same reticle and lock artwork with thin lines"). The cockpit's stage draws through the library's line (bitmap record, x0, y0, x1, y1, colour), its shape in one colour (the reticle at the view's middle, and the text) and its turned-and-scaled shape (the target display's ship). At *the space alone* (1) a line drawn on the space (a third of its pixels or more on the marker: the target's brackets; the reticle, drawn first, crosses their pieces) is one fine pixel wide, the rest the game's. At *all* (2) every line is, the reticle is line art (Privateer's: its pixels joined, its rings fitted round; src/cpu/wcnet_armada_art.cpp, copied from wcnet_priv_hires.cpp), the target display's ship is drawn at the fine pixels, and the window's edge is smoothed (the window blurred 3x3 and sampled at each fine pixel). These are drawn into an overlay laid over the composed picture, and the game's own call is stepped over.

`WCARMHIRES=1` (all), `=space` or `=0` at the launch (the page's two boxes), `wc_web_arm_hires(level)` while flying, <kbd>`</kbd> goes all → the space alone → off → all. Off, none of it is watched. Test aids: `WCARMHIRESTIME=1` (the passes' time a frame), `WCARMHIRESCALLS=1` (the cockpit's stage's library calls), `WCARMHIRESDEBUG=1`, `WCARMHIRESCOLOURS=1` (the colours the cockpit's stage used), `WCARMHIRESMARKER=` (another marker), `!armhires=1|0`, `!armside`. Research tools: `WCNET_ARM_PROF=1` (where the CPU is), `WCNET_ARM_TRACE=<s>` (every call and return of two frames; `WCNET_ARM_PWATCH=<phys>` who writes a byte), `!armdump=<name>` (memory, the GDT, its code), `WCNET_ARM_PACKET=1` (the control packet's stick bytes).
