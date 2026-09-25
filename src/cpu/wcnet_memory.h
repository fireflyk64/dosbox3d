/*
 *  Wing Commander 1 (WC.EXE) data-segment map and typed accessors.
 *
 *  Every offset below is named after the symbol in the IDA database export
 *  WcMulti/wcmulti.idc where one exists; look any of them up with
 *      scripts/wcdis.py sym dseg:XXXX
 *  Names in [brackets] are ours, chosen from the disassembly of the code that
 *  uses the array (the IDA name for that address is given after it).
 *
 *  The program uses the Borland "compact" convention SS == DS == 13D3, which
 *  is why stack arguments are read through DS_OFF + esp.
 */
#ifndef WCNET_MEMORY_H_
#define WCNET_MEMORY_H_

#include <string>
#include "dosbox.h"
#include "mem.h"

namespace wc {

enum {
    DS = 0x13d3,            // data segment (DGROUP) at run time
    DS_OFF = DS * 0x10,     // linear address of dseg:0000
};

// Entity slots.  The game keeps 0x40 entity slots; every per-entity array is
// indexed by slot.  Slot 0 is always the local player.
enum Slot {
    kPlayerSlot = 0,
    kMinShipSlot = 1,       // ships, capships and missiles live in 1..9
    kMaxShipSlot = 9,
    kMinTempSlot = 10,      // bolts, explosions, dust, ... live in 10..0x3c
    kMaxTempSlot = 0x3c,
    kCameraSlot = 0x3d,
    kTempVectorSlot = 0x3f,
    kNumSlots = 0x40,
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

namespace ds {
enum Offsets {
    missionStatus = 0x00AE,          // 0=flying, 1=landed, 2=ejected, 3=carrier, 4=dead, 5=exit
    playerCanBeDamaged = 0x00BA,     // [missionStatus+C]: do_damage ignores slot 0 while zero
    aLoadingWingCom = 0x0187,        // string buffer we reuse as scratch code/data
    aSorryAnErrorHasOccured = 0x0395,// 249 byte string buffer we reuse for shellcode
    savedGameLoaded = 0x300C,        // has_loaded_game
    randomSeed = 0x7728,
    commGlobalTxt = 0x8DF8,          // 80 bytes shown by the VDU comm display
    pilotLastName = 0x9A42,          // 14 bytes
    pilotCallsign = 0x9A50,          // 14 bytes
    gPositionVector = 0xA9C2,        // 3 x int32 per slot
    entityCullStatus = 0xACC4,       // word per slot (0x8001 = culled/inactive)
    gOrientationRightVector = 0xAEB6,// 3 x int32 per slot
    gOrientationUpVector = 0xB1B6,
    gOrientationFrontVector = 0xB4B6,
    setSpeed = 0xB9F6,               // [gOrientationFrontVector+540] int32 per slot
    damageProperty = 0xBB8E,         // word per slot: bolt damage / AI timers
    gunDamage = 0xBD0E,              // [damageProperty+180] byte per slot, 0..5
    maybeEntityType = 0xBD1A,        // word per slot, EntityType
    gunEnergy = 0xBE3A,              // [maybeEntityType+120] word per slot, recharges to 100
    speedLo = 0xBE6C,                // [maybeEntityType+152] int32 per slot (lo/hi words)
    commAnimInfo = 0xBE94,           // word per slot (1 = has a pilot that talks)
    engineFlag = 0xBFD6,             // [commAnimInfo+142] byte per slot, 0xff = disabled
    gInstanceShipTypes = 0xBFF8,     // word per slot, index into kShipStats (0x6f bytes each)
    frameCounter = 0xC0B8,           // [gInstanceShipTypes+C0] incremented once per game frame
    damagePoints = 0xC17A,           // [currentNavPoint+2] word per slot, accumulated hull damage
    currentMission = 0xC255,         // byte
    currentSeries = 0xC256,          // byte
    statusPilots = 0xC260,           // byte array, nonzero = pilot KIA
    victoryPoints = 0xC280,          // word
    missileTarget = 0xC284,          // [currentCampaign+2] byte per slot
    parentShipForShip = 0xC30E,      // byte per slot, 0xff = none
    gMaybeShipRadius = 0xC3B0,       // word per slot
    curShield = 0xC430,              // curShieldMaybe: word[2] per slot (front, rear)
    gShipBoltList = 0xC472,          // 0x33 bytes per slot: count + 10 x 5-byte gun entries
    tempVector = 0xC6B2,             // scratch vector used by the game
    armorState = 0xC85E,             // armorStateArray: word[4] per slot (front, rear, left, right)
    coreHp = 0xC8B8,                 // [armorStateArray+5A] byte per slot, starts at 4
    shieldMax = 0xC988,              // shieldMaxMaybe: word[2] per slot
    shipAiState = 0xCA56,            // [comm6to5+E] word per slot, 9 = dying, 8 = ?, 0xffff
    gVelocityVector = 0xCAE2,        // 3 x int32 per slot
    shipStateByte = 0xD1A2,          // [vduModeMaybe+2] byte per slot (0xff normal)
    hullCounter = 0xD22C,            // [vduModeMaybe+8C] byte per slot
    vduStatus10WhenPlayerHitsSmth = 0xD236,
    navPointState = 0xD25D,          // 8 x 19 bytes: mission tree progress
    entitiesToDespawn = 0x6610,      // 0x14 bytes
    entitiesToDespawnTargetA = 0x6626,
    playerTarget = 0x662C,           // [entitiesToDespawn+1C]
    unknown07A6 = 0x07A6,
};
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
inline bool is_temp_slot(int slot) { return slot >= kMinTempSlot && slot <= kMaxTempSlot; }

// Follow dseg:C30E until a slot is its own parent (or has none); this is what
// ovr143:1D97 getTopLevelParent does.
int top_level_parent(int slot);

// Number of free temporary slots (bolts, explosions...).
int free_temp_slots();

// Find a live bolt (ET_BOLT) whose top-level parent is `owner`, or -1.
int find_bolt_of(int owner);

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
void write_health(int slot, const ShipHealthState &h);

// Strings ------------------------------------------------------------------
std::string read_cstring(Bit16u off, Bit16u maxLen);
void write_cstring(Bit16u off, Bit16u maxLen, const std::string &s);

}  // namespace wc

#endif
