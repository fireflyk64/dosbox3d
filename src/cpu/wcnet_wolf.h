/*
 *  Wolfenstein 3-D (id Software / Apogee, 1992): WOLF3D.EXE, a 16-bit real-mode program (Borland C, medium model),
 *  the shareware episode and the six-episode game alike.  docs/wolf3d.md.
 *
 *  The game has no network game of its own (DOOM's port, wcnet_doom.cpp, needed nothing hooked): this is a port in
 *  the way Raptor's and Terminal Velocity's are.  The program is looked for in memory by the bytes of its code
 *  (scripts/wolfsig.py's anchors, wcnet_wolf_sig.h: the same code in every 1.4 build, at other addresses), every data
 *  address read out of the code's own operands, and watched at five places: the level's set-up, the frame, the
 *  frame loop's end, DoActor and DamageActor.
 *
 *  Played together: every player's game runs the same level, each its own (its player, its weapons, its pick-ups).
 *  The level's actors are the same everywhere (the order the level made them is their number).  An actor is thought
 *  for by one machine at a time -- the one whose player it went for -- and shown on the others where that machine
 *  says it is; hurt by anybody, its owner's game takes the damage; killed, it falls on every machine.  Doors, pushed
 *  walls, keys and the level's end are everybody's.  The other players are drawn as actors of the game's own (B.J.'s
 *  running sprites, the ones the episode's ending uses).  The session is wcnet_wolf_session.cpp.
 */
#ifndef WCNET_WOLF_H_
#define WCNET_WOLF_H_

#include <string>
#include <vector>
#include "dosbox.h"

namespace wc {

// The dispatcher's (wcnet_game.cpp, wcnet_hooks.cpp).
void wolf_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp);     // DOS loaded a program (its name, upper case)
void wolf_tick();                                   // every ~1000 instructions
bool wolf_watching();                               // WOLF3D.EXE runs (found or still looked for)
bool wolf_found();                                  // ... and its places are known
bool wolf_check_hooks(Bit32u ip);                   // a watched IP came up: true when it was one of ours
void wolf_watch_list(void (*watch)(Bit16u ip));     // our IPs, for the dispatcher's table
bool wolf_script_item(const std::string &item, double t);   // key-script aids (!wolf...)
bool wolf_in_play();                                // a level's frames are coming (the page walks by the arrows)
bool wolf_key(int key, bool pressed);               // a key on its way to the game: true to keep it (Esc while a guest's level is held)

enum { kWolfMaxPlayers = 4, kWolfMaxActors = 150, kWolfMaxDoors = 64, kWolfAreaTile = 107 };

// The others marked over the picture (sdlmain.cpp's partner markers): fractions of the picture.
struct WolfMarker { bool on; double x, y, size; bool onScreen; char label[48]; };
const WolfMarker *wolf_markers();

// --- the game, for the session (wcnet_wolf_session.cpp) --------------------------------------------------------------

struct WolfLevel {
    int episode, map, difficulty;
    WolfLevel() : episode(0), map(0), difficulty(0) {}
    bool operator==(const WolfLevel &o) const { return episode == o.episode && map == o.map && difficulty == o.difficulty; }
    bool operator!=(const WolfLevel &o) const { return !(*this == o); }
};
WolfLevel wolf_level();                             // the game's own (gamestate)
void wolf_set_level(const WolfLevel &l);            // written there (before a level is set up)
bool wolf_has_level(const WolfLevel &l);            // the game's data files hold it (the shareware: episode one)
void wolf_reload();                                 // the frame loop left as the game's own level warp leaves it: the
                                                    // level in gamestate set up anew
void wolf_end_level(int playstate);                 // the frame loop left the way the game leaves it (1 completed, 9 to
                                                    // the secret level, 6 the episode won)
std::string wolf_build();                           // the program and its data, for "the same game?"

struct WolfBody {
    Bit32u x, y;                // 16.16 tiles
    int angle;                  // degrees
    int health, attack;
};
WolfBody wolf_player();

