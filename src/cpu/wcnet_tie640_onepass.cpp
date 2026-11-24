/*
 *  The TIE Fighter demo at 640x400: the scene in ONE PASS (docs/tie640.md 4b; WCNET_T640_NATIVE=2).
 *
 *  The quarters (wcnet_tie640.cpp) run the game's scene section four times a frame, because the renderer's lower
 *  half cannot hold a picture wider than 320 pixels (an x is 9 bits in its lists, a row a byte).  Here the section
 *  runs ONCE, with the window 640x378: the game's upper half (objects, transforms, lighting, the BSP walk, the
 *  markings, the edges' classification) is interpreted once, and what it hands to the lower half is taken down
 *  instead of traced:
 *
 *      Capture     every call of the lower half's entries, with what the call was given, in order
 *      Replay      for each quarter of the picture: an image of the game's memory as that quarter's own pass
 *                  would have it, the calls made again on it by the hand-written tracers (wcnet_tie640_trace.cpp)
 *                  with the screen points moved by the quarter's corner, and what depends on the window worked
 *                  out again for the quarter's 320x189 (an edge's side of the window, which objects and faces the
 *                  game would draw there, their numbers)
 *      Resolve     the hand-written scanline pass (wcnet_tie640_native.cpp) on each quarter's image
 *
 *  So the formats stay the game's own 320x189 ones, and the picture is the quarters' picture: the same lists, the
 *  same dither at the quarters' edges.  Only the interpreted upper half is run once instead of four times.
 *
 *  Written from the disassembly of the demo (the research notes, native.md section 11); nothing translated by
 *  machine.  WCNET_T640_CHECK=3 draws every frame first in quarters, puts the memory back, draws it in one pass
 *  and compares the two pictures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "wcnet_t640_game.h"
#include "wcnet_tie640.h"
#include "wcnet_log.h"
#include "wcnet_perf.h"
#include "regs.h"
#include "cpu.h"
#include "mem.h"
#include "pic.h"
#include "callback.h"
#include "wc_net.h"

namespace wc {

// (wcnet_tie640_trace.cpp: the image the tracers work on, and the four of them)
struct TraceMem {
    Bit8u *ds, *tb;     // the data segment, the tables' segment (64 K each)
    const Bit8u *cs;    // the renderer's code segment (for the materials' table in it; may be null)
    // (words and longs through memcpy: the emulator's host_readw / host_writed are casts of the pointer, and with
    // them the compiler may take a word read not to see a long written just before -- it did, in the lines' twin)
    static inline Bit16u get16(const Bit8u *p) { Bit16u v; memcpy(&v, p, 2); return v; }
    static inline Bit32u get32(const Bit8u *p) { Bit32u v; memcpy(&v, p, 4); return v; }
    static inline void put16(Bit8u *p, Bit16u v) { memcpy(p, &v, 2); }
    static inline void put32(Bit8u *p, Bit32u v) { memcpy(p, &v, 4); }
    inline Bit16u d16(Bit16u o) const { return get16(ds + o); }
    inline Bit8u d8(Bit16u o) const { return ds[o]; }
    inline Bit32u d32(Bit16u o) const { return get32(ds + o); }
    inline void D16(Bit16u o, Bit16u v) { put16(ds + o, v); }
    inline void D8(Bit16u o, Bit8u v) { ds[o] = v; }
    inline void D32(Bit16u o, Bit32u v) { put32(ds + o, v); }
    inline Bit16u t16(Bit16u o) const { return get16(tb + o); }
    inline Bit8u t8(Bit16u o) const { return tb[o]; }
    inline Bit32u t32(Bit16u o) const { return get32(tb + o); }
    inline void T16(Bit16u o, Bit16u v) { put16(tb + o, v); }
    inline void T8(Bit16u o, Bit8u v) { tb[o] = v; }
    inline void T32(Bit16u o, Bit32u v) { put32(tb + o, v); }
};
void tie640_trace_drawface(TraceMem &m, const Bit8u *face, int vertices);
int tie640_trace_flat_polygon(TraceMem &m, Bit16u strayHigh, Bit16u strayEaxHigh, int *kind);
void tie640_trace_thick_line(TraceMem &m, Bit16u q, Bit16u strayHigh, int *kind);
void tie640_trace_sprite_rows(TraceMem &m, Bit8u *bg, int *kind);
// (wcnet_tie640_native.cpp)
bool tie640_native_image(Bit8u *image, Bit8u *draw, int pitch, int limitX, int limitY, int atX, int atY, int quarter,
                         int (*oracle)(const Bit16u *args), Bit16u *asked);
void tie640_native_log_row(int set, int quarter, int row);
bool tie640_fast_sprite_off();      // (wcnet_tie640_fast.cpp: the scaler's call at hand can reach no pixel of the window)

namespace {

typedef Bit8u u8; typedef Bit16u u16; typedef Bit32u u32; typedef Bit32s s32;

enum {
    kRendPara = XA(0x1CCA, 0x222D), kDataPara = XA(0x421F, 0x3F1C), kDrawPara = XA(0x168C, 0x1D5D), kTablesSeg = XA(0xD8A6, 0xD34A),
    // the renderer's places (1CCA)
    kDrawObject = XA(0x0AAB, 0x0B9B),       // drawpol_drawpolyobject: every polygon object comes through here
    kDrawObjectOut = XA(0x0B41, 0x0C31),    // ... and leaves here (AX: drawn or not)
    kTraceFace = XA(0x53F3, 0x559D), kTraceFlat = XA(0x4FF3, 0x519D), kTraceLine = XA(0x2BB0, 0x2D30),
    kMarkings = XA(0x1CFC, 0x1E5C), kMarkingsOut = XA(0x2070, 0x21E8),
    kSprite = XA(0xA02A, 0xA44D), kSpriteRows = XA(0xA132, 0xA555), kScanlines = XA(0x85E9, 0x89A7),
    // the data segment: an object being drawn
    kVertexPoint = XA(0x213A, 0x2C72), kVertexLight = XA(0x223A, 0x2D72), kFaceResult = XA(0x203A, 0x2B72), kObjects = XA(0x233A, 0x2E72),
    kGouraud = XA(0x243A, 0x2F72), kColour = XA(0x2447, 0x2F7F), kObject = XA(0x2464, 0x2F9C), kRecordTop = XA(0x2468, 0x2FA0), kFaces = XA(0x246E, 0x2FA6), kEdges = XA(0x2470, 0x2FA8),
    kFace = XA(0x2474, 0x2FAC), kFaceTable = XA(0x247A, 0x2FB2), kRecord = XA(0x247E, 0x2FB6), kParent = XA(0x2480, 0x2FB8), kIdLow = XA(0x2484, 0x2FBC),
    kFaceBody = XA(0x2486, 0x2FBE), kFaceSeg = XA(0x2488, 0x2FC0), kShadeMode = XA(0x26DC, 0x3214), kNearFlag = XA(0x276A, 0x32AA), kSpareEdge = XA(0x2764, 0x32A4),
    kPointA = XA(0x2AF0, 0x3635), kPointB = XA(0x2BF0, 0x3735), kAbsDx = XA(0x3090, 0x3BD5), kAbsDy = XA(0x3978, 0x44BD), kSignDx = XA(0x28F0, 0x3435), kSignDy = XA(0x2970, 0x34B5),
    kFlags = XA(0x2FF0, 0x3B35), kSlopeFrac = XA(0x2DF0, 0x3935), kSlopeLo = XA(0x2EF0, 0x3A35), kSlopeHi = XA(0x3B78, 0x46BD),
    // ... flat polygons, lines, markings
    kRingFirst = XA(0x2452, 0x2F8A), kRingEnd = XA(0x2454, 0x2F8C), kMinX = XA(0x2456, 0x2F8E), kMaxX = XA(0x2458, 0x2F90), kMinY = XA(0x245A, 0x2F92), kMaxY = XA(0x245C, 0x2F94),
    kSameRow = XA(0x2460, 0x2F98), kSameColumn = XA(0x2462, 0x2F9A), kPoints = XA(0x2472, 0x2FAA),
    kFlatNext = XA(0x26E3, 0x321B), kFlatColour = XA(0x410C, 0x4C51), kFlatComponent = XA(0x428C, 0x4DD1), kFlatParent = XA(0x418C, 0x4CD1),
    kFlatX = XA(0x3E0C, 0x4951), kFlatY = XA(0x3F0C, 0x4A51), kFlatZ = XA(0x400C, 0x4B51),
    kThick = XA(0x2730, 0x3268), kLineP = XA(0x2732, 0x326A), kLightP = XA(0x2738, 0x3270), kLightQ = XA(0x273A, 0x3272), kAcrossStep = XA(0x273E, 0x3276), kId = XA(0x274C, 0x3284),
    kMarkPtr = XA(0x2552, 0x308A), kMarkOf = XA(0x2652, 0x318A), kMarkCount = XA(0x26D2, 0x320A),
    kPool = XA(0x274A, 0x3282), kRows = XA(0x6232, 0xD3C8),
    kWide = XA(0x48C5, 0x5423), kDeep = XA(0x48D1, 0x5433), kBufferSeg = XA(0x48BD, 0x541B), kBufferOff = XA(0x48BF, 0x541D),
    kBox = XA(0x48B2, 0x5410), kLastRow = XA(0x48D3, 0x5437), kSpriteMap = XA(0x48A2, 0x5400), kSpriteX = XA(0xC448, 0xC282), kSpriteY = XA(0xC44C, 0xC286), kSpriteZ = XA(0xC458, 0xC28A),
};

// ---------------------------------------------------------------------------------------------------------------
// Capture
//
// What a call was given, copied out of the game's memory when it was made.  The screen points are the one pass's
// (640x378); everything else is as the game's own pass of any quarter would have it.
// ---------------------------------------------------------------------------------------------------------------
struct Pt { s32 x, y; };
struct EdgeIn {         // an edge of a mesh object as transfm2_classifyedges left it
    u8 e, flags;        // (flags: the one pass's; 80 alone = both ends behind the near plane, no points)
    u8 signDx, signDy;
    u16 a, b;           // where its two points are in the data segment
    Pt pa, pb;
    u32 dx, dy;
    u16 slope[3];
};
struct VertIn { u8 v; u16 at, light; Pt p; };       // a vertex: where its screen point is (0: behind), the point, its light

enum Kind { kEnter, kObjInfo, kFaceCall, kLineCall, kFlatCall, kMarkBegin, kMarkEnd, kLeave, kSpriteCall };
enum Context { kOfFace, kOfMarking, kOfObject };

struct Event {
    Kind kind;
    Context context;
    size_t edges, verts, bytes, points;     // where its things begin in the pools
    int n;                                  // how many of them
    // kEnter, kLeave
    bool fromBolt;          // called by 168C:06CA, which puts the flat objects' counter back and then counts one
    bool committed;         // the object took a number
    // kObjInfo
    int faces, edgeCount, marks;
    bool nearEye;
    // kFaceCall
    u16 cx, strayCx, strayAx;
    bool bsp;
    u8 colour, fileIndex, gouraud, shadeMode, header;
    // kLineCall
    u16 p, q, thick, lightP, lightQ, id, flatNext;
    bool lightsNew;
    u8 flatColour; u16 flatParent, flatX, flatY, flatZ;
    // kFlatCall
    u16 ringFirst, ringEnd, minX, maxX, minY, maxY, pointCount, parent, idLow;
    u8 sameRow, sameColumn, polyColour;
    // kMarkBegin, calls of a marking
    int mark;
    // kSpriteCall
    int sprite;             // its SpriteIn
};

// A sprite (or the backdrop) drawn by the game's own scaler, once for each quarter, into that quarter's own copy of
// the 320x189 buffer; what its row pass (1CCA:A132) was given there, and the buffer as the scaler left it
struct SpriteIn {
    bool has[4], pixels[4], stale[4];
    u8 box[4][8];
    u32 at[4][3];
    u16 parent[4], idLow[4];
    u8 map[4][0x40];
    size_t snap[4];         // where the buffer's copy is in g_snaps
};

struct Capture {
    std::vector<Event> events;
    std::vector<EdgeIn> edges;
    std::vector<VertIn> verts;
    std::vector<u8> bytes;
    std::vector<Pt> points;
    bool infoDone;          // the object being drawn has its kObjInfo
    bool fromBolt;
    u16 markTop;            // the records' top when the markings of a face began
    u8 markRaw[10], markTables[64]; bool markSame, markHas;   // the marking's own colours as its data has them (1CCA:1FB0)
    u16 lightP, lightQ;     // the lines' two lights as last seen (they are left over from call to call)
    void clear() { events.clear(); edges.clear(); verts.clear(); bytes.clear(); points.clear(); infoDone = false; fromBolt = false; }
};

// the game's memory, live
struct Live {
    u8 *M;
    PhysPt ds, tb;
    void at(Bit16u loadSeg) {
        M = GetMemBase();
        ds = (PhysPt)(u16)(loadSeg + kDataPara) << 4;
        tb = (PhysPt)d16(kTablesSeg) << 4;
    }
    inline u16 d16(u16 o) const { return TraceMem::get16(M + ds + o); }
    inline u8 d8(u16 o) const { return M[ds + o]; }
    inline u32 d32(u16 o) const { return TraceMem::get32(M + ds + o); }
    inline void D16(u16 o, u16 v) { TraceMem::put16(M + ds + o, v); }
    inline u8 t8(u16 o) const { return M[tb + o]; }
    inline Pt point(u16 o) const { Pt p; p.x = (s32)d32(o); p.y = (s32)d32((u16)(o + 4)); return p; }
};

Capture g_cap;
std::vector<SpriteIn> g_sprites;
std::vector<u8> g_snaps;
std::vector<u8> g_quarterBg[4];     // each quarter's buffer, as the sprites drawn so far have left it
struct SpriteRun { bool active, backdrop; int q, sprite; u16 x, y, retIp, retCs; } g_run = { false, false, 0, 0, 0, 0, 0, 0 };
bool g_backdropNext = false;
std::vector<u8> g_scratch; bool g_nativeMap = true;
// The sprites' scaler run ONCE a sprite, at the game's own scale into a buffer of its own, and its pixels put into the
// four quarters' buffers twice the size (WCNET_T640_SPR1X=0: the scaler run in each quarter at twice the scale, as
// before).  The user, 2026-11-23, of the one pass in the browser: "unacceptably slow, especially when any kind of
// explosion renders"; measured there against the host's clock, a crowd with six kills: the scaler's runs inside the
// hook (four a sprite, each four times the pixels) were 10.1 s of 30, the emulator at 66 to 84% of real time and a
// second with no frame at all.  The art is the game's 320 art either way.
bool g_spr1x = true, g_small = false;
std::vector<u8> g_smallBg;
struct SmallIn { bool stale; u8 box[8]; u32 at[3]; u16 parent, idLow; u8 map[0x40]; } g_smallIn;
enum { kRowOffsetsCs = XA(0xCFE0, 0xD400) };    // (the code segment's: a row's offset in the buffer, words)
struct SprSig { u16 w[16]; };      // (the check) x, y, scale, image off/seg, the box at the row pass, 4320, 435C, 435E, 50BC
std::vector<SprSig> g_sprRef[4], g_sprOne[4]; int g_sprLogs = 0;
u8 g_quarterScaler[4][0x80];     // DS:430C..438B as the scaler left them in each quarter
// (off: a call left out leaves the scaler's variables as they were, and the scanline pass reads them)
bool g_cullSprites = false; unsigned g_spritesOff = 0, g_spritesRun = 0;
// The scaler's box (DS:48B2..48B9) is written only when the scaler draws something; its row pass uses whatever is
// there.  So each quarter has its own, as its own pass would leave it from sprite to sprite.
u8 g_quarterBox[4][8]; bool g_quarterBoxKnown[4]; bool g_runStale = false; unsigned g_staleRuns = 0;
const u8 kBoxUnset[8] = { 0xAD, 0x7B, 0xAD, 0x7B, 0xAD, 0x7B, 0xAD, 0x7B };
enum { kBgBytes = 0x10000 };
int g_wide = 320, g_deep = 189; // the game's own window
Bit16u g_load = 0;
int g_on = -1, g_check = -1;
bool g_noSprites = false;

// The sprites' buffer (a segment of conventional memory), copied whole: straight from the emulator's memory (the
// emulator's own block copies go byte by byte through the paging's handlers: a frame's cost in the browser)
void bg_read(PhysPt at, void *to, Bitu n) {
    if (at + n <= 0xA0000) memcpy(to, GetMemBase() + at, n); else MEM_BlockRead(at, to, n);
}
void bg_write(PhysPt at, const void *from, Bitu n) {
    if (at + n <= 0xA0000) memcpy(GetMemBase() + at, from, n); else MEM_BlockWrite(at, from, n);
}

// The game's own code run by the interpreter until it comes to the idle place (PSP:0060, `jmp $` written there: the
// FCB's bytes, which the program does not use), with no emulated time passing: the cycles and the periodic call's
// count are put back.  Hooks of the one pass's own (the row pass) are made as ever.  False: it did not get there.
enum { kIdleAt = 0x60 };
bool g_nested = true, g_skipEntry = false;
unsigned g_nestedRuns = 0, g_nestedSlices = 0;
double g_costNestedMs = 0, g_costSpriteMs = 0, g_costHookMs = 0;   // (the host's clock: the scaler's runs, the sprites' whole handling, every hook of the one pass)
unsigned g_costSprites = 0, g_costHooks = 0;
bool run_to_idle(u16 stubSeg) {
    const PhysPt stub = ((PhysPt)stubSeg << 4) + kIdleAt;
    mem_writeb(stub, 0xEB); mem_writeb(stub + 1, 0xFE);
    const Bits keepCycles = CPU_Cycles;
    const Bit32s keepCount = wc_net_countdown;
    wc_net_countdown = 0x3FFFFFFF;
    bool ok = true;
    unsigned slices = 0;
    while (!(SegValue(cs) == stubSeg && reg_eip == kIdleAt)) {
        CPU_Cycles = 4000;
        Bits r = (*cpudecoder)();
        // (a callback's instruction met -- the EMS driver's, when the picture's page is mapped: made as the
        // emulator's own loop makes it)
        if (r > 0 && r < CB_MAX && CallBack_Handlers[r] && (*CallBack_Handlers[r])() == CBRET_NONE) r = 0;
        if (r != 0 || ++slices > 100000) {
            wclog(0, T640_GAME " 640: the scaler's run inside the hook ended oddly (core %d, %u slices, at %04X:%04X): the sprites go the slower way", (int)r, slices, (unsigned)SegValue(cs), (unsigned)reg_eip);
            ok = false;
            break;
        }
    }
    g_nestedRuns++; g_nestedSlices += slices;
    CPU_Cycles = keepCycles;
    wc_net_countdown = keepCount;
    return ok;
}

Event &add(Kind kind) {
    Event e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.edges = g_cap.edges.size(); e.verts = g_cap.verts.size(); e.bytes = g_cap.bytes.size(); e.points = g_cap.points.size();
    g_cap.events.push_back(e);
    return g_cap.events.back();
}

// the far return address of the routine the CPU is at the entry of
u16 return_ip() { return mem_readw(SegPhys(ss) + (u16)reg_sp); }
u16 return_cs() { return mem_readw(SegPhys(ss) + (u16)(reg_sp + 2)); }
void far_return() {
    const u16 ip = CPU_Pop16();
    const u16 seg = CPU_Pop16();
    SegSet16(cs, seg);
    reg_eip = ip;
}

// A mesh object's first call: what decides, quarter by quarter, whether the game would draw the object at all and
// which of its faces.  drawpol's first loop has classified every face by now (1CCA:120B, 1784): the edges' flags
// and points, each face's result (DS:203A[]).  Taken: the record's head, every edge's flags and points, every
// face's edges, result and "two-sided" bit.
void capture_object(const Live &L) {
    Event &ev = add(kObjInfo);
    ev.faces = L.d16(kFaces);
    ev.edgeCount = L.d16(kEdges);
    ev.marks = L.d16(kMarkCount);
    ev.nearEye = L.d16(kNearFlag) != 0;
    const u16 rec = L.d16(kRecord);
    for (int i = 0; i < 0x27; i++) g_cap.bytes.push_back(L.t8((u16)(rec + i)));
    for (int e = 0; e < ev.edgeCount; e++) {
        EdgeIn in;
        memset(&in, 0, sizeof(in));
        in.e = (u8)e;
        in.flags = L.d8((u16)(kFlags + e));
        if (in.flags & 0x0F) {
            in.pa = L.point(L.d16((u16)(kPointA + 2 * e)));
            in.pb = L.point(L.d16((u16)(kPointB + 2 * e)));
        }
        g_cap.edges.push_back(in);
    }
    // the faces: {vertices, header, result, a vertex behind the near plane, its edges}
    const PhysPt seg = (PhysPt)L.d16(kFaceSeg) << 4;
    const u16 table = L.d16(kFaceTable);
    for (int f = 0; f < ev.faces; f++) {
        const u16 entry = (u16)(table + 8 * f);
        const u16 head = (u16)(entry + mem_readw(seg + (u16)(entry + 6)));
        const u8 header = mem_readb(seg + head);
        const int n = header & 0x3F;
        g_cap.bytes.push_back((u8)n);
        g_cap.bytes.push_back(header);
        g_cap.bytes.push_back(L.d8((u16)(kFaceResult + f)));
        bool behind = false;
        const size_t flagAt = g_cap.bytes.size();
        g_cap.bytes.push_back(0);
        for (int k = 0; k < n; k++) {
            const u8 v = mem_readb(seg + (u16)(head + 1 + 2 * k));
            if (!L.d16((u16)(kVertexPoint + 2 * v))) behind = true;
            g_cap.bytes.push_back(mem_readb(seg + (u16)(head + 2 + 2 * k)));
        }
        g_cap.bytes[flagAt] = behind ? 1 : 0;
    }
    g_cap.infoDone = true;
}

// trace2_drawface (1CCA:53F3) called: CX = the vertices, the face's body at [2488]:[2486]
void capture_face(const Live &L) {
    if (!g_cap.infoDone) capture_object(L);
    Event &ev = add(kFaceCall);
    ev.cx = reg_cx;
    ev.strayCx = (u16)(reg_ecx >> 16);
    ev.n = reg_cx & 0xFF;
    const u16 ret = return_ip();
    ev.bsp = ret == XA(0x193D, 0x1A7D) || ret == XA(0x1A1A, 0x1B5A);
    const u16 rec = L.d16(kRecord), face = L.d16(kFace);
    ev.colour = L.t8((u16)(rec + 2 * face + 0x65));
    ev.fileIndex = L.t8((u16)(rec + 2 * face + 0x66));
    ev.gouraud = L.d8(kGouraud);
    ev.shadeMode = L.d8(kShadeMode);
    const PhysPt body = ((PhysPt)L.d16(kFaceSeg) << 4) + L.d16(kFaceBody);
    ev.header = mem_readb(body - 1);
    for (int i = 0; i < 2 * ev.n + 2; i++) g_cap.bytes.push_back(mem_readb(body + (PhysPt)i));     // (the ship's data: in the EMS page frame)
    const u8 *b = &g_cap.bytes[ev.bytes];
    for (int k = 0; k <= ev.n; k++) {
        VertIn in;
        memset(&in, 0, sizeof(in));
        in.v = b[2 * k];
        in.at = L.d16((u16)(kVertexPoint + 2 * in.v));
        in.light = L.d16((u16)(kVertexLight + 2 * in.v));
        if (in.at) in.p = L.point(in.at);
        g_cap.verts.push_back(in);
    }
    for (int k = 0; k < ev.n; k++) {
        EdgeIn in;
        memset(&in, 0, sizeof(in));
        const int e = b[2 * k + 1];
        in.e = (u8)e;
        in.flags = L.d8((u16)(kFlags + e));
        in.signDx = L.d8((u16)(kSignDx + e));
        in.signDy = L.d8((u16)(kSignDy + e));
        in.a = L.d16((u16)(kPointA + 2 * e));
        in.b = L.d16((u16)(kPointB + 2 * e));
        if (in.flags & 0x0F) { in.pa = L.point(in.a); in.pb = L.point(in.b); }
        in.dx = L.d32((u16)(kAbsDx + 4 * e));
        in.dy = L.d32((u16)(kAbsDy + 4 * e));
        in.slope[0] = L.d16((u16)(kSlopeFrac + 2 * e));
        in.slope[1] = L.d16((u16)(kSlopeLo + 2 * e));
        in.slope[2] = L.d16((u16)(kSlopeHi + 2 * e));
        g_cap.edges.push_back(in);
    }
}

// drawln2_tracelineedges (1CCA:2BB0) called: DI and [2732] the two points; by whom is told by the return address
void capture_line(Live &L) {
    const u16 ret = return_ip();
    const Context context = ret == XA(0x21C0, 0x2338) ? kOfFace : ret == XA(0x20CD, 0x2245) ? kOfMarking : kOfObject;
    if (context == kOfFace && !g_cap.infoDone) capture_object(L);
    Event &ev = add(kLineCall);
    ev.context = context;
    ev.strayCx = (u16)(reg_ecx >> 16);
    ev.q = reg_di;
    ev.p = L.d16(kLineP);
    g_cap.points.push_back(L.point(ev.p));
    g_cap.points.push_back(L.point(ev.q));
    ev.thick = L.d16(kThick);
    ev.lightP = L.d16(kLightP);
    ev.lightQ = L.d16(kLightQ);
    ev.lightsNew = context == kOfFace || ev.lightP != g_cap.lightP || ev.lightQ != g_cap.lightQ;
    g_cap.lightP = ev.lightP; g_cap.lightQ = ev.lightQ;
    ev.id = L.d16(kId);
    ev.flatNext = L.d16(kFlatNext);
    if (context == kOfFace) {
        const u16 rec = L.d16(kRecord), face = L.d16(kFace);
        ev.colour = L.t8((u16)(rec + 2 * face + 0x65));
        ev.fileIndex = L.t8((u16)(rec + 2 * face + 0x66));
        ev.gouraud = L.d8(kGouraud);
        ev.shadeMode = L.d8(kShadeMode);
    } else if (context == kOfMarking) {
        ev.mark = L.d16(kIdLow);
    } else {
        // (a line object's line: the caller has written the flat objects' tables at the counter's place)
        const u16 n = ev.flatNext;
        ev.flatColour = L.d8((u16)(kFlatColour + n));
        ev.flatParent = L.d16((u16)(kFlatParent + 2 * n));
        ev.flatX = L.d16((u16)(kFlatX + 2 * n));
        ev.flatY = L.d16((u16)(kFlatY + 2 * n));
        ev.flatZ = L.d16((u16)(kFlatZ + 2 * n));
    }
}

// trace2_drawscreencoords (1CCA:4FF3) called: the ring of points.  Gives the game's AX as the 640x378 window has
// it (0: wholly outside).
int capture_flat(Live &L) {
    const u16 ret = return_ip();
    Event &ev = add(kFlatCall);
    ev.context = ret == XA(0x1F80, 0x20F4) ? kOfMarking : kOfObject;
    ev.strayCx = (u16)(reg_ecx >> 16);
    ev.strayAx = (u16)(reg_eax >> 16);
    ev.ringFirst = L.d16(kRingFirst); ev.ringEnd = L.d16(kRingEnd);
    ev.minX = L.d16(kMinX); ev.maxX = L.d16(kMaxX); ev.minY = L.d16(kMinY); ev.maxY = L.d16(kMaxY);
    ev.sameRow = L.d8(kSameRow); ev.sameColumn = L.d8(kSameColumn);
    ev.pointCount = L.d16(kPoints);
    ev.polyColour = L.d8(kColour);
    ev.parent = L.d16(kParent);
    ev.idLow = L.d16(kIdLow);
    ev.mark = ev.idLow;
    ev.flatNext = L.d16(kFlatNext);
    ev.n = 0;
    for (u16 at = ev.ringFirst; at < ev.ringEnd && ev.n < 64; at = (u16)(at + 8), ev.n++) g_cap.points.push_back(L.point(at));
    const Pt top = L.point(ev.maxY), right = L.point(ev.maxX), first = L.point(ev.minY), left = L.point(ev.minX);
    if (top.y < 0 || right.x < 0 || first.y >= 2 * g_deep || left.x >= 2 * g_wide) return 0;
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Replay
//
// One quarter of the picture: the image, and the calls made again on it.
// ---------------------------------------------------------------------------------------------------------------
enum { kDs = 0, kTb = 0x10000, kLs = 0x20000, kBg = 0x30000, kCs = 0x40000, kImage = 0x50000 };
std::vector<u8> g_start;        // the memory at the scene's start: the data segment, the tables' segment
std::vector<u8> g_image;
u8 g_carry[5][0x80];            // [q]: what the tracers left over before quarter q (see kCarry); [4]: after the last

// what the tracers leave over from call to call in the data segment (their own variables: a later call reads some
// of them before it writes them): carried from a quarter to the next and back to the game's memory at the end,
// as the quarters' own passes would leave them
const u16 kCarry[][2] = { { XA(0x48B2, 0x5410), XA(0x48BA, 0x5418) }, { XA(0x243A, 0x2F72), XA(0x243B, 0x2F73) }, { XA(0x26DC, 0x3214), XA(0x26DD, 0x3215) }, { XA(0x245E, 0x2F96), XA(0x245F, 0x2F97) }, { XA(0x2466, 0x2F9E), XA(0x2468, 0x2FA0) }, { XA(0x2726, 0x325E), XA(0x274A, 0x3282) }, { XA(0x274C, 0x3284), XA(0x276A, 0x32AA) }, { XA(0x28B0, 0x33F5), XA(0x28B4, 0x33F9) }, { XA(0x28D2, 0x3417), XA(0x28D4, 0x3419) } };

// an edge's side of a window wide x deep, as transfm2_classifyedges decides it (1CCA:8116..8203): 04 its rows miss
// the window (or it is level), 02 its right end is at or left of x 0, 01 its left end is at or right of the
// width, 08 else (it crosses the window's columns)
u8 side_of(const Pt &a, const Pt &b, int wide, int deep) {
    const Pt &upper = b.y >= a.y ? a : b, &lower = b.y >= a.y ? b : a;
    if (a.y == b.y || lower.y < 0 || upper.y >= deep) return 4;
    const Pt &leftEnd = b.x >= a.x ? a : b, &rightEnd = b.x >= a.x ? b : a;
    if (rightEnd.x <= 0) return 2;
    if (leftEnd.x >= wide) return 1;
    return 8;
}

struct Replay {
    TraceMem m;
    const u8 *code;
    int offX, offY;         // this quarter's corner in the one pass's picture
    // the object being drawn
    bool objectOn;          // the game would draw it in this quarter
    u16 rec; int object, faces, edgeCount;
    int faceNext, faceNow;  // the next face's number, the number of the face drawn last
    bool faceOn;            // ... and whether it was drawn in this quarter
    bool marksOn; int markBase;
    bool markOpen; u16 markFlatKept;
    u16 boltFlatKept;
    std::vector<u8> result; // each face's result in this quarter (drawpol's DS:203A[])
    unsigned skippedFaces, skippedObjects;

    inline Pt moved(const Pt &p) const { Pt r; r.x = p.x - offX; r.y = p.y - offY; return r; }
    void put_point(u16 at, const Pt &p) { const Pt r = moved(p); m.D32(at, (u32)r.x); m.D32((u16)(at + 4), (u32)r.y); }

    // The object's first call.  The game draws an object when one of its faces at least "shows" (drawpol's
    // count DS:246C: a face whose winding was looked at and is not level, or a two-sided face), gives it the next
    // number and a record; so the same is decided here for the quarter's window.  A face's winding is looked
    // at (1CCA:8206) when some edge crosses the window's columns, or is behind the near plane or right of the
    // window while not all are right of it or outside its rows (transfm2_classifyedges' counters DS:28A8..AB),
    // or -- the object reaching behind the eye -- when a vertex of the face is behind.
    void object_info(const Event &ev) {
        const u8 *b = &g_cap.bytes[ev.bytes];
        const EdgeIn *edges = ev.edgeCount ? &g_cap.edges[ev.edges] : NULL;
        faces = ev.faces; edgeCount = ev.edgeCount;
        result.assign((size_t)faces, 0);
        std::vector<u8> side((size_t)edgeCount, 0);
        for (int e = 0; e < edgeCount; e++) side[e] = (edges[e].flags & 0x0F) ? side_of(moved(edges[e].pa), moved(edges[e].pb), g_wide, g_deep) : 0;
        bool shows = false;
        const u8 *f = b + 0x27;
        for (int i = 0; i < faces; i++) {
            const int n = f[0];
            const u8 header = f[1], live = f[2];
            const bool behind = f[3] != 0;
            if (n == 2) result[i] = live;       // (a line: its two points only)
            else {
                int crossing = 0, rightOrBehind = 0, rightOrOut = 0, left = 0;
                for (int k = 0; k < n; k++) {
                    const int e = f[4 + k];
                    const u8 s = e < edgeCount ? side[e] : 0;
                    if (!s) rightOrBehind++;
                    else if (s == 1) { rightOrBehind++; rightOrOut++; }
                    else if (s == 4) rightOrOut++;
                    else if (s == 2) left++;
                    else crossing++;
                }
                bool looked = rightOrOut != n && left != n && (crossing || rightOrBehind);
                if (!looked && ev.nearEye && behind) looked = true;
                result[i] = looked ? live : 0;
            }
            if ((result[i] & 4) || (header & 0x80)) shows = true;
            f += 4 + n;
        }
        const u16 size = (u16)(0x67 + 2 * faces + 4 * edgeCount);
        object = m.d16(kObject);
        rec = m.d16(kRecordTop);
        objectOn = shows && object < 0x7F && (u16)(rec + size) < XA(0x1F00, 0x3F00);
        if (!objectOn) { skippedObjects++; return; }
        memcpy(m.tb + rec, b, 0x27);
        m.D16((u16)(kObjects + 2 * object), rec);
        m.D16(kRecordTop, (u16)(rec + size));
        m.D16(kRecord, rec);
        m.D16(kEdges, (u16)edgeCount);
        m.D16(kFaces, (u16)faces);
        // the vertices' points and lights, the edges' flags: none yet
        memset(m.ds + kVertexPoint, 0, 0x100);
        memset(m.ds + kVertexLight, 0xFF, 0x100);
        memset(m.ds + kFlags, 0, (size_t)edgeCount);
        // its markings take their numbers when there is room for all of them (1CCA:12DE)
        markBase = m.d16(kMarkCount);
        marksOn = ev.marks > 0 && markBase + ev.marks < 0x7F;
        if (marksOn) m.D16(kMarkCount, (u16)(markBase + ev.marks));
        faceNext = 1;
        faceOn = false;
    }

    // the next face's number taken, its two bytes of the record written (drawpol, before the call)
    void begin_face(const Event &ev) {
        faceNow = faceNext++;
        faceOn = true;
        m.D16(kFace, (u16)faceNow);
        m.T8((u16)(rec + 2 * faceNow + 0x65), ev.colour);
        m.T8((u16)(rec + 2 * faceNow + 0x66), ev.fileIndex);
    }

    // DS:26DC for the face about to be traced: the game's own pass of the whole picture had `ev.shadeMode` there.
    // That is this quarter's too when the face is not shaded (drawpol has just written it), or when it is not what
    // the face before had (something outside the tracers wrote it); else this pass's own is kept (a cut face's 7).
    int lastShade;
    void shade(const Event &ev) {
        if (!ev.gouraud || (int)ev.shadeMode != lastShade) m.D8(kShadeMode, ev.shadeMode);
        lastShade = ev.shadeMode;
    }

    void face(const Event &ev) {
        if (!objectOn) return;
        // (the BSP walk draws a face in front only when its winding was looked at: 1CCA:18DD)
        if (ev.bsp && ev.fileIndex < result.size() && !result[ev.fileIndex]) { faceOn = false; skippedFaces++; return; }
        begin_face(ev);
        // (the face's shading, drawpol's 1CCA:1B98: DS:243A written for every face; DS:26DC only for a face not
        // shaded -- a shaded face leaves it as the last flat face drawn in THIS pass, or a cut face's 7, left it)
        m.D8(kGouraud, ev.gouraud);
        shade(ev);
        const u8 *body = &g_cap.bytes[ev.bytes];
        // the vertices: their points; their lights as drawpol_getlightvalue leaves them (worked out for a shaded
        // face's vertices that have none yet, again every time for a two-sided face)
        const VertIn *v = &g_cap.verts[ev.verts];
        for (int k = 0; k <= ev.n; k++) {
            m.D16((u16)(kVertexPoint + 2 * v[k].v), v[k].at);
            if (v[k].at) put_point(v[k].at, v[k].p);
            const u16 lightAt = (u16)(kVertexLight + 2 * v[k].v);
            if (ev.gouraud && ((ev.header & 0x80) || m.d16(lightAt) == 0xFFFF)) m.D16(lightAt, v[k].light);
        }
        // the edges not traced yet: their points, lengths and signs; their side of this quarter's window
        const EdgeIn *e = &g_cap.edges[ev.edges];
        for (int k = 0; k < ev.n; k++) {
            const int i = e[k].e;
            const u8 has = m.d8((u16)(kFlags + i));
            if (has & 0x20) continue;
            if (!(e[k].flags & 0x0F)) { m.D8((u16)(kFlags + i), 0x80); continue; }
            m.D16((u16)(kPointA + 2 * i), e[k].a);
            m.D16((u16)(kPointB + 2 * i), e[k].b);
            put_point(e[k].a, e[k].pa);
            put_point(e[k].b, e[k].pb);
            m.D32((u16)(kAbsDx + 4 * i), e[k].dx);
            m.D32((u16)(kAbsDy + 4 * i), e[k].dy);
            m.D8((u16)(kSignDx + i), e[k].signDx);
            m.D8((u16)(kSignDy + i), e[k].signDy);
            // (the slope and "steep" may have been worked out by the winding's test already: 1CCA:82E7..)
            if ((e[k].flags & 0x40) && !(has & 0x40)) {
                m.D16((u16)(kSlopeFrac + 2 * i), e[k].slope[0]);
                m.D16((u16)(kSlopeLo + 2 * i), e[k].slope[1]);
                m.D16((u16)(kSlopeHi + 2 * i), e[k].slope[2]);
            }
            m.D8((u16)(kFlags + i), (u8)(side_of(moved(e[k].pa), moved(e[k].pb), g_wide, g_deep) | ((has | e[k].flags) & 0xC0)));
        }
        tie640_trace_drawface(m, body, ev.cx);
    }

    void line(const Event &ev) {
        const Pt *pts = &g_cap.points[ev.points];
        if (ev.context == kOfFace) {
            // a face of two vertices (drawpol_drawlineface 1CCA:20D0): the line is the object's edge, the face's
            // number goes into the edge's slot, the flat objects' counter is put back after
            if (!objectOn) return;
            begin_face(ev);
            m.D8(kGouraud, ev.gouraud);
            shade(ev);
            const u8 edge = (u8)ev.id;
            m.D8(kId, edge);
            m.D8(kId + 1, (u8)object);
            const u16 slot = (u16)(rec + 0x67 + 2 * faces + 4 * edge);
            m.T16(slot, (u16)faceNow);
            set_line(ev, pts);
            const u16 kept = m.d16(kFlatNext);
            tie640_trace_thick_line(m, ev.q, ev.strayCx, NULL);
            m.D16(kFlatNext, kept);
            m.T16((u16)(slot + 2), (u16)(m.d16(kAcrossStep) >> 1));
        } else if (ev.context == kOfMarking) {
            if (!markOpen) return;
            m.D16(kFlatNext, ev.flatNext);
            m.D8(kId, (u8)(markBase + ev.mark));
            m.D8(kId + 1, (u8)(ev.id >> 8));
            set_line(ev, pts);
            tie640_trace_thick_line(m, ev.q, ev.strayCx, NULL);
        } else {
            // a line object's line (1CCA:0D9A): the flat objects' tables written at the counter's place first
            const u16 n = m.d16(kFlatNext);
            m.D8((u16)(kFlatColour + n), ev.flatColour);
            m.D8((u16)(kFlatComponent + n), m.d8(kObject));
            m.D16((u16)(kFlatParent + 2 * n), ev.flatParent);
            m.D16((u16)(kFlatX + 2 * n), ev.flatX);
            m.D16((u16)(kFlatY + 2 * n), ev.flatY);
            m.D16((u16)(kFlatZ + 2 * n), ev.flatZ);
            m.D8(kId, (u8)ev.id);
            m.D8(kId + 1, (u8)(0x80 + n));
            set_line(ev, pts);
            tie640_trace_thick_line(m, ev.q, ev.strayCx, NULL);
        }
    }
    void set_line(const Event &ev, const Pt *pts) {
        m.D16(kLineP, ev.p);
        put_point(ev.p, pts[0]);
        put_point(ev.q, pts[1]);
        m.D16(kThick, ev.thick);
        if (ev.lightsNew) { m.D16(kLightP, ev.lightP); m.D16(kLightQ, ev.lightQ); }
    }

    void flat(const Event &ev) {
        if (ev.context == kOfMarking) {
            if (!markOpen) return;
            m.D16(kFlatNext, ev.flatNext);
            m.D16(kIdLow, (u16)(markBase + ev.mark));
        } else m.D16(kIdLow, ev.idLow);
        m.D16(kRingFirst, ev.ringFirst); m.D16(kRingEnd, ev.ringEnd);
        m.D16(kMinX, ev.minX); m.D16(kMaxX, ev.maxX); m.D16(kMinY, ev.minY); m.D16(kMaxY, ev.maxY);
        m.D8(kSameRow, ev.sameRow); m.D8(kSameColumn, ev.sameColumn);
        m.D16(kPoints, ev.pointCount);
        m.D8(kColour, ev.polyColour);
        m.D16(kParent, ev.parent);
        const Pt *pts = &g_cap.points[ev.points];
        for (int i = 0; i < ev.n; i++) put_point((u16)(ev.ringFirst + 8 * i), pts[i]);
        tie640_trace_flat_polygon(m, ev.strayCx, ev.strayAx, NULL);
    }

    // A sprite's rows: what the game's row pass was given in this quarter, the buffer as its scaler left it, and
    // the hand-written row pass on them
    int quarter;
    u8 *bg;
    void sprite(const Event &ev) {
        const SpriteIn &in = g_sprites[(size_t)ev.sprite];
        if (!in.has[quarter]) return;
        if (!in.stale[quarter]) memcpy(m.ds + kBox, in.box[quarter], 8);
        m.D32(kSpriteX, in.at[quarter][0]); m.D32(kSpriteY, in.at[quarter][1]); m.D32(kSpriteZ, in.at[quarter][2]);
        m.D16(kParent, in.parent[quarter]);
        m.D16(kIdLow, in.idLow[quarter]);
        memcpy(m.ds + kSpriteMap, in.map[quarter], 0x40);
        if (in.pixels[quarter]) memcpy(bg, &g_snaps[in.snap[quarter]], kBgBytes);
        tie640_trace_sprite_rows(m, bg, NULL);
    }

    void run(const std::vector<Event> &events) {
        objectOn = faceOn = marksOn = markOpen = false;
        skippedFaces = skippedObjects = 0;
        for (size_t i = 0; i < events.size(); i++) {
            const Event &ev = events[i];
            switch (ev.kind) {
            case kEnter:
                objectOn = faceOn = marksOn = markOpen = false;
                m.D16(kSpareEdge, 0);
                if (ev.fromBolt) boltFlatKept = m.d16(kFlatNext);
                break;
            case kObjInfo: object_info(ev); break;
            case kFaceCall: face(ev); break;
            case kLineCall: line(ev); break;
            case kFlatCall: flat(ev); break;
            case kSpriteCall: sprite(ev); break;
            case kMarkBegin:
                // a face's markings (drawpol_drawmarkings 1CCA:1CFC): the flat objects' counter kept, the
                // marking's polygons and lines numbered down from 7F
                markOpen = objectOn && faceOn && marksOn;
                if (markOpen) markFlatKept = m.d16(kFlatNext);
                break;
            case kMarkEnd:
                if (!markOpen) break;
                if (ev.n) {
                    // the marking's record (1CCA:1F9C..2060): {object, face}, its colours (eight bytes' room) worked
                    // out from the craft's own by the shading this quarter's pass has at this moment (DS:26DC, the
                    // last flat face's or a cut face's 7; DS:243A), eight bytes of nothing
                    const u8 *raw = &g_cap.bytes[ev.bytes];
                    const bool same = raw[0] != 0;
                    const int n = raw[1];
                    const u8 shade = m.d8(kShadeMode), gouraud = m.d8(kGouraud);
                    const u16 at = m.d16(kRecordTop);
                    m.T8(at, (u8)object);
                    m.T8((u16)(at + 1), (u8)faceNow);
                    for (int k = 0; k < n; k++) {
                        const u8 c = raw[2 + k];
                        u8 out;
                        if (same || !gouraud) {
                            const u8 index = same ? raw[10 + (c & 0x1F)] : (u8)(c & 0x1F);
                            const u16 where = (u16)((u16)(index << 4) - shade);
                            out = (u8)((u8)(0 - (c >> 5)) + code[(u16)(where + 0x79F)]);
                        } else {
                            out = (u8)(c + raw[42 + (c & 0x1F)]);
                        }
                        m.T8((u16)(at + 2 + k), out);
                    }
                    for (int k = 0; k < 8; k++) m.T8((u16)(at + 10 + k), 0);
                    m.D16((u16)(kMarkPtr + 2 * (markBase + ev.mark)), at);
                    m.D16(kRecordTop, (u16)(at + 18));
                }
                m.D16(kFlatNext, markFlatKept);
                markOpen = false;
                break;
            case kLeave:
                if (ev.fromBolt) m.D16(kFlatNext, (u16)(boltFlatKept + 1));
                if (objectOn && ev.committed) {
                    // (the object's end, 1CCA:1427: an object of 128 edges and more takes two numbers)
                    m.D16(kObject, (u16)(object + 1));
                    if (edgeCount >= 0x80) { m.D16(kObject, (u16)(object + 2)); m.D16((u16)(kObjects + 2 * (object + 1)), rec); }
                }
                objectOn = false;
                break;
            default: break;
            }
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------
// The depth sort's answers (the game's draw_polydepthsort, 168C:0798).  The scanline pass asks it which of two
// objects whose boxes overlap is in front; the passes run on images, so a pass that meets a question not yet
// answered stops, the game's routine is run by the interpreter from the hook's return, and the pass is made again.
// ---------------------------------------------------------------------------------------------------------------
struct Question { u16 a[8]; bool operator<(const Question &o) const { return memcmp(a, o.a, sizeof(a)) < 0; } };
std::map<Question, int> g_answers;
int oracle(const u16 *args) {
    Question q;
    memcpy(q.a, args, sizeof(q.a));
    const std::map<Question, int>::const_iterator i = g_answers.find(q);
    return i == g_answers.end() ? -1 : i->second;
}

// The one run's sprite (g_smallBg as the scaler left it, g_smallIn what its row pass was given) into the four
// quarters: each quarter's buffer given the sprite's box twice the size (a pixel of the small buffer four of the
// 640x378), the quarter's own box worked out, the buffer kept for the replay's row pass, and the hand-written row
// pass made on the quarter's buffer (it maps the sprite's pixels to their colours, as it does in the small one).
void small_to_quarters(const Live &L, Bit16u loadSeg) {
    SpriteIn &in = g_sprites[(size_t)g_run.sprite];
    const u8 *cs = L.M + ((PhysPt)(u16)(loadSeg + kRendPara) << 4);
    const int last = g_deep - 1;
    const Bit16s minX = (Bit16s)TraceMem::get16(g_smallIn.box), maxY = (Bit16s)TraceMem::get16(g_smallIn.box + 2),
                 maxX = (Bit16s)TraceMem::get16(g_smallIn.box + 4), minY = (Bit16s)TraceMem::get16(g_smallIn.box + 6);
    if (g_scratch.empty()) g_scratch.resize(0x20000);
    for (int q = 0; q < 4; q++) {
        in.has[q] = true;
        in.stale[q] = g_smallIn.stale;
        in.pixels[q] = false;
        in.at[q][0] = g_smallIn.at[0]; in.at[q][1] = g_smallIn.at[1]; in.at[q][2] = g_smallIn.at[2];
        in.parent[q] = g_smallIn.parent; in.idLow[q] = g_smallIn.idLow;
        memcpy(in.map[q], g_smallIn.map, 0x40);
        memcpy(g_quarterScaler[q], L.M + L.ds + XA(0x430C, 0x4E51), 0x80);
        if (g_smallIn.stale) { memcpy(in.box[q], kBoxUnset, 8); continue; }
        const int offX = (q & 1) * g_wide, offY = (q >> 1) * g_deep;
        // (the box: x from the left; y UPWARDS from the window's bottom row)
        const int bx0 = 2 * minX - offX, bx1 = 2 * maxX + 1 - offX, by1 = 2 * maxY - g_deep + 1 + offY, by0 = 2 * minY - g_deep + offY;
        const int clamp0 = -0x7000, clamp1 = 0x7000;
        const Bit16s b[4] = { (Bit16s)std::max(clamp0, std::min(clamp1, bx0)), (Bit16s)std::max(clamp0, std::min(clamp1, by1)),
                              (Bit16s)std::max(clamp0, std::min(clamp1, bx1)), (Bit16s)std::max(clamp0, std::min(clamp1, by0)) };
        for (int i = 0; i < 4; i++) { in.box[q][2 * i] = (u8)(b[i] & 0xFF); in.box[q][2 * i + 1] = (u8)((u16)b[i] >> 8); }
        // (the 64 bytes taken as the sprite's colour map run over the box itself, DS:48A2 + 0x10: this quarter's there)
        memcpy(in.map[q] + (kBox - kSpriteMap), in.box[q], 8);
        const int left = b[0] - 2, top = b[1] + 2, right = b[2] + 2, bottom = b[3] - 2;
        in.pixels[q] = !(left >= g_wide || right < 0 || last - top >= g_deep || last - bottom < 0);
        if (!in.pixels[q]) continue;
        // the quarter's buffer: the box's part of it, from the small buffer
        u8 *to = &g_quarterBg[q][0];
        const u8 *from = &g_smallBg[0];
        const int x0 = std::max(0, left), x1 = std::min(g_wide - 1, right), r0 = std::max(0, last - top), r1 = std::min(last, last - bottom);
        for (int r = r0; r <= r1; r++) {
            const u16 rowTo = TraceMem::get16(cs + (u16)(kRowOffsetsCs + 2 * r)), rowFrom = TraceMem::get16(cs + (u16)(kRowOffsetsCs + 2 * ((r + offY) >> 1)));
            for (int x = x0; x <= x1; x++) to[(u16)(rowTo + x)] = from[(u16)(rowFrom + ((x + offX) >> 1))];
        }
        in.snap[q] = g_snaps.size();
        g_snaps.resize(g_snaps.size() + kBgBytes);
        memcpy(&g_snaps[in.snap[q]], to, kBgBytes);
        // the row pass on it (the lists thrown away): the sprite's pixels mapped; the quarter's box as it leaves it
        memcpy(&g_scratch[0], L.M + L.ds, 0x10000);
        TraceMem t;
        t.ds = &g_scratch[0]; t.tb = &g_scratch[0x10000]; t.cs = cs;
        memcpy(t.ds + kBox, in.box[q], 8);
        t.D16(kFlatNext, TraceMem::get16(&g_start[kFlatNext]));
        t.D16(kPool, TraceMem::get16(&g_start[kPool]));
        memset(t.ds + kRows, 0, 2 * 200);
        tie640_trace_sprite_rows(t, to, NULL);
        memcpy(g_quarterBox[q], t.ds + kBox, 8);
        g_quarterBoxKnown[q] = true;
    }
    // ... and the small buffer's own row pass: its sprite's pixels mapped too (the next sprite is drawn over them)
    if (!g_smallIn.stale) {
        memcpy(&g_scratch[0], L.M + L.ds, 0x10000);
        TraceMem t;
        t.ds = &g_scratch[0]; t.tb = &g_scratch[0x10000]; t.cs = cs;
        memcpy(t.ds + kBox, g_smallIn.box, 8);
        t.D16(kFlatNext, TraceMem::get16(&g_start[kFlatNext]));
        t.D16(kPool, TraceMem::get16(&g_start[kPool]));
        memset(t.ds + kRows, 0, 2 * 200);
        tie640_trace_sprite_rows(t, &g_smallBg[0], NULL);
    }
}

// the scaler has drawn the sprite in quarter g_run.q: that quarter's buffer, the scaler's variables and box kept
void quarter_drawn(const Live &L, PhysPt buffer) {
    bg_read(buffer, &g_quarterBg[g_run.q][0], kBgBytes);
    memcpy(g_quarterScaler[g_run.q], L.M + L.ds + XA(0x430C, 0x4E51), 0x80);
    // (the box as this quarter's pass has it now -- unless the row pass ran on nothing known)
    if (!(g_runStale && !g_quarterBoxKnown[g_run.q])) {
        for (int i = 0; i < 8; i++) g_quarterBox[g_run.q][i] = L.d8((u16)(kBox + i));
        g_quarterBoxKnown[g_run.q] = true;
    }
}

unsigned g_frames = 0, g_events = 0, g_faces = 0, g_lines = 0, g_flats = 0, g_objects = 0, g_sorts = 0, g_skippedFaces = 0, g_skippedObjects = 0;
int g_quarter = 0;              // the quarter to be resolved next
Question g_pending;
bool g_awaiting = false;

// one quarter: the image made, the calls replayed, the scanline pass.  False: the pass stopped at a question.
bool resolve_quarter(int q, Bit8u *draw, int pitch, int limitX, int limitY, int corner, Bit16u *asked) {
    const Bit8u *M = GetMemBase();
    const Bit8u *code = M + ((PhysPt)(u16)(g_load + kRendPara) << 4);
    // the image: the scene's start; what the tracers left over, from the quarter before
    size_t n = 0;
    memcpy(&g_image[kDs], &g_start[0], 0x10000);
    // (the scanline pass reads a flat object's component number 80h too far, the game's own bug at 1CCA:9DFD: in the
    // sprite scaler's variables, DS:430C on, which hold what the scaler left when it last ran in THIS quarter's
    // pass -- not what the scene began with)
    memcpy(&g_image[kDs + XA(0x430C, 0x4E51)], g_quarterScaler[q], 0x80);
    memcpy(&g_image[kDs + XA(0x430C, 0x4E51)], M + ((PhysPt)(u16)(g_load + kDataPara) << 4) + XA(0x430C, 0x4E51), XA(0x14, 0x2D));     // (... the bytes before them: as they are now)
    memcpy(&g_image[kTb], &g_start[0x10000], 0x10000);
    memcpy(&g_image[kBg], &g_start[0x20000], 0x10000);
    memcpy(&g_image[kCs + 0x700], code + 0x700, 0x500);
    if (q > 0) for (size_t r = 0; r < sizeof(kCarry) / sizeof(kCarry[0]); r++) { memcpy(&g_image[kCarry[r][0]], g_carry[q] + n, (size_t)(kCarry[r][1] - kCarry[r][0])); n += kCarry[r][1] - kCarry[r][0]; }
    Replay r;
    r.m.ds = &g_image[kDs]; r.m.tb = &g_image[kTb]; r.m.cs = code;
    r.code = code;
    r.offX = (q & 1) * g_wide; r.offY = (q >> 1) * g_deep;
    r.quarter = q; r.bg = &g_image[kBg];
    r.lastShade = g_start[kShadeMode];
    r.run(g_cap.events);
    // (the tracers' leftovers kept for the next quarter before the scanline pass uses the same memory)
    n = 0;
    for (size_t k = 0; k < sizeof(kCarry) / sizeof(kCarry[0]); k++) { memcpy(g_carry[q + 1] + n, &g_image[kCarry[k][0]], (size_t)(kCarry[k][1] - kCarry[k][0])); n += kCarry[k][1] - kCarry[k][0]; }
    if (!tie640_native_image(&g_image[0], draw, pitch, limitX, limitY, 2 * (corner % 320) + r.offX, 2 * (corner / 320) + r.offY, q, oracle, asked)) return false;
    g_skippedFaces += r.skippedFaces; g_skippedObjects += r.skippedObjects;
    return true;
}

}  // namespace

// WCNET_T640_NATIVE=2: the scene in one pass.  WCNET_T640_CHECK=3 with it: each frame in quarters first, compared.
bool tie640_onepass_on() {
    if (g_on < 0) {
        const char *e = getenv("WCNET_T640_NATIVE");
        g_on = (e && e[0]) ? (e[0] == '2' ? 1 : 0) : XA(1, 0);      // (the default; X-Wing: off until proved)
        e = getenv("WCNET_T640_CHECK");
        g_check = g_on && e && e[0] == '3' ? 1 : 0;
        e = getenv("WCNET_T640_NOSPRITES");
        g_noSprites = e && e[0] == '1';
        e = getenv("WCNET_T640_SPRNEST");
        g_nested = !(e && e[0] == '0');
        e = getenv("WCNET_T640_SPRMAP");
        g_nativeMap = !(e && e[0] == '0');
        e = getenv("WCNET_T640_SPR1X");
        g_spr1x = !(e && e[0] == '0');
        e = getenv("WCNET_T640_SPRCULL");
        g_cullSprites = e && e[0] == '1';
        if (g_on) wclog(1, T640_GAME " 640: the scene in one pass (WCNET_T640_NATIVE=2)%s%s", g_check ? ", every frame checked against the quarters" : "", g_noSprites ? ", no sprites" : "");
    }
    return g_on == 1;
}
bool tie640_onepass_sprite_running() { return g_run.active; }
bool tie640_onepass_check() { return tie640_onepass_on() && g_check == 1; }
bool tie640_onepass_no_sprites() { tie640_onepass_on(); return g_noSprites; }

void tie640_onepass_places(void (*place)(Bit16u para, Bit16u ip)) {
    static const u16 ips[] = { kDrawObject, kDrawObjectOut, kTraceFace, kTraceFlat, kTraceLine, kMarkings, kMarkingsOut, XA(0x1FB0, 0x2124), kSprite, kSpriteRows, kScanlines };
    for (size_t i = 0; i < sizeof(ips) / sizeof(ips[0]); i++) place(kRendPara, ips[i]);
}

// The scene section has set the renderer up for the game's own window (0090:0B36), which is about to be made
// 640x378 for the pass: the memory every quarter's image starts from is kept.
void tie640_onepass_begin(Bit16u loadSeg) {
    g_load = loadSeg;
    Live L;
    L.at(loadSeg);
    if (g_start.empty()) { g_start.resize(0x30000); g_image.assign(kImage, 0); }
    memcpy(&g_start[0], L.M + L.ds, 0x10000);
    memcpy(&g_start[0x10000], L.M + L.tb, 0x10000);
    bg_read((PhysPt)L.d16(kBufferSeg) << 4, &g_start[0x20000], 0x10000);
    g_wide = L.d16(kWide); g_deep = L.d16(kDeep);
    g_sprites.clear(); g_snaps.clear();
    for (int q = 0; q < 4; q++) g_quarterBg[q].assign(g_start.begin() + 0x20000, g_start.begin() + 0x30000);
    g_smallBg.assign(g_start.begin() + 0x20000, g_start.begin() + 0x30000);
    g_small = false;
    g_run.active = false; g_backdropNext = false;
    for (int q = 0; q < 4; q++) g_quarterBoxKnown[q] = false;
    for (int q = 0; q < 4; q++) memcpy(g_quarterScaler[q], &g_start[XA(0x430C, 0x4E51)], 0x80);
    g_cap.clear();
    g_cap.lightP = L.d16(kLightP); g_cap.lightQ = L.d16(kLightQ);
    g_answers.clear();
    g_quarter = 0;
    g_awaiting = false;
}

// A watched place of the renderer's while the one pass runs.  `draw`: the 640 picture; `corner`: the window's
// corner in the game's picture.  True when the place was dealt with.
static bool onepass_hook_inner(Bit16u loadSeg, Bit32u ip, Bit8u *draw, int pitch, int limitX, int limitY, int corner);
bool tie640_onepass_hook(Bit16u loadSeg, Bit32u ip, Bit8u *draw, int pitch, int limitX, int limitY, int corner) {
    const double t0 = perf_now_ms();
    const bool r = onepass_hook_inner(loadSeg, ip, draw, pitch, limitX, limitY, corner);
    g_costHookMs += perf_now_ms() - t0;
    g_costHooks++;
    return r;
}
// (the one pass's cost since the last asking, for the wall clock's line)
void tie640_onepass_cost(char *to, size_t n) {
    snprintf(to, n, "the one pass's hooks %.0f ms in %u calls, of that the sprites' scaler %.0f ms in %u runs (%u slices)", g_costHookMs, g_costHooks, g_costNestedMs, g_costSprites, g_nestedSlices);
    g_costHookMs = g_costNestedMs = 0; g_costHooks = g_costSprites = 0; g_nestedSlices = 0;
}
static bool onepass_hook_inner(Bit16u loadSeg, Bit32u ip, Bit8u *draw, int pitch, int limitX, int limitY, int corner) {
    Live L;
    L.at(loadSeg);
    switch (ip) {
    case XA(0x0775, 0x0775): g_backdropNext = true; return false;      // (the backdrop's place: the mode's own hook doubles it)
    case XA(0xDF00, 0xE190): g_backdropNext = false; return false;
    case kDrawObject: {
        Event &ev = add(kEnter);
        ev.fromBolt = return_ip() == XA(0x070C, 0x0770) && return_cs() == (u16)(loadSeg + kDrawPara);
        g_cap.fromBolt = ev.fromBolt;
        g_cap.infoDone = false;
        return true;
    }
    case kDrawObjectOut: {
        Event &ev = add(kLeave);
        ev.fromBolt = g_cap.fromBolt;
        ev.committed = L.d16(kObject) > 1;
        if (ev.committed) g_objects++;
        // (the game's own pass is left with room for the next object whatever the picture holds: a quarter's
        // image counts its own objects, records and markings)
        L.D16(kObject, 1);
        L.D16(kRecordTop, 0);
        L.D16(kMarkCount, 0);
        return true;
    }
    case kTraceFace:
        capture_face(L);
        g_faces++;
        far_return();
        return true;
    case kTraceLine:
        capture_line(L);
        g_lines++;
        far_return();
        return true;
    case kTraceFlat:
        reg_ax = (u16)capture_flat(L);
        g_flats++;
        far_return();
        return true;
    case kMarkings: {
        Event &ev = add(kMarkBegin);
        ev.mark = L.d8((u16)(kMarkOf + reg_si));
        g_cap.markTop = L.d16(kRecordTop);
        g_cap.markHas = false;
        return true;
    }
    case XA(0x1FB0, 0x2124): {
        // the marking's record about to be written: its colours as the craft's data has them (ES:SI: a count, the
        // bytes), and whether the craft is the one named in DS:C450 (its colours are then taken another way)
        const PhysPt src = SegPhys(es) + reg_si;
        const int n = mem_readb(src);
        g_cap.markRaw[0] = (u8)(n > 8 ? 8 : n);
        for (int i = 0; i < g_cap.markRaw[0]; i++) g_cap.markRaw[1 + i] = mem_readb(src + 1 + (PhysPt)i);
        g_cap.markSame = L.d16(XA(0xC450, 0xC71E)) == L.d16(kParent);
        // (the two colour tables in the renderer's code segment are the craft's own, written before it is drawn)
        for (int i = 0; i < 32; i++) { g_cap.markTables[i] = mem_readb(SegPhys(cs) + 0x94F + (PhysPt)i); g_cap.markTables[32 + i] = mem_readb(SegPhys(cs) + 0x99B + (PhysPt)i); }
        g_cap.markHas = true;
        return true;
    }
    case kMarkingsOut: {
        Event &ev = add(kMarkEnd);
        // (the marking's number: its kMarkBegin's)
        for (size_t i = g_cap.events.size() - 1; i-- > 0;) if (g_cap.events[i].kind == kMarkBegin) { ev.mark = g_cap.events[i].mark; break; }
        const u16 top = L.d16(kRecordTop);
        if (top > g_cap.markTop && top - g_cap.markTop < 0x100 && g_cap.markHas) {
            ev.n = top - g_cap.markTop;
            g_cap.bytes.push_back(g_cap.markSame ? 1 : 0);
            for (int i = 0; i < 9; i++) g_cap.bytes.push_back(g_cap.markRaw[i]);
            for (int i = 0; i < 64; i++) g_cap.bytes.push_back(g_cap.markTables[i]);
            g_cap.markHas = false;
        }
        return true;
    }
    case kSprite: {
        // rotscale_rotatescaleimage(x, y, scale, far image): the game's own scaler run once for each quarter, the
        // sprite's place moved by the quarter's corner, into that quarter's copy of the buffer.  Its far return is
        // made to come back HERE between the quarters.
        // (the interpreter started on the scaler from inside this hook comes by here first)
        if (g_skipEntry) { g_skipEntry = false; return true; }
        if (g_noSprites) { far_return(); return true; }
        const PhysPt buffer = (PhysPt)L.d16(kBufferSeg) << 4;
        const PhysPt stack = SegPhys(ss);
        if (!g_run.active) {
            g_run.active = true;
            g_run.q = 0;
            g_run.backdrop = g_backdropNext;
            g_backdropNext = false;
            g_run.retIp = CPU_Pop16();
            g_run.retCs = CPU_Pop16();
            g_run.x = mem_readw(stack + (u16)reg_sp);
            g_run.y = mem_readw(stack + (u16)(reg_sp + 2));
            g_run.sprite = (int)g_sprites.size();
            SpriteIn in;
            memset(&in, 0, sizeof(in));
            g_sprites.push_back(in);
            Event &ev = add(kSpriteCall);
            ev.sprite = g_run.sprite;
            if (g_spr1x && g_nested && buffer + kBgBytes <= 0xA0000) {
                // One run at the game's own scale: the place and the scale halved (the caller's are the 640x378's).
                const int base = g_run.backdrop ? 0 : -2 * g_deep + 2 * (g_deep >> 1);
                const u16 scale = mem_readw(stack + (u16)(reg_sp + 4));
                mem_writew(stack + (u16)reg_sp, (u16)((Bit16s)g_run.x >> 1));
                // (y runs UPWARDS from the window's bottom row: the caller's + base is the upper quarters' own, the
                // 640x378's a window's depth more)
                mem_writew(stack + (u16)(reg_sp + 2), (u16)((Bit16s)(u16)(g_run.y + base + g_deep) >> 1));
                mem_writew(stack + (u16)(reg_sp + 4), (u16)(scale >> 1));
                const u16 stubSeg = (u16)(loadSeg - 0x10);
                CPU_Push16(stubSeg);
                CPU_Push16(kIdleAt);
                g_spritesRun++;
                bg_write(buffer, &g_smallBg[0], kBgBytes);
                for (int i = 0; i < 8; i++) mem_writeb(L.ds + kBox + i, kBoxUnset[i]);
                memset(&g_smallIn, 0, sizeof(g_smallIn));
                g_smallIn.stale = true;
                g_small = true;
                SegSet16(cs, (u16)(loadSeg + kRendPara));
                reg_eip = kSprite;
                g_skipEntry = true;
                const double t0 = perf_now_ms();
                const bool ok = run_to_idle(stubSeg);
                g_costNestedMs += perf_now_ms() - t0; g_costSprites++;
                g_small = false;
                if (!ok) { g_nested = false; return true; }
                bg_read(buffer, &g_smallBg[0], kBgBytes);
                mem_writew(stack + (u16)(reg_sp + 4), scale);
                small_to_quarters(L, loadSeg);
                g_run.q = 4;
            }
        } else {
            quarter_drawn(L, buffer);
            g_run.q++;
        }
        for (; g_run.q < 4; g_run.q++) {
            // (the place: x from the window's left; y UPWARDS from its bottom row, worked out for the 640x378
            // window by the caller: 2 * (depth >> 1) - the screen's row)
            const int offX = (g_run.q & 1) * g_wide, offY = (g_run.q >> 1) * g_deep;
            // A queued sprite's y was worked out while the window was 640x378 (1768:022D, 168C:05EA: twice the
            // half depth less the row); the backdrop's with the game's own window (1CCA:0775 on).
            const int base = g_run.backdrop ? 0 : -2 * g_deep + 2 * (g_deep >> 1);
            mem_writew(stack + (u16)reg_sp, (u16)(g_run.x - offX));
            mem_writew(stack + (u16)(reg_sp + 2), (u16)(g_run.y + base + offY));
            const u16 stubSeg = (u16)(loadSeg - 0x10);
            CPU_Push16(g_nested ? stubSeg : (u16)(loadSeg + kRendPara));
            CPU_Push16(g_nested ? kIdleAt : kSprite);
            // (a quarter no corner of the picture can reach: the scaler would draw nothing and add nothing)
            if (g_cullSprites && tie640_fast_sprite_off()) { reg_sp = (u16)(reg_sp + 4); g_spritesOff++; continue; }
            g_spritesRun++;
            bg_write(buffer, &g_quarterBg[g_run.q][0], kBgBytes);
            for (int i = 0; i < 8; i++) mem_writeb(L.ds + kBox + i, kBoxUnset[i]);
            g_runStale = false;
            if (!g_nested) return true;        // (on with the routine: its return comes back here)
            // The scaler run to its end here and now, by the emulator's own interpreter, outside the emulated
            // clock: the game's code as it is, at none of the frame's cycles (under an explosion it was most of
            // them).  Its return lands on two bytes of `jmp $` in the PSP, where the run is ended.
            SegSet16(cs, (u16)(loadSeg + kRendPara));      // (the quarter before left the CPU at the idle place)
            reg_eip = kSprite;
            g_skipEntry = true;
            { const double t0 = perf_now_ms(); const bool ok = run_to_idle(stubSeg); g_costNestedMs += perf_now_ms() - t0; g_costSprites++;
              if (!ok) { g_nested = false; return true; } }   // (it did not come back: the old way from here on)
            quarter_drawn(L, buffer);
        }
        g_run.active = false;
        mem_writew(stack + (u16)reg_sp, g_run.x);
        mem_writew(stack + (u16)(reg_sp + 2), g_run.y);
        SegSet16(cs, g_run.retCs);
        reg_eip = g_run.retIp;
        return true;
    }
    case kSpriteRows: {
        // the scaler's row pass in the quarter being drawn: what it is given taken down; the game's own then runs
        // (it maps the sprite's pixels in the buffer; its lists are of no use and start from nothing each time)
        if (!g_run.active) return false;
        if (g_small) {
            // (the one run's: what the row pass is given taken down, the row pass itself left: small_to_quarters)
            for (int i = 0; i < 8; i++) g_smallIn.box[i] = L.d8((u16)(kBox + i));
            g_smallIn.stale = memcmp(g_smallIn.box, kBoxUnset, 8) == 0;
            g_smallIn.at[0] = L.d32(kSpriteX); g_smallIn.at[1] = L.d32(kSpriteY); g_smallIn.at[2] = L.d32(kSpriteZ);
            g_smallIn.parent = L.d16(kParent); g_smallIn.idLow = L.d16(kIdLow);
            for (int i = 0; i < 0x40; i++) g_smallIn.map[i] = L.d8((u16)(kSpriteMap + i));
            reg_eip = CPU_Pop16();      // (a near call)
            return true;
        }
        SpriteIn &in = g_sprites[(size_t)g_run.sprite];
        const int q = g_run.q;
        in.has[q] = true;
        for (int i = 0; i < 8; i++) in.box[q][i] = L.d8((u16)(kBox + i));
        if (g_check == 1) { SprSig g; memset(&g, 0, sizeof(g)); g.w[0] = L.d16(XA(0x434A, 0x4EA8)); g.w[1] = L.d16(XA(0x434C, 0x4EAA)); g.w[2] = L.d16(XA(0x5112, 0x5C6A)); g.w[3] = L.d16(XA(0x4360, 0x4EBE));
            for (int i = 0; i < 4; i++) g.w[5 + i] = L.d16((u16)(kBox + 2 * i)); g.w[9] = L.d16(XA(0x4320, 0x4E7E)); g.w[10] = L.d16(XA(0x435C, 0x4EBA)); g.w[11] = L.d16(XA(0x435E, 0x4EBC)); g.w[12] = L.d16(XA(0x50BC, 0x5C14)); g.w[13] = L.d16(XA(0x50D2, 0x5C2A)); g.w[14] = L.d16(XA(0x50D4, 0x5C2C)); g_sprOne[q].push_back(g); }
        in.stale[q] = g_runStale = memcmp(in.box[q], kBoxUnset, 8) == 0;
        if (g_runStale) {
            // the scaler drew nothing: the row pass works on the box as it was left in this quarter (by the last
            // sprite's row pass there; the replay has it).  The game's own is run on that when it is known here,
            // else kept from doing anything (the buffer then holds no sprite yet to map)
            g_staleRuns++;
            in.pixels[q] = false;
            if (g_quarterBoxKnown[q]) for (int i = 0; i < 8; i++) mem_writeb(L.ds + kBox + i, g_quarterBox[q][i]);
            else { L.D16(kBox, 0x7000); L.D16(kBox + 4, 0x7000); }
            in.at[q][0] = L.d32(kSpriteX); in.at[q][1] = L.d32(kSpriteY); in.at[q][2] = L.d32(kSpriteZ);
            in.parent[q] = L.d16(kParent); in.idLow[q] = L.d16(kIdLow);
            for (int i = 0; i < 0x40; i++) in.map[q][i] = L.d8((u16)(kSpriteMap + i));
            L.D16(kFlatNext, TraceMem::get16(&g_start[kFlatNext]));
            L.D16(kPool, TraceMem::get16(&g_start[kPool]));
            return true;
        }
        in.at[q][0] = L.d32(kSpriteX); in.at[q][1] = L.d32(kSpriteY); in.at[q][2] = L.d32(kSpriteZ);
        in.parent[q] = L.d16(kParent); in.idLow[q] = L.d16(kIdLow);
        for (int i = 0; i < 0x40; i++) in.map[q][i] = L.d8((u16)(kSpriteMap + i));
        const int left = (Bit16s)L.d16(kBox) - 2, top = (Bit16s)L.d16(kBox + 2) + 2, right = (Bit16s)L.d16(kBox + 4) + 2, bottom = (Bit16s)L.d16(kBox + 6) - 2;
        const int last = (Bit16s)L.d16(kLastRow);
        in.pixels[q] = !(left >= g_wide || right < 0 || last - top >= g_deep || last - bottom < 0);
        if (in.pixels[q]) {
            in.snap[q] = g_snaps.size();
            g_snaps.resize(g_snaps.size() + kBgBytes);
            bg_read((PhysPt)L.d16(kBufferSeg) << 4, &g_snaps[in.snap[q]], kBgBytes);
        }
        // The sprite's pixels are mapped in the buffer by the row pass (later sprites are drawn over them): done
        // here by the hand-written row pass on a copy of the data segment, its lists thrown away, and the game's
        // own (a scan of every pixel of the box: half a frame's instructions under an explosion) left at once.
        {
            const PhysPt buffer = (PhysPt)L.d16(kBufferSeg) << 4;
            if (g_nativeMap && buffer + kBgBytes <= 0xA0000) {
                if (g_scratch.empty()) g_scratch.resize(0x20000);
                memcpy(&g_scratch[0], L.M + L.ds, 0x10000);
                TraceMem t;
                t.ds = &g_scratch[0]; t.tb = &g_scratch[0x10000]; t.cs = L.M + ((PhysPt)(u16)(loadSeg + kRendPara) << 4);
                t.D16(kFlatNext, TraceMem::get16(&g_start[kFlatNext]));
                t.D16(kPool, TraceMem::get16(&g_start[kPool]));
                memset(t.ds + kRows, 0, 2 * 200);
                tie640_trace_sprite_rows(t, L.M + buffer, NULL);
                for (int i = 0; i < 10; i++) mem_writeb(L.ds + kBox + i, t.ds[kBox + i]);
                reg_eip = CPU_Pop16();      // (a near call)
                return true;
            }
        }
        L.D16(kFlatNext, TraceMem::get16(&g_start[kFlatNext]));
        L.D16(kPool, TraceMem::get16(&g_start[kPool]));
        return true;
    }
    case kScanlines: {
        // xtrans2_drawxtrans: the scene is complete.  Each quarter replayed and resolved; a depth sort not yet
        // answered is asked of the game (the interpreter runs its routine, which returns HERE again).
        if (g_awaiting) {
            g_awaiting = false;
            g_answers[g_pending] = reg_ax == g_pending.a[1] ? 0 : 1;
            reg_sp = (u16)(reg_sp + 16);
        } else { g_events += (unsigned)g_cap.events.size(); g_quarter = 0; }
        for (; g_quarter < 4; g_quarter++) {
            if (resolve_quarter(g_quarter, draw, pitch, limitX, limitY, corner, g_pending.a)) continue;
            g_sorts++;
            g_awaiting = true;
            for (int i = 7; i >= 0; i--) CPU_Push16(g_pending.a[i]);
            CPU_Push16(SegValue(cs));
            CPU_Push16(kScanlines);
            SegSet16(cs, (u16)(loadSeg + kDrawPara));
            reg_eip = XA(0x0798, 0x07FA);
            return true;
        }
        // as the quarters' passes leave the tracers' own variables, and the segment registers
        { size_t n = 0; for (size_t r = 0; r < sizeof(kCarry) / sizeof(kCarry[0]); r++) { memcpy(L.M + L.ds + kCarry[r][0], g_carry[4] + n, (size_t)(kCarry[r][1] - kCarry[r][0])); n += kCarry[r][1] - kCarry[r][0]; } }
        // (FS: the pass's own work segment; X-Wing's is a block it asks for each frame, its segment at DS:4E5B)
#ifdef T640_XWING
        if (L.d16(0x4E5B)) SegSet16(fs, L.d16(0x4E5B));
#else
        SegSet16(fs, (u16)(loadSeg + 0x3598));
#endif
        SegSet16(es, L.d16(kTablesSeg));
        far_return();
        g_frames++;
        return true;
    }
    }
    return false;
}

void tie640_onepass_say() {
    const unsigned f = g_frames ? g_frames : 1;
    wclog(1, "TIE demo 640 one pass: %u frames; a frame: %.1f calls taken (%.1f faces, %.1f lines, %.1f flat polygons), %.1f objects, %.2f depth sorts asked of the game; "
          "in the quarters %.1f objects and %.1f faces left out; %u row passes on a box left over; the scaler run for %u quarters, left out for %u",
          g_frames, (double)g_events / f, (double)g_faces / f, (double)g_lines / f, (double)g_flats / f, (double)g_objects / f, (double)g_sorts / f,
          (double)g_skippedObjects / f, (double)g_skippedFaces / f, g_staleRuns, g_spritesRun, g_spritesOff);
}

// ---------------------------------------------------------------------------------------------------------------
// The check (WCNET_T640_CHECK=3): the frame drawn in quarters (the game's upper half four times, the native
// scanline pass), the memory put back, the frame drawn in one pass, the pictures compared.
// ---------------------------------------------------------------------------------------------------------------
namespace {
std::vector<Bit8u> g_checkStart, g_checkAfter;
Bit8u g_checkPic[640 * 400];
unsigned g_checks = 0, g_checkBad = 0, g_checkPixels = 0, g_checkDumps = 0, g_checkLogs = 0;
std::set<Bit32u> g_stateDiff;
double g_checkNextMs = 0;
}

void tie640_native_keep(bool on);
// (the check) the game's own row pass called in the reference's quarter: what it was given, to tell a sprite the game drew differently from quarter to quarter
void tie640_onepass_ref_sprite(Bit16u loadSeg, int quarter) {
    Live L; L.at(loadSeg);
    SprSig g; memset(&g, 0, sizeof(g)); g.w[0] = L.d16(XA(0x434A, 0x4EA8)); g.w[1] = L.d16(XA(0x434C, 0x4EAA)); g.w[2] = L.d16(XA(0x5112, 0x5C6A)); g.w[3] = L.d16(XA(0x4360, 0x4EBE));
    for (int i = 0; i < 4; i++) g.w[5 + i] = L.d16((u16)(kBox + 2 * i)); g.w[9] = L.d16(XA(0x4320, 0x4E7E)); g.w[10] = L.d16(XA(0x435C, 0x4EBA)); g.w[11] = L.d16(XA(0x435E, 0x4EBC)); g.w[12] = L.d16(XA(0x50BC, 0x5C14)); g.w[13] = L.d16(XA(0x50D2, 0x5C2A)); g.w[14] = L.d16(XA(0x50D4, 0x5C2C));
    if (quarter >= 0 && quarter < 4) g_sprRef[quarter].push_back(g);
}
void tie640_onepass_check_begin() {
    for (int q = 0; q < 4; q++) { g_sprRef[q].clear(); g_sprOne[q].clear(); }
    g_checkStart.assign(GetMemBase(), GetMemBase() + 0xA0000);
    tie640_native_keep(true);
}
// the quarters are done: their picture and memory kept, the scene's start put back
void tie640_onepass_check_middle(const Bit8u *draw) {
    g_checkAfter.assign(GetMemBase(), GetMemBase() + 0xA0000);
    memcpy(g_checkPic, draw, sizeof(g_checkPic));
    memcpy(GetMemBase(), &g_checkStart[0], 0xA0000);
}
// the one pass is done
void tie640_onepass_check_end(Bit16u loadSeg, const Bit8u *draw, int wide, int deep, int corner) {
    for (int q = 0; q < 4 && g_sprLogs < 12; q++) { 
        size_t i = 0;
        while (i < g_sprRef[q].size() && i < g_sprOne[q].size() && !memcmp(&g_sprRef[q][i], &g_sprOne[q][i], sizeof(SprSig))) i++;
        if (i < g_sprRef[q].size() || i < g_sprOne[q].size()) {
            g_sprLogs++;
            std::string t; char b[8];
            if (i < g_sprRef[q].size()) { t += " game"; for (int k = 0; k < 15; k++) { snprintf(b, sizeof(b), " %04X", g_sprRef[q][i].w[k]); t += b; } }
            if (i < g_sprOne[q].size()) { t += " | one"; for (int k = 0; k < 15; k++) { snprintf(b, sizeof(b), " %04X", g_sprOne[q][i].w[k]); t += b; } }
            wclog(1, "T640 one pass check: frame %u quarter %d: sprite %u of %u/%u differs:%s", g_checks + 1, q, (unsigned)i, (unsigned)g_sprRef[q].size(), (unsigned)g_sprOne[q].size(), t.c_str());
        }
    }
    g_checks++;
    unsigned bad = 0;
    int first = -1;
    for (int i = 0; i < 640 * 400; i++) if (g_checkPic[i] != draw[i]) { if (!bad++) first = i; }
    if (bad) {
        g_checkBad++;
        g_checkPixels += bad;
        const int fx = first % 640, fy = first / 640;
        const int x0 = 2 * (corner % 320), y0 = 2 * (corner / 320);
        const int q = (fx - x0 >= wide ? 1 : 0) + (fy - y0 >= deep ? 2 : 0);
        const int row = fy - y0 - (q >> 1) * deep, col = fx - x0 - (q & 1) * wide;
        if (g_checkDumps < 6) {
            const char *dir = getenv("WCNET_DUMP_DIR");
            char name[512];
            for (int which = 0; which < 2; which++) {
                snprintf(name, sizeof(name), "%s/op%u_%s.pgm", dir && dir[0] ? dir : ".", g_checkDumps, which ? "onepass" : "quarters");
                FILE *f = fopen(name, "wb");
                if (!f) continue;
                fprintf(f, "P5\n640 400\n255\n");
                fwrite(which ? draw : g_checkPic, 1, 640 * 400, f);
                fclose(f);
            }
            g_checkDumps++;
        }
        if (g_checkLogs < 12) {
            g_checkLogs++;
            wclog(1, "T640 one pass check: frame %u: %u pixels differ, the first at %d,%d (quarter %d, row %d, column %d): quarters %02X, one pass %02X",
                  g_checks, bad, fx, fy, q, row, col, g_checkPic[first], draw[first]);
            tie640_native_log_row(0, q, row);
            tie640_native_log_row(1, q, row);
        }
    }
    // the game's state: the objects (DS:67CA..8FAA) and the craft data (DS:9436..BF00)
    const Bit8u *M = GetMemBase();
    const PhysPt ds = (PhysPt)(Bit16u)(loadSeg + kDataPara) << 4;
    bool news = false;
    for (Bit32u o = XA(0x67CA, 0x7046); o < XA(0xBF00, 0xBE96); o++) {
        if (o == XA(0x8FAA, 0xFFFE)) o = XA(0x9436, 0xFFFE);
        if (M[ds + o] == g_checkAfter[ds + o] || g_stateDiff.count(o)) continue;
        g_stateDiff.insert(o);
        news = true;
    }
    const double now = PIC_FullIndex();
    if (now >= g_checkNextMs || news) {
        g_checkNextMs = now + 5000;
        wclog(1, "T640 one pass check: %u frames drawn both ways, %u differ (%u pixels); %u bytes of the objects and craft data ever differed",
              g_checks, g_checkBad, g_checkPixels, (unsigned)g_stateDiff.size());
        if (news) {
            std::string text;
            unsigned ranges = 0;
            for (std::set<Bit32u>::const_iterator i = g_stateDiff.begin(); i != g_stateDiff.end() && ranges < 60; ranges++) {
                Bit32u lo = *i, hi = lo;
                for (++i; i != g_stateDiff.end() && *i <= hi + 2; ++i) hi = *i;
                char b[40];
                snprintf(b, sizeof(b), lo == hi ? " DS:%04X" : " DS:%04X-%04X", lo, hi);
                text += b;
            }
            wclog(1, "T640 one pass check: the state that differed:%s", text.c_str());
        }
    }
}

}  // namespace wc
