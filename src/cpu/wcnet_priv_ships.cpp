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
 *  - OPTIONS\APPRCOCK.IFF has a row per hull: the ship's appearance ("FIGHTER") and its cockpit ("FIGHTCK").  It gets
 *    a row per added ship ("STILETTO", "STILCK"), and the flight's set-up, which seeks the hull's row (ovr262:07AF,
 *    through the IFF reader's seg214:03DF), is given the added ship's row.  The appearance is the ship as every
 *    view and every other player sees it, and by SKELETON.IFF its gun mounts and flight numbers too;
 *  - MIDGAMES\SHIP2CHR.IFF names an appearance's ship in the landing and takeoff scenes: a row for the added ship;
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
    // id  name        appearance   type        cockpit    hull  scene ship  hold  more
    { 1,  "Stiletto", "STILETTO",  "STILTYPE", "STILCK",   2,    2,          20,   10, { { 2, -16, -2 }, { 3, 16, -2 }, { -1, 0, 0 } },
      { 0x2C, 0x2F, 0x3E, 0 },        // (the game's own Stiletto: the generators of engine upgrade 2, shield generator 2's 200)
      500000, "credits" },            // (the user: "you can buy the Stiletto for 500,000 credits", from Admiral Terrell)
};
const int kShipCount = (int)(sizeof(kShips) / sizeof(kShips[0]));

int g_ship = 0;             // the added ship the player owns (its id; 0: one of the game's four)
int g_gameRows = 4;         // APPRCOCK.IFF's rows of the game's own hulls: the added ships' follow
int g_hullHeld = -1;        // the hull byte's own value while it is hidden from the hold's routines (-1: not)
int g_shipEnv = -1;         // WCPRIVSHIP=<name>: the player's ship is that one whatever the game says (a rig's)
// A sale of the dealer's routine while it runs (ovr286:0CC1 to its end)
struct Sale {
    bool active, accepted, renamed;
    int ship;               // the added ship sold (its id; 0: one of the game's four)
    int owner;              // the added ship owned when it began
    int prices;             // the price routine's calls so far: the old ship's, the new one's, and after an Accept again
    Bit8u kept[4][9];       // the dealer's own conversations' names, written over meanwhile
    Sale() : active(false), accepted(false), renamed(false), ship(0), owner(0), prices(0) {}
};
Sale g_sale;

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
    g_gameRows = (int)rows;
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

// OPTIONS\LIMITS.IFF: FORM SHIP { FIGH, MRCH, CLNK, TUGS: per hull (the Centurion's, the Galaxy's, the Tarsus's, the
// Orion's) the item numbers its dealers do not sell; PRIC ... }.  The added ship's own list: its hull's and its own.
bool limits_for_ship(const PrivAddedShip &ship, const std::vector<Bit8u> &in, std::vector<Bit8u> *out) {
    static const char *const tags[4] = { "CLNK", "TUGS", "FIGH", "MRCH" };
    if (ship.hull < 0 || ship.hull > 3 || in.size() < 20 || memcmp(&in[0], "FORM", 4) || memcmp(&in[8], "SHIP", 4)) return false;
    size_t at = 12;
    while (at + 8 <= in.size()) {
        const Bit32u n = get32be(&in[at + 4]);
        if (n > in.size() - at - 8) return false;
        if (!memcmp(&in[at], tags[ship.hull], 4)) {
            std::vector<Bit8u> list(in.begin() + at + 8, in.begin() + at + 8 + n);
            for (int i = 0; i < 8 && ship.notSold[i]; i++) {
                if (std::find(list.begin(), list.end(), ship.notSold[i]) == list.end()) list.push_back(ship.notSold[i]);
            }
            out->assign(in.begin(), in.begin() + at + 4);
            put32be(*out, (Bit32u)list.size());
            out->insert(out->end(), list.begin(), list.end());
            if (list.size() & 1) out->push_back(0);
            out->insert(out->end(), in.begin() + at + 8 + n + (n & 1), in.end());
            const Bit32u total = (Bit32u)out->size() - 8;
            (*out)[4] = (Bit8u)(total >> 24); (*out)[5] = (Bit8u)(total >> 16); (*out)[6] = (Bit8u)(total >> 8); (*out)[7] = (Bit8u)total;
            return true;
        }
        at += 8 + n + (n & 1);
    }
    return false;
}

