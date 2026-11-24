/*
 *  Fine lines for Armada's 640x400 flight (wcnet_armada_hires.cpp): the target's brackets on the space and the reticle's
 *  icons, one fine pixel wide.  The lines are Privateer's (wcnet_priv_hires.cpp), copied.
 */
#ifndef WCNET_ARMADA_ART_H_
#define WCNET_ARMADA_ART_H_

#include "dosbox.h"

namespace wc {
namespace armart {

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
void circle_fine(const Target &t, int X, int Y, int r, Bit8u c);         // about a fine pixel (the midpoint circle)

}  // namespace armart
}  // namespace wc

#endif
