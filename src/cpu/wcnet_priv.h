/*
 *  Wing Commander: Privateer (Origin, 1993: the CD-ROM edition as GOG sells it, with Righteous Fire).
 *  docs/privateer.md.
 *
 *  PRIV.EXE is Origin's loader (JEMM, its EMS manager, when the machine has none); it runs PRCD.EXE, a real-mode
 *  Borland C++ overlay program like the Wing Commanders, through DOS, so DOS tells the layer where it is
 *  (priv_program_loaded).  Its world lives mostly in EMS.  Nothing of the Wing Commander layer applies to it: g_game
 *  stays GAME_NONE and DS 0, as for TIE Fighter; this module has its own places, controls and (later) session.
 */
#ifndef WCNET_PRIV_H_
#define WCNET_PRIV_H_

#include <string>
#include "dosbox.h"

namespace wc {

// DOS loaded a program (its name without the path, upper case) at loadSeg for the process psp; that process ended
// (true: it was Privateer's).
void priv_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp);
bool priv_program_ended(Bit16u psp);

// The program runs: its load segment and data segment (DGROUP), 0 while it does not.
bool priv_found();
Bit16u priv_load_seg();
Bit16u priv_ds();
// Righteous Fire (PRCD.EXE started with "r"): its own campaign, the same program.
bool priv_righteous_fire();

// Its data segment, by offset (nothing while it is not found: reads 0).
Bit8u priv_r8(Bit16u off);
Bit16u priv_r16(Bit16u off);
Bit32u priv_r32(Bit16u off);
void priv_w8(Bit16u off, Bit8u v);
void priv_w16(Bit16u off, Bit16u v);

// The periodic tick (every 999 instructions): the report, the controls' frame.
void priv_tick();
// The per-instruction hook found a watched IP (wc_net_watch) while the program runs: true when it was one of ours.
bool priv_check_hooks(Bit32u ip);
// Its places, for the dispatcher's table (build_watch_list): calls watch(ip) for each.
void priv_watch_list(void (*watch)(Bit16u ip));

// A key script item of ours ("!priv..."): true when it was one.
bool priv_script_item(const std::string &item, double t);

// The flight controls (wcnet_priv_controls.cpp): a key the keyboard is about to give the game; true: taken (the
// arrows in flight, ramped as Wing Commander's and RetroVega's are).
bool priv_controls_key(int kbdKey, bool pressed);
// Once a frame of the game's, at seg088:0BAD: the keyboard's routine has filled the control record.
void priv_controls_frame();
// Frames seen (the controls' place), and the game's control mode ("keyboard", "mouse", "joystick").
unsigned priv_controls_frames();
const char *priv_control_mode();
// In flight (the cockpit or a view of space): what the controls ask.
bool priv_in_flight();

}  // namespace wc

#endif
