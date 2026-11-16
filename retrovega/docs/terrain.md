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
  - `step_ground`: the player that flies into the ground (its centre within 10 km), or into an arch's rock, is
    destroyed (the user, 2026-11-15: "intersection with the terrain causes a human ship to explode"; it had been
    stopped and knocked as by a rock). Every other ship glides over it: under 40 km (`HOVER_OVER`) it is lifted each
    step, a quarter of the way (at least 3 km), its fall that step taken back ("perhaps AIs get shifted some fixed
    number of meters upward smoothly ... so it looks like they are hovering towards the surface"); under 400 km the
    ground 8 steps ahead counts too, so it rises before a cliff, not inside it (the canyon run's lowest fighter, every
    30 frames of a flight in RVT.EXE's log: 28 km, where without the look-ahead one was 49 km in the rock). Under a
    slab's roof, too close, a ship has its step undone.
  - The speed floor (`SPEED_FLOOR_KPS`): where the ALT readout shows (under 20,000 km), the player's throttle goes no
    lower than 100 (the user, 2026-11-15: "near a planet the min speed should be 100... below 100 should bring it back
    up and do a few points of damage at that time"). The pilot's own cut, the ship still moving, costs 3 points
    (`STALL_POINTS`, at most once a second); a stop the game made (a ram, the carrier's hull: the speed 0 too) is
    only brought back up. Landing needs no speed, so it is untouched.
  - The autopilot raises anyone it puts under the ground to 300 km over it.
- **The native builds** answer the same requests with terrain.c (`planet.c` under `RV_HOST`; rvsim without the
  picture).
- **What it costs RV.EXE:** about 2.2 KB of conventional memory (3,936 bytes left for DOS).
- **The missions.** In missions.txt:
  - `planet SEED x y z RADIUS [kind=NAME]` (km; the kind is its look, below);
  - `ground pad navN radius= slope=` and `ground arch navN span= width= air= thick= [heading=]`;
  - `nav ... ground`: a nav point as high over the ground as the mission puts it over the radius.

- **Mirage**, the first planet (now Shmrn's: below, "A terrain mission in every set").
  - The Vigilance holds 8,000 km over the desert planet.
  - `/planet N` is the test scene (the planet N km under the player, an arch ahead).

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
- **The missions** (node MIRAGE then, after the victory missions in the page's menu; since the third brief, below,
  Shmrn's):
  - **MIRAGE1, the canyon run** (`/fly 40`, now SHMRNC). With Comet then, Torch now:
    - Nav 1 is the canyon's mouth on the plain north of the carrier, two Nicanders there;
    - Nav 2: an Agasicles loading on a pad by the river (kill it), three Agis;
    - Nav 3: a stone bridge across the canyon, a cruiser waiting past it with two Aristons. It runs for Nav 5 when
      the player comes (the escort route, `role=out jump=5`, held at Nav 4 until the player has been there) and jumps
      at Nav 5 unless it is killed;
    - Nav 4: three Nicanders;
    - Nav 5: the canyon's end.

    The canyon runs about 37,000 km, 3,000 km deep, its floor 1,800 km wide, its walls 1,300 km across each.
  - **MIRAGE2, the depot patrol** (`/fly 41`): the first mission re-laid south of the carrier, its arch and pad grown
    with the planet (1,400 km of air under the crown, so the Ox at Nav 2 sits under it). Now Defiance's, on a planet
    of its own; its place on Mirage is Shmrn's Redeemer strike (SHMRNP).
- **Check:** `tests/host/planet_view.sh MISSION [OUTDIR]` makes a mission's planet from the packed RVGAME.DAT as the
  card does, puts its nav points over the ground, prints how far each line between them clears the ground, and draws
  it from each nav point, from over each, high over the first and straight down. `--map OUT.PNG SEED RADIUS CX CY CZ
  X0 Z0 [SPAN]` draws the seed's ground from over the world's +y with a 5,000 km grid, for laying out nav points.

## A terrain mission in every set (the user's third brief, 2026-11-14)

> Can we make one terrain mission per set. That way players get to play terrain with each fighter type. Final set one
> should have two terrain including redeemer (for six total in that set). Final terrain mission should be the canyon
> one. We can have escort and other mission types. Patrol. Strike.

The user's answers: Shmrn is the set with two; each mission wherever it fits the set's story; they count for the set's
points and are skipped without the card; the Mirage series in the menu removed. Then: "The first terrain mission with
the shroedinger should make use of cloak ... Maybe lots of heavy bad on the way. Stay cloaked. Arrive at the target
lightly defended. Or hit each heavy bad with surprise"; "Different systems should have different planets (color
scheme. Rostov should be green). Rostov needs to have asteroids en route. Maybe near the carrier so we don't need to
render both"; of the sets with the captured destroyer and the asteroids, no escorts there ("it will frustrate players
who want to find themselves to the winning path"), but "We want an escort mission though"; and of the miners' hauler,
"Keep hauler protection but don't cause failure if user fails it".

| Set | Mission | Fighter, wingman | Planet (seed, look) | What |
|---|---|---|---|---|
| Enigma | ENIGMAP, third | Schroedinger, alone | 101, ice | Strike: the listening post. Cloaked past four Cleomenes and two Aristons that guard their posts (`role=guard`), or one at a time from behind; a Charillus on a pad and two Agis at Nav 3 |
| Cephid 17 | CEPHIDP, third | Franklin, Drifter | 211, red | Strike: a Charillus tanker on a pad in the dust |
| Shelton | SHELTONP, third | Schroedinger, Comet | 307, savanna | Patrol: the strip the Nicanders on the deck came from, a Charillus under an arch |
| Crucible | CRUCIBLP, second | Gawain, Comet | 401, ash (lava pools) | Escort: an Ox lifts the Kahan's lifeboat crew out of the lava fields (`role=back`) |
| Oldziey | OLDZIEYP, third | Franklin, Prime | 503, ochre | Strike: the north base, one of the second planet's four |
| Defiance | DEFIANCP, second | Schroedinger, Comet | 607, violet | Patrol: the depot under the arch (MIRAGE2), its Ox a bonus |
| Lixis | LIXISP, second | Gawain, Anchor | 709, jungle | Escort: the vaccine's Ox down to the jungle settlement (`role=out jump=3`) |
| Ataraxia | ATARAXIP, third | Franklin, Echo | 811, ocean | Patrol: the island chain, a Charillus under an island's arch |
| Rigel | RIGELP, second | Lancelot, Frost | 919, tundra | Strike: the relay's two ground stations |
| Barnard's Star | BARNARDP, second | Gawain, Kestrel | 1013, green | Patrol: asteroids out of the launch (Nav 1, a field 11,000 km off the carrier, 8,800 km over the ground), then the forest and the miners' camp, their Ox a bonus |
| Wolf 359 | WOLF359P, second | Franklin, Kit | 1117, steppe | Patrol: the colony (its Ox a bonus) and the bombers' strip |
| Shmrn | SHMRNP, third | Redeemer, Torch | 7, desert (Mirage) | Strike: the Leonidas's supply ship under the arch |
| Shmrn | SHMRNC, fifth | Lancelot, Torch | 7, desert (Mirage) | The canyon run (MIRAGE1), the last terrain mission, the day before the Leonidas |
| Luyten's Star | LUYTENP, fourth | Franklin, Tally | 1301, grey | Escort: the grey moon's miners home in an Ox (`role=back`) |

- **Without the card** (campaign.c `camp_slot`): a set's missions are counted as this machine flies them, a planet
  mission only where a planet is drawn (`camp_node_missions`, the briefing's "mission N of M", the way out by points
  or by wins). Since 2026-11-15 RV.EXE draws a planet itself without the card (below), so every DOS machine flies
  them all; the skip is left for the play tests' `g_planet_off` (M8.66) and the native game without the card's
  picture. RVGAME 28: six missions a node, 16 nodes (the save's `systems` has a bit a node).
- **The story's yesterday:** a planet mission leaves the campaign's last result alone (`g_camp.last`), so the next
  briefing's `after=won|lost` lines are about the mission before it, flown with the card or without it.
- **Their place in the story:** each comes where its set has a gap for it (after the destroyer at Cephid, before the
  assault at Oldziey, after Rowe at Defiance, the day before the Leonidas at Shmrn), and its lines name only what is
  true there with the wingman alive or dead (`alive=`, `dead=`).
- **Points:** they add to the set's points against its pass mark (the old game's marks unchanged), so the card's
  players have more ways to the good path, never fewer.

### The planets' looks

`planet ... kind=NAME`: desert, green, ice, red, ash, savanna, violet, ochre, jungle, ocean, tundra, steppe, grey
(terrain.h `TR_KIND_*`, terrain.c `tr_looks`). A look is the eight materials' colours (on a green world the sand's slot
is meadow and the dark crests are snow), the haze's and the high sky's, and how wet the land is: more green and scrub
lower or higher, and the pools' level (an ocean world is mostly sea; the dry ones have none; ash has lava). The wet
lean changes where the materials go and the green's water table only, in integers, so the heights stay the same on
every machine. The record carries it in its ground count's high nibble (no size change). Mirage keeps the desert, its
heights as they were. `PV_KIND=N PV_SHOTS=DIR tests/host/planet_view.sh --map ...` draws a seed in a look from low,
high and orbit.

### Guards (`role=guard`)

An enemy fighter wave that keeps its nav point (logic.c `guard_return`): it fights what it sees within 6,000 km of
itself and lets it go at 9,000 (a fighter's engage distance is some 25,000 km: before the guards, Enigma's Cleomenes
went for the Vigilance 9,000 km off as soon as the player cloaked); with nobody to fight it flies back to its post
(its nav point and `at=`) and loiters within 1,500 km of it. It has no orders and raids nothing. An idle guard does not
hold the autopilot or the landing, nor a wave waiting for the last to go, and the mission is done without it: it may
be slipped past. Its nav point's goal is met only with it gone (points for a fight won).

### A parked ship of ours

A capital ship with `role=stay` on our side (the miners' Ox, the colony's, the depot's) is worth its goal's points
alive (`goal escort OX navN`: met and alive), and its loss never loses the mission (logic.c `mship_must_live`).

### The cloak strike, flown (play test M8.65)

Cloaked at Nav 1 its two Cleomenes come and neither sees the player nor leaves its post; a surprise from 600 km
behind one turns only that one (the other guards 7,000 km off), and from its tail it died in about 5 s, the player
unhurt (twenty runs of the test pilot: 20 kills, no deaths; the old game's hull roulette from behind); Alt+C again and
it lets go; the patrol between Nav 1 and Nav 2 comes at its hidden point (reach 2,500 km); cloaked at Nav 2 and Nav 3
nobody sees the player; uncloaked at the post for 40 s no heavy comes (the nearest 10,300 km off); with the post's Agis
gone the autopilot goes home past the guards.

### Checks

`tests/host/planet_view.sh` for each of the fourteen: every line between nav points clears the ground (the least,
Rigel's 432 km and Wolf 359's 396), and the escorts' climbs home clear it by 4,300 km and more. Play tests M8.63 and
M8.64 fly SHMRNC now.

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

## Without the card: RV.EXE draws the planet (the user's fourth brief, 2026-11-14)

> do the best we can with the planet on dos at 30 fps without the accelerator card ... Strong preference for really
> low rez decimated or low rez terrain as long as everything else is high rez

Then: "Lets use the bake and do 128x80 size ... And lets download the planets on demand between mission or at mission
start if possible", and "hopefully we can make the bake look just as nice as the other version".

- **The bake** (`tools/planetbake.c`, `tools/planets.sh`, `src/bake.h`): each planet mission's ground made at build
  time into `build/dos/ID.PLN` (5 to 17 MB, 96 MB for the fourteen), and every machine's logic reads it (`bk_column`,
  bake.asm with its C twin): the card, the native builds and DOS ask the same integers. The file's view section
  (`PvHead`, pview.h) is the picture's: a gnomonic plane at the nav points' mean way, a pyramid of levels (16 km cells
  at the finest, coarser out to four radii), row spans, each level's 4 x 4 blocks' highest, slab grids round the
  arches, the dither, the haze's table and the glow's.
- **The picture** (`src/pview.c`, `src/pview.asm`): 128 x 80, 5 x 5 blocks dithered into the cockpit's opening,
  everything else at full resolution. The card's march (lines through V, the point straight down) a quarter at a
  time, 96 x 96-cell windows a level kept in XMS and moved when the eye's window moves; the samples weighed between
  four cells, their light and haze weighed between samples along a line (one 32-bit sum, 8.8 each), the arches' slabs
  with sub-steps where a line crosses their grid, the glow above the horizon. The C twins draw it on the host
  (`tools/pvshot.c`: the card's picture beside it, eye by eye); on DOS the asm, a blob (PVIEW.BIN, kept in XMS) in
  three parts copied in for each use: the set-up (the frame's numbers, steps, a quarter's lines, the haze's rates, the
  glow) into the scratch buffer's window room, the march into the band buffer's tail, the blit into the scratch
  buffer's tail at a band. RVP.EXE /twins holds each part to its C twin.
- **The speed.** The card's look costs 1.7 to 4.8 M cycles a picture at the page's 50,000 cycles (2.9 M on average
  over Shmrn's planet's 14 eyes); beside the rest of the game 30 fps leaves some 0.94 M. So its sampling coarsens
  while frames run long (`pv_pace`): six details, steps about 2, 3, 4 pixels apart, then every other line marched
  (each filling its neighbour, the slab sub-steps a cell apart) with steps 4, 6, 8. A flight frame's work (the wait
  for the rate left out) weighed over some 8 frames: past 8.5 ticks of the 240 Hz clock coarser (and half a second
  before it moves again), under 5 finer (two seconds). It begins at steps 3 and keeps what it reached from flight to
  flight; the flight's end logs the details it had. Per picture: detail 1 2.9 M on average, 3 1.26 M, 5 0.92 M (1.33
  M beside the arches, where the sub-steps stay). Flying Shmrn's planet mission (`/fly 41 /nogx /flight 30`): 872
  frames in 30 s.
- **Hidden behind the ground** (bake.asm `rv_bk_hides`): the line from the eye under the lowest ground anywhere (the
  planet's body), and for a ship the bake's ground at 8 of its points: what is drawn follows it, nothing else.
- **The page** fetches a mission's .PLN when its flight needs it: `ships_want` lists it in RVWANT.TXT with the ships'
  sets (at the briefing and at the launch, where the flight waits for it), `tools/artweb.py` serves the files
  gzipped (72 MB in all), `web/rvsets.js` writes it among the game's files, keeps it in the browser's cache by its
  hash, and drops the planets a new list does not name.
- **Conventional memory:** RV.EXE has some 2,300 bytes left for DOS (build.sh fails under 2,048): pview's C is 5 KB
  of it. New code for DOS goes in the blob or in asm.
- **Checks:** `tools/pvshot.c` `DETAIL=n` draws a detail, `PVEYES=file` writes each eye and prints each picture's sum;
  a bench patch kept outside the tree renders those eyes in RV.EXE (every sum the host's, the cycles by RDTSC);
  `nasm -dPROF` makes a blob that counts each part's cycles.

## Still to do

- A guest that comes into a planet mission after the host's launch holds the host's game while it makes the planet
  (about 5 s natively), the layer waiting for its first frame ("Waiting for GUEST to launch..."). In the rooms both make
  it during the briefing. The two-emulator rig starts that way (`/fly`): on the canyon run with the card, the autopilot
  scenario passes 35 of 37 checks and combat 18 of 20, the two failing ones its clock-timed look at 8 s and the host's
  frame count then (all 37 pass without the card). The same on Enigma's cloak strike (2026-11-14): meet 16 of 17,
  and the cloak scenario's clock-timed looks each see the guest's cloak one step late (on, disrupted, on again, in
  order); on the missions without a planet it passes 26 of 26.
- Close to a canyon wall (tens of km) its patch's 21 km cells show as long smooth streaks.

- The approach and straight-down views: 2 to 3 times over budget natively (the card's detail adapts; the page held
  30 fps in Mirage regardless).
- From right under an arch, the outer edge of its underside still shows a patch cell's steps.
- RV.EXE: some 2,300 bytes left for DOS with the 24 KB balloon held, after its own planet picture (above); the user then
  let 4 KB of the balloon go for head room (2026-11-15: "also deflate the balloon by an extra 4 KB so that we have some head
  room"): some 6,400 bytes left with the 20 KB balloon.
- Without the card, the views beside the arches still run long at the coarsest detail (about 24 fps at 50,000
  cycles): their slab sub-steps, the lines' set-up and the window moves are what is left to make faster.
- The native game without the card's picture still skips the planet missions (its `planet_can`).

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
