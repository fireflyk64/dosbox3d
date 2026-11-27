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
// WCPRIVCLASSIC=1 (the page's ?privclassic=1): the game as it was before 2026-11-22's work -- no high-resolution
// picture, no native twins, the frame wait spun and the empty game port polled as the game does (docs section 22).
bool priv_classic();
// The places looked at inside the CPU's core, as they are in the browser too (none of them waits): the high-resolution
// picture's drawing calls, then the native twins and the idled frame wait.  True: nothing else is to look at this one.
bool priv_core_hook(Bit32u ip);
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
// At seg088:0BBD, where every control mode's reading meets: a built-in pad picks JOYSTICK, an empty port's JOYSTICK
// steers by the arrows, the page's stick steers in any mode.
void priv_controls_join();
// The page's stick (a pad not built in, the touch stick): -1..1, right and climbing positive; said every 16 ms.
void priv_controls_stick(double x, double y);
// The page's pointer choice (WCPRIVPOINTER=ring, the default, or game; wc_web_priv_pointer while running): the
// pointer drawn where the game shows none or a dark one.
bool priv_pointer_shown();
void priv_pointer_set(bool shown);
// The page's "Invert mouse" (WCPRIVMOUSEINVERT=1, wc_web_priv_mouse_invert while running): true, the game's own pitch
// (the mouse pushed up dives); false, the default, pushed up climbs.
bool priv_mouse_inverted();
void priv_mouse_invert_set(bool inverted);
// The mouse flying the ship (the options' MOUSE): where to draw its pointer, shares of the picture (false: none to draw).
bool priv_controls_marker(double *x, double *y);
// The game's own pointer where its palette makes it dark (the options screen over a flight, Alt+O): its pixels, a box of
// 16 x 16 of the game's 320 x 200 picture from x0, y0 (a row's bit 15 the leftmost), to draw white (false: none).
bool priv_pointer_pixels(int *x0, int *y0, Bit16u rows[16]);
// The game's frame was seen lately (its controls' place, seg088:0B2E): the flight runs, no menu over it.
bool priv_flight_frame_recent();
// Frames seen (the controls' place), and the game's control mode ("keyboard", "mouse", "joystick").
unsigned priv_controls_frames();
// When the pilot last pressed T (the target key) in flight (emulated ms; -1e9 never).
double priv_controls_target_key_at();
const char *priv_control_mode();
// In flight: the player's ship exists (ds:66B4; 0 at a base, set in flight, the nav map's pauses too).
bool priv_in_flight();
// The player's ship object (a near pointer, ds:66B4; 0 at a base): a new one is a new flight.
Bit16u priv_player_ship();
// The ship the player's targeting system is on (0: none).
Bit16u priv_player_target();
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
bool priv_hold_ai(Bit16u controller, bool hold);      // false: not held (the list is full)
Bit16u priv_ship_target_now(Bit16u ship);                   // the ship its AI is after (0: none known)
// An AI ship's target as its brain has it (the pilot's +0x13, its +0x12; 0: none, or no AI brain to vouch for), and set by
// the game's own setter of kept pointers at the next frame (false: not an AI ship we can vouch for).
Bit16u priv_ai_target(Bit16u ship);
bool priv_ai_set_target(Bit16u ship, Bit16u target);
int priv_kept_pointers();                                   // the game's kept pointers taken, of 200
// Up to n live bolts retired (their alive byte cleared: the game takes them out itself): rank 0's first, then 1's, then
// 2's, at random within a rank.
int priv_bolts_retire(int n, int (*rank)(Bit16u shooter), int by[3]);
int priv_calls_queued();                                    // our calls of the game's functions waiting, of 16
int priv_ship_pilot_anger(Bit16u ship);                     // its pilot's anger at the player (+0x1A, 0x100 its mood's bit 1); -1 none
// The ships that fired the bolts now in our scene, one a bolt.
int priv_scene_bolt_shooters(Bit16u out[], int cap);
// Its guns (those groups: the control record's +0x1E bits 3-6) fired by the game's weapons update at the next frame (a copy's shot).
bool priv_ship_fire_guns(Bit16u obj, int groups);
// A ship's guns as descriptions (each gun's first kGunBytes bytes: wcnet_priv.cpp), read at the frame; and a copy of a
// pilot's ship given a pilot's guns: as many of its own as there are rewritten to them, the rest never fired (their
// station one no seat fires from); the ones switched on in group 0, the others in group 3.  Both at the frame.
enum { kGunBytes = 0x36 };
// A ship's afterburner lit (the control record's +0x1E bit 0), and a copy's set so (its flames: section 39); at the frame.
bool priv_ship_afterburner(Bit16u obj);
void priv_ship_set_afterburner(Bit16u obj, bool on);
int priv_ship_gun_list(Bit16u ship, std::vector<std::string> &out);
int priv_ship_set_guns(Bit16u ship, const std::vector<std::string> &guns);
// (A test aid's) angry at the player, as a hit from the player makes it (at the next frame).
void priv_ship_anger(Bit16u ship);
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
// Its four quadrants' shields (the damage model's int32 at +0x31 + 6q), and written (a puppet's, from its owner's report).
void priv_ship_shields(Bit16u obj, Bit32s s[4]);
void priv_ship_set_shields(Bit16u obj, const Bit32s s[4]);
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
bool priv_session_pilot_pos(int seat, Bit32s out[3]);
void priv_session_test_provoke();                         // (a test aid's: a provocation of the nearest of others' encounter ships)
bool priv_session_puppet_of(Bit16u obj, int *seat, Bit32u *id);   // (whose a puppet is: the seat, its id there, 0 the pilot's ship)
bool priv_session_puppet_parked(Bit16u obj);           // (a puppet parked far off)      // (a rig aid's: a pilot's latest reported place)   // (a rig aid's: a pilot's mission nav points as we know them)