// APPEARNC\SKELETON.IFF: a table of offsets (from byte 20), each record a length and a FORM SKEL whose FORM WEAP's
// FORM GUNS has a POSG per mount: the mount, its place on the ship (three words), a word, and its mark's place on the
// weapons display (two words).  The marks of the added ship's skeleton (the number in its appearance's SKEL chunk)
// moved; nothing else of the file changes.
bool skeleton_with_marks(const PrivAddedShip &ship, const std::vector<Bit8u> &appearance, std::vector<Bit8u> *skel) {
    int id = -1;
    static const Bit8u tag[] = { 'S', 'K', 'E', 'L', 0, 0, 0, 1 };
    for (size_t i = appearance.size() >= 9 ? appearance.size() - 9 : 0; i > 12; i--) {
        if (!memcmp(&appearance[i], tag, 8)) { id = appearance[i + 8]; break; }
    }
    std::vector<Bit8u> &d = *skel;
    if (id < 0 || d.size() < 24 || memcmp(&d[12], "TABL", 4)) return false;
    const Bit32u rows = get32be(&d[16]) / 4;
    if ((Bit32u)id >= rows || 20 + rows * 4 > d.size()) return false;
    const Bit32u at = get32le(&d[20 + id * 4]);
    if (at + 4 > d.size()) return false;
    const Bit32u len = get32le(&d[at]);
    if (len > d.size() - at - 4) return false;
    size_t end = at + 4 + len;
    static const Bit8u posg[] = { 'P', 'O', 'S', 'G', 0, 0, 0, 13 };
    bool any = false;
    for (size_t i = at + 4; i + 21 <= end; i++) {
        if (!memcmp(&d[i], "LNCH", 4)) break;           // (the launchers' follow the guns')
        if (memcmp(&d[i], posg, 8)) continue;
        for (int k = 0; k < 4 && ship.marks[k].mount >= 0; k++) {
            if (d[i + 8] != ship.marks[k].mount) continue;
            d[i + 17] = (Bit8u)ship.marks[k].x; d[i + 18] = (Bit8u)(ship.marks[k].x >> 8);
            d[i + 19] = (Bit8u)ship.marks[k].y; d[i + 20] = (Bit8u)(ship.marks[k].y >> 8);
            any = true;
        }
    }
    return any;
}

// ---- The TRE as the game reads it -----------------------------------------------------------------------------------

// OPTIONS\GAMEFLOW.IFF: every room's sprites with what a click on each does (EFCT: pairs of a command and its
// number, run by ovr281:11C3).  Command 0x00 plays conversation n of the file's own list (FORM CONV, DATA: names of
// 8 bytes), 0x29 changes the tune, 0x1E n is the dealer's sale of hull n (ovr286:0CC1).  An added ship is offered
// after its conversation (Admiral Terrell's "credits", which the game plays once the campaign is won, every time
// he is talked to): a sale of (the ship's id * 16 + its hull) follows it, which priv_ships_hook takes for that
// ship's.  WCPRIVSALE=<ship> (a rig's) makes every dealer's Centurion that ship instead.
bool gameflow_chunks(const Bit8u *d, size_t n, const std::map<int, int> &after, int rigSale, std::vector<Bit8u> *out, int *edits) {
    size_t off = 0;
    while (off + 8 <= n) {
        const Bit32u size = get32be(d + off + 4);
        if (size > n - off - 8) return false;
        const Bit8u *body = d + off + 8;
        out->insert(out->end(), d + off, d + off + 4);
        const size_t sizeAt = out->size();
        put32be(*out, 0);
        if (!memcmp(d + off, "FORM", 4)) {
            if (size < 4) return false;
            out->insert(out->end(), body, body + 4);
            if (!gameflow_chunks(body + 4, size - 4, after, rigSale, out, edits)) return false;
        } else if (!memcmp(d + off, "EFCT", 4) && !(size & 1)) {
            for (Bit32u i = 0; i < size; i += 2) {
                Bit8u op = body[i], arg = body[i + 1];
                if (op == 0x1E && rigSale && arg == (rigSale & 15)) { arg = (Bit8u)rigSale; (*edits)++; }
                out->push_back(op); out->push_back(arg);
                if (op != 0x00 || !after.count(arg)) continue;
                while (i + 2 < size && body[i + 2] == 0x29) { i += 2; out->push_back(body[i]); out->push_back(body[i + 1]); }
                out->push_back(0x1E); out->push_back((Bit8u)after.find(arg)->second);
                (*edits)++;
            }
        } else {
            out->insert(out->end(), body, body + size);
        }
        const Bit32u now = (Bit32u)(out->size() - sizeAt - 4);
        (*out)[sizeAt] = (Bit8u)(now >> 24); (*out)[sizeAt + 1] = (Bit8u)(now >> 16); (*out)[sizeAt + 2] = (Bit8u)(now >> 8); (*out)[sizeAt + 3] = (Bit8u)now;
        if (now & 1) out->push_back(0);
        off += 8 + size + (size & 1);
    }
    return off == n || off == n + 1;
}

