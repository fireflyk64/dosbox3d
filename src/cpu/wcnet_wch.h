/*
 *  Wing Commander 1 at 640 x 400 in flight, each game pixel 2 x 2 (wcnet_wch.cpp, docs/wc-hires.md).
 */
#ifndef WCNET_WCH_H_
#define WCNET_WCH_H_

#include "dosbox.h"

namespace wc {

void wch_watch(void (*watch)(Bit16u ip));   // the places, for the dispatcher's table (none until wanted)
bool wch_hook(Bit32u ip);                   // one came up: true when it was ours (never waits)
void wch_tick();                            // every ~1000 instructions: the picture turned on and off
void wch_set(int level);                    // the page's boxes, live: 2 all, 1 the space alone, 0 off
int wch_level();                            // what shows: 2 all, 1 the space alone, 0 the game's own
bool wch_key(bool pressed);                 // the backquote key: true when it is ours (allowed; a press flips)
bool wch_on();                              // the override is ours

}  // namespace wc

// The VGA's frame while the picture is ours (vga_draw.cpp, the override).
void wc_wch_vsync();

#endif
