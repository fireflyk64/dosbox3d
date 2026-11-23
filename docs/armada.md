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

- **Keyboard.** Steer with the arrow keys or numeric keypad. **Space** fires guns, **Enter** fires missiles, **Tab** applies afterburners, and the keypad **+** and **−** change speed.
- **Mouse.** Click normally in the menus and on the strategy map. In flight, hold the right button and drag to steer. Release the right button to center the steering. Press the left button while steering to fire guns.
- **Joystick or gamepad.** Connect the controller, press a button so the browser sees it, and pick its **Built in** entry under **Controller** before pressing **Fly**. Calibrate **JOY 1** in the game's Options before entering the in-game network lobby. In Armada, **Alt+O** opens Options from the main menu. [Origin's Armada Play Guide](https://download.wcnews.com/files/arma/Armada%20-%20Play%20Guide%20-%20Text.pdf)

## Network and checks

`ARMADA.EXE` uses the bundled Novell `NETBIOS.EXE` driver over IPX. The browser launcher enables DOSBox IPX for multiplayer and runs the driver before Armada. `PG.EXE` uses IPX directly. Each emulator has its own IPX node, and the room host forwards guest IPX datagrams over the LobbyLink data channels. Armada and Proving Grounds handle their own matchmaking, turns, and combat.

`scripts/armadanet-duo.sh` runs the native emulator pair over TCP. `scripts/web-armada.mjs` runs the browser build against a LobbyLink server. Set `PLAYWRIGHT_DIR` to a directory with Playwright installed, then pass the page URL, the lobby server URL, and the GOG installer. A fourth argument can pass the guest a zip instead. Set `MODE=strategy` or `MODE=arcade` for those routes. The script saves each player's game screens in `SHOT_DIR`.
