/*
 *  Privateer's added ships (wcnet_priv.h, docs/privateer.md section 42): ships the player can fly that the game never
 *  let a player fly -- the Stiletto first (the user, 2026-11-27: "you can purchase the stiletto in privateer ... we
 *  would need a new cockpit ... manufacture at runtime the actual cockpit from the centurion and orion pieces ...
 *  Think extensibly since we may want a place to insert the demon as well").
 *
 *  The game knows four hulls (ds:655E, 0-3) and answers many questions by that number in its own code (the hold's
 *  size, the dealer's screens).  An added ship is therefore flown ON one of the four (its `hull`: the Stiletto on the
 *  Centurion), and what makes it itself is data the game reads by NAME, which the layer adds to:
 *
 *  - the player's record names the ship's appearance (+0x75: "FIGHTER"); the record's maker (ovr372:0000, through
 *    its thunk stub372:003E) is given the added ship's ("STILETTO": the ship as every view and every other player
 *    sees it);
 *  - OPTIONS\APPRCOCK.IFF names an appearance's cockpit, MIDGAMES\SHIP2CHR.IFF its ship in the landing and takeoff
 *    scenes: each gets a row for the added ship;
 *  - the cockpit itself (COCKPITS\<name>.IFF and .PAK) is a new file, made here from the game's own cockpits.
 *
 *  The files are added to PRIV.TRE as the game reads it, not on the disk: the game finds a file by a binary search of
 *  the TRE's directory ON DISK at every open (measured: seeks to the middle entry, the quarter, ...), and the
 *  directory has room for 339 more entries before the first file's data.  So the layer answers the program's reads of
 *  the directory with its own (the real entries and the added ones, sorted by name as the search needs; a replaced
 *  file's entry pointed at the new bytes), and reads past the real file's end with the added files' bytes.  Nothing
 *  of the game's memory or files is changed for it, and RF.TRE (Righteous Fire) is treated the same way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "wcnet_priv.h"
#include "wcnet_priv_ships.h"
#include "wcnet_log.h"
#include "dos_inc.h"
#include "regs.h"
#include "mem.h"

namespace wc {

namespace {

// ---- The added ships ------------------------------------------------------------------------------------------------

const PrivAddedShip kShips[] = {
    // id  name        appearance   cockpit    hull  scene ship
    { 1,  "Stiletto", "STILETTO",  "STILCK",   2,    2 },
};
const int kShipCount = (int)(sizeof(kShips) / sizeof(kShips[0]));

int g_ship = 0;             // the added ship the player owns (its id; 0: one of the game's four)
int g_shipEnv = -1;         // WCPRIVSHIP=<name>: the player's ship is that one whatever the game says (a rig's)

const PrivAddedShip *ship_by_id(int id) {
    for (int i = 0; i < kShipCount; i++) if (kShips[i].id == id) return &kShips[i];
    return 0;
}

// ---- IFF, as much as the tables need --------------------------------------------------------------------------------

void put32be(std::vector<Bit8u> &v, Bit32u x) { v.push_back((Bit8u)(x >> 24)); v.push_back((Bit8u)(x >> 16)); v.push_back((Bit8u)(x >> 8)); v.push_back((Bit8u)x); }
void put32le(std::vector<Bit8u> &v, Bit32u x) { v.push_back((Bit8u)x); v.push_back((Bit8u)(x >> 8)); v.push_back((Bit8u)(x >> 16)); v.push_back((Bit8u)(x >> 24)); }
Bit32u get32be(const Bit8u *p) { return ((Bit32u)p[0] << 24) | ((Bit32u)p[1] << 16) | ((Bit32u)p[2] << 8) | p[3]; }
Bit32u get32le(const Bit8u *p) { return ((Bit32u)p[3] << 24) | ((Bit32u)p[2] << 16) | ((Bit32u)p[1] << 8) | p[0]; }
void put_name8(std::vector<Bit8u> &v, const char *s) { for (int i = 0; i < 8; i++) { v.push_back(*s ? (Bit8u)*s : 0); if (*s) s++; } }

// OPTIONS\APPRCOCK.IFF: FORM SHIP { TABL: an offset per row; then per row a length (24, little-endian) and a chunk
// ASHP of the appearance's and the cockpit's names, 8 bytes each }.  The added ships' rows after the game's.
bool apprcock_with_rows(const std::vector<Bit8u> &in, std::vector<Bit8u> *out) {
    if (in.size() < 20 || memcmp(&in[0], "FORM", 4) || memcmp(&in[8], "SHIPTABL", 8)) return false;
    const Bit32u rows = get32be(&in[16]) / 4;
    if (in.size() != 20 + rows * 4 + rows * 28) return false;
    std::vector<Bit8u> body;                // the rows: 16 bytes each
    for (Bit32u i = 0; i < rows; i++) {
        const Bit8u *r = &in[20 + rows * 4 + i * 28];
        if (get32le(r) != 24 || memcmp(r + 4, "ASHP", 4) || get32be(r + 8) != 16) return false;
        body.insert(body.end(), r + 12, r + 28);
    }
    for (int i = 0; i < kShipCount; i++) { put_name8(body, kShips[i].appearance); put_name8(body, kShips[i].cockpit); }
    const Bit32u n = (Bit32u)(body.size() / 16);
    out->clear();
    out->insert(out->end(), (const Bit8u *)"FORM", (const Bit8u *)"FORM" + 4);
    put32be(*out, 4 + 8 + n * 4 + n * 28);
    out->insert(out->end(), (const Bit8u *)"SHIPTABL", (const Bit8u *)"SHIPTABL" + 8);
    put32be(*out, n * 4);
    for (Bit32u i = 0; i < n; i++) put32le(*out, 20 + n * 4 + i * 28);
    for (Bit32u i = 0; i < n; i++) {
        put32le(*out, 24);
        out->insert(out->end(), (const Bit8u *)"ASHP", (const Bit8u *)"ASHP" + 4);
        put32be(*out, 16);
        out->insert(out->end(), body.begin() + i * 16, body.begin() + i * 16 + 16);
    }
    return true;
}

// MIDGAMES\SHIP2CHR.IFF: FORM MIDG { INFO: rows of an appearance's name (8 bytes) and its ship in the landing and
// takeoff scenes (a word) }.
bool ship2chr_with_rows(const std::vector<Bit8u> &in, std::vector<Bit8u> *out) {
    if (in.size() < 20 || memcmp(&in[0], "FORM", 4) || memcmp(&in[8], "MIDGINFO", 8)) return false;
    const Bit32u len = get32be(&in[16]);
    if (len % 10 || in.size() != 20 + len) return false;
    out->assign(in.begin(), in.end());
    for (int i = 0; i < kShipCount; i++) {
        put_name8(*out, kShips[i].appearance);
        out->push_back((Bit8u)kShips[i].sceneShip);
        out->push_back(0);
    }
    const Bit32u now = (Bit32u)out->size() - 20;
    (*out)[4] = (Bit8u)((now + 12) >> 24); (*out)[5] = (Bit8u)((now + 12) >> 16); (*out)[6] = (Bit8u)((now + 12) >> 8); (*out)[7] = (Bit8u)(now + 12);
    (*out)[16] = (Bit8u)(now >> 24); (*out)[17] = (Bit8u)(now >> 16); (*out)[18] = (Bit8u)(now >> 8); (*out)[19] = (Bit8u)now;
    return true;
}

// ---- The TRE as the game reads it -----------------------------------------------------------------------------------

enum { kTreEntry = 74, kTreName = 65 };

struct TreAdded { std::string name; std::vector<Bit8u> data; Bit32u at; };

struct VTre {
    int entry;                      // DOS's handle entry (-1: none open)
    std::string file;               // its name ("PRIV.TRE")
    bool on;                        // the layer answers for it (false: the game reads the file as it is)
    Bit32u realSize, size;          // the file's length; with the added files
    Bit32u pos;                     // the program's place in it
    std::vector<Bit8u> head;        // the count and the directory as the program shall read them (from offset 0)
    std::vector<TreAdded> added;    // the files past the real end
    std::vector<Bit8u> realDir;     // the file's own header and directory
    VTre() : entry(-1), on(false), realSize(0), size(0), pos(0) {}
};
VTre g_tre;
bool g_inside = false;              // our own reads of the file go through DOS untouched
int g_treLog = -1;
bool tre_log() { if (g_treLog < 0) { const char *e = getenv("PRIVTRELOG"); g_treLog = e && atoi(e) ? 1 : 0; } return g_treLog == 1; }

struct Inside { Inside() { g_inside = true; } ~Inside() { g_inside = false; } };

bool real_read(Bit32u at, Bit8u *to, Bit32u n) {
    Inside in;
    Bit32u p = at;
    if (!DOS_SeekFile((Bit16u)g_tre.entry, &p, DOS_SEEK_SET)) return false;
    while (n) {
        Bit16u part = (Bit16u)(n > 0x8000 ? 0x8000 : n);
        const Bit16u want = part;
        if (!DOS_ReadFile((Bit16u)g_tre.entry, to, &part) || part != want) return false;
        to += part; n -= part;
    }
    return true;
}

std::string tre_name(const char *leaf_dir, const char *leaf) { return std::string("..\\..\\DATA\\") + leaf_dir + "\\" + leaf; }

// A file of the real TRE by its name there (false: none).
bool real_file(const std::string &name, std::vector<Bit8u> *out) {
    const Bit32u count = get32le(&g_tre.realDir[0]);
    for (Bit32u i = 0; i < count; i++) {
        const Bit8u *e = &g_tre.realDir[8 + i * kTreEntry];
        if (strncmp((const char *)e + 1, name.c_str(), kTreName) != 0) continue;
        const Bit32u at = get32le(e + 66), size = get32le(e + 70);
        if (at > g_tre.realSize || size > g_tre.realSize - at) return false;
        out->resize(size);
        return size == 0 || real_read(at, &(*out)[0], size);
    }
    return false;
}

void tre_add(const std::string &name, const std::vector<Bit8u> &data) {
    TreAdded a;
    a.name = name; a.data = data; a.at = 0;
    g_tre.added.push_back(a);
}

struct DirLess {
    bool operator()(const std::vector<Bit8u> &a, const std::vector<Bit8u> &b) const { return strncmp((const char *)&a[1], (const char *)&b[1], kTreName) < 0; }
};

// The TRE just opened: its directory read, the added ships' files made from the game's own, the directory the program
// shall see built.  Anything amiss (another TRE's layout, a file missing, no room): the game reads the file as it is.
void tre_build() {
    VTre &t = g_tre;
    t.on = false; t.added.clear(); t.head.clear(); t.pos = 0;
    {
        Inside in;
        Bit32u end = 0;
        if (!DOS_SeekFile((Bit16u)t.entry, &end, DOS_SEEK_END)) return;
        t.realSize = end;
        Bit32u zero = 0;
        DOS_SeekFile((Bit16u)t.entry, &zero, DOS_SEEK_SET);
    }
    Bit8u h[8];
    if (t.realSize < 8 || !real_read(0, h, 8)) return;
    const Bit32u count = get32le(h), dataStart = get32le(h + 4);
    if (count == 0 || count > 4000 || dataStart < 8 + count * kTreEntry || dataStart > t.realSize || dataStart > (1u << 20)) {
        wclog(1, "Privateer: added ships: %s is not a TRE we know (%u files, data at %u): left as it is", t.file.c_str(), count, dataStart);
        return;
    }
    t.realDir.resize(dataStart);
    if (!real_read(0, &t.realDir[0], dataStart)) return;

    std::vector<Bit8u> in, out, pak;
    if (real_file(tre_name("OPTIONS", "APPRCOCK.IFF"), &in) && apprcock_with_rows(in, &out)) tre_add(tre_name("OPTIONS", "APPRCOCK.IFF"), out);
    else { wclog(1, "Privateer: added ships: %s's APPRCOCK.IFF not as expected: left as it is", t.file.c_str()); t.added.clear(); return; }
    if (real_file(tre_name("MIDGAMES", "SHIP2CHR.IFF"), &in) && ship2chr_with_rows(in, &out)) tre_add(tre_name("MIDGAMES", "SHIP2CHR.IFF"), out);
    else { wclog(1, "Privateer: added ships: %s's SHIP2CHR.IFF not as expected: left as it is", t.file.c_str()); t.added.clear(); return; }
    for (int i = 0; i < kShipCount; i++) {
        if (!priv_ship_cockpit(kShips[i], real_file, &out, &pak)) {
            wclog(1, "Privateer: added ships: the %s's cockpit could not be made from %s: left as it is", kShips[i].name, t.file.c_str());
            t.added.clear();
            return;
        }
        tre_add(tre_name("COCKPITS", (std::string(kShips[i].cockpit) + ".IFF").c_str()), out);
        tre_add(tre_name("COCKPITS", (std::string(kShips[i].cockpit) + ".PAK").c_str()), pak);
    }

    // the directory: the real entries (a replaced file's pointed at ours), the new ones, sorted by name
    std::vector<std::vector<Bit8u> > dir;
    for (Bit32u i = 0; i < count; i++) dir.push_back(std::vector<Bit8u>(t.realDir.begin() + 8 + i * kTreEntry, t.realDir.begin() + 8 + (i + 1) * kTreEntry));
    Bit32u at = t.realSize;
    for (size_t k = 0; k < t.added.size(); k++) {
        TreAdded &a = t.added[k];
        a.at = at; at += (Bit32u)a.data.size();
        std::vector<Bit8u> e(kTreEntry, 0);
        e[0] = 1;
        strncpy((char *)&e[1], a.name.c_str(), kTreName - 1);
        e[66] = (Bit8u)a.at; e[67] = (Bit8u)(a.at >> 8); e[68] = (Bit8u)(a.at >> 16); e[69] = (Bit8u)(a.at >> 24);
        const Bit32u n = (Bit32u)a.data.size();
        e[70] = (Bit8u)n; e[71] = (Bit8u)(n >> 8); e[72] = (Bit8u)(n >> 16); e[73] = (Bit8u)(n >> 24);
        bool replaced = false;
        for (size_t i = 0; i < dir.size() && !replaced; i++) {
            if (strncmp((const char *)&dir[i][1], a.name.c_str(), kTreName) == 0) { dir[i] = e; replaced = true; }
        }
        if (!replaced) dir.push_back(e);
    }
    if (8 + dir.size() * kTreEntry > dataStart) {
        wclog(1, "Privateer: added ships: no room in %s's directory (%u entries): left as it is", t.file.c_str(), (unsigned)dir.size());
        t.added.clear();
        return;
    }
    std::stable_sort(dir.begin(), dir.end(), DirLess());
    t.head.assign(t.realDir.begin(), t.realDir.end());
    const Bit32u n = (Bit32u)dir.size();
    t.head[0] = (Bit8u)n; t.head[1] = (Bit8u)(n >> 8); t.head[2] = (Bit8u)(n >> 16); t.head[3] = (Bit8u)(n >> 24);
    for (size_t i = 0; i < dir.size(); i++) memcpy(&t.head[8 + i * kTreEntry], &dir[i][0], kTreEntry);
    t.size = at;
    t.on = true;
    wclog(1, "Privateer: added ships: %s read with %u files of ours (%u entries, was %u; %u bytes past its end)", t.file.c_str(),
          (unsigned)t.added.size(), n, count, t.size - t.realSize);
}

}   // namespace

// ---- DOS's side (src/dos/dos_files.cpp) -----------------------------------------------------------------------------

void priv_ships_file_opened(const char *fullname, Bit16u entry) {
    const size_t n = strlen(fullname);
    if (n < 4 || strcasecmp(fullname + n - 4, ".TRE") != 0 || g_inside) return;
    const char *off = getenv("WCPRIVSHIPS");
    if (off && !strcmp(off, "0")) return;               // (WCPRIVSHIPS=0: the game's files as they are)
    g_tre.entry = entry;
    g_tre.file = fullname;
    tre_build();
    if (!g_tre.on) g_tre.entry = -1;
}

void priv_ships_file_closed(Bit16u entry) {
    if ((int)entry == g_tre.entry && !g_inside) { g_tre.entry = -1; g_tre.on = false; }
}

bool priv_tre_is(Bit16u entry) { return g_tre.on && (int)entry == g_tre.entry && !g_inside; }

bool priv_tre_seek(Bit16u entry, Bit32u *pos, Bit32u type) {
    (void)entry;
    VTre &t = g_tre;
    Bit32s move = (Bit32s)*pos;
    Bit32u to;
    if (type == DOS_SEEK_SET) to = *pos;
    else if (type == DOS_SEEK_CUR) to = (Bit32u)((Bit32s)t.pos + move);
    else if (type == DOS_SEEK_END) to = (Bit32u)((Bit32s)t.size + move);
    else return false;
    t.pos = to;
    *pos = to;
    return true;
}

bool priv_tre_read(Bit16u entry, Bit8u *data, Bit16u *amount) {
    (void)entry;
    VTre &t = g_tre;
    Bit32u want = *amount, done = 0;
    if (t.pos >= t.size) want = 0;
    else if (want > t.size - t.pos) want = t.size - t.pos;
    while (done < want) {
        const Bit32u p = t.pos + done;
        Bit32u n = want - done;
        if (p < t.head.size()) {
            if (n > t.head.size() - p) n = (Bit32u)t.head.size() - p;
            memcpy(data + done, &t.head[p], n);
        } else if (p < t.realSize) {
            if (n > t.realSize - p) n = t.realSize - p;
            if (!real_read(p, data + done, n)) { DOS_SetError(DOSERR_ACCESS_DENIED); return false; }
        } else {
            size_t k = 0;
            while (k < t.added.size() && p >= t.added[k].at + t.added[k].data.size()) k++;
            if (k == t.added.size()) break;
            const TreAdded &a = t.added[k];
            const Bit32u in = p - a.at;
            if (n > a.data.size() - in) n = (Bit32u)a.data.size() - in;
            memcpy(data + done, &a.data[in], n);
        }
        done += n;
    }
    t.pos += done;
    *amount = (Bit16u)done;
    if (tre_log()) wclog(1, "Privateer: TRE read %u at %u", done, t.pos - done);
    return true;
}

// ---- The player's ship ----------------------------------------------------------------------------------------------

const PrivAddedShip *priv_added_ship() {
    if (g_shipEnv < 0) {
        g_shipEnv = 0;
        const char *e = getenv("WCPRIVSHIP");
        for (int i = 0; e && i < kShipCount; i++) if (!strcasecmp(e, kShips[i].name)) g_shipEnv = kShips[i].id;
    }
    return ship_by_id(g_shipEnv ? g_shipEnv : g_ship);
}

// At stub372:003E, the player's record about to be made: (this, place x y z, appearance far, type far).  The record
// keeps the names' first 8 letters (+0x75, +0x7E).  An added ship's appearance: written where the maker will copy it
// to, and the argument pointed there (a copy onto itself).
void priv_ships_record_make() {
    const PrivAddedShip *s = priv_added_ship();
    if (!s || !g_tre.on) return;
    const PhysPt stack = SegPhys(ss) + reg_sp;
    const Bit16u rec = mem_readw(stack + 4);
    char was[9] = {0};
    const RealPt name = mem_readd(stack + 0x12);
    for (int i = 0; i < 8; i++) was[i] = (char)mem_readb(Real2Phys(name) + i);
    const PhysPt to = SegPhys(ds) + (Bit16u)(rec + 0x75);
    for (int i = 0; i < 9; i++) mem_writeb(to + i, i < (int)strlen(s->appearance) ? (Bit8u)s->appearance[i] : 0);
    mem_writew(stack + 0x12, (Bit16u)(rec + 0x75));
    mem_writew(stack + 0x14, SegValue(ds));
    wclog(1, "Privateer: added ships: the player's record made as %s (the game's: %s)", s->appearance, was);
}

}
