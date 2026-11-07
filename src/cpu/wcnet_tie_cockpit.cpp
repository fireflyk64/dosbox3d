/*
 *  TIE Fighter's Imperial shuttle cockpit, made from the player's own gunboat cockpit (docs/tiefighter.md, step 9; the
 *  user, 2026-11-06: "modify the gunboat cockpit to work for the imperial shuttle... the two strut bars should curve
 *  in rather than out making the edge of an A character rather than the V that we see... Make sure that the edges are
 *  done with instructions and code so we don't include the gunboat cockpit art in the distribution").
 *
 *  The game has no cockpit for the shuttle: a craft's cockpit is <cockpit dir><its spec's internal name>.INT, and the
 *  shuttle's spec (16) has no name.  So, when the player flies a shuttle, just before the flight loads its craft
 *  (fediskio_loadspecies), the emulator:
 *
 *  - names the shuttle's spec SHUTTLE (9 bytes of the spec table);
 *  - writes CP640\SHUTTLE.INT and CP320\SHUTTLE.INT beside the game's own: the gunboat's description with one change,
 *    its forward view SHUTFWD.LFD instead of GUNBT12.LFD.  Every other view, the instruments' places and the parts
 *    (GUNBOATP.PNL) stay the gunboat's own files, read where they are: the gauges, scopes and displays all work as
 *    in the gunboat;
 *  - writes SHUTFWD.LFD, the gunboat's forward view changed by the code below: its two struts, a V from the top
 *    corners down to the dashboard, are taken out, and two legs of an A are drawn from the struts' own pixels, leaning
 *    in towards the top, pushed out until their lower ends are just past the screen's sides, with a frame bar along
 *    the top that the scopes hang from (the stolen shuttle's cockpit in Return of the Jedi);
 *  - gives the targeting display the gunboat's mask (the game picks one by the spec's number: the shuttle would get
 *    the T/F's), by writing the gunboat's 480 (and 200) bytes over the default's for as long as a shuttle flies.
 *
 *  Nothing of the gunboat's art is in this program: only how to change it.  The game's files stay as they are.
 *
 *  A view's LFD is three resources, read in order: PANL (the picture: a sprite, drawn once when the view comes up,
 *  color 253 left out), MASK (for each row of the 3-D view's depth, the runs where space shows and where the panel
 *  does) and PLTT (the panel's 64 colors).
 *
 *  WCNET_TIE_FLY_AS=<craft type> (17 the shuttle; the mission files' numbers) flies the player's flight group in
 *  that craft instead, for the rigs (and the first half of the solo missions' pairings, step 9).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#include "dosbox.h"
#include "dos_inc.h"
#include "../dos/drives.h"
#include "mem.h"
#include "wcnet_tie.h"
#include "wcnet_log.h"

namespace wc {

namespace {

// The program's tables (image addresses; OpenTIE's map, checked against Z_TIE__.EXE).
enum {
    kSpecData = 0xC7AB4,            // SpecData spec_data[69], 0xEC each: +0x2C internal_name[9]
    kSpecSize = 0xEC,
    kSpecName = 0x2C,
    kSpeciesTable = 0xCBBC4,        // SpeciesEntry species_table[], 0x16 each: +1 load_flags, +0x13 spec_num
    kSpeciesSize = 0x16,
    kSpeciesConvert = 0xC1834,      // a mission's craft type -> the species
    kShuttleType = 17,              // the mission files' craft type, and its species
    kShuttleSpec = 16,
    kGunboatSpec = 15,
    kCmd640Mask = 0xC66C3,          // the targeting display's masks: the default (480 bytes), the gunboat's
    kGunboatCmd640Mask = 0xC6C63,
    kCmdMask = 0xC61C3,             // ... and at 320x200 (200 bytes)
    kGunboatCmdMask = 0xC628B,
};
// The craft whose cockpits are made from the gunboat's: the shuttle (step 9) and the Z-95 Headhunter (step 14: the
// Combat Chamber's challenge; its spec, 13, is named XWING as the R-41's is, a cockpit the game never had).
enum { kForwardShuttle, kForwardZ95 };
struct Derived {
    int spec;
    const char *name;           // the spec's internal name: <cockpit dir>\<name>.INT
    const char *forward;        // its forward view's LFD
    int kind;
    const char *parts;          // its parts file (<name>.PNL) with its own craft's icon; NULL: the gunboat's parts
    bool made;
};
Derived g_derived[] = {
    { kShuttleSpec, "SHUTTLE", "SHUTFWD", kForwardShuttle, NULL, false },
    { 13, "Z95", "Z95FWD", kForwardZ95, "Z95P", false },
};

// -- the files' formats ---------------------------------------------------------------------------------------------

struct Resource {
    char type[5];
    char name[9];
    std::vector<Bit8u> body;
};

bool read_file(const std::string &path, std::vector<Bit8u> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    out.clear();
    Bit8u buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    fclose(f);
    return true;
}

bool write_file(const std::string &path, const std::vector<Bit8u> &data) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

Bit32u le32(const Bit8u *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((Bit32u)p[3] << 24); }

// An LFD: 16-byte heads (type, name, size) each followed by its body.
bool parse_lfd(const std::vector<Bit8u> &d, std::vector<Resource> &out) {
    out.clear();
    for (size_t i = 0; i + 16 <= d.size();) {
        Resource r;
        memcpy(r.type, &d[i], 4);
        r.type[4] = 0;
        memcpy(r.name, &d[i + 4], 8);
        r.name[8] = 0;
        const Bit32u size = le32(&d[i + 12]);
        if (i + 16 + size > d.size()) {
            return false;
        }
        r.body.assign(d.begin() + i + 16, d.begin() + i + 16 + size);
        out.push_back(r);
        i += 16 + size;
    }
    return !out.empty();
}

std::vector<Bit8u> write_lfd(const std::vector<Resource> &res) {
    std::vector<Bit8u> out;
    for (size_t k = 0; k < res.size(); k++) {
        Bit8u head[16] = { 0 };
        memcpy(head, res[k].type, 4);
        memcpy(head + 4, res[k].name, strnlen(res[k].name, 8));
        const Bit32u size = (Bit32u)res[k].body.size();
        head[12] = size & 0xff;
        head[13] = (size >> 8) & 0xff;
        head[14] = (size >> 16) & 0xff;
        head[15] = size >> 24;
        out.insert(out.end(), head, head + 16);
        out.insert(out.end(), res[k].body.begin(), res[k].body.end());
    }
    return out;
}

const Resource *find_res(const std::vector<Resource> &res, const char *type) {
    for (size_t k = 0; k < res.size(); k++) {
        if (!memcmp(res[k].type, type, 4)) {
            return &res[k];
        }
    }
    return NULL;
}

const Bit8u kSkip = 253;           // the color a cockpit's picture leaves out

// A cockpit picture (the game's sprite: 0..250 a run of 1..4 of the base color plus op>>2, 251 a new base color,
// 252 a run of a color and the next one in turn, 253 a run of 1..256 of one color, 254 the row's end, 255 the
// picture's) into w x h pixels, kSkip where it draws nothing.  The rows it has.
int decode_panl(const std::vector<Bit8u> &d, int w, int h, std::vector<Bit8u> &px) {
    px.assign((size_t)w * h, kSkip);
    int x = 0, y = 0;
    Bit8u base = 0;
    size_t p = 0;
    while (p < d.size() && y < h) {
        const Bit8u op = d[p++];
        if (op < 251 || op == 253) {
            int n;
            Bit8u c;
            if (op == 253) {
                if (p + 2 > d.size()) break;
                n = d[p] + 1;
                c = d[p + 1];
                p += 2;
            } else {
                c = (Bit8u)((op >> 2) + base);
                n = (op & 3) + 1;
            }
            for (int k = 0; k < n; k++, x++) {
                if (x >= 0 && x < w) px[(size_t)y * w + x] = c;
            }
        } else if (op == 251) {
            if (p >= d.size()) break;
            base = d[p++];
        } else if (op == 252) {
            if (p + 2 > d.size()) break;
            const Bit8u c = d[p];
            const int n = d[p + 1] + 1;
            p += 2;
            for (int k = 0; k < n; k++, x++) {
                if (x >= 0 && x < w) px[(size_t)y * w + x] = (Bit8u)(c + (k & 1));
            }
        } else if (op == 255) {
            return y + 1;
        } else {
            y++;
            x = 0;
        }
    }
    return y;
}

std::vector<Bit8u> encode_panl(const std::vector<Bit8u> &px, int w, int rows) {
    std::vector<Bit8u> out;
    for (int y = 0; y < rows; y++) {
        const Bit8u *row = &px[(size_t)y * w];
        int end = w;
        while (end > 0 && row[end - 1] == kSkip) end--;     // (a row's trailing nothing is left out)
        for (int x = 0; x < end;) {
            const Bit8u c = row[x];
            int n = 1;
            while (x + n < end && row[x + n] == c && n < 256) n++;
            if (c <= 61 && n <= 4) {
                out.push_back((Bit8u)((c << 2) | (n - 1)));   // (the base color stays 0: the game starts it there)
            } else {
                out.push_back(253);
                out.push_back((Bit8u)(n - 1));
                out.push_back(c);
            }
            x += n;
        }
        out.push_back(y + 1 < rows ? 254 : 255);
    }
    if (!rows) out.push_back(255);
    return out;
}

// A view's mask: for each row, a signed state byte (not negative: space shows first) and the runs' lengths, the
// state turning at each (a length past 255 is 0, then the rest less 256; past 511, 0, 0, the rest less 512).
bool decode_mask(const std::vector<Bit8u> &d, int w, int depth, std::vector<Bit8u> &vis) {
    vis.assign((size_t)w * depth, 0);
    size_t p = 0;
    for (int y = 0; y < depth; y++) {
        if (p >= d.size()) return true;              // (a short mask: the rest is panel)
        int st = (Bit8s)d[p++];
        for (int x = 0; x < w;) {
            if (p >= d.size()) return false;
            int v = d[p++];
            if (!v) {
                if (p >= d.size()) return false;
                v = d[p++];
                if (!v) {
                    if (p >= d.size()) return false;
                    v = d[p++] + 512;
                } else {
                    v += 256;
                }
            }
            for (int k = 0; k < v && x + k < w; k++) {
                if (st >= 0) vis[(size_t)y * w + x + k] = 1;
            }
            x += v;
            st = -st;
        }
    }
    return true;
}

// (A run of exactly 256 has no encoding: its last pixel goes to the next run, the state of one pixel changed.)
std::vector<Bit8u> encode_mask(std::vector<Bit8u> &vis, int w, int depth) {
    std::vector<Bit8u> out;
    for (int y = 0; y < depth; y++) {
        Bit8u *row = &vis[(size_t)y * w];
        out.push_back(row[0] ? 1 : 0xff);
        for (int x = 0; x < w;) {
            int n = 1;
            while (x + n < w && row[x + n] == row[x]) n++;
            if (n == 256) {
                n = 255;
                row[x + 255] = !row[x];
            }
            if (n < 256) {
                out.push_back((Bit8u)n);
            } else if (n < 512) {
                out.push_back(0);
                out.push_back((Bit8u)(n - 256));
            } else {
                out.push_back(0);
                out.push_back(0);
                out.push_back((Bit8u)(n - 512));
            }
            x += n;
        }
    }
    return out;
}

// -- the shuttle's forward view ---------------------------------------------------------------------------------------

struct Run {
    int a, b;   // [a, b)
};

struct Fit {
    double k, c;
    double at(double y) const { return k * y + c; }
};

Fit fit_line(const std::vector<double> &ys, const std::vector<double> &vs) {
    double n = (double)ys.size(), sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < ys.size(); i++) {
        sx += ys[i];
        sy += vs[i];
        sxx += ys[i] * ys[i];
        sxy += ys[i] * vs[i];
    }
    Fit f;
    const double den = n * sxx - sx * sx;
    f.k = den ? (n * sxy - sx * sy) / den : 0;
    f.c = n ? (sy - f.k * sx) / n : 0;
    return f;
}

struct Strut {
    std::vector<int> rows;      // its free stretch: consecutive rows, top down
    std::vector<Run> runs;      // its run on each of them
    Fit middle, width;
    int first() const { return rows.front(); }
    int last() const { return rows.back(); }
};

class Forward {
public:
    Forward(std::vector<Bit8u> &px, std::vector<Bit8u> &vis, int w, int h, int depth)
        : px_(px), vis_(vis), orig_(px), w_(w), h_(h), d_(depth) {}

    bool visible(int x, int y) const { return y < d_ && vis_[(size_t)y * w_ + x]; }

    // The hidden runs of a row with space on both sides.
    std::vector<Run> free_runs(int y) const {
        std::vector<Run> out;
        for (int x = 0; x < w_;) {
            const bool v = visible(x, y);
            int e = x;
            while (e < w_ && visible(e, y) == v) e++;
            if (!v && x > 0 && e < w_) {
                Run r = { x, e };
                out.push_back(r);
            }
            x = e;
        }
        return out;
    }

    // A strut, left of the middle or right of it: the longest stretch of consecutive rows on which a free run of
    // some width moves smoothly from row to row.
    bool find_strut(bool left, Strut &s) const {
        const int minWidth = 20 * w_ / 640, tolerance = 20 * w_ / 640;
        std::vector<int> best, cur;
        std::vector<Run> bestRuns, curRuns;
        for (int y = 0; y < d_; y++) {
            const std::vector<Run> rs = free_runs(y);
            const Run *found = NULL;
            for (size_t k = 0; k < rs.size(); k++) {
                if (rs[k].b - rs[k].a < minWidth) continue;
                if (left ? rs[k].b <= w_ / 2 : rs[k].a >= w_ / 2) {
                    found = &rs[k];         // (the last of a side's: the right one is beyond the dashboard's hood)
                }
            }
            if (found && !cur.empty() && cur.back() == y - 1 &&
                abs((found->a + found->b) - (curRuns.back().a + curRuns.back().b)) <= tolerance) {
                cur.push_back(y);
                curRuns.push_back(*found);
            } else {
                cur.clear();
                curRuns.clear();
                if (found) {
                    cur.push_back(y);
                    curRuns.push_back(*found);
                }
            }
            if (cur.size() > best.size()) {
                best = cur;
                bestRuns = curRuns;
            }
        }
        if (best.size() < (size_t)(d_ / 5)) {
            return false;
        }
        s.rows = best;
        s.runs = bestRuns;
        std::vector<double> ys, mids, widths;
        for (size_t i = 0; i < best.size(); i++) {
            ys.push_back(best[i]);
            mids.push_back((bestRuns[i].a + bestRuns[i].b) / 2.0);
            widths.push_back(bestRuns[i].b - bestRuns[i].a);
        }
        s.middle = fit_line(ys, mids);
        s.width = fit_line(ys, widths);
        return true;
    }

    // The color of space's pixels on a row of the picture (they are drawn over; the commonest).
    Bit8u window_color(int y) const {
        unsigned count[256] = { 0 };
        for (int x = 0; x < w_; x++) {
            if (visible(x, y)) count[px_[(size_t)y * w_ + x]]++;
        }
        int best = 0;
        for (int c = 1; c < 256; c++) {
            if (count[c] > count[best]) best = c;
        }
        return (Bit8u)best;
    }

    void open(int x, int y, Bit8u color) {
        vis_[(size_t)y * w_ + x] = 1;
        px_[(size_t)y * w_ + x] = color;
    }
    void close(int x, int y, Bit8u color) {
        vis_[(size_t)y * w_ + x] = 0;
        px_[(size_t)y * w_ + x] = color;
    }

    struct Scope {
        int x, y, r;
    };

    bool in_scope(const std::vector<Scope> &scopes, int x, int y) const {
        for (size_t k = 0; k < scopes.size(); k++) {
            const int dx = x - scopes[k].x, dy = y - scopes[k].y;
            if (dx * dx + dy * dy <= scopes[k].r * scopes[k].r) return true;
        }
        return false;
    }

    // A scope's disk: its middle (the instrument's place) and the run around it on its row.
    Scope scope_at(int cx, int cy) const {
        Scope s = { cx, cy, 0 };
        if (cy < 0 || cy >= d_ || cx < 0 || cx >= w_) return s;
        int a = cx, b = cx;
        while (a > 0 && !visible(a - 1, cy)) a--;
        while (b < w_ - 1 && !visible(b + 1, cy)) b++;
        s.r = (cx - a > b - cx ? cx - a : b - cx) + 2;
        return s;
    }

    // Out with a strut: its band (fitted middle and width, a margin more) from the top down, the scopes' disks kept;
    // below its free stretch, its own run while it is a strut's width (then it has met the dashboard).
    int remove(const Strut &s, const std::vector<Scope> &scopes, int margin) {
        int removed = 0;
        for (int y = 0; y < d_; y++) {
            const double c = s.middle.at(y), hw = s.width.at(y) / 2 + margin;
            const int a = (int)floor(c - hw), b = (int)ceil(c + hw) + 1;
            const Bit8u win = window_color(y);
            if (y > s.last()) {
                const int mid = (int)lround(c);
                if (mid < 0 || mid >= w_ || visible(mid, y)) break;
                int ra = mid, rb = mid;
                while (ra > 0 && !visible(ra - 1, y)) ra--;
                while (rb < w_ - 1 && !visible(rb + 1, y)) rb++;
                if (rb - ra + 1 > 2 * hw) break;
                for (int x = ra; x <= rb; x++) {
                    open(x, y, win);
                    removed++;
                }
                continue;
            }
            for (int x = a < 0 ? 0 : a; x < b && x < w_; x++) {
                if (visible(x, y) || in_scope(scopes, x, y)) continue;
                open(x, y, win);
                removed++;
            }
        }
        return removed;
    }

    // What the struts left: small hidden pieces, touching nothing else, all of them near a strut's band.
    int clean(const std::vector<const Strut *> &struts, int margin) {
        std::vector<Bit8u> seen((size_t)w_ * d_, 0);
        std::vector<int> stack, comp;
        const size_t small = (size_t)(60 * w_ / 640);
        int cleaned = 0;
        for (int y0 = 0; y0 < d_; y0++) {
            for (int x0 = 0; x0 < w_; x0++) {
                if (visible(x0, y0) || seen[(size_t)y0 * w_ + x0]) continue;
                stack.assign(1, y0 * w_ + x0);
                seen[(size_t)y0 * w_ + x0] = 1;
                comp.clear();
                bool big = false;
                while (!stack.empty()) {
                    const int at = stack.back();
                    stack.pop_back();
                    comp.push_back(at);
                    if (comp.size() > small) big = true;
                    const int x = at % w_, y = at / w_;
                    const int nx[4] = { x + 1, x - 1, x, x }, ny[4] = { y, y, y + 1, y - 1 };
                    for (int k = 0; k < 4; k++) {
                        if (nx[k] < 0 || nx[k] >= w_ || ny[k] < 0 || ny[k] >= d_) {
                            big = true;
                            continue;
                        }
                        const size_t i = (size_t)ny[k] * w_ + nx[k];
                        if (!vis_[i] && !seen[i]) {
                            seen[i] = 1;
                            stack.push_back(ny[k] * w_ + nx[k]);
                        }
                    }
                }
                if (big) continue;
                bool near = true;
                for (size_t i = 0; i < comp.size() && near; i++) {
                    const int x = comp[i] % w_, y = comp[i] / w_;
                    near = false;
                    for (size_t k = 0; k < struts.size(); k++) {
                        const double reach = struts[k]->width.at(y) / 2 + 3 * margin;
                        if (fabs(x - struts[k]->middle.at(y)) <= reach) near = true;
                    }
                }
                if (!near) continue;
                for (size_t i = 0; i < comp.size(); i++) {
                    const int x = comp[i] % w_, y = comp[i] / w_;
                    open(x, y, window_color(y));
                }
                cleaned += (int)comp.size();
            }
        }
        return cleaned;
    }

    // A leg's width at its foot, as leg() makes it: the source strut's last rows, narrowed or widened by the slants.
    int leg_width(const Strut &src, double lean) const {
        const int y1 = src.last() - 2 * h_ / 480;
        size_t i = 0;
        while (i < src.rows.size() && src.rows[i] < y1) i++;
        if (i >= src.rows.size()) i = src.rows.size() - 1;
        const double ratio = sqrt(1 + lean * lean) / sqrt(1 + src.middle.k * src.middle.k);
        const int nw = (int)lround((src.runs[i].b - src.runs[i].a) * ratio);
        return nw < 4 ? 4 : nw;
    }

    // In with a leg: from its foot (its lowest row) up to the top row, its middle on the line between, each row a
    // row of the source strut (the one leaning the same way), the strut's length stretched over the leg's and its
    // runs narrowed or widened by the slants (a leg as thick as the strut).  Drawn only where space shows: it goes
    // behind the dashboard and the scopes.
    void leg(const Strut &src, double footX, double footY, double topX) {
        const int y0 = src.first() + 4 * h_ / 480, y1 = src.last() - 2 * h_ / 480;
        if (y1 <= y0) return;
        const double slSrc = src.middle.k, slNew = (footX - topX) / footY;
        const double ratio = sqrt(1 + slNew * slNew) / sqrt(1 + slSrc * slSrc);
        for (int y = 0; y < d_ && y < footY + 40 * h_ / 480; y++) {
            const double t = y / footY < 1 ? y / footY : 1;
            int sy = (int)(y0 + (y1 - y0) * t);
            size_t i = 0;
            while (i < src.rows.size() && src.rows[i] < sy) i++;
            if (i >= src.rows.size()) i = src.rows.size() - 1;
            sy = src.rows[i];
            const Run r = src.runs[i];
            const int sw = r.b - r.a;
            int nw = (int)lround(sw * ratio);
            if (nw < 4) nw = 4;
            const double cx = topX + (footX - topX) * y / footY;
            const int x0 = (int)lround(cx - nw / 2.0);
            for (int k = 0; k < nw; k++) {
                const int x = x0 + k;
                if (x < 0 || x >= w_ || !visible(x, y)) continue;
                int from = r.a + k * sw / nw;
                if (from > r.b - 1) from = r.b - 1;
                close(x, y, orig_[(size_t)sy * w_ + from]);
            }
        }
    }

    // The frame's bar along the top, behind the scopes: each column a row of a strut turned on its side (the rows
    // taken in turn along the bar, so the rivets come along).
    void bar(const Strut &src, int thick) {
        const int n = (int)src.rows.size() - 12;
        if (n <= 0) return;
        for (int x = 0; x < w_; x++) {
            const size_t i = 4 + (size_t)((x * 3 / 2) % n);
            const int sy = src.rows[i];
            const Run r = src.runs[i];
            const int sw = r.b - r.a;
            for (int y = 0; y < thick && y < d_; y++) {
                if (!visible(x, y)) continue;
                int from = r.a + y * sw / thick;
                if (from > r.b - 1) from = r.b - 1;
                close(x, y, orig_[(size_t)sy * w_ + from]);
            }
        }
    }

    // The Z-95's arch (step 14): an ellipse's upper half from the dashboard's sides over the top, its legs going on
    // down to `legs`, `thick` across, each pixel of the band a pixel of a strut's row across its width (the rows taken
    // in turn along the arch), drawn where space shows.
    void arch(const Strut &src, double cx, double base, double ax, double top, double legs, double thick) {
        const int n = (int)src.rows.size() - 16;
        if (n <= 0) return;
        const double by = base - top;
        for (int y = 0; y <= (int)legs && y < d_; y++) {
            for (int x = 0; x < w_; x++) {
                if (!visible(x, y)) continue;
                const double dx = (x - cx) / ax, dy = y < base ? (base - y) / by : 0.0;
                const double rr = sqrt(dx * dx + dy * dy);
                double g = sqrt(dx * dx / (ax * ax) + dy * dy / (by * by));
                if (g <= 0) g = 1e-9;
                const double dist = (rr - 1.0) / g;
                if (fabs(dist) > thick / 2) continue;
                const double ang = fabs(atan2(dy, dx));
                const size_t i = 4 + (size_t)((int)(ang * 40) % n);
                const Run r = src.runs[i];
                const int sw = r.b - r.a;
                int k = (int)((dist / thick + 0.5) * (sw - 1));
                if (k < 0) k = 0;
                if (k > sw - 1) k = sw - 1;
                close(x, y, orig_[(size_t)src.rows[i] * w_ + r.a + k]);
            }
        }
    }

    // An n x n box from (x, y) opened to space where the panel hides it (the picture's own lights for guns a craft
    // does not have); the pixels opened.
    int open_box(int x, int y, int n) {
        int opened = 0;
        for (int j = y; j < y + n && j < d_; j++) {
            for (int i = x; i < x + n && i < w_; i++) {
                if (i < 0 || j < 0 || visible(i, j)) continue;
                open(i, j, window_color(j));
                opened++;
            }
        }
        return opened;
    }

    // The first row from y down where the panel hides column x (the depth when space shows to the view's end).
    int floor_below(int x, int y) const {
        if (x < 0 || x >= w_) return d_;
        while (y < d_ && visible(x, y)) y++;
        return y;
    }

    // The Z-95's long nose (step 14): a trapezoid from the dashboard's middle (y0, w0 wide) towards the horizon (y1,
    // w1), shaded by `greys` (the struts' own, darkest first): lit along its top, darker at its edges and its far end,
    // a seam along its ridge, two panel lines and five seams across; drawn where space shows (behind the dashboard).
    void nose(const std::vector<Bit8u> &greys, double cx, double y0, double y1, double w0, double w1) {
        if (greys.size() < 2) return;
        for (int y = (int)y1; y <= (int)y0 && y < d_; y++) {
            const double t = (y - y1) / (y0 - y1), hw = (w1 + (w0 - w1) * t) / 2;
            for (int x = (int)(cx - hw); x <= (int)(cx + hw); x++) {
                if (x < 0 || x >= w_ || !visible(x, y)) continue;
                const double u = (x - cx) / hw;
                double shade = 0.85 - 0.55 * pow(fabs(u), 1.6) + 0.12 * t;
                if (fabs(u) < 0.06) shade -= 0.25;
                if (fabs(fabs(u) - 0.62) < 0.035) shade -= 0.2;
                if ((int)((1 - t) * 5) != (int)((1 - t + 0.03) * 5)) shade -= 0.15;
                int k = (int)(shade * (double)(greys.size() - 1));
                if (k < 0) k = 0;
                if (k > (int)greys.size() - 1) k = (int)greys.size() - 1;
                close(x, y, greys[(size_t)k]);
            }
        }
    }

    // The colors of a strut's pixels, darkest first by the panel's palette (PLTT: its first and last index, then 8-bit
    // RGB), those neither near black nor near white.
    std::vector<Bit8u> greys(const Strut &src, const std::vector<Bit8u> &pltt) const {
        bool seen[256] = { false };
        std::vector<Bit8u> out;
        if (pltt.size() < 2) return out;
        const int first = pltt[0], last = pltt[1];
        for (size_t i = 0; i < src.rows.size(); i++) {
            for (int x = src.runs[i].a; x < src.runs[i].b; x++) {
                const Bit8u c = orig_[(size_t)src.rows[i] * w_ + x];
                if (seen[c] || c < first || c > last || 2 + (size_t)(c - first) * 3 + 2 >= pltt.size()) continue;
                seen[c] = true;
                const Bit8u *rgb = &pltt[2 + (size_t)(c - first) * 3];
                const double lum = 0.3 * rgb[0] + 0.59 * rgb[1] + 0.11 * rgb[2];
                if (lum > 40 && lum < 235) out.push_back(c);
            }
        }
        for (size_t i = 1; i < out.size(); i++) {      // (by luminance, insertion: a few dozen colors)
            for (size_t j = i; j > 0; j--) {
                const Bit8u *a = &pltt[2 + (size_t)(out[j - 1] - first) * 3], *b = &pltt[2 + (size_t)(out[j] - first) * 3];
                if (0.3 * a[0] + 0.59 * a[1] + 0.11 * a[2] <= 0.3 * b[0] + 0.59 * b[1] + 0.11 * b[2]) break;
                std::swap(out[j - 1], out[j]);
            }
        }
        return out;
    }

private:
    std::vector<Bit8u> &px_;
    std::vector<Bit8u> &vis_;
    const std::vector<Bit8u> orig_;
    int w_, h_, d_;
};

Bit16u le16(const Bit8u *p) { return (Bit16u)(p[0] | (p[1] << 8)); }

// The shuttle's forward view from the gunboat's (its LFD's bytes), at a screen w x h whose 3-D view is `depth` rows;
// `scopes`: the two scopes' middles (the gunboat's instruments 0 and 1).  False: the struts were not found.
bool make_forward(const std::vector<Bit8u> &gunboat, int w, int h, int depth, const int scopes[4],
                  std::vector<Bit8u> &out, std::string &why) {
    std::vector<Resource> res;
    if (!parse_lfd(gunboat, res)) {
        why = "not an LFD";
        return false;
    }
    const Resource *panl = find_res(res, "PANL"), *mask = find_res(res, "MASK");
    if (!panl || !mask || !find_res(res, "PLTT")) {
        why = "no PANL, MASK or PLTT";
        return false;
    }
    std::vector<Bit8u> px, vis;
    const int rows = decode_panl(panl->body, w, h, px);
    if (rows < depth || !decode_mask(mask->body, w, depth, vis)) {
        why = "the picture or its mask did not read";
        return false;
    }
    Forward f(px, vis, w, h, depth);
    Strut left, right;
    if (!f.find_strut(true, left) || !f.find_strut(false, right)) {
        why = "no struts found";
        return false;
    }
    std::vector<Forward::Scope> sc;
    sc.push_back(f.scope_at(scopes[0], scopes[1]));
    sc.push_back(f.scope_at(scopes[2], scopes[3]));
    const int margin = 9 * w / 640;
    int removed = f.remove(left, sc, margin);
    removed += f.remove(right, sc, margin);
    std::vector<const Strut *> both;
    both.push_back(&left);
    both.push_back(&right);
    const int cleaned = f.clean(both, margin);
    // (the legs: the A's lean the user liked, 122 pixels in over 282 rows at 640x480, pushed out until the right
    // leg's outer edge is just past the screen's side on the last row where space shows at that side (the user,
    // 2026-11-06: "push the struts out so the bottom right of the strut is just beyond the right of the screen"); the
    // left leg its mirror; each drawn from the strut that leans its way: the right one for the left leg)
    const double lean = (122.0 / 282.0) * (w / 640.0) / (h / 480.0);
    int bottom = depth - 1;
    while (bottom > 0 && !f.visible(w - 1, bottom)) bottom--;
    const double rightMiddle = w + 2 - f.leg_width(left, lean) / 2.0;
    const double footX = w - 1 - rightMiddle, footY = bottom, topX = footX + lean * bottom;
    f.leg(right, footX, footY, topX);
    f.leg(left, w - 1 - footX, footY, w - 1 - topX);
    f.bar(left, 24 * h / 480);
    wclog(1, "TIE: the shuttle's forward view at %dx%d: struts on rows %d..%d and %d..%d, %d pixels of them out (%d "
             "left over), an A in", w, h, left.first(), left.last(), right.first(), right.last(), removed, cleaned);
    std::vector<Resource> outRes;
    for (size_t k = 0; k < res.size(); k++) {
        Resource r = res[k];
        if (!memcmp(r.type, "PANL", 4)) {
            r.body = encode_panl(px, w, rows);
        } else if (!memcmp(r.type, "MASK", 4)) {
            r.body = encode_mask(vis, w, depth);
        }
        outRes.push_back(r);
    }
    out = write_lfd(outRes);
    return true;
}

// -- the files beside the game's --------------------------------------------------------------------------------------

bool g_masksSwapped = false;
Bit8u g_cmd640[480], g_cmd320[200];     // the default masks, as they were

// The drive the game's cockpits are on (a host directory: the files go beside them).
localDrive *cockpit_drive() {
    for (int d = 0; d < DOS_DRIVES; d++) {
        if (!Drives[d]) continue;
        const char *info = Drives[d]->GetInfo();
        if (!info || strncmp(info, "local directory", 15)) continue;
        if (Drives[d]->FileExists("CP640\\GUNBOAT.INT") || Drives[d]->FileExists("CP320\\GUNBOAT.INT")) {
            return static_cast<localDrive *>(Drives[d]);
        }
    }
    return NULL;
}

std::string host_path(localDrive *drive, const std::string &dosName) {
    char sys[CROSS_LEN * 2];
    std::vector<char> name(dosName.begin(), dosName.end());
    name.push_back(0);
    if (!drive->GetSystemFilename(sys, &name[0])) {
        return std::string();
    }
    return sys;
}

// The Z-95's forward view (step 14; the user: "a cockpit that's a bit like a gunboat but it has a curvey line and the
// long nose you can see. We'll have to kitbash some of the cockpits"): the gunboat's, its struts out as the shuttle's,
// an arched canopy frame of the struts' pixels over the top down to the side consoles, and a long nose from the
// dashboard's middle towards the horizon in the struts' greys.  The figures are 640x480's, scaled.
bool make_forward_z95(const std::vector<Bit8u> &gunboat, int w, int h, int depth, const int scopes[4], const int lights[4],
                      std::vector<Bit8u> &out, std::string &why) {
    std::vector<Resource> res;
    if (!parse_lfd(gunboat, res)) {
        why = "not an LFD";
        return false;
    }
    const Resource *panl = find_res(res, "PANL"), *mask = find_res(res, "MASK"), *pltt = find_res(res, "PLTT");
    if (!panl || !mask || !pltt) {
        why = "no PANL, MASK or PLTT";
        return false;
    }
    std::vector<Bit8u> px, vis;
    const int rows = decode_panl(panl->body, w, h, px);
    if (rows < depth || !decode_mask(mask->body, w, depth, vis)) {
        why = "the picture or its mask did not read";
        return false;
    }
    Forward f(px, vis, w, h, depth);
    Strut left, right;
    if (!f.find_strut(true, left) || !f.find_strut(false, right)) {
        why = "no struts found";
        return false;
    }
    std::vector<Forward::Scope> sc;
    sc.push_back(f.scope_at(scopes[0], scopes[1]));
    sc.push_back(f.scope_at(scopes[2], scopes[3]));
    const int margin = 9 * w / 640;
    f.remove(left, sc, margin);
    f.remove(right, sc, margin);
    std::vector<const Strut *> both;
    both.push_back(&left);
    both.push_back(&right);
    f.clean(both, margin);
    const double sx = w / 640.0, sy = h / 480.0;
    f.arch(left, w / 2.0, 262 * sy, 232 * sx, 6 * sy, 312 * sy, 28 * sx);
    // (the nose's bottom row on the dashboard's top: its lower corners just above the panel below them, the user:
    // "translate the long polygon downward so the bottom is flush with the top of the VDU display's bevel"; row 279 of
    // 480, the side panels' top beside the displays' console, the console itself hiding the nose's middle there)
    const double noseW = 236 * sx, noseH = 62 * sy, top = 200 * sy;
    const int floorY = std::min(f.floor_below((int)(w / 2.0 - noseW / 2 + 1), (int)top),
                                f.floor_below((int)(w / 2.0 + noseW / 2 - 1), (int)top));
    const double base = floorY < depth ? floorY - 1 : 262 * sy;
    f.nose(f.greys(left, pltt->body), w / 2.0, base, base - noseH, noseW, 40 * sx);
    // (no ion cannons: the targeting box's two upper gun lights, painted in the picture at instruments 39 and 40's
    // places, opened to space; the user: "removing the top 2 icons from the crosshair since z-95 does not have ions")
    const int dot = std::max(2, w / 160);
    const int lit = f.open_box(lights[0], lights[1], dot) + f.open_box(lights[2], lights[3], dot);
    wclog(1, "TIE: the Z-95's forward view at %dx%d: the struts out, an arch and a long nose in (its bottom at row %d), "
          "the upper gun lights out (%d pixels)", w, h, (int)base, lit);
    std::vector<Resource> outRes;
    for (size_t k = 0; k < res.size(); k++) {
        Resource r = res[k];
        if (!memcmp(r.type, "PANL", 4)) {
            r.body = encode_panl(px, w, rows);
        } else if (!memcmp(r.type, "MASK", 4)) {
            r.body = encode_mask(vis, w, depth);
        }
        outRes.push_back(r);
    }
    out = write_lfd(outRes);
    return true;
}

// -- the parts (step 14) --------------------------------------------------------------------------------------------
// A parts file (the INT names it at 0x62A, its sprites' count after): the instruments' sprites one after another,
// each the PANL's opcodes (above), every row ended by 254, the sprite by 255.  The gunboat's 30..33 are the shield
// display's little picture of the player's own craft (its back: 19x14 at 640x480, 9x9 at 320x200), red, yellow,
// green and white by the hull's state.

enum { kPartsName = 0x62A, kIconSprite = 30, kIconSprites = 4, kSightSprite = 34, kSightSprites = 5 };

// Each sprite's first byte, and the file's end last.
std::vector<size_t> sprite_starts(const std::vector<Bit8u> &d) {
    std::vector<size_t> starts;
    size_t p = 0;
    while (p < d.size()) {
        starts.push_back(p);
        while (p < d.size()) {
            const Bit8u op = d[p++];
            if (op == 251) p += 1;
            else if (op == 252 || op == 253) p += 2;
            else if (op == 255) break;
        }
    }
    starts.push_back(d.size());
    return starts;
}

std::vector<std::vector<Bit8u> > sprite_rows(const std::vector<Bit8u> &d, size_t p, size_t end) {
    std::vector<std::vector<Bit8u> > rows(1);
    Bit8u base = 0;
    while (p < end) {
        const Bit8u op = d[p++];
        if (op < 251) {
            for (int k = 0; k <= (op & 3); k++) rows.back().push_back((Bit8u)((op >> 2) + base));
        } else if (op == 251 && p < end) {
            base = d[p++];
        } else if ((op == 252 || op == 253) && p + 1 < end) {
            const Bit8u a = d[p], b = d[p + 1];
            p += 2;
            for (int k = 0; k <= (op == 252 ? b : a); k++) rows.back().push_back(op == 252 ? (Bit8u)(a + (k & 1)) : b);
        } else if (op == 254) {
            rows.push_back(std::vector<Bit8u>());
        } else {
            break;
        }
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

void put_sprite(std::vector<Bit8u> &out, const std::vector<std::vector<Bit8u> > &rows) {
    for (size_t y = 0; y < rows.size(); y++) {
        const std::vector<Bit8u> &r = rows[y];
        for (size_t x = 0; x < r.size();) {
            size_t n = 1;
            while (x + n < r.size() && r[x + n] == r[x] && n < 256) n++;
            out.push_back(253);
            out.push_back((Bit8u)(n - 1));
            out.push_back(r[x]);
            x += n;
        }
        out.push_back(254);
    }
    out.push_back(255);
}

// The Z-95 from behind (the user: "the little pixel art gunboat star on the bottom left should be a z-95 shape if
// possible, or else just squashed somehow like a back view --8[]8--"): wings straight out with a gun at each tip, an
// engine each side of the fuselage, the canopy on top.  1 the darkest of the icon's shades .. 4 the brightest.
const char *const kZ95Big[14] = {
    "...................",
    "...................",
    "........141........",
    ".......14441.......",
    ".......23332.......",
    "....43423332434....",
    "4...31323332313...4",
    "4333434233324343334",
    "4...31323332313...4",
    "....43423332434....",
    ".......23332.......",
    ".......12221.......",
    "........111........",
    "...................",
};
const char *const kZ95Small[9] = {
    ".........",
    "....4....",
    ".4.232.4.",
    ".3.232.3.",
    "433333334",
    ".3.232.3.",
    ".4.232.4.",
    "...111...",
    ".........",
};

double luminance(const std::vector<Bit8u> &pltt, Bit8u c) {
    if (pltt.size() < 2 || c < pltt[0] || c > pltt[1] || 2 + (size_t)(c - pltt[0]) * 3 + 2 >= pltt.size()) return c;
    const Bit8u *rgb = &pltt[2 + (size_t)(c - pltt[0]) * 3];
    return 0.3 * rgb[0] + 0.59 * rgb[1] + 0.11 * rgb[2];
}

// The gunboat's parts with the Z-95's icon instead of its own, in each of the icon's colorings: its background (the
// sprite's first pixel) kept, its other colors by brightness (the view's palette) the shades 1..4.
bool make_parts_z95(const std::vector<Bit8u> &gunboat, const std::vector<Bit8u> &pltt, std::vector<Bit8u> &out, std::string &why) {
    const std::vector<size_t> starts = sprite_starts(gunboat);
    if (starts.size() < kSightSprite + kSightSprites + 1) {
        why = "too few sprites";
        return false;
    }
    out.assign(gunboat.begin(), gunboat.begin() + (long)starts[kIconSprite]);
    for (int v = 0; v < kIconSprites; v++) {
        const std::vector<std::vector<Bit8u> > rows = sprite_rows(gunboat, starts[kIconSprite + v], starts[kIconSprite + v + 1]);
        if (rows.empty() || rows[0].empty()) {
            why = "an empty icon";
            return false;
        }
        const int w = (int)rows[0].size(), h = (int)rows.size();
        const Bit8u bg = rows[0][0];
        std::vector<Bit8u> shades;
        for (int y = 0; y < h; y++) {
            for (size_t x = 0; x < rows[y].size(); x++) {
                const Bit8u c = rows[y][x];
                if (c != bg && std::find(shades.begin(), shades.end(), c) == shades.end()) shades.push_back(c);
            }
        }
        if (shades.empty()) {
            why = "an icon of one color";
            return false;
        }
        for (size_t i = 1; i < shades.size(); i++) {        // (by brightness: a few colors)
            for (size_t j = i; j > 0 && luminance(pltt, shades[j - 1]) > luminance(pltt, shades[j]); j--) std::swap(shades[j - 1], shades[j]);
        }
        const bool big = w >= 19 && h >= 14;
        const char *const *art = big ? kZ95Big : kZ95Small;
        const int aw = big ? 19 : 9, ah = big ? 14 : 9, ox = (w - aw) / 2, oy = (h - ah) / 2;
        std::vector<std::vector<Bit8u> > icon((size_t)h, std::vector<Bit8u>((size_t)w, bg));
        for (int y = 0; y < ah; y++) {
            for (int x = 0; x < aw; x++) {
                const char c = art[y][x];
                if (c < '1' || c > '4' || y + oy < 0 || y + oy >= h || x + ox < 0 || x + ox >= w) continue;
                const size_t k = (size_t)((c - '1') * (int)(shades.size() - 1) + 1) / 3;
                icon[(size_t)(y + oy)][(size_t)(x + ox)] = shades[std::min(k, shades.size() - 1)];
            }
        }
        put_sprite(out, icon);
    }
    // (the targeting box: its states with the gun lights' unlit dots drawn in, the gunboat's four at its corners, lose
    // the upper two, the ion cannons'; a dot a small blot of color alone in its corner)
    out.insert(out.end(), gunboat.begin() + (long)starts[kIconSprite + kIconSprites], gunboat.begin() + (long)starts[kSightSprite]);
    for (int v = 0; v < kSightSprites; v++) {
        std::vector<std::vector<Bit8u> > rows = sprite_rows(gunboat, starts[kSightSprite + v], starts[kSightSprite + v + 1]);
        if (rows.empty() || rows[0].empty()) {
            why = "an empty targeting box";
            return false;
        }
        const int w = (int)rows[0].size(), h = (int)rows.size(), cw = w / 10 + 1, ch = h / 10 + 1;
        const Bit8u bg = rows[0][w / 2];
        int cleared = 0;
        for (int y = 0; y < ch && y < h; y++) {
            for (int x = 0; x < cw; x++) {
                const int xs[2] = { x, w - 1 - x };
                for (int k = 0; k < 2; k++) {
                    if (xs[k] < (int)rows[(size_t)y].size() && rows[(size_t)y][(size_t)xs[k]] != bg) {
                        rows[(size_t)y][(size_t)xs[k]] = bg;
                        cleared++;
                    }
                }
            }
        }
        if (cleared) wclog(2, "TIE: the targeting box's state %d: %d pixels of its upper gun lights cleared", v, cleared);
        put_sprite(out, rows);
    }
    out.insert(out.end(), gunboat.begin() + (long)starts[kSightSprite + kSightSprites], gunboat.end());
    return true;
}

// One cockpit directory's <name>.INT and its forward view's LFD.
bool make_cockpit(localDrive *drive, const char *dir, int w, int h, const Derived &dv) {
    std::vector<Bit8u> intBytes, fwd;
    const std::string d = dir;
    if (!read_file(host_path(drive, d + "\\GUNBOAT.INT"), intBytes) || intBytes.size() < 1589) {
        return false;
    }
    // (view 0, the forward one: flags, name[9], x, y, width, depth, y offset; instruments from 0x3F0, 6 bytes each)
    char fwdName[10] = { 0 };
    memcpy(fwdName, &intBytes[1], 9);
    const int depth = le16(&intBytes[1 + 9 + 6]);
    const int scopes[4] = { le16(&intBytes[0x3F0]), le16(&intBytes[0x3F2]), le16(&intBytes[0x3F6]), le16(&intBytes[0x3F8]) };
    const Bit8u *gun = &intBytes[0x3F0 + 39 * 6];        // (the gun lights of weapon groups 2 and 3: the ion cannons')
    const int lights[4] = { le16(gun), le16(gun + 2), le16(gun + 6), le16(gun + 8) };
    std::vector<Bit8u> gunboatFwd, made;
    std::string why;
    bool own = false;
    if (intBytes[0] == 1 && read_file(host_path(drive, d + "\\" + fwdName + ".LFD"), gunboatFwd) &&
        (dv.kind == kForwardShuttle ? make_forward(gunboatFwd, w, h, depth, scopes, made, why)
                                    : make_forward_z95(gunboatFwd, w, h, depth, scopes, lights, made, why))) {
        own = write_file(host_path(drive, d + "\\" + dv.forward + ".LFD"), made);
    } else {
        wclog(1, "TIE: the %s's forward view in %s not made (%s): the gunboat's own", dv.name, dir, why.c_str());
    }
    if (own) {
        memset(&intBytes[1], 0, 9);
        memcpy(&intBytes[1], dv.forward, strlen(dv.forward));
    }
    if (dv.parts) {
        // (its own parts: the gunboat's with this craft's icon on the shield display; the gunboat's own if not made)
        char partsName[10] = { 0 };
        memcpy(partsName, &intBytes[kPartsName], 9);
        std::vector<Bit8u> gunboatParts, parts;
        const Resource *pltt = NULL;
        std::vector<Resource> fwdRes;
        if (parse_lfd(gunboatFwd, fwdRes)) pltt = find_res(fwdRes, "PLTT");
        why.clear();
        if (pltt && read_file(host_path(drive, d + "\\" + partsName + ".PNL"), gunboatParts) &&
            make_parts_z95(gunboatParts, pltt->body, parts, why) && write_file(host_path(drive, d + "\\" + dv.parts + ".PNL"), parts)) {
            memset(&intBytes[kPartsName], 0, 9);
            memcpy(&intBytes[kPartsName], dv.parts, strlen(dv.parts));
        } else {
            wclog(1, "TIE: the %s's parts in %s not made (%s): the gunboat's own", dv.name, dir, why.empty() ? "not read" : why.c_str());
        }
    }
    if (!write_file(host_path(drive, d + "\\" + dv.name + ".INT"), intBytes)) {
        wclog(0, "TIE: %s\\%s.INT could not be written", dir, dv.name);
        return false;
    }
    return true;
}

bool make_files(Derived &dv) {
    if (dv.made) {
        return true;
    }
    localDrive *drive = cockpit_drive();
    if (!drive) {
        wclog(0, "TIE: no drive with the gunboat's cockpit (CP640\\GUNBOAT.INT): the %s has none", dv.name);
        return false;
    }
    const bool hi = make_cockpit(drive, "CP640", 640, 480, dv);
    const bool lo = make_cockpit(drive, "CP320", 320, 200, dv);
    drive->EmptyCache();        // (DOS sees the new files)
    dv.made = hi || lo;
    wclog(1, "TIE: the %s's cockpit made from the gunboat's: CP640 %s, CP320 %s", dv.name, hi ? "yes" : "no", lo ? "yes" : "no");
    return dv.made;
}

void copy_mem(Bit32u to, Bit32u from, Bitu n) {
    for (Bitu i = 0; i < n; i++) {
        mem_writeb(tie_linear(to) + i, mem_readb(tie_linear(from) + i));
    }
}

void swap_masks(bool gunboats) {
    if (gunboats && !g_masksSwapped) {
        for (Bitu i = 0; i < sizeof(g_cmd640); i++) g_cmd640[i] = mem_readb(tie_linear(kCmd640Mask) + i);
        for (Bitu i = 0; i < sizeof(g_cmd320); i++) g_cmd320[i] = mem_readb(tie_linear(kCmdMask) + i);
        copy_mem(kCmd640Mask, kGunboatCmd640Mask, sizeof(g_cmd640));
        copy_mem(kCmdMask, kGunboatCmdMask, sizeof(g_cmd320));
        g_masksSwapped = true;
    } else if (!gunboats && g_masksSwapped) {
        for (Bitu i = 0; i < sizeof(g_cmd640); i++) mem_writeb(tie_linear(kCmd640Mask) + i, g_cmd640[i]);
        for (Bitu i = 0; i < sizeof(g_cmd320); i++) mem_writeb(tie_linear(kCmdMask) + i, g_cmd320[i]);
        g_masksSwapped = false;
    }
}

// WCNET_TIE_FLY_AS: the player's flight group flown in another craft (the mission files' type number).
int fly_as() {
    static int type = -2;
    if (type == -2) {
        const char *env = getenv("WCNET_TIE_FLY_AS");
        type = env && *env ? atoi(env) : -1;
        if (type == 0 || type > 88) type = -1;
    }
    return type;
}

}  // namespace

// The player's flight group in another craft, before the flight loads its craft: its type, the species marked to be
// loaded (create_loadmission's own mark), and the player's spec number as create_loadmission would have set it.
void tie_fly_group_as(int fg, int type) {
    const Bit32u fgAt = tie_linear(tie::kFlightGroups + (Bit32u)fg * tie::kFlightGroupSize);
    const int was = mem_readb(fgAt + 0x32);
    mem_writeb(fgAt + 0x32, (Bit8u)type);
    const int species = mem_readb(tie_linear(kSpeciesConvert) + (Bit32u)type);
    const Bit32u sp = tie_linear(kSpeciesTable + (Bit32u)species * kSpeciesSize);
    mem_writeb(sp + 1, mem_readb(sp + 1) | 0x10);
    const Bit32u pstate = tie_linear(tie::kPstate);
    if (mem_readb(pstate + 0x0A) == fg) {
        mem_writeb(pstate + 0x0E, mem_readb(sp + 0x13));
    }
    wclog(1, "TIE: flight group %d flown as craft type %d (it was %d): species %d, spec %d", fg, type, was, species,
          mem_readb(sp + 0x13));
}

// At fediskio_loadspecies's entry: the mission is read (create_loadmission), its craft not yet loaded, the cockpit
// not yet either.
bool tie_game_file(const std::string &dosName, std::vector<Bit8u> &out) {
    localDrive *drive = cockpit_drive();
    if (!drive) return false;
    const std::string path = host_path(drive, dosName);
    return !path.empty() && read_file(path, out);
}

void tie_cockpit_before_species() {
    const Bit32u pstate = tie_linear(tie::kPstate);
    const int fg = mem_readb(pstate + 0x0A);
    if (fly_as() > 0 && fg < 48) {
        tie_fly_group_as(fg, fly_as());
    }
    // (a craft of ours: its spec named for its cockpit, the cockpit's files made, the targeting display the gunboat's)
    const int spec = mem_readb(pstate + 0x0E);
    Derived *dv = NULL;
    for (size_t k = 0; k < sizeof(g_derived) / sizeof(g_derived[0]); k++) {
        if (g_derived[k].spec == spec) dv = &g_derived[k];
    }
    if (dv) {
        const Bit32u name = tie_linear(kSpecData + (Bit32u)dv->spec * kSpecSize + kSpecName);
        for (Bitu i = 0; i < 9; i++) {
            mem_writeb(name + i, i < strlen(dv->name) ? (Bit8u)dv->name[i] : 0);
        }
        make_files(*dv);
    }
    swap_masks(dv != NULL);
}

void tie_cockpit_forget() {
    g_masksSwapped = false;     // (a new program: its own masks)
    for (size_t k = 0; k < sizeof(g_derived) / sizeof(g_derived[0]); k++) g_derived[k].made = false;
}

}  // namespace wc
