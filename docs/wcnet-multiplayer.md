# Wing Commander cooperative multiplayer (wcnet)

This DOSBox fork hooks the Wing Commander 1 executable (`WC.EXE`) so that two
pilots can fly a mission together: one machine is the server (the wing
leader, slot 0 of its own game) and the other flies the wingman.  This
document is the design reference for the code in `src/cpu/wcnet_*.cpp` and
records how the damage desync that motivated the rewrite was diagnosed.

Every game address below is written the way the IDA database (`wc.idb`,
exported as `WcMulti/wcmulti.idc`) names it, and can be reproduced with
`scripts/wcdis.py` (see "Reading the game" at the end).

## 1. Diagnosis: why damage was not coordinated

### 1.1 The game has two damage models, selected by slot

`do_damage(src, dst, quantity, vec)` is `ovr143:0A99` (reached through the
stub thunk `stub143:0084`, DOS `12D7:0084`).  Its flow:

1. `dseg:00BA` zero and `dst == 0` → the player is invulnerable, return.
2. Subtract the front or rear shield (`dseg:C430`, chosen by the sign of
   `dot(vec, front[dst])`, `ovr140:0924 vector_dot`).
3. Subtract the armor quadrant (`dseg:C85E`, chosen by `dot(vec, right[dst])`).
4. If armor is penetrated: with probability 1/100 destroy the ship outright
   (`delayedDespawn`, `ovr143:1F15`), otherwise apply *internal damage*
   (`ovr143:0EDB`) and if it returns 1 credit the kill (`ovr143:0A08`).

`ovr143:0EDB` starts with `or si,si / jnz`: **slot 0 takes the player path
`ovr143:1273`, every other slot takes the NPC path.**

| | Player model `ovr143:1273` | NPC model `ovr143:0EDB` |
|---|---|---|
| damage points per bolt hit | `max(quantity >> 4, 1)` | `max(quantity / 6, 1)` |
| per point | random system from a table, comm messages, HUD damage | random system 0..8 |
| instant death | system 2 from the front with probability 1/3, or 1/100 critical | system 2 from the front, always |
| hull | core hit points `dseg:C8B8` (4) → `missionStatus = 4` | `dseg:D22C` counter > ship max, or `delayedDespawn` inside `do_damage` |

So the same hit does very different things to "the ship in slot 0" and "the
same ship in slot 1".  In a two-player game each human's ship is slot 0 on
their own machine and an NPC slot on the other, and the old code replayed the
same `do_damage` call on both.  The server's copy of the wingman took NPC
damage and could be destroyed (from inside a replayed call, where the hooks
were inert, so the "ignore wingman death" logic never ran), while the client's
own copy took player damage.  Symmetrically the client's copy of the hero died
under the NPC model while the real hero was fine.  That is the "one player has
extra health" and "wingman explodes" behaviour.

### 1.2 Secondary problems found in the same pass

* The replay passed the shooter's *parent ship* as `src` even on the server,
  so the `entityType[src] == 8` (bolt) checks in `do_damage` never fired: no
  hit sounds, different taunt behaviour, and kill credit through a ship
  instead of a bolt.  (`ovr143:0BC6..0C1D`)
* Hooks were disabled for the whole duration of a replayed call, so nested
  `delayedDespawn`/spawn calls inside replayed damage were neither filtered
  nor synced.  Enemy deaths only stayed in sync because the RNG seed was
  replicated.
* The server still ran the wingman AI (`ovr163:160E`, dispatched from
  `ovr141:28D0`) for a human-flown slot: it fired guns (bolts that only
  existed on the server) and changed AI state on damage.
* Shield recharge (`ovr143:340E`) ticks on `frameCounter % rate`, so two
  machines with different frame phases drift by a point; nothing corrected
  that.
* A client's death was forwarded as a *shared* mission end, ending the
  server's mission with `EndDeath` (and the forwarded status was read after
  it had been zeroed, so a client landing sent `Proceed`).
* The per-instruction hook read ~15 words of guest memory for every emulated
  instruction (`isExecutingOverlay` per hook).

## 2. Authority model

* **The server owns the mission.**  Spawns, despawns, the AI and damage to
  NPC ships happen on the server and are replayed on clients as events
  (`Spawn`, `Despawn`, `WeaponFire`, `Damage` with the RNG seed).
