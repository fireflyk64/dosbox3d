/*
 *  The TIE Fighter demo's flight at 640x400 (docs/tie640.md).
 *
 *  TIEMISS.EXE draws straight into the VGA's 320x200 (mode 13h).  While its flight runs, the emulator shows a picture
 *  of its own, 640x400, made of the game's 320x200 (the cockpit, the instruments, the text: doubled, the cockpit's
 *  edge smoothed at the finer grid) and of the scene drawn at 640x400.  The game's picture stays what it was: the
 *  game reads and writes it as before.
 */
#ifndef WCNET_TIE640_H_
#define WCNET_TIE640_H_

#include <string>
#include "dosbox.h"
#include "mem.h"

// The VGA's side (vga_memory.cpp, vga_draw.cpp).
extern bool wc_hires_watch;                                 // every write to mode 13h's memory is shown to wc_hires_write
bool wc_hires_write(PhysPt offset, Bitu val, int bytes);    // a write (offset in the picture): true when kept from the VGA
bool wc_hires_read(PhysPt offset, int bytes, Bitu *val);   // a read: true when answered here
void wc_hires_vsync();                                      // the VGA's frame, while the picture is ours (override)

namespace wc {

void tie640_tick(bool flight, Bit16u loadSeg);      // every ~1000 instructions: turns the picture on and off
bool tie640_on();
void tie640_flip();                                 // the ` key: the 640x400 picture off, or on again
void tie640_wall_frame(bool say);        // a flight frame, counted against the host's clock (say: the figures logged now)
void watch_at(Bit16u seg, Bit16u ip);                // (wcnet_hooks.cpp: a place watched in its own code segment only)
void tie640_watch(Bit16u loadSeg);                  // the places hooked, each watched in its own segment
bool tie640_hook(Bit32u ip);
bool tie640_fast_trip(Bit32u ip);                   // (the browser's core: a place of the one pass's, dealt with there)                        // one of them came up: true when it was ours
bool tie640_shot(const std::string &path);          // the 640x400 picture as a PPM (the key script's !shot)
bool tie640_script_item(const std::string &item, double t);

// The stars natively (wcnet_tie640_stars.cpp): rtsvga2_drawstars left at its entry, its stars put into the finished
// 640x378 scene one pixel each; and a check of the same arithmetic against the game's own plots in its 320x200.
bool tie640_stars_native();                         // WCNET_TIE640_STARS=0: the game's routine in each quarter, as before
void tie640_stars_gather(Bit16u loadSeg, int centreX, int centreY, int wide, int deep);    // at the routine's entry
void tie640_stars_skip(Bit16u loadSeg);             // ... which is then left (the far return)
void tie640_stars_plot(Bit8u *scene, int pitch, int wide, int deep);       // the scene complete: the stars gathered
void tie640_stars_watch(void (*watch)(Bit16u ip));  // WCNET_TIESTARS_CHECK=1: the check's places
bool tie640_stars_hook(Bit32u ip, Bit16u loadSeg);
void tie640_stars_say(double t, const std::string &name);   // !t640stars[=name]: the last frame's count and colours, logged (and listed)

// Made fast (wcnet_tie640_fast.cpp): what a quarter costs cut, the picture the same.
void tie640_fast_tick(Bit16u loadSeg);              // every ~1000 instructions while TIEMISS runs (the profile's sample)
void tie640_fast_scene(bool on, int ip);            // the scene section's start (0B1A) or end (1069), the 640 off or on
void tie640_fast_pass(int pass, bool frameDone);    // the quarter about to be drawn (-1: none)
void tie640_fast_watch(void (*watch)(Bit16u ip));   // its own places
void tie640_fast_places(void (*place)(Bit16u para, Bit16u ip));     // ... the same with their segments (image paragraph, offset)
void tie640_fast_hooked(const Bit16u *ips, int n);  // tie640's own places (the translation calls their hooks itself)
bool tie640_fast_script_item(const std::string &item, double t);    // its test aids of the key script
void tie640_fast_run_scene();                       // the CPU is at the scene section's start: on as translated code
bool tie640_fast_hook(Bit32u ip);                   // one of its places came up
// the quarter's own mask written over the opened one (the cockpit's window at the finer grid), when there is room
void tie640_fast_mask(int pass, PhysPt maskAt, const Bit8u *kept, int keptBytes, const Bit8u *window, const Bit8u *from, int wide, int deep, int corner);
bool tie640_fast_again(int pass, const Bit8u *draw, int wide, int deep, int corner);    // the check: true to have the quarter drawn once more
void tie640_fast_frame_begin();                     // the check of whole frames: the scene's start
bool tie640_fast_frame_again(const Bit8u *draw);    // ... its end: true to have the frame's scene drawn once more

bool tie640_fast_reference();                       // the check's second drawing (the game alone) is being made

// The scanline pass hand-written (wcnet_tie640_native.cpp): WCNET_T640_NATIVE=1.
bool tie640_native_on();
// xtrans2_drawxtrans (1CCA:85E9) has been called: the pass done natively into the picture (pitch wide, limitX x limitY,
// this quarter's corner at atX, atY), the routine left
void tie640_native_scanlines(Bit16u loadSeg, Bit8u *draw, int pitch, int limitX, int limitY, int atX, int atY);
void tie640_native_say();
void tie640_native_keep(bool on);                 // the check: the pass's runs kept
void tie640_native_row(int row);                    // ... and those of a row logged

}  // namespace wc

#endif
