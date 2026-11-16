/*
 *  Which craft a message names, in both TIE Fighter ports (the CD's wcnet_tie_session.cpp, the 1994 demo's
 *  wcnet_tiedemo_session.cpp): its identity, the same on every machine, never its object slot.
 *
 *  Two machines' object slots part as a mission goes on: a craft dies a frame or two later on one machine than on
 *  another, its explosion holds its slot a while, and the next arrival takes another slot there (measured in both
 *  games: the user's "Gold 5" in the demo took a hundred hits and never died, its hits sent to another craft's slot on
 *  its owner's machine).  A craft's identity is its flight group and the order of its arrival in that group: a group's
 *  craft are made one after another, the same way on every machine (the same mission, the same seeds), whenever each
 *  machine makes them.  On the wire: kIdent | group << 8 | arrival; each machine finds its own slot for it.
 *
 *  A craft made by another craft's AI (a carrier's drop-off: paiman_dropoffmaneuver's create_startflightgroup) is made
 *  only where that craft's AI runs; its machine says so and the others make it too, but it is no arrival in its group's
 *  count: two droppers on two machines, dropping at once into one group, would make them in two orders.  Its identity
 *  is its dropper's, its group's and the drop's number in that group, given by the dropper's machine (kDrop | dropper
 *  << 13 | group << 7 | number: one carrier drops craft 0 of one group, then craft 0 of another, measured in B9M4DW).
 *
 *  The slots are looked at by the session at the frame's top and at a hooked call's entry, never from the clock's tick
 *  (a craft half made has its type and not yet its group).  A new object in a slot (another id number) is a new
 *  craft: the caller forgets the last one's state there (its owner, its report, a kill under way) and, when the last
 *  one left without its death sent, says so with its identity (still in ident() until take()).
 */
#ifndef WCNET_TIE_IDENT_H
#define WCNET_TIE_IDENT_H

#include "dosbox.h"

namespace wc {

// A craft slot as its game has it now (each port reads its own object table).
struct TieSlotNow {
    int id;         // the object's id number (+0x00); -1: the slot is empty
    bool arrival;   // a craft come into the slot (not an explosion, debris, or a projectile)
    int fg;         // its flight group (+0x35)
    int type;       // its ship index (+0x04)
};

class TieIdents {
public:
    enum { kMaxSlots = 32, kGroups = 48 };
    static const Bit32u kIdent = 0x10000;   // (above any slot: an older build's slot numbers name nothing here)
    static const Bit32u kDrop = 0x20000000; // (a dropped-off craft's: its dropper's identity's low word, its group, its number)
    static const Bit32u kNone = 0xFFFF;     // (no craft: a target that is none, or something else)

    TieIdents() { clear(); }
    // A new flight: no slot known, no arrivals counted.
    void clear() {
        for (int o = 0; o < kMaxSlots; o++) {
            slotId_[o] = -1;
            ident_[o] = 0;
            type_[o] = 0;
            fg_[o] = -1;
            at_[o] = 0;
        }
        for (int g = 0; g < kGroups; g++) arrivals_[g] = 0;
    }
    // Another object (or none) in the slot since the last look?  Then the caller acts on the last one (ident(o) is still
    // its identity) and calls take().
    bool changed(int o, const TieSlotNow &s) const { return o >= 0 && o < kMaxSlots && s.id != slotId_[o]; }
    // The slot's new object: an identity if it is a craft's arrival.
    void take(int o, const TieSlotNow &s, double now) {
        if (o < 0 || o >= kMaxSlots) return;
        slotId_[o] = s.id;
        ident_[o] = 0;
        if (s.id < 0 || !s.arrival || s.fg < 0 || s.fg >= kGroups) return;
        name(o, s, now, kIdent | (Bit32u)s.fg << 8 | (Bit32u)(arrivals_[s.fg]++ & 0xFF));
    }
    // ... a craft whose identity is given (a dropped-off craft's: not counted among its group's arrivals).
    void takeAs(int o, const TieSlotNow &s, double now, Bit32u id) {
        if (o < 0 || o >= kMaxSlots) return;
        slotId_[o] = s.id;
        ident_[o] = 0;
        if (s.id < 0 || !s.arrival || !named(id)) return;
        name(o, s, now, id);
    }
    // A dropped-off craft's identity: its dropper's, its group's and its number in the group.
    static Bit32u drop(Bit32u dropper, int fg, int number) {
        return kDrop | (dropper & 0xFFFF) << 13 | (Bit32u)(fg & 0x3F) << 7 | (Bit32u)(number & 0x7F);
    }
    static bool named(Bit32u id) { return (id & 0xFFFF0000u) == kIdent || (id & 0xE0000000u) == kDrop; }
    // The slot's craft's identity (0: none).
    Bit32u ident(int o) const { return o >= 0 && o < kMaxSlots ? ident_[o] : 0; }
    // A craft named for the others (kNone: not a craft, or none known).
    Bit32u wire(int o) const { const Bit32u i = ident(o); return i ? i : kNone; }
    int type(int o) const { return o >= 0 && o < kMaxSlots ? type_[o] : 0; }         // (its ship index when it came)
    int fg(int o) const { return o >= 0 && o < kMaxSlots ? fg_[o] : -1; }            // (its flight group)
    double arrivedAt(int o) const { return o >= 0 && o < kMaxSlots ? at_[o] : 0; }
    static bool dropped(Bit32u id) { return (id & 0xE0000000u) == kDrop; }
    static int arrival(Bit32u id) { return (int)(id & 0xFF); }     // (an arrival's order in its group; a drop's number)
    // Our slot for a craft another machine names (-1: none here).  idNow(o): the object id in slot o now (-1 empty): a slot
    // taken by another object since the last look names nothing.
    template <class IdNow> int local(Bit32u id, int slots, IdNow idNow) const {
        if (!named(id)) return -1;
        for (int o = 0; o < slots && o < kMaxSlots; o++) {
            if (ident_[o] == id) return idNow(o) == slotId_[o] ? o : -1;
        }
        return -1;
    }

private:
    void name(int o, const TieSlotNow &s, double now, Bit32u id) {
        ident_[o] = id;
        type_[o] = s.type;
        fg_[o] = s.fg;
        at_[o] = now;
    }
    int slotId_[kMaxSlots];
    Bit32u ident_[kMaxSlots];
    int type_[kMaxSlots];
    int fg_[kMaxSlots];
    double at_[kMaxSlots];
    int arrivals_[kGroups];
};

}  // namespace wc

#endif