* **Each human's ship is authoritative on that human's machine**, because
  only there is it slot 0.  Damage to it is computed *locally* from that
  machine's own collision detection (victim side), never replayed.  The owner
  replicates the resulting `ShipHealth` snapshot every frame it changes (and
  every 30 frames regardless); everyone else writes the snapshot into their
  copy.  No machine ever runs `do_damage` or a despawn against a remote
  player's ship, at any nesting depth (`wcnet_events.cpp`,
  `on_do_damage_entry` / `intercept_despawn`).
* **NPC health is also replicated** by the server, applied after the frame's
  events have been replayed, so replay drift (recharge phase, RNG) is
  corrected every frame while cosmetic effects still come from the replay.
* The server skips the AI think function for a human-flown slot and refuses
  AI fire from it; the set-speed AI is skipped as before.
* Positions of a player's ship come from its owner every frame; the server
  broadcasts all positions.
* Any ending of a client's own mission (landed, died, ejected, quit) sends
  `PlayerEnd`; the server removes that slot (with an explosion for death,
  ejection and quitting, quietly for landing), broadcasts the despawn and
  continues the mission.  Only the leader's (server's) ending is shared with
  everyone (`MissionEnd`), so the mission ends when the leader lands or dies.
* Clients fire their own guns immediately (`FireJob::PREDICT`) and tag the
  event with `client_seq`; the server replays it, and the echo lets the client
  map the missile slot instead of firing twice.

## 3. Frame exchange

Unchanged lockstep from the original design.  At `seg001:20E3` (top of
`main_loop`) the client sends its `ShipUpdate` (+ events, + health) and blocks
for the server's frame; the server blocks for one message per client in the
mission, merges, and sends one frame to each.  The server sends exactly one
frame per client message so the two sides never drift in message count; the
autopilot and mission-end flushes (`exchange(true)`) still consume one client
message each.  Events are replayed through the trampoline before the game
simulates the frame; the client applies received health snapshots when the
trampoline drains (`on_trampoline_idle`).

## 4. Modules

| file | role |
|---|---|
| `wcnet_memory.h/.cpp` | data-segment map with IDA names, typed accessors, `ShipHealthState` |
| `wcnet_code.h` | stub/overlay/root addresses of every hooked function and location |
| `wcnet_vm.h/.cpp` | interception helpers (`call_arg16`, `return_from_call`), `GameCall` thunks, the `Trampoline` job queue |
| `wcnet_transport.h/.cpp` | TCP framing, `Connection` with per-category queues |
| `wcnet_entities.h/.cpp` | `NetworkShipId`, client `EntityMap`, server `SpawnRegistry` |
| `wcnet_events.h/.cpp` | interception policy for damage/fire/spawn/despawn/AI, replay jobs |
| `wcnet_session.h/.cpp` | `ServerSession` / `ClientSession`: lifecycle, handshake, frame exchange, health replication, chat |
| `wcnet_hooks.cpp` | the per-instruction dispatcher (IP bitmap prefilter) |
| `wc_net.h` | the public API used by the CPU core, the shell command and the GUI |

### Running game code

`GameCall(seg, off).arg(..).invoke()` writes a far-call thunk into an unused
error string in the data segment (`dseg:0395`) and jumps to it with the
current CS:IP pushed as the return address.  The `Trampoline` is a 6-byte stub
(`push bp / mov bp,sp / nop / pop bp / retf`) at `dseg:0187+101`; its `nop` is
a hook point.  `run_instead_of_current_call()` is used at a function entry so
the stub's final `retf` returns to the intercepted function's caller;
`run_before_current_instruction()` pushes CS:IP first.  Each `VmJob::start()`
issues one `GameCall`; `finish()` runs when it returns (this is how the slot a
spawn used is learned).  `return_from_call(ax)` skips an intercepted function
by popping its far return address, the Borland cdecl caller cleans up the
arguments.

### Hook points (`wcnet_code.h`)

