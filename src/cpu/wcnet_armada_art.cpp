/*
 *  Fine lines for Armada's 640x400 flight (wcnet_armada_art.h): the target's brackets drawn on the space, the reticle's
 *  icons.  The lines are Privateer's (wcnet_priv_hires.cpp) with its picture made a parameter.  (The reticle made into
 *  line art each time, its rings fitted, and the cockpit's own lines are gone: the user, 2026-11-24, "remove the code to
 *  make the very high resolution displays"; "I wonder if we could have saved out reticle icons instead of runnign some
 *  post process".)
 */
#include <stdlib.h>
#include "wcnet_armada_art.h"

namespace wc {
namespace armart {

void plot(const Target &t, int X, int Y, Bit8u c) {
    if (X < 2 * t.x0 || X >= 2 * (t.x1 + 1) || Y < 2 * t.y0 || Y >= 2 * (t.y1 + 1) || X < 0 || Y < 0 || X >= t.w || Y >= t.h) return;
    t.px[Y * t.w + X] = c;
    if (t.set) t.set[Y * t.w + X] = 1;
}

void line_fine(const Target &t, int X, int Y, int Xe, int Ye, Bit8u c) {
    const int dx = abs(Xe - X), dy = -abs(Ye - Y), sx = X < Xe ? 1 : -1, sy = Y < Ye ? 1 : -1;
    int err = dx + dy;
    for (int n = 0; n < 8192; n++) {
        plot(t, X, Y, c);
        if (X == Xe && Y == Ye) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; X += sx; }
        if (e2 <= dx) { err += dx; Y += sy; }
    }
}

void line(const Target &t, int x0, int y0, int x1, int y1, Bit8u c) {
    if (x0 < -2000 || x0 > 2000 || x1 < -2000 || x1 > 2000 || y0 < -2000 || y0 > 2000 || y1 < -2000 || y1 > 2000) return;
    line_fine(t, 2 * x0 + 1, 2 * y0 + 1, 2 * x1 + 1, 2 * y1 + 1, c);
}

void circle_fine(const Target &t, int X, int Y, int r, Bit8u c) {
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        plot(t, X + x, Y + y, c); plot(t, X + y, Y + x, c); plot(t, X - y, Y + x, c); plot(t, X - x, Y + y, c);
        plot(t, X - x, Y - y, c); plot(t, X - y, Y - x, c); plot(t, X + y, Y - x, c); plot(t, X + x, Y - y, c);
        y++;
        if (d < 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

}  // namespace armart
}  // namespace wc