// The session's side of the hooks: may this encounter wing (at that nav sphere) spawn here; a hit decided here on a
// ship (the target's near pointer, the quadrant, the damage); a ship of the game's fired its guns here.
bool priv_session_allow_wing(int sphereId);
// Either ship is another machine's (a puppet).
bool priv_session_is_puppet(Bit16u a, Bit16u b);
// The game's dead-ship handler ran for this ship (its death, wcnet_priv.cpp): one of ours is reported dead, not gone.
void priv_session_ship_died(Bit16u ship);
// A puppet dying as we give it back (or parked far off): its explosion's sound left out.
bool priv_session_silent_death(Bit16u ship);
// A hit in our game, before the damage reaches the model: target, the ship behind it (the shooter, or the other ship of a
// bump), the quadrant and the damage; returns the damage our game keeps (a hit on another machine's ship is sent to it).
int priv_session_hit(Bit16u target, Bit16u source, bool bump, int loc, int damage);
void priv_session_fired(Bit16u ship, int groups, Bit16u target);
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
// A line on the emulator's own line over the picture (what cannot be said in the game).
void priv_session_notice(const std::string &text);

// Every record of the system's sphere list (ds:66C2; at the frame: far data): the nav spheres (kind 0xFF) and what sits
// in them -- a jump point (kind 2, `what` the system it leads to) or a base (kind 1) -- with the system's own id.
struct PrivPlace {
    int id, system, kind, what, nav, radius;
    Bit32s x, y, z;
    Bit32u rec;                 // the record's linear address
};
int priv_places(PrivPlace out[], int cap);
void priv_place_set_what(const PrivPlace &p, int what);
// The view (ds:143C: 0 forward, 1 and 2 the sides, 3 back, 4 and 5 turrets 1 and 2).
int priv_view();
// The player's ship's turrets (k = 1, 2; ds:143D, ds:143F: 0 none): its angles (256 a degree) and its attitude within
// the ship (3x3, 1.0 = 256); false: the ship has no such turret.
bool priv_turret(int k, Bit32s *yaw, Bit32s *pitch, Bit32s m[9]);
void priv_turret_set(int k, Bit32s yaw, Bit32s pitch, const Bit32s m[9]);
// A ship's guns by seat (a gun's station: 0 the pilot's, 1 and 2 the turrets'): that seat's switched on and every other
// seat's off (-1: all off); how many that seat has.
int priv_guns_seat(Bit16u ship, int station);
// The player's ship's guns of that seat fired once by the game's own routine at the next frame (a gunner's shot shown in
// the pilot's game: no energy taken, its bolts harmless here); false: not queued.
bool priv_fire_station(int station);
// The player's ship's power moved between its shields and its guns' energy, none made or lost (both are kept in the
// same units, 256 to a point: the shields' four quadrants in the damage model, the guns' pool in the ship's energy
// object).  kind 1: the four quadrants made even (their sum kept); 2: up to `points` from the shields (each quadrant
// its share) to the guns, as far as the guns' pool has room; 3: up to `points` from the guns to the shields (the
// quadrants with room, the emptiest first).  Returns the points moved (kind 1: the largest change of a quadrant).
int priv_ship_power(int kind, int points);
// The player's ship's guns' energy now and at most (x 256), -1 not known.
Bit32s priv_ship_energy(Bit32s *max);
// Comms.  A ship's comm record (its controller's +8) holds the message the player's comm menu chose for it (+6, 0xFFFF
// none: its pilot's next think takes it, seg096:03FF, and answers): taken out (-1 none), and put in.  And the line on
// the comm now (ds:67E4: its speaker, its text as shown, "NAME: text"; false: none).
int priv_ship_comm_take(Bit16u ship);
void priv_ship_comm_put(Bit16u ship, int msg);
bool priv_comm_line(Bit16u *speaker, std::string *text);
// The bolt (or whatever hit) is one of those copies'.
bool priv_copy_bolt(Bit16u hitter);
// A key pressed and let go for the game by the layer itself (past the crew's key filter).
void priv_tap_key(int kbdKey);
bool priv_tapping();

