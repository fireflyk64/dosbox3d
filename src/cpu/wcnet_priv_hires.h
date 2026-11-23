/*
 *  Privateer at 640 x 400, each game pixel 2 x 2 (wcnet_priv_hires.cpp, docs/privateer.md section 20).
 */
#ifndef WCNET_PRIV_HIRES_H_
#define WCNET_PRIV_HIRES_H_

#include "dosbox.h"

namespace wc {

void priv_hires_watch(void (*watch)(Bit16u ip));    // the places hooked, for the dispatcher's table
bool priv_hires_hook(Bit32u ip);                    // one came up: true when it was ours (never waits)
void priv_hires_tick();                             // every ~1000 instructions: the picture turned on and off
void priv_hires_set(int level);                     // the page's boxes, live: 0 off, 1 the space alone, 2 all
int priv_hires_level();                             // what shows (0, 1, 2)
void priv_hires_flip();                             // the backquote key
bool priv_hires_on();

}  // namespace wc

// The VGA's frame while the picture is ours (vga_draw.cpp, the override).
void wc_priv_hires_vsync();

#endif
