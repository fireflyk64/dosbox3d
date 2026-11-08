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
or more. The old game steps once a frame, some 15 frames a second on an average machine of its day (the user,
2026-11-02; the measures in this file until then were read at 20, `--fps 20`); ours 30 (`--fps 30`). The turn rates are angles between the frames'
front vectors, normalised (the vectors are not quite unit length in either trace).

`fireangles.py TRACE [FRAME:SLOT,...]` (the user's idea, 2026-10-31) takes every F (fire) event and prints, per
AI slot within 6,000 km of the player, histograms of the shooter's nose angle to the player and of the player's
nose angle to the shooter at the moment of firing, the head-on volleys against the frames spent head on, and the
volleys split by whether the slot was the player's selected target (from the dumps' `dseg:662C`, given as the
frame it was selected from): the check on the held-fire rule read in the code, and on any rule the code reading
missed.

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
| the units (2026-10-31, the user: within a few percent of the old data) | shields 40/40 and armour 45/40/30/30 on the Hornet, 50/50 and 45/35/30/30 on the Dralthi, 60/50 and 85/80/65/65 on the Scimitar, 300/300 and 240/200/250/250 on the carrier (the traces at spawn); a laser hit 13-14 (the D lines; the "dumbfire 24-25, the Dralthi's seeker 41" read here then were missile blasts some way off a ship: every missile bursts for 93 on the ship it strikes, "Missiles' blasts" below); the hull a counter of hits through the armour, quantity/6 a hit on an NPC and quantity>>4 on the player (at least 1), the Dralthi dead at some 27 of them, the player's core 4 (the layer's notes, docs/wcnet-multiplayer.md) | the same numbers in ships.csv and weapons.csv (the untraced types x 14/3 of what they were); the hull as points: the Nicander 182 (13 laser hits), the Schroedinger 56 (4) |
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
over from the third, which is almost always maneuver 21: leave. Maneuver 21 (`ovr165:0699`) looks for a living
capital ship of the ship's own side among the slots (or a mission flag at `dseg:3077`): with one about, the ship's
fear is set to 0 and its maneuver cleared, and it fights on from the bold column (so the Kilrathi never run while
their carrier is in the fight, and a Confed wingman never runs beside the Claw); without one its order becomes
"leave" (5), a Kilrathi counts as fled, and a comm line goes out if it was the player's target. The leave order
(`ovr163:0228`) flies it straight up, or straight down from an odd slot (the world's axis, not the ship's), with
the afterburner in 40-frame bursts while any enemy is within 16,000 km, and removes it quietly more than 16,000 km
from the player; a wingman whose leader is leaving follows the leader instead. The "hit" situation (a hit adds 4
to a counter that falls 1 a frame) neither raises nor lowers the fear: nothing calms a ship under fire. This is the
retreat the layer saw as "maneuver 21, order 5".

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