// The crew (wcnet_priv_crew.cpp, docs/privateer.md section 30): DOS's file calls while the program runs -- a file opened
// or created (its full name on its drive, the handle's entry), read into or written from the caller's buffer, closed.
// About to open (before the drive is asked: the file may be written first).
void priv_file_opening(const char *fullname, Bit8u drive, Bit8u flags);
void priv_file_opened(const char *fullname, Bit16u entry, bool created);
void priv_file_read(Bit16u entry, Bit8u *data, Bit16u amount);
void priv_file_written(Bit16u entry, const Bit8u *data, Bit16u amount);
void priv_file_closed(Bit16u entry);
// Its tick (from priv_tick).
void priv_crew_tick();
// At stub273:0020, the game about to build the base's ship (ovr273:0000).
void priv_crew_ship_load();
bool priv_crew_script_item(const std::string &item, double t);
// The room's kind and our part in it (the session's word, at its hello): role 0 none (everybody flies their own ship),
// 1 the crew's pilot, 2 a gunner in the pilot's turret `turret` (1, 2; more gunners than turrets ride as passengers).
void priv_crew_role(int role, int turret);
bool priv_crew_gunner();
bool priv_crew_pilot();
// The pilot's ship as its game wrote it at its last launch (the pilot's side: to send; serial 0 none yet), and taken (a
// gunner's side: the ship its next flight is flown in).
unsigned priv_crew_own_ship(std::string *file, int *hull);
void priv_crew_take_ship(const std::string &file, int hull, unsigned serial);
// The pilot as the room tells of it (a gunner's side): where it is (its presence) and its ship's state (15 and more a
// second while it flies).
void priv_crew_pilot_presence(bool flying, const std::string &system, int systemId, int base, int baseSystem, const std::vector<int> &path);
// (the pilot's side) the base we are at or launched from, its system, and the systems flown through since (the launch's first).
void priv_crew_own_way(int *base, int *baseSystem, std::vector<int> *path);
void priv_crew_pilot_state(const std::string &system, const double pos[3], const double vel[3], const Bit32s m[9], const Bit32s armor[4],
                           const Bit32s shield[4], bool damage, bool destroyed);
// Once a frame in flight (before the session's): a gunner's ship put where the pilot's is, its view and guns its turret's.
void priv_crew_frame();
// A gunner's turret to tell the pilot (false: nothing to tell now), and told (the pilot's side: seat, turret...).
bool priv_crew_turret_report(int *turret, Bit32s *yaw, Bit32s *pitch, Bit32s m[9], bool *fire, unsigned *shots);
void priv_crew_take_turret(int turret, Bit32s yaw, Bit32s pitch, const Bit32s m[9], bool fire, unsigned shots);
// A gunner's keys: true, not the game's (the ship is the pilot's to fly).
bool priv_crew_key(int kbdKey, bool pressed);
// A crew member's key for the pilot's ship sent to the pilot (1 the shields evened, 2 shields to guns, 3 guns to shields);
// and taken there.
void priv_session_crew_key(int key);
void priv_crew_take_key(int seat, int key);
// A gunner's control record after the controls were read (seg088:0BBD): no missile is the gunner's to launch.
void priv_crew_controls(Bit32u rec);
// A gunner's game spawns no wings (the pilot's does), and nothing hits its copy of the ship (the pilot's game decides).
bool priv_crew_no_wings();
bool priv_crew_hit(Bit16u target, Bit16u source);
// Our system's id as the game's sphere records have it (0: not known; read at the frame).
int priv_crew_system_id();

}  // namespace wc

#endif
