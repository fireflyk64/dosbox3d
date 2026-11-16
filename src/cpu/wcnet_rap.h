/*
 *  Raptor: Call of the Shadows (Cygnus Studios / Apogee, 1994): RAP.EXE, a 32-bit flat program bound with DOS/4GW
 *  Professional.  docs/raptor.md.
 *
 *  A module of its own, as Terminal Velocity's is (wcnet_tv.cpp): once DOS has started RAP.EXE the program is looked
 *  for in memory by the bytes of its code (scripts/rapsig.py's windows, wcnet_rap_sig.h), every data address read out
 *  of the code's own operands; found, it is watched at a few places through wc_net_watch32.  Nothing else of the
 *  layer applies to it.
 *
 *  Flown together: each pilot's game is its own (its ship, its weapons, its money, its shop), the wave the same
 *  (the host's, launched together), the enemies the same (the wave's script, by the scroll) and their damage shared
 *  (each pilot's damage told and taken off the same enemy everywhere, the money the killer's, the bonus each game's
 *  own to pick up), the other pilots' ships and shots drawn in each game, a pilot blown up the end of the wave for
 *  everybody.
 */
#ifndef WCNET_RAP_H_
#define WCNET_RAP_H_

#include <string>
#include <vector>
#include "dosbox.h"

namespace wc {

// The dispatcher's (wcnet_game.cpp, wcnet_hooks.cpp).
void rap_program_loaded(const char *name, Bit16u psp);     // DOS loaded a program (its name, upper case)
void rap_tick();                                            // every ~1000 instructions
void rap_check_hooks(Bit32u eip);                           // a watched EIP's low word came up
bool rap_fast_hook(Bit32u eip);                             // the places made in the core itself (true: handled)
bool rap_found();
bool rap_script_item(const std::string &item, double t);    // key-script aids: !rapenergy=N, !rapgod=1|0

// The game, for the session (wcnet_rap_session.cpp).
struct RapLocal {
    bool inWave;                // Do_Game under way
    int game, wave;             // the sector and its wave (cur_game, game_wave [cur_game])
    Bit32u diff;                // cur_diff
    int plrdiff;                // curplr_diff
    Bit32u frame;               // frames into the wave
    int x, y;                   // the ship's top left
    Bit32u pic;                 // its frame (an item)
    bool drawn;
    int shield;                 // the energy (0..100)
    int bank;                   // its bank (playerpic: 0..6, 3 level), which spreads its engines' flames
};
bool rap_local(RapLocal *out);
void rap_shots(std::vector<Bit32u> &out, int max);         // our shots, as RapState carries them
int rap_wave_hold_frames();                                 // (the frame hold's lead)
void rap_set_wave(int game, int wave, Bit32u diff, int plrdiff);   // a guest's launch: the host's wave
void rap_take_hits(const std::vector<std::pair<int, int> > &hits);  // others' damage (the enemy's index, the damage)
void rap_blow_up();                                         // our ship's energy to none (another pilot blown up)
bool rap_esc_down();                                        // the game's Esc key down
bool rap_in_wave();                                         // Do_Game under way (the page's pad steers by the arrows)
// a pilot's ship and shots drawn in our frame (Do_Game's draw point), its engines' flames under it (another colour and
// shape than ours: rap_draw_flames), its shadow put among the game's own (rap_add_shadow: drawn by SHADOW_DisplaySky)
void rap_draw_flames(int bank, int x, int y);
void rap_add_shadow(Bit32u pic, int x, int y);
void rap_draw_ship(Bit32u pic, int x, int y, int seat);     // (its stripes its seat's colour)
void rap_draw_shots(const std::vector<Bit32u> &shots, int shipX, int shipY);

// The session's (wcnet_rap_session.cpp), for the game's hooks.
void rap_session_tick();
void rap_session_shutdown();
bool rap_session_active();                  // in a room with somebody
// the launch: true to hold (the others not there yet); *go set once it may go on (the wave set by then)
bool rap_session_launch_arrive();
bool rap_session_launch_go();               // asked by the hold's idle: may it go on
void rap_session_wave_start();              // Do_Game entered
void rap_session_wave_end();                // Do_Game left
Bit32u rap_session_run();                   // the wave flown together (0: alone)
int rap_session_seat();                     // our seat in the room (0 the host: the ship's stripes by it)
bool rap_session_hold_frame(Bit32u frame);  // the frame hold: ahead of a pilot of the same run
void rap_session_damage(int index, int damage);    // our damage to an enemy (sent at the next tick)
void rap_session_dead();                    // our ship blown up
void rap_session_draw();                    // the others' ships and shots, at the draw point
void rap_session_shadows();                 // the others' ships' shadows, before the game draws its shadows
unsigned rap_session_drawn();               // the others' ships drawn since the last asked

}  // namespace wc

#endif