**Firing.** The weapons routine (`ovr143:216D`) is reached from five places: the tailing attacks, maneuvers 26
(the tail), 29 (nose on the target) and 18 (the same with a speed step), through a wrapper (`ovr165:0F2F`); the
missile dodge, maneuver 32 (a missile homing on the ship: it turns on it and fires at its target within 8,000 km),
and the rare maneuver 10, directly; and the transports' guns (`ovr163:1519`: a Dorkir or a Lumbari fires at any
enemy within 3,000 km). The routine fires every gun whose countdown is up and whose target is within its reach
(speed times life, 4,800 km for a laser, less by the target's own speed when it recedes) when the nose is within
46 degrees of the target (a laser; a neutron 37, a mass driver 32, flak 84). So the fighters do not aim: they
spray a wide cone while the steering brings the nose round, and the hits come as the nose crosses the target (the
13-28% measured). The approach maneuver 40 has no guns at all: it flies at a point 500 km plus the target's radius
behind the target (above it when the target's nose is within 37 degrees of the ship) and becomes 26, the tail,
only within the radius plus 2,000 km with the target facing away by more than 120 degrees; maneuver 45 (the
head-on case for the unnamed classes) is 29 unless the target faces the ship within 37 degrees, then 40. A
missile goes with a chance of one in twenty a frame scaled down by the distance over 7,000 km: a Dart within 14
degrees, a Javelin from behind (the target facing away, within 66), a Spiculum within 66, a Pilum at any angle;
a mine when a tailing target is within 2,000 km behind. The wrapper the three tailing attacks go through is the
held fire: the ship the player has selected as a target (`dseg:662C`) holds its guns while the player is within 37
degrees in front of its nose (cos x 100 over 80 from `ovr140:1454`), unless its AI class is 11; the missile dodge
and the transports' guns do not hold. The scratch `fireangles.py` reads the F (fire) events of a trace with the
shooter's nose angle to the player and the player's to the shooter. In the hunting run (`enyo1-hunt`, nobody
selected: `dseg:662C` was -1) the three Dralthi fired 270 volleys within 6,000 km of the player, 242 of them with
the nose within 37 degrees of the player, and 30 of them head on (both noses within 30 degrees), in 900 head-on
frames: a volley a second head on, where they fire one every 13 frames when they fire. The earlier count with the
leader poked in as the target (`enyo1-target2`, "2 shots in 1,557 in-cone frames") proved nothing: the leader
spent that run 78,000 km off fighting the wingman and fired nothing within 6,000 km of the player. The test is a
run with the leader selected from 12 s and the player hunting it (`enyo1-target-hunt`: the nose onto the nearest
enemy every 3 s with the trigger held, the leader re-selected every 30 s, 230 s): within 6,000 km of the player the
leader fired nothing in the 1,978 frames its nose was within 37 degrees of the player (189 of them head on, both
noses within 30 degrees) and 6 volleys in the 1,152 frames it was not (3 of them in the 37-46 degree band, 3 at
46-60), while its two wingmen, never selected, fired 98 and 105 volleys, 90 and 97 of them within 37 degrees and 7
head on. So the rule holds as read, with no range exception: the ship you have selected does not shoot you while
it is pointing at you, and what hits you in a head-on charge is its wingmen, or it in the band past 37 degrees
(the user's recollection of being fired on head on; the user's own cone, and the hold, make the selected ship the
safe one to charge).

**Built from it (the user's ruling, 2026-10-31: try them all; the held fire and the no-lead cone as flags; the
one-in-a-hundred kill left out as too deadly for a wingman).** The hit volume is the mission's rule or the type's
(missions.txt `rules hit=`, ships.csv `hit`: the sphere, the box of the sphere's volume grown tall for the thin
ships, the silhouettes; the bolt's 10 km and its whole step in every one; docs/engine.md). A shot carries the
ship's speed along the gun's line only, and hurts by its life left (weapons.csv `damage_near` to `damage`: the
laser 10 to 25). The shields come back a point a side every `shield_steps` (the Hornet's 5, the Dralthi's 6). A
collision costs the square of the closing speed in km a step, halved (`rules ram=soft` the old dozen). The AI:
`rules aim=cone` (no lead, every ready gun within the weapon's `cone_deg`, 13 steps apart, the old approach point
and the tail from behind; `aim=lead` the measured lead-and-scatter model) and `rules hold=on` (the held fire; an
ace never holds); the fear model with the class bytes per profile (`fear_up`, `fear_down`, `scared_pct`) or per
mission ship (`fear=c/d`), the leave (up or down, the afterburner, removed beyond 16,000 km of every pilot, not
killed), the run to a capital ship of its side and the fear reset (`T_HOME`); the wingman's own tailed row (a break
or a loop, the first named pilot's table). docs/ai.md has the model; M3.14-M3.15, M5.13-M5.16, M6.3 hold it.

Flown again in this scene under those rules (compare.sh, 2026-10-31): the player straight, the three fighters
fire first from 3,700-4,600 km at 9-10 s, the tail sits behind with its nose on the player 92-97% of the time
and hits a straight player with one shot in four (the old tail one in four: 34 of 122) once its shots scatter as
the old nose wandered (held exactly on, it hit with two in three); the wingman killed one in 119 s and the player's
hull took 3,841 in 240 s, a good part of it the wingman's own bolts through the player's sphere (the old code has no
same-side test either: a wingman's bolts hit its leader). Hunted, the three died at 107, 129 and 16 s (the old
run: two of three by 190 s), the player's hits now hurting 10-25 a bolt; one ram of the player's and one between
the wingman and a Dralthi, both deadly under the old rule (the old hunt: one bump in 52 passes); the steer-clear
break (`avoid_break`) and the avoidance's push in proportion to the way wanted took the fighters' rams of each
other out of the straight run. Still apart: the old approach's spacing (the nearest two 1,746 km apart) and the
old wingman's bumps (none seen).

## The asteroid fields (the code of overlay 168 read, and three flights measured, 2026-11-02)

The user: the fields "show up as a fixed setup of rocks", where the old game's are "a field effect where while inside
rocks spawn with velocity towards you". Read with `scripts/wcdis.py dis ovr168:0890 0x356` (and 0000, 036A); measured by
flying Enyo 1's nav 2 field (`run-old.sh OUT 1 0 "1:!rocks=1,3:!pos=11520000:1792000:-3840000,3.5:@d000,3.7:@d001,..."`,
a dump every 0.2 s) at the launch's 200 kps, stopped (`3.2:backspace`) and on the burner (`3.2:+tab`), and reading the
rock slots of each dump (`entitiesToDespawn` dseg:6610, their positions, velocities and `entityCullStatus`).

- **When:** a field is the current one while the player is within its radius + 4,300 km of its middle (ovr168:0B86,
  every 16 frames; the radii are 5,000 and 7,500 km); as it becomes current, 3 rocks at once (0B13).
- **How many:** 20 slots (dseg:6610). Each frame, with a slot free, a rock is made when random(215) < speed + 30, the
  speed in km a frame (= kps / 10; dseg:39A2): 14% a frame stopped, 26% at 250 kps (0AA7).
- **Where:** 3,050 km ahead of the nose (measured 2,940-3,130), turned at random +-35 degrees across and +-20 up and
  down (measured: +-36 and +-21), plus the stick's deflection x 10 (the turn led); not made when that point is outside
  the field (06A9). Its camera's focal length is 160 (fitted from the rocks' screen x, 2,504 points): +-45 degrees
  across, so the cone is on screen.
- **Which way:** about one in nine (level 0: the pitch -10..4, the yaw within +-19, then 60%; dseg:39A6/39AA hold the
  other levels; dseg:BFF4 was 0 in Enyo 1) flies at where the player will be after T frames, T = 56 + random(15) less
  random(2 |25 - speed|), at least 7; its speed covers the way in T less random(5) frames (measured 33-41 km a frame at
  200 kps, 96 stopped). The rest face the field's middle and are turned a right angle up, down, left or right (their
  ways 89-92 degrees from the middle, every axis), a fifth still, the others 10 to 16 km a frame (100-160 kps; 036A,
  01E8).
