# Planets: the terrain card

Branch `terrain` (worktree `~/dev/rv-terrain`), kept off `retrovega` and the live site until it is ready.

## The brief (the user, 2026-11-14)

> Ok now we need to start a new branch "terrain" for a new initiative. ... We want to do planetary missions. That means
> we will need to have a special interrupt to draw a terrain sphere. This kind of mission should only be available if
> there is hardware acceleration. The accelerator needs to take a command to load a textured trrrain and spherical
> height map. It should be procedural so only the seed is needed. The host should be able to do height queries and
> occlusion queries to know whether to draw 2d sprites. We can simplify by doing only full occlusion or zero occlusion
> even when peaking over the mountain. We should allow two layers of terrain to support caves and tunnels. (Arches) and
> probably an overlay over the procedural height map so missions can define something. The planets should be small so
> you can fly over them in about 5 minutes. And auto to them. We need also some kind of atmospheric fog effect that
> depends on height to camera. A sample mission can be a patrol with a base under an arch. The first planet can be a
> simple desert planet with some greenery sections

## What is built (step 1: the terrain itself, without the game)

`src/terrain.c` / `terrain.h`: plain C that runs only on the host (the RVGX card and the native builds, never RV.EXE),
like `debris.c`.

- **The planet** is a cube's six faces blown up to a sphere, each 1,536 x 1,536 cells (about 65 km a cell at Mirage's
  64,000 km; 1,024 and 49 km before the second brief), with a border of the next faces' cells so a lookup never crosses
  a seam. Each cell has a height (int16, quarters of a km over the datum: up to 8,000 km either way) and a colour id
  (material x 32 levels of light). Halvings of the grid down to 8 cells a side serve the far
  ground.
- **Made from the seed alone**, with integers only: 3D value noise on a lattice (a hash), integer square roots and
  divisions. The same seed gives the same ground on every machine, so a game's logic can ask about it in lockstep.
  The desert: broad basins, ridged mountains where the land is high, mesas (flat tops, steep sides), long dunes across
  a wind on the sand, rubble, and green in the low wet basins. Rock where it is steep.
- **The size:** radius 32,000 km at first; 64,000 km since the second brief (below). The relief is a sixteenth of the
  radius: at 64,000 km, mountains about 4,000 km over the lowest basins.
- **The second layer:** in a few places a slab of rock with air under it (an underside and a top over the datum), kept
  in 16 x 16 tiles only where there is one. That is how an arch, a tunnel or a cave's roof is made.
- **The missions' overlay:** stamps applied over what the seed makes. `TR_STAMP_PAD`, a round pad made flat. `TR_STAMP_ARCH`,
  a stone arch between two half ellipses, its feet solid down to the ground, its span a slab.
- **Queries** (integers, deterministic):
  - `tr_column(x, y, z)` gives the ground's radius under a planet-relative point, and the slab's underside and top
    if there is one.
  - `tr_seen(eye, point)` gives 1 when no ground or slab is in between, 0 when there is. It is all or nothing, as the
    brief asks: a sprite is drawn whole or not at all.
- **The picture:** `tr_view(camera)` then `tr_blit(band)`, drawn the way the old voxel-landscape games drew theirs,
  adapted to a sphere and to any way the eye may turn:
  - **The lines:** a plane through the eye and the planet's centre cuts the ground in a curve and the screen in a line
    through V, the point straight below the eye as the screen shows it. The screen is split in four by the diagonals
    through V, as Voxlap does, and each quarter is drawn as lines through V, a pixel apart at the edge farthest from V.
  - **The march:** each line marches out along its great circle, front to back, filling the pixels its hills rise
    into beyond what is already filled. Nearer rock (a slab included) hides what is behind it.
  - **The blit:** a screen row reads its pixels' lines with one addition a pixel.
  - **The haze:** it depends on the height of the eye and of the ground, `e^(-h/H)` summed along the ray, so low
    valleys are hazier than peaks. The sky glows where a ray meets no ground: a halo round the planet from space, and a
    hazy sky from low down.
  - **The colours:** made in RGB and matched to the palette's fixed entries (16 to 207, the same in every mission) in
    L\*a\*b\*. Each colour is two entries dithered on a 4 x 4 threshold.