// The level's actors: the objects after the player, in the order the level made them.
enum {
    WOLF_SHOOTABLE = 0x01, WOLF_NEVERMARK = 0x04, WOLF_VISABLE = 0x08, WOLF_ATTACKMODE = 0x10, WOLF_FIRSTATTACK = 0x20,
    WOLF_AMBUSH = 0x40, WOLF_NONMARK = 0x80,
};
struct WolfAct {
    int cls;                    // obclass (3 guard, 4 officer, 5 SS, 6 dog, 7 Hans Grosse, ... 2: a thing that does nothing)
    Bit32u x, y;
    int tilex, tiley, dir;
    int state;                  // from the first guard state
    int ticcount, flags, hp;
    Bit32s distance;
};
int wolf_actors();
Bit32u wolf_level_sig();                            // what the level's actors are and where they began
bool wolf_actor(int i, WolfAct *out);
// an actor shown as its owner has it: its place, its state (taken: its countdowns too, ours to think for from here)
void wolf_actor_show(int i, const WolfAct &a, bool taken);
// an actor nobody is after yet put where the host's game has it on its round (ours goes on thinking for it)
void wolf_actor_place(int i, const WolfAct &a);
void wolf_actor_puppet(int i, bool on);             // DoActor passes it by (thought for elsewhere)
bool wolf_actor_is_puppet(int i);
bool wolf_actor_fighter(int cls);                   // a class that fights and falls (not a corpse, a ghost, a shot)
void wolf_hurt(int i, int damage);                  // the game's DamageActor called for it (at the frame's place)
// the game's KillActor called for it where it fell; quiet (a level come into): its fall run to its end, no points
void wolf_kill(int i, Bit32u x, Bit32u y, bool quiet);

// doors (the game's: 0 open, 1 closed, 2 opening, 3 closing)
int wolf_doors();
int wolf_door_action(int d);
void wolf_door_open(int d);                         // as the game's OpenDoor
void wolf_door_keep(int d);                         // an open door's time begun again
bool wolf_door_at(int tilex, int tiley, int *d);
// pushed walls
bool wolf_push_moving(int *x, int *y, int *dir);    // one moves now (where it was pushed from, the way)
void wolf_push(int x, int y, int dir, int area);    // the game's PushWall called for it; the tiles it leaves get the pusher's area
void wolf_push_done(int x, int y, int dir, int area);   // ... as it ends, at once (a level come into)
int wolf_player_area();
// keys (bit 0 gold, bit 1 silver)
int wolf_keys();
void wolf_give_keys(int bits);
// the other players: an actor each (slot 0..2), B.J. walking; hidden when not in the level
void wolf_avatar(int slot, bool shown, Bit32u x, Bit32u y, bool walking, const char *label);
void wolf_notice(const std::string &text, double ms = 5000);   // a line over the picture for a while

// --- the session's, for the game's hooks ----------------------------------------------------------------------------
void wolf_session_tick();                   // every ~1000 instructions while WOLF3D.EXE runs
void wolf_session_shutdown();
bool wolf_session_configured();             // a room (or a rig's TCP port) to play in
// SetupGameLevel's entry (asked again while held): true to hold there (a guest whose host's level is not known yet);
// the room's level written into the game's by the time it says false
bool wolf_session_level_hold(bool esc);
void wolf_session_frame(bool fresh);        // the frame (fresh: the level's first)
void wolf_session_loop_exit(int playstate); // the frame loop left
bool wolf_session_damage(int i, int damage);    // DamageActor for actor i: true when told to its owner instead
void wolf_session_idle();                   // no frame for a while (a menu, a death, the level's end)
std::string wolf_session_status();          // (a rig's: !wolfstat)
std::string wolf_session_report(int i);     // (a rig's: !wolfact: actor i as last told to us, and how long ago)

}  // namespace wc

#endif
