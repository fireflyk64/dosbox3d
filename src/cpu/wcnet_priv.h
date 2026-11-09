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
#include <vector>
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
// In flight: the player's ship exists (ds:66B4; 0 at a base, set in flight, the nav map's pauses too).
bool priv_in_flight();
// The player's ship object (a near pointer, ds:66B4; 0 at a base): a new one is a new flight.
Bit16u priv_player_ship();
// The player's ship's place (world units: 256 to the game's metre; z up), zeros when not flying.
void priv_player_pos(Bit32s pos[3]);
// The name of the system the player flies in (the game's own copy, as its displays show it), "" when not flying.
std::string priv_system_name();

// The player's attitude (3x3 int32, 1.0 = 256; identity when not flying) and appearance ("CLUNKER" the Tarsus, "TUG"
// the Orion, "FIGHTER" the Centurion, "MERCHANT" the Galaxy; "" when not flying).
void priv_player_matrix(Bit32s m[9]);
std::string priv_player_appearance();

// Ships of ours in the game's space (puppets): a ship of that type (TARSTYPE...), appearance and cast name (its AI and
// side: MER_DP a merchant's), placed by the game's own seg099:0074 at the next frame (false: not in flight, or the
// queue full); done is told the object (a near pointer, as ds:66B4's; 0: none made).
bool priv_spawn_ship(const char *type, const char *appearance, const char *cast, const Bit32s pos[3], const Bit32s m[9],
                     void (*done)(int tag, Bit16u obj), int tag);
// Its AI held (its think returns at once) or let go; none held (a flight's end).
bool priv_hold_ai(Bit16u controller, bool hold);
void priv_ship_set_target(Bit16u ship, Bit16u target);
void priv_ship_anger(Bit16u ship);                         // angry at the player, as a hit from the player makes it (at the next frame)    // its AI's target (written at the next frame: the plan is far data)    // false: not held (the list is full)
void priv_hold_ai_none();
void priv_let_think(Bit16u controller, int frames);   // a held controller thinks for this many frames (its pilot speaks)
bool priv_thinking(Bit16u controller);
Bit16u priv_ship_controller(Bit16u obj);
Bit16u priv_ship_type(Bit16u obj);
// The object is still a ship of that type (not destroyed and its memory taken for another).
bool priv_ship_alive(Bit16u obj, Bit16u type);
// Its place and attitude (m may be NULL: the place alone).
void priv_ship_set(Bit16u obj, const Bit32s pos[3], const Bit32s m[9]);

// The nav spheres of the system the player flies in (the game's list, ds:66C2: the nav points, ids 0..7, their
// centres in world units and radii in the game's metres), and the one the player (or any place) is inside, or -1.
struct PrivSphere {
    int id;
    Bit32s x, y, z;
    int radius;
};
int priv_spheres(PrivSphere out[], int cap);
void priv_spheres_refresh();
unsigned priv_mission_navs();       // the nav points (bits) of our accepted missions' objectives (read at the frame)        // (at the frame's top)
int priv_nav_at(const Bit32s pos[3]);
// Every ship in the scene (the game's list ds:66CE; the player's too), as near pointers.
int priv_scene_ships(Bit16u out[], int cap);
// A ship's names (type "TALNWIMP", appearance "TALMIL", cast "PIR_DA"), its place, attitude, armour (the four
// quadrants of its damage model), whether it lives; its death (the game's own: the alive flag cleared, the game
// explodes and removes it); its guns fired (the control record's fire bit, held a frame).
std::string priv_ship_type_name(Bit16u obj);
std::string priv_ship_appearance_name(Bit16u obj);
std::string priv_ship_cast_name(Bit16u obj);
void priv_ship_pos(Bit16u obj, Bit32s pos[3]);
void priv_ship_matrix(Bit16u obj, Bit32s m[9]);
bool priv_ship_is_alive(Bit16u obj);
void priv_ship_armor(Bit16u obj, Bit32s a[4]);
void priv_ship_set_armor(Bit16u obj, const Bit32s a[4]);
void priv_ship_kill(Bit16u obj);
void priv_ship_fire(Bit16u obj, bool on);
// A ship of the game's speaks a line of ours in the game's own comms (the user, 2026-11-08: the room's chat "would
// show up in comms as a merchant face"): the name and the first line of each of its cast's received sets are
// rewritten in the ship's own copy of the cast, and its pilot told to say line 1.  False: no cast (not a ship of a
// cast with lines).
bool priv_ship_say(Bit16u obj, const char *name, const char *text, int face);   // face: the cast's portrait id, -1 as it is
// Its face: the portrait id its cast copy holds (chosen when the ship was made: a merchant's 14, a woman, or 47, a
// man), -1 none; and the ship taken out of the scene without a sound (the game's own removal, queued).
int priv_ship_face(Bit16u obj);
// The last ship a spawn of the key script's made (test aids).
Bit16u priv_last_spawned();
// A hit applied to a ship of ours (our own, or an encounter ship we run) the game's way: the shields take it
// (seg101:0CF0), the rest goes to the armour (0C7D), the cockpit's damage display for our own ship, and a quadrant
// gone kills it.
void priv_hit_apply(Bit16u obj, int loc, int damage);
unsigned priv_session_pilot_missions(int seat);
bool priv_session_pilot_pos(int seat, Bit32s out[3]);      // (a rig aid's: a pilot's latest reported place)   // (a rig aid's: a pilot's mission nav points as we know them)