- **Check:** `tests/host/terrain_view.sh [OUTDIR] [SEED]` makes the planet (twice: the two must agree), asks the
  queries, draws eight views (orbit, approach, low, banked, the arch, under it, straight down, looking up) and writes
  PNGs and a contact sheet.

**Measured** (this box, native -O2, the best of 8 frames; wasm under node about 1.3 times these):

| View | View | Blit |
|---|---|---|
| Orbit | 2.9 ms | 1.8 ms |
| Approach, 8,000 km up | 10.5 ms | 1.1 ms |
| Low and level | 3.8 ms | 1.0 ms |
| Banked | 4.7 ms | 1.2 ms |
| Straight down | 8.1 ms | 1.9 ms |
| Looking up | 2.1 ms | 1.8 ms |

Making the planet takes 3.1 s.

The speed-ups so far:

- Each cell keeps its height, colour and slab mark together.
- The frame's picture is kept a row at a time, which suits both the march and the blit.
- Hidden ground is skipped against a coarse grid of the highest ground near each cell (levels 2 on).
- The lines are thinned near V, where they bunch up.
- Steps grow as the ray grazes the ground.
- The haze comes from tables.
- The sky's glow is worked out per screen row in the blit, every 8th pixel and weighed between.
- Rows between two samples are filled with their light and haze weighed between the two, so shading is smooth.

Still to do for 30 fps on the page:

- The approach and straight-down views are 2 to 3 times over budget.
- A frame-time budget that coarsens the step when a frame runs long.
- Making the planet in the background.

## In the game (steps 3 and 4)

- **The card (RVGX 1.3).** `src/hardware/rvgx.cpp`, `E740h` to `E749h` (docs/gx.md, *Planets*). It makes the planet a
  piece at a time between frames, and adapts the detail to hold the frame rate.
- **The game (planet.c).**
  - A mission's planet is begun as the mission loads (`camp_load_units`).
  - The rooms make 8 ms of it a frame (`scene_update`).
  - The launch finishes it and colours it from the flight's palette.
  - The ground nav points are put over the ground.
  - Each frame: the view after the camera; the ground in each band after the sky and before the ships.
  - A ship the ground hides is not drawn.
  - `step_ground`: a ship that flies into the ground, or into an arch's rock, has its step undone. The player is
    stopped too and knocked as by a rock.
  - The autopilot raises anyone it puts under the ground to 300 km over it.
- **The native builds** answer the same requests with terrain.c (`planet.c` under `RV_HOST`; rvsim without the
  picture).
- **What it costs RV.EXE:** about 2.2 KB of conventional memory (3,936 bytes left for DOS).
- **The missions.** In missions.txt:
  - `planet SEED x y z RADIUS` (km);
  - `ground pad navN radius= slope=` and `ground arch navN span= width= air= thick= [heading=]`;
  - `nav ... ground`: a nav point as high over the ground as the mission puts it over the radius.

  A campaign node in layer 0 is aside from the campaign's way: the page's mission menu flies its missions alone.
- **Mirage** (`MIRAGE1`, node MIRAGE, the menu's mission 40).
  - The Vigilance holds 8,000 km over the desert planet.
  - The patrol goes down over the dunes to Nav 1, then to the Ox depot parked under a stone arch at Nav 2 (three
    Nicanders come for it), over the mesas to Nav 3 (two Agis), and home.
  - `RVT.EXE /fly 40` flies it; `/planet N` is the test scene (the planet N km under the player, an arch ahead).

## Detail patches

Where a mission puts a pad or an arch, the card keeps the ground again on a finer grid of its own:

- **Size:** 384 cells a side over up to 4,800 km of the tangent plane there, some 8 to 12 km a cell where the
  planet's are 49.
- **Making it:** it is made from the planet's ground before the shapes, plus a finer octave of noise. The shapes are
  drawn on it again at that grain; one shape function serves the planet's grid and the patches.
- **Light:** lit from its own slopes, with the planet's shadows and its own (an arch's soft shadow on its pad).
- **Queries:** they read it in integers (the patch's frame in 2^30ths, a point's place by divisions, bilinear
  heights), so collisions and nav points over a base are its, the same on every machine. The play tests' hashes
  agree, host against card.
