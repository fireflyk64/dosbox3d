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
