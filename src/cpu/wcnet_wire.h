/*
 *  Line art drawn at twice its size, one fine pixel wide (wcnet_wire.cpp): the wireframes of a ship display.
 */
#ifndef WCNET_WIRE_H_
#define WCNET_WIRE_H_

#include <vector>
#include "dosbox.h"

namespace wc {

struct WireFrame {
    int w, h;
    int axis;                       // u + u' of a mirrored pair of the art's columns, -1: not a mirror image
    std::vector<Bit16s> fine;       // 2w x 2h: a colour, or -1
};

// px: the art, w x h colours (-1 none); block: per pixel, 1 keeps it the game's 2 x 2 block (may be empty).
void wire_make(int w, int h, const std::vector<Bit16s> &px, const std::vector<char> &block, WireFrame &out);

// A picture as the game shows it at the top level: wire_make, with the user's notes on single pictures (kWireFixes).
void wire_picture(int w, int h, const std::vector<Bit16s> &px, WireFrame &out);

}  // namespace wc

#endif
