// Made by scripts/wolfsig.py from the shareware WOLF3D.EXE (Wolfenstein 3-D 1.4, Apogee): do not edit.
// Each anchor: a run of the program's code (mask 0: a byte that differs from build to build -- a segment the
// loader relocates, a call's target, a data address), found once in the running program; each item a 16-bit
// operand in one; each place an instruction the hooks watch.  The comments' addresses are the shareware image's.
#ifndef WCNET_WOLF_SIG_H
#define WCNET_WOLF_SIG_H
namespace wc { namespace wolfsig {
struct Anchor { const char *name; int len; const Bit8u *bytes; const Bit8u *mask; int key; int keyLen; };
enum Kind { K_DATA, K_IMM, K_REL, K_FAROFF, K_FARSEG };
struct Item { const char *name; int anchor; int at; Kind kind; };
struct Place { const char *name; int anchor; int at; int seg; };     // seg: the item that is its code segment (-1: none)
enum AnchorId {
    A_NEWGAME = 0,     // 044E:036B NewGame (difficulty, episode): gamestate cleared, 100 health, 8 bullets, 3 lives
    A_VIEWSIZE = 1,     // 044E:13AC SetViewSize (width, height): viewwidth (a multiple of 16), viewheight, centerx, shootdelta
    A_TEDLEVEL = 2,     // 044E:15DB DemoLoop: the TEDLEVEL start (episode and map from tedlevelnum), GameLoop () called
    A_SETUPLEVEL = 3,     // 0B56:068B SetupGameLevel (): the level's counts cleared unless a game was loaded
    A_SETUPRND = 4,     // 0B56:06B6 SetupGameLevel: the random table's start (a demo's: the same every time), the map read
    A_DIED = 5,     // 0B56:1125 Died (): a life less (never under TEDLEVEL), health, weapons and keys as at a start
    A_GAMELOOP = 6,     // 0B56:11CD GameLoop: PlayLoop () called for the level
    A_LEVELDONE = 7,     // 0B56:1238 GameLoop: PlayLoop left, by playstate; a level completed: the keys gone, DrawKeys ()
    A_INITACTORS = 8,     // 0C8E:09DE InitActorList (): the free list, the last object, the player the first one made
    A_FRAME = 9,     // 0C8E:1039 PlayLoop's frame: madenoise = false, MoveDoors (), MovePWalls (), DoActor (obj) for every object
    A_FRAME_END = 10,     // 0C8E:1112 PlayLoop's frame: while (!playstate && !startgame), and the loop left
    A_DAMAGE = 11,     // 1019:0EF0 DamageActor (ob, damage): madenoise, twice the damage when not attacking, KillActor or FirstSighting
    A_KNIFE = 12,     // 1193:0F0F KnifeAttack: DamageActor (closest, US_RndT () / 6)
    A_STATICS = 13,     // 12B2:000B InitStaticList (), SpawnStatic (tilex, tiley, type): laststatobj, statinfo, spotvis
    A_CONNECT = 14,     // 12B2:01B3 ConnectAreas (): areabyplayer cleared, the player's area's connections followed
    A_OPENDOOR = 15,     // 12B2:033C OpenDoor (door): an open door's time again, any other started opening
    A_SPAWNDOOR = 16,     // 12B2:0263 SpawnDoor (tilex, tiley, vertical, lock): the door's record, its number | 0x80 in actorat and tilemap
    A_DOOROPEN = 17,     // 12B2:062D DoorOpen (door): its time open counted in tics, closed after 300
    A_MOVEDOORS = 18,     // 12B2:08E8 MoveDoors (): every door by its action, unless gamestate.victoryflag
    A_PUSHWALL = 19,     // 12B2:093B PushWall (checkx, checky, dir): none while one moves (pwallstate), the tile's wall
    A_PUSHWALL_GO = 20,     // 12B2:0A7B PushWall: gamestate.secretcount++, pwallx, pwally, pwalldir, pwallstate = 1, pwallpos = 0, the info plane's mark gone
    A_COUNT = 21
};
static const Bit8u k_newgame_bytes[54] = { 0x55, 0x8b, 0xec, 0x57, 0xbf, 0xe0, 0x8b, 0x33, 0xc0, 0x1e, 0x07, 0x8a, 0xe0, 0xb9, 0x21, 0x00, 0xf3, 0xab, 0x8b, 0x46, 0x06, 0xa3, 0xe0, 0x8b, 0xb8, 0x01, 0x00, 0xa3, 0xfc, 0x8b, 0xa3, 0xf8, 0x8b, 0xa3, 0xfa, 0x8b, 0xc7, 0x06, 0xf2, 0x8b, 0x64, 0x00, 0xc7, 0x06, 0xf4, 0x8b, 0x08, 0x00, 0xc7, 0x06, 0xf0, 0x8b, 0x03, 0x00 };
static const Bit8u k_newgame_mask[54] = { 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1 };
static const Bit8u k_viewsize_bytes[48] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x02, 0x8b, 0x46, 0x06, 0x25, 0xf0, 0xff, 0xa3, 0xbc, 0x8e, 0x8b, 0x46, 0x08, 0x25, 0xfe, 0xff, 0xa3, 0xba, 0x8e, 0xa1, 0xbc, 0x8e, 0x99, 0x2b, 0xc2, 0xd1, 0xf8, 0x48, 0xa3, 0xb8, 0x8e, 0xa1, 0xbc, 0x8e, 0xbb, 0x0a, 0x00, 0x99, 0xf7, 0xfb, 0xa3, 0xb6, 0x8e };
static const Bit8u k_viewsize_mask[48] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0 };
static const Bit8u k_tedlevel_bytes[27] = { 0xa1, 0xaf, 0x91, 0xbb, 0x0a, 0x00, 0x99, 0xf7, 0xfb, 0xa3, 0x06, 0x8c, 0xa1, 0xaf, 0x91, 0x99, 0xf7, 0xfb, 0x89, 0x16, 0xe2, 0x8b, 0x9a, 0x82, 0x11, 0x56, 0x0b };
static const Bit8u k_tedlevel_mask[27] = { 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0 };
static const Bit8u k_setuplevel_bytes[35] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x0e, 0x56, 0x57, 0x83, 0x3e, 0xad, 0x91, 0x00, 0x75, 0x1c, 0x33, 0xc0, 0xa3, 0x0a, 0x8c, 0xa3, 0x0c, 0x8c, 0xa3, 0x08, 0x8c, 0xa3, 0x10, 0x8c, 0xa3, 0x12, 0x8c, 0xa3, 0x0e, 0x8c };
static const Bit8u k_setuplevel_mask[35] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0 };
static const Bit8u k_setuprnd_bytes[51] = { 0x83, 0x3e, 0x08, 0x99, 0x00, 0x75, 0x07, 0x83, 0x3e, 0x0a, 0x99, 0x00, 0x74, 0x0c, 0x6a, 0x00, 0x9a, 0x08, 0x00, 0x3b, 0x1c, 0x83, 0xc4, 0x02, 0xeb, 0x0a, 0x6a, 0x01, 0x9a, 0x08, 0x00, 0x3b, 0x1c, 0x83, 0xc4, 0x02, 0xa1, 0x06, 0x8c, 0xba, 0x0a, 0x00, 0xf7, 0xea, 0x8b, 0x16, 0xe2, 0x8b, 0x03, 0xd0, 0x52 };
static const Bit8u k_setuprnd_mask[51] = { 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1 };
static const Bit8u k_died_bytes[56] = { 0x83, 0x3e, 0xb1, 0x91, 0x00, 0x75, 0x04, 0xff, 0x0e, 0xf0, 0x8b, 0x83, 0x3e, 0xf0, 0x8b, 0xff, 0x7e, 0x47, 0xc7, 0x06, 0xf2, 0x8b, 0x64, 0x00, 0xb8, 0x01, 0x00, 0xa3, 0xfc, 0x8b, 0xa3, 0xf8, 0x8b, 0xa3, 0xfa, 0x8b, 0xc7, 0x06, 0xf4, 0x8b, 0x08, 0x00, 0xc7, 0x06, 0xf6, 0x8b, 0x00, 0x00, 0x33, 0xc0, 0xa3, 0x04, 0x8c, 0xa3, 0x02, 0x8c };
static const Bit8u k_died_mask[56] = { 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0 };
static const Bit8u k_gameloop_bytes[56] = { 0xc7, 0x06, 0x8c, 0x96, 0x01, 0x00, 0x9a, 0xeb, 0x0a, 0x8e, 0x0c, 0x9a, 0xe3, 0x02, 0xa1, 0x18, 0x0b, 0xf6, 0x75, 0x07, 0x9a, 0xd9, 0x0e, 0x32, 0x0a, 0xeb, 0x02, 0x33, 0xf6, 0xc7, 0x06, 0x1a, 0x99, 0x01, 0x00, 0x9a, 0xc0, 0x03, 0x93, 0x11, 0x9a, 0xa1, 0x0f, 0x8e, 0x0c, 0x9a, 0xb3, 0x0a, 0x8e, 0x0c, 0xc7, 0x06, 0x8c, 0x96, 0x00, 0x00 };
static const Bit8u k_gameloop_mask[56] = { 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1 };
static const Bit8u k_leveldone_bytes[18] = { 0xd1, 0xe3, 0x2e, 0xff, 0xa7, 0x75, 0x13, 0xc7, 0x06, 0xf6, 0x8b, 0x00, 0x00, 0x9a, 0x7d, 0x04, 0x93, 0x11 };
static const Bit8u k_leveldone_mask[18] = { 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0 };
static const Bit8u k_initactors_bytes[38] = { 0xc7, 0x06, 0x94, 0xce, 0x00, 0x00, 0xc7, 0x06, 0x68, 0xab, 0x6e, 0xab, 0xc7, 0x06, 0x6a, 0xab, 0x00, 0x00, 0xc7, 0x06, 0x72, 0x9a, 0x00, 0x00, 0x90, 0x0e, 0xe8, 0x09, 0x00, 0xa1, 0xdc, 0x8b, 0xa3, 0xda, 0x8b, 0x5e, 0x5d, 0xcb };
static const Bit8u k_initactors_mask[38] = { 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1 };
static const Bit8u k_frame_bytes[52] = { 0xc7, 0x06, 0x96, 0xce, 0x00, 0x00, 0x9a, 0xe8, 0x08, 0xb2, 0x12, 0x9a, 0xd5, 0x0a, 0xb2, 0x12, 0xa1, 0xda, 0x8b, 0xa3, 0x6c, 0xab, 0xeb, 0x15, 0xff, 0x36, 0x6c, 0xab, 0x0e, 0xe8, 0xbc, 0xfd, 0x83, 0xc4, 0x02, 0x8b, 0x1e, 0x6c, 0xab, 0x8b, 0x47, 0x38, 0xa3, 0x6c, 0xab, 0x83, 0x3e, 0x6c, 0xab, 0x00, 0x75, 0xe4 };
static const Bit8u k_frame_mask[52] = { 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1 };
static const Bit8u k_frame_end_bytes[28] = { 0x83, 0x3e, 0xde, 0x8b, 0x00, 0x75, 0x0a, 0x83, 0x3e, 0xb4, 0x8e, 0x00, 0x75, 0x03, 0xe9, 0xee, 0xfe, 0x83, 0x3e, 0xde, 0x8b, 0x02, 0x74, 0x04, 0x0e, 0xe8, 0xbd, 0xfc };
static const Bit8u k_frame_end_mask[28] = { 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0 };
static const Bit8u k_damage_bytes[72] = { 0x55, 0x8b, 0xec, 0x56, 0x8b, 0x76, 0x06, 0x8b, 0x56, 0x08, 0xc7, 0x06, 0x96, 0xce, 0x01, 0x00, 0x8a, 0x44, 0x08, 0xb4, 0x00, 0xa9, 0x10, 0x00, 0x75, 0x02, 0xd1, 0xe2, 0x8b, 0x44, 0x2c, 0x2b, 0xc2, 0x89, 0x44, 0x2c, 0x83, 0x7c, 0x2c, 0x00, 0x7f, 0x0b, 0x56, 0x0e, 0xe8, 0x0b, 0xfd, 0x83, 0xc4, 0x02, 0x5e, 0x5d, 0xcb, 0x8a, 0x44, 0x08, 0xb4, 0x00, 0xa9, 0x10, 0x00, 0x75, 0x09, 0x56, 0x90, 0x0e, 0xe8, 0x57, 0x04, 0x83, 0xc4, 0x02 };
static const Bit8u k_damage_mask[72] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1 };
static const Bit8u k_knife_bytes[27] = { 0x9a, 0x30, 0x00, 0x3b, 0x1c, 0xbb, 0x06, 0x00, 0x99, 0xf7, 0xfb, 0x8b, 0xd0, 0x52, 0x57, 0x9a, 0xf0, 0x0e, 0x19, 0x10, 0x83, 0xc4, 0x04, 0x5f, 0x5e, 0xc9, 0xcb };
static const Bit8u k_knife_mask[27] = { 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1 };
static const Bit8u k_statics_bytes[63] = { 0x55, 0x8b, 0xec, 0xc7, 0x06, 0x58, 0x7f, 0x5a, 0x7f, 0x5d, 0xcb, 0x55, 0x8b, 0xec, 0x56, 0x8b, 0x56, 0x06, 0x8b, 0x4e, 0x08, 0x8b, 0x76, 0x0a, 0x8b, 0xde, 0xc1, 0xe3, 0x02, 0x8b, 0x87, 0x2e, 0x19, 0x8b, 0x1e, 0x58, 0x7f, 0x89, 0x47, 0x04, 0x88, 0x17, 0x88, 0x4f, 0x01, 0x8b, 0xc2, 0xc1, 0xe0, 0x06, 0x03, 0xc1, 0x05, 0x68, 0x9b, 0x89, 0x47, 0x02, 0x8b, 0xde, 0xc1, 0xe3, 0x02 };
static const Bit8u k_statics_mask[63] = { 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 };
static const Bit8u k_connect_bytes[54] = { 0x55, 0x8b, 0xec, 0x57, 0xbf, 0xfa, 0x48, 0x33, 0xc0, 0x1e, 0x07, 0x8a, 0xe0, 0xb9, 0x25, 0x00, 0xf3, 0xab, 0x8b, 0x1e, 0xda, 0x8b, 0x8a, 0x47, 0x1c, 0xb4, 0x00, 0xd1, 0xe0, 0x8b, 0xd8, 0xc7, 0x87, 0xfa, 0x48, 0x01, 0x00, 0x8b, 0x1e, 0xda, 0x8b, 0x8a, 0x47, 0x1c, 0xb4, 0x00, 0x50, 0x0e, 0xe8, 0x81, 0xff, 0x83, 0xc4, 0x02 };
static const Bit8u k_connect_mask[54] = { 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1 };
static const Bit8u k_opendoor_bytes[41] = { 0x55, 0x8b, 0xec, 0x56, 0x8b, 0x46, 0x06, 0xba, 0x0a, 0x00, 0xf7, 0xea, 0x8b, 0xf0, 0x8b, 0xd8, 0x83, 0xbf, 0xde, 0x7c, 0x00, 0x75, 0x09, 0xc7, 0x84, 0xe0, 0x7c, 0x00, 0x00, 0x5e, 0x5d, 0xcb, 0xc7, 0x84, 0xde, 0x7c, 0x02, 0x00, 0x5e, 0x5d, 0xcb };
static const Bit8u k_opendoor_mask[41] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1 };
static const Bit8u k_spawndoor_bytes[82] = { 0x8b, 0x1e, 0x2e, 0xd1, 0xd1, 0xe3, 0xc7, 0x87, 0x46, 0x49, 0x00, 0x00, 0x8b, 0x1e, 0xe3, 0xd8, 0x8a, 0x46, 0x06, 0x88, 0x07, 0x8a, 0x46, 0x08, 0x88, 0x47, 0x01, 0x89, 0x77, 0x02, 0x8a, 0x46, 0x0c, 0x88, 0x47, 0x04, 0xc7, 0x47, 0x06, 0x01, 0x00, 0xa1, 0x2e, 0xd1, 0x0d, 0x80, 0x00, 0x8b, 0x5e, 0x06, 0xc1, 0xe3, 0x07, 0x8b, 0x56, 0x08, 0xd1, 0xe2, 0x03, 0xda, 0x89, 0x87, 0xd8, 0x4b, 0x8b, 0x5e, 0x06, 0xc1, 0xe3, 0x06, 0x03, 0x5e, 0x08, 0xa0, 0x2e, 0xd1, 0x0c, 0x80, 0x88, 0x87, 0xd8, 0x6b };
static const Bit8u k_spawndoor_mask[82] = { 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0 };
static const Bit8u k_dooropen_bytes[46] = { 0x55, 0x8b, 0xec, 0x56, 0x8b, 0x4e, 0x06, 0x8b, 0xc1, 0xba, 0x0a, 0x00, 0xf7, 0xea, 0x8b, 0xf0, 0x8b, 0xd8, 0x8b, 0x87, 0xe0, 0x7c, 0x03, 0x06, 0x1c, 0x99, 0x89, 0x84, 0xe0, 0x7c, 0x3d, 0x2c, 0x01, 0x7c, 0x08, 0x51, 0x0e, 0xe8, 0x10, 0xfd, 0x83, 0xc4, 0x02, 0x5e, 0x5d, 0xcb };
static const Bit8u k_dooropen_mask[46] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1 };
static const Bit8u k_movedoors_bytes[41] = { 0x55, 0x8b, 0xec, 0x56, 0x57, 0x83, 0x3e, 0x20, 0x8c, 0x00, 0x75, 0x43, 0x33, 0xf6, 0xbf, 0xde, 0x7c, 0x3b, 0x36, 0x2e, 0xd1, 0x7d, 0x38, 0x8b, 0x05, 0x0b, 0xc0, 0x74, 0x0c, 0x3d, 0x02, 0x00, 0x74, 0x11, 0x3d, 0x03, 0x00, 0x74, 0x16, 0xeb, 0x1c };
static const Bit8u k_movedoors_mask[41] = { 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const Bit8u k_pushwall_bytes[48] = { 0x55, 0x8b, 0xec, 0x56, 0x57, 0x8b, 0x4e, 0x06, 0x8b, 0x76, 0x08, 0x8b, 0x7e, 0x0a, 0x83, 0x3e, 0x44, 0x49, 0x00, 0x74, 0x04, 0x5f, 0x5e, 0x5d, 0xcb, 0x8b, 0xd9, 0xc1, 0xe3, 0x06, 0x03, 0xde, 0x8a, 0x87, 0xd8, 0x6b, 0xb4, 0x00, 0x8b, 0xd0, 0x0b, 0xd2, 0x75, 0x04, 0x5f, 0x5e, 0x5d, 0xcb };
static const Bit8u k_pushwall_mask[48] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static const Bit8u k_pushwall_go_bytes[82] = { 0xff, 0x06, 0x08, 0x8c, 0x89, 0x0e, 0xf8, 0x48, 0x89, 0x36, 0xf6, 0x48, 0x89, 0x3e, 0xf4, 0x48, 0xc7, 0x06, 0x44, 0x49, 0x01, 0x00, 0xc7, 0x06, 0xc6, 0x49, 0x00, 0x00, 0x8b, 0xd9, 0xc1, 0xe3, 0x06, 0x03, 0xde, 0x80, 0x8f, 0xd8, 0x6b, 0xc0, 0x8b, 0x1e, 0xf6, 0x48, 0xd1, 0xe3, 0x8b, 0x9f, 0x58, 0x7c, 0xd1, 0xe3, 0x8e, 0x06, 0x66, 0x9a, 0xa1, 0xf8, 0x48, 0xd1, 0xe0, 0x03, 0xd8, 0x26, 0xc7, 0x07, 0x00, 0x00, 0x6a, 0x2e, 0x9a, 0x99, 0x15, 0xb7, 0x19, 0x83, 0xc4, 0x02, 0x5f, 0x5e, 0x5d, 0xcb };
static const Bit8u k_pushwall_go_mask[82] = { 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1 };
static const Anchor kAnchors[A_COUNT] = {
    { "newgame", 54, k_newgame_bytes, k_newgame_mask, 7, 15 },
    { "viewsize", 48, k_viewsize_bytes, k_viewsize_mask, 0, 10 },
    { "tedlevel", 27, k_tedlevel_bytes, k_tedlevel_mask, 3, 7 },
    { "setuplevel", 35, k_setuplevel_bytes, k_setuplevel_mask, 0, 10 },
    { "setuprnd", 51, k_setuprnd_bytes, k_setuprnd_mask, 21, 8 },
    { "died", 56, k_died_bytes, k_died_mask, 22, 6 },
    { "gameloop", 56, k_gameloop_bytes, k_gameloop_mask, 25, 6 },
    { "leveldone", 18, k_leveldone_bytes, k_leveldone_mask, 0, 5 },
    { "initactors", 38, k_initactors_bytes, k_initactors_mask, 22, 5 },
    { "frame", 52, k_frame_bytes, k_frame_mask, 32, 5 },
    { "frame_end", 28, k_frame_end_bytes, k_frame_end_mask, 4, 5 },
    { "damage", 72, k_damage_bytes, k_damage_mask, 14, 31 },
    { "knife", 27, k_knife_bytes, k_knife_mask, 5, 11 },
    { "statics", 63, k_statics_bytes, k_statics_mask, 9, 22 },
    { "connect", 54, k_connect_bytes, k_connect_mask, 7, 13 },
    { "opendoor", 41, k_opendoor_bytes, k_opendoor_mask, 0, 18 },
    { "spawndoor", 82, k_spawndoor_bytes, k_spawndoor_mask, 16, 26 },
    { "dooropen", 46, k_dooropen_bytes, k_dooropen_mask, 0, 20 },
    { "movedoors", 41, k_movedoors_bytes, k_movedoors_mask, 21, 20 },
    { "pushwall", 48, k_pushwall_bytes, k_pushwall_mask, 0, 16 },
    { "pushwall_go", 82, k_pushwall_go_bytes, k_pushwall_go_mask, 59, 12 },
};
enum ItemId {
    I_GAMESTATE = 0,     // gamestate (66 bytes: difficulty, mapon, the scores, lives, health, ammo, keys, weapons, ... episode, the counts) (8BE0, 95C2)
    I_VIEWWIDTH = 1,     // viewwidth: the 3-D view's width in pixels (centred above the status bar) (8EBC, 52D0)
    I_VIEWHEIGHT = 2,     // viewheight (8EBA, 52CE)
    I_TEDLEVELNUM = 3,     // tedlevelnum (the TEDLEVEL argument: 10 * episode + map) (91AF, 9B87)
    I_GAMELOOP_OFF = 4,     // GameLoop () (1182, 1158)
    I_SEG_GAME = 5,     // WL_GAME's code segment (0B56, 0BD2)
    I_LOADEDGAME = 6,     // loadedgame (91AD, 9B85)
    I_DEMOPLAYBACK = 7,     // demoplayback (9908, A2B6)
    I_DEMORECORD = 8,     // demorecord (990A, A2B8)
    I_TEDLEVEL = 9,     // tedlevel: started by TEDLEVEL (no life is ever lost) (91B1, 9B89)
    I_PLAYLOOP_OFF = 10,     // PlayLoop () (0FA1, 0F5A)
    I_SEG_PLAY = 11,     // WL_PLAY's code segment (0C8E, 0D07)
    I_DRAWKEYS_OFF = 12,     // DrawKeys () (047D, 044E)
    I_SEG_AGENT = 13,     // WL_AGENT's code segment (1193, 11E3)
    I_OBJFREELIST = 14,     // objfreelist (AB68, B516)
    I_OBJLIST = 15,     // objlist [MAXACTORS] (60 bytes each) (AB6E, B51C)
    I_LASTOBJ = 16,     // lastobj (AB6A, B518)
    I_OBJCOUNT = 17,     // objcount (9A72, A420)
    I_GETNEWACTOR = 18,     // GetNewActor (): the next free object, linked last, in `new` (0D2E4, 0DA30)
    I_NEWOBJ = 19,     // new (8BDC, 95BE)
    I_PLAYER = 20,     // player (8BDA, 95BC)
    I_ACTORAT = 21,     // actorat [64][64] (words: an object, or a wall's or door's number) (4BD8, 55BA)
    I_MADENOISE = 22,     // madenoise (CE96, D844)
    I_MOVEDOORS_OFF = 23,     // MoveDoors () (08E8, 08B6)
    I_SEG_ACT1 = 24,     // WL_ACT1's code segment (12B2, 12F3)
    I_DOACTOR = 25,     // DoActor (ob) (0D6F5, 0DE41)
    I_TICS = 26,     // tics: the frame's length, 70ths of a second (991C, A2CA)
    I_PLAYSTATE = 27,     // playstate (0 playing, 1 completed, 2 died, 3 warped, 6 victorious, 9 the secret level) (8BDE, 95C0)
    I_STARTGAME = 28,     // startgame: a new game asked for (the menu's) (8EB4, 9892)
    I_DAMAGE_OFF = 29,     // DamageActor (ob, damage) (0EF0, 0E52)
    I_SEG_STATE = 30,     // WL_STATE's code segment (1019, 1084)
    I_KILLACTOR = 31,     // KillActor (ob) (10DBA, 113E1)
    I_FIRSTSIGHTING = 32,     // FirstSighting (ob) (1151C, 11ABC)
    I_LASTSTATOBJ = 33,     // laststatobj (7F58, 893A)
    I_STATOBJLIST = 34,     // statobjlist [MAXSTATS] (8 bytes each) (7F5A, 893C)
    I_STATINFO = 35,     // statinfo [] (a static's picture and kind, 4 bytes each) (192E, 1A1E)
    I_SPOTVIS = 36,     // spotvis [64][64] (9B68, A516)
    I_AREABYPLAYER = 37,     // areabyplayer [NUMAREAS] (words) (48FA, 52DC)
    I_DOORACTION = 38,     // doorobjlist [0].action (a door 10 bytes: tilex, tiley, vertical, lock, action, ticcount) (7CDE, 86C0)
    I_DOORPOSITION = 39,     // doorposition [MAXDOORS] (words: 0 closed .. 0xFFFF open) (4946, 5328)
    I_VICTORYFLAG = 40,     // gamestate.victoryflag (8C20, 9602)
    I_DOORNUM = 41,     // doornum: the level's doors (D12E, D85C)
    I_PWALLSTATE = 42,     // pwallstate (0: no wall moving) (4944, 5326)
    I_TILEMAP = 43,     // tilemap [64][64] (bytes) (6BD8, 75BA)
    I_PWALLX = 44,     // pwallx (48F8, 52DA)
    I_PWALLY = 45,     // pwally (48F6, 52D8)
    I_PWALLDIR = 46,     // pwalldir (0 north, 1 east, 2 south, 3 west) (48F4, 52D6)
    I_PWALLPOS = 47,     // pwallpos (49C6, 53A8)
    I_FARMAPYLOOKUP = 48,     // farmapylookup [64] (a map row's offset) (7C58, 863A)
    I_MAPSEGS1 = 49,     // mapsegs [1]: the info plane's segment (mapsegs [0], the walls and areas, the word before) (9A66, A414)
    I_COUNT = 50
};
static const Item kItems[I_COUNT] = {
    { "gamestate", A_NEWGAME, 5, K_IMM },
    { "viewwidth", A_VIEWSIZE, 13, K_DATA },
    { "viewheight", A_VIEWSIZE, 22, K_DATA },
    { "tedlevelnum", A_TEDLEVEL, 1, K_DATA },
    { "gameloop_off", A_TEDLEVEL, 23, K_FAROFF },
    { "seg_game", A_TEDLEVEL, 25, K_FARSEG },
    { "loadedgame", A_SETUPLEVEL, 10, K_DATA },
    { "demoplayback", A_SETUPRND, 2, K_DATA },
    { "demorecord", A_SETUPRND, 9, K_DATA },
    { "tedlevel", A_DIED, 2, K_DATA },
    { "playloop_off", A_GAMELOOP, 41, K_FAROFF },
    { "seg_play", A_GAMELOOP, 43, K_FARSEG },
    { "drawkeys_off", A_LEVELDONE, 14, K_FAROFF },
    { "seg_agent", A_LEVELDONE, 16, K_FARSEG },
    { "objfreelist", A_INITACTORS, 8, K_DATA },
    { "objlist", A_INITACTORS, 10, K_IMM },
    { "lastobj", A_INITACTORS, 14, K_DATA },
    { "objcount", A_INITACTORS, 20, K_DATA },
    { "getnewactor", A_INITACTORS, 27, K_REL },
    { "newobj", A_INITACTORS, 30, K_DATA },
    { "player", A_INITACTORS, 33, K_DATA },
    { "actorat", A_SPAWNDOOR, 62, K_DATA },
    { "madenoise", A_FRAME, 2, K_DATA },
    { "movedoors_off", A_FRAME, 7, K_FAROFF },
    { "seg_act1", A_FRAME, 9, K_FARSEG },
    { "doactor", A_FRAME, 30, K_REL },
    { "tics", A_DOOROPEN, 24, K_DATA },
    { "playstate", A_FRAME_END, 2, K_DATA },
    { "startgame", A_FRAME_END, 9, K_DATA },
    { "damage_off", A_KNIFE, 16, K_FAROFF },
    { "seg_state", A_KNIFE, 18, K_FARSEG },
    { "killactor", A_DAMAGE, 45, K_REL },
    { "firstsighting", A_DAMAGE, 67, K_REL },
    { "laststatobj", A_STATICS, 5, K_DATA },
    { "statobjlist", A_STATICS, 7, K_IMM },
    { "statinfo", A_STATICS, 31, K_DATA },
    { "spotvis", A_STATICS, 53, K_IMM },
    { "areabyplayer", A_CONNECT, 5, K_IMM },
    { "dooraction", A_OPENDOOR, 18, K_DATA },
    { "doorposition", A_SPAWNDOOR, 8, K_DATA },
    { "victoryflag", A_MOVEDOORS, 7, K_DATA },
    { "doornum", A_MOVEDOORS, 19, K_DATA },
    { "pwallstate", A_PUSHWALL, 16, K_DATA },
    { "tilemap", A_PUSHWALL, 34, K_DATA },
    { "pwallx", A_PUSHWALL_GO, 6, K_DATA },
    { "pwally", A_PUSHWALL_GO, 10, K_DATA },
    { "pwalldir", A_PUSHWALL_GO, 14, K_DATA },
    { "pwallpos", A_PUSHWALL_GO, 24, K_DATA },
    { "farmapylookup", A_PUSHWALL_GO, 48, K_DATA },
    { "mapsegs1", A_PUSHWALL_GO, 54, K_DATA },
};
enum PlaceId {
    P_SETUPLEVEL = 0,     // 0B56:068B SetupGameLevel's entry
    P_FRAME = 1,     // 0C8E:1039 the frame, the controls read (tics made), before the doors and the actors
    P_LOOP_EXIT = 2,     // 0C8E:1123 PlayLoop left (playstate says why)
    P_DAMAGE = 3,     // 1019:0EF0 DamageActor's entry
    P_PUSHWALL = 4,     // 12B2:093B PushWall's entry
    P_COUNT = 5
};
static const Place kPlaces[P_COUNT] = {
    { "setuplevel", A_SETUPLEVEL, 0, I_SEG_GAME },
    { "frame", A_FRAME, 0, I_SEG_PLAY },
    { "loop_exit", A_FRAME_END, 17, I_SEG_PLAY },
    { "damage", A_DAMAGE, 0, I_SEG_STATE },
    { "pushwall", A_PUSHWALL, 0, I_SEG_ACT1 },
};
} }
#endif