- **The picture:** it reads the patch out to level 2. The march's step there is the patch's cells', and it looks only
  on lines whose plane passes near a patch.

## In the browser

The page built from this branch, headless Chrome on this box, Mirage by the autopilot to Nav 2 and its fight
(scratch rig: a room of one, `?cmd=rv /noquit /verbose /fly 40&env.WCNET_PERF=4`):

- 30.0 fps throughout, its worst frame 34 to 43 ms, before and after the detail patches.
- Load 32% in orbit and 46 to 55% over the ground with six or seven ships.
- The emulator at 100% of real time.

The HUD has `ALT n` under the gunsight over a planet (km over the ground, red under 100).

The canyon run (`/fly 40`, the page built from this branch, the same rig): 30.0 fps from the launch through the
autopilot to the canyon's mouth and the fight there, its worst frames 35 to 43 ms, load 39 to 47%, the emulator at 100%
of real time.

## Bigger, hillier, and a canyon (the user's second brief, 2026-11-14)

> Can we have some pretty interesting and hilly terrain with a canyon chase mission where the nav points are along a
> long canyon and various enemy objectives are along it. This should be a new mission campaign selected with an option
> in the dropdown. It can generally be after the victory mission ones. Then the planet mission. I think the planet ought
> to be twice as big actually so it's a bit flatter on the horizon and so the terrain features can be more extreme.
> Ensure multiplayer also works with it

- **Twice as big:** Mirage's radius is 64,000 km. The grid is 1,536 cells a face (65 km a cell) and a height unit a
  quarter of a km, so the relief (4,000 km) and the deepest canyons fit the int16 heights.
- **Hillier** (`tr_desert`):
  - rolling hills everywhere (lower among the mountains, whose ridges are their own);
  - more mountains with sharper ridges;
  - dry gorges, steep sided, wherever a noise crosses 0 on the high dry land;
  - dunes only in fields of them;
  - mesas a fifth of the relief high.
- **Rock in layers:** bands by height, a little wavy, lighter and darker. The bands are light, not materials: the fill
  blends light from sample to sample, so a band's edge stays smooth up a wall, where a material's would step with the
  samples.
- **The haze** grows no thicker under the datum. A canyon's floor 3,000 km down had been lost in it (13 times the
  datum's). The integral along a ray stays exact: an exponential above the floor, straight on under it.
