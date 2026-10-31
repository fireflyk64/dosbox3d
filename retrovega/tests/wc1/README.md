# The AI against the old game's: how it is measured

The AI's tables (docs/ai.md) are meant to reproduce the behaviour of the space combat of the early nineties, so
this directory flies the old game itself, as a black box, and ours in the same scene, and compares what the
ships did. Nothing here read the old program's code until 2026-10-31, when the user asked for its collision and
AI routines to be disassembled (`scripts/wcdis.py` over `wc/WC.EXE`): what they say is in the last section, as rules
and numbers; the multiplayer layer already knew where its ships' slots are (`src/cpu/wcnet_ds.def`), and the trace
is what any spectator could see.

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
- a hard flat yaw away at 900 km when a pass is coming (`break_km`, at the type's full rate, `turn_pct` 100), then
  straight out with the afterburner for the leg's first 20 steps (`burst` x 2: lit during the yaw instead, into the
  pass, it closed at 2,000 km/s and grazed a straight player in 7 duels of 8) until the target is 2,500 km off or
  1.8 s have passed (`out_km`, `BREAK_OUT_STEPS`), then the yaw back, over as soon as the nose is on the target. Measured
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
data/weapons.csv; the guns' energy rule is the type's `gun_energy_steps`, blank a point a step):

