/*
 *  The TIE Fighter demo at 640x400: the renderer's scanline pass, hand-written (docs/tie640.md).
 *
 *  TIEMISS.EXE's renderer has no back buffer.  Its upper half (drawpol, transfm2, trace2) leaves, for every row of the
 *  3D window, a list of EDGE CROSSINGS: each says "edge e of object o is at pixel x here, with light l".  Its lower
 *  half, xtrans2_drawxtrans (1CCA:85E9..9FA1), then walks every row from left to right, keeps track of which faces
 *  of which objects are open, decides which object is in front, and writes the row as runs of pixels.  That lower
 *  half is ~70% of a scene's emulated instructions.  This file is that lower half written again as native code, one
 *  call a quarter instead of ~300,000 interpreted instructions:
 *
 *      RowLists    the row's crossings gathered from the game's lists, the edges that run over several rows stepped
 *      Resolver    the walk of a row: an object's open faces, the heap of open objects, the object in front
 *      in_front    which of two objects hides the other (parents, bounding boxes, the game's own depth sort)
 *      Markings    a craft's markings: a face's colour swapped between the marking's edges
 *      Fills       a run of pixels: flat, copied from the sprites' buffer, shaded (a gradient or a dither)
 *
 *  It works on the game's own memory in the game's own formats (the lists, the object records), and changes that
 *  memory as the game's routine does, so the frame after is the same whichever of the two has run.  What it does not
 *  keep is the "last frame's runs" ring: every quarter of the 640 picture is drawn whole.
 *
 *  Everything here was written from the disassembly of the demo (notes in the research scratchpad, native.md); the
 *  formats are the demo's own 16-bit ones.  WCNET_T640_NATIVE=1 turns it on; WCNET_T640_CHECK=1 draws every quarter
 *  both ways and counts the pixels that differ (and logs, for the first row that differs, the game's own runs and
 *  this file's, with every in_front decision of the row); `!t640native` in the key script logs the passes made and
 *  the depth sorts asked of the game.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include "wcnet_t640_game.h"
#include "wcnet_tie640.h"
#include "wcnet_log.h"
#include "regs.h"
#include "cpu.h"
#include "mem.h"
#include "callback.h"

namespace wc {

namespace {

typedef Bit16u u16;
typedef Bit8u u8;

enum {
    kRendPara = XA(0x1CCA, 0x222D), kDataPara = XA(0x421F, 0x3F1C), kListPara = 0x3598, kDrawPara = XA(0x168C, 0x1D5D),
    kDepthSort = XA(0x0798, 0x07FA),        // 168C: draw_polydepthsort(faceA, a, parentA, idA, faceB, b, parentB, idB)
    // DS: the window
    kCorner = XA(0x48C3, 0x5421), kWide = XA(0x48C5, 0x5423), kDeep = XA(0x48D1, 0x5433), kBackColour = XA(0x48BC, 0x541A), kBufferSeg = XA(0x48BD, 0x541B), kBufferOff = XA(0x48BF, 0x541D),
    kTablesSeg = XA(0xD8A6, 0xD34A), kMaskAt = XA(0x28B6, 0x33FB),
    kRows = XA(0x6232, 0xD3C8),             // DS: for each row the first chunk of its entries (in the tables' segment)
    // DS: an object's state on the row being walked
    kObjFlag = XA(0x2AF0, 0x3635),          // [obj] its place in the heap (FF: it is the one in front, 0: not open)
    kOpenFaces = XA(0x28F0, 0x3435),        // [obj] how many of its faces are open
    kMinFace = XA(0x2970, 0x34B5),          // [obj] the lowest open face: the one seen
    kMinEdge = XA(0x29F0, 0x3535),          // [2*obj] the crossing that face began at (its x and light)
    kHeap = XA(0x2CF0, 0x3835),             // [2..] the open objects that are not in front
    kObjects = XA(0x233A, 0x2E72),          // [2*obj] the object's record in the tables' segment
    kMarkPtr = XA(0x2552, 0x308A), kMarkOn = XA(0x2652, 0x318A), kMarkCount = XA(0x26D2, 0x320A),
    kFlatColour = XA(0x410C, 0x4C51), kFlatComp = XA(0x428C, 0x4DD1), kFlatParent = XA(0x418C, 0x4CD1), kFlatX = XA(0x3E0C, 0x4951), kFlatY = XA(0x3F0C, 0x4A51), kFlatZ = XA(0x400C, 0x4B51),
    // DS: the row's lists (near pointers into the tables' segment)
    kNew = XA(0x2DF0, 0x3935),              // the row's single crossings, as found
    kNewSorted = XA(0x38B8, 0x43FD),        // ... sorted by x: {slot in kNew, crossing}, the order kept from row to row
    kTallNew = XA(0x3090, 0x3BD5), kTall = XA(0x3378, 0x3EBD),   // crossings that stay at their x for several rows: {crossing, rows left, light's fraction}
    // the lists' segment: edges that run over several rows {record, long x}
    kLeftNew = 0x000C, kLeft = 0x05DC,   // left of the window
    kRightNew = 0x0BAC, kRight = 0x117C, // right of it
    kShades = XA(0x07A0, 0x07A0),           // the renderer's code segment: 16 colours a material
};

struct Native {
    u8 *M;                      // the emulated memory
    PhysPt ds, tb, ls, cs;      // DGROUP, the tables' segment, the lists' segment, the renderer's code
    int wide, deep;
    // where the pixels go: the 640 picture, this quarter's corner in it
    u8 *draw; int pitch, limitX, limitY, atX, atY;
    PhysPt buffer;              // the sprites' buffer's row
    // the lists' ends
    u16 newEnd, sortedEnd, tallNewEnd, tallEnd, leftNewEnd, leftEnd, rightNewEnd, rightEnd;
    // the walk
    int row;
    u16 maskAt, maskX, startX, endX, runX;
    Bit8s maskFlag;             // negative: the mask is closed here
    u16 front;                  // the object in front (0: none, the background; FFFF: to be found)
    u16 heapTop;                // (in words' offsets: 2 an object)
    Bit8s popFlag;
    u16 obj, edge, info;        // the crossing being taken
    u16 cursorN, cursorT;       // the walk's places in kNewSorted and kTall (the shaded run looks ahead from them)
    unsigned sorts;             // (the game's depth sort called)
    int (*oracle)(const u16 *args);     // ONE PASS: the depth sort's answers kept (0: the first, 1: the second, -1: not known)
    u16 asked[8];               // ... the question a pass stopped at
    bool stopped;

    inline u16 d16(u16 o) const { return host_readw(M + ds + o); }
    inline u8 d8(u16 o) const { return M[ds + o]; }
    inline void D16(u16 o, u16 v) { host_writew(M + ds + o, v); }
    inline void D8(u16 o, u8 v) { M[ds + o] = v; }
    inline u16 t16(u16 o) const { return host_readw(M + tb + o); }
    inline u8 t8(u16 o) const { return M[tb + o]; }
    inline Bit32u t32(u16 o) const { return host_readd(M + tb + o); }
    inline void T16(u16 o, u16 v) { host_writew(M + tb + o, v); }
    inline void T8(u16 o, u8 v) { M[tb + o] = v; }
    inline u16 l16(u16 o) const { return host_readw(M + ls + o); }
    inline Bit32u l32(u16 o) const { return host_readd(M + ls + o); }
    inline void L16(u16 o, u16 v) { host_writew(M + ls + o, v); }
    inline void L32(u16 o, Bit32u v) { host_writed(M + ls + o, v); }

    inline u16 record(u16 o) const { return d16((u16)(kObjects + 2 * o)); }
    // an edge's slot in its object's record: the two faces it parts, the light's step a row
    inline u16 edge_slot(u16 o, u16 e) const { const u16 r = record(o); return (u16)(r + 2 * t8((u16)(r + 0x10)) + 4 * e + 0x67); }

    // ---------------------------------------------------------------------------------------------------------
    // RowLists
    //
    // A row's entries are a chain of 80h-byte chunks (word 0: the bytes used, or the next chunk when full).  An
    // entry is 4 bytes {edge, object, x << 7 | light}; three kinds of records come before an entry they belong to:
    //   FFFF, (rows, light's fraction)          the entry stays at its x for that many rows, its light stepped
    //   FFFD..FFF7, slope / FFF6..FFEF, slope   an edge running over several rows, left of the window's left edge
    //                                           / right of its right edge: 12 bytes {kind, slope}, {edge, object,
    //                                           light}, {x or error, rows}; its x is kept in the lists' segment
    // ---------------------------------------------------------------------------------------------------------

    // a tall crossing's light, a row on: 6 bits and 8 of fraction, the edge's step added, turned back at the ends
    void step_tall(u16 at) {
        const u16 e = d16(at);
        const u16 o = t8((u16)(e + 1));
        if (o < 0x80) {
            const u16 step = t16((u16)(edge_slot(o, t8(e)) + 2));
            const unsigned f = d8((u16)(at + 3)) + (step & 0xFF);
            D8((u16)(at + 3), (u8)f);
            u8 l = (u8)((step >> 8) + t8((u16)(e + 2)) + (f >> 8));
            if (l & 0x40) l = (u8)~l;
            T8((u16)(e + 2), (u8)((t8((u16)(e + 2)) & 0x80) | (l & 0x3F)));
        }
    }

    // a running edge's record: where its three parts are (it may lie across two chunks)
    void parts(u16 rec, u16 &entry, u16 &third) const {
        const int past = (rec & 0x7F) - 0x78;
        if (past > 0) { const u16 next = t16((u16)(rec & XA(0xFF80, 0xFF80))); entry = (u16)(next + 4); third = (u16)(next + 8); }
        else if (past == 0) { const u16 next = t16((u16)(rec & XA(0xFF80, 0xFF80))); entry = (u16)(rec + 4); third = (u16)(next + 4); }
        else { entry = (u16)(rec + 4); third = (u16)(rec + 8); }
    }

    // ... a row on: its x by its kind (an error term that moves it a pixel now and then, or a fixed-point x), its
    // light.  False when its rows are over.  `stray`: the game's routine for the edges right of the window writes
    // the new x into the DATA segment at the list's offset, not into the list (so the list keeps the first x, and
    // the game's colour tables at DS:117C.. are written over): done here as the game does it.
    bool step_running(u16 at, bool stray) {
        const u16 rec = l16(at);
        u16 entry, third;
        parts(rec, entry, third);
        const u8 rows = (u8)(t8((u16)(third + 3)) - 1);
        T8((u16)(third + 3), rows);
        if (!rows) return false;
        const int kind = (0x100 - t8(rec)) & 0xFF;
        const u16 slope = t16((u16)(rec + 2));
        Bit32u x = stray ? host_readd(M + ds + (u16)(at + 2)) : l32((u16)(at + 2));
        bool put = true;
        switch (stray ? kind - 8 : kind) {
        case 2: case 6: case 3: case 7: {
            const u16 err = t16(third);
            T16(third, (u16)(err - 0x100));
            if (err < 0x100) {
                const int k = stray ? kind - 8 : kind;
                // (left of the window: kinds 2, 6 a pixel left, 3, 7 a pixel right; right of it the other way round)
                const bool up = stray ? (k == 2 || k == 6) : (k == 3 || k == 7);
                x = up ? x + 1 : x - 1;
                T16(third, (u16)(err - 0x100 + slope));
            } else put = false;
            break;
        }
        case 4: case 5: case 8: case 9: {
            const int k = stray ? kind - 8 : kind;
            Bit32u v = t32(third) & 0xFFFFFF;
            // (left of the window: 4, 8 take the slope off, 5, 9 add it; right of it the other way round)
            const bool add = stray ? (k == 4 || k == 8) : (k == 5 || k == 9);
            v = (add ? v + slope : v - slope) & 0xFFFFFF;
            T16(third, (u16)v);
            T8((u16)(third + 2), (u8)(v >> 16));
            x = (k == 4 || k == 5) ? v >> 7 : v;
            break;
        }
        default: put = false; break;
        }
        if (put) { if (stray) host_writed(M + ds + (u16)(at + 2), x); else L32((u16)(at + 2), x); }
        const u16 o = t8((u16)(entry + 1));
        if (o < 0x80) {
            u16 l = (u16)(t16((u16)(edge_slot(o, t8(entry)) + 2)) << 1);
            l = (u16)(l + t16((u16)(entry + 2)));
            if (l & 0x8000) l = (u16)~l;
            T16((u16)(entry + 2), l);
        }
        return true;
    }

    void gather() {
        tallNewEnd = kTallNew; newEnd = kNew; leftNewEnd = kLeftNew; rightNewEnd = kRightNew;
        u16 at = d16((u16)(kRows + 2 * row));
        if (!at) sortedEnd = kNewSorted;
        else {
            u16 next = 0;
            for (;;) {
                int left = t16(at);
                at = (u16)(at + 4);
                next = 0;
                if (left > 0x7C) { next = (u16)left; left = 0x7C; }
                if (left <= 0) break;       // (an empty chunk: never made by the game)
                for (;;) {
                    const u16 w = t16(at);
                    if (w < XA(0xFFEF, 0xFFEF)) {
                        D16(newEnd, at);
                        newEnd = (u16)(newEnd + 2);
                    } else if (w == 0xFFFF) {
                        const u16 rows = t16((u16)(at + 2));
                        at = (u16)(at + 4);
                        left -= 4;
                        if (!left) {
                            at = next; next = 0;
                            left = t16(at);
                            at = (u16)(at + 4);
                            if (left > 0x7C) { next = (u16)left; left = 0x7C; }
                        }
                        D16(tallNewEnd, at);
                        D16((u16)(tallNewEnd + 2), rows);
                        tallNewEnd = (u16)(tallNewEnd + 4);
                    } else {
                        const bool leftSide = w >= XA(0xFFF7, 0xFFF7);
                        u16 &end = leftSide ? leftNewEnd : rightNewEnd;
                        L16(end, at);
                        const u16 slope = t16((u16)(at + 2));
                        const int kind = 0x10000 - w - (leftSide ? 0 : 8);
                        at = (u16)(at + 8);
                        left -= 8;
                        if (left <= 0) {
                            const u16 chunk = next;
                            at = (u16)(chunk + 4 - left);
                            next = t16(chunk);
                            if (next <= 0x7C) { left += next; next = 0; } else left += 0x7C;
                        }
                        Bit32u x;
                        if (kind == 2 || kind == 3) {
                            x = t8(at);
                            if (!leftSide) x = (u16)(x + wide);
                            const u8 b = t8((u16)(at + 1));
                            T16((u16)(at + 2), (u16)(t16((u16)(at + 2)) + b));
                            T16(at, (u16)(slope - b));
                        } else {
                            x = t32(at) & 0xFFFFFF;
                            if (kind == 4 || kind == 5) x >>= 7;
                            else if (kind == 6 || kind == 7) T16(at, slope);
                        }
                        L32((u16)(end + 2), x);
                        end = (u16)(end + 6);
                    }
                    at = (u16)(at + 4);
                    left -= 4;
                    if (!left) break;
                }
                at = next;
                if (!at) break;
            }
            // the single crossings sorted by x: last row's order kept for the slots still in use, the rest after
            u16 to = kNewSorted;
            for (u16 from = kNewSorted; from != sortedEnd; from = (u16)(from + 4)) {
                const u16 slot = d16(from);
                if (slot >= newEnd) continue;
                D16(to, slot); D16((u16)(to + 2), d16(slot));
                to = (u16)(to + 4);
            }
            for (u16 slot = (u16)(kNew + (to - kNewSorted) / 2); slot < newEnd; slot = (u16)(slot + 2)) {
                D16(to, slot); D16((u16)(to + 2), d16(slot));
                to = (u16)(to + 4);
            }
            sortedEnd = to;
            for (bool swapped = true; swapped;) {
                swapped = false;
                for (u16 p = kNewSorted; (u16)(p + 4) < sortedEnd; p = (u16)(p + 4)) {
                    const u16 a = d16((u16)(p + 2)), b = d16((u16)(p + 6));
                    if (t16((u16)(a + 2)) <= t16((u16)(b + 2))) continue;
                    const u16 sa = d16(p), sb = d16((u16)(p + 4));
                    D16(p, sb); D16((u16)(p + 2), b);
                    D16((u16)(p + 4), sa); D16((u16)(p + 6), a);
                    swapped = true;
                }
            }
        }
        // the tall crossings: each a row on (its light), those over replaced by this row's new ones, the rest of
        // the new ones after; sorted again when any came
        {
            u16 to = kTall, in = kTallNew, from = kTall;
            bool compact = false;
            if (in >= tallNewEnd) compact = true;
            while (!compact) {
                if (from >= tallEnd) break;
                step_tall(from);
                const u8 rows = (u8)(d8((u16)(from + 2)) - 1);
                D8((u16)(from + 2), rows);
                if (!rows) {
                    host_writed(M + ds + from, host_readd(M + ds + in));
                    from = (u16)(from + 4);
                    in = (u16)(in + 4);
                    if (in >= tallNewEnd) compact = true;
                } else from = (u16)(from + 4);
            }
            if (compact) {
                to = from;
                for (; from < tallEnd; from = (u16)(from + 4)) {
                    step_tall(from);
                    const u8 rows = (u8)(d8((u16)(from + 2)) - 1);
                    D8((u16)(from + 2), rows);
                    if (rows) { host_writed(M + ds + to, host_readd(M + ds + from)); to = (u16)(to + 4); }
                }
            } else {
                to = from;
                for (; in < tallNewEnd; in = (u16)(in + 4)) { host_writed(M + ds + to, host_readd(M + ds + in)); to = (u16)(to + 4); }
            }
            tallEnd = to;
            if (tallNewEnd != kTallNew) {
                for (bool swapped = true; swapped;) {
                    swapped = false;
                    for (u16 p = kTall; (u16)(p + 4) < tallEnd; p = (u16)(p + 4)) {
                        const u16 a = d16(p), b = d16((u16)(p + 4));
                        if (t16((u16)(a + 2)) <= t16((u16)(b + 2))) continue;
                        const Bit32u va = host_readd(M + ds + p), vb = host_readd(M + ds + (u16)(p + 4));
                        host_writed(M + ds + p, vb);
                        host_writed(M + ds + (u16)(p + 4), va);
                        swapped = true;
                    }
                }
            }
        }
        // the running edges, a row on; this row's new ones after them
        for (int side = 0; side < 2; side++) {
            const u16 first = side ? kRight : kLeft, fresh = side ? kRightNew : kLeftNew, freshEnd = side ? rightNewEnd : leftNewEnd;
            u16 &end = side ? rightEnd : leftEnd;
            u16 to = first;
            for (u16 from = first; from < end; from = (u16)(from + 6)) {
                if (!step_running(from, side != 0)) continue;
                L16(to, l16(from));
                L32((u16)(to + 2), l32((u16)(from + 2)));
                to = (u16)(to + 6);
            }
            for (u16 from = fresh; from < freshEnd; from = (u16)(from + 2)) { L16(to, l16(from)); to = (u16)(to + 2); }
            end = to;
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Fills
    // ---------------------------------------------------------------------------------------------------------

    inline u8 *pixel(int x) const {
        const int X = atX + x, Y = atY + row;
        return X < limitX && Y < limitY ? draw + Y * pitch + X : NULL;
    }

    void fill_flat(u8 colour) {
        for (int x = startX; x < endX; x++) { u8 *p = pixel(x); if (p) *p = colour; }
    }
    // colour 0: the run is the sprites' and the backdrop's, already drawn into the game's buffer
    void fill_buffer() {
        for (int x = startX; x < endX; x++) { u8 *p = pixel(x); if (p) *p = M[buffer + (u16)x]; }
    }
    // a shaded run of a material: the light goes from `from` at the face's left edge leftX to `to` at its right
    // edge rightX (0..63 each, 63 darkest in the table's order after the turn below); 16 colours a material, the
    // steps between them dithered by an error carried along the run (started from the row's parity)
    void fill_shaded(int material, int leftX, int rightX, int from, int to) {
        const PhysPt table = cs + kShades + (PhysPt)(((material - 1) << 4) & 0xFFFF);
        const u16 li = (u16)(0x3F - (from & 0x3F));
        const Bit16s delta = (Bit16s)((0x3F - (to & 0x3F)) - li);
        if (delta) {
            u16 v = (u16)(li << 9);
            Bit16s step = (Bit16s)(delta << 9);
            const Bit16s span = (Bit16s)(rightX - leftX);
            if (span) {
                // (the game's idiv: a quotient that does not fit is its divide fault; not seen, the value kept)
                const int q = (int)step / (int)span;
                step = (Bit16s)q;
            }
            const Bit16s off = (Bit16s)(leftX - startX);
            if (off) v = (u16)(v + (u16)((int)step * (int)(Bit16s)-off));
            u16 err = (u16)((row & 1) << 10);
            const u8 last = M[table + 15];
            for (int x = startX; x < endX; x++) {
                const u16 t = (u16)(v + err);
                err = (u16)(t & 0x7FF);
                const unsigned i = t >> 11;
                u8 *p = pixel(x);
                if (p) *p = i == 16 ? last : M[table + i];
                v = (u16)(v + (u16)step);
            }
        } else {
            unsigned i = li >> 2;
            unsigned frac = (li & 3) << 6;
            if (i == 15) frac = 0;
            if (!frac) fill_flat(M[table + i]);
            else {
                unsigned acc = (row & 1) << 7;
                for (int x = startX; x < endX; x++) {
                    acc += frac;
                    u8 *p = pixel(x);
                    if (p) *p = M[table + i + (acc >> 8)];
                    acc &= 0xFF;
                }
            }
        }
    }

    // the run [startX, endX) in the colour of the object in front (xtrans2_outputxt)
    void output() {
        if (endX <= startX) return;
        const u16 f = front;
        if (keep) { RunLog r; memset(&r, 0, sizeof(r)); r.row = row; r.from = startX; r.to = endX; r.front = f; r.obj = obj; r.edge = edge; r.info = info; r.material = -1; runs.push_back(r); }
        if (!f) fill_plain(d8(kBackColour));
        else if (f >= 0x80) fill_plain(d8((u16)(kFlatColour + f - 0x80)));
        else {
            const u8 face = d8((u16)(kMinFace + f));
            const u8 colour = t8((u16)(record(f) + 2 * face + 0x65));
            if (colour >= 0x40) fill_plain(colour);
            else shaded(f, face, colour);
        }
        startX = endX;
    }
    void fill_plain(u8 colour) { if (colour) fill_flat(colour); else fill_buffer(); }

    // does this crossing's edge bound the face?
    inline bool bounds(u16 o, u16 entry, u8 face) const {
        const u16 s = edge_slot(o, t8(entry));
        return t8(s) == face || t8((u16)(s + 1)) == face;
    }

    // the shaded run's two edges.  The left one is the crossing the face began at (a running edge left of the
    // window: its light worked out for the window's edge).  The right one is the crossing being taken if it is the
    // face's, else the first of the face's found ahead: the single and the tall crossings in turn, then the
    // running edges right of the window.
    void shaded(u16 f, u8 face, u8 material) {
        u16 right = info;
        bool found = f == obj && t8((u16)(edge_slot(f, edge))) == face;
        if (!found && f == obj && t8((u16)(edge_slot(f, edge) + 1)) == face) found = true;
        int rightX = -1, rightLight = -1;
        if (!found) {
            u16 n = cursorN, t = cursorT;
            bool got = false;
            for (;;) {
                if (n < sortedEnd) {
                    const u16 e = d16((u16)(n + 2));
                    if (t8((u16)(e + 1)) == f && bounds(f, e, face)) { right = t16((u16)(e + 2)); got = true; break; }
                    n = (u16)(n + 4);
                    if (t >= tallEnd) continue;
                } else if (t >= tallEnd) break;
                const u16 e = d16(t);
                if (t8((u16)(e + 1)) == f && bounds(f, e, face)) { right = t16((u16)(e + 2)); got = true; break; }
                t = (u16)(t + 4);
            }
            if (!got) {
                for (u16 r = kRight; r < rightEnd; r = (u16)(r + 6)) {
                    u16 rec = l16(r);
                    if ((rec & 0x7F) == 0x7C) rec = t16((u16)(rec & XA(0xFF80, 0xFF80)));
                    const u16 e = (u16)(rec + 4);
                    if (t8((u16)(e + 1)) != f || !bounds(f, e, face)) continue;
                    if (l16((u16)(r + 4))) break;       // (its x past 65535: the crossing being taken serves)
                    rightX = l16((u16)(r + 2));
                    rightLight = t16((u16)(e + 2)) >> 9;
                    break;
                }
            }
        }
        if (rightX < 0) { rightX = right >> 7; rightLight = right & 0x3F; }
        const u16 left = d16((u16)(kMinEdge + 2 * f));
        int leftX, leftLight;
        if (left < 0xC000) { leftX = left >> 7; leftLight = left & 0x3F; }
        else {
            const u16 at = (u16)((left & 0x3FFF) + kLeft);
            leftX = 0;
            if (l16((u16)(at + 4))) leftLight = rightLight;
            else {
                const u16 away = l16((u16)(at + 2));
                u16 part;       // how much of the way from the left edge to the right one the window's edge is
                if (!away) part = 0xFFFF;
                else {
                    const Bit32u whole = (Bit32u)(u16)rightX + away;
                    if (whole <= 0xFFFF) part = (u16)((((Bit32u)(u16)rightX) << 16) / whole);    // (whole > rightX: it fits)
                    else {
                        // (the game halves both first: the sum's 17 bits)
                        const Bit32u half = whole >> 1;
                        const Bit32u num = (Bit32u)(((u16)rightX >> 1) & 0x7FFF) << 16;
                        part = (u16)(num / (half & 0xFFFF ? half & 0xFFFF : 1));
                    }
                }
                u16 rec = l16(at);
                if ((rec & 0x7F) == 0x7C) rec = t16((u16)(rec & XA(0xFF80, 0xFF80)));
                const u16 edgeLight = t16((u16)(rec + 4 + 2));
                u16 l = (u16)((rightLight & 0x3F) << 9);
                const Bit16s d = (Bit16s)(edgeLight - l);
                if (d >= 0) l = (u16)(l + (u16)(((Bit32u)part * (u16)d) >> 16));
                else l = (u16)(l - (u16)(((Bit32u)part * (u16)-d) >> 16));
                leftLight = l >> 9;
            }
        }
        if (keep) { RunLog &r = runs.back(); r.material = material; r.left = left; r.leftX = leftX; r.leftLight = leftLight; r.rightX = rightX; r.rightLight = rightLight; r.found = found; }
        fill_shaded(material, leftX, rightX, leftLight, rightLight);
    }

    // ---------------------------------------------------------------------------------------------------------
    // in_front: which of two open objects hides the other (xtrans2_getinfront).  1..7F a mesh (a craft's convex
    // part), 80.. a flat one (a sprite, a billboard; 80 itself loses to everything).
    // ---------------------------------------------------------------------------------------------------------

    inline Bit16s box(u16 rec, int i) const { return (Bit16s)t16((u16)(rec + 2 * i)); }   // xmin ymin zmin xmax ymax zmax

    u16 flat_or_mesh(u16 flat, u16 mesh) const {
        const u16 i = (u16)(2 * (flat - 0x80)), rec = record(mesh);
        const Bit16s x = (Bit16s)d16((u16)(kFlatX + i)), y = (Bit16s)d16((u16)(kFlatY + i)), z = (Bit16s)d16((u16)(kFlatZ + i));
        Bit16s c;
        c = box(rec, 0); if (!(c < x)) return c >= 0 ? flat : mesh;
        c = box(rec, 3); if (!(c > x)) return c >= 0 ? mesh : flat;
        c = box(rec, 1); if (!(c < y)) return c < 0 ? mesh : flat;
        c = box(rec, 4); if (!(c > y)) return c >= 0 ? mesh : flat;
        c = box(rec, 2); if (!(c < z)) return c < 0 ? mesh : flat;
        c = box(rec, 5); if (!(c > z)) return c >= 0 ? mesh : flat;
        return flat;
    }

    // a mesh against a flat one; `meshFirst`: the mesh was the first of the two asked about (the one in front so
    // far).  Of the same craft, the mesh is in front when its number is below the flat's component number -- which
    // the game reads from the right place only when the flat was the first: with the mesh first it reads the byte
    // 80h further on (1CCA:9DFD, [bx+428C] where [di+428C] was meant), another table's.  Kept as the game has it.
    u16 mesh_flat(u16 mesh, u16 flat, bool meshFirst) const {
        const u16 i = (u16)(2 * (flat - 0x80));
        const u16 parent = t16((u16)(record(mesh) + 0xC));
        const u16 z = d16((u16)(kFlatZ + i));
        if (z == 0x8000) return mesh;
        if (d8((u16)(kObjFlag + 0x80)) && (parent >> 8) == 0x10) return flat;
        if (z == XA(0x8001, 0x8001)) return mesh;
        if (parent == d16((u16)(kFlatParent + i))) return (u8)mesh < d8((u16)(kFlatComp + (meshFirst ? flat : i >> 1))) ? mesh : flat;
        return flat_or_mesh(flat, mesh);
    }

    void remember(u16 rec, u8 behind) {     // (five places: the first free of the first four, else the fifth)
        int k = 0;
        while (k < 4 && t8((u16)(rec + 0x12 + k))) k++;
        T8((u16)(rec + 0x12 + k), behind);
    }

    // (the check: every run of the pass kept, for the row a difference is found in)
    struct RunLog { int row, from, to, front, obj, edge, info, material, left, leftX, leftLight, rightX, rightLight, found; };
    std::vector<RunLog> runs;
    std::vector<std::pair<int, std::string> > notes;
    bool keep;
    u16 in_front(u16 a, u16 b) {
        const u16 r = in_front_(a, b);
        if (keep) {
            char ta[80] = "", tb[80] = "";
            for (int k = 0; k < 2; k++) {
                const u16 o = k ? b : a; char *t = k ? tb : ta;
                if (o < 0x80) { const u16 rc = record(o); snprintf(t, 80, "parent %04X id %04X box %d %d %d %d %d %d", t16((u16)(rc + 0xC)), t16((u16)(rc + 0xE)), box(rc, 0), box(rc, 1), box(rc, 2), box(rc, 3), box(rc, 4), box(rc, 5)); }
                else { const u16 i = (u16)(2 * (o - 0x80)); snprintf(t, 80, "flat parent %04X comp %u at %d %d %d", d16((u16)(kFlatParent + i)), d8((u16)(kFlatComp + (i >> 1))), (Bit16s)d16((u16)(kFlatX + i)), (Bit16s)d16((u16)(kFlatY + i)), (Bit16s)d16((u16)(kFlatZ + i))); }
            }
            char line[300];
            snprintf(line, sizeof(line), "x %u: %02X (%s) against %02X (%s): %02X; first flat open %u", endX, a, ta, b, tb, r, d8((u16)(kObjFlag + 0x80)));
            notes.push_back(std::make_pair(row, std::string(line)));
        }
        return r;
    }
    u16 in_front_(u16 a, u16 b) {
        if (a == 0x80) return b;
        if (a > 0x80) {
            if (b == 0x80) return a;
            if (b > 0x80) {
                if (d8((u16)(kObjFlag + 0x80))) {
                    if (d16((u16)(kFlatParent + 2 * (b - 0x80))) == 0x1000) return a;
                    if (d16((u16)(kFlatParent + 2 * (a - 0x80))) == 0x1000) return b;
                }
                return a >= b ? a : b;
            }
            return mesh_flat(b, a, false);
        }
        if (b >= 0x80) return mesh_flat(a, b, true);
        const u16 ra = record(a), rb = record(b);
        if (t16((u16)(ra + 0xC)) == t16((u16)(rb + 0xC))) return a >= b ? b : a;
        for (int k = 0; k < 5; k++) if (t8((u16)(rb + 0x12 + k)) == (u8)a) return b;
        for (int k = 0; k < 5; k++) if (t8((u16)(ra + 0x12 + k)) == (u8)b) return a;
        int winner = 0;     // 1: a, 2: b
        Bit16s c, d;
        do {
            c = box(rb, 3); d = box(ra, 0); if (!(c > d)) { if (d < 0) { winner = 1; break; } if (c >= 0) { winner = 2; break; } }
            c = box(rb, 0); d = box(ra, 3); if (!(c < d)) { if (c < 0) { winner = 2; break; } if (d >= 0) { winner = 1; break; } }
            c = box(rb, 4); d = box(ra, 1); if (!(c > d)) { if (d < 0) { winner = 1; break; } if (c >= 0) { winner = 2; break; } }
            c = box(rb, 1); d = box(ra, 4); if (!(c < d)) { if (c < 0) { winner = 2; break; } if (d >= 0) { winner = 1; break; } }
            c = box(rb, 5); d = box(ra, 2); if (!(c > d)) { if (d < 0) { winner = 1; break; } if (c >= 0) { winner = 2; break; } }
            c = box(rb, 2); d = box(ra, 5); if (!(c < d)) { if (c < 0) { winner = 2; break; } if (d >= 0) { winner = 1; break; } }
        } while (false);
        if (!winner) winner = depth_sort(a, b) == a ? 1 : 2;
        if (winner == 1) { remember(ra, (u8)b); return a; }
        remember(rb, (u8)a);
        return b;
    }

    // the boxes overlap: the game's own draw_polydepthsort, run by the interpreter (a C routine of the game's upper
    // half with the craft's tables and an overlay's help; a handful of calls a frame, its answer kept in the records)
    u16 depth_sort(u16 a, u16 b) {
        sorts++;
        const u16 ra = record(a), rb = record(b);
        const u16 args[8] = {
            t8((u16)(ra + 2 * d8((u16)(kMinFace + a)) + 0x66)), a, t16((u16)(ra + 0xC)), t16((u16)(ra + 0xE)),
            t8((u16)(rb + 2 * d8((u16)(kMinFace + b)) + 0x66)), b, t16((u16)(rb + 0xC)), t16((u16)(rb + 0xE)) };
        if (oracle) {
            // ONE PASS: the pass works on an image, and the game's routine is asked between passes, not from
            // inside one (tie640_native_image): an answer known is used, a question not yet answered stops the pass
            const int known = oracle(args);
            if (known >= 0) return known ? b : a;
            memcpy(asked, args, sizeof(asked));
            stopped = true;
            return a;
        }
        const CPU_Regs keptRegs = cpu_regs;
        const Segments keptSegs = Segs;
        for (int i = 7; i >= 0; i--) CPU_Push16(args[i]);
        CALLBACK_RunRealFar((Bit16u)(SegValue(::cs) - kRendPara + kDrawPara), kDepthSort);
        const u16 answer = reg_ax;
        cpu_regs = keptRegs;
        Segs = keptSegs;
        return answer;
    }

    // the nearest of the heap's objects, taken out of it (xtrans2_findnearest)
    u16 nearest() {
        if (!heapTop) return 0;
        u16 best = d16((u16)(kHeap + heapTop));
        for (u16 i = (u16)(heapTop - 2); i; i = (u16)(i - 2)) best = in_front(best, d16((u16)(kHeap + i)));
        const u16 at = d8((u16)(kObjFlag + best));
        if (at != heapTop) {
            const u16 top = d16((u16)(kHeap + heapTop));
            D16((u16)(kHeap + at), top);
            D8((u16)(kObjFlag + top), (u8)at);
            popFlag = 1;
        }
        heapTop = (u16)(heapTop - 2);
        if (--popFlag < 0) popFlag = 0;
        return best;
    }

    // ---------------------------------------------------------------------------------------------------------
    // Resolver
    // ---------------------------------------------------------------------------------------------------------

    inline void output_if(u16 o) { if (o == front && maskFlag >= 0) output(); }
    void set_front(u16 o) {
        if (maskFlag >= 0) output();
        front = o;
        D8((u16)(kObjFlag + o), 0xFF);
    }
    void front_gone() {     // the object in front has closed and nothing was pushed under it just now
        if (maskFlag < 0) front = heapTop ? 0xFFFF : 0;
        else set_front(nearest());
    }

    void close_object(u16 o) {
        const u8 at = d8((u16)(kObjFlag + o));
        D8((u16)(kObjFlag + o), 0);
        if (o == front) {
            if (popFlag) {
                const u16 top = heapTop;
                heapTop = (u16)(heapTop - 2);
                popFlag--;
                set_front(d16((u16)(kHeap + top)));
            } else front_gone();
            return;
        }
        if (o == 0x80 && front != 0xFFFF && front) {
            // (the game's oddity: the first flat object closing puts the object in front into its place in the
            // heap, and the front is found again)
            D8((u16)(kObjFlag + front), at);
            D16((u16)(kHeap + at), front);
            front_gone();
            return;
        }
        const u16 top = heapTop;
        heapTop = (u16)(heapTop - 2);
        if (top != at) {
            const u16 moved = d16((u16)(kHeap + top));
            D16((u16)(kHeap + at), moved);
            D8((u16)(kObjFlag + moved), at);
            popFlag = 1;
        }
        if (--popFlag < 0) popFlag = 0;
    }

    void push_behind(u16 o) {
        popFlag = 0;
        heapTop = (u16)(heapTop + 2);
        D8((u16)(kObjFlag + o), (u8)heapTop);
        D16((u16)(kHeap + heapTop), o);
    }

    void open_object(u16 o) {
        for (;;) {
            if (front == 0xFFFF) { push_behind(o); }
            else if (!front) { D8(kObjFlag, 0); set_front(o); return; }
            else {
                const u16 winner = in_front(front, o);
                if (winner != front) {
                    heapTop = (u16)(heapTop + 2);
                    popFlag++;
                    D16((u16)(kHeap + heapTop), front);
                    D8((u16)(kObjFlag + front), (u8)heapTop);
                    set_front(winner);
                    return;
                }
                push_behind(o);
            }
            // (the first flat object opening behind something: the heap's nearest is asked against the front again)
            if (o != 0x80 || heapTop == 2 || front == 0xFFFF) return;
            o = nearest();
            popFlag = -1;
        }
    }

    // a mesh's edge crossed: its two faces open or close.  The record keeps the open faces (up to 16) with the
    // crossing each began at; the lowest-numbered one is the face seen (the faces' numbers are the order the game
    // drew them in: the ship's own face order, or its BSP walk).
    void mesh_crossing() {
        const u16 o = obj, rec = record(o), slot = edge_slot(o, edge);
        u8 f1 = t8(slot), f2 = t8((u16)(slot + 1));
        const u16 faces = (u16)(rec + 0x17), began = (u16)(rec + 0x27);
        const u16 countAt = (u16)(kOpenFaces + o), minAt = (u16)(kMinFace + o), edgeAt = (u16)(kMinEdge + 2 * o);
        u8 count = d8(countAt);
        if (!count) {
            T8(faces, f1); T8((u16)(faces + 1), f2);
            count = 1;
            if (f2) { count = 2; if (f1 > f2) f1 = f2; }
            D8(countAt, count);
            D8(minAt, f1);
            T16(began, info); T16((u16)(began + 2), info);
            D16(edgeAt, info);
            open_object(o);
            return;
        }
        for (;;) {      // (once for each of the two faces, when the first could not settle both)
            count = d8(countAt);
            if (count == 1) {
                if (f1 == t8(faces)) {
                    if (!f2) { D8(countAt, 0); close_object(o); return; }
                    output_if(o);
                    D8(minAt, f2); T8(faces, f2); T16(began, info); D16(edgeAt, info);
                    return;
                }
                if (f2 == t8(faces)) {
                    output_if(o);
                    D8(minAt, f1); T8(faces, f1); T16(began, info); D16(edgeAt, info);
                    return;
                }
                T8((u16)(faces + 1), f1); T8((u16)(faces + 2), f2);
                T16((u16)(began + 2), info); T16((u16)(began + 4), info);
                count = 2;
                if (f2) { count = 3; if (f1 > f2) f1 = f2; }
                D8(countAt, count);
                if (f1 < d8(minAt)) { output_if(o); D8(minAt, f1); D16(edgeAt, info); }
                return;
            }
            int i = 0;
            while (i < count && t8((u16)(faces + i)) != f1) i++;
            if (i == count) {       // face 1 opens
                if (count != 0x10) {
                    D8(countAt, (u8)(count + 1));
                    T8((u16)(faces + count), f1);
                    T16((u16)(began + 2 * count), info);
                    if (f1 < d8(minAt)) { output_if(o); D8(minAt, f1); D16(edgeAt, info); }
                }
            } else {                // face 1 closes
                for (int k = i; k < count; k++) {
                    T8((u16)(faces + k), t8((u16)(faces + k + 1)));
                    T16((u16)(began + 2 * k), t16((u16)(began + 2 * k + 2)));
                }
                count--;
                D8(countAt, count);
                if (f1 == d8(minAt)) {
                    output_if(o);
                    if (f2 && f2 <= f1) {
                        D8(minAt, f2); D16(edgeAt, info);
                        T8((u16)(faces + count), f2); T16((u16)(began + 2 * count), info);
                        D8(countAt, (u8)(count + 1));
                        return;
                    }
                    u8 low = t8((u16)(faces + count - 1));
                    int lowAt = count - 1;
                    for (int k = count - 2; k >= 0; k--) if (t8((u16)(faces + k)) < low) { low = t8((u16)(faces + k)); lowAt = k; }
                    if (f2 && f2 < low) {
                        D8(minAt, f2); D16(edgeAt, info);
                        T8((u16)(faces + count), f2); T16((u16)(began + 2 * count), info);
                        D8(countAt, (u8)(count + 1));
                        return;
                    }
                    D8(minAt, low);
                    D16(edgeAt, t16((u16)(began + 2 * lowAt)));
                }
            }
            if (!f2) return;
            f1 = f2; f2 = 0;
        }
    }

    // a marking's edge crossed.  A marking is a small table {object, face, .., at +0Ah the layers open now (the
    // highest first), ..} with a colour for each layer at +1+layer; the face's colour byte in the object's record
    // is swapped with the layer's while the layer is the top one, so the runs between come out in the marking's
    // colours.
    void marking_crossing() {
        const u8 pos = (u8)(0x100 - obj);
        const u16 mark = d16((u16)(kMarkPtr + 2 * edge));
        const u8 mo = t8(mark), mf = t8((u16)(mark + 1));
        const u8 top = t8((u16)(mark + 0xA));
        u16 with;       // the colour's place to swap with the face's
        if (pos == top) {
            const u8 under = t8((u16)(mark + 0xB));
            u16 p = mark;
            if (!under) {
                T8((u16)(mark + 0xA), 0);
                D8((u16)(kMarkOn + edge), 0);
                D8(XA(0x26D8, 0x3210), (u8)(d8(XA(0x26D8, 0x3210)) - 1));
                with = (u16)(mark + pos);
            } else {
                u8 c;
                do { c = t8((u16)(p + 0xB)); T8((u16)(p + 0xA), c); p++; } while (c);
                const u16 a = (u16)(mark + pos), b = (u16)(mark + under);
                const u8 t = t8((u16)(a + 1));
                T8((u16)(a + 1), t8((u16)(b + 1)));
                T8((u16)(b + 1), t);
                with = a;
            }
        } else if (pos > top) {
            u16 p = mark;
            u8 c = pos;
            do { const u8 was = t8((u16)(p + 0xA)); T8((u16)(p + 0xA), c); c = was; p++; } while (c);
            if (!top) {
                D8((u16)(kMarkOn + edge), 1);
                D8(XA(0x26D8, 0x3210), (u8)(d8(XA(0x26D8, 0x3210)) + 1));
                with = (u16)(mark + pos);
            } else {
                const u16 a = (u16)(mark + pos), b = (u16)(mark + top);
                const u8 t = t8((u16)(b + 1));
                T8((u16)(b + 1), t8((u16)(a + 1)));
                T8((u16)(a + 1), t);
                with = b;
            }
        } else {
            // (a layer under the top one opens or closes: the list only)
            u16 p = mark;
            u8 c = pos;
            for (;;) {
                p++;
                const u8 here = t8((u16)(p + 0xA));
                if (c < here) continue;
                if (c == here) {
                    u8 n;
                    do { n = t8((u16)(p + 0xB)); T8((u16)(p + 0xA), n); p++; } while (n);
                    return;
                }
                T8((u16)(p + 0xA), c);
                c = here;
                if (!c) return;
            }
        }
        u8 face = mf;
        if (mo == front && mf == d8((u16)(kMinFace + mo))) {
            output();       // (whatever the mask says, as the game does)
            face = d8((u16)(kMinFace + front));
        }
        const u16 colourAt = (u16)(record(mo) + 2 * face + 0x65);
        const u8 t = t8((u16)(with + 1));
        T8((u16)(with + 1), t8(colourAt));
        T8(colourAt, t);
    }

    void crossing() {
        if (obj > 0xF8) marking_crossing();
        else if (obj >= 0x80) { if (d8((u16)(kObjFlag + obj))) close_object(obj); else open_object(obj); }
        else mesh_crossing();
    }

    // the mask: for each row a flag byte (its top bit: the row begins closed) and run lengths, closed and open in
    // turn (a 0 then a byte: 256 and more)
    u16 mask_run() {
        u16 n = t8(maskAt);
        maskAt++;
        if (!n) { n = (u16)(0x100 | t8(maskAt)); maskAt++; }
        return n;
    }

    void walk() {
        heapTop = 0; startX = 0; front = 0; popFlag = 0;
        maskFlag = (Bit8s)t8(maskAt);
        maskAt++;
        maskX = mask_run();
        bool done = false;
        if (maskFlag < 0) { startX = maskX; if (maskX >= wide) done = true; }
        if (!done) {
            // the edges left of the window first, all at x 0, nothing drawn
            const Bit8s kept = maskFlag;
            maskFlag = -1;
            u16 marker = 0xC000;
            for (u16 at = kLeft; at < leftEnd; at = (u16)(at + 6), marker = (u16)(marker + 6)) {
                u16 rec = l16(at);
                if ((rec & 0x7F) == 0x7C) rec = t16((u16)(rec & XA(0xFF80, 0xFF80)));
                const u16 e = (u16)(rec + 4);
                obj = t8((u16)(e + 1)); edge = t8(e); info = marker; endX = 0;
                crossing();
            }
            maskFlag = kept;
            if (maskFlag >= 0) {
                if (front == 0xFFFF) front = nearest();
                D8((u16)(kObjFlag + front), 0xFF);
            }
            // the single and the tall crossings, by x (a single one first when both are at the same place)
            cursorN = kNewSorted; cursorT = kTall;
            for (;;) {
                u16 e;
                if (cursorN < sortedEnd) {
                    e = d16((u16)(cursorN + 2));
                    if (cursorT < tallEnd && t16((u16)(e + 2)) > t16((u16)(d16(cursorT) + 2))) { e = d16(cursorT); cursorT = (u16)(cursorT + 4); }
                    else cursorN = (u16)(cursorN + 4);
                } else if (cursorT < tallEnd) { e = d16(cursorT); cursorT = (u16)(cursorT + 4); }
                else break;
                edge = t8(e); obj = t8((u16)(e + 1)); info = t16((u16)(e + 2));
                endX = (u16)(info >> 7);
                if (endX >= maskX) {
                    runX = endX;
                    for (;;) {
                        maskFlag = (Bit8s)-maskFlag;
                        if (maskFlag >= 0) {
                            if (front == 0xFFFF) front = nearest();
                            D8((u16)(kObjFlag + front), 0xFF);
                            maskX = (u16)(maskX + mask_run());
                        } else {
                            endX = maskX;
                            output();
                            if (maskX >= wide) { done = true; break; }
                            const u16 n = mask_run();
                            startX = (u16)(startX + n);
                            maskX = (u16)(maskX + n);
                            if (maskX >= wide) { done = true; break; }
                        }
                        if (runX < maskX) break;
                    }
                    if (done) break;
                    endX = runX;
                }
                crossing();
            }
        }
        if (!done) {
            // the row's rest
            obj = 0;
            if (front == 0xFFFF) front = nearest();
            for (;;) {
                maskFlag = (Bit8s)-maskFlag;
                if (maskFlag < 0) {
                    endX = maskX;
                    output();
                    if (maskX >= wide) break;
                    const u16 n = mask_run();
                    maskX = (u16)(maskX + n);
                    startX = (u16)(startX + n);
                } else {
                    if (maskX >= wide) break;
                    maskX = (u16)(maskX + mask_run());
                }
            }
        }
        // the row's end: the markings' colours put back, every object closed
        if (d8(XA(0x26D8, 0x3210))) {
            D8(XA(0x26D8, 0x3210), 0);
            for (u16 m = d16(kMarkCount); m; m--) {
                if (!d8((u16)(kMarkOn + m))) continue;
                D8((u16)(kMarkOn + m), 0);
                const u16 mark = d16((u16)(kMarkPtr + 2 * m));
                const u8 mo = t8(mark), mf = t8((u16)(mark + 1));
                const u16 with = (u16)(mark + t8((u16)(mark + 0xA)));
                for (int k = 0; k < 8; k++) T8((u16)(mark + 0xA + k), 0);
                const u16 colourAt = (u16)(record(mo) + 2 * mf + 0x65);
                const u8 t = t8((u16)(with + 1));
                T8((u16)(with + 1), t8(colourAt));
                T8(colourAt, t);
            }
        }
        if (front != 0xFFFF) {
            D8((u16)(kObjFlag + front), 0);
            if (front < 0x80) D8((u16)(kOpenFaces + front), 0);
        }
        for (u16 i = heapTop; i; i = (u16)(i - 2)) {
            const u16 o = d16((u16)(kHeap + i));
            D8((u16)(kObjFlag + o), 0);
            if (o < 0x80) D8((u16)(kOpenFaces + o), 0);
        }
    }

    void run() {
        tallEnd = kTall; sortedEnd = kNewSorted; leftEnd = kLeft; rightEnd = kRight;
        memset(M + ds + kObjFlag, 0, 0x100);
        memset(M + ds + kOpenFaces, 0, 0x80);
        D8(XA(0x26D8, 0x3210), 0);
        maskAt = d16(kMaskAt);
        buffer = ((PhysPt)d16(kBufferSeg) << 4) + d16(kBufferOff);
        for (row = 0; row < deep && !stopped; row++, buffer += (PhysPt)wide) {
            gather();
            walk();
        }
    }
};

int g_nativeOn = -1;
bool g_nativeKeep = false;
Native *g_last = NULL;
unsigned g_nativeRuns = 0, g_nativeSorts = 0;

}  // namespace

// WCNET_T640_NATIVE=1: the quarters' scanline pass is this file's
bool tie640_native_on() {
    if (g_nativeOn < 0) {
        const char *e = getenv("WCNET_T640_NATIVE");
        g_nativeOn = e && e[0] ? atoi(e) : XA(2, 0);
        if (XA(0, 1) && g_nativeOn == 1) g_nativeOn = 2;    // (X-Wing: no quarters with this pass: its lists' segment is not a fixed one)   // (X-Wing: the quarters by the game's own code until its tracers are proved)
             // (the default: 2, the scene in one pass; 1 the quarters with this pass; 0 the quarters by the game's own)
        if (g_nativeOn) wclog(1, T640_GAME " 640: the scanline pass is native (WCNET_T640_NATIVE=%d)", g_nativeOn);
    }
    return g_nativeOn > 0;
}

// (the one pass's check: the runs of both drawings kept by quarter, [0] the quarters', [1] the one pass's)
std::vector<Native::RunLog> g_keptRuns[2][4];
static std::vector<std::pair<int, std::string> > g_keptNotes[2][4];
static int g_quarterNow = 0;
void tie640_native_quarter(int quarter) { g_quarterNow = quarter; }
void keep_runs(const Native &n, int set, int quarter) {
    if (!n.keep || quarter < 0 || quarter > 3) return;
    g_keptRuns[set][quarter] = n.runs;
    g_keptNotes[set][quarter] = n.notes;
}
void tie640_native_log_row(int set, int quarter, int row) {
    const char *who = set ? "one pass" : "quarters";
    for (size_t i = 0; i < g_keptNotes[set][quarter].size(); i++) if (g_keptNotes[set][quarter][i].first == row) wclog(1, "T640 %s: quarter %d row %d %s", who, quarter, row, g_keptNotes[set][quarter][i].second.c_str());
    for (size_t i = 0; i < g_keptRuns[set][quarter].size(); i++) {
        const Native::RunLog &r = g_keptRuns[set][quarter][i];
        if (r.row != row) continue;
        if (r.material < 0) wclog(1, "T640 %s: quarter %d row %d run %d..%d front %02X (crossing %02X/%02X %04X)", who, quarter, row, r.from, r.to, r.front, r.obj, r.edge, r.info);
        else wclog(1, "T640 %s: quarter %d row %d run %d..%d front %02X material %d: left %04X x %d light %d, right x %d light %d%s (crossing %02X/%02X %04X)", who, quarter, row, r.from, r.to, r.front, r.material,
                   r.left, r.leftX, r.leftLight, r.rightX, r.rightLight, r.found ? " (the crossing)" : "", r.obj, r.edge, r.info);
    }
}

// ONE PASS (wcnet_tie640_onepass.cpp): the scanline pass on an IMAGE of a quarter's memory, five blocks of 64 K in
// one piece: the data segment, the tables' segment, the lists' segment, the sprites' buffer, the renderer's code
// (its materials' colours).  The picture's quarter goes to (atX, atY).  False when the pass stopped at a depth
// sort the oracle does not know yet (`asked` then has the game routine's eight arguments): the image is spent,
// the pass is to be made again on a fresh one when the answer is there.
bool tie640_native_image(Bit8u *image, Bit8u *draw, int pitch, int limitX, int limitY, int atX, int atY, int quarter,
                         int (*oracle)(const Bit16u *args), Bit16u *asked) {
    static Native n;
    n.M = image;
    n.ds = 0; n.tb = 0x10000; n.ls = 0x20000; n.cs = 0x40000;
    n.D16(kBufferSeg, 0x3000);
    n.wide = n.d16(kWide);
    n.deep = n.d16(kDeep);
    n.draw = draw; n.pitch = pitch; n.limitX = limitX; n.limitY = limitY; n.atX = atX; n.atY = atY;
    n.sorts = 0;
    n.oracle = oracle; n.stopped = false;
    n.keep = g_nativeKeep; n.runs.clear(); n.notes.clear();
    n.run();
    if (n.stopped) { memcpy(asked, n.asked, sizeof(n.asked)); return false; }
    keep_runs(n, 1, quarter);
    g_nativeRuns++;
    return true;
}

// xtrans2_drawxtrans (1CCA:85E9) has been called: the whole pass done here, into the 640 picture at (atX, atY), and
// the routine left (its far return).
void tie640_native_scanlines(Bit16u loadSeg, Bit8u *draw, int pitch, int limitX, int limitY, int atX, int atY) {
    static Native n;
    n.M = GetMemBase();
    n.ds = (PhysPt)(Bit16u)(loadSeg + kDataPara) << 4;
    n.ls = (PhysPt)(Bit16u)(loadSeg + kListPara) << 4;
    n.cs = (PhysPt)(Bit16u)(loadSeg + kRendPara) << 4;
    n.tb = (PhysPt)n.d16(kTablesSeg) << 4;
    n.wide = n.d16(kWide);
    n.deep = n.d16(kDeep);
    const int corner = n.d16(kCorner);
    n.draw = draw; n.pitch = pitch; n.limitX = limitX; n.limitY = limitY;
    n.atX = atX + corner % 320; n.atY = atY + corner / 320;
    n.sorts = 0;
    n.oracle = NULL; n.stopped = false;
    n.keep = g_nativeKeep; n.runs.clear(); n.notes.clear();
    g_last = &n;
    n.run();
    keep_runs(n, 0, g_quarterNow);
    g_nativeRuns++;
    g_nativeSorts += n.sorts;
    // as the game's routine leaves things: FS the lists' segment, ES the tables'
    SegSet16(fs, (Bit16u)(loadSeg + kListPara));
    SegSet16(es, n.d16(kTablesSeg));
    const Bit16u ip = CPU_Pop16();
    const Bit16u seg = CPU_Pop16();
    SegSet16(cs, seg);
    reg_eip = ip;
}

// (the check's reference drawing: the game's own shaded runs, taken at 1CCA:98BF where the right edge is known)
struct RefShade { int row, from, to, front, rightX, rightInfo, left; };
static std::vector<RefShade> g_refShades;
void tie640_native_ref(Bit16u loadSeg, int what) {
    if (what == 0) { g_refShades.clear(); return; }
    const PhysPt ds = (PhysPt)(Bit16u)(loadSeg + kDataPara) << 4;
    RefShade r;
    if (what == 2) {        // (xtrans2_outputxt's entry: every run)
        r.row = mem_readw(ds + XA(0x28D4, 0x3419)); r.from = mem_readw(ds + XA(0x28C4, 0x3409)); r.to = mem_readw(ds + XA(0x28C6, 0x340B)); r.front = mem_readw(ds + XA(0x28DC, 0x3421));
        if (r.to <= r.from) return;
        r.rightX = -1; r.rightInfo = mem_readw(ds + XA(0x28BE, 0x3403)); r.left = (mem_readw(ds + XA(0x28BC, 0x3401)) << 8) | mem_readb(ds + XA(0x28BA, 0x33FF));
        g_refShades.push_back(r);
        return;
    }
    r.row = mem_readw(ds + XA(0x28D4, 0x3419)); r.from = mem_readw(ds + XA(0x28C4, 0x3409)); r.to = mem_readw(ds + XA(0x28C6, 0x340B)); r.front = mem_readw(ds + XA(0x28DC, 0x3421));
    r.rightX = reg_cx; r.rightInfo = reg_di; r.left = mem_readw(ds + kMinEdge + 2 * (r.front & 0x7F));
    g_refShades.push_back(r);
}

// the check found a difference in the quarter just compared: the native pass's runs of that row
void tie640_native_keep(bool on) { g_nativeKeep = on; }
void tie640_native_row(int row) {
    if (!g_last) return;
    for (size_t i = 0; i < g_refShades.size(); i++) {
        const RefShade &r = g_refShades[i];
        if (r.row != row) continue;
        if (r.rightX < 0) wclog(1, "T640 native: the game's row %d run %d..%d front %02X (crossing %02X/%02X %04X)", row, r.from, r.to, r.front, r.left >> 8, r.left & 0xFF, r.rightInfo);
        else wclog(1, "T640 native: the game's row %d   shaded: left %04X, right x %d light/info %04X", row, r.left, r.rightX, r.rightInfo);
    }
    for (size_t i = 0; i < g_last->notes.size(); i++) if (g_last->notes[i].first == row) wclog(1, "T640 native: row %d %s", row, g_last->notes[i].second.c_str());
    for (size_t i = 0; i < g_last->runs.size(); i++) {
        const Native::RunLog &r = g_last->runs[i];
        if (r.row != row) continue;
        if (r.material < 0) wclog(1, "T640 native: row %d run %d..%d front %02X (crossing %02X/%02X %04X)", row, r.from, r.to, r.front, r.obj, r.edge, r.info);
        else wclog(1, "T640 native: row %d run %d..%d front %02X material %d: left %04X x %d light %d, right x %d light %d%s (crossing %02X/%02X %04X)", row, r.from, r.to, r.front, r.material,
                   r.left, r.leftX, r.leftLight, r.rightX, r.rightLight, r.found ? " (the crossing)" : "", r.obj, r.edge, r.info);
    }
}

// The two fills a full update begins with (0090:0B21, 0B26), each one string instruction of the game's that costs
// its 30,000 repeats in emulated time: logbuf2_clearbuffer (1CCA:D772: the sprites' buffer to the background
// colour) and xtrans2_clearruntable (1CCA:8510: the "last frame's runs" ring made to match nothing).  The same
// bytes written here, the routine left.
bool tie640_native_fill(Bit16u loadSeg, Bit32u ip) {
    Bit8u *M = GetMemBase();
    const PhysPt ds = (PhysPt)(Bit16u)(loadSeg + kDataPara) << 4;
    if (ip == XA(0xD772, 0xDB94)) {
        const Bit16u off = host_readw(M + ds + kBufferOff), words = host_readw(M + ds + XA(0x48D9, 0x543F));
        Bit8u *to = M + ((PhysPt)host_readw(M + ds + kBufferSeg) << 4);
        const Bit8u c = M[ds + kBackColour];
        for (unsigned i = 0; i < 2u * words; i++) to[(Bit16u)(off + i)] = c;
#ifndef T640_XWING     // (X-Wing's run tables are in a block the routine asks for: its own code makes them)
    } else if (ip == 0x8510) {
        Bit8u *tb = M + ((PhysPt)host_readw(M + ds + kTablesSeg) << 4);
        Bit16u at = (Bit16u)(host_readw(M + ds + XA(0xD8A4, 0xFFFE)) + XA(0xA000, 0xA000));
        host_writew(M + ds + XA(0x28E8, 0x342D), at);
        host_writew(M + ds + XA(0x28EE, 0x3433), at);
        host_writew(M + ds + XA(0x28EC, 0x3431), (Bit16u)(at + XA(0x2EDE, 0x2EDE)));
        const int rows = host_readw(M + ds + kDeep);
        Bit16u run = (Bit16u)(host_readw(M + ds + kWide) - 1);
        for (int r = 0; r < rows; r++) {
            if (!(run >> 8)) { host_writew(tb + at, (Bit16u)(0xFF00 | run)); at = (Bit16u)(at + 2); }
            else { host_writew(tb + at, 0xFFFF); host_writew(tb + (Bit16u)(at + 2), (Bit16u)(0xFF00 | ((run - 0x100) & 0xFF))); at = (Bit16u)(at + 4); }
        }
        host_writew(M + ds + XA(0x28EA, 0x342F), at);
#endif
    } else return false;
    const Bit16u rip = CPU_Pop16();
    const Bit16u seg = CPU_Pop16();
    SegSet16(::cs, seg);
    reg_eip = rip;
    return true;
}

void tie640_native_say() {
    wclog(1, "TIE demo 640 native: %u scanline passes, the game's depth sort called %u times", g_nativeRuns, g_nativeSorts);
}

}  // namespace wc