- **Gone:** each rock is looked at every 20 frames: past 4,300 km when drawn, past 4,300 down to 3,100 (by the speed
  0..20) when not (0890).
- **Seen:** at 200 kps 20 rocks, 5 to 9 drawn; stopped 17 to 20 drawn; on the burner the player left the field in
  seconds. Rocks are 100 km in radius, the Hornet's own size. Mines (type 0x21) face the player and drift at it at 2 km
  a frame, and close in when near (092E).

Ours (`src/rocks.c`, docs/engine.md "Nav points and fields") has all of this in the old game's own numbers since
2026-11-03 (our ships, view and speeds are its own now), the fields at its size about their own nav points, and its
level (dseg:BFF4, by series: 0 in Enyo, 2 or 3 later), which the code reads at two places: the aimed rock's pitch
window (06F2, dseg:39AA, a pair a level: -10..4, -8..8, -12..8, -8..8; past level 3 none) and its time's base (041B,
dseg:39A6: 56, 52, 75, 73 frames, 65 past 3). Measured flying straight through, invulnerable, at 200 kps
(`run-old.sh OUT 1 0 "1:!rocks=1,3:!pos=11520000:1792000:-1152000,110:@end" 150`, the D lines of the trace a hit of
1,000 or more each): the nav 2 field (7,500 km) 6 and 7 hits, the nav 3 field (5,000 km, with two Salthi) 1 and 2.
Ours, tests/rocks-cross.txt on ENIGMA1: 3 and 9, 1 and 5 (before: 2 and 2, 3).

## At 15 frames a second (2026-11-02)

