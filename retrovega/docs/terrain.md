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

- **The planet** is a cube's six faces blown up to a sphere, each 1,024 x 1,024 cells (about 49 km a cell), with a
  border of the next faces' cells so a lookup never crosses a seam. Each cell has a height (int16, eighths of a km over
  the datum) and a colour id (material x 64 levels of light). Halvings of the grid down to 8 cells a side serve the far
  ground.
- **Made from the seed alone**, with integers only: 3D value noise on a lattice (a hash), integer square roots and
  divisions. The same seed gives the same ground on every machine, so a game's logic can ask about it in lockstep.
  The desert: broad basins, ridged mountains where the land is high, mesas (flat tops, steep sides), long dunes across
  a wind on the sand, rubble, and green in the low wet basins. Rock where it is steep.
- **The size:** radius 32,000 km. At the Schroedinger's cruise (375 km/s) half the way round (100,000 km) takes about
  4.5 minutes. The relief is a sixteenth of the radius: mountains about 2,000 km over the lowest basins.
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

## Still to do

- The approach and straight-down views: 2 to 3 times over budget natively (the card's detail adapts).
- The minds know nothing of the ground: an enemy chasing low skims it, its step undone.
- Shots, missiles and explosions are not yet hidden by the ground.
- Missions with planets are not yet kept from DOS without the card except by being off the campaign's way.

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
