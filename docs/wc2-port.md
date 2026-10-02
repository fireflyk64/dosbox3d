# Wing Commander II in the multiplayer layer: working notes

Status and findings of the port of `src/cpu/wcnet_*` to `WC2.EXE` (and later
`SO1.EXE`, `SO2.EXE`).  `docs/wcnet-multiplayer.md` is the design reference for
the Wing Commander 1 implementation; this file records what is different in
WC2 and how far the port is.  Addresses are in `scripts/wcexe.py` notation
(`-e wc2/WC2.EXE`); names collected so far are in `scripts/wc2.names`.

## 1. The executables

All five programs (WC.EXE, SM2.EXE, WC2.EXE, SO1.EXE, SO2.EXE) are Borland
C++ 1991 overlay programs with the same layout: MZ image, then an `FBOV`
block (overlay code, and the offset and length of the linker's segment table
inside the image).  `scripts/wcexe.py` reads that table, so no IDA export is
needed: `segNNN` is segment-table index NNN, `stubNNN`/`ovrNNN` an overlay,
`dseg` DGROUP (the paragraph the startup code loads into DX).

| | WC.EXE | WC2.EXE | SO1.EXE | SO2.EXE |
|---|---|---|---|---|
| segments | 178 | 240 | 240 | 207 |
| overlays | 40 (132..171) | 124 (105..228) | 124 | 93 |
| DGROUP paragraph | 1231 | 1976 | | |