The user: playtesters "can almost never beat retrovega but can almost always beat wing commander" in the first two
missions, and the old game runs at 15 frames a second. Everything above was read at 20, so the Nicander had been
made 4/3 the Dralthi of a real machine, and some of the old game's per-frame rules ran once a step of our 30
(shield_steps, the cone rule's 13 frames). Now (world.h `WC_FPS`, `WC_FRAME_STEPS`): its stats in ours from its ship
table (top speed S km a frame: 10 S kps, our kps being its unit at 15 frames; a turn of D degrees a frame: 10 D, a
dps flying 1.5 times itself a second; the afterburner three times the top speed; the shield's frames a point), for
every mapped fighter (port.py TYPES): the Schroedinger turns 80/90/80 (was 110/110/180), the Nicander 400 kps and
100/140/100 (was 530 and 125/105/190), the Agis 480 and 140/120/220; a stat counted in its frames is that many times
`WC_FRAME_STEPS` steps; what was measured in seconds at 20 (the guns' refire, the fire gap, the dash, the break's
straight leg, the fuel) is 4/3 as long. A slow turner keeps off other ships by its turning circle (`turn_room`: the
Gratha-like Convolution flew into the carrier 5 times in 16 without it).

The same scene, the old game traced again and read at 15 (`run-old.sh`: straight, and hunting with `!face=npc` every
3 s and the trigger held), against ours (`compare.sh`):

| what | the old game, straight / hunting | ours, straight / hunting |
|---|---|---|
| speed, median / 90% (km/s) | 395-491 / 607-653; 475-565 / 612-1,382 | 486-494 / 601; 600 / 756-850 |
| turn, 90% (degrees a second) | 82-142; 117-143 | 35-144; 150 |
| nose on the player (within 25 degrees) | 22-73%; 41-67% | 90% (all three tail); 27-36% |
| first shot after the arrival | 20-45 s; 25-30 s | 11-21 s; 22-25 s |
| shots a fighter | 23-74 (one fought the wingman); 46-82 | 193-208; 15-87 |
| passes a fighter, the nose back after one | one or none; 7-11, median 2.3 s, a tenth 9-12 s | none; about 25, median 3.3 s, a tenth 7-8 s |

What was still apart was the speed of an attack. The old tail (straight) flew at its attack speed, 348 km/s under the
player's 450, fell back from 1,200 to 8,500 km firing while within some 5,500, then closed at full speed (605) to
4,300-7,500 and fell back again: in range a third of the time. Hunted, it slowed to the same 348 at some 5,500 km head
on and fired its way in. Ours went everywhere at full speed and its tail held at 3,000 km for good. Now (`CATCH_*`,
`tail_pct` as the attack speed; an attack closes in only on a target facing it from beyond its guns' reach, the
afterburner only while closing; off the burner an AI slows at its acceleration, as the old fighters came down from a
dash at some 22 km/s a frame): straight, shots a fighter 41-98, speed median 330-434; hunted, the nose back after a
pass in 1.3-3.4 s, 6-9 passes a fighter in their 64-89 s. And the leave (order 5, ovr163:0228; its burner
ovr143:2F5D, read 2026-11-02) burns in bursts of 40 frames, the next once the ship is down to its top speed: ours held
the burner on and outran a pursuer. With the measured fighter's approach on no afterburner (`ab_pct` 0: the old one
closed at its top speed), straight: 19-46 shots a fighter. Hunted by rvsim's test pilot ours now die in 42-80 s, the
old ones lived 150-190: unexamined. And in the game an enemy wave comes in from 15,000 km (`wave_out`: the old
arrival; ours had been 4,600), first shots some 12 s after the arrival against the old 18-21 (ours close at some
900 km/s, the old ones at 610-675: they angle in).

### The fighters' aim at range (2026-11-02, later)

With the tail falling back as the old one did, a straight player took 256 points in 4 minutes where the old game's took
872: the same damage a hit (21 and 23), the same fire (202 volleys and 220), a third of the hits. By range, the old
tail hit 71% of its volleys from 2,000-3,000 km, 45% from 3,000-4,000, 25% from 4,000-5,000; ours 25%, 13%, 3%: its 4
degrees of scatter (`AI_SCATTER`, tuned when the tail sat at 3,000 km and fired all the time) missed at range. The
bolt's reach was not it (5.4 thousand km for the Photon changed nothing). Now the scatter goes with the range (the user:
"maybe the 4% number should depend on range"; `scatter_of`, the nearest ship of another side within 25 degrees of the
nose): 4 degrees up close, 3 past 1,000 km, 2 past 1,500: 36 hits, 782 points straight (the old game's 38, 872), and
hunted 449 (the old 613; ours die sooner, 42-80 s against 138-180: the Schroedinger's three lasers against the
Hornet's two, and our hunter hitting 7.6% against 3.5%). Going on down to 1 degree past 2,000 and none past 2,500 gave
1,449 points straight: too many.

## Capital ships (2026-11-05)

The user: an Agasicles on CEPHID2 (the old McAuliffe 2) "starts shooting at me my franklin can't take it. I remember
the fire rate of the destroyers were very low.... 1 laser cannon tops. Just enough to take you down a bit but not
enough to do you in, if you were careful. Moreover I think the shields were a bit less fast to recharge", and "carry
over all the stats for capships, so the ox isn't overly strong either and is more like a drayman--same with fralthi and
exeter and all them".

**The ship table** (dseg:49DC, 0x6F a type; its shields and armour are plain words at +5B..+65, the rows being odd
in length; a gun list at +28, a count then five bytes a mount: weapon, mount, side), the capital types:

| type | the old ship | radius | mass | shield frames a point | hull count | top / attack kps | turns | shields | armour f/r/l/rt | guns |
|---|---|---|---|---|---|---|---|---|---|---|
| 6 | Drayman (ours: Ox) | 240 | 400 | 4 | 60 | 150 / 100 | 2/2/2 | 120/120 | 80/80/60/60 | 1 flak |
| 7 | Exeter (Kahan) | 500 | 5000 | 2 | 200 | 200 / 150 | 2/2/2 | 240/240 | 220/200/200/200 | 4 flak, 1 image recognition |
| 8 | Tiger's Claw (Vigilance) | 700 | 10000 | 1 | 560 | 0 | 1/1/1 | 300/300 | 240/200/250/250 | 8 flak |
| 17 | Ralari (Agasicles) | 325 | 3000 | 3 | 90 | 150 / 100 | 2/2/2 | 200/120 | 200/90/180/180 | 6 flak, 1 IR |
| 18 | Fralthi (Anaxidamus) | 450 | 10000 | 2 | 110 | 150 / 100 | 2/2/2 | 270/170 | 280/140/260/260 | 6 flak, 2 IR |
| 21 | a still one (Leonidas) | 400 | 20000 | 4 | 120 | 0 | 0 | 200/200 | 180 all round | 4 flak, 4 friend or foe |

Types 4 and 5 (a Drayman's twin; 70 and 60 counts), 14 (unarmed, 45), 15, 16 and 19, 20 are the rest. Ours (ships.csv)
take them as they are: the speeds x10 kps, the turns x10, the shields and armour, `shield_steps`, the hull's count x 14,
the turrets and launchers; our accel and sizes stay (they were never mapped for the fighters either).

**The guns** (weapon 1B, its row: radius 15, energy 15, life 40 frames, damage 50, speed 150 km a frame, +12 1000:
the flak's numerator). The turret routine (ovr143:284F, from the capital ships' mind, ovr163:15B1): with any enemy
within 5,000 km, for each mount, one chance in two; an enemy of those near picked at random (the next tried when it does
not bear); bearing (ovr143:25EE: the mount's outward line dotted with the way to it, 25 or more for a turret, over 50
and one in 14 for a launcher). The shot (ovr143:26F6): a place about the target, each axis off by
min(1500, rand(max(400, its distance word / 4)) + its speed x16)... either way; a roll of 100: under 40 with the ship's
bolt counter clear, else under 8, a bolt from the mount toward that place, its life the way there less 5 to 12 frames
(spawnShip1B), the counter set to 7 to 16 frames; otherwise a burst there (an entity 0x30) that hurts every ship whose
hull is within 1,000 km of it (ovr143:1DCF): 1000 / k², k 40 past 750 km, 30 past 500, 8 to 25 within, the player
three quarters, at least one; only what is over 1 counts.

**Missiles' blasts** (read 2026-11-07, the user: "do they match wc1 ... or do they do just a pinprick here"). A missile
does not hurt what it strikes itself: hitByWeapon's case 0xB (ovr141:2331) ends it (delayedDespawn), and its explosion
(outerSpawnExplosion, ovr143:1D0D: any entity below a ship's) turns the slot into an explosion type, 0x30 or 0x32, and
bursts for that type's power (kShipStats +0x12): 6,000 for both, whatever the missile (the Dart DF's own 14,500, the
Javelin HS's 13,500, the Spiculum IR's 11,500 and the Pilum FF's 10,500 are not read there). The burst is the flak's
routine, ovr143:1DCF, over slots 0-9, the missile's own skipped and its launcher not: 6000 / k², at most 100 (`min`,
seg002:0000), the player three quarters. So the ship struck (k 8) takes 93 and the player 69: the largest hit of 28 of the 29
bursts in three traces (McAuliffe 3 twice and a missile run: Darts, Javelins and Pilums alike; the 29th burst far
from any hull, 11), and those near less: 49 on a wingman 250 km
from the blast, 10 on the launcher. A dying fighter bursts the same way for its own type's power (ovr143:1CE5: Hornet
and Salthi 4,000, Rapier, Scimitar, Dralthi and Krant 6,000, Gratha 7,000, Raptor and Jalthi 8,000). Ours (logic.c
`blast`, 2026-11-07): the four missiles and the Aera seeker 93 at the place struck (the player 69) and the burst on
the rest at 93 x 64; the fighters' death bursts not built.

**The fighters' missiles** (WC1's stats table, a type's mounts at +0x28: a count, then 5 bytes a mount, the weapon type
first: 0x18 laser, 0x19 neutron, 0x1a mass driver, 0x1c DF, 0x1d HS, 0x1e FF, 0x1f IR, 0x21 a decoy): the Hornet 2 lasers,
2 DF, 1 HS; the Rapier 2 lasers, 2 neutron, 1 IR, 2 FF, 2 DF; the Scimitar 2 mass drivers, 2 DF, 3 HS; the Raptor 2
neutron, 2 mass drivers, 2 HS, 2 IR, 1 FF; the Salthi 2 lasers, 1 DF; the Dralthi 2 lasers, 3 decoys, 2 HS; the Gratha 2
lasers, 2 mass drivers, 1 IR, 2 HS, 3 decoys. In Kurasawa 2 with the player idle its Gratha put 2 missiles into the Ralari
each run (of 95 hits). Ours (2026-11-07): the Lancelot the Rapier's missiles, the Convolution three Aera seekers (the Aera
have the one kind); launched on 70% of its locks (P_ACE) they put some 1.2 into the Agasicles and 1.3 into the player a
flight, 2.7 launched of 12 carried (most of the four die before a lock: 2.5 s in a 26-degree cone within 4,000 km).

