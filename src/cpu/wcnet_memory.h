/*
 *  Wing Commander 1 (WC.EXE) data-segment map and typed accessors.
 *
 *  Every offset below is named after the symbol in the IDA database export
 *  WcMulti/wcmulti.idc where one exists; look any of them up with
 *      scripts/wcdis.py sym dseg:XXXX
 *  Names in [brackets] are ours, chosen from the disassembly of the code that
 *  uses the array (the IDA name for that address is given after it).
 *
 *  The programs use the Borland "compact" convention SS == DS, which is why
 *  stack arguments are read through DS_OFF + esp.
 */
#ifndef WCNET_MEMORY_H_
#define WCNET_MEMORY_H_

#include <string>
#include "dosbox.h"
#include "mem.h"
#include "wcnet_game.h"

namespace wc {

// The data segment (DGROUP) of the game that is running and the linear address
// of its offset 0: WC.EXE loaded first by DOSBox's shell has DS = 13D3.  Zero
// while no known game runs (wcnet_game.h).
extern Bit16u DS;
extern PhysPt DS_OFF;

// Entity slots.  The game keeps 0x40 entity slots (WC2: 0x46); every
// per-entity array is indexed by slot.  Slot 0 is always the local player.
enum Slot {
    kPlayerSlot = 0,
    kMinShipSlot = 1,       // ships, capships and missiles live in 1..9
    kMaxShipSlot = 9,
    kMinTempSlot = 10,      // bolts, explosions, dust, ... live from 10 to max_temp_slot()
    kInvalidSlot = 0xffff,
};

// dseg:BD1A maybeEntityType values.
enum EntityType {
    ET_NONE = 0,
    ET_NAVPOINT = 2,
    ET_NAVICON = 3,
    ET_DUST = 4,
    ET_EXPLOSION = 5,
    ET_SPARKLE = 6,
    ET_ENGINE = 7,
    ET_BOLT = 8,
    ET_ASTEROID = 9,
    ET_MINE = 0xa,
    ET_MISSILE = 0xb,
    ET_SHIP = 0xc,
    ET_CAPSHIP = 0xd,
};

// Ship types (dseg:BFF8 gInstanceShipTypes, and word 0 of a mission ship) that
// are not ships: a mission "ship" of one of these types is an asteroid or
// mine field (ovr145:1183 registers it with ovr168:0B40 instead of spawning).
enum FieldShipType {
    ST_ASTEROID_FIELD = 0x16,
    ST_MINE_FIELD = 0x17,
};

namespace ds {
// One variable per line of wcnet_ds.def, holding the offset in the game that
// is running (wcnet_game.cpp loads them); 0 = that game has no such thing, or
// it is not located yet.
#define WC_DS(name, wc1, wc2) extern Bit16u name;
#include "wcnet_ds.def"
#undef WC_DS
inline bool known(Bit16u offset) { return offset != 0; }
}  // namespace ds

struct Vec3 {
    Bit32s x, y, z;
};

inline PhysPt addr(Bit16u dsOffset) { return DS_OFF + dsOffset; }

inline Bit8u rd8(Bit16u off) { return mem_readb(addr(off)); }
inline Bit16u rd16(Bit16u off) { return mem_readw(addr(off)); }
inline Bit32u rd32(Bit16u off) { return mem_readd(addr(off)); }
inline void wr8(Bit16u off, Bit8u v) { mem_writeb(addr(off), v); }
inline void wr16(Bit16u off, Bit16u v) { mem_writew(addr(off), v); }
inline void wr32(Bit16u off, Bit32u v) { mem_writed(addr(off), v); }

inline Vec3 read_vec(Bit16u off) {
    Vec3 v;
    v.x = (Bit32s)rd32(off);
    v.y = (Bit32s)rd32(off + 4);
    v.z = (Bit32s)rd32(off + 8);
    return v;
}
inline void write_vec(Bit16u off, const Vec3 &v) {
    wr32(off, (Bit32u)v.x);
    wr32(off + 4, (Bit32u)v.y);
    wr32(off + 8, (Bit32u)v.z);
}
inline Bit16u vec_slot(Bit16u base, int slot) { return (Bit16u)(base + 12 * slot); }

// Per-slot accessors -------------------------------------------------------
inline Bit16u entity_type(int slot) { return rd16((Bit16u)(ds::maybeEntityType + 2 * slot)); }
inline bool slot_in_use(int slot) { return entity_type(slot) != ET_NONE; }
inline int parent_of(int slot) { return rd8((Bit16u)(ds::parentShipForShip + slot)); }
inline bool is_ship_slot(int slot) { return slot >= kPlayerSlot && slot <= kMaxShipSlot; }
// The last slots are the game's own: with 0x40 of them (WC.EXE) bolts,
// explosions, dust... end at 0x3c, the camera is 0x3d and 0x3f is a scratch
// vector; WC2.EXE has 0x46 and the same three at the end.
inline int num_slots() { return g_params.slots; }
inline int max_temp_slot() { return g_params.slots - 4; }
inline int camera_slot() { return g_params.slots - 3; }
#define kNumSlots (wc::num_slots())
#define kMaxTempSlot (wc::max_temp_slot())
#define kCameraSlot (wc::camera_slot())
inline bool is_temp_slot(int slot) { return slot >= kMinTempSlot && slot <= kMaxTempSlot; }

// Follow dseg:C30E until a slot is its own parent (or has none); this is what
// ovr143:1D97 getTopLevelParent does.
int top_level_parent(int slot);

// Number of free temporary slots (bolts, explosions...).
int free_temp_slots();

// Find a live bolt (ET_BOLT) whose top-level parent is `owner`, or -1.
int find_bolt_of(int owner);

// Asteroid and mine fields.  The game keeps at most 20 rocks or mines, placed
// around slot 0 while it is inside a field (overlay 168), so every machine
// has its own: they are never replicated, only the damage they do is.
bool is_field_mission_ship(int missionShip);
// True when `slot` is, or descends from, an entity no ship owns (a rock, a
// mine, the blast of a mine): something that exists on this machine only.
bool is_local_hazard(int slot);

// The game's own view of a ship's damage state; the arrays are listed in the
// proto comments of ShipHealth.
struct ShipHealthState {
    Bit16s shield[2];
    Bit16s shieldMax[2];
    Bit16s armor[4];
    Bit16s damagePoints;
    Bit8u coreHp;
    Bit8u hullCounter;
    Bit8u stateByte;
    Bit8u gunDamage;
    Bit8u engineFlag;
    Bit16s gunEnergy;

    bool operator==(const ShipHealthState &o) const;
    bool operator!=(const ShipHealthState &o) const { return !(*this == o); }
};

ShipHealthState read_health(int slot);
// keepPilot leaves dseg:D1A2 alone: a human's own ship carries 8 (the player)
// there, which names nobody on the other machine.
void write_health(int slot, const ShipHealthState &h, bool keepPilot = false);

// Strings ------------------------------------------------------------------
std::string read_cstring(Bit16u off, Bit16u maxLen);
void write_cstring(Bit16u off, Bit16u maxLen, const std::string &s);

}  // namespace wc

#endif