bool gameflow_with_sales(const std::vector<Bit8u> &in, std::vector<Bit8u> *out) {
    static const char mark[] = "CONVDATA";
    size_t at = 0;
    for (; at + 12 <= in.size(); at++) if (!memcmp(&in[at], mark, 8)) break;
    if (at + 12 > in.size()) return false;
    const Bit32u size = get32be(&in[at + 8]);
    if (size > in.size() - at - 12) return false;
    std::map<int, int> after;
    int rigSale = 0;
    const char *rig = getenv("WCPRIVSALE");
    for (int i = 0; i < kShipCount; i++) {
        if (rig && !strcasecmp(rig, kShips[i].name)) rigSale = kShips[i].id * 16 + kShips[i].hull;
        if (!kShips[i].soldAfter) continue;
        for (Bit32u k = 0; k + 8 <= size && k / 8 < 256; k += 8) {
            if (!strncmp((const char *)&in[at + 12 + k], kShips[i].soldAfter, 8)) after[(int)(k / 8)] = kShips[i].id * 16 + kShips[i].hull;
        }
    }
    if (after.empty() && !rigSale) return false;
    int edits = 0;
    out->clear();
    if (!gameflow_chunks(&in[0], in.size(), after, rigSale, out, &edits) || !edits) return false;
    wclog(1, "Privateer: added ships: GAMEFLOW.IFF given %d sale%s", edits, edits == 1 ? "" : "s");
    return true;
}

enum { kTreEntry = 74, kTreName = 65 };

struct TreAdded {
    std::string name; std::vector<Bit8u> data; Bit32u at;
    // A file that differs for an added ship's owner: `data` is the game's own (for everybody else), `own` that
    // ship's, both filled with zeros to one length -- the directory cannot differ (the program keeps what it read of
    // it in its file buffer), only what a read of the file returns.
    int forShip;
    std::vector<Bit8u> own;
};

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

void tre_add(const std::string &name, const std::vector<Bit8u> &data, int forShip = 0, const std::vector<Bit8u> *own = 0) {
    TreAdded a;
    a.name = name; a.data = data; a.at = 0; a.forShip = forShip;
    if (own) {
        a.own = *own;
        const size_t n = a.own.size() > a.data.size() ? a.own.size() : a.data.size();
        a.own.resize(n, 0); a.data.resize(n, 0);
    }
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

    std::vector<Bit8u> in, out, pak, skel;
    bool skelChanged = false;
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
        if (priv_ship_appearance(kShips[i], real_file, &out)) tre_add(tre_name("APPEARNC", (std::string(kShips[i].appearance) + ".IFF").c_str()), out);
        else real_file(tre_name("APPEARNC", (std::string(kShips[i].appearance) + ".IFF").c_str()), &out);
        if (real_file(tre_name("OPTIONS", "LIMITS.IFF"), &in) && limits_for_ship(kShips[i], in, &pak)) tre_add(tre_name("OPTIONS", "LIMITS.IFF"), in, kShips[i].id, &pak);
        if (skel.empty()) real_file(tre_name("APPEARNC", "SKELETON.IFF"), &skel);
        if (!skel.empty() && skeleton_with_marks(kShips[i], out, &skel)) skelChanged = true;
    }

    if (skelChanged) tre_add(tre_name("APPEARNC", "SKELETON.IFF"), skel);
    // the sales: the conversations, and the rooms' script that runs them (neither there: the ships are flown all the
    // same, from a saved game)
    {
        std::vector<std::pair<std::string, std::vector<Bit8u> > > convs;
        bool all = true;
        for (int i = 0; i < kShipCount && all; i++) {
            if (kShips[i].price && !priv_ship_convs(kShips[i], real_file, &convs)) all = false;
        }
        if (all && real_file(tre_name("OPTIONS", "GAMEFLOW.IFF"), &in) && gameflow_with_sales(in, &out)) {
            for (size_t i = 0; i < convs.size(); i++) tre_add(tre_name("CONV", convs[i].first.c_str()), convs[i].second);
            tre_add(tre_name("OPTIONS", "GAMEFLOW.IFF"), out);
        } else {
            wclog(1, "Privateer: added ships: the sales could not be made from %s: no ship is offered", t.file.c_str());
        }
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
    for (size_t i = 0; i < dir.size(); i++) {
        memcpy(&t.head[8 + i * kTreEntry], &dir[i][0], kTreEntry);
    }
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
            const PrivAddedShip *owner = a.forShip ? priv_added_ship() : 0;
            memcpy(data + done, owner && owner->id == a.forShip ? &a.own[in] : &a.data[in], n);
        }
        done += n;
    }
    t.pos += done;
    *amount = (Bit16u)done;
    if (tre_log()) wclog(1, "Privateer: TRE read %u at %u", done, t.pos - done);
    return true;
}

