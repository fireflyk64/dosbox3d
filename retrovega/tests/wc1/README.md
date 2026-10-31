# The AI against the old game's: how it is measured

The AI's tables (docs/ai.md) are meant to reproduce the behaviour of the space combat of the early nineties, so
this directory flies the old game itself, as a black box, and ours in the same scene, and compares what the
ships did. Nothing here reads the old program's code: the multiplayer layer already knows where its ships' slots
are (`src/cpu/wcnet_ds.def`), and the trace is what any spectator could see.

## The old game, traced

`run-old.sh OUT SERIES MIS "KEYSCRIPT" [SECONDS]` runs it alone under the layer (`WCNET=0`: no network, the mission
picked by `SERIES` and `MIS`, the barracks and the briefing passed by the layer's own keys), with:

- `WCNET_SHIPTRACE=file`: a line a ship slot a frame (`S frame slot entity type side pilot x y z fx fy fz ux uy uz
  vx vy vz speed setspeed aistate aitimer missiletarget parent hull shf shr arf arr arl arrt`), a line a gun
  fired (`F frame ship gun`), a line a hit (`D frame src dst qty`);
- the key script in space (the items are listed in `src/cpu/wcnet_hooks.cpp` above `key_script()`): `a` the
  autopilot, `!rocks=0` no asteroids, `!face=npc` the nose onto the nearest enemy (set at once, not turned), `+space`/
  `-space` the trigger, `@tag` a dump of the data segment (`dsdump.py tag.bin --slots --stats` reads the nav points,
  the mission ships, the slots and the ship stats from it), `!shot=name` a screenshot;
- a callsign with CHEAT in it: the player takes no damage (else the game restarts the mission when it dies, and
  the trace holds two flights).

`trace.py TRACE [--fps N] [--timeline] [--slot S]` splits a trace into flights (the frame count restarts) and
each slot's lives (a slot changes ships at a nav point), and for each AI ship against the player prints its
speeds, turn rates, distances, how much of the time its nose is on the player, when and from how far it fired,
its closest passes, and its AI state words. `digest.py DIR TRACE FPS HULLSUM` (DIR: this directory, for the
reader) prints the same in a line a ship, with every pass and the time until its nose was back on the player, and
the damage by (source, target) pairs: a collision shows as a symmetric pair of ship slots, a shot as a slot of 32
or more. The old game steps 20 times a second; ours 30 (`--fps 30`). The turn rates are angles between the frames'
front vectors, normalised (the vectors are not quite unit length in either trace).

`dump-all.sh OUT` dumps every mission of the campaign at its first second in space: the nav points and the
ships of each (for porting them: `port.py`).

## Ours, in the same scene

`compare.sh` builds rvsim and flies `enyo1.scene` (the first mission's nav 1 as the old game's trace had it at
the arrival: three Dralthi, here Nicanders, 15,000 km ahead and to the left in a loose line, crossing; the wingman
750 km to the right; the player at the 300 setting) twice for four minutes: the player flying straight, and the
test pilot hunting (`--hunt 3`: the nose set onto the nearest enemy every 3 s, as the old rig's `!face=npc` was, the
trigger held), both invulnerable (`--nodamage`), each traced (`--shiptrace`) and digested the same way. `rvsim
--scene FILE` takes `ship TYPE SIDE X Y Z YAW [PROFILE] [KPS]` lines (km from the player, who starts at the origin
facing +z; the profile byte 17 of a mission ship record, docs/ai.md) and `player KPS`. `RV_TACTICS=1` in the
environment has rvsim log every tactic a mind takes (`tactic FRAME: slot S from -> to (target, km, passing)`) in
its `--log` file.

The play tests M5.11 and M5.12 (tests/play/playtest.c, `pt_old_duel`) fly one line fighter against the player in
this scene's geometry, straight and hunting, and hold it to the old game's measure below: the first shot from
1,500-6,000 km, a volley at a time, passes without a collision (one at most when hunted), the nose back on the
player within 15 s of a pass. They run on the host and in DOS, so the envelope is checked at every change.

## What the old game did (Enyo 1, nav 1: three Dralthi against a Hornet, 2026-10-30)

Two runs: the player flying straight at the 300 setting for four minutes, and the test pilot hunting.

| what | the old game, player straight | the old game, player hunting |
|---|---|---|
| speed (km/s of its positions, median / 90% / most) | 466-654 / 808-1,920 / 1,704-2,401 (the afterburner) | 621-785 / 810-1,428 / 2,173-2,425 |
| turn rate, degrees a second (90% / most) | 99-189 / 271-686 | 161-196 / 279-339 |
| nose on the player (share of the time within 25 degrees) | 22-90% | 38-67% |
| first shot | 25-28 s in, at 2,455-5,129 km | 30-32 s in, at 2,748-4,225 km |
| shots | 23-62, 1-2 guns a volley, a volley every 0.75 s while the aim holds | 55-134, the same rhythm |
| passes (closest points under 2,500 km) | 402, 756, 1,840 km (then they fought the wingman) | 52: median 460-670 km, a tenth under 290-390, a tenth over 1,400-2,200 |
| the nose back on the player after a pass | 1.4-4.3 s | median 2 s, a tenth over 7-9 s |
| the turn at a pass | a flat yaw: 20-120 degrees in the next second, no pitch at all | the same |
| the afterburner | a 0.3 s dash (AI state 1) at nearly every pass, 1,500-2,400 km/s | the same |
| collisions | none | none (one pass at 255 km) |
| a tail | one sat behind the player at 466 km/s (58% of its 800), nose on 90% of the time | - |
| AI state words seen | -1 always, 1 the dash, 8 a short slow-down, 9 dying | the same |

Facts of its world that ours now shares, or does not:

- **Its ships are small**: 100 km for the Hornet, 160 for the Dralthi, 120 Scimitar and Salthi, 165 Raptor, 180
  Rapier, 140 Gratha, 240 Drayman, 500 for the Fralthi-class, 700 for the carrier (`gMaybeShipRadius` in the
  dumps). Ours now have the same (`radius_km` in data/ships.csv, a half box of `collide_half`, the sprite drawn
  full size at that width), by the user's call: two fighters pass 250 km apart without touching.
- **Its speed setting is twice its km a second**: a setting of 200 moves 25.6 positions a frame at 20 frames a
  second, 2 km/s a point. Ours moves 1.5 (`KPS_HZ` 20 at 30 steps a second, world.h). The real speeds of the two
  Dralthi-class fighters still come out alike (the old one's 400 setting at 2, our Nicander's 500 at 1.5), but a
  player at the 300 setting flies 600 km/s there and 450 here, which is why ours can keep up with one when tailing.
- **Its afterburner is three times its top speed and instant**: 2,400 km/s within a frame or two. Ours is 2.4 times
  (`ab_kps`) and the player's ramps up at the type's acceleration; an AI's is set at once (`throttle`).
- **Its fighters shoot at the lead point**: their shots were at 0 degrees off a player flying straight ahead of them
  (where the lead point lies on the same line) and 4.8 degrees off a crossing one at 2,045 km moving 264 km/s
  across, which is the lead angle for a 3,300 km/s bolt. Ours do the same (`lead_pct` 100, since 2026-10-30 late:
  read as 0 at first; with 0 our tail never hit a player flying straight where the old's hit 15% of the time, and
  our hunters hit a hunting player 4% of their shots against the old's 13%; with 100, 14% and 19%).

## Ours, tuned to it (the same scene, 2026-10-30)

| what | ours, player straight | ours, player hunting |
|---|---|---|
| speed (median / 90% / most) | 450 (the tail) / 752-794 / 1,524-1,806 | 666-688 / 982-1,001 / 1,280-1,798 |
| turn rate (90% / most) | 150-160 / 225-230 | 160-161 / 224-230 |
| nose on the player | 82-83% (tailing), 24% | 23-33% |
| first shot | 9-10 s in, at 4,422-4,564 km (they start nearer) | 9-17 s in, at 1,492-3,641 km |
| shots | 233-235 (the tail stays in range), 12 | 43-67, a volley every 0.75 s |
| passes | 1,396-2,221 km | 187: median 459-557 km, a tenth under 220-297, a tenth over 677-1,019 |
| the nose back after a pass | 1.4-2.6 s | median 1.4-1.7 s, a tenth over 2.9-4.5 s |
| collisions | none | none (3-17 passes a ship under 260 km, no damage) |

What the tuning set, all in the line fighter's row of `k_profiles` (docs/ai.md) unless said:

- straight at the target at full speed (`pick`: a pursuit in every situation but tailed, passed and hurt), aiming
  at it, not ahead of it (`lead_pct` 0), the guns from 3,000 km (`fire_km`), a volley every 22 steps (`fire_gap`);
- a hard flat yaw away at 900 km when a pass is coming (`break_km`, with the afterburner for its first 10 steps,
  `burst`, at the type's full rate, `turn_pct` 100), then straight out until the target is 2,500 km off or 1.8 s
  have passed (`out_km`, `BREAK_OUT_STEPS`), then the yaw back, over as soon as the nose is on the target. Measured
  against the old game's hunting run (2026-10-30, the scratch measure.py): the old fighters' nose was back 1.9 s
  after a pass at 2,529 km, the next pass 5.3 s on, the nearest two of them 1,746 km apart (median; under 500 km
  4.8% of the time, never a collision); ours before this came round in 1.0 s at 935 km, passed every 2.2 s, flew
  670 km apart (under 500 km 28%) and bumped one another; now 2.5 s at 1,780 km, 4.1 s, 1,633 km apart (under 500
  km 2.3%), no bumps between them in 190 s. The Nicander's afterburner is three times its top speed (`ab_kps` 1590,
  2,385 km/s) as the Dralthi's 2,400 was;
- yaws, with a roll to bring the target level, and does not pitch (`pulls` right and left, `bank` 1: within 20
  degrees of the nose a yaw and a pitch at once for the fine aim), as the old fighters' turns were flat;
- a tail at 55% of its top speed (`tail_pct`: 437 km/s, under a player's 300 setting as the old Dralthi's 466 was under
  the old player's 600; at 60% of the new 530 top it crept up on a straight player and bumped it) behind the target
  within the guns' range;
- attackers of one target spread a hull's width apart (`pass_pct` 170) and steer round anything closing within
  three hull sums (`avoid_pct` 300).

## The ships themselves, and the guns (the user's play report, 2026-10-30)

The same traces say what the ships are made of and how the guns run, and these were set to match (data/ships.csv,
data/weapons.csv; the guns' energy rule is fablevega's M2):

| what | the old game | ours now |
|---|---|---|
| the Dralthi's guns | two (gun indices 0 and 1 in its F lines), 14 points a hit | the Nicander's 2xPH3 at 3 (it had 2xPH2 2xPH3: 20 a volley against the Dralthi's 28 at 14, that is 3.3 times the hurt) |
| the Dralthi's top speed, turn | the 400 setting at 2 km/s a point; 161-196 degrees a second | `max_kps` 530 at 1.5 (was 500), `yaw_dps` 125 x 1.5 = 187 (was 105: 157) |
| shields and armour (front/rear; f/r/l/rt) | Hornet 40/40, 45/40/30/30; Dralthi 50/50, 45/35/30/30; Raptor 60/50, 85/80/65/65 | x 3/14 (a laser hit of 14 is 3 here): Schroedinger 9/9, 10/9/6/6; Nicander 11/11, 10/8/6/6; Gawain 13/11, 18/17/14/14 |
| a shield back from nothing | 15 s (a point every 0.3 s over the Dralthi's 50) | 17 s (`SHIELD_FULL_SHIFT` 9; was 8.5 s) |
| the player's trigger held | 6 shots a second (two lasers, a volley every third of a second) for a minute, 4 in every seventh second | the energy back a point a step, a laser 5 a shot at a refire of 10 steps: 60 shots in 10 s, sustained (M2.10); it was 20 shots in the first second, then 2-3 a second with one gun at a time, the user's "only one gun comes out" |
| the hunting player's hits | 1,184 shots, 80 hits (7%) in 190 s, two Dralthi dead | 500 shots, 27 hits (5%) before this round: the fire rate, not the hit box, was the gap (bolts are segment-tested, shots.c) |
| the wingman, player straight | killed two of the three Dralthi (75 shots) and came home | fired 6 shots in 69 s and died: its row pitched and fled; now the fighter's row (`P_WINGMAN`) |
| what the player flies | the mission's ship: Hornet, Raptor, Scimitar, Rapier by system (the dumps' player record) | each ported mission's `fighter` line (port.py from the dumps); the player had flown the campaign's default, the Redeemer |
| guns at the launch | full | full (`gunEnergy` 100 at spawn; it was 0 for every AI ship) |
| the guns' reach | the Hornet's lasers hit from 3,800-4,000 km (the shot-to-hit pairs), the tailing Dralthi fired from 3,645 km (median) | `range_km` 4.5 for the fighters' guns (was 3.0: the bolts died before a receding target), `fire_km` 4,000 |
| avoidance | the nearest two Dralthi 1,746 km apart, never a collision | `avoid_pct` 350 (with the lead, 300 left the hunted duel with two bumps in 90 s) |
| the hull under the armour | a laser hit of 14 adds 2-4 to the Dralthi's damage points; it died at 12-27 of them: 4-8 hits on the bare hull, 20 or so from one side in all (its slot 2 took 44 hits over 147 s before dying, slot 3 29 and lived) | `hull` 24 (8 hits; it was 10, 3 hits: a Nicander died after 11 hits in one pass of the hunting player, where the old took 15 in ten seconds and lived) |

Still apart: its speed setting is 2 km/s a point
and ours 1.5 (`KPS_HZ`: a player at the 300 setting flies 600 km/s there and 450 here, so the Dralthi that tailed the
straight player at 466 km/s fell behind and turned on the wingman, where ours keep up: the user's flight model's
call); its hunted passes spread wider (a tenth over 1,400 km, ours 1,000); its fighters open fire some 25 s after the
arrival where ours, starting at the arrival's positions, fire at 9 s; the Salthi, Krant, Gratha and Jalthi loadouts
and hides are not measured yet (the Salthi's two guns are known).

## What is still to measure

The other types (the Salthi at nav 3; Krant, Gratha, Jalthi, the capital ships and their turrets in the later
missions: `dump-all.sh` lists which missions have which), the wingman's own behaviour (it flies in formation at 750
km, breaks to attack, chases far), and the AI state word 8 (a slow-down of a second or two: what for).
