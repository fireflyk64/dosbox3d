/*
 *  Terminal Velocity: the rigs' pilot (wcnet_tv.h; docs/terminalvelocity.md section 4).  Key-script items, for tests
 *  and the videos, flown at the level loop's frame top (before the game reads its keys):
 *
 *    !tvfly=obj:<index>          turn towards that object (hit points left: else the pilot lets go)
 *    !tvfly=near:<class>[:<r>]   towards the nearest object of that class with hit points within r world units (400),
 *                                chosen again when it dies (7 a craft, 9 an installation, 10 a ground vehicle)
 *    !tvfly=partner[:<right>:<up>:<back>]   on the other pilot's wing (world units in its own frame: 4, 0, 6)
 *    !tvfly=head:<deg>:<deg>     a heading and a pitch (up positive)
 *    !tvfly=tun:<n>              into tunnel n's mouth (the game's own tunnel table: flown in level, no climbing over hills
 *                                in the last stretch)
 *    !tvtunnel=<n>               into tunnel n at once, by the game's own way in (its enter function, at a frame's top)
 *    !tvtunnelend                in a tunnel: the ship moved to its far end (the game's own way out takes it there)
 *    !tvfly=off
 *    !tvfire=1 / 0               fire whenever the target is within 140 units and within 3 degrees of the nose
 *    !tvfire=hold / 0            fire all the time
 *    !tvburn=<seconds>           hold the afterburner's key that long (it burns only with fuel: TRIBURN gives some)
 *    !tvgod=1 / 0                the shield kept full (a take that must not end in a death)
 *    !tvfuel=<n>                 the afterburner's fuel set (a pickup gives 120)
 *    !tvdone                     the level done, as the exit flown (TRINEXT's doing)
 *    !tvdie                      the shield gone: the player's ship dies (the game asks "Retry Mission")
 *
 *  A target on the ground is flown at from above: the aim held kAbove world units over it until within kDive of it
 *  (hills between: the game's terrain is not looked at).
 *
 *  It flies as a pilot would look flying: the nose turned at a rate eased by how far it has to go (at most 90 degrees
 *  a second across, 50 up and down), banked into the turn, level again on a straight line; the game's own speed and
 *  movement.  The angles are the game's: x the pitch (0x10000 a turn, negative nose up), y the roll, z the heading
 *  (0 along +z, growing towards +x).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "wcnet_tv.h"
#include "wcnet_tv_sig.h"
#include "wcnet_log.h"
#include "pic.h"

namespace wc {

namespace {

enum Mode { M_OFF, M_OBJ, M_NEAR, M_PARTNER, M_HEAD, M_TUN };
Mode g_mode = M_OFF;
int g_obj = -1, g_class = -1;
double g_range = 400;
double g_wing[3] = { 4, 0, 6 };
double g_head = 0, g_pitch = 0;     // (radians, M_HEAD)
int g_fire = 0;                     // 1 when lined up, 2 always
double g_burnUntil = -1e9;
double g_roll = 0;
double g_nearAt = -1e9;
bool g_wroteFire = false, g_wroteBurn = false;
bool g_god = false;
bool g_tunDive = false;             // (M_TUN: over the mouth, diving into it)
const double kAbove = 22.0, kDive = 70.0;
const double kUnit = 65536.0;
const double kTurn = 65536.0 / (2 * M_PI);

double wrap(double a) {
    while (a > M_PI) a -= 2 * M_PI;
    while (a < -M_PI) a += 2 * M_PI;
    return a;
}
double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

bool object_pos(int i, double out[3]) {
    const Bit32u o = tv_object_addr(i);
    if (!o || (Bit32s)tv_r32(o + 0x1c) <= 0) return false;
    for (int k = 0; k < 3; k++) out[k] = (double)(Bit32s)tv_r32(o + 4 * k) / kUnit;
    return true;
}

int nearest_of(int cls, const double p[3], double range) {
    int best = -1;
    double bd = range * range;
    const int n = tv_object_count();
    for (int i = 0; i < n; i++) {
        if (tv_object_class(i) != cls) continue;
        double q[3];
        if (!object_pos(i, q)) continue;
        const double d2 = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]);
        if (d2 < bd) {
            bd = d2;
            best = i;
        }
    }
    return best;
}

void key(int scanDataId, bool down, bool &wrote) {
    if (!down && !wrote) return;            // (never lifts a key the player holds)
    const Bit32u table = tv_data(tvsig::D_KEY_TABLE);
    const Bit32u code = tv_r32(tv_data(scanDataId));
    if (!table || code == 0 || code > 0x7f) return;
    const Bit32u at = table + code;
    const Bit32u w = tv_r32(at & ~3u);
    const int sh = (int)(at & 3) * 8;
    tv_w32(at & ~3u, (w & ~(0xffu << sh)) | ((down ? 1u : 0u) << sh));
    wrote = down;
}

}  // namespace

void tv_pilot_frame() {
    if (g_mode == M_OFF && !g_fire && !g_wroteFire && !g_wroteBurn && !g_god && PIC_FullIndex() >= g_burnUntil) return;
    TvShip s;
    if (!tv_player(&s)) return;
    const double now = PIC_FullIndex();
    key(tvsig::D_BURN_KEY, now < g_burnUntil, g_wroteBurn);
    if (g_god) tv_w32(tv_data(tvsig::D_PLAYER_SHIELD), 0xFFFF);
    const double p[3] = { s.pos[0] / kUnit, s.pos[1] / kUnit, s.pos[2] / kUnit };
    double t[3];
    bool have = false;
    if (g_mode == M_OBJ) {
        have = object_pos(g_obj, t);
        if (!have) {
            wclog(1, "Terminal Velocity pilot: object %d gone: the stick let go", g_obj);
            g_mode = M_OFF;
        }
    } else if (g_mode == M_NEAR) {
        if (g_obj < 0 || !object_pos(g_obj, t) || now - g_nearAt > 4000) {
            const int was = g_obj;
            g_obj = nearest_of(g_class, p, g_range);
            g_nearAt = now;
            if (g_obj != was) wclog(1, "Terminal Velocity pilot: towards object %d (class %d) '%s'", g_obj, g_class, tv_object_name(g_obj).c_str());
        }
        have = g_obj >= 0 && object_pos(g_obj, t);
    } else if (g_mode == M_TUN) {
        // A tunnel's mouth is a hole in the ground: the game takes the ship in when it is within a unit under the
        // mouth's plane (the ground's slope there), inside 0.7 of its half sizes, diving more than 45 degrees (its
        // entrance test, 0x42490 from the loop 0x42540).  So: to a point 30 units short of it and 36 above, then
        // straight down at it.
        const Bit32u mouth = tv_data(tvsig::D_TUNNEL_MOUTHS) + (Bit32u)g_obj * 0x173C;
        double m[3];
        for (int k = 0; k < 3; k++) m[k] = (double)(Bit32s)tv_r32(mouth + 4 * k) / kUnit;
        have = tv_space() == 0 && g_obj < (int)tv_r32(tv_data(tvsig::D_TUNNEL_COUNT));
        if (!have) {
            wclog(1, "Terminal Velocity pilot: %s: the stick let go", tv_space() ? "in a tunnel" : "no such tunnel");
            g_mode = M_OFF;
        } else {
            const double dx = m[0] - p[0], dz = m[2] - p[2], a = sqrt(dx * dx + dz * dz), above = p[1] - m[1];
            if (!g_tunDive && a < 36 && above > 1.1 * a) {
                g_tunDive = true;
                wclog(1, "Terminal Velocity pilot: over tunnel %d's mouth (%.0f units off, %.0f above, its half sizes %.1f %.1f): "
                      "diving", g_obj, a, above, (Bit32s)tv_r32(mouth + 0x84) / 256.0, (Bit32s)tv_r32(mouth + 0x88) / 256.0);
            } else if (g_tunDive && (a > 45 || above < -3)) {
                g_tunDive = false;
                wclog(1, "Terminal Velocity pilot: missed tunnel %d's mouth (%.0f units off, %.0f above): round again", g_obj, a, above);
            }
            for (int k = 0; k < 3; k++) t[k] = m[k];
            if (g_tunDive) {
                t[1] -= 4;
            } else if (a > 1e-3) {
                t[0] -= dx / a * 30;
                t[2] -= dz / a * 30;
                t[1] += 36;
            }
        }
    } else if (g_mode == M_PARTNER) {
        TvShip q;
        if (tv_session_partner(&q)) {
            const double h = q.ang[2] / kTurn;
            // its right, up, back in the world (the heading alone: a wingman keeps level)
            const double right[3] = { cos(h), 0, -sin(h) }, back[3] = { -sin(h), 0, -cos(h) };
            for (int k = 0; k < 3; k++) {
                t[k] = q.pos[k] / kUnit + right[k] * g_wing[0] + back[k] * g_wing[2] + (k == 1 ? g_wing[1] : 0);
            }
            // aim at a point ahead of the slot, so the turn eases onto it
            const double fwd = 10;
            for (int k = 0; k < 3; k++) t[k] += -back[k] * fwd;
            have = true;
        }
    }
    double want_h, want_p;
    const double cur_h = s.ang[2] / kTurn, cur_p = -(Bit32s)(Bit16s)(Bit16u)s.ang[0] / kTurn;
    double dist = 0;
    if (g_mode == M_HEAD) {
        want_h = g_head;
        want_p = g_pitch;
    } else if (have) {
        double dx = t[0] - p[0], dy = t[1] - p[1], dz = t[2] - p[2];
        dist = sqrt(dx * dx + dy * dy + dz * dz);
        const double across = sqrt(dx * dx + dz * dz);
        if (g_mode != M_PARTNER && g_mode != M_TUN && across > kDive * 0.4) {
            dy += kAbove * clampd((across - kDive * 0.4) / (kDive * 0.6), 0, 1);     // (over the hills, then down onto it)
        }
        want_h = atan2(dx, dz);
        want_p = atan2(dy, sqrt(dx * dx + dz * dz));
    } else {
        key(tvsig::D_FIRE_KEY, g_fire == 2, g_wroteFire);
        return;
    }
    double dt = tv_r32(tv_data(tvsig::D_FRAME_TIME)) / kUnit;
    dt = clampd(dt, 0.0, 0.2);
    const double dh = wrap(want_h - cur_h), dp = clampd(want_p, -1.1, 1.1) - cur_p;
    const double rh = clampd(2.2 * dh, -1.57, 1.57), rp = clampd(2.2 * dp, -0.87, 0.87);
    const double nh = cur_h + rh * dt, np = cur_p + rp * dt;
    // banked into the turn as a pilot would: a hard turn some 35 degrees
    const double wantRoll = clampd(rh / 1.57, -1, 1) * 0.61;
    g_roll += (wantRoll - g_roll) * clampd(3.0 * dt, 0, 1);
    // The ship's attitude is its matrix (the player's place + 0x1C, row by row, 1.0 = 0x10000): its columns the
    // ship's right, up and forward in the world; the game takes the three angles from it.  A right bank (roll
    // positive here) puts the right wing down.
    const Bit32u base = tv_data(tvsig::D_PLAYER_POS);
    const double ch = cos(nh), sh = sin(nh), cp = cos(np), sp = sin(np), cr = cos(g_roll), sr = sin(g_roll);
    const double f[3] = { sh * cp, sp, ch * cp };
    const double r0[3] = { ch, 0, -sh }, u0[3] = { -sh * sp, cp, -ch * sp };
    double rt[3], up[3];
    for (int k = 0; k < 3; k++) {
        rt[k] = r0[k] * cr - u0[k] * sr;
        up[k] = u0[k] * cr + r0[k] * sr;
    }
    // (the game's own attitude, floats: it makes the angles and the fixed-point matrix from it each frame)
    const Bit32u att = tv_data(tvsig::D_ATTITUDE) + 4;
    for (int row = 0; row < 3; row++) {
        const float v[3] = { (float)rt[row], (float)up[row], (float)f[row] };
        for (int c = 0; c < 3; c++) {
            Bit32u bits;
            memcpy(&bits, &v[c], 4);
            tv_w32(att + 12 * row + 4 * c, bits);
        }
    }
    const Bit32u spin = tv_data(tvsig::D_LOCAL_SPIN);
    for (int k = 0; k < 3; k++) tv_w32(spin + 4 * k, 0);
    const double off = sqrt(dh * dh + (want_p - np) * (want_p - np));
    const bool lined = g_mode != M_PARTNER && g_mode != M_HEAD && dist < 140 && off < 0.052;
    key(tvsig::D_FIRE_KEY, g_fire == 2 || (g_fire == 1 && lined), g_wroteFire);
}

bool tv_pilot_item(const std::string &item, double t) {
    if (item.compare(0, 7, "!tvfly=") == 0) {
        const std::string a = item.substr(7);
        if (a == "off") {
            g_mode = M_OFF;
        } else if (a.compare(0, 4, "obj:") == 0) {
            g_mode = M_OBJ;
            g_obj = atoi(a.c_str() + 4);
        } else if (a.compare(0, 5, "near:") == 0) {
            g_mode = M_NEAR;
            g_class = atoi(a.c_str() + 5);
            const char *c = strchr(a.c_str() + 5, ':');
            g_range = c ? atof(c + 1) : 400;
            g_obj = -1;
            g_nearAt = -1e9;
        } else if (a.compare(0, 7, "partner") == 0) {
            g_mode = M_PARTNER;
            if (a.size() > 8) sscanf(a.c_str() + 8, "%lf:%lf:%lf", &g_wing[0], &g_wing[1], &g_wing[2]);
        } else if (a.compare(0, 4, "tun:") == 0) {
            g_mode = M_TUN;
            g_obj = atoi(a.c_str() + 4);
            g_tunDive = false;
        } else if (a.compare(0, 5, "head:") == 0) {
            double h = 0, pd = 0;
            sscanf(a.c_str() + 5, "%lf:%lf", &h, &pd);
            g_mode = M_HEAD;
            g_head = h * M_PI / 180;
            g_pitch = pd * M_PI / 180;
        }
        wclog(1, "key script %.1fs: Terminal Velocity pilot: %s", t, a.c_str());
        return true;
    }
    if (item.compare(0, 8, "!tvfire=") == 0) {
        const std::string a = item.substr(8);
        g_fire = a == "hold" ? 2 : atoi(a.c_str()) ? 1 : 0;
        return true;
    }
    if (item.compare(0, 10, "!tvtunnel=") == 0) {
        tv_queue_call(TV_AT_FRAME, tv_fn(TV_FN_TUNNEL_ENTER), std::vector<Bit32u>(1, (Bit32u)atoi(item.c_str() + 10)),
                      std::vector<Bit32u>(), NULL, 0);
        wclog(1, "key script %.1fs: Terminal Velocity: into tunnel %d", t, atoi(item.c_str() + 10));
        return true;
    }
    if (item == "!tvtunnelend") {
        if (tv_space() == 0) return true;
        const Bit32u len = tv_r32(tv_data(tvsig::D_TUNNEL_LENGTH));
        tv_w32(tv_data(tvsig::D_PLAYER_POS) + 8, (len << 20) + 0x8000);
        wclog(1, "key script %.1fs: Terminal Velocity: to the tunnel's end (%u units)", t, len * 16);
        return true;
    }
    if (item == "!tvdone") {
        tv_w32(tv_data(tvsig::D_LEVEL_DONE), 1);
        wclog(1, "key script %.1fs: Terminal Velocity: the level done", t);
        return true;
    }
    if (item == "!tvdie") {
        g_god = false;
        tv_w32(tv_data(tvsig::D_PLAYER_SHIELD), 0);
        wclog(1, "key script %.1fs: Terminal Velocity: the shield gone", t);
        return true;
    }
    if (item.compare(0, 8, "!tvfuel=") == 0) {
        tv_w32(tv_data(tvsig::D_BURN_FUEL), (Bit32u)atoi(item.c_str() + 8));
        return true;
    }
    if (item.compare(0, 7, "!tvgod=") == 0) {
        g_god = atoi(item.c_str() + 7) != 0;
        return true;
    }
    if (item.compare(0, 8, "!tvburn=") == 0) {
        g_burnUntil = PIC_FullIndex() + atof(item.c_str() + 8) * 1000;
        wclog(1, "key script %.1fs: Terminal Velocity pilot: the afterburner held %.1f s (fuel %d)", t, atof(item.c_str() + 8),
              (int)tv_r32(tv_data(tvsig::D_BURN_FUEL)));
        return true;
    }
    return false;
}

}  // namespace wc