// ---- The player's ship ----------------------------------------------------------------------------------------------

enum { kHullByte = 0x655E };

const PrivAddedShip *priv_added_ship_by_id(int id) { return g_tre.on ? ship_by_id(id) : 0; }

const char *priv_hull_appearance(int hull) {
    static const char *const names[4] = { "CLUNKER", "TUG", "FIGHTER", "MERCHANT" };
    return hull >= 0 && hull < 4 ? names[hull] : "CLUNKER";
}

// The ship the player owns, if an added one.  Which: the saved game's word (below), kept while the game's hull byte
// is that ship's hull -- the game writes the byte at a load and at the dealer's sale only, so a ship of the game's
// bought on another hull ends it.
const PrivAddedShip *priv_added_ship() {
    if (g_shipEnv < 0) {
        g_shipEnv = 0;
        const char *e = getenv("WCPRIVSHIP");       // (a rig's: that ship whatever the saved game says)
        for (int i = 0; e && i < kShipCount; i++) if (!strcasecmp(e, kShips[i].name)) g_shipEnv = kShips[i].id;
    }
    const PrivAddedShip *s = ship_by_id(g_shipEnv ? g_shipEnv : g_ship);
    if (!s || !g_tre.on) return 0;
    if (g_hullHeld < 0 && priv_r8(kHullByte) != s->hull) {
        if (g_ship && !g_shipEnv) {
            wclog(1, "Privateer: added ships: the hull is %u now: the %s is no longer the player's ship", priv_r8(kHullByte), s->name);
            g_ship = 0;
        }
        return 0;
    }
    return s;
}

