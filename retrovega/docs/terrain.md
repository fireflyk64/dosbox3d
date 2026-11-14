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

**Measured** (this box, native, -O2):

- Making the planet takes 3.1 s.
- A view takes 4 to 17 ms to make and 1 to 2.5 ms to blit.

That is still too slow for the page: the target is about 3 ms natively for the two together. Next is to make it faster
(cheaper samples, a cache-friendlier grid, fewer lines where they bunch) and to make the planet in the background.

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
