# The player's damage

What a hit through to the player's hull breaks, and what that does. `src/damage.c` is the model, all of it in tables a
system a row; `src/fx.c` the effects, `src/vdu.c` and `src/cockpit.c` what the displays show. Only the player's own ship
has any of this. Each system is at 0 whole, 1 light, 2 moderate, 3 heavy or 4 wrecked, read with `sys_level(SYS_x)`.

A hit spends the shield on its side, then that side's armour, then the hull, as ever. A hit that reaches the hull breaks a
system with odds of `left x 1.5 / hull`, and a further one per 24 points of it, at most three.

## The reserve

The hull's last point is not the end. Past it the player has **twice the hull again** (at least 60 points), and every hit on
it breaks at least one system. The VDU and the survival display read `CORE HIT` in place of the hull, the master warning goes
frantic, a klaxon sounds, smoke drifts up through the cockpit, and the view never stops shaking. A hit bigger than all of it
still kills outright.

## Where a hit lands

The side is the armour side the hit took. `k_odds` is the table: a change of mind is a number.

| Forward | Aft | Either side |
|---|---|---|
| guns, radar, tracking, computer, both VDUs, nav, IFF | engine, fuel tank, power, thrusters, shields, absorbers | that side's VDU, guns and missile racks, thrusters, the tank |

A rolled system that is wrecked already passes to the next, so no hit is wasted.

## The systems

| System | Light | Moderate | Heavy | Wrecked |
|---|---|---|---|---|
| Engine | 85% speed and acceleration | 65% | 45%, the view rumbles | 25%, no afterburner |
| Fuel tank | holds 80%, leaks, loses an eighth at the hit | 55%, leaks faster | 30% | empty, Kit's reserve too, no afterburner |
| Thrusters | 90% turn rates | 75% | 60%, the nose pulls | 40%, the nose pulls |
| Absorbers | slides from the type's damaged figure | 80% of it, the view jolts in a hard turn | 60% | 40%, jolts harder |
| Shield generator | holds 75%, slower back | 50% | 25% | none |
| Power plant | energy to 85%, slower back, running lights out | 65% | 40% | none comes back |
| Guns | a gun destroyed a point, on the hit's side first | | | |
| Missile racks | a launcher destroyed a point, its rounds with it | | | |
| Decoys | half lost a point | | | none |
| Tracking | brackets wander, lock takes twice as long | wander more, three times | no range, no target hull, brackets come and go | no lock |
| Computer | VDUs burst into static | longer | most of the time, and locks slower | both VDUs static for good |
| Left and right VDU | a few dashes of static | more | cracked glass, rows out | dead |
| Radar | snow | half the range | cracked, contacts come and go | dead |
| IFF | contacts lose their colours now and then | oftener | oftener | all contacts grey, brackets grey, no radar missile lock |
| Nav computer | | | no distance on the nav readout | no autopilot (the cross still shows the way) |
| Intercom | words lost from the comm | more | half | nearly all, no comm menu but the landing |
| Ejector | | | | no ejection |
| Repair | mends nothing | | | |

**The repair system** (the user's rules) mends a point off a hurt system every 20 s, chosen at random, a wrecked one too, for as long as it
is whole itself: any damage to it and nothing mends again. It cannot mend the absorbers or the tracking, a missing weapon stays
missing (guns, missile racks and decoys), and it never mends the hull, the armour or the reserve. The glass stays cracked after the system behind it is mended, and fuel that leaked is gone.

**The comm menu is how a pilot lands**, so it stays up: on the other VDU if its own is dead, over the static if the computer is
gone. Nothing leaves a pilot unable to get home: the nav cross always shows, and a wrecked intercom still asks for the landing.

## On the panel

`D` brings up the damage page on the left VDU: the ship's outline, as the target and weapons pages draw it, with every hurt part on it
as a shape, where it sits in a ship (the user: "engine section for engine", "a computer for computer", "radar for radar"). The engine
section is the tail of the ship's own outline around its real engines, ribbed, with the nozzles; guns and missile racks at their real
mounts; thrusters on the outline, firing out; the others are pictures from `tools/vduparts.py` (a dish, a monitor and keyboard, a reactor,
a spring for the absorbers), placed by `g_part_at` ahead of the engine section. A lost gun or launcher counts as its system hurt, for good.
One part at a time is bright with a black card behind it, its name below in small red letters and nothing about how badly (the user:
"just state the part if it's damaged"). They come round by themselves every two seconds, and `D` moves on at once.

`G` and `W` put up the weapons pages in the same place, from the user's picture of an old game's VDU: `ACTIVE GUNS` or `ACTIVE MISSILE`,
the weapon's name, how hard it hits in a word (`DAMAGE: MODERATE`: under 25 light, under 36 moderate, under 100 heavy, else severe), the
ship with the weapons that fire in red and the others grey, a missile for each round left, and the count (`4 GUNS ENGAGED`, `2 LOADED`). A
VDU under 80 rows and narrower than twice its height has the name above the ship and the rest below. A page asked for ends a comm on that
VDU, as the nav page always did: the face would hide it.

The own hull has no figure anywhere (the user: "a little mystery", "let the players live on edge"): `HULL OK` down to 60%, `HULL HIT`
down to 25%, then `HULL LOW`, `CORE HIT` on the reserve, and the kits' hull bars move in the same steps. `H` shows the count on the survival display.

The cockpit kits' painted damage shows each system hurt, a bit of `rvCockpitDamage` per overlay (`k_overlay`). Not every kit
paints every overlay. The hull's own: the armour readout with a side gone, the hull readout at a quarter gone, the left canopy at
half, the warning panel and survival VDU on the reserve, the right canopy at 75% of it, the overhead at 40%.

## The effects

Read from what changed, a hit from any source, using the effects' own random stream, so none of it touches the play.

- **The view shakes**: through the opening only, the cockpit stands. A pixel for a shield hit, three for armour, five for the
  hull, seven on the reserve, easing off. The engine heavy, the absorbers in a hard turn, or the reserve rumble on their own.
- **The screen washes**: red for a hit on the hull, faint white for armour.
- **Sparks** fly from the panel of the system that broke, with an arc or (wrecked) a clunk and a dying whine; smoke rises from a
  wrecked one, and all round the cockpit on the reserve.
- **A ship with half its hull gone** spits sparks now and then, for anyone to see, the player's own from outside.

## Checks

- `tests/play/playtest.sh M12` (and `--dos`): the model. `M10.2`, `M10.3` and `M10.7` as before.
- `EXE=RVT.EXE tests/run.sh tests/damage.txt /fly 1 /flight 60`: a shot of each level (RVT.EXE: `(` every system a point worse,
  `)` the hull and half the reserve gone). `tests/damagefx.txt` with `/record 1` for the effects, frame by frame.