namespace {

bool code_is(const Bit8u *want, size_t n) {
    const PhysPt at = SegPhys(cs) + reg_ip;
    for (size_t i = 0; i < n; i++) if (mem_readb(at + i) != want[i]) return false;
    return true;
}

// The hold's size.  The game sets it by the hull in its own code (ovr269:0AA5, 0B40, 0B89: 50 for the Centurion, 75
// with the expansion), and leaves it alone for a hull that is none of the four: an added ship's is written here, and
// the routine given no hull (0AA5's is an argument; the other two read the hull byte, which is put back at their
// ends).
void hold_hook(int which) {
    const PrivAddedShip *s = priv_added_ship();
    if (getenv("PRIVSHIPLOG")) wclog(1, "Privateer: added ships: hold routine %d, ship %s, hull byte %u, arg %u", which, s ? s->name : "none", priv_r8(kHullByte), mem_readb(SegPhys(ss) + reg_sp + 6));
    if (!s) return;
    const PhysPt stack = SegPhys(ss) + reg_sp;
    const Bit16u hold = mem_readw(stack + 4);
    const PhysPt h = SegPhys(ds) + hold;
    if (which == 0) {
        if (mem_readb(stack + 6) != s->hull) return;
        mem_writew(h + 0xC, (Bit16u)(s->hold + (mem_readb(h + 0xA) ? s->holdMore : 0)));
        mem_writeb(stack + 6, 0xFF);
        return;
    }
    if (g_hullHeld >= 0 || priv_r8(kHullByte) != s->hull) return;
    mem_writew(h + 0xC, (Bit16u)(s->hold + (which == 2 ? s->holdMore : 0)));
    g_hullHeld = s->hull;
    priv_w8(kHullByte, 0xFF);
}

void hold_end() {
    if (g_hullHeld < 0) return;
    priv_w8(kHullByte, (Bit8u)g_hullHeld);
    g_hullHeld = -1;
}

// The sale.  The dealer's routine ovr286:0CC1(room, hull) does all of it: "you fly one already" if the hull is the
// player's, else the old ship's and its extras' value against the price (ovr286:0C4F, by the hull), the offer's
// conversation with its Accept and Reject -- or "too poor", or "your cargo will not fit" (ovr269:179F) -- and, accepted,
// the hull byte, the extras sold, the credits, and the new ship made (stub273:0025).  The rooms' script calls it for an
// added ship with (id * 16 + hull): here the argument is made the hull, the four conversations' names (DGROUP:08F8
// "buyfight", 0911 "cargo", 0917 "toopoor", 091F "samefght", the Centurion's) are written over with the ship's own
// until the routine's end, the price is the ship's, and "the same" means the same SHIP: a Centurion's owner is sold a
// Stiletto, a Stiletto's owner a Centurion (its value is its price, as the game values its own).
const Bit16u kSaleNames[4] = { 0x08F8, 0x0911, 0x0917, 0x091F };
const char *const kSaleOwn[4] = { "buyfight", "cargo", "toopoor", "samefght" };

void sale_begin() {
    const PhysPt stack = SegPhys(ss) + reg_sp;
    const Bit8u arg = mem_readb(stack + 6);
    const PrivAddedShip *owner = priv_added_ship();
    g_sale = Sale();
    g_sale.active = true;
    g_sale.owner = owner && g_ship == owner->id ? owner->id : 0;
    if (!(arg & 0xF0)) return;
    const PrivAddedShip *s = g_tre.on ? ship_by_id(arg >> 4) : 0;
    bool ok = s != 0;
    for (int k = 0; k < 4 && ok; k++) {
        for (size_t i = 0; i <= strlen(kSaleOwn[k]); i++) if (priv_r8((Bit16u)(kSaleNames[k] + i)) != (Bit8u)kSaleOwn[k][i]) ok = false;
    }
    if (!ok) {
        // (not a ship of ours, or another program's data: the routine is told the hull owned, and says so)
        wclog(1, "Privateer: added ships: a sale of %u could not be made", arg);
        mem_writeb(stack + 6, priv_r8(kHullByte));
        g_sale.ship = g_sale.owner;
        return;
    }
    char names[4][9];
    priv_ship_sale_names(*s, names);
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 9; i++) g_sale.kept[k][i] = priv_r8((Bit16u)(kSaleNames[k] + i));
        for (size_t i = 0; i <= strlen(names[k]); i++) priv_w8((Bit16u)(kSaleNames[k] + i), (Bit8u)names[k][i]);
    }
    g_sale.renamed = true;
    g_sale.ship = s->id;
    mem_writeb(stack + 6, (Bit8u)s->hull);
    wclog(1, "Privateer: added ships: the %s is offered (%ld credits)", s->name, s->price);
}

void sale_end() {
    if (g_sale.renamed) {
        for (int k = 0; k < 4; k++) for (size_t i = 0; i <= strlen(kSaleOwn[k]); i++) priv_w8((Bit16u)(kSaleNames[k] + i), g_sale.kept[k][i]);
    }
    g_sale = Sale();
}

}   // namespace

void priv_ship_sale_names(const PrivAddedShip &ship, char names[4][9]) {
    snprintf(names[0], 9, "buyshp%d", ship.id % 100);       // (for "buyfight")
    snprintf(names[1], 9, "crgo%d", ship.id % 10);          // ("cargo": five letters)
    snprintf(names[2], 9, "poorsh%d", ship.id % 10);        // ("toopoor": seven)
    snprintf(names[3], 9, "samesh%d", ship.id % 100);       // ("samefght")
}

