# Wolfenstein 3-D, played together

The user, 2026-11-15, after DOOM: "great. Now do wolfenstein"; "that may need the more original techniques"; "put it in
the same branch as doom"; "and make it available by default"; "we will need this technique to also work with the full
version of wolfenstein"; of the GOG installer, "seems oddly windows specific--but maybe the data files can combine with
the demo" (it holds the plain DOS game: nothing to combine); of keys, "either a key will affect it for both players or
each player needs to get their own key ... whatever is easier and most consistent" (a key found is everybody's).
The work is on the branch `doom`, with DOOM's: not merged, not deployed until the user has looked at it.

## 1. What it does

* **Up to four players in the same floor**, the host's: its skill and floor are the room's mission; every player's
  game starts straight in it.
* **You see each other**: the others are B.J. (the running sprites of the episode's ending), a bracket and their name
  over them. They do not block the way and cannot be shot.
* **Enemies.** One goes for the player it notices first and shoots at that player; everybody sees it where it is, and
  anybody's shots hurt it. When it falls it falls in every game: the body, the points (everybody's), what it drops.
* **Shared**: doors (one opened opens for all; it stays open while a player stands in it), pushed walls, keys (one
  player's find is everybody's, and given again to a player who comes back), the lift (one player takes it: the floor
  ends for everybody the game's own way, its tally, the next floor; the secret floor's lift too; the episode won).
* **Each player's own**: health, weapons, ammunition, and the pick-ups lying in the floor (everybody can take each).
* **Killed**: that player starts the floor again from its beginning with a pistol; the others play on, what they
  have killed stays dead, the enemies that were after the killed player go for the nearest other. Lives do not run
  out (the game's TEDLEVEL start takes none).
* **Late**: a player who comes into the room after the start walks into the floor as it is.
* **Out of step**: a guest's game that is anywhere else than the room's floor (its own New Game, a loaded game) is
  sent to the room's by the game's own level warp.
* The shareware (episode one) and the six-episode game (GOG's) both; players need the same one (the two are
  different builds of the program: a guest with the other is not taken, and told so).
* Alone (Fly Alone, a room of one) it is the game as it is: its title, menus, saved games.

## 2. Finding the program (src/cpu/wcnet_wolf.cpp, scripts/wolfsig.py)

* WOLF3D.EXE is a 16-bit real-mode program (Borland C++ 3.1, medium model), packed with LZEXE: what DOS loads unpacks
  itself. `scripts/unlzexe.py` unpacks one for reading.
* The two programs known: Apogee's shareware 1.4 (109,959 bytes) and GOG's (Activision's 1.4, 108,779 bytes). The same
  source built twice, the second for a 386: wherever a `long` is handled the instructions differ, and every address
  differs.
* **Anchors** (`scripts/wolfsig.py` -> `wcnet_wolf_sig.h`, checked in): 21 runs of code both builds have, with every
  byte masked that is a relocated segment, a call's target or a data address. Each must be found exactly once.
  The tool takes both programs and prints every item's value in each.
* **Items**: 50 operands of the found code: the data's offsets (gamestate, player, objlist, actorat, tilemap,
  the doors, the pushed wall's state, playstate, tics ...), the code segments as loaded (the far calls' own), the
  functions called.
* **States**: found in the data segment by what they are: the guard's first (sprite 50, turning, no time), B.J.'s run
  (sprites 408 to 411). An actor's state goes between machines as its distance from the guard's first.
* The program is looked for every 60 ms from its load (found at the first look, 0.2 s); a WOLF3D.EXE whose code is
  not found in 30 s is left alone ("played as it is").
* The source as id Software published it (github.com/id-Software/wolf3d) is the map; the names here are its own.

## 3. The places watched

* **SetupGameLevel's entry**: the host's layer names the room's level; a guest's game is given it (episode, floor,
  skill written into gamestate) before it loads. A guest whose host is not known yet is held there up to 15 s, in the
  PSP's unused bytes ("Waiting for the host's game... (Esc: play alone)").
* **PlayLoop's frame** (after the controls are read, before the doors and the actors): the session's work, and the
  calls into the game made from here, one after another, CPU state only: `DamageActor`, `KillActor`, `PushWall`,
  `DrawKeys`, `GetNewActor`.
* **PlayLoop's end**: playstate says why (1 completed, 9 the secret way, 6 the episode won, 2 killed).
* **DoActor's entry**: passed by for an actor thought for elsewhere and for the other players' own.
* **DamageActor's entry**: a hit on an actor thought for elsewhere is told to its owner instead.
* Fixed in passing: the game never stops a pushed wall at a level's set-up (a player killed while one moved found it
  moving on in the new level). The layer stops it.

## 4. The session (src/cpu/wcnet_wolf_session.cpp)

