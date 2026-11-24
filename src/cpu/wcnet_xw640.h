/*
 *  X-Wing Collector's CD's flight at 640x400 (docs/xwing.md section 6): the TIE Fighter demo's (wcnet_tie640*.cpp,
 *  docs/tie640.md) compiled for BFLIGHT.OVL's addresses (wcnet_t640_game.h).  What wcnet_xw.cpp calls.
 */
#ifndef WCNET_XW640_H_
#define WCNET_XW640_H_

#include <string>
#include "dosbox.h"

void xw640_hires_vsync();                           // the VGA's frame, while the picture is ours (vga_draw.cpp)

namespace wc {

void xw640_tick(bool flight, Bit16u loadSeg);       // every ~1000 instructions: turns the picture on and off
bool xw640_on();
void xw640_watch(Bit16u loadSeg);                   // the places hooked, each watched in its own segment
bool xw640_hook(Bit32u ip);
bool xw640_fast_trip(Bit32u ip);
bool xw640_shot(const std::string &path);
bool xw640_script_item(const std::string &item, double t);
void xw640_stars_watch(void (*watch)(Bit16u ip));
bool xw640_stars_hook(Bit32u ip, Bit16u loadSeg);

}  // namespace wc

#endif
