# Wing Commander 1 at 640 x 400

`src/cpu/wcnet_wch.cpp`: the flight drawn again at twice the game's resolution, each game pixel a block of 2 x 2,
from the game's own drawing calls: Privateer's high-resolution picture (`wcnet_priv_hires.cpp`, docs/privateer.md)
brought back to the game its library grew from.  WC.EXE and SM2.EXE.

## The rules (the user, 2026-11-27)

* "take the 640x480 technology from privateer and backport it to wing commander 1 and 2 for higher resolution
  sprites and higher resolution target reticle. Then we can look at fonts as well and top down wireframes."
  640 x 400 as Privateer's, square pixels.
* "we must make sure not to break classic WC1 and so that the ` returns to the current mode. that game was the first
  and is the most fragile": off unless the page's **High resolution (2x)** box is ticked (`WCHIRES=1` at the launch,
  `wc_web_wc_hires(level)` while running).  Unticked, none of this runs and its places are not even watched; the `
  key is the game's as it always was.  Ticked, ` cycles the levels.
* Three levels: **2** the reticle, the target boxes, the ships and the cockpit's text; **1** the same without the
  text; **0** "exactly original resolution no upres or change at all" (the VGA override let go: DOSBox draws the game).
* "the privateer reticle and privateer fonts should be useful": the reticle is Privateer's line art (rings fitted,
  drawn half and mirrored); the cockpit's letters are the user's own Privateer font.
* "I never expect to see red engine holes unless the player is stopped. The blue flame should always cover them".

## How it is drawn

WC.EXE loads two drivers at its start (`GAMEDAT\TM.DRV`, `SUPERTM.DRV`; their code segments in `tmDriverSeg`,
`superTmDriverSeg`).  The 3D view goes into a buffer of the view's size (`flightBuffer`) and reaches the screen by a
list of spans (SPANCOPY); the cockpit, the radar and the VDUs are drawn on the screen itself.  Each call on the
buffer is drawn again into H (twice the buffer); at the span copy H is frozen with what the game shows; at the VGA's
frame a screen pixel that still holds what was copied shows H, every other the game's own 2 x 2.

| place | routine | drawn as |
|---|---|---|
| TM 0EBB | SHAPE | the art's pixels 2 x 2; the gunsight and lock cross (`cockpitMarkers` 0, 1) Privateer's line art |
| ST 161E | TURNSCALE | sampled at each fine pixel (magnified: the game's corner placement; 180 degrees and over = both axes mirrored and the rest, as the driver) |
| TM 3A12 / 3896 | LINE / RECT | one fine pixel wide: the target's brackets and box |
| TM 37F2 / 192F / 198F | FILLRECT / CLEAR / COPY | blocks |
| ST 7454 | SPANCOPY | the present |
| TM 4925 | GLYPH (registers) | the cockpit's letters (below) |

Unhooked drawing in the buffer (the comm text) is filled in at the span copy as the game's own pixels.

## The cockpit's letters (level 2)

Every letter goes through GLYPH (AL the character, BX the text context: +0 the record, +2 x, +6 the font, +A ink,
+B ground; CX the height, DX the width, SI twice the row).  WC1's cockpit font (height 6, used on the VDUs and the
speed readouts) is the 3 x 4 font Privateer's grew from, so the user's Privateer letters fit it: a letter the game
writes on the screen in exactly a glyph of `wcnet_wch_font.h` (scripts/wchfont.py, from a font dump, `WCHIRESFONTS=1`)
is kept as a cell; at the VGA's frame a cell whose ink is all still there shows without that ink, the user's letter
drawn on it.  23 of WC1's glyphs differ from the Privateer ones by a pixel or two (the same letters; listed in the
header).  Characters the user's font lacks (`# $ % & * + / ; < = > ? @ [ \ ] ^ _` and the rest) stay the game's.
The comm text in the view is another font (eight rows, capitals six high): the game's own.  "KPS", "SET" and the
other labels are the cockpit's picture, not text.

## The VDUs' ships (level 2)

The weapons display's own ship (and its missiles over it) and the target display's hull and shields are SHAPEs on
the VDUs' records on the screen (`leftVdu`, `commVdu`; found with `WCHIRESOPS=1`), line art of one game pixel, as
Privateer's target display.  The shapes drawn on one VDU one after another are gathered into one picture and drawn
by Privateer's wireframe maker (`wcnet_wire.cpp`, a copy of `wcnet_priv_hires.cpp`'s `wire_make` without Privateer's
hand-made parts, so that Privateer's approved ships cannot move with a change here): lines one fine pixel wide
between the pixels' middles, rings fitted, solid parts the game's blocks, exactly symmetric where the art is.  Kept
as a cell of the screen like the letters (checked by its pixels).  A picture of more than 12 colours or filling more
than 45% of its box (the comm display's face) stays the game's.  Letters and pictures are kept while ` passes level
0: the game does not draw a static VDU again.

## WC2

WC2's `GAMEDAT\SUPERTM.DRV` VGA entry is byte for byte WC1's (TURNSCALE ST 161E, SPANCOPY ST 7454); its `TM.DRV` is
another build (entries stored, flag E0, not LZW): SHAPE at 0EBC, FILLRECT 1AC8, RECT 1B6C, GLYPH 2BFB by WC1's
signatures.  Not hooked yet.

## Speed

`web-smoke.sh wc1hiresperf` (scripts/web-wc1-hires-perf.mjs): two pages in a room, each throttled to half speed
(Chrome's CPU throttling), Gimle 1's fight, 640 x 400 and the game's own by turns; frames counted in each wall-clock
second (`wc_web_perf(6)`), the drawing's wall time (`wc_web_perf(7)`).

## Test aids

`WCHIRESCHECK=1` (agreement with the game's buffer, logged), `WCHIRESDUMP=<per cent>` / `WCHIRESDUMPFROM=<frame>`
(frames below it written with the frame's calls), `WCHIRESFONTS=1` (the fonts used, dumped), `WCHIRESOPS=1` (calls on
the cockpit's screens, each kind once with its caller).  Page test: `web-smoke.sh wc1hires` (it asks
`wc_web_wc_hires(-2 / -3 / -4)`: the gunsight's line-art draws, the letters and the VDU pictures drawn finer).
