# The silhouette experiment (branch experiment/silhouette; not taken)

Tried on 2026-10-23, judged by the user on recordings: **not worth it**.
The block mode (`/res 2`, `docs/engine.md`) stays as it is on the main
branch; this branch keeps the experiment for the record.

**The idea** (the user's): a view carries two things, the outline, which
is what pops when the view switches, and the shading inside, which barely
changes between neighbouring views. Store the outlines alone, one bit a
pixel, on a denser lattice (5 degrees: 1,297 views a fighter) at full
resolution, and the paint on the 10-degree lattice at half resolution as
the block mode has it; draw every screen pixel inside the turned outline
with the turned paint under it.

**What is here**: `tools/rvsprites.c` makes a mask set (`IN OUT.RVM 0 0 0
1 1 mask`: the ladder at full size on every view, every opaque pixel 1;
header byte 9 bit 3); `tools/pack.py` knows the 5-degree lattice (1,297 /
2,522 views); `src/ships.c` loads SHIPnn.RVM beside a type's block set
(`load_mask_set`; `ShipSet.mask`, `mask_set`; its view records stay in
XMS, being more than SHIP_VIEWS) and `draw_masked` paints inside the
5-degree outline from the 2 x 2 paint, probing outward (`paint_at`) where
the finer outline reaches past the paint; `/nomask` compares; `/zoom 4`
is the slow deliberate roll (20 degrees a second, a 24-second swing). The
Gawain's 5-degree render (`render_all.py --step 5 --only GAWAIN`, 3
minutes) is not committed.

**What it showed**: single-pixel-smooth edges stepping every 5 degrees
around 2 x 2 paint stepping every 10. The user, on the slow roll and the
slow yaw side by side with the block mode: not worth it.

**What it would have cost**: a one-bit span format for the masks (as
pixel rows the Gawain's are 25 MB; as bits 1.5-3 MB for the near sizes),
the recipe in the page's generator, a 5-degree render of every fighter,
and the draw in assembly (two inverse maps a pixel, about twice the
exact draw; the C kernel here is 15 ms a frame for a big ship).
