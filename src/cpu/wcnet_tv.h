/*
 *  Terminal Velocity (Terminal Reality / 3D Realms, 1995): GAME.EXE, a 32-bit flat program bound with the DOS/4G
 *  extender.  docs/terminalvelocity.md.
 *
 *  TV.EXE (the launcher) loads the sound driver and runs GAME.EXE, whose extender loads the LE image into extended
 *  memory: once DOS has started GAME.EXE, the layer looks for the program now and then (tv_tick) by the bytes of its
 *  code (scripts/tvsig.py's windows, wcnet_tv_sig.h), the same windows giving every data address it uses (read out
 *  of the code's own operands, as the loader relocated them): the shareware's build and any other whose code is the
 *  same but for where things are.  Found, it is watched at a few places through wc_net_watch32 (as TIE Fighter is);
 *  nothing of the Wing Commander layer applies to it (DS stays 0, g_game GAME_NONE, g_session NULL).
 */
#ifndef WCNET_TV_H_
#define WCNET_TV_H_

#include <string>
#include <vector>
#include "dosbox.h"

namespace wc {

// DOS loaded a program (its name without the path, upper case) for the process psp.
void tv_program_loaded(const char *name, Bit16u psp);

// The periodic tick (every 999 instructions): the search for the program while it is awaited, the session's.
void tv_tick();

// A watched EIP's low word came up (the dispatcher, for any 32-bit program): ours, if it is one of our places.
void tv_check_hooks(Bit32u eip);

// The program is in memory, and where (linear addresses).
bool tv_found();

// The flight's facts (all 0 / false while not found).  Positions and angles are the game's: 32-bit world units,
// angles 16-bit (0x10000 a turn) kept in 32-bit words.
bool tv_in_flight();            // a level is being flown (frames seen within the last half second)
struct TvShip {
    Bit32s pos[3];              // x, y, z
    Bit32s ang[3];              // the three angles
    Bit32s vel[3];              // per second (16.16 seconds: the game's frame time)
    Bit32s spin[3];             // the angles' rates
    Bit32s shield;              // 0x10000 full
};
bool tv_player(TvShip *out);
int tv_level();                 // the campaign's level index (-1: none)
std::string tv_level_name();    // its file's name ("" none)
Bit32u tv_frames();             // frames of the level loop so far

// The level's objects (0xB4 bytes each, placed at its load, the same on every machine flying the level).
enum { kTvObjSize = 0xB4, kTvTypeSize = 0x244, kTvMaxObjects = 4096 };
int tv_object_count();
Bit32u tv_object_addr(int i);   // linear (0: none)
int tv_object_class(int i);     // its type's class (7 a craft, 9 an installation, 10 a ground vehicle, 33 a tree...)
std::string tv_object_name(int i);
Bit32u tv_object_type_addr(int i);   // its type's record (linear)

// The game's memory, by linear address (fault-free reads; 0 for a page not there).
Bit32u tv_r32(Bit32u lin);
void tv_w32(Bit32u lin, Bit32u v);

// A call into the program (its functions take their arguments on the stack, the caller cleaning up): made at the
// next safe place (the level loop's frame top or the remote-ship place, `where`), `args` pushed last first; with
// `scratch` (dwords) copied below the stack first, an argument TV_SCRATCH is its address.  `done` gets EAX.
enum { TV_AT_FRAME = 0, TV_AT_DRAW = 1 };
const Bit32u TV_SCRATCH = 0xFFFFFFF0u;
typedef void (*TvCallDone)(int tag, Bit32u eax);
void tv_queue_call(int where, Bit32u fn, const std::vector<Bit32u> &args, const std::vector<Bit32u> &scratch, TvCallDone done, int tag);
bool tv_in_call();              // one of ours is under way (its effects are not the player's)
enum TvFn { TV_FN_FIRE = 0, TV_FN_DRAW_MODEL = 1, TV_FN_HIT = 2, TV_FN_LOAD_MODEL = 3, TV_FN_DESTROY = 4, TV_FN_POD_LOOKUP = 5 };
Bit32u tv_fn(TvFn f);           // the function's linear address (0: none)
Bit32u tv_data(int dataId);     // a tvsig::D_* address (0: none)

// Key-script aids (wcnet_hooks.cpp): "!tv..." items; true when the item was one of ours.
bool tv_script_item(const std::string &item, double t);

// The session (wcnet_tv_session.cpp).
void tv_session_tick();
void tv_session_frame();                            // the level loop's frame top
void tv_session_draw();                             // the remote-ship place: the others' ships into the draw list
bool tv_session_puppet(int obj);                    // an object another machine flies (its update skipped here)
void tv_session_fired(const Bit32u posmat[16], Bit32u weapon, Bit32u damage, Bit32u kind);  // the player's shot
bool tv_session_hit(int obj, Bit32s damage, Bit32u projectile);   // a hit on an object about to be applied: true, apply it
void tv_session_updated(int obj);                   // an object's update made here this frame (not a puppet)
void tv_session_level(int index, const std::string &name);  // a level about to be flown
// The campaign's call of a level: true, held (made again later); *index may be changed (the host's level).
bool tv_session_level_call(int *index);
void tv_session_shutdown();
bool tv_session_active();
bool tv_session_partner(TvShip *out);              // the nearest other pilot flying our level, carried on to now

// The rigs' pilot (wcnet_tv_pilot.cpp): key-script items !tvfly= !tvfire= !tvburn=, run at the frame's top.
void tv_pilot_frame();
bool tv_pilot_item(const std::string &item, double t);

}  // namespace wc

#endif
