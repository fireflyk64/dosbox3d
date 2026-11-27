/*
 *  Privateer's added ships' cockpits (wcnet_priv_ships.h, docs/privateer.md section 42): made at run time from the
 *  game's own cockpit files.  The user (2026-11-27): "It would look like a very cramped version of the centurion
 *  cockpit with 2 VDUs instead of one ... manufature at runtime the actual cockpit from the centurion and orion
 *  pieces ... the location of vdus etc can be takend from the stiletto_cp folder it's just the art looks too
 *  cartooney so should come from this game being cut and pasted instead."
 *
 *  None of the game's art is in this program: a ship's recipe is a list of cuts (which rectangle of which of the
 *  game's cockpits goes where) and the places of its instruments.  The recipe's source is
 *  scripts/privships/stilcock.py, which draws it for looking at and writes the table (wcnet_priv_stiletto.h); the
 *  code below does to the player's own files what the script does (their outputs are compared byte for byte:
 *  WCPRIVCKDUMP=<dir> writes this side's).
 *
 *  A cockpit file (DATA\COCKPITS\<name>.IFF, FORM COCK):
 *    FORM FRNT, RITE, BACK, LEFT (and TUR1, TUR2 a turret's): a view -- SHAP its picture (a shape pack of one 320 x
 *      200 shape) and TPLT its template: the spans of every row where the picture has NO pixel.  There the flight's
 *      picture shows: space, and the instruments, which are drawn under the cockpit;
 *    OFFS: the gunsight's rows above the screen's middle (the view's centre);  FONT;
 *    FORM CMFD: FORM AMFD per VDU (INFO: x1, y1, x2, y2, the view, the VDU's number and letter), SOFT;
 *    FORM CHUD: HINF (a colour, a box per view), FORM HSFT (the speeds' texts on the glass -- SSPD, ASPD: INFO a place
 *      from the box's corner and the label -- the target's and nav point's marks, the gunsight);
 *    FORM DIAL: RADR (INFO its box), SHLD (box, the eight bars' places, their art), ENER and FUEL (FORM VIEW: box,
 *      DATA the bar's direction and box, SHAP its art), AUTO (box, the light's two pictures), and -- the Tarsus's
 *      way, on the panel -- SSPD and ASPD (box, DATA the label);
 *    FORM DAMG, FORM CDMG (the damage's sparks: the PAK's pictures and their places).
 *  A shape: four words (its extents right, left, up, down of its origin), then stretches of a row: a word (pixels
 *  * 2, + 1 packed), x, y, the pixels -- packed as pieces of (count * 2 + 1, colour) or (count * 2, colours); a zero
 *  word ends it.  A shape pack: its length, a table of offsets (24 bits), the shapes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "wcnet_priv_ships.h"
#include "wcnet_log.h"

namespace wc {

namespace {

struct PrivCut { int op, src, sx, sy, w, h, dx, dy; };
struct PrivCockpitLayout { int vduLeft[4], vduRight[4], radar[4], shield[4], fuel[4], energy[4], set[4], kps[4], autopilot[4], sight[2]; };
#include "wcnet_priv_stiletto.h"

typedef std::vector<Bit8u> Bytes;
enum { W = 320, H = 200, kNone = -1 };

// ---- IFF ------------------------------------------------------------------------------------------------------------

struct Chunk {
    std::string tag;            // a chunk's tag, or a FORM's type
    bool form;
    Bytes data;
    std::vector<Chunk> kids;
    Chunk() : form(false) {}
    // The n-th child of that name (0: none).
    const Chunk *find(const char *name, int n = 0) const {
        for (size_t i = 0; i < kids.size(); i++) if (kids[i].tag == name && n-- == 0) return &kids[i];
        return 0;
    }
    Chunk *find(const char *name, int n = 0) { return const_cast<Chunk *>(static_cast<const Chunk *>(this)->find(name, n)); }
};

Bit32u be32(const Bit8u *p) { return ((Bit32u)p[0] << 24) | ((Bit32u)p[1] << 16) | ((Bit32u)p[2] << 8) | p[3]; }
Bit32u le32(const Bit8u *p) { return ((Bit32u)p[3] << 24) | ((Bit32u)p[2] << 16) | ((Bit32u)p[1] << 8) | p[0]; }
int le16s(const Bit8u *p) { return (Bit16s)(p[0] | (p[1] << 8)); }
void add16(Bytes &v, int x) { v.push_back((Bit8u)x); v.push_back((Bit8u)(x >> 8)); }
void add32(Bytes &v, Bit32u x) { add16(v, (int)(x & 0xFFFF)); add16(v, (int)(x >> 16)); }
void add32be(Bytes &v, Bit32u x) { v.push_back((Bit8u)(x >> 24)); v.push_back((Bit8u)(x >> 16)); v.push_back((Bit8u)(x >> 8)); v.push_back((Bit8u)x); }

bool iff_parse(const Bytes &d, size_t at, size_t end, std::vector<Chunk> *out, int depth = 0) {
    while (at + 8 <= end) {
        const Bit32u n = be32(&d[at + 4]);
        if (n > end - at - 8 || depth > 8) return false;
        Chunk c;
        if (!memcmp(&d[at], "FORM", 4)) {
            if (n < 4) return false;
            c.form = true;
            c.tag.assign((const char *)&d[at + 8], 4);
            if (!iff_parse(d, at + 12, at + 8 + n, &c.kids, depth + 1)) return false;
        } else {
            c.tag.assign((const char *)&d[at], 4);
            c.data.assign(d.begin() + at + 8, d.begin() + at + 8 + n);
        }
        out->push_back(c);
        at += 8 + n + (n & 1);
    }
    return true;
}

bool iff_root(const Bytes &d, Chunk *root) {
    std::vector<Chunk> top;
    if (d.size() < 12 || !iff_parse(d, 0, d.size(), &top) || top.empty() || !top[0].form) return false;
    *root = top[0];
    return true;
}

void iff_build(const Chunk &c, Bytes *out) {
    if (c.form) {
        Bytes body(c.tag.begin(), c.tag.end());
        for (size_t i = 0; i < c.kids.size(); i++) iff_build(c.kids[i], &body);
        out->insert(out->end(), (const Bit8u *)"FORM", (const Bit8u *)"FORM" + 4);
        add32be(*out, (Bit32u)body.size());
        out->insert(out->end(), body.begin(), body.end());
        return;
    }
    out->insert(out->end(), c.tag.begin(), c.tag.end());
    add32be(*out, (Bit32u)c.data.size());
    out->insert(out->end(), c.data.begin(), c.data.end());
    if (c.data.size() & 1) out->push_back(0);
}

Chunk leaf(const char *tag, const Bytes &data) { Chunk c; c.tag = tag; c.data = data; return c; }
Chunk form(const char *type) { Chunk c; c.tag = type; c.form = true; return c; }

// A descent by names; 0 when any is missing.
const Chunk *path(const Chunk &root, const char *a, const char *b = 0, const char *c = 0) {
    const Chunk *k = root.find(a);
    if (k && b) k = k->find(b);
    if (k && c) k = k->find(c);
    return k;
}

// ---- Shapes ---------------------------------------------------------------------------------------------------------

struct Pixel { int x, y; Bit8u c; };
struct Shape { int ext[4]; std::vector<Pixel> pix; };      // (in the file's order: a row's stretches left to right, rows down)

bool shape_decode(const Bytes &v, size_t at, Shape *out) {
    if (at + 8 > v.size()) return false;
    for (int i = 0; i < 4; i++) out->ext[i] = le16s(&v[at + 2 * i]);
    at += 8;
    out->pix.clear();
    for (;;) {
        if (at + 2 > v.size()) return false;
        const int h = v[at] | (v[at + 1] << 8);
        if (!h) return true;
        if (at + 6 > v.size()) return false;
        int x = le16s(&v[at + 2]);
        const int y = le16s(&v[at + 4]);
        at += 6;
        int n = h >> 1;
        if (!(h & 1)) {
            if (at + n > v.size()) return false;
            for (int i = 0; i < n; i++) { Pixel p = { x + i, y, v[at + i] }; out->pix.push_back(p); }
            at += n;
            continue;
        }
        while (n > 0) {
            if (at + 2 > v.size()) return false;
            const int c = v[at++], r = c >> 1;
            if (!r) return false;
            if (!(c & 1) && at + r > v.size()) return false;
            for (int i = 0; i < r; i++) { Pixel p = { x + i, y, (c & 1) ? v[at] : v[at + i] }; out->pix.push_back(p); }
            at += (c & 1) ? 1 : r;
            x += r; n -= r;
        }
    }
}

// A pack's shapes' offsets.
bool pack_offsets(const Bytes &v, std::vector<size_t> *out) {
    if (v.size() < 8) return false;
    const size_t first = le32(&v[4]) & 0xFFFFFF;
    if (first < 8 || first > v.size() || (first - 4) % 4) return false;
    for (size_t j = 0; j < (first - 4) / 4; j++) out->push_back(le32(&v[4 + 4 * j]) & 0xFFFFFF);
    return true;
}

bool pack_shape(const Bytes &v, size_t index, Shape *out) {
    std::vector<size_t> offs;
    return pack_offsets(v, &offs) && index < offs.size() && shape_decode(v, offs[index], out);
}

// A stretch of a row packed: repeats of 3 or more as (count * 2 + 1, colour), the rest as (count * 2, colours).
void pack_run(const Bit8u *run, int len, Bytes *out) {
    Bytes lit;
    int k = 0;
    while (k <= len) {
        int n = 1;
        if (k < len) while (k + n < len && run[k + n] == run[k] && n < 127) n++;
        if (k == len || n >= 3) {
            for (size_t i = 0; i < lit.size(); i += 127) {
                const size_t part = lit.size() - i < 127 ? lit.size() - i : 127;
                out->push_back((Bit8u)(part * 2));
                out->insert(out->end(), lit.begin() + i, lit.begin() + i + part);
            }
            lit.clear();
            if (k == len) break;
            out->push_back((Bit8u)(n * 2 + 1));
            out->push_back(run[k]);
        } else {
            lit.insert(lit.end(), run + k, run + k + n);
        }
        k += n;
    }
}

// A canvas (kNone: no pixel) from (x0, y0) as a shape whose origin is the canvas's (ox, oy).
void shape_encode(const std::vector<int> &canvas, int w, int h, int ox, int oy, const int ext[4], Bytes *out) {
    for (int i = 0; i < 4; i++) add16(*out, ext[i]);
    Bytes run;
    for (int y = 0; y < h; y++) {
        int x = 0;
        while (x < w) {
            if (canvas[y * w + x] == kNone) { x++; continue; }
            const int x0 = x;
            run.clear();
            while (x < w && canvas[y * w + x] != kNone) run.push_back((Bit8u)canvas[y * w + x++]);
            add16(*out, (int)run.size() * 2 + 1);
            add16(*out, x0 - ox);
            add16(*out, y - oy);
            pack_run(&run[0], (int)run.size(), out);
        }
    }
    add16(*out, 0);
}

void pack_build(const Bytes &shape, Bytes *out) {
    add32(*out, 8 + (Bit32u)shape.size());
    add32(*out, 8);
    out->insert(out->end(), shape.begin(), shape.end());
}

// ---- The recipe -----------------------------------------------------------------------------------------------------

bool view_canvas(const Chunk &cockpit, const char *view, std::vector<int> *canvas) {
    const Chunk *shap = path(cockpit, view, "SHAP");
    Shape s;
    if (!shap || !pack_shape(shap->data, 0, &s)) return false;
    canvas->assign(W * H, kNone);
    for (size_t i = 0; i < s.pix.size(); i++) {
        const Pixel &p = s.pix[i];
        if (p.x >= 0 && p.x < W && p.y >= 0 && p.y < H) (*canvas)[p.y * W + p.x] = p.c;
    }
    return true;
}

bool compose(const PrivCut *cuts, size_t count, const std::vector<int> src[3], std::vector<int> *out) {
    out->assign(W * H, kNone);
    for (size_t k = 0; k < count; k++) {
        const PrivCut &c = cuts[k];
        if (c.src < 0 || c.src > 2) return false;
        int colour = kNone;
        if (c.op == 2) {
            if (c.sx < 0 || c.sx >= W || c.sy < 0 || c.sy >= H) return false;
            colour = src[c.src][c.sy * W + c.sx];
            if (colour == kNone) return false;         // (another release's art: the recipe does not fit it)
        }
        for (int j = 0; j < c.h; j++) {
            for (int i = 0; i < c.w; i++) {
                const int x = c.dx + i, y = c.dy + j;
                if (x < 0 || x >= W || y < 0 || y >= H) continue;
                int &to = (*out)[y * W + x];
                if (c.op == 2) { to = colour; continue; }
                if (c.op == 3) { to = kNone; continue; }
                const int sx = c.sx + i, sy = c.sy + j;
                const int p = sx >= 0 && sx < W && sy >= 0 && sy < H ? src[c.src][sy * W + sx] : kNone;
                if (p != kNone) to = p;
                else if (c.op == 1) to = kNone;
            }
        }
    }
    return true;
}

// A view's template: the spans of every row where the picture has no pixel.
void tplt_build(const std::vector<int> &canvas, Bytes *out) {
    Bytes spans;
    int rows = 0;
    for (int y = 0; y < H; y++) {
        int x = 0;
        while (x < W) {
            if (canvas[y * W + x] != kNone) { x++; continue; }
            const int x0 = x;
            while (x < W && canvas[y * W + x] == kNone) x++;
            add16(spans, x0); add16(spans, y); add16(spans, x - x0);
            rows = y + 1;
        }
    }
    add16(spans, 0xFFFF);
    add16(*out, (int)(12 + spans.size()) / 2);
    add16(*out, 2); add16(*out, W); add16(*out, rows); add16(*out, 0); add16(*out, 0);
    out->insert(out->end(), spans.begin(), spans.end());
}

Bytes rect(int x, int y, int w, int h) { Bytes b; add16(b, x); add16(b, y); add16(b, x + w); add16(b, y + h); return b; }

// Four words moved by (dx, dy).
Bytes moved(const Bit8u *info, int dx, int dy) {
    Bytes b;
    for (int i = 0; i < 4; i++) add16(b, le16s(info + 2 * i) + ((i & 1) ? dy : dx));
    return b;
}

void centre(const Bit8u *info, int *cx, int *cy) {
    *cx = (le16s(info) + le16s(info + 4)) >> 1;
    *cy = (le16s(info + 2) + le16s(info + 6)) >> 1;
}

// A gauge's FORM VIEW moved so that its box's middle is the bay's.
bool gauge(const Chunk *view, const int r[4], Chunk *out) {
    const Chunk *info = view ? view->find("INFO") : 0, *data = view ? view->find("DATA") : 0, *shap = view ? view->find("SHAP") : 0;
    if (!info || !data || !shap || info->data.size() != 8 || data->data.size() != 9) return false;
    int cx, cy;
    centre(&info->data[0], &cx, &cy);
    const int dx = r[0] + r[2] / 2 - cx, dy = r[1] + r[3] / 2 - cy;
    *out = form("VIEW");
    out->kids.push_back(leaf("INFO", moved(&info->data[0], dx, dy)));
    Bytes d(1, data->data[0]);
    const Bytes m = moved(&data->data[1], dx, dy);
    d.insert(d.end(), m.begin(), m.end());
    out->kids.push_back(leaf("DATA", d));
    out->kids.push_back(*shap);
    return true;
}

Chunk amfd(const int r[4], int number, int letter) {
    Bytes b;
    for (int i = 0; i < 4; i++) add16(b, r[i]);
    b.push_back(0); b.push_back((Bit8u)number); b.push_back((Bit8u)letter);
    Chunk f = form("AMFD");
    f.kids.push_back(leaf("INFO", b));
    return f;
}

bool stiletto_cockpit(PrivTreFile file, Bytes *iff, Bytes *pak) {
    static const char *const names[3] = { "FIGHTCK", "TUGCK", "CLUNKCK" };
    Chunk ck[3];
    std::vector<int> src[3];
    for (int i = 0; i < 3; i++) {
        Bytes d;
        if (!file(std::string("..\\..\\DATA\\COCKPITS\\") + names[i] + ".IFF", &d) || !iff_root(d, &ck[i]) || ck[i].tag != "COCK" ||
            !view_canvas(ck[i], "FRNT", &src[i])) return false;
    }
    if (!file("..\\..\\DATA\\COCKPITS\\FIGHTCK.PAK", pak)) return false;
    const Chunk &cent = ck[0], &tug = ck[1], &clunk = ck[2];
    const PrivCockpitLayout &L = kStilettoLayout;

    std::vector<int> canvas;
    if (!compose(kStilettoCuts, sizeof(kStilettoCuts) / sizeof(kStilettoCuts[0]), src, &canvas)) return false;
    Chunk frnt = form("FRNT");
    {
        static const int ext[4] = { W - 1, 0, 0, H - 1 };
        Bytes shape, pack, tplt;
        shape_encode(canvas, W, H, 0, 0, ext, &shape);
        pack_build(shape, &pack);
        tplt_build(canvas, &tplt);
        frnt.kids.push_back(leaf("SHAP", pack));
        frnt.kids.push_back(leaf("TPLT", tplt));
    }

    const Chunk *soft = path(cent, "CMFD", "SOFT"), *hinf = path(cent, "CHUD", "HINF"), *hsftIn = path(cent, "CHUD", "HSFT"),
                *hdamg = path(cent, "CHUD", "DAMG"), *dc = cent.find("DIAL");
    const Chunk *sh = dc ? dc->find("SHLD") : 0, *autoIn = dc ? dc->find("AUTO") : 0;
    const Chunk *rite = cent.find("RITE"), *back = cent.find("BACK"), *left = cent.find("LEFT"), *font = cent.find("FONT"),
                *damg = cent.find("DAMG"), *cdmg = cent.find("CDMG");
    if (!soft || !hinf || hinf->data.size() < 9 || !hsftIn || !hdamg || !sh || !autoIn || !rite || !back || !left || !font || !damg || !cdmg) return false;
    const Chunk *shInfo = sh->find("INFO"), *shData = sh->find("DATA"), *shShap = sh->find("SHAP"), *auInfo = autoIn->find("INFO"), *auShap = autoIn->find("SHAP");
    if (!shInfo || !shData || !shShap || !auInfo || auInfo->data.size() != 8 || !auShap) return false;

    Chunk cmfd = form("CMFD");
    cmfd.kids.push_back(amfd(L.vduLeft, 0x1A, 'l'));
    cmfd.kids.push_back(amfd(L.vduRight, 0x1B, 'r'));
    cmfd.kids.push_back(*soft);

    Chunk chud = form("CHUD");
    {
        Bytes h(hinf->data.begin(), hinf->data.begin() + 9);       // the colour, the forward view's box; no turret's
        h.resize(17, 0);
        chud.kids.push_back(leaf("HINF", h));
        Chunk hsft = form("HSFT");
        const int hx = le16s(&h[1]), hy = le16s(&h[3]);             // (the speeds' places are from the box's corner)
        for (size_t i = 0; i < hsftIn->kids.size(); i++) {
            Chunk k = hsftIn->kids[i];
            if (k.tag == "SSPD" || k.tag == "ASPD") {
                const int *r = k.tag == "SSPD" ? L.set : L.kps;
                Chunk *info = k.find("INFO");
                if (!info || info->data.size() < 4) return false;
                Bytes b; add16(b, r[0] - hx); add16(b, r[1] - hy);
                b.insert(b.end(), info->data.begin() + 4, info->data.end());
                info->data = b;
            }
            hsft.kids.push_back(k);
        }
        chud.kids.push_back(hsft);
        chud.kids.push_back(*hdamg);
    }

    Chunk dial = form("DIAL");
    {
        Chunk radr = form("RADR");
        radr.kids.push_back(leaf("INFO", rect(L.radar[0], L.radar[1], L.radar[2], L.radar[3])));
        dial.kids.push_back(radr);
        Chunk shld = form("SHLD");
        const int scx = L.shield[0] + L.shield[2] / 2, scy = L.shield[1] + L.shield[3] / 2;
        Bytes b; add16(b, scx - 20); add16(b, scy - 18); add16(b, scx + 21); add16(b, scy + 19);
        shld.kids.push_back(leaf("INFO", b));
        shld.kids.push_back(*shData);
        shld.kids.push_back(*shShap);
        dial.kids.push_back(shld);
        Chunk ener = form("ENER"), fuel = form("FUEL"), view;
        if (!gauge(path(clunk, "DIAL", "ENER", "VIEW"), L.energy, &view)) return false;
        ener.kids.push_back(view);
        dial.kids.push_back(ener);
        if (!gauge(path(tug, "DIAL", "FUEL", "VIEW"), L.fuel, &view)) return false;
        fuel.kids.push_back(view);
        dial.kids.push_back(fuel);
        Chunk au = form("AUTO");
        int acx, acy;
        centre(&auInfo->data[0], &acx, &acy);
        au.kids.push_back(leaf("INFO", moved(&auInfo->data[0], L.autopilot[0] + L.autopilot[2] / 2 - acx, L.autopilot[1] + L.autopilot[3] / 2 - acy)));
        au.kids.push_back(*auShap);
        dial.kids.push_back(au);
    }

    Chunk root = form("COCK");
    root.kids.push_back(frnt);
    root.kids.push_back(*rite);
    root.kids.push_back(*back);
    root.kids.push_back(*left);
    Bytes offs; add16(offs, L.sight[0]); add16(offs, L.sight[1]);
    root.kids.push_back(leaf("OFFS", offs));
    root.kids.push_back(*font);
    root.kids.push_back(cmfd);
    root.kids.push_back(chud);
    root.kids.push_back(dial);
    root.kids.push_back(*damg);
    root.kids.push_back(*cdmg);
    iff->clear();
    iff_build(root, iff);
    return true;
}

bool read_host(const std::string &name, Bytes *out) {
    FILE *f = fopen(name.c_str(), "rb");
    if (!f) return false;
    out->clear();
    Bit8u buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->insert(out->end(), buf, buf + n);
    fclose(f);
    return true;
}

void dump_host(const std::string &leafName, const Bytes &d) {
    const char *dir = getenv("WCPRIVCKDUMP");       // (a rig's: what was made, for comparing with the script's)
    if (!dir) return;
    if (FILE *f = fopen((std::string(dir) + "/" + leafName).c_str(), "wb")) { if (!d.empty()) fwrite(&d[0], 1, d.size(), f); fclose(f); }
}

}   // namespace

bool priv_ship_cockpit(const PrivAddedShip &ship, PrivTreFile file, std::vector<Bit8u> *iff, std::vector<Bit8u> *pak) {
    // WCPRIVCKDIR=<dir>: <cockpit>.IFF and .PAK from there (a rig's: scripts/privships/stilcock.py --files, a recipe
    // tried before it is built in)
    if (const char *dir = getenv("WCPRIVCKDIR")) {
        if (read_host(std::string(dir) + "/" + ship.cockpit + ".IFF", iff) && read_host(std::string(dir) + "/" + ship.cockpit + ".PAK", pak)) return true;
    }
    if (strcmp(ship.cockpit, "STILCK") != 0 || !stiletto_cockpit(file, iff, pak)) return false;
    dump_host(std::string(ship.cockpit) + ".IFF", *iff);
    dump_host(std::string(ship.cockpit) + ".PAK", *pak);
    return true;
}

// The ship's appearance with a picture for the weapons display (FORM MFDS's WEAP).  A ship no player flew has none,
// and the display reads it (the game's error 150E): its own target-display drawing -- the first four parts: the two
// halves, the body, the engines -- as one shape, nose up, in the colour the Centurion's picture uses most.
// False: the appearance is good as it is (it has one), or cannot be read.
bool priv_ship_appearance(const PrivAddedShip &ship, PrivTreFile file, std::vector<Bit8u> *iff) {
    if (const char *dir = getenv("WCPRIVCKDIR")) {
        if (read_host(std::string(dir) + "/" + ship.appearance + ".IFF", iff)) return true;
    }
    Bytes d, cd;
    Chunk root, croot;
    if (!file(std::string("..\\..\\DATA\\APPEARNC\\") + ship.appearance + ".IFF", &d) || !iff_root(d, &root)) return false;
    Chunk *bm = root.find("BM3D");
    Chunk *mfds = bm ? bm->find("MFDS") : 0;
    const Chunk *targ = mfds ? mfds->find("TARG") : 0;
    if (!targ || mfds->find("WEAP")) return false;
    if (!file("..\\..\\DATA\\APPEARNC\\FIGHTER.IFF", &cd) || !iff_root(cd, &croot)) return false;
    const Chunk *cweap = path(croot, "BM3D", "MFDS", "WEAP");
    Shape cs;
    if (!cweap || !pack_shape(cweap->data, 0, &cs) || cs.pix.empty()) return false;
    int used[256] = {0}, colour = 0;
    for (size_t i = 0; i < cs.pix.size(); i++) used[cs.pix[i].c]++;
    for (int i = 1; i < 256; i++) if (used[i] > used[colour]) colour = i;

    int ext[4] = {0, 0, 0, 0};
    std::vector<Pixel> all;
    for (int part = 0; part < 4; part++) {
        Shape s;
        if (!pack_shape(targ->data, part, &s)) return false;
        for (int i = 0; i < 4; i++) if (part == 0 || s.ext[i] > ext[i]) ext[i] = s.ext[i];
        all.insert(all.end(), s.pix.begin(), s.pix.end());
    }
    // (nose up: y turned over, the extents up and down exchanged)
    const int flipped[4] = { ext[0], ext[1], ext[3], ext[2] };
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    for (size_t i = 0; i < all.size(); i++) {
        all[i].y = -all[i].y;
        if (i == 0 || all[i].x < x0) x0 = all[i].x;
        if (i == 0 || all[i].x > x1) x1 = all[i].x;
        if (i == 0 || all[i].y < y0) y0 = all[i].y;
        if (i == 0 || all[i].y > y1) y1 = all[i].y;
    }
    if (all.empty() || x1 - x0 > 400 || y1 - y0 > 400) return false;
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<int> canvas(w * h, kNone);
    for (size_t i = 0; i < all.size(); i++) canvas[(all[i].y - y0) * w + (all[i].x - x0)] = colour;
    Bytes shape, pack;
    shape_encode(canvas, w, h, -x0, -y0, flipped, &shape);
    pack_build(shape, &pack);
    mfds->kids.push_back(leaf("WEAP", pack));
    iff->clear();
    iff_build(root, iff);
    dump_host(std::string(ship.appearance) + ".IFF", *iff);
    return true;
}

}
