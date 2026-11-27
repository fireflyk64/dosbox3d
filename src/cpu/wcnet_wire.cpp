/*
 *  Line art drawn at twice its size, one fine pixel wide (wcnet_wire.h): Privateer's target display's wireframes
 *  (wcnet_priv_hires.cpp, wire_make: the user's rulings on its ships, 2026-11-23, in the comments below) for the games
 *  its library grew from.  Privateer keeps its own copy, with its hand-made parts (the Talon's front, the Centurion's
 *  nose), so that its approved pictures cannot move with a change made here.
 */
#include <math.h>
#include <stdlib.h>
#include <algorithm>
#include <vector>
#include "dosbox.h"
#include "wcnet_wire.h"

namespace wc {

namespace {

enum { kChainMax = 4096, kMaxChains = 512 };

// M x = the last column, four unknowns (Gauss, the largest pivot): false if it has no single answer.
bool solve4(double M[4][5], double x[4]) {
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) {
            if (fabs(M[r][c]) > fabs(M[p][c])) p = r;
        }
        if (fabs(M[p][c]) < 1e-9) return false;
        for (int k = 0; k < 5; k++) std::swap(M[c][k], M[p][k]);
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            const double f = M[r][c] / M[c][c];
            for (int k = c; k < 5; k++) M[r][k] -= f * M[c][k];
        }
    }
    for (int i = 0; i < 4; i++) x[i] = M[i][4] / M[i][i];
    return true;
}

// The chain's points simplified (Douglas and Peucker, within tol), its ends kept: the indices kept, in order.
void simplify(const std::vector<int> &cu, const std::vector<int> &cv, double tol, std::vector<int> &keep) {
    const int n = (int)cu.size();
    std::vector<char> on(n, 0);
    on[0] = on[n - 1] = 1;
    std::vector<std::pair<int, int> > stack(1, std::make_pair(0, n - 1));
    while (!stack.empty()) {
        const int a = stack.back().first, b = stack.back().second;
        stack.pop_back();
        if (b - a < 2) continue;
        const double dx = cu[b] - cu[a], dy = cv[b] - cv[a], len = sqrt(dx * dx + dy * dy);
        double best = -1;
        int k = a;
        for (int i = a + 1; i < b; i++) {
            const double ex = cu[i] - cu[a], ey = cv[i] - cv[a];
            const double d = len > 0 ? fabs(dx * ey - dy * ex) / len : sqrt(ex * ex + ey * ey);
            if (d > best) { best = d; k = i; }
        }
        if (best <= tol) continue;
        on[k] = 1;
        stack.push_back(std::make_pair(a, k));
        stack.push_back(std::make_pair(k, b));
    }
    keep.clear();
    for (int i = 0; i < n; i++) {
        if (on[i]) keep.push_back(i);
    }
}

}  // namespace

