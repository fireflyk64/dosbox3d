/*
 *  Wing Commander Armada's flight at 640x400: the game's own scene drawn four times a frame, a quarter of a picture
 *  twice as large each time, and the cockpit over it (wcnet_armada_hires.cpp, docs/armada.md "Twice the resolution").
 */
#ifndef WCNET_ARMADA_HIRES_H_
#define WCNET_ARMADA_HIRES_H_

#include <string>
#include "dosbox.h"

namespace wc {

// wcnet_armada.cpp's
void arm_hires_loaded(const char *exe, Bit16u loadSeg);   // one of the four programs is running (NULL: none)
void arm_hires_watch(void (*watch)(Bit16u ip));          // our places, for the dispatcher's table (none unless wanted)
bool arm_hires_hook(Bit32u ip);                          // a watched IP came up: true when it was one of ours
void arm_hires_tick();                                   // every ~1000 instructions: the picture on and off
bool arm_hires_script_item(const std::string &item, double t);   // key-script aids (!armside, !armhires=0|1, !armhost=0|1)

// the page's and the keyboard's
void arm_hires_set(int level);                           // the page's box (wc_web_arm_hires): 0 off, 1 on
int arm_hires_level();                                   // the picture's level now (0: the game's own)
void arm_hires_flip();                                   // the ` key
bool arm_hires_on();                                     // the picture is 640x400 now
int arm_hires_pass();                                    // the flight's pass now: 0..3 the quarters, 4 the game's own, 5 the
                                                         // cockpit, -1 none (a test aid's)
bool arm_hires_key(int kbdKey, bool pressed);            // a key (KBD_KEYS): true when taken from the game (+ and - in
                                                         // Move Resources click its arrows; they go on to the game too)
bool arm_hires_wheel(int dy);                            // the mouse's wheel (up > 0): true when the map's

}  // namespace wc

void wc_arm_hires_vsync();      // vga_draw.cpp's, while the VGA's drawing is overridden: our lines, when ours

#endif
