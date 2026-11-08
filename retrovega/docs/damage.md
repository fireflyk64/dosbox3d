# The player's damage

What a hit through to the player's hull breaks, and what that does. `src/damage.c` is the model, all of it in tables a
system a row; `src/fx.c` the effects, `src/vdu.c` and `src/cockpit.c` what the displays show. Only the player's own ship
has any of this. Each system is at 0 whole, 1 light, 2 moderate, 3 heavy or 4 wrecked, read with `sys_level(SYS_x)`.

A hit spends the shield on its side, then that side's armour, then the hull, as ever. A hit that reaches the hull breaks a
system with odds of `left x 1.5 / hull`, and a further one per 24 points of it, at most three.

## The reserve

The hull's last point is not the end. Past it the player has **1.7 times the hull again** (at least 60 points; `DAMAGE_RESERVE_TENTHS`, damage.h), and every hit on
it breaks at least one system. The VDU and the survival display read `CORE HIT` in place of the hull, the master warning goes
frantic, a klaxon sounds, smoke drifts up through the cockpit, and the view never stops shaking. The klaxon silences
itself after 15 seconds with no new damage and sounds again at the next hit (the user, 2026-11-03: "it should
self-silence after about 15 seconds of no damage"; fx.c `KLAXON_QUIET_STEPS`). A hit bigger than all of it
still kills outright.

**Against the old game (measured 2026-11-02; the user: "within a shot or two of wc1 so the wingman and the hero feel
like they did in recollection"):** hits of 14 from ahead (the laser's, or the Photon Mk III's, at about 1,000 km)
through the layer's `!hit` and `!shoot`, until the ship is dying, a dump after each. Shields and armour go on the same
hit in both games (the same numbers and recharge); past them the old game rolls dice, so its counts scatter.

| one hit every | 0.25 s | 0.3 | 0.5 | 0.7 | 0.8 | 0.9 | 1.0 |
|---|---|---|---|---|---|---|---|
| the old game's Hornet, the player | 19 | 18 | 21 | 28 | 19 | 30 | 35 |
| the Schroedinger, the player | 19 | 20 | 21 | 24 | 25 | 26 | 27 |
| the old game's wingman (Spirit's Hornet) | 14 | 16 | 14 | 15 | | | 20 |
| a Schroedinger wingman | 14 | 14 | 15 | 16 | | | 17 |

Two things made it so: the reserve went from twice the hull to 1.7 times (twice was a hit or two more than the
Hornet), and a named pilot's ship takes a hit past its armour as a share of its hull, as the old game's wingmen do (its
code: a named pilot's ship a fortieth of the hit, at least one, of its hull's count; logic.c `hull_take`, eight laser
hits; before it the wingman died two hits early under fast fire). An unnamed fighter's hull is the old game's
roulette since 2026-11-05 (docs/ai.md "The fighters' hulls"; `fighter_roulette`, ships.csv `hull_hits`): a few laser
hits from behind kill it outright, head on it takes some twenty past the armour. Any other ship's hull takes the hit
itself.

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
| Shield generator | holds 75%, back at half the rate | 50%, a third | 25%, a quarter | none |
| Power plant | energy to 85%, back at half the rate, running lights out | 65%, a third | 40%, a quarter | 40%, a fifth (never none: the guns must still fire) |
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

## Coming home

A landing on the carrier's bow is followed by two things, each skipped by a click (`landedOn` says it was a real landing, set by
`carrier_contact`; an ejection, a test's exit key or a lost flight has neither). The flight and its result are settled before either starts.

**The landing, from outside** (`landing_show`, flight.c): six seconds (three and a half until 2026-11-05: "goes too fast";
and in a shared flight it had run uncapped, the layer's pacing over with the flight). The camera starts close behind the fighter, which fills
the view, and pulls back and round to above the carrier's quarter, while the fighter flies in along the carrier's far side and goes behind
the hull: the sprites are drawn far to near, so the carrier covers it. Then black. The game's logic is not running; everyone else is taken
out of the shot. Cleared, a fighter on its afterburner does not land: within the 100 km it is told to cut it, and
the hull is a ram's, its damage and its dead stop (the user, 2026-11-05: "if you have afterburner on when attempting
landing it should cause normal collision damage"; play test M8.46).

**Kit at the plate** (`enter_home`, game.c): her close-up in front of an armour plate, the flank of a fighter curving under into the dark,
with this flight's damage on it, rolling about 100 pixels into place behind her as the scene opens and slowing to a stop (the user), then the
debrief. If she flew and was lost, there is no scene.

| Tier (`damage_home_tier`) | When |
|---|---|
| `intact` | all the armour there, nothing hurt |
| `scarred` | armour worn, or a system hurt |
| `holed` | a face of armour gone, the hull touched, or four systems hurt |
| `wrecked` | more than three quarters of the hull gone, or on the reserve |

Her words are in `data/homecoming.csv`, at least four a tier (the build refuses fewer), never the same one twice running.

The plate (`tools/plates.py`, art/places/plate/, by hand) is a clean picture and cut-out stamps, in the close-up's 48 colours. The game lays
a big stamp (a torn hole, wires) on it for each system hurt, and a small one (a puncture, a slash, a splash, gouges) for each ten of the
hull gone, or each quarter of the armour while the hull is whole. Which stamp is seeded by what it stands for (`home_stamp`), so the same
damage looks the same and other damage looks other. At most six of each (the user: "cap it after like 4 or 6"); a wrecked ship shows at least
four big ones whichever systems they were. No rust and no soot: it happened in a vacuum (the user), so what a hit leaves is paint blasted off
to bare steel, scoring and spatter. The plate is 100 pixels wider than the screen, at the left, with stamps of its own there for the roll. The picture is put together once, in
a 296 KB resident XMS block (`game_reserve`, twice over: a drawing band is a screen wide), so her mouth moving costs only the copy.

## On the panel

`D` brings up the damage page on the left VDU: the ship's outline, as the target and weapons pages draw it, with every hurt part on it
as a shape, where it sits in a ship (the user: "engine section for engine", "a computer for computer", "radar for radar"). The engine
section is the end of the ship's own outline by its real nozzles, ribbed, and the nozzles; guns and missile racks at their real
mounts; thrusters on the outline, firing out; the others are pictures from `tools/vduparts.py` (a dish, a monitor and keyboard, a reactor,
a spring for the absorbers), placed by `g_part_at` ahead of the engine section. A lost gun or launcher counts as its system hurt, for good.
One part at a time is bright with a black card behind it, its name below in small red letters and nothing about how badly (the user:
"just state the part if it's damaged"). They come round by themselves every two seconds, and `D` moves on at once.

`G` and `W` put up the weapons pages in the same place, from the user's picture of an old game's VDU: `ACTIVE GUNS` or `ACTIVE MISSILE`,
the weapon's name, how hard it hits in a word (`DAMAGE: MODERATE`: under 25 light, under 36 moderate, under 100 heavy, else severe), the
ship with the weapons that fire in light green and the others grey (two kinds of gun on one mount side by side, each kind of missile in
its own row), a missile for each round left, and the count (`4 GUNS ENGAGED`, `2 LOADED`). A
VDU under 80 rows and narrower than twice its height has the name above the ship and the rest below. A page asked for ends a comm on that
VDU, as the nav page always did: the face would hide it.

The own hull has no figure anywhere (the user: "a little mystery", "let the players live on edge"): `HULL OK` down to 60%, `HULL HIT`
down to 25%, then `HULL LOW`, `CORE HIT` on the reserve, and the kits' hull bars move in the same steps. The outline and everything on it come out mirror images on a symmetric ship: the box is an odd width, so there is a centre pixel, and a
pixel and its mirror image read mirror-image columns of the outline (`col_of`). Only the Convolution is asymmetric; the rest differ by at
most 3% of their cells, from the renders. `H` shows the count on the survival display.

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
