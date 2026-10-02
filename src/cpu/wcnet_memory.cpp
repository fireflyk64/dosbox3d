#include <string>
#include <string.h>
#include "wcnet_memory.h"

namespace wc {

int top_level_parent(int slot) {
    if (slot < 0 || slot >= kNumSlots) {
        return slot;
    }
    Bit16u id = (Bit16u)slot;
    for (int i = 0; i < 10; i++) {
        Bit8u parent = rd8((Bit16u)(ds::parentShipForShip + id));
        if (parent == (Bit8u)id || parent == 0xFF) {
            return id;
        }
        id = parent;
    }
    return slot;
}

int free_temp_slots() {
    int n = 0;
    for (int i = kMinTempSlot; i <= kMaxTempSlot; i++) {
        if (!slot_in_use(i)) {
            n++;
        }
    }
    return n;
}

int find_bolt_of(int owner) {
    for (int i = kMinTempSlot; i <= kMaxTempSlot; i++) {
        if (entity_type(i) == ET_BOLT && top_level_parent(i) == owner) {
            return i;
        }
    }
    return -1;
}

bool is_local_hazard(int slot) {
    if (slot < 0 || slot >= kNumSlots) {
        return false;
    }
    int top = top_level_parent(slot);
    return is_temp_slot(top) && slot_in_use(top);
}

bool ShipHealthState::operator==(const ShipHealthState &o) const {
    return memcmp(this, &o, sizeof(*this)) == 0;
}

ShipHealthState read_health(int slot) {
    ShipHealthState h;
    memset(&h, 0, sizeof(h));
    for (int i = 0; i < 2; i++) {
        h.shield[i] = (Bit16s)rd16((Bit16u)(ds::curShield + 4 * slot + 2 * i));
        h.shieldMax[i] = (Bit16s)rd16((Bit16u)(ds::shieldMax + 4 * slot + 2 * i));
    }
    for (int i = 0; i < 4; i++) {
        h.armor[i] = (Bit16s)rd16((Bit16u)(ds::armorState + 8 * slot + 2 * i));
    }
    h.damagePoints = (Bit16s)rd16((Bit16u)(ds::damagePoints + 2 * slot));
    h.coreHp = rd8((Bit16u)(ds::coreHp + slot));
    h.hullCounter = rd8((Bit16u)(ds::hullCounter + slot));
    h.stateByte = rd8((Bit16u)(ds::shipStateByte + slot));
    h.gunDamage = rd8((Bit16u)(ds::gunDamage + slot));
    h.engineFlag = rd8((Bit16u)(ds::engineFlag + slot));
    h.gunEnergy = (Bit16s)rd16((Bit16u)(ds::gunEnergy + 2 * slot));
    return h;
}

void write_health(int slot, const ShipHealthState &h, bool keepPilot) {
    for (int i = 0; i < 2; i++) {
        wr16((Bit16u)(ds::curShield + 4 * slot + 2 * i), (Bit16u)h.shield[i]);
        wr16((Bit16u)(ds::shieldMax + 4 * slot + 2 * i), (Bit16u)h.shieldMax[i]);
    }
    for (int i = 0; i < 4; i++) {
        wr16((Bit16u)(ds::armorState + 8 * slot + 2 * i), (Bit16u)h.armor[i]);
    }
    wr16((Bit16u)(ds::damagePoints + 2 * slot), (Bit16u)h.damagePoints);
    wr8((Bit16u)(ds::coreHp + slot), h.coreHp);
    wr8((Bit16u)(ds::hullCounter + slot), h.hullCounter);
    if (!keepPilot) {
        wr8((Bit16u)(ds::shipStateByte + slot), h.stateByte);
    }
    wr8((Bit16u)(ds::gunDamage + slot), h.gunDamage);
    wr8((Bit16u)(ds::engineFlag + slot), h.engineFlag);
    wr16((Bit16u)(ds::gunEnergy + 2 * slot), (Bit16u)h.gunEnergy);
}

std::string read_cstring(Bit16u off, Bit16u maxLen) {
    std::string ret;
    for (Bit16u i = 0; i < maxLen; i++) {
        Bit8u c = rd8((Bit16u)(off + i));
        if (!c) {
            break;
        }
        ret.push_back((char)c);
    }
    return ret;
}

void write_cstring(Bit16u off, Bit16u maxLen, const std::string &s) {
    Bit16u i = 0;
    for (; i + 1 < maxLen && i < s.length(); i++) {
        wr8((Bit16u)(off + i), (Bit8u)s[i]);
    }
    wr8((Bit16u)(off + i), 0);
}

}  // namespace wc
