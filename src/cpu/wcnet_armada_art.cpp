/*
 *  Line art for Armada's high-resolution cockpit (wcnet_armada_art.h).  Copied from Privateer's
 *  (wcnet_priv_hires.cpp: the rings found and drawn, the line art) with its picture made a parameter.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include "wcnet_armada_art.h"

namespace wc {
namespace armart {

namespace {
const double kSy = 2.0;     // (fine rows to a game row)
}

void plot(const Target &t, int X, int Y, Bit8u c) {
    if (X < 2 * t.x0 || X >= 2 * (t.x1 + 1) || Y < 2 * t.y0 || Y >= 2 * (t.y1 + 1) || X < 0 || Y < 0 || X >= t.w || Y >= t.h) return;
    t.px[Y * t.w + X] = c;
    if (t.set) t.set[Y * t.w + X] = 1;
}

void line_fine(const Target &t, int X, int Y, int Xe, int Ye, Bit8u c) {
    const int dx = abs(Xe - X), dy = -abs(Ye - Y), sx = X < Xe ? 1 : -1, sy = Y < Ye ? 1 : -1;
    int err = dx + dy;
    for (int n = 0; n < 8192; n++) {
        plot(t, X, Y, c);
        if (X == Xe && Y == Ye) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; X += sx; }
        if (e2 <= dx) { err += dx; Y += sy; }
    }
}

void line(const Target &t, int x0, int y0, int x1, int y1, Bit8u c) {
    if (x0 < -2000 || x0 > 2000 || x1 < -2000 || x1 > 2000 || y0 < -2000 || y0 > 2000 || y1 < -2000 || y1 > 2000) return;
    line_fine(t, 2 * x0 + 1, 2 * y0 + 1, 2 * x1 + 1, 2 * y1 + 1, c);
}

Bit32u shape_hash(const Shape &s) {
    Bit32u h = 2166136261u;
    const int head[4] = { s.w, s.h, s.left, s.top };
    for (int i = 0; i < 4; i++) h = (h ^ (Bit32u)head[i]) * 16777619u;
    for (size_t i = 0; i < s.px.size(); i++) h = (h ^ (Bit16u)s.px[i]) * 16777619u;
    return h;
}

struct Ring { double cx, cy, a, b; int colour; };
struct Rings {
    Bit32u hash;
    std::vector<Ring> rings;
    std::vector<Bit8u> on;          // per pixel of the shape: 1 + the ring's index it belongs to, 0 none
};
enum { kRingCache = 16, kMaxRings = 24, kMaxPiecePixels = 4096, kRingSectors = 48 };

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

void rings_find(const Shape &s, int ground, Rings &out) {
    const int w = s.w, h = s.h;
    out.rings.clear();
    out.on.assign(w * h, 0);
    std::vector<Bit8u> seen(w * h, 0);
    std::vector<int> piece, stack;
    for (int start = 0; start < w * h && (int)out.rings.size() < kMaxRings; start++) {
        const int colour = s.px[start];
        if (colour < 0 || colour == ground || seen[start]) continue;
        // the piece: this colour's pixels 8-connected to it
        piece.clear();
        stack.assign(1, start);
        seen[start] = 1;
        while (!stack.empty() && (int)piece.size() < kMaxPiecePixels) {
            const int i = stack.back();
            stack.pop_back();
            piece.push_back(i);
            const int u = i % w, v = i / w;
            for (int dv = -1; dv <= 1; dv++) {
                for (int du = -1; du <= 1; du++) {
                    const int nu = u + du, nv = v + dv;
                    if (nu < 0 || nv < 0 || nu >= w || nv >= h) continue;
                    const int j = nv * w + nu;
                    if (!seen[j] && s.px[j] == colour) {
                        seen[j] = 1;
                        stack.push_back(j);
                    }
                }
            }
        }
        // rings in it, the biggest first, each found taken out before the next look (at most four a piece)
        for (int round = 0; round < 4 && (int)out.rings.size() < kMaxRings; round++) {
            std::vector<int> left;
            for (size_t k = 0; k < piece.size(); k++) {
                if (!out.on[piece[k]]) left.push_back(piece[k]);
            }
            if (left.size() < 12) break;
            int u0 = w, u1 = -1, v0 = h, v1 = -1;
            for (size_t k = 0; k < left.size(); k++) {
                const int u = left[k] % w, v = left[k] / w;
                u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, v); v1 = std::max(v1, v);
            }
            const double cx = (u0 + u1) / 2.0, cy = (v0 + v1) / 2.0;
            double bestQ = 1, bestR = 0;
            int bestN = 0;
            std::vector<double> r(left.size());
            for (int qi = 0; qi <= 50; qi++) {
                const double q = 0.6 + qi * 0.02;           // (height over width)
                for (size_t k = 0; k < left.size(); k++) {
                    const double du = left[k] % w - cx, dv = (left[k] / w - cy) / q;
                    r[k] = sqrt(du * du + dv * dv);
                }
                std::sort(r.begin(), r.end());
                for (size_t a = 0, b = 0; a < r.size(); a++) {
                    while (b < r.size() && r[b] - r[a] <= 1.2) b++;
                    if ((int)(b - a) > bestN) {
                        bestN = (int)(b - a);
                        bestQ = q;
                        bestR = (r[a] + r[b - 1]) / 2;
                    }
                }
            }
            if (bestR < 2.5 || bestR * bestQ < 2.5) break;
            // the ellipse itself, fitted: A du^2 + B dv^2 + C du + D dv = 1 by least squares over the pixels near the last
            // one (within a pixel, then 0.8), four times.  (Its pixels' box was taken first: the crosshair's ticks lie on
            // its axes, and the box made its circle of 9 an ellipse of 9 by 10 that most of its pixels missed: the user,
            // "The targeting reticle is still wobbly back and forth instead of a beautiful circle like the radar".)
            double ex = cx, ey = cy, ea = bestR, eb = bestR * bestQ;
            bool fitted = true;
            for (int it = 0; it < 4 && fitted; it++) {
                const double tol = it ? 0.8 : 1.0;
                double M[4][5] = { { 0 } };
                int n = 0;
                for (size_t k = 0; k < left.size(); k++) {
                    const double du = left[k] % w - ex, dv = left[k] / w - ey;
                    const double rho = sqrt((du / ea) * (du / ea) + (dv / eb) * (dv / eb));
                    if (rho < 1e-6 || sqrt(du * du + dv * dv) * fabs(1 - 1 / rho) > tol) continue;
                    const double f[4] = { du * du, dv * dv, du, dv };
                    for (int i = 0; i < 4; i++) {
                        M[i][4] += f[i];
                        for (int j = 0; j < 4; j++) M[i][j] += f[i] * f[j];
                    }
                    n++;
                }
                double sol[4];
                fitted = n >= 8 && solve4(M, sol) && sol[0] > 0 && sol[1] > 0;
                if (!fitted) break;
                ex -= sol[2] / (2 * sol[0]);
                ey -= sol[3] / (2 * sol[1]);
                const double K = 1 + sol[2] * sol[2] / (4 * sol[0]) + sol[3] * sol[3] / (4 * sol[1]);
                fitted = K > 0;
                if (!fitted) break;
                ea = sqrt(K / sol[0]);
                eb = sqrt(K / sol[1]);
                fitted = ea < w && eb < h && fabs(ex - cx) < w && fabs(ey - cy) < h;
            }
            if (!fitted) break;
            Ring g = { ex, ey, ea, eb, colour };
            if (g.a < 2.5 || g.b < 2.5) break;
            // the pixels near that ellipse (within 0.8 of a pixel along its radius), and how far round they go: of the
            // points along it, those with one of them within a pixel
            std::vector<int> on;
            std::vector<Bit8u> mark(w * h, 0);
            for (size_t k = 0; k < left.size(); k++) {
                const double du = left[k] % w - g.cx, dv = left[k] / w - g.cy;
                const double rho = sqrt((du / g.a) * (du / g.a) + (dv / g.b) * (dv / g.b));
                if (rho < 1e-6 || sqrt(du * du + dv * dv) * fabs(1 - 1 / rho) > 0.8) continue;
                on.push_back(left[k]);
                mark[left[k]] = 1;
            }
            int covered = 0;
            for (int k = 0; k < kRingSectors; k++) {
                const double th = k * 6.283185307179586 / kRingSectors;
                const int cu = (int)floor(g.cx + g.a * cos(th) + 0.5), cv = (int)floor(g.cy + g.b * sin(th) + 0.5);
                bool near = false;
                for (int dv = -1; dv <= 1 && !near; dv++) {
                    for (int du = -1; du <= 1 && !near; du++) {
                        const int u = cu + du, v = cv + dv;
                        near = u >= 0 && v >= 0 && u < w && v < h && mark[v * w + u];
                    }
                }
                covered += near;
            }
            if (covered * 100 < kRingSectors * 80) break;
            out.rings.push_back(g);
            for (size_t k = 0; k < on.size(); k++) out.on[on[k]] = (Bit8u)out.rings.size();
        }
    }
}

const Rings &rings_of(const Shape &s, int ground) {
    static Rings cache[kRingCache];
    static int used = 0, next = 0;
    const Bit32u hash = shape_hash(s) ^ (Bit32u)(ground + 1) * 0x9E3779B9u;
    for (int i = 0; i < used; i++) {
        if (cache[i].hash == hash && cache[i].on.size() == s.px.size()) return cache[i];
    }
    Rings &r = cache[next];
    next = (next + 1) % kRingCache;
    if (used < kRingCache) used++;
    r.hash = hash;
    rings_find(s, ground, r);
    return r;
}

// The rings drawn: each one fine pixel wide along its ellipse, where the game's ring has a pixel within one of it.  The
// ellipse's fine pixels in order round it, the corner of any L of three left out (the curve 8-connected, one wide).
void rings_draw(const Target &t, const Shape &s, const Rings &r, int left, int top, int ground) {
    std::vector<int> px, py;
    for (size_t k = 0; k < r.rings.size(); k++) {
        const Ring &g = r.rings[k];
        const double fx = 2 * (left + g.cx) + 1, fy = kSy * (top + g.cy) + kSy / 2, A = 2 * g.a, B = kSy * g.b;
        const int n = 16 + (int)(8 * (A > B ? A : B));
        px.clear();
        py.clear();
        auto flush = [&]() {
            for (size_t i = 0; i < px.size(); i++) {
                const bool corner = i > 0 && i + 1 < px.size() && abs(px[i + 1] - px[i - 1]) == 1 && abs(py[i + 1] - py[i - 1]) == 1;
                if (corner) continue;
                if (i > 0 && (abs(px[i] - px[i - 1]) > 1 || abs(py[i] - py[i - 1]) > 1)) {
                    line_fine(t, px[i - 1], py[i - 1], px[i], py[i], (Bit8u)g.colour);
                }
                plot(t, px[i], py[i], (Bit8u)g.colour);
            }
            px.clear();
            py.clear();
        };
        for (int i = 0; i <= n; i++) {
            const double th = i * 6.283185307179586 / n;
            const double cu = g.cx + g.a * cos(th), cv = g.cy + g.b * sin(th);
            bool near = false;
            for (int dv = -1; dv <= 1 && !near; dv++) {
                for (int du = -1; du <= 1 && !near; du++) {
                    const int u = (int)floor(cu + 0.5) + du, v = (int)floor(cv + 0.5) + dv;
                    near = u >= 0 && v >= 0 && u < s.w && v < s.h && r.on[v * s.w + u] == k + 1;
                }
            }
            if (!near) {
                // (where two rings touch, a pixel they share is one ring's colour: the other passes over any ink there)
                const int u = (int)floor(cu + 0.5), v = (int)floor(cv + 0.5);
                near = u >= 0 && v >= 0 && u < s.w && v < s.h && s.px[v * s.w + u] >= 0 && s.px[v * s.w + u] != ground;
            }
            if (!near) {
                flush();
                continue;
            }
            const int X = (int)floor(fx + A * cos(th) + 0.5), Y = (int)floor(fy + B * sin(th) + 0.5);
            if (!px.empty() && X == px.back() && Y == py.back()) continue;
            px.push_back(X);
            py.push_back(Y);
        }
        flush();
    }
}

// Line art of one pixel's width: each pixel joined to its neighbours' middles, its rings drawn round.
void shape_lines(const Target &t, const Shape &s, int x, int y) {
    static const int kDx[4] = { 1, 0, 1, -1 }, kDy[4] = { 0, 1, 1, 1 };
    const int left = x - s.left, top = y - s.top;
    const Rings &r = rings_of(s, -1);
    rings_draw(t, s, r, left, top, -1);
    for (int v = 0; v < s.h; v++) {
        for (int u = 0; u < s.w; u++) {
            const int c = s.px[v * s.w + u];
            if (c < 0) continue;
            const bool ring = r.on[v * s.w + u] != 0;
            if (!ring) line(t, left + u, top + v, left + u, top + v, (Bit8u)c);
            for (int k = 0; k < 4; k++) {
                const int nu = u + kDx[k], nv = v + kDy[k];
                if (nu < 0 || nu >= s.w || nv >= s.h || s.px[nv * s.w + nu] < 0) continue;
                if (ring && r.on[nv * s.w + nu]) continue;
                if (kDx[k] && kDy[k] && (s.px[v * s.w + nu] >= 0 || s.px[nv * s.w + u] >= 0)) continue;
                line(t, left + u, top + v, left + nu, top + nv, (Bit8u)c);
            }
        }
    }
}

}  // namespace armart
}  // namespace wc