// The saved game's word.  A saved game's first record begins with the hull as a word, and the game writes the high
// byte 0 and reads only the low one (ovr262:0000 and 0040): that byte is the added ship's id.  The game without this
// layer (or with WCPRIVSHIPS=0) loads such a game as the hull's own ship with what the added ship carried.
bool priv_ships_hook(Bit16u ip, Bit16u codeSeg, Bit16u loadSeg) {
    enum { kStub262 = 0x69E2 };
    const PhysPt stack = SegPhys(ss) + reg_sp;
    switch (ip) {
    case kShipsRecordLoad:
        if (codeSeg != (Bit16u)(loadSeg + kStub262)) return false;
        {
            const int id = mem_readb(SegPhys(ds) + (Bit16u)(mem_readw(stack + 4) + 1));
            const PrivAddedShip *s = ship_by_id(id);
            hold_end();
            if (s && mem_readb(SegPhys(ds) + mem_readw(stack + 4)) != s->hull) s = 0;
            if ((s ? s->id : 0) != g_ship) wclog(1, "Privateer: added ships: the game loaded is %s%s", s ? "a " : "one of the game's own ships", s ? s->name : "");
            g_ship = g_tre.on && s ? s->id : 0;
        }
        return true;
    case kShipsRecordStored: {
        // (mov al,[6560] / mov [si+8],al / pop si / pop bp / retf)
        static const Bit8u code[] = { 0x5E, 0x5D, 0xCB };
        static const Bit8u before[] = { 0xA0, 0x60, 0x65, 0x88, 0x44, 0x08 };
        if (!code_is(code, sizeof(code))) return false;
        for (size_t i = 0; i < sizeof(before); i++) if (mem_readb(SegPhys(cs) + reg_ip - sizeof(before) + i) != before[i]) return false;
        const PrivAddedShip *s = priv_added_ship();
        if (s && g_ship == s->id) mem_writeb(SegPhys(ds) + (Bit16u)(reg_si + 1), (Bit8u)s->id);
        return true;
    }
    case kShipsHoldSet: {
        static const Bit8u code[] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x76, 0x06, 0x8A, 0x46, 0x08, 0xB4, 0x00, 0x8B, 0xD8, 0x83, 0xFB, 0x03 };
        if (!code_is(code, sizeof(code))) return false;
        hold_hook(0);
        return true;
    }
    case kShipsHoldPlain:
    case kShipsHoldMore: {
        static const Bit8u code[] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x76, 0x06, 0xC6, 0x44, 0x0A };
        if (!code_is(code, sizeof(code)) || mem_readb(SegPhys(cs) + reg_ip + 10) != (ip == kShipsHoldMore ? 1 : 0)) return false;
        hold_hook(ip == kShipsHoldMore ? 2 : 1);
        return true;
    }
    case kShipsHoldRead: {
        // (the game knows the Centurion's 50 and 75 only -- anything else is its error 7024: ours are told as those)
        static const Bit8u code[] = { 0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0x76, 0x06, 0x8B, 0x7E, 0x08, 0x8A, 0x46, 0x0A };
        if (!code_is(code, sizeof(code))) return false;
        const PrivAddedShip *s = priv_added_ship();
        if (s && mem_readb(stack + 8) == s->hull && s->hull == 2) {
            const Bit16u size = mem_readw(stack + 6);
            if (size == s->hold) mem_writew(stack + 6, 50);
            else if (size == s->hold + s->holdMore) mem_writew(stack + 6, 75);
        }
        return true;
    }
    case kShipsHoldSize: {
        static const Bit8u code[] = { 0x8B, 0xC2, 0x5E, 0x5D, 0xCB };
        if (!code_is(code, sizeof(code))) return false;
        // (a sale asks what the ship it sells holds, before it is anybody's)
        const PrivAddedShip *s = g_sale.active && !g_sale.accepted ? ship_by_id(g_sale.ship) : priv_added_ship();
        if (s && mem_readb(SegPhys(ss) + reg_bp + 8) == s->hull) {
            reg_dx = (Bit16u)(s->hold + (mem_readb(SegPhys(ds) + (Bit16u)(reg_si + 0x28)) ? s->holdMore : 0));
        }
        return true;
    }
    case kShipsSaleEntry: {
        static const Bit8u code[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x86, 0x00, 0x56, 0x57, 0x8B, 0x76, 0x06, 0xC7, 0x46, 0xFE, 0x00, 0x00 };
        if (!code_is(code, sizeof(code))) return false;
        sale_begin();
        return true;
    }
    case kShipsSaleSame: {
        // (cmp al,[bp+8] / jnz +3 / jmp ...: the same hull is "you fly one already")
        static const Bit8u code[] = { 0x3A, 0x46, 0x08, 0x75, 0x03, 0xE9 };
        if (!g_sale.active || !code_is(code, sizeof(code))) return false;
        if (reg_al == mem_readb(SegPhys(ss) + reg_bp + 8) && g_sale.owner != g_sale.ship) reg_al = 0xFF;    // (the same hull, another ship)
        return true;
    }
    case kShipsSalePrice: {
        static const Bit8u code[] = { 0x66, 0x8B, 0x46, 0xFC, 0x66, 0x0F, 0xA4, 0xC2, 0x10, 0xC9, 0xCB };
        if (!g_sale.active || !code_is(code, sizeof(code))) return false;
        const PrivAddedShip *s = ship_by_id((++g_sale.prices & 1) ? g_sale.owner : g_sale.ship);
        if (s && mem_readb(SegPhys(ss) + reg_bp + 6) == s->hull) mem_writed(SegPhys(ss) + (Bit16u)(reg_bp - 4), (Bit32u)s->price);
        return true;
    }
    case kShipsSaleAccept: {
        static const Bit8u code[] = { 0xA0, 0x5E, 0x65, 0x88, 0x46, 0xF0, 0x8A, 0x46, 0x08, 0xA2, 0x5E, 0x65 };
        if (!g_sale.active || !code_is(code, sizeof(code))) return false;
        const PrivAddedShip *s = ship_by_id(g_sale.ship);
        g_sale.accepted = true;
        hold_end();
        g_ship = s ? s->id : 0;
        wclog(1, "Privateer: added ships: bought, %s%s", s ? "a " : "one of the game's own ships", s ? s->name : "");
        return true;
    }
    case kShipsSaleEnd: {
        static const Bit8u code[] = { 0x5F, 0x5E, 0xC9, 0xCB };
        if (!g_sale.active || !code_is(code, sizeof(code))) return false;
        sale_end();
        return true;
    }
    case kShipsHoldPlainEnd:
    case kShipsHoldMoreEnd: {
        static const Bit8u code[] = { 0x5E, 0x5D, 0xCB };
        if (g_hullHeld < 0 || !code_is(code, sizeof(code))) return false;
        hold_end();
        return true;
    }
    }
    return false;
}