**Missiles' knock** (read 2026-11-07, the user: "I remember missiles were important because they spun ships around").
Before the burst, hitByWeapon's case 0xB (ovr141:2348) takes the missile's momentum, its velocity by its mass (kShipStats
+0x06, the missiles' 5, a fighter's 120-200) and gives it, with the way from the struck ship's middle to the missile,
to ovr140:1D32: both into the ship's own frame (ovr140:11F0), their cross product over the ship's inertia by its radius
(+0x26 over +0x04: the fighters' 900-2,000 over 100-180, some 1/9 for every one; the capital ships' 10,000-20,000 over
240-700), onto its three turn rates (dseg:BF42, BD9A, D1AC: degrees a frame).  ship_stats_every_frame (ovr143:067C)
brings each rate back to the one its pilot asks at its angular acceleration (+0x20, +0x22, +0x24: the fighters' 5-14
degrees a frame each frame), sharing the change among the three by how far each is off; the player's rates are its
stick's and are not brought back there (si 0 is skipped), and the player's traces show no spin. Traced (wc1-mis,
wc1-mc3, wc1-mc3h, the S lines' front vectors frame to frame): every one of the 10 direct hits on a fighter (a Krant,
Scimitars) threw it at once, 10 to 41 degrees a frame, slowing some 5 degrees a frame each frame and gone within 3 to 8
frames, up to some 170 degrees off its line (the Krant at frame 599 turned from its course nearly to the reverse); no
laser hit did (209 of them, 13-49), and no capital ship moved. The fit (spinfit.py, scratch): the first frame's turn
= 0.028 x |r x v| x radius / inertia, r the missile's place off the ship's middle in km, v its velocity in km a frame
(6 hits, 0.020-0.040). The old missiles struck its spheres anywhere across them: their lines passed 18 to 150 km from
the middles. Ours (logic.c `knock_start`, 2026-11-07; the user: "The current maneuvering feels right ... maybe have a
6-12 (cus we run at 30) frame mimic script to get the off course feeling"): the steering as it is, and a fighter struck
(not the player, not a capital ship) turned by the old throw (w0^2 / 10 degrees, w0 = 0.028 x b x v / 9, b how far off
the middle by the dice, evenly to its radius) over 6 to 12 steps, slowing evenly, the nose any way and the wings a
little (the exact axis cost 1.2 KB of conventional memory more); M7.11.

The capital hull (ovr143:0F11): a count, a hit past the
armour max(1, rest / 8) on the Kilrathi side, / 10 on the others.

**Measured** (`run-old.sh` on McAuliffe 2, series 2 mission 1: the player put near the Ralari with `!pos`, the two
Krants and, for the clean runs, the wingman killed with `!shoot=SLOT:900`, `!tail=2:2500` each second; the D lines'
sizes tell flak, at most 11 on the player, from the rest):

| the scene | frames | hits on the player | points |
|---|---|---|---|
| still (`backspace` each second), alone | 4,176 | 259: 2 x125, 3 x56, 4 x30, 6 x18, 11 x17 | 950 |
| at 20 km a frame (200 kps), alone | 4,181 | 62, nearly all 2 | 132 |
| at 20 km a frame, the wingman about | 4,106 | 148 of flak, 5 others | 490 of flak |

The code's chances taken every frame make some ten times the bursts measured, and its speed term at x16 leaves no hit
on a pilot at 200 kps where there were 62: a Monte Carlo of the burst's rule fits the still run at 0.07 bursts a frame
and the moving one at some 6 frames of travel. Ours (logic.c): the turrets' turn once in `CAP_TURRET_FRAMES` 10 frames,
`CAP_SPREAD_FRAMES` 6, the bearing 3 in 8 a turret (the mounts are the art's placeholders); M8.44 has a lone Franklin
(smaller than the Scimitar: the bursts count from the hull) at 352 points still and 36 at 200 kps in 1,800 frames,
the old 409 and 57 on its Scimitar.

## The fighters' hulls (the NPC damage routine read, 2026-11-05)

The user: the Aera fighters "are effectively assembled from tissue paper", an Ariston "can be blasted with a few shots"
where the Krant took "precious tens of seconds". Ours spent a hit past the armour on the hull's points (a Nicander's
182: 13 laser hits; an Ariston's 84: 6). The old game's NPC routine (ovr143:0EDB, called from do_damage with the slot,
the quantity past the armour and the shield side) does something else: an unnamed ship's hit is max(1, quantity / 6)
points (a named pilot's max(1, quantity / 40), its last point always the hull), and each point is spent on one of nine
systems by rand(9), the loop going round until the points are spent:

| system | what | spends | only when |
|---|---|---|---|
| 0 | the core counter (C8B8) down one | yes | |
| 1 | something more (0x460:B6) | yes | the hit from behind |
| 2 | delayedDespawn: dead outright | | the hit from behind |
| 3 | both shields' maxima to 0 (the generator) | no | |
| 4 | the hull counter (D22C) up one; dead when it passes the ship table's +0x10 | yes | |
| 5 | a gun of the ship's list destroyed, by the dice | yes | the hit from the front, a gun left |
| 6 | BD0E up one, to 5 | yes | the hit from the front |
| 7 | the engine (the speed set from the table; dead if the ship then has no speed) | yes | the hit from behind |
| 8 | the flag BFD6 set to 0xFF | yes | the hit from the front, once |

The side: do_damage's vector is the bolt's way (hitByWeapon ovr141:209D copies the bolt's velocity; the layer's
`!hit` says so too), its dot with the ship's front over 0 for a bolt flying the ship's own way, that is one that struck
from behind; that picks shield index 1 (the aft one: the table's +5D, the Rapier's 75 behind its 80) and is the
routine's side flag. An earlier note here had the kill "from the front": wrong. The hull counts (+0x10): Hornet 5,
Rapier 6, Scimitar 7, Raptor 8, Salthi 5, Dralthi 7, Krant 6, Gratha 7, Jalthi 7 (the capital ships' 60 to 560 are
counts too, by the other rule above). So from behind a point in five kills a fighter outright (the systems 3, 5, 6, 8
roll again), and a laser hit of 14, two points, kills one in three times past the rear armour: a Dralthi dies to three
such hits, a Krant to three past its thickest side (rear shield 80, rear armour 100: the whaling). Head on nothing
kills outright: the hull's count climbs a point in five or so (the gun, gun damage and flag systems spend while they
last), eight counts for a Dralthi, some twenty laser hits.

Ours now (`fighter_roulette`, ships.csv `hull_hits`; M8.49): the same nine systems with the fixed block's coreHp,
gunDamage, engineFlag, the guns' destroyed bits and SHIELD_MAX, the hull a count of (hull + hull_hits) / (hull_hits + 1)
points, the side the hit's place before or behind the ship's middle (ours keeps no bolt direction in do_damage; a bolt
striking the rear half flew the ship's way). The shields and armour of every mapped type are the table's now (the
Agis 35/35 30/20/15/15, the Ariston 80/80 90/100/80/80, the Convolution 100/95 140/120/100/100, the Cleomenes 160/160
200/100/170/170, the Gawain 70/70 100/90/80/80, the Lancelot 80/75 60/55/50/50), their guns the table's counterparts
(the Krant's two lasers, the Gratha's two lasers and two mass drivers, the Jalthi's six: PH3 for a laser, PH2 for a
neutron or a mass driver). Still apart, the user's call: the Schroedinger's three lasers against the Hornet's two, and
our test pilot's hit rate (compare.sh hunted: the three Nicanders dead at 19, 37 and 46 s; the old game's Dralthi
lived 150-190 s under the same rig, its Hornet hitting 3.5% with two guns against ours 7.6% with three).

## Orders (the mission records' attack orders, 2026-11-05)

The user: in Enigma 2 "the ox is nigh unkillable and the aera minds do not make progress before it can jump out".
The Ox's stats are the Drayman's to the word (above), and alone against Nicanders in rvsim it dies in 88-105 s (one
Nicander or three: they get in each other's way); with the wing about, half the Nicanders raided it and it was at
40% when the wing had killed them. The old game at Enyo 2's nav 1 (run-old.sh 1 1, the dump at 60 s): both Salthi had
the Drayman (slot 6) as their target with the player 8,000 km off and the Drayman further, and in 118 s of their
passes it took 108 hits of 12-24, its shields and three armour sides gone and 39 of its 60 counts (the shields came
back between passes: 3.75 points a side a second). The order is in the mission record: byte +0x29 is the mission ship
a behaviour-2 ship attacks (the spawn, ovr145:108C, copies it to the slot's D3E6; ovr163:0373 makes it the target
while divide_armor, 74 for the four armour sides weighted 23/27/12/12 and 26 for the hull's count, is over 70 - 15 x
min(class, 4), the class byte +0x20); a wingman (behaviour 4) copies its leader's. Of the 44 missions dumped, 72
enemy ships carry such an order, at the Draymen, Exeters, the Claw, a captured Ralari and now and then the player's
own ship. missions.txt has them as `prey=KEY` (54 lines: 32 where the type and nav point matched the dump, 22 more by
hand where the mission's enemies had moved; port.py reads the byte as `target`), logic.c `prey_of` holds the order
while `health_pct` is at least 55 for a fighter (40 heavy, 10 an ace). M8.50.

## The taunt (read and flown 2026-11-07)

The user: "is this the same taunting behavior in WC1... I remember taunting usually didn't work in the original... it
might be too effective now". The comm menu's three taunts are its messages 4-6 (`ovr139:0583`, the switch in
`breakAndAttackInner`): the enemy answers 7 times in 10 (a named Kilrathi ace, pilot byte D1A2 9-12, always), and
then, unless it already has the taunter as its target or is leaving (order 5), `ovr165:1627` sets its sub-state to
6 (attack) and its target to the taunter. No dice for that and no timer. What follows is its order's own thinking,
every frame:

- **A ship that carries the mission's attack order** (behaviour 2, AI order 2, `ovr163:10B7`) runs `ovr163:0373` in
  sub-state 6 too: while its armour is over 70 - 15 x class percent its target is its prey again at once; below it,
  1 frame in 25 sends it back to the prey. Flown (Enyo 2's nav 1, `run-old.sh 1 1`, the Salthi with the order on the
  Drayman given the taunt's state with `!poke=CDE6:06` and `!poke=C286:00` at 33 s and 45 s): back on the Drayman the
  next frame both times.
- **Any other fighter** (the order's wingmen, behaviour 4, and the free ones) keeps the taunter (`ovr163:0329`) until
  it dies, the fighter flees, or a fighter of the taunter's side that has it as its target sits on its tail within
  7,000 km (behind it, nose on it: `ovr165:07F0`), which then becomes its target. Flown: Kurasawa 2's hidden first
  encounter (`run-old.sh 9 1`, four Salthi; T pressed four times selects slot 3, on Bossman), the taunt turned it
  on the player the frame it went out and it stayed 36 s, until it was gone; Enyo 2's order wingman stayed 6 s,
  until Spirit got on its tail.
- **Most were already on the player**: with no order, its pick (`ovr164:04B0`) takes the player half the time when
  the player is within 5,000 km. Kurasawa 2's Salthi: three of four on the player within 10 s of arriving.

So the taunt did not work on what an escort mission sends at the escort: Kurasawa 2's Ralari encounter has four
Gratha (type 12), two with the order on the Ralari and two following them. Ours had every enemy fighter, under orders
or not, turn on the taunter for 20 s whatever else happened; ported the same day (the user: "Port the wc1 rule"):
logic.c `order_hold` (the first ship of a `prey=` line carries the order; healthy it shrugs the taunt off, hurt it
goes back 1 step in 50), `tailer_of` (one on its tail within 7,000 km takes it), no time limit; M10.12.

## The waves at the hidden points (2026-11-07)

The user, testing LIXIS1 (the old game's Dakota 1: an outbound Drayman, then an inbound one with the vaccine): "Can we
check how the wc1 equivalent manages it", then "I'm worried this is a systemic problem ... The hidden nav points are
key". Dakota 1 flown in the old game (`run-old.sh 7 0`, the player idle and CHEAT, autopilot nav to nav): 5 Salthi at a
hidden point halfway to Nav 1 stopped the autopilot 8 s out, three of them under orders on the outbound Drayman, and it
died 19 s later; at Nav 2 the inbound Drayman came 33 s after the Jalthi and died 20 s after it came. Our LIXIS1 had
no such ambush: the record of that point has an EMPTY name, and port.py and places.py skipped a nameless record
(`if not name`), so its ships fell to "the launch", where the briefing was written around them ("We're under attack
here already, and the turrets are off. 5 Agis...") and places.py then dropped them as in no list.

`tests/wc1/waves.py DUMPDIR` checks every mission against the old records, by distinct ship and by place (the old
lists name some ships twice, and some waves at two points). It found 8 missions: 14 enemies missing outright, at
four nameless points halfway along the first leg (OLDZIEY3 2 Cleomenes, DEFIANC2 4 Agis, LIXIS1 5 Agis, ATARAXI1 3
Convolutions), and four waves the old game lists at two points (an ambush on the way or the nav point, Nav 1 or the way
home, the carrier's third place or the rendezvous), which come at whichever is reached first, but ours only at the
later one (OLDZIEY2, RIGEL3, SHMRN2, SHMRN3: port.py kept the last list). Fixed: the hidden points and their lines
(missions.txt), `also=N` for a wave at two points (RVGAME.DAT 25; campaign.c lists it at both; logic.c enterNavPoint
brings no mission ship twice, `mship_brought`; M8.60), the briefings and `turrets=off` that told of attacks at the
launch that the old game never made. waves.py now finds none.

The old orders name one mission ship, ours a type (prey=OX: the nearest Ox of the other side). In three missions an
Ox only waits at the carrier beside the escorted one (LIXIS1 three, ATARAXI1, LUYTEN1: the old game's Draymen, its
type 6 like the escort's), so an order could fall on it: ATARAXI1's ambushers, whose old orders are on the Ox still to
come, would have turned back for the one at the ship. The user: "We can make it the Goddard": those waiting ships are
Goddards (SHIP25.RV packed from build/sprites' 37 views), and ATARAXI1's ambushers have their prey=OX.

Our LIXIS1 after it (the test pilot, 12 flights a cell): the pilot idle at the escort, the outbound Ox lasts 30 to 34 s
under the 5 Agis (the old one 19 s); flown at 9 degrees of aim wander 9 of 12 won, 13 degrees 6, 18 degrees 4. Also
found flying it: the inbound Ox (role=in) waits off the scene till a pilot comes within 5,000 km of its place, and the
mission counted as won without it (the pilot came home from Nav 2's fight without flying in): mission_done now asks
that it has come (M8.57).

## What is still to measure

The other types (the Salthi at nav 3; Krant, Gratha, Jalthi, the capital ships' guns on each other in the later
missions: `dump-all.sh` lists which missions have which), the wingman's own behaviour (it flies in formation at 750
km, breaks to attack, chases far), and the AI state word 8 (a slow-down of a second or two: what for).
