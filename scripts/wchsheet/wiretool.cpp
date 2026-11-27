// wiretool: stdin records [int16 w][int16 h][int16 px x w*h] -> stdout [int16 fine x 4*w*h] (wcnet_wire.cpp's
// wire_picture: what the game draws at the top level)
#include <stdio.h>
#include <vector>
#include "wcnet_wire.h"
int main() {
    short wh[2];
    while (fread(wh, 2, 2, stdin) == 2) {
        const int w = wh[0], h = wh[1];
        std::vector<Bit16s> px(w * h);
        if (fread(&px[0], 2, w * h, stdin) != (size_t)(w * h)) return 1;
        wc::WireFrame f;
        wc::wire_picture(w, h, px, f);
        fwrite(&f.fine[0], 2, f.fine.size(), stdout);
    }
    return 0;
}