* The host is seat 0; everything goes through it. Messages: wc.proto fields 110 to 114.
* **The room's level** has a serial number. The host's layer keeps its story: which actors have fallen and where,
  who thinks for which, the walls pushed, the keys found. Any game that comes into the level (the first time, late,
  back from the dead; the host's own too) is given the story: the fallen fall at once without sound or points, the
  walls stand where they ended, the keys are given, the open doors open.
* **Actors** are numbered by the order the level made them (the same map and skill make the same list: a hash of it is
  compared, a game with another is told and plays alone).
  * Free: not yet after anybody. Every game runs it by itself, so that it can notice that game's player; the ones on
    a round are put where the host's game has them with each of the host's reports (the user: "it would be jarring if
    the guards are in different places and one player is like 'go right to avoid the guard' but he's left"): the same
    round at the same pace between reports, so nothing is seen of it.
  * Claimed: the game in which it noticed the player (or was shot) asks; the host says who was first. The owner's game
    thinks for it; its place, state, health and countdowns go out 15 times a second with the owner's own place.
    The others show it between its last two reports.
  * An owner whose frames stop (killed, in a menu, at the floor's end) gives its actors up: the host hands each to
    the nearest player still playing, who takes it over where it stood; with nobody playing it is free again.
* **Events** (reliable, in order): claim, owner, damage, died, door, push, key, done (the lift), out (frames
  stopped), the story asked for.
* **A wall pushed elsewhere** is pushed here by the game's own PushWall (it slides); the tiles it leaves get the
  pusher's area (the game would give them this machine's player's). Where the game will not push it here, it is put
  where it ends.

## 5. The page

* **The registry** (web/gamefiles.js, `wolf`): detected by WOLF3D.EXE; 25,000 cycles; a click captures the mouse.
* **The shareware archive**, Apogee's 1wolf14.zip as released: INSTALL.EXE and W3D1_BBS._1, which is a zip of the
  game's files behind 335 bytes of the installer's own (read by `expandSplitZips` as it is). In web/demos byte for
  byte (765,063 bytes, sha256 309eaa5d...): its VENDOR.DOC allows free unchanged distribution, any description to
  name Apogee. A "Wolfenstein 3-D" button in "Free to play now".
* **The full game**: GOG's installer (setup_wolfenstein_3d_1.4_(58806).exe) dropped as it is: WOLF3D.EXE and the
  .WL6 files are in it (the rest is GOG's own launcher).
* **The mission menu**: four skills, sixty floors (the full game's; the shareware plays the same floor of episode
  one). The command line is the game's own `tedlevel <10 * episode + floor> <skill> nowait`; every page of a room is
  started that way (`roomArgs` when none is picked).
* **A controller**: the stick and D-pad are the arrows, the buttons the game's keys (`WOLF_PAD`, web/wc.js).
* Room codes start `WOLF-`.

## 6. The rigs

* `scripts/wolfrun.sh [seconds] [key script]`: one machine, headless. `WOLFDIR=`, `PROG=`.
* `scripts/wolfnet-duo.sh [seconds] [host's keys] [guest's] ...`: two to four machines over TCP. `PLAYERS=`, `LEVEL=`,
  `SKILL=`, `GUEST_LATE=`, `WOLFDIR=` (`~/dev/doom/wolfgame-reg`: the full game).
* `scripts/web-wolf.sh [seconds]`: two to four browser pages; ends "WOLF PAGE OK". `ZIP=` (the shareware's zip or
  GOG's installer), `PLAYERS=`, `MISSION=2/0`, `DEMO=1`, `LATE=`, `FALLEN=`.
* Key-script aids: `!wolfpos`, `!wolfnear` (the nearest fighters), `!wolfact=<n>`, `!wolfstat` (the session),
  `!wolfdoors`, `!wolfhurt=<n>:<damage>`, `!wolfkeys=<bits>`, `!wolfwarp=<x>:<y>:<angle>` (our player put there), `!wolfgod=1`, `!wolfhp=<n>`; pictures by `!frame=`.
* E1 floor 1: the start is 29.5, 57.5 facing east (0); a guard at 39.5, 61.5; the lift's switch is worked from
  25.5, 47.5 facing east; a pushable wall north of 10.5, 14.5.

## 7. Measured (2026-11-15)

* **Native, two machines over TCP**, shareware and full game alike:
  * both players on both screens, named;
  * a guard the host woke shown on the guest in its place and pose; the guest's shot took it from 25 to 7 on the
    host; its fall, body and dropped clip on both, 100 points each;
  * a door the host opened open on the guest; the host's lift took both to floor 2, in step;
  * the host killed: its guard handed to the guest; the host back in the floor as it was;
  * a wall the guest pushed pushed on the host; a guest come 28 s late found the fallen guard fallen.
* **Guards on their rounds**, a guest come 20 s late: each within 0.1 tile of the host's last report of it (a guard
  0.02, a running dog 0.1), told 15 times a second.
* **The boss floor** (episode 1 floor 9, two machines): Hans Grosse (1,050 health) after the host, shown on the guest;
  the guest's damage (four hits of 300 through its owner) felled him in both games, 5,000 points each; a gold key the
  guest found was the host's too, whose gold door opened in both games; the host walked out: "YOU WIN" on both.
* **Two browser pages**, the shareware dropped, and by the page's own button: WOLF PAGE OK, the emulator at 100% of
  real time, each player on the other's picture.
* **Three browser pages**, GOG's installer dropped, the host's pick "I am Death incarnate": every page's level had
  the skill's 38 actors, a guard the host shot fell on all three; 93 to 100% of real time (the machine busy besides).
* **DOOM** on the same build, two pages: DOOM PAGE OK.

## 8. Open

* **An enemy shoots only at the player it noticed.** Two players side by side: the one it saw first takes all its
  fire. Handing it to the nearer player as the fight moves is not done.
* **The full game's bosses that throw things or change shape** (their shots, Hitler's second form) are made by the
  owner's game only; not tried. Episodes two to six have only been started, not played through.
* **Shareware and full game together** in one room: not taken (different builds). Episode one's maps are the same in
  both.
* **The episode won, or Quit from the game's menu**, ends the program (the TEDLEVEL start has no title to go back
  to): the page shows the DOS prompt; reload to play again.
* **Saved games in a room** are not carried: a guest that loads one is sent back to the room's floor.
* **The picture** is the game's default view size, 240 x 120 in a border; the game's own Change View makes it larger.
