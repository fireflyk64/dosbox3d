# Wing Commander 1 at 640 x 400

`src/cpu/wcnet_wch.cpp`: the flight drawn again at twice the game's resolution, each game pixel a block of 2 x 2,
from the game's own drawing calls: Privateer's high-resolution picture (`wcnet_priv_hires.cpp`, docs/privateer.md)
brought back to the game its library grew from.  WC.EXE, SM2.EXE, WC2.EXE, SO1.EXE and SO2.EXE (SO3.EXE is SO2's).

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

## The VDUs' pictures (level 2)

The VDUs' pictures (the weapons display's own ship and its missiles; the target display's hull, drawn a section at a
time through copies of the VDU's record with narrower clips, its systems and its shield arcs) are SHAPEs on the VDUs;
those drawn on one VDU one after another are gathered into one picture and drawn by Privateer's wireframe maker
(`wcnet_wire.cpp`: a copy of `wcnet_priv_hires.cpp`'s `wire_make` without Privateer's hand-made parts) plus the user's
notes on single pictures (`kWireFixes`: WC1's Hornet's nose, "The <> should be more like a (>").  A picture the VDU's
size (a comm face, the static) stays the game's, as does one of more than 12 colours or filling more than 45% of its
box.  Each picture's wireframe is made once and kept (128 of them) and then copied (the user: "Make sure to cache the
drawings so it doesn't slow down framerate").  Kept as a cell of the screen like the letters (checked by its pixels).

The user approved the sheets of every ship (2026-11-27, "All uprez are acceptable"): `scripts/wchsheet/sheet.py` draws
them from the game's files with the same code (WC1's target display, 22 ships; its weapons display, 6; WC2's target
display, 29).  WC2's weapons display (its ships with two VDUs: Rapier, Broadsword, Epee, Sabre, Crossbow, Morningstar;
the PCSHIP files' table of the ship and its weapons) has its own sheet, `wc2-vertical`, and waits for the user's look
(`kWc2WeaponsDisplayHeld`: WC2's left VDU the game's pixels).  After a change, compare the new sheets with the approved ones pixel
for pixel (the Hornet's nose is the one change since).  `WCHIRESPARTS=1` writes each new VDU part and the palette.

Static and comm effects stay the game's 320 x 200 pixels at every level (the user: "static and comms effects should
remain at 320x200 resolution per original"): the snow of a broken VDU (SNOW, ST 74C6, not hooked; ovr134:0B9F when the
VDU's page is out, the words at dseg:BF32, 8 bytes a VDU) and the comm display's static and faces (VDU-sized
pictures) are never redrawn, and the letters and wireframes step aside as soon as the pixels under them change.  To
see it without harm: `!poke=BF32:00` ... `!poke=BF41:00` in a key script (both VDUs' pages broken, the hull
untouched).

## Stars and backgrounds

At the top level every star (a SHAPE of 5 x 5 or less in the view: the points, glints and crosses of the space
objects' table) is one fine pixel at the middle of its own pixel, in its colour there (the user: "logically every star
should be 1 pixel no matter the resolution ... all be 1 pixel in the highest mode and for the other two modes they can
be as they were"); levels 1 and 0 draw them as the game does.  The nebulae and planets are the game's own 320 x 200
pixels at every level ("should be their ordinary 320x200 selves"): WC1's are plain SHAPEs already; WC2's (its draw
loop's type 12, `objectType` of `drawnObject`) are turned and scaled as ships are, and are sampled once per game pixel
and drawn 2 x 2.  WC1's turned types 3 and 4 are the carrier's launch bay, left as they are.

## The speed readouts' labels (level 2)

"KPS" and "SET" (the user: "ops set font") are the cockpits' pictures: four rows of one ink colour, the 3 x 4 letters
the user's font was drawn for, in five patterns over the cockpits read (WC1's Hornet, Scimitar, Rapier, Raptor; WC2's
Ferret; SO2's).  The screen is searched for them when the picture comes on, when a cockpit is drawn (a SHAPE at the
screen's corner) and every two seconds; one found is drawn as the user's glyphs in its ink colour, Privateer's
art_word way, its ink's pixels the panel's colour about them (the Rapier's and SO2's panels are grained).

## WC2

WC2's `GAMEDAT\SUPERTM.DRV` VGA entry is byte for byte WC1's; its `TM.DRV` is WC1's with the routines it never calls
(the triangles, the dissolve, the silhouette) taken out and the rest moved up (entries stored raw, flag E0, not LZW).
So the places come from each driver's own table of entries (the words at its start, which the loaders copy into the
game's slots: WC.EXE seg098:0043, WC2.EXE ovr204:003F), the same code for both games.  WC2.EXE's data: TM.DRV's
segment A838, SUPERTM.DRV's DEC4, the flight's buffer BD34 (its present seg001:0401 is WC1's seg001:05DB), the
space's colour 8A91, the markers BD66 (0 the gunsight, 4 the same a pixel right, 1 the lock cross; 5 the pointer),
EMS's frame A913 and last handle A919 (seg089:0043), the VDUs B458 / B46C (seg005:0C1B).  Its cockpit font is WC1's:
the user's letters fit it.  SO1 and SO2 carried over by scripts/wcport.py (WC2's offsets - 104 / + 192).

## Speed

`web-smoke.sh wc1hiresperf` (scripts/web-wc1-hires-perf.mjs): two pages in a room, each throttled to half speed
(Chrome's CPU throttling), Gimle 1's fight, 640 x 400 and the game's own by turns; frames counted in each wall-clock
second (`wc_web_perf(6)`), the drawing's wall time (`wc_web_perf(7)`).

## Test aids

`WCHIRESCHECK=1` (agreement with the game's buffer, logged), `WCHIRESDUMP=<per cent>` / `WCHIRESDUMPFROM=<frame>`
(frames below it written with the frame's calls), `WCHIRESFONTS=1` (the fonts used, dumped), `WCHIRESOPS=1` (calls on
the cockpit's screens, each kind once with its caller).  Page test: `web-smoke.sh wc1hires` (it asks
`wc_web_wc_hires(-2 / -3 / -4)`: the gunsight's line-art draws, the letters and the VDU pictures drawn finer).