// At seg214:03DF, the IFF reader's (reader near, index dword): a table's row sought.  The flight's set-up
// (ovr262:07AF) reads the player's ship's appearance and cockpit from APPRCOCK.IFF's row of the HULL (the call that
// returns to ovr262:0941, told by the bytes there: add sp,6 / push 1 / push 'ASHP').  For an added ship the row is
// its own: the appearance (the ship in space, its mounts and flight numbers by SKELETON.IFF) and the cockpit follow.
void priv_ships_table_seek() {
    const PhysPt stack = SegPhys(ss) + reg_sp;
    if (mem_readw(stack) != 0x0941) return;
    static const Bit8u after[] = { 0x83, 0xC4, 0x06, 0x6A, 0x01, 0x66, 0x68, 'A', 'S', 'H', 'P' };
    const PhysPt ret = ((PhysPt)mem_readw(stack + 2) << 4) + 0x0941;
    for (size_t i = 0; i < sizeof(after); i++) if (mem_readb(ret + i) != after[i]) return;
    const PrivAddedShip *s = priv_added_ship();
    if (!s || !g_tre.on) return;
    const Bit32u row = mem_readd(stack + 6);
    if ((int)row != s->hull) {
        wclog(1, "Privateer: added ships: the flight asks for hull %u's cockpit, not the %s's hull %d: left alone", row, s->name, s->hull);
        return;
    }
    mem_writed(stack + 6, (Bit32u)(g_gameRows + (int)(s - kShips)));
    wclog(1, "Privateer: added ships: the flight's ship is the %s (APPRCOCK.IFF's row %d, not the hull's %u)", s->name, g_gameRows + (int)(s - kShips), row);
}

}