| hook | where | purpose |
|---|---|---|
| `do_damage` | `ovr143:0A99` | damage policy + broadcast |
| `fireGunFromShip` | `ovr143:2978` | fire policy, prediction, broadcast |
| `delayedDespawn` / `despawn` | `ovr143:1F15` / `ovr140:1C16` | despawn policy + broadcast |
| `outerSpawnShipEntity` | `ovr145:115D` | spawn broadcast, slot registry |
| AI think / set speed | `ovr163:160E` / `ovr143:0874` | suppress for human-flown slots |
| mission starting / ended / score | `ovr161:0470` / `04DD` / `0251` | briefing handshake, mission end sync |
| main loop top, status checks | `seg001:20E3`, `20F4`, `2108`, `20F2` | frame exchange, client mission-status policy |
| autopilot | `ovr133:0003`, `ovr133:05C9`, `seg001:1695` | camera replication, client key lockout |
| barracks, startup | `ovr150:105D`, `seg001:0512`, `seg001:04F2` | connect handshake, direct mission start |

## 5. Protocol additions (`src/wc.proto`)

* `ShipHealth` inside `ShipUpdate.health`: shields, shield max, armor
  quadrants, damage points, core HP, hull counter, state byte, gun damage,
  engine flag, gun energy (the arrays are listed in the proto comments).
* `WeaponFire.client_seq` for predicted client shots.
* `Frame.player_end` (`PlayerEnd`) for a client leaving the mission.

Old peers are not wire-compatible with these semantics (a client that does
not replicate health would be trusted as authoritative for a ship it never
damages), so run the same build on both machines.

## 6. Running and testing

Server: `WCPORT=13255 src/dosbox -c "mount c wc" -c c: -c wc` (or
`runwc.sh <dospath>`).  Client: same with `WCHOST=<server>`.  The `WCNET`
shell command still starts/stops the server or connects.  Useful environment:

| variable | meaning |
|---|---|
| `WCNET_LOG=0..3` | log verbosity (1 default; 2 per event; 3 full protocol) |
| `MIS=<n> SERIES=<n>` | jump straight into a campaign mission |
| `WCCALLSIGN`, `WCLASTNAME` | override the pilot identity |
| `WCNET_AUTOKEYS=1` | test aid: press Enter through the briefing, then `A` (autopilot) once in space |

`scripts/wcnet-smoke.sh [seconds] [mission] [series]` starts a headless
server and client (dummy SDL drivers), lets them fly mission 1 with
`WCNET_AUTOKEYS`, and prints both logs.  It exercises the handshake, the
mission start state, spawn mapping, autopilot, enemy fire/damage replay and
the client's local damage model.  It does not exercise chat, landing, or a
client death (nothing shoots back headless for long enough).

The default build needs no special flags (`include/setup.h` no longer uses
dynamic exception specifications); `make -j8` after `./configure`.

## 7. Known limitations and follow-ups

* The exchange is still blocking lockstep: both games run at the pace of the
  slower machine plus one round trip.  The next step for smoothness is to
  pipeline it (send frame N, consume the server's frame N-1) with velocity
  extrapolation for remote ships; `ServerSession::exchange` and
  `ClientSession::on_frame_top` are the only places that would change.
* A server with no connected client blocks at the start of every frame until
  one connects (original behaviour); the same happens if the only client
  drops mid-mission.
* A client that died cannot rejoin the mission in progress; it gets the next
  briefing.
* Only two client slots (network ids 1 and 3) are allowed, as before.
* Bolts fired by a remote ship are spawned on the client one frame later
  from the replicated position, so a fast-turning ship's shots can appear
  slightly off.

## 8. Reading the game

`scripts/wcdis.py` resolves any of the address forms used in this document
and disassembles with symbols from the IDC export:

```
scripts/wcdis.py sym do_damage           # ovr143:0A99, stub143:0084, DOS 12D7:0084
scripts/wcdis.py dis do_damage           # symbolized listing until the next name
scripts/wcdis.py dis ovr143:0EDB 0x300   # explicit range
scripts/wcdis.py callers stub143:0084    # every far call site, root image and overlays
scripts/wcdis.py entries stub143         # the stub's thunk table
scripts/wcdis.py names dseg              # all named data
```

Facts the tool encodes that are easy to get wrong by hand: overlay code is at
`FBOV + 0x10 + stub.fileofs` in `WC.EXE`; far-call segment operands inside
overlay code are `8 * segment-index` (index N is IDA's `segNNN` or
`stubNNN`), patched by the overlay loader; IDA segments may start unaligned
(`seg001` starts at 0x3BE8 with paragraph 0x3BE), so offsets are relative to
`paragraph * 16`; DOS segment = IDA segment + 0x1A2.
