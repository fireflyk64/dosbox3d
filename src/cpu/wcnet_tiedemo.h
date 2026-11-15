/*
 *  The TIE Fighter demo (LucasArts, early 1994): TIEMISS.EXE, the demo's flight program, a 16-bit real-mode program
 *  (Microsoft C, overlays in the EXE, EMS).  docs/tiedemo.md.
 *
 *  A module of its own, as Privateer's is (wcnet_priv.cpp): DOS loads TIEMISS.EXE (TIEDEMO.BAT: "tiemiss demo"), the
 *  layer knows its load segment from the start, and the build is checked by its version string in the image.  Nothing
 *  of the CD version's TIE Fighter hooks (wcnet_tie*.cpp, a 32-bit DOS/4GW program) applies to it.
 */
#ifndef WCNET_TIEDEMO_H_
#define WCNET_TIEDEMO_H_

#include <string>
#include "dosbox.h"

namespace wc {

// The dispatcher's (wcnet_game.cpp, wcnet_hooks.cpp).
void tdm_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp);  // DOS loaded a program
void tdm_tick();                                    // every ~1000 instructions
bool tdm_found();                                   // TIEMISS.EXE loaded and known
bool tdm_check_hooks(Bit32u ip);                    // a watched IP came up: true when it was one of ours
void tdm_watch_list(void (*watch)(Bit16u ip));      // our IPs, for the dispatcher's table
bool tdm_script_item(const std::string &item, double t);   // key-script aids (!tdm...)

// The game, for the session (wcnet_tiedemo_session.cpp).
struct TdmLocal {
    bool inFlight;              // the flight's frames coming (TIEMISS.EXE's frame function)
    Bit32u frame;               // frames into the flight
    std::string rec;            // our craft's record, as TdmState carries it
};
bool tdm_local(TdmLocal *out);
double tdm_frame_ms();          // a frame's length, measured (the game's own rate)
// another pilot's craft put in our flight: the `k`-th wingman of our flight group (0 the first) given the record
// `rec` carried, its place carried on by its own motion for `frames` frames since it was told
bool tdm_put_pilot(int k, const std::string &rec, double frames);
// a guest takes its own place in the flight group: our craft and the `k`-th wingman change places (their places and
// attitudes), at the flight's start
bool tdm_take_place(int k);

// The session's (wcnet_tiedemo_session.cpp), for the game's hooks.
void tdm_session_tick();        // every ~1000 instructions
void tdm_session_frame();       // each frame of the flight, after the game moved everything: ours told, the others put
void tdm_session_shutdown();

}  // namespace wc

#endif
