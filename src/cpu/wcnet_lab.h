/*
 *  The walkthrough video's recorder (WCLAB=<dir>): everything the video's
 *  memory and debugger panels are drawn from, recorded as the game runs, so
 *  that how they look is decided afterwards (walkthrough/ in the repository's
 *  root reads the file).  Off unless WCLAB names a directory.
 *
 *  <dir>/rec.bin is a stream of chunks, each
 *      type (1 byte) · payload length (u32) · emulated ms (f64) · payload
 *  in the order things happened (nothing is compressed: a minute is some
 *  250 MB, which the video's tools pack away):
 *      'V' a picture as the renderer finished it (every vertical retrace):
 *          u16 width, u16 height, u8 bits per pixel, then the
 *          palette (768 bytes, 8-bit channels) and the pixels
 *      'S' the same picture again
 *      'A' sound as the mixer made it: u32 rate, u32 frames, s16 stereo
 *      'M' the data segment at the top of a game frame: u16 frame counter,
 *          u16 DS, u16 load segment, then the 64K
 *      'X' all of the first megabyte: name, NUL, the 1M
 *      'E' an event, one line of JSON (pokes, marks, traced instructions,
 *          watched writes, the 2016 injection)
 *      'T' TIE Fighter's flight tables at the top of a frame (tie_updatetime):
 *          u32 code base, u32 data base (linear), u32 frame, u32 regions,
 *          then each region: u32 image address, u32 length, its bytes
 *          (the objects, the CraftData, pstate, the clock, ...)
 *      'Y' all of the machine's memory (16 MB): name, NUL, the bytes (TIE
 *          Fighter's: the moment the program is found, "found")
 */
#ifndef WCNET_LAB_H_
#define WCNET_LAB_H_

#include <string>
#include "dosbox.h"

// For the renderer and the mixer (outside the wc namespace).
extern bool wclab_on;
void WCLAB_VideoFrame(Bitu width, Bitu height, Bitu bpp, Bitu pitch, const Bit8u *data, const Bit8u *pal);
void WCLAB_Audio(Bit32u rate, Bitu frames, const Bit16s *stereo);

namespace wc {

// At the top of every game frame (the main loop's hook), and again at the
// end of the hooks' work there (the 2016 injection waits for that).
void lab_game_frame();
void lab_after_frame_hooks();
// True while every instruction is wanted (a trace or a watch is running).
bool lab_per_instruction();
void lab_instruction();
// The key script's lab items ("!add32=", "!pokes=", "!dump=", "!mark=",
// "!trace=", "!watch=", "!inject2016"); false for anything else.
bool lab_script_item(const std::string &item, double t);
// An event of one's own into the recording (one line of JSON), while one is made.
void lab_event(const std::string &json);

// TIE Fighter (the videos of docs/tiefighter.md's post): a scan for the program (its number, the first dword 20000 seen
// above 1 MB, found or not: found, all of memory 'Y' too); the flight's tables at every frame's top ('T'); a stick's
// axis as the game scaled it and signed ('axis').  The key script's TIE items: "!tiecall=<fn>:<eax>:<edx>:<ebx>:<ecx>"
// (hex: a call into the program by the trampoline, its result an event), "!tiefire=<obj>[:<hardpoint>[:<type>]]"
// (laser_createprojectile for that craft), "!tiepoke=<image>:<hex bytes>", "!dumpall=<name>".
void lab_tie_scan(unsigned attempt, Bit32u firstRank, bool found, Bit32u code, Bit32u data);
void lab_tie_frame();
void lab_tie_axis(bool across, int delta, int scale, int game, int fixed, bool bug);

}  // namespace wc

#endif