| what | the old game | ours now |
|---|---|---|
| the Dralthi's guns | two (gun indices 0 and 1 in its F lines), 14 points a hit | the Nicander's 2xPH3 at 3 (it had 2xPH2 2xPH3: 20 a volley against the Dralthi's 28 at 14, that is 3.3 times the hurt) |
| the Dralthi's top speed, turn | the 400 setting at 2 km/s a point; 161-196 degrees a second | `max_kps` 530 at 1.5 (was 500), `yaw_dps` 125 x 1.5 = 187 (was 105: 157) |
| shields and armour (front/rear; f/r/l/rt) | Hornet 40/40, 45/40/30/30; Dralthi 50/50, 45/35/30/30; Scimitar (type 2: the McAuliffe player's, the sluggish thick one) 60/50, 85/80/65/65 | x 3/14 (a laser hit of 14 is 3 here): Schroedinger 9/9, 10/9/6/6; Nicander 11/11, 10/8/6/6; Franklin 13/11, 18/17/14/14 with two mass drivers (the Scimitar's), and the Gawain (the Raptor, untraced) the same |
| a shield back from nothing | 15 s (a point every 0.3 s over the Dralthi's 50) | 17 s (`SHIELD_FULL_SHIFT` 9; was 8.5 s) |
| the player's trigger held | 6 shots a second (two lasers, a volley every third of a second) for a minute, 4 in every seventh second | the energy back a point a step (ships.csv `gun_energy_steps`, blank), a laser 5 a shot at a refire of 10 steps: 60 shots in 10 s, sustained (M2.19); it was 20 shots in the first second, then 2-3 a second with one gun at a time, the user's "only one gun comes out" |
| the hunting player's hits | 1,184 shots, 80 hits (7%) in 190 s, two Dralthi dead | 500 shots, 27 hits (5%) before this round: the fire rate, not the hit box, was the gap (bolts are segment-tested, shots.c) |
| the wingman, player straight | killed two of the three Dralthi (75 shots) and came home | fired 6 shots in 69 s and died: its row pitched and fled; now the fighter's row (`P_WINGMAN`) |
| what the player flies | the mission's ship: Hornet, Raptor, Scimitar, Rapier by system (the dumps' player record) | each ported mission's `fighter` line (port.py from the dumps); the player had flown the campaign's default, the Redeemer |
| guns at the launch | full | full (`gunEnergy` 100 at spawn; it was 0 for every AI ship) |
| the guns' reach | the Hornet's lasers hit from 3,800-4,000 km (the shot-to-hit pairs), the tailing Dralthi fired from 3,645 km (median) | `range_km` 4.5 for the fighters' guns (was 3.0: the bolts died before a receding target), `fire_km` 4,000 |
| avoidance | the nearest two Dralthi 1,746 km apart, never a collision | `avoid_pct` 350 (with the lead, 300 left the hunted duel with two bumps in 90 s) |
| the units (2026-10-31, the user: within a few percent of the old data) | shields 40/40 and armour 45/40/30/30 on the Hornet, 50/50 and 45/35/30/30 on the Dralthi, 60/50 and 85/80/65/65 on the Scimitar, 300/300 and 240/200/250/250 on the carrier (the traces at spawn); a laser hit 13-14, a dumbfire 24-25, the Dralthi's seeker 41 (the D lines); the hull a counter of hits through the armour, quantity/6 a hit on an NPC and quantity>>4 on the player (at least 1), the Dralthi dead at some 27 of them, the player's core 4 (the layer's notes, docs/wcnet-multiplayer.md) | the same numbers in ships.csv and weapons.csv (the untraced types x 14/3 of what they were); the hull as points: the Nicander 182 (13 laser hits), the Schroedinger 56 (4) |
| the hit volume | the hunting Hornet's bolts hit a Dralthi with their line 200-300 km from its middle (hitvol.py over the traces; a crude line, the shot's carried velocity left out) | a bolt hits within the ship's `radius_km` of its middle (shots.c segment_in_sphere) as well as in its silhouettes; ours hardly hit past 150 km before, and the ships felt small and hard to hit |
| the fighters' inaccuracy | they hit a hunting player with 13% of their shots and a straight one with 28%, firing 545 times in 190 s hunted | an AI's shot scatters up to 4 degrees about the nose (`AI_SCATTER` 18, a hash of the step, the ship and the gun), and it fires when its nose's line passes within 300 km of the lead point (`AIM_MISS_KM`): 13% and 25%, 114 shots hunted |
| the fine aim | the tailing Dralthi's shots at 0.0 degrees off | the steer mapping gave no steer under 3 units of 1024 a turn (1.1 degrees): a tail's shots passed 45 km over a target 3,000 km ahead, every one (4% hits); the fine aim now steers the last degree too (`steer_of`), and a tail holds at 3,000 km (`TAIL_HOLD_KM`), not at the guns' 4,000-km edge where its bolts died on a receding target: 216 hits of 484 on a straight player (the old tail: 34 of 122) |
| the shot's own motion | (unmeasured: the implied bolt speeds from the shot-to-hit pairs fit both ways) | a shot carries the ship's velocity (the fork's gun_fire); the AI's guns test their aim against the lead point as the shot flies, the target's velocity less the ship's (`cos_fire`, `aim_fire`), and let go only within 150 km of it at its range (`AIM_MISS_KM`); within the guns' reach the nose goes to that point too (`aim_at`; beyond it, to the target's own lead point, the old approach), and a turret leads the same way: 72-87 hits on the hunting player in 190 s (the old 73) |
| the hull under the armour | a laser hit of 14 adds 2-4 to the Dralthi's damage points; it died at 12-27 of them: 4-8 hits on the bare hull, 20 or so from one side in all (its slot 2 took 44 hits over 147 s before dying, slot 3 29 and lived) | `hull` 24 (8 hits; it was 10, 3 hits: a Nicander died after 11 hits in one pass of the hunting player, where the old took 15 in ten seconds and lived) |

Still apart: its speed setting is 2 km/s a point
and ours 1.5 (`KPS_HZ`: a player at the 300 setting flies 600 km/s there and 450 here, so the Dralthi that tailed the
straight player at 466 km/s fell behind and turned on the wingman, where ours keep up: the user's flight model's
call); its hunted passes spread wider (a tenth over 1,400 km, ours 1,000); its fighters open fire some 25 s after the
arrival where ours, starting at the arrival's positions, fire at 9 s; the Salthi, Krant, Gratha and Jalthi loadouts
and hides are not measured yet (the Salthi's two guns are known).

## What the code says (the collision and AI routines disassembled, 2026-10-31)

The user asked for the old program's collision and AI routines to be read, to see what sources of difficulty the
measurements had missed. `scripts/wcdis.py dis ovr141:209D` (the weapon hit), `ovr140:1964` (the hit test),
`ovr143:0A99` (the damage), `ovr143:216D` (the AI's weapons), `ovr163:13CF` (the orders), `ovr164:1024` (the
situations), `ovr166:11F8` (the maneuvers) are the entries; the ship table is `dseg:49DC`, 0x6F bytes a type.
What they say, and where we stand:

**Hits.** Once a frame a bolt is tested against every ship: it hits when its point lies within the bolt's radius
plus the ship's of the ship's middle (a box test per axis first, then a cheap length). The radii are a column of
the ship table: Hornet 100, Rapier 120, Scimitar 165, Raptor 180, Salthi 120, Dralthi 160, Krant 140, Gratha 140,
Jalthi 160, the capital ships 240-700, a bolt 10, a missile 20-25. The same rule both ways: the player's Hornet is
the smallest target of all. Ours: `radius_km` in data/ships.csv (Schroedinger 100, Nicander 160) and a segment
test, slightly kinder than the point. Two ships collide within half the sum of their radii (130 km between a Hornet
and a Dralthi); each takes the square of the closing speed in km a frame, halved (two fighters head on at 400 kps
close at 53 km a frame: 1,400 points, death; a 100-kps brush, 18), then they are pushed apart by their masses
(Hornet 125, Dralthi 140, the Claw 10,000). Ours: a touch is 2 points plus the closing speed over 1,600 positions a
step, a dozen head on (`RAM_BASE`, `RAM_PER`: logic.c): far gentler, the user's call.

**Damage.** A hit takes the facing shield (front or rear by the hit's direction against the ship's front), then
the facing armour (front/rear, or left/right beyond 45 degrees of the right axis), then the hull: an unnamed
fighter adds max(1, rest/6) to its damage points and dies at its type's count (Hornet 5, Rapier 6, Scimitar 7,
Raptor 8, Salthi 5, Dralthi 7, Krant 6, Gratha 7, Jalthi 7; a named pilot's ship adds max(1, rest/40)); the player
adds max(1, rest/16) a bolt (rest/32 a collision) and each hit through the armour also rolls a system from a table
by ship. One hit in a hundred that gets through the armour kills the ship outright when the shooter is a fighter
that is neither the player nor the player's selected target. Shields come back a point a side every N frames, N a
type stat: Hornet 5 (6 a second: 40 in 7 s), Rapier 3, Scimitar 6, Raptor 3, Salthi 10, Dralthi 6, Krant 5, Gratha
4, Jalthi 7; ours a full shield in 17 s (`SHIELD_FULL_SHIFT`), two and a half times slower. Gun energy is 100, a
laser costs 7, a neutron 14, a mass driver 9, and it comes back 2 a frame with full shields, 1 while they charge;
the player fires with any energy above zero (it goes negative).

**Bolts.** A bolt's damage is the gun's base less half its remaining life: a laser's base is 25 and it lives 30
frames, so it hits for 10 at the muzzle and 25 at the end of its reach, 14 at a thousand km (the figure the layer
measured); a neutron 30-40 over 20 frames, a mass driver 17-30 over 25. A shot from far hurts more. Ours: 14 flat.
It flies at 160 km a frame (4,800 km/s at 30 a second) along the gun's line plus only the ship's speed along that
line (the dot of its velocity and its front: no sideways drift), for 30 frames: 4,800 km plus the ship's motion.
Ours carries the ship's whole velocity (the fork's gun_fire, kept 2026-10-31). The refire is a countdown on the
ship: the player's lasers every 7 frames (mass drivers 5, neutrons 11), all guns at once; an AI ship every 13
frames, all its ready guns at once. A dumbfire is launched toward the lead point of the shooter's target when it
has one (the player's selected target too): only a ship with no target fires it straight.

**The AI's shape.** A frame runs, per ship: its order (from the mission: attack, escort, patrol, leave...), a
sub-state, and a tactic; in the attack the tactic picks a *situation*, adjusts a *fear* value, picks a *maneuver*
from a table when none runs, and runs it. The situations, in the order tested: a missile of the enemy's in flight
at it; its target dying; the target over 8,000 km off; hit in the last frames (a hit adds 4 to a counter that
falls 1 a frame); the target ahead within 57 degrees and facing away (a tail chase); head on (each within some 40
degrees of the other); the target behind it within 127 degrees, facing it within 32, inside 7,000 km (tailed);
the target nearly stopped (a speed setting under 20); else the plain case.

**Fear.** Each ship has an AI class from its mission record (0-4 the unnamed, 5-16 the named pilots), and two
bytes a class, c (3 for most, 2 or 5 for some, 0 for the carrier) and d (6-10). Fear rises by c when tailed, 2c
under a missile, falls by c in a tail chase, halves when the target dies, falls by d in the plain case; then by
the ship's health (a 0-100 mix of its four armours and its hull count): under 40, +2c; 40-74, +c capped at 28;
75 and over, capped at 7. Fear under 15 picks from the bold column of the table, 15-29 from the wary one, 30 and
over from the third, which is almost always maneuver 21: leave (afterburn away; the order becomes "leave", and
the ship is removed 16,000 km from the player). This is the retreat the layer saw as "maneuver 21, order 5".

**The tables.** (Enyo 1's dump: the Dralthi leader is class 1, its wingmen class 0, the Salthi the same; the
player's wingman class 5, the first named pilot; the player class 13, whose table is empty.) A named pilot (the
wingmen, the aces) has a 9x3x3 table (situation, fear column: a chance in a
hundred, maneuver A, else B); an unnamed Kilrathi class has one of the same shape; an unnamed Confed class a flat
list, from which the plain case takes maneuver 40 with a chance of 60 + 5 x class, else a random list entry. The
Kilrathi classes, bold column, plain case: 40-80% maneuver 40 (close in), else 45 (29 unless the target is
facing it, then 40); target slow: 29 or 37; far: 39; tailed: 17 (yaw 90), 8 (two half loops), 46 (nothing new),
12 (burn, yaw 180) or, class 3 once in fifty, 22 (drop a mine); head on: 45, 46, 41, 29, 31; a tail chase: 45;
hit: 46 mostly, else 17, 38 (random jinks), 29, 36, 11; target dying: 2 (steer clear). The maneuvers themselves
(47, a routine each, chained by their own steps): 40 flies at a point 500 km plus the target's radius behind it
(above it when the target faces it) at attack speed and, within the radius plus 2,000 km with the target facing
away, becomes 26, the tail: nose on the target, the target's speed matched inside three of its own radii, guns.
29: nose on the target, guns. 18: the same with a speed step. 39: close at attack speed, a 5-degree jink four
frames in a hundred. 37: afterburn when under 700 km. 7: break 90 degrees on a random axis and burn. 2: turn
away by 10 or 40 degrees, break 8 when within three radii, burn. The steering is always at the full turn rate
toward the target's present position: no lead at all; the yaw/pitch commands are the angles, the physics clamps
them to the type's rates. The attack speed is a type stat: Hornet 300, Rapier 250, Scimitar 150, Raptor 250,
Salthi 300, Dralthi 230 (the 466 km/s the traces measured), Krant 200, Gratha 200, Jalthi 200; the afterburner
is bursts of 5, 10 or 40 frames when a maneuver asks and the ship is moving.

**Firing.** The attack maneuvers (26, 29, 18, 10, 32) and a reaction to any enemy within 3,000 km run the weapons
routine: every gun whose countdown is up and whose target is within its reach (speed times life, 4,800 km for a
laser, less by the target's own speed when it recedes) fires when the nose is within 46 degrees of the target (a
laser; a neutron 37, a mass driver 32, flak 84). So the fighters do not aim: they spray a wide cone while the
steering brings the nose round, and the hits come as the nose crosses the target (the 13-28% measured). A
missile goes with a chance of one in twenty a frame scaled down by the distance over 7,000 km: a Dart within 14
degrees, a Javelin from behind (the target facing away, within 66), a Spiculum within 66, a Pilum at any angle;
a mine when a tailing target is within 2,000 km behind. The three maneuver calls (26, 29, 18) go through a check
first: the ship the player has selected as a target holds its guns while the player is within 37 degrees in front
of it, unless its AI class is 11. Seen as well as read: with the Dralthi leader poked in as the player's selected
target (`!poke=662C:02`, the player flying straight at nav 1), it fired 2 shots in the 1,557 frames the player sat
within 37 degrees of its nose and 146 in the 3,983 it did not, while its two wingmen fired 92 and 43 shots at the
player in their cones (wc1/enyo1-target2 in the scratchpad; the first try with the T key selected the wingman).

**Against ours.** Matched already: the radii, the shields and armour, the guns' reach, the attack speed, the
retreat, the enemies' hit rates. Apart, and worth the user's ruling: the bolt that hurts more with distance (ours
flat), the shield recharge (ours slow), the bolt that takes only the forward speed (ours the whole velocity),
the lethal collisions (ours gentle), the one-in-a-hundred kill, the 46-degree spray in place of a lead and a
scatter, the target that holds its fire, and the wingmen's own tables.

## What is still to measure

The other types (the Salthi at nav 3; Krant, Gratha, Jalthi, the capital ships and their turrets in the later
missions: `dump-all.sh` lists which missions have which), the wingman's own behaviour (it flies in formation at 750
km, breaks to attack, chases far), and the AI state word 8 (a slow-down of a second or two: what for).
