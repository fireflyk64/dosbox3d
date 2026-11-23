/*
 *  Line art for Armada's high-resolution cockpit (wcnet_armada_hires.cpp): the game's shapes drawn one fine pixel wide,
 *  their rings fitted round.  The ring fitting and the line art are Privateer's (wcnet_priv_hires.cpp, docs/privateer.md
 *  section 26), copied here while that file is being worked on; one shared module is the aim.
 */
#ifndef WCNET_ARMADA_ART_H_
#define WCNET_ARMADA_ART_H_

#include <vector>
#include "dosbox.h"

namespace wc {
namespace armart {

struct Shape { int w, h, left, top; std::vector<Bit16s> px; };   // -1 where nothing is drawn; the anchor at (left, top)

// A 640x400 picture of 8-bit pixels, clipped to the game's rectangle x0..x1, y0..y1 (in the game's pixels)
struct Target {
    Bit8u *px;
    int w, h;
    int x0, y0, x1, y1;
    Bit8u *set;             // (if not NULL: 1 where a pixel was drawn)
};

void plot(const Target &t, int X, int Y, Bit8u c);                      // a fine pixel
void line_fine(const Target &t, int X, int Y, int Xe, int Ye, Bit8u c);  // between fine pixels
void line(const Target &t, int x0, int y0, int x1, int y1, Bit8u c);     // between the game's pixels' middles
void shape_lines(const Target &t, const Shape &s, int x, int y);         // a shape as line art at the game's (x, y)

}  // namespace armart
}  // namespace wc

#endif