- **The canyon** (`TR_STAMP_CANYON`; missions.txt `ground canyon navA to=navB width= wall= depth= rise=`):
  - **Its path:** from its nav point through the mission's next ones to `to`, a chain of stamps (`TR_STAMP_JOINED`),
    up to 8 stretches. The record's heading word holds the last nav point's number; the record keeps its 12 bytes.
  - **The floor:** straight from point to point, as a ship flies between them, so the line between two nav points
    keeps its height over the floor (Mirage's canyon run: 619 to 635 km all along). It lies `depth` under the land
    round each point.
  - **The mouth:** the first point is the canyon's mouth, at the land's own level.
  - **The walls:** three steps of cliff and ledge, wider in bays, rock where they are tall.
  - **The land beside:** raised to about the rim where it is lower, keeping a quarter of its own rise and fall and
    rough on top, pale where raised far, falling away to the land's own over `rise`.
  - **The floor's colours:** sand and scrub, a river winding along the middle with green banks.
  - **Determinism:** the noise is the integer noise at the direction; only `+ - * /` and square roots (a heading's
    sine is a series, `tr_sincos_deg`), so the heights are every machine's.
- **Detail patches along it:** a patch per stretch, as long as the stretch and as wide as the canyon, its cells as
  fine as 330,000 points allow (about 21 km for a 9,000 km stretch). Pads and arches keep their square ones.
  - **Order:** patches are kept finest first, since the queries and the picture read the first a point is in.
  - **The march's bounds:** the planet's own grid has the shapes too, coarser, so a patch's bound is the planet's
    plus `over`, the most it stands over the planet's cells round each point (tens of km). It is no longer the
    patch's top, which had kept the march from skipping anything near a canyon.
- **Made in pieces of a few ms:** every phase goes a block of rows, a face or a level at a time (the coarse octaves,
  a canyon's rows, a patch's shadows and light, the halvings, the bounds, a haze level's tables), the longest 9 ms
  natively. The rooms make 12 ms of it a frame. Mirage's canyon run: 4.7 s natively (7.9 s before the trims:
  - the lighting's arithmetic shared per row and column, 1.8 to 1.0 s;
  - one rubble octave;
  - a patch's own shadows marched at every other point and weighed between;
  - the shadow steps longer farther out).
  The depot patrol takes 3.7 s.
- **The same planet again is kept:** the card and the native builds compare a load with what they have (`tr_same`),
  so a mission flown again (R), or a guest given its host's, is not made twice.
- **Multiplayer:**
  - A guest flies the host's mission by its records, and now begins the host's planet (game.c `flight_prepare`).
    Before, it kept its own save's: no ground drawn, none to hit.
  - The rest of the planet is made before the launch (`flight_fly`, before `uiMode(0)`), not in the flight's first
    frame. Made after the launch, one pilot's seconds of making held every other pilot's game at its frames: two
    native emulators on the canyon run (`scripts/rvnet-duo.sh`, the autopilot, the card on) had the host at 11 fps
    for its first seconds and the two worlds' looks taken at different moments.
  - `planet_centre()` returns a far pointer (`g_pl` is far). The near one it returned read DS on DOS, so the AI's
    pull-up (`ground_mind`) steered by garbage there while the native build steered right.
- **The play tests touch the ground now:** M8.63 makes the canyon run's planet, places its nav points (700 km over
  the floor) and leaves a world that only the ground's answers decide: a dive into the floor and a climb into the
  stone bridge stopped short of the rock, an Aera fighter kept over the floor. Its hash is the same on DOS (the card)
  as natively. Before it, no play test had made a planet (`planet_on()` was 0 throughout), so the walk of every
  mission (M8.40) matched with the card off as well as on. The DOS run turns the card on (`playtest.sh`, `GX=1`).