// The session's side of the hooks: may this encounter wing (at that nav sphere) spawn here; a hit decided here on a
// ship (the target's near pointer, the quadrant, the damage); a ship of the game's fired its guns here.
bool priv_session_allow_wing(int sphereId);
// Either ship is another machine's (a puppet).
bool priv_session_is_puppet(Bit16u a, Bit16u b);
void priv_session_hit(Bit16u target, int loc, int damage);
void priv_session_fired(Bit16u ship);
// The room's chat: a line typed here (the 0 key's prompt) to the others; true when a session takes it.
bool priv_session_chat(const std::string &text);

// The game's near heap: the bytes free between its top and the frame's stack (a ship costs some 600).
int priv_near_free();
void priv_near_heap(int &total, int &largest, int &blocks);
bool priv_near_heap_read(int &total, int &largest, int &blocks);   // false: the ring not to be trusted at this moment
int priv_near_heap_lowest();                                        // the lowest free seen in flight, -1 none yet
int priv_near_heap_low_take();                                      // the lowest free since the last call (the governor's)
void priv_session_counts(int &pilots, int &npcs, int &pods);        // our puppets made in this flight
void priv_calls_flush(const char *why);                            // every queued call of ours dropped (the flight is over)
void priv_session_our_death();
int priv_session_wing_extras(int sphereId, const char *cast, int count);   // a wing about to spawn: how many more ships it gets
void priv_session_wing_extended(int sphereId, int extras);
void priv_wings_flight_changed();                                    // (our ship was destroyed: the others are told)
Bit16u priv_session_pilot_ship(int seat);                          // a pilot's ship in our game (ours or its puppet), 0 none
void priv_session_force_release();                                // (a test aid: one puppet given back at the next frame)   // the game's near heap: free bytes, its largest free block, blocks
void priv_ship_delete(Bit16u obj, int mode);
// Cargo pods (type CDBRTYPE, vtable 0x1584: section 13): a ship's layout up to the alive flag; at +0x4B a list of
// cargo records (far; each given as its 0x38-byte block: the commodity's name, then the record), at +0x56 the units
int priv_scene_pods(Bit16u out[], int cap);
bool priv_scene_has(Bit16u obj);                               // the object is in the scene list
bool priv_pod_is(Bit16u obj);
bool priv_pod_cargo(Bit16u obj, std::vector<std::vector<Bit8u> > &records, int &units);   // (at the frame: far data)
void priv_pod_fill(Bit16u obj, const std::vector<std::vector<Bit8u> > &records, int units);   // the pod given that cargo (the game's far heap)
void priv_pod_set(Bit16u obj, const Bit32s pos[3]);                   // (a test aid: the game's destructor on a ship of ours)

// The session (wcnet_priv_session.cpp): the room, presence; its tick (from priv_tick, every 999 instructions) and an
// occasion to say where we are (1 the autopilot engaged).
void priv_session_tick();
void priv_session_event(int why);
// Once a frame in flight (seg088:0B2E's entry, before the player's controls): the puppets placed, our state sent.
void priv_session_frame();
bool priv_session_active();

}  // namespace wc

#endif