A program loaded by DOSBox's shell with nothing else resident starts at
segment 01A2 (WC1's fixed `DS = 13D3` is 01A2 + 1231).  GOG starts WC2 with
`loadfix -34`, which moves it; the hooks therefore take the load segment from
`DOS_Execute` instead of assuming it.

WC2 is the same engine grown: `scripts/wcexe.py -e wc2/WC2.EXE match wc/WC.EXE
<address>` finds the descendants of WC1's functions by instruction shape.
What moved:

| WC1 | WC2 | |
|---|---|---|
| `ovr143` ships, damage, weapons | `ovr114` | |
| `ovr143:0A99 do_damage(src, dst, qty, vec)` cdecl | `ovr114:1128`, **pascal** (arguments pushed left to right, callee pops) | |
| `ovr143:2978 fireGunFromShip` | `ovr114:3847` | |
| `ovr143:1F15 delayedDespawn` | `ovr114:2B69` (and `2B10`?) | |
| `ovr143:1273` player damage model | `ovr114:1CA9` | |
| `ovr145:115D outerSpawnShipEntity` | `ovr116:1CED` | |
| `ovr140:1C16 despawn` | `seg006:1AB7` (root image) | |
| `ovr168` asteroid and mine fields | `ovr134` (`0B2B` = in-field test) | |

Data seen in `do_damage`: entity type word array `dseg:64AA`, parent byte
array `dseg:6392`, shields `dseg:6680` (two words per slot), AI state
`dseg:6752` (9 = dying), damage points `dseg:6306`, ship type **byte** array
`dseg:4B3C`, ship statistics records of 0xF3 bytes (WC1: 0x6F), "player can
be damaged" `dseg:00C0`.

## 2. Running it headless

`wc2 Origin -k s<series> m<mission>` is the developers' mission jump
(`ovr127:09A0` parses the command line: `Origin` enables the rest, `-k` makes
the player invulnerable, `s`/`m` pick the mission and start in the barracks).
From the barracks a left click on the round door at the middle of the screen
(pointer at 0.48, 0.55 of the mouse range) is "Fly mission"; the airlock on
the left is "Exit to DOS".  The briefing conversation plays for about 110
emulated seconds at `cycles=8000` before the cockpit appears; Esc during the
opening sequence of a new game drops back to the title.

The key script works without the WC1 hooks when `WCNET_KEYSCRIPT_FROM=boot`
starts its clock with the program; `!shot=` screenshots work in any mode 13h
screen.

## 3. What works (two native instances, `wc2 Origin -k s1 m0`)

Stage 1 to 4 of the plan below, for a mission the story flies with a wingman:

* **Tables.**  `src/cpu/wcnet_ds.def` has a WC2 column; `scripts/wcmap.py
  wc/WC.EXE wc2/WC2.EXE data` produced most of it by pairing functions and
  voting on the addresses in aligned instructions (`funcs` and `where` do the
  same for code), checked against memory dumps taken in flight.  70 entity
  slots (WC1: 64): right, up, front, position and velocity vectors are five
  consecutive arrays at `dseg:4D26`, `506E`, `53B6`, `56FE`, `5A46`.  Ship
  slots are still 0..9, the player is slot 0 and his wingman slot 1.
* **Hooks** (`load_wc2_code` in `wcnet_game.cpp`): frame loop `seg001:1BED`
  (top of the loop `1CA3`, status tests `1CF2`/`1D07`), the campaign loop
  around a flight `ovr128:02CE` (mission about to load `0486`, flight over
  `04C3`), `do_damage ovr114:1128`, destroy `ovr114:2B10`, fire
  `ovr114:3847`, spawn `ovr116:1CED`, nav point setup `ovr116:1511`, despawn
  `seg006:1AB7`, the per-entity AI dispatcher `ovr141:2CD3`, AI set speed
  `ovr114:0E5A`, autopilot `ovr107` (`0003`, `0667`, key at `seg001:1079`).
  The dispatcher, the session and the event code are shared with WC1; the
  differences are data (`GameParams`, `Loc.pascal`).
* **What was different.**
  * Many WC2 functions are pascal: `call_arg16(fn, n)`, `return_from_call(fn,
    ax)` and `GameCall(fn)` take the convention from the table.
  * WC2 loads a nav point's ship types when the player gets there
    (`ovr116:1511`); a spawn of a type that is not loaded returns a slot and
    creates nothing.  The server's spawns after an autopilot reach a client
    before its own nav check has run, so the client runs the nav point setup
    first (`NavSetupJob`), with the spawns and despawns nested in it answered
    as a client's own.
  * WC2 places a wingman by formation AI, not by a start offset.  The AI is
    off for a human's ship, so the client takes the position the server
    holds for its slot from the start state (this also fixed WC1, where a
    client used to start inside the host's ship).
  * Fighters get their AI from `ovr129:150E`, capital ships from `1F13`:
    the hook is on the dispatcher above both.  On a WC2 client the AI of the
    host's ship is off as well (it would fly formation on the client).
  * Mission ship records are 0x3C bytes (name 20 bytes, type byte at +0x14,
    class word at +0x15: 5 and 6 are asteroid and mine fields); nav point
    records 0x65 bytes with their ten ship words at +0x51.
* **Tested**: start in formation, positions and health in step, three Sartha
  at Nav 1 firing, damaging each other and dying on both machines, autopilot
  to Nav 1, a landing (both play the landing sequence and return to the
  barracks), a wingman's death (both get "You have died... Replay Mission").

Not done yet: chat on the comms display, the mission's outcome for the
debriefing and the campaign path, cloaking, turrets, torpedoes, tractor
beams, in-flight conversations, Special Operations, the browser page.

## 3. Plan

1. Game detection and per-game tables (this is also what Secret Missions 2
   needs): `DOS_Execute` reports the program and its load segment; `ds::` and
   `code::` become tables loaded for the game that is running; calls know
   their calling convention.  WC1 must keep passing its tests.
2. Map WC2's data segment and frame loop; mission start and end.
3. Two players in one WC2 mission: handshake, spawns, positions.
4. Fire, damage, despawn, health, autopilot, mission endings.
5. WC2's own features: turrets (a second player as gunner), missions the
   story flies alone (an invisible drone or a weakened spotter ship for the
   second player), in-flight cutscenes.
6. The browser build: game registry entry, mission menu.