- **The logic:**
  - a nav point's ships put under the ground as they come (a wave moved out to 15,000 km goes in a straight line, and
    a canyon bends) are raised 300 km over it, as the autopilot's are;
  - a capital ship with `role=stay` holds station (the depot ship on its pad, the Ox under the arch); it had cruised
    off at its cruise speed.
  - rvdata refuses a mission ship with no views in art/ships (a Charillus has none: the flight waited for the
    page's set of it for ever).
- **The missions** (node MIRAGE, after the victory missions in the page's menu: "Mirage 1", "Mirage 2"):
  - **MIRAGE1, the canyon run** (`/fly 40`). With Comet:
    - Nav 1 is the canyon's mouth on the plain north of the carrier, two Nicanders there;
    - Nav 2: an Agasicles loading on a pad by the river (kill it), three Agis;
    - Nav 3: a stone bridge across the canyon, a cruiser waiting past it with two Aristons. It runs for Nav 5 when
      the player comes (the escort route, `role=out jump=5`, held at Nav 4 until the player has been there) and jumps
      at Nav 5 unless it is killed;
    - Nav 4: three Nicanders;
    - Nav 5: the canyon's end.

    The canyon runs about 37,000 km, 3,000 km deep, its floor 1,800 km wide, its walls 1,300 km across each.
  - **MIRAGE2, the depot patrol** (`/fly 41`): the first mission re-laid south of the carrier, its arch and pad grown
    with the planet (1,400 km of air under the crown, so the Ox at Nav 2 sits under it).
- **Check:** `tests/host/planet_view.sh MISSION [OUTDIR]` makes a mission's planet from the packed RVGAME.DAT as the
  card does, puts its nav points over the ground, prints how far each line between them clears the ground, and draws
  it from each nav point, from over each, high over the first and straight down. `--map OUT.PNG SEED RADIUS CX CY CZ
  X0 Z0 [SPAN]` draws the seed's ground from over the world's +y with a 5,000 km grid, for laying out nav points.

## Fighters raining down along the canyon (the user, 2026-11-14)

> Can we have aera ships continuously spawning from above to keep around 2 or 3 raining down upon the player in the
> canyon mission. Once the player takes one out another shows up ahead either in the canyon or above

- **`role=stream`** (missions.txt; `jump=N` its last nav point): `ship NICANDR 1 nav1 count=3 role=stream jump=5` in
  the canyon run. From its nav point's reaching until Nav 5 is reached, each of its records keeps a ship in the world
  (logic.c `stream_step`).
- **Where one comes:** one killed or gone, its record comes again 2 s after the last one came, ahead of the player on
  the way to the next nav point (6,000 to 9,000 km on). It comes down at the way's height or, one in two, 2,500 to
  4,500 km over it, its nose on the player, raised over the ground.
- **Through the layer:** it comes by `outerSpawnShipEntity`, the waves' hook, so the host's ships are its guests'
  puppets.
- **What it holds up:** a stream's ships hold no wave (`enemy_fighters_out`), and no nav point's goal waits for them.
- **Play test M8.64:** three come within 8 s of Nav 1, ahead of the player and over the ground; one killed, another
  within 3 s; none after Nav 5.

**DOS memory:** the stream cost about 1.5 KB of RV.EXE, which left 1,248 bytes, and the build refused that. The slots
went from 128 to 62 at the same time (the user: "Should be 64 probably ... Or like 62 in case power of two is
wasteful"; abi/rvabi.def, ABI 3): the fixed block ends at 0x4200, not 0x7400. RV.EXE now leaves 14,032 bytes for DOS.

## Still to do

- A guest that comes into a planet mission after the host's launch holds the host's game while it makes the planet
  (about 5 s natively), the layer waiting for its first frame ("Waiting for GUEST to launch..."). In the rooms both make
  it during the briefing. The two-emulator rig starts that way (`/fly`): on the canyon run with the card, the autopilot
  scenario passes 35 of 37 checks and combat 18 of 20, the two failing ones its clock-timed look at 8 s and the host's
  frame count then (all 37 pass without the card).
- Close to a canyon wall (tens of km) its patch's 21 km cells show as long smooth streaks.

- The approach and straight-down views: 2 to 3 times over budget natively (the card's detail adapts; the page held
  30 fps in Mirage regardless).
- From right under an arch, the outer edge of its underside still shows a patch cell's steps.
- RV.EXE: 14,032 bytes left for DOS since the slots went to 62 (2,816 before; 6,144 before the planets).
- Missions with planets are kept from DOS without the card only by being off the campaign's way.

Done since step 4:

- Shots, missiles and blasts behind the ground are hidden too.
- An AI fighter within 400 km of the ground with its nose going down (or within 200 km) pulls up.
- Arches thin out to their edges, and material edges change half way between samples on gentle ground.
- The card read the mission record into padded structs (Mirage's arch was misplaced): packed now, checked by size.

## The plan

1. ~~The terrain, its queries and its picture, tested without the game.~~ (above)
2. **The card (RVGX 1.3).** New calls on the multiplex interrupt, the block extended:
   - `E740h`: load a planet (seed, radius, kind, the stamps, the palette).
   - `E741h`: the frame's view (the eye relative to the planet in positions, its basis, focal and centre).
   - `E742h`: blit a band (the frame-buffer fields as the sprite draw has them).
   - `E743h`: the ground under a point (`tr_column`).
   - `E744h`: is a point seen (`tr_seen`).

   Also: `gx_kernel_test` fuzzing, and the native builds calling `terrain.c` directly.
3. **The game:**
   - a planet in the world (a mission's `planet` line), drawn after the sky and before the ships;
   - ships, shots and rocks not drawn when the ground hides them;
   - the ground's collisions (as the rocks' and the capital ships');
   - the autopilot to a planet's nav point stopping over the ground.
4. **The missions:**
   - planet missions offered only with the card (1.3 or later);
   - the sample mission, a patrol with a base under an arch;
   - the first planet, the desert with green.