void wire_make(int w, int h, const std::vector<Bit16s> &px, const std::vector<char> &block, WireFrame &out) {
    const int W = 2 * w, H = 2 * h;
    out.w = w;
    out.h = h;
    out.axis = -1;
    out.fine.assign(W * H, (Bit16s)-1);
    auto col = [&](int u, int v) { return u >= 0 && v >= 0 && u < w && v < h ? (int)px[v * w + u] : -1; };
    auto put = [&](int X, int Y, int c) { if (X >= 0 && Y >= 0 && X < W && Y < H) out.fine[Y * W + X] = (Bit16s)c; };
    auto drawf = [&](double x0, double y0, double x1, double y1, int c) {
        const int X0 = (int)floor(x0 + 0.5), Y0 = (int)floor(y0 + 0.5), X1 = (int)floor(x1 + 0.5), Y1 = (int)floor(y1 + 0.5);
        int X = X0, Y = Y0;
        const int dx = abs(X1 - X), dy = -abs(Y1 - Y), sx = X < X1 ? 1 : -1, sy = Y < Y1 ? 1 : -1;
        int err = dx + dy;
        for (int n = 0; n < 4 * (W + H); n++) {
            put(X, Y, c);
            if (X == X1 && Y == Y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; X += sx; }
            if (e2 <= dx) { err += dx; Y += sy; }
        }
    };
    static const int kDu8[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }, kDv8[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    // solid parts: a pixel in a square of four of its colour, the game's block
    std::vector<char> solid(w * h, 0);
    for (int v = 0; v < h; v++) {
        for (int u = 0; u < w; u++) {
            const int c = col(u, v);
            if (c < 0) continue;
            if ((size_t)(v * w + u) < block.size() && block[v * w + u]) solid[v * w + u] = 1;
            for (int dv = -1; dv <= 0 && !solid[v * w + u]; dv++) {
                for (int du = -1; du <= 0; du++) {
                    if (col(u + du, v + dv) == c && col(u + du + 1, v + dv) == c && col(u + du, v + dv + 1) == c &&
                        col(u + du + 1, v + dv + 1) == c) {
                        solid[v * w + u] = 1;
                        break;
                    }
                }
            }
        }
    }
    // A piece of four pixels or fewer of a colour touching a solid part is solid too (the Stiletto's engines: a yellow
    // block and the orange row at its end -- the user: "Stilettos back engines should be flat")
    {
        std::vector<char> seen(w * h, 0);
        std::vector<int> piece;
        for (int v = 0; v < h; v++) {
            for (int u = 0; u < w; u++) {
                const int c = col(u, v);
                if (c < 0 || solid[v * w + u] || seen[v * w + u]) continue;
                piece.assign(1, v * w + u);
                seen[v * w + u] = 1;
                // (the piece grown whole, at most the picture: one cut short at five left its rest unmarked, and
                //  each bit of a long line then counted as a small piece of its own: chance blocks)
                for (size_t i = 0; i < piece.size() && piece.size() <= (size_t)(w * h); i++) {
                    const int pu = piece[i] % w, pv = piece[i] / w;
                    for (int k = 0; k < 8; k++) {
                        const int nu = pu + kDu8[k], nv = pv + kDv8[k];
                        if (col(nu, nv) == c && !solid[nv * w + nu] && !seen[nv * w + nu]) {
                            seen[nv * w + nu] = 1;
                            piece.push_back(nv * w + nu);
                        }
                    }
                }
                if (piece.size() > 4) continue;
                bool touches = false;
                for (size_t i = 0; i < piece.size() && !touches; i++) {
                    const int pu = piece[i] % w, pv = piece[i] / w;
                    for (int k = 0; k < 8 && !touches; k += 2) {        // (the four orthogonal neighbours)
                        const int nu = pu + kDu8[k], nv = pv + kDv8[k];
                        touches = nu >= 0 && nv >= 0 && nu < w && nv < h && solid[nv * w + nu] == 1;
                    }
                }
                if (touches) {
                    for (size_t i = 0; i < piece.size(); i++) solid[piece[i]] = 2;
                }
            }
        }
    }
    for (int v = 0; v < h; v++) {
        for (int u = 0; u < w; u++) {
            if (!solid[v * w + u]) continue;
            const int c = col(u, v);
            put(2 * u, 2 * v, c);
            put(2 * u + 1, 2 * v, c);
            put(2 * u, 2 * v + 1, c);
            put(2 * u + 1, 2 * v + 1, c);
        }
    }
    // A colour's loop round a hole that is an ellipse is drawn as one (the Paradigm's pods: the user, "Paradign should
    // have every pod be rounded"): for each colour, a region of other pixels (4-connected) that touches no edge, of 200
    // pixels at most; its loop, the colour's pixels about it; not a box (two corners of its bounding box its own
    // pixels: the Tarsus's cells stay square), an upright ellipse fitted (least squares) with radii of 1.5 to 14 pixels
    // and every pixel of the loop within 0.8 of it.  Its pixels are then the ellipse's: no chain's; a chain reaching one
    // ends at its middle.
    std::vector<int> ring(w * h, 0);
    {
        std::vector<int> region(w * h, 0), stamp(w * h, 0), comp, loop;
        int stampNo = 0, rings = 0;
        std::vector<int> colours;
        for (int i = 0; i < w * h; i++) {
            if (px[i] >= 0 && std::find(colours.begin(), colours.end(), (int)px[i]) == colours.end() && colours.size() < 32) {
                colours.push_back(px[i]);
            }
        }
        for (size_t ci = 0; ci < colours.size() && rings < 64; ci++) {
            const int c = colours[ci];
            std::fill(region.begin(), region.end(), 0);
            for (int start = 0; start < w * h && rings < 64; start++) {
                if (region[start] || col(start % w, start / w) == c) continue;
                comp.assign(1, start);
                region[start] = 1;
                bool edge = false;
                // (the region flooded whole, at most the picture: one cut short left its rest unmarked, each piece of
                //  it then taken for a hole of its own -- chance rings, moved by a change anywhere in the picture)
                for (size_t i = 0; i < comp.size() && comp.size() <= (size_t)(w * h); i++) {
                    const int pu = comp[i] % w, pv = comp[i] / w;
                    if (pu == 0 || pv == 0 || pu == w - 1 || pv == h - 1) edge = true;
                    for (int k = 0; k < 8; k += 2) {
                        const int nu = pu + kDu8[k], nv = pv + kDv8[k];
                        if (nu < 0 || nv < 0 || nu >= w || nv >= h || region[nv * w + nu] || col(nu, nv) == c) continue;
                        region[nv * w + nu] = 1;
                        comp.push_back(nv * w + nu);
                    }
                }
                if (edge || comp.size() > 200) continue;
                stampNo++;
                loop.clear();
                bool line = true;                       // (a loop along a part kept as blocks is no ring: the Talon's)
                for (size_t i = 0; i < comp.size() && line; i++) {
                    const int pu = comp[i] % w, pv = comp[i] / w;
                    for (int k = 0; k < 8 && line; k++) {
                        const int nu = pu + kDu8[k], nv = pv + kDv8[k];
                        if (col(nu, nv) == c && (size_t)(nv * w + nu) < block.size() && block[nv * w + nu]) line = false;
                        if (col(nu, nv) != c || solid[nv * w + nu] || stamp[nv * w + nu] == stampNo) continue;
                        stamp[nv * w + nu] = stampNo;
                        loop.push_back(nv * w + nu);
                    }
                }
                if (!line || loop.size() < 6) continue;
                int u0 = w, u1 = -1, v0 = h, v1 = -1;
                for (size_t i = 0; i < loop.size(); i++) {
                    const int pu = loop[i] % w, pv = loop[i] / w;
                    u0 = std::min(u0, pu); u1 = std::max(u1, pu); v0 = std::min(v0, pv); v1 = std::max(v1, pv);
                }
                const int corners = (stamp[v0 * w + u0] == stampNo) + (stamp[v0 * w + u1] == stampNo) +
                                    (stamp[v1 * w + u0] == stampNo) + (stamp[v1 * w + u1] == stampNo);
                if (corners >= 2) continue;
                const double cx = (u0 + u1) / 2.0, cy = (v0 + v1) / 2.0;
                double M[4][5] = { { 0 } };
                for (size_t i = 0; i < loop.size(); i++) {
                    const double du = loop[i] % w - cx, dv = loop[i] / w - cy, f[4] = { du * du, dv * dv, du, dv };
                    for (int a = 0; a < 4; a++) {
                        M[a][4] += f[a];
                        for (int b = 0; b < 4; b++) M[a][b] += f[a] * f[b];
                    }
                }
                double sol[4];
                if (!solve4(M, sol) || sol[0] <= 0 || sol[1] <= 0) continue;
                const double ex = cx - sol[2] / (2 * sol[0]), ey = cy - sol[3] / (2 * sol[1]);
                const double K = 1 + sol[2] * sol[2] / (4 * sol[0]) + sol[3] * sol[3] / (4 * sol[1]);
                if (K <= 0) continue;
                const double ea = sqrt(K / sol[0]), eb = sqrt(K / sol[1]);
                if (ea < 1.5 || eb < 1.5 || ea > 14 || eb > 14) continue;
                bool fits = true;
                for (size_t i = 0; i < loop.size() && fits; i++) {
                    const double du = loop[i] % w - ex, dv = loop[i] / w - ey;
                    const double rho = sqrt((du / ea) * (du / ea) + (dv / eb) * (dv / eb));
                    fits = rho > 1e-9 && sqrt(du * du + dv * dv) * fabs(1 - 1 / rho) <= 0.8;
                }
                if (!fits) continue;
                rings++;
                for (size_t i = 0; i < loop.size(); i++) ring[loop[i]] = rings;
                const int n = 16 + (int)(16 * std::max(ea, eb));
                double px0 = 2 * (ex + ea) + 1, py0 = 2 * ey + 1;
                for (int i = 1; i <= n; i++) {
                    const double th = i * 6.283185307179586 / n;
                    const double px1 = 2 * (ex + ea * cos(th)) + 1, py1 = 2 * (ey + eb * sin(th)) + 1;
                    drawf(px0, py0, px1, py1, c);
                    px0 = px1;
                    py0 = py1;
                }
            }
        }
    }
    // each pixel's links (bit k: neighbour k) to its colour, a diagonal only where no corner of the colour joins them
    static const int kDu[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }, kDv[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    std::vector<Bit8u> link(w * h, 0), used(w * h, 0), degree(w * h, 0);
    for (int v = 0; v < h; v++) {
        for (int u = 0; u < w; u++) {
            const int c = col(u, v);
            if (c < 0 || solid[v * w + u] || ring[v * w + u]) continue;
            for (int k = 0; k < 8; k++) {
                const int nu = u + kDu[k], nv = v + kDv[k];
                if (col(nu, nv) != c) continue;
                if (kDu[k] && kDv[k] && (col(nu, v) == c || col(u, nv) == c)) continue;
                link[v * w + u] |= (Bit8u)(1 << k);
                degree[v * w + u]++;
            }
        }
    }
    // the chains: pixels of two links between ends and junctions (or round a loop), each link walked once
    struct Chain { int col; std::vector<int> u, v; bool alive; };
    std::vector<Chain> chains;
    auto walk = [&](int pu, int pv, int k) {
        Chain c;
        c.col = col(pu, pv);
        c.alive = true;
        c.u.assign(1, pu);
        c.v.assign(1, pv);
        int u = pu, v = pv;
        for (int steps = 0; steps < kChainMax; steps++) {
            used[v * w + u] |= (Bit8u)(1 << k);
            const int nu = u + kDu[k], nv = v + kDv[k];
            used[nv * w + nu] |= (Bit8u)(1 << ((k + 4) & 7));
            c.u.push_back(nu);
            c.v.push_back(nv);
            u = nu;
            v = nv;
            if (solid[v * w + u] || ring[v * w + u] || degree[v * w + u] != 2) break;
            int next = -1;
            for (int j = 0; j < 8 && next < 0; j++) {
                if ((link[v * w + u] & (1 << j)) && !(used[v * w + u] & (1 << j))) next = j;
            }
            if (next < 0) break;
            k = next;
        }
        if (chains.size() < kMaxChains) chains.push_back(c);
    };
    for (int pass = 0; pass < 2; pass++) {          // (from the ends and junctions first, then what is left: loops)
        for (int v = 0; v < h; v++) {
            for (int u = 0; u < w; u++) {
                const int i = v * w + u;
                if (col(u, v) < 0 || solid[i] || ring[i] || (pass == 0 && degree[i] == 2)) continue;
                for (int k = 0; k < 8; k++) {
                    if ((link[i] & (1 << k)) && !(used[i] & (1 << k))) walk(u, v, k);
                }
            }
        }
    }
    // A straight line through a junction is one line (the user, 2026-11-23: "the talon's leading edge wings are
    // straight, but the system drew it as a subtle curve": the wing's 45-degree edge, met half-way by an inner stroke,
    // was two chains, each simplified to its own slope).  Of the chains of a colour that end at one pixel, the pair
    // that together simplify to no corner there (within the tolerance) is joined -- the straightest pair first, again
    // until no pair is left; a closed loop and a chain's own two ends are not joined.
    std::vector<int> keep, cu, cv;
    auto cat = [&](const Chain &A, bool aEndsHere, const Chain &B, bool bStartsHere) {
        cu.clear();
        cv.clear();
        const int na = (int)A.u.size(), nb = (int)B.u.size();
        for (int i = 0; i < na; i++) {
            const int j = aEndsHere ? i : na - 1 - i;
            cu.push_back(A.u[j]);
            cv.push_back(A.v[j]);
        }
        for (int i = 1; i < nb; i++) {
            const int j = bStartsHere ? i : nb - 1 - i;
            cu.push_back(B.u[j]);
            cv.push_back(B.v[j]);
        }
        return na - 1;                              // (the junction's index in the joined chain)
    };
    for (int round = 0; round < kMaxChains; round++) {
        double best = 1e9;
        int bi = -1, bj = -1;
        bool bEi = false, bEj = false;
        for (size_t i = 0; i < chains.size(); i++) {
            const Chain &A = chains[i];
            if (!A.alive || A.u.size() < 2 || (A.u.front() == A.u.back() && A.v.front() == A.v.back())) continue;
            for (int ei = 0; ei < 2; ei++) {
                const int ju = ei ? A.u.back() : A.u.front(), jv = ei ? A.v.back() : A.v.front();
                for (size_t j = i + 1; j < chains.size(); j++) {
                    const Chain &B = chains[j];
                    if (!B.alive || B.col != A.col || B.u.size() < 2 || (B.u.front() == B.u.back() && B.v.front() == B.v.back())) continue;
                    for (int ej = 0; ej < 2; ej++) {
                        const int ku = ej ? B.u.back() : B.u.front(), kv = ej ? B.v.back() : B.v.front();
                        if (ku != ju || kv != jv) continue;
                        const int at = cat(A, ei == 1, B, ej == 0);
                        simplify(cu, cv, 0.75, keep);
                        int k0 = -1, k1 = -1;
                        bool corner = false;
                        for (size_t k = 0; k < keep.size(); k++) {
                            if (keep[k] == at) corner = true;
                            if (keep[k] < at) k0 = keep[k];
                            if (keep[k] > at && k1 < 0) k1 = keep[k];
                        }
                        if (corner || k0 < 0 || k1 < 0) continue;
                        const double dx = cu[k1] - cu[k0], dy = cv[k1] - cv[k0], len = sqrt(dx * dx + dy * dy);
                        const double d = len > 0 ? fabs(dx * (cv[at] - cv[k0]) - dy * (cu[at] - cu[k0])) / len : 0;
                        if (d < best) { best = d; bi = (int)i; bj = (int)j; bEi = ei == 1; bEj = ej == 0; }
                    }
                }
            }
        }
        if (bi < 0) break;
        cat(chains[bi], bEi, chains[bj], bEj);
        chains[bi].u = cu;
        chains[bi].v = cv;
        chains[bj].alive = false;
    }
    // each chain's straight segments, drawn between the pixels' middles; a chain ending where another's line passes
    // through (a junction) ends on that line, the nearest point of it within 1.5 pixels
    struct Poly { int col; std::vector<double> x, y; const Chain *chain; };
    std::vector<Poly> polys;
    for (size_t i = 0; i < chains.size(); i++) {
        const Chain &c = chains[i];
        if (!c.alive || c.u.size() < 2) continue;
        simplify(c.u, c.v, 0.75, keep);
        Poly p;
        p.col = c.col;
        p.chain = &c;
        for (size_t k = 0; k < keep.size(); k++) {
            p.x.push_back(c.u[keep[k]]);
            p.y.push_back(c.v[keep[k]]);
        }
        polys.push_back(p);
    }
    auto snap = [&](double &px, double &py, size_t own) {
        double best = 1.5;
        double bx = px, by = py;
        for (size_t k = 0; k < polys.size(); k++) {
            if (k == own || polys[k].col != polys[own].col) continue;
            const Chain &c = *polys[k].chain;
            bool through = false;                       // (the point one of its inner pixels)
            for (size_t i = 1; i + 1 < c.u.size() && !through; i++) through = c.u[i] == px && c.v[i] == py;
            if (!through) continue;
            for (size_t i = 0; i + 1 < polys[k].x.size(); i++) {
                const double ax = polys[k].x[i], ay = polys[k].y[i], dx = polys[k].x[i + 1] - ax, dy = polys[k].y[i + 1] - ay;
                const double l2 = dx * dx + dy * dy;
                double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                const double qx = ax + t * dx, qy = ay + t * dy, d = sqrt((qx - px) * (qx - px) + (qy - py) * (qy - py));
                if (d < best) { best = d; bx = qx; by = qy; }
            }
        }
        px = bx;
        py = by;
    };
    std::vector<char> inChain(w * h, 0);
    for (size_t k = 0; k < polys.size(); k++) {
        Poly &p = polys[k];
        snap(p.x.front(), p.y.front(), k);
        snap(p.x.back(), p.y.back(), k);
        for (size_t i = 0; i + 1 < p.x.size(); i++) {
            drawf(2 * p.x[i] + 1, 2 * p.y[i] + 1, 2 * p.x[i + 1] + 1, 2 * p.y[i + 1] + 1, p.col);
        }
        for (size_t i = 0; i < p.chain->u.size(); i++) inChain[p.chain->v[i] * w + p.chain->u[i]] = 1;
    }
    for (int v = 0; v < h; v++) {                   // (a pixel in no chain, nor solid, nor a ring's: its middle)
        for (int u = 0; u < w; u++) {
            if (col(u, v) >= 0 && !solid[v * w + u] && !ring[v * w + u] && !inChain[v * w + u]) put(2 * u + 1, 2 * v + 1, col(u, v));
        }
    }
    // A line's end reaches half-way to a pixel of another colour that touches it (a diagonal one only where no corner
    // pixel of another colour comes between): the colours meet as the game's pixels do (the Tarsus's cockpit bar and its
    // weapon pods: the user, "Tarsis front cockpit should connect through the 2 weapon pods. There shouldn't be gaps")
    for (int v = 0; v < h; v++) {
        for (int u = 0; u < w; u++) {
            const int i = v * w + u, c = col(u, v);
            if (c < 0 || solid[i] || ring[i] || degree[i] > 1) continue;
            for (int k = 0; k < 8; k++) {
                const int nu = u + kDu[k], nv = v + kDv[k], nc = col(nu, nv);
                if (nc < 0 || nc == c) continue;
                if (kDu[k] && kDv[k]) {
                    const int a = col(nu, v), b = col(u, nv);
                    if ((a >= 0 && a != c) || (b >= 0 && b != c)) continue;
                }
                drawf(2 * u + 1, 2 * v + 1, u + nu + 1, v + nv + 1, c);      // (to the middle between the two)
            }
        }
    }
    // a mirror image?  The axis (u + u' of a mirrored pair) near the middle that most of the outline agrees with
    int lit = 0, best = -1, bestAgree = -1;
    for (int i = 0; i < w * h; i++) lit += px[i] >= 0;
    for (int axis = w - 3; axis <= w + 1; axis++) {
        int agree = 0;
        for (int v = 0; v < h; v++) {
            for (int u = 0; u < w; u++) {
                if (px[v * w + u] >= 0 && col(axis - u, v) >= 0) agree++;
            }
        }
        if (agree > bestAgree) { bestAgree = agree; best = axis; }
    }
    if (w < 8 || lit == 0 || bestAgree * 100 < lit * 60) return;
    out.axis = best;
    // the right half's pixels where the art is a mirror image there (its outline in the 3 x 3 pixels about it the same
    // as about its mirror) are the left half's copied, pixel for pixel; elsewhere (a side's shields, which differ) each
    // half keeps its own drawing.  A copy's colour is the art's own there (its pixel, or the nearest lit one about it
    // whose mirror is the left's colour, or any lit one): a side's damage shows.
    const int sum = 2 * best + 1;                  // (fine X and sum - X are mirror images)
    if (best % 2 == 0) {
        // A column on the axis itself (best even: u and best - u the same column): its two fine columns are each
        // other's mirror, so a line along it, drawn in its right one (the pixels' middles are 2u + 1), is drawn in
        // both -- two pixels wide, the one way it is a mirror image of itself
        const int XL = best, XR = best + 1;
        for (int Y = 0; Y < H; Y++) {
            if (XR < W && out.fine[Y * W + XR] >= 0 && out.fine[Y * W + XL] < 0) out.fine[Y * W + XL] = out.fine[Y * W + XR];
        }
    }
    std::vector<Bit16s> drawn(out.fine);
    auto mirrored = [&](int u, int v) {
        for (int dv = -1; dv <= 1; dv++) {
            for (int du = -1; du <= 1; du++) {
                if ((col(u + du, v + dv) >= 0) != (col(best - u - du, v + dv) >= 0)) return false;
            }
        }
        return true;
    };
    for (int Y = 0; Y < H; Y++) {
        for (int X = 0; X < W; X++) {
            const int M = sum - X;                      // (X here the right half's, M its mirror in the left)
            if (M >= X || M < 0 || !mirrored(X >> 1, Y >> 1)) continue;
            const Bit16s c = drawn[Y * W + M];
            if (c < 0) {
                out.fine[Y * W + X] = (Bit16s)-1;
                continue;
            }
            const int u = X >> 1, v = Y >> 1;
            bool same = true;                           // (the art's colours a mirror image here too: the copy's own)
            for (int dv = -1; dv <= 1 && same; dv++) {
                for (int du = -1; du <= 1 && same; du++) same = col(u + du, v + dv) == col(best - u - du, v + dv);
            }
            if (same) {
                out.fine[Y * W + X] = c;
                continue;
            }
            int pick = -1, any = -1;
            for (int k = -1; k < 8 && pick < 0; k++) {
                const int qu = k < 0 ? u : u + kDu[k], qv = k < 0 ? v : v + kDv[k];
                const int qc = col(qu, qv);
                if (qc < 0) continue;
                if (any < 0) any = qc;
                if (col(best - qu, qv) == c) pick = qc;
            }
            out.fine[Y * W + X] = (Bit16s)(pick >= 0 ? pick : any >= 0 ? any : c);
        }
    }
}


}  // namespace wc
