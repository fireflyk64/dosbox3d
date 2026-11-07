/* bot.c: a test pilot for the native game (host/play.c; a dev aid, never in RV.EXE).  RV_BOT=1 flies the player's
 * fighter through the player's own controls only: the mouse pointer for the stick (flight.c's steering by the
 * pointer's distance from the middle), the keys for the trigger (Space), the afterburner (Tab), a missile (Enter),
 * the comm menu (C and its digits) and the autopilot (A); the throttle as the keys set it and the target and the
 * launcher as T and W choose them.  What it knows is what the radar and the target display tell a pilot: where every
 * ship is, how it flies, its shields.  The user's ask (2026-11-07): can Kurasawa 2's equivalent (RIGEL2) be won,
 * how often, a recording of a win and of a loss.
 *
 *   RV_BOT=1            fly
 *   RV_BOT_SEED=N       the flight's gameplay dice (randomSeed at its first step) and the pilot's own
 *   RV_BOT_AIM=D        the pilot's aim wanders by up to D tenths of a degree (default 0: as steady as the stick)
 *   RV_BOT_TAUNT=0      never taunt (default 1: taunt an enemy at the escort it is not fighting)
 *   RV_BOT_MISSILES=0   never fire a missile
 *   RV_BOT_AFTER=S      seconds to go on after the flight ends (default 4), then exit with 0 won, 1 lost
 *   RV_BOT_PAUSE=S      seconds of a breath after each kill before the next target (the user: "If the ai takes 2 second
 *                       victory pauses between ships does that turn tables")
 *   RV_BOT_PASSIVE=1    at the escort: nothing, and the player not hurt (the old game's CHEAT rig): the escort's own odds
 *   RV_BOT_ESC_END=S    end the run S seconds after the escort is lost (exit 1)
 *   RV_BOT_WATCH=P      the escort under P percent of itself: turn to look at it (a recording's)
 *   RV_BOT_DEBUG=1      a line a second: what it is doing, where, how it stands
 * Each flight ends with a line "bot: result ..." on stderr: how it ended and the pilot's numbers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "rvhooks.h"
#include "world.h"
#include "logic.h"
#include "panel.h"
#include "damage.h"
#include "cockpit.h"
#include "shots.h"
#include "campaign.h"
#include "rocks.h"
#include "hle.h"
#include "bot.h"

/* scan codes (set 1) */
enum { K_ESC = 0x01, K_1 = 0x02, K_2 = 0x03, K_3 = 0x04, K_TAB = 0x0F, K_ENTER = 0x1C, K_A = 0x1E, K_C = 0x2E,
       K_SPACE = 0x39 };

static struct {
    int on, inited, aim_tenths, taunt, missiles, after_s;
    unsigned seed, rng;
    uint16_t frame;                     /* the last frameCounter acted on */
    int in_flight, flight_frames, over_frames, done;
    int held_space, held_tab;
    int tap_key, tap_up;                /* a key going down this frame, up the next */
    int queue[16], nq;                  /* keys (or -100 - slot: the target set to that slot) to come, one a frame */
    int wiggle;
    int ap_wait;                        /* frames to wait after the autopilot key */
    int ap_tries, ap_last;
    int target, target_frames, hold_target;
    int fight;                          /* enemies about: a fight on (the wingman told once a fight) */
    int taunted_at[NUM_SHIP_SLOTS];
    int missile_at;                     /* the frame of the last missile */
    int landing_asked, landing_frames;
    int jink;                           /* frames of a break left */
    double aim_dx, aim_dy;              /* the aim's wander now, degrees */
    /* the numbers */
    long shots, hits, missiles_fired, missile_hits;
    long hits_taken;
    int escort_slot;
    long escort_start, escort_least;
    int result_said, debug, aiming;
    int arrived;                        /* the escort's attackers seen: the arrival's geometry kept */
    double arr_esc, arr_near, arr_near_esc;
    int arr_first_at_esc;               /* seconds after the arrival an attacker came within 1,500 km of the escort */
    int passive, passive_told, esc_dead_at;
    int pause_s, pause, target_alive;
    int esc_end_s, esc_lost_frame;      /* RV_BOT_ESC_END: end the run that many seconds after the escort's loss */
    int watch;                          /* RV_BOT_WATCH: the escort nearly gone, turn to see it (a recording's) */   /* RV_BOT_PAUSE: seconds after a kill before the next (a pilot's breath) */   /* RV_BOT_PASSIVE: at the escort, nothing (the wingman kept in formation) */
    long tgt_esc, tgt_player, tgt_other, tgt_none, esc_hits, esc_hits_60;   /* the attackers' targets, frame by frame */
    long esc_shots_60;                  /* bolts fired by ships targeting the escort, the first 60 s */
    uint64_t done_us;
    const char *mode;
} B;

static unsigned rnd(void)
{
    B.rng = B.rng * 1103515245u + 12345u;
    return (B.rng >> 16) & 0x7FFF;
}

typedef struct { double x, y, z; } V;
static V v3(double x, double y, double z) { V v = { x, y, z }; return v; }
static V vsub(V a, V b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V vadd(V a, V b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V vmul(V a, double k) { return v3(a.x * k, a.y * k, a.z * k); }
static double vdot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double vlen(V a) { return sqrt(vdot(a, a)); }
static V pos(int s) { return v3(gPositionVector[s].x / 256.0, gPositionVector[s].y / 256.0, gPositionVector[s].z / 256.0); }
static V vel(int s) { return v3(gVelocityVector[s].x / 256.0, gVelocityVector[s].y / 256.0, gVelocityVector[s].z / 256.0); }   /* km a step */
static V fwd(int s) { return v3(gOrientationFrontVector[s].x / 256.0, gOrientationFrontVector[s].y / 256.0, gOrientationFrontVector[s].z / 256.0); }
static V upv(int s) { return v3(gOrientationUpVector[s].x / 256.0, gOrientationUpVector[s].y / 256.0, gOrientationUpVector[s].z / 256.0); }
static V rightv(int s) { return v3(gOrientationRightVector[s].x / 256.0, gOrientationRightVector[s].y / 256.0, gOrientationRightVector[s].z / 256.0); }
static double km(int a, int b) { return vlen(vsub(pos(a), pos(b))); }

const LzRock __far *rocks_bot_rock(int i);
int rocks_bot_count(void);
int rocks_bot_field(int i, long *x, long *y, long *z, long *r_km);

/* the field p is within `margin` km of (its middle and radius), 0 none */
static int field_near(V p, double margin, V *c, double *r)
{
    int i;
    long x, y, z, rk;
    for (i = 0; rocks_bot_field(i, &x, &y, &z, &rk); i++) {
        V m = v3(x / 256.0, y / 256.0, z / 256.0);
        if (vlen(vsub(p, m)) < rk + margin) {
            *c = m;
            *r = rk;
            return 1;
        }
    }
    return 0;
}

/* a field the way from the player to `to` (km, world) passes within `margin` of: its middle and radius */
static int field_on_way(V to, double margin, V *c, double *r)
{
    int i;
    long x, y, z, rk;
    V a = pos(0), d = vsub(to, a);
    double L = vlen(d);
    for (i = 0; L > 1 && rocks_bot_field(i, &x, &y, &z, &rk); i++) {
        V m = v3(x / 256.0, y / 256.0, z / 256.0), u = vmul(d, 1 / L);
        double t = fmax(0, fmin(L, vdot(vsub(m, a), u)));
        if (vlen(vsub(m, vadd(a, vmul(u, t)))) < rk + margin) {
            *c = m;
            *r = rk;
            return 1;
        }
    }
    return 0;
}

/* a rock about to strike: the way to turn the nose (km, relative), 1 when there is one */
static int rock_threat(V *away)
{
    int i, n = rocks_bot_count(), hit = 0;
    double best = 1e18;
    for (i = 0; i < n; i++) {
        const LzRock __far *k = rocks_bot_rock(i);
        V p = vsub(v3(k->x / 256.0, k->y / 256.0, k->z / 256.0), pos(0));
        V w = vsub(v3(k->vx / 256.0, k->vy / 256.0, k->vz / 256.0), vel(0));
        double ww = vdot(w, w), t, room = k->r8 * 8.0 + 220;
        V m;
        if (vlen(p) > 4000 || ww < 1e-6)
            continue;
        t = -vdot(p, w) / ww;
        if (t < 0 || t > 75)
            continue;
        m = vadd(p, vmul(w, t));
        if (vlen(m) < room && t < best) {
            double ml = vlen(m);
            V side = ml > 1 ? vmul(m, -1 / ml) : upv(0);
            best = t;
            *away = vadd(vmul(side, 1500), vmul(fwd(0), 800));
            hit = 1;
        }
    }
    return hit;
}

static int alive_ship(int s)
{
    return s > 0 && s <= RV_MAX_SHIP_SLOT && slot_is_ship((int16_t)s) && shipAiState[s] != 9;
}
static int enemy(int s) { return alive_ship(s) && commAnimInfo[s] != commAnimInfo[0]; }
static int fighter(int s) { return maybeEntityType[s] == ET_SHIP; }
static double radius_of(int s) { const RvShipType __far *t = slot_type((int16_t)s); return t ? t->radius_km : 100; }

/* the escort: the capital ship of the player's side, not the home carrier, farthest from home (a capital ship kept at
 * the launch is no escort) within 30,000 km of the player */
static int home_slot(void);
static int escort_slot(void)
{
    int s, best = -1, h = home_slot();
    double bd = -1;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (alive_ship(s) && commAnimInfo[s] == commAnimInfo[0] && maybeEntityType[s] == ET_CAPSHIP
            && !logic_is_home((int16_t)s) && km(0, s) < 30000 && (h < 0 || km(h, s) > 30000) && (h < 0 || km(h, s) > bd)) {
            bd = h < 0 ? 0 : km(h, s);
            best = s;
        }
    return best;
}
static int home_slot(void)
{
    int s;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (logic_is_home((int16_t)s))
            return s;
    return -1;
}

static void key_hold(int scan, int *held, int want)
{
    if (want != *held) {
        hle_key(scan, want);
        *held = want;
    }
}
static void queue(int k)
{
    if (B.nq < 16)
        B.queue[B.nq++] = k;
}

/* the pointer for a steer (-9 .. 9) on each axis: the middle of that steer's band (flight.c STEER_DEAD_*, STEER_FULL_*),
 * wiggled 4 pixels either side each frame so the driver always sees it move (a key's press pins the pointer until it
 * moves: input.c KEYBOARD_SLACK) */
static void steer_to(int sx, int sy)
{
    const RvCockpit __far *c = cockpit_record();
    int cx = 320, cy = c ? (c->open_y0 + c->open_y1) / 2 : 200, ox, oy;
    ox = sx == 0 ? 0 : sx >= 9 ? 130 : sx <= -9 ? -130 : (sx * 120 + (sx > 0 ? 60 : -60)) / 9;
    oy = sy == 0 ? 0 : sy >= 9 ? 88 : sy <= -9 ? -88 : (sy * 80 + (sy > 0 ? 40 : -40)) / 9;
    B.wiggle = !B.wiggle;
    hle_mouse_at(cx + ox + (B.wiggle ? 4 : -4), cy - oy);
}

/* steer toward a point (km, world) relative to the player: returns the angle off the nose, degrees */
static double steer_at(V d, int hard)
{
    V F = fwd(0), U = upv(0), R = rightv(0);
    double f, u, r, yaw, pitch, off;
    const RvShipType __far *t = slot_type(0);
    double dps = t ? t->yaw_dps : 100, per = dps / 180.0;   /* degrees a step per steer unit */
    int sx, sy;
    /* the point as the pilot sees it: off by the aim's wander (a turn of the way by aim_dx right, aim_dy up) */
    if (B.aiming && (B.aim_dx || B.aim_dy)) {
        double L = vlen(d), ax = B.aim_dx * M_PI / 180, ay = B.aim_dy * M_PI / 180;
        V F0 = vmul(d, 1 / fmax(1e-6, L));
        V Rp = vsub(R, vmul(F0, vdot(R, F0))), Up = vsub(U, vmul(F0, vdot(U, F0)));
        double rl = vlen(Rp), ul = vlen(Up);
        if (rl > 1e-3 && ul > 1e-3)
            d = vmul(vadd(F0, vadd(vmul(Rp, tan(ax) / rl), vmul(Up, tan(ay) / ul))), L);
    }
    f = vdot(d, F), u = vdot(d, U), r = vdot(d, R);
    yaw = atan2(r, f) * 180 / M_PI, pitch = atan2(u, f) * 180 / M_PI;
    off = acos(fmax(-1, fmin(1, f / fmax(1e-6, vlen(d))))) * 180 / M_PI;
    if (f < 0 || hard) {                /* (behind: the stick hard over toward it) */
        if (fabs(r) >= fabs(u) * 0.4) {
            sx = r >= 0 ? 9 : -9;
            sy = (int)fmax(-9, fmin(9, pitch / per * 0.7));
        } else {
            sx = (int)fmax(-9, fmin(9, yaw / per * 0.7));
            sy = u >= 0 ? 9 : -9;
        }
    } else {
        sx = (int)lround(fmax(-9, fmin(9, yaw / per * 0.65)));
        sy = (int)lround(fmax(-9, fmin(9, pitch / per * 0.65)));
    }
    steer_to(sx, sy);
    return off;
}

/* where to aim at slot t: its lead point for a bolt of `kps` (the shot carries the ship's own motion) */
static V lead_of(int t, double kps)
{
    V p = vsub(pos(t), pos(0)), w = vsub(vel(t), vel(0));
    double bolt = kps * 256.0 / KPS_HZ / 256.0, tt = vlen(p) / bolt;   /* km a step */
    int i;
    for (i = 0; i < 3; i++)
        tt = vlen(vadd(p, vmul(w, tt))) / bolt;
    return vadd(p, vmul(w, tt));
}

static int pick_target(void)
{
    int s, best = -1, esc = escort_slot();
    double bs = 1e18;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++) {
        double d, sc;
        if (!enemy(s) || (rvFlags[s] & 16))
            continue;
        d = km(0, s);
        sc = d;
        if (esc > 0) {
            double de = km(s, esc);
            if (de < 9000)
                sc = fmin(sc, de * 0.4 + d * 0.3);
            if (rvTarget[s] == esc)
                sc -= 2500;
        }
        if (maybeEntityType[s] == ET_CAPSHIP)
            sc += 20000;                /* (a fighter's guns are for fighters) */
        if (rvTarget[s] == 0 && d < 2500)
            sc -= 1500;
        if (s == B.target)
            sc -= 2000;
        if (sc < bs) {
            bs = sc;
            best = s;
        }
    }
    return best;
}

static int nearest_enemy_km(double *dist)
{
    int s, best = -1;
    double bd = 1e18;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (enemy(s) && !(rvFlags[s] & 16) && km(0, s) < bd) {
            bd = km(0, s);
            best = s;
        }
    *dist = bd;
    return best;
}

static int navs_left(void)
{
    int n, left = 0;
    int32_t k;
    for (n = 1; n < 16; n++)
        if (logic_nav_km((int16_t)n, &k) && !logic_nav_seen((int16_t)n))
            left++;
    return left;
}

/* the launcher of a seeker kind chosen (as W does), 1 when there is one with rounds */
static int choose_launcher(int lock)
{
    int n;
    for (n = 0; n < 8; n++) {
        int g = logic_launcher();
        if (g < 0)
            return 0;
        if (weapon(GUN_TYPE(0, g))->lock == lock)
            return 1;
        (void)logic_missile_next();
    }
    return 0;
}

static int shield_pct(int s, int k)
{
    int m = SHIELD_MAX(s, k);
    return m ? SHIELD(s, k) * 100 / m : 0;
}

static void fight_step(void)
{
    int t, esc = escort_slot();
    if (B.target > 0 && B.target_alive && !alive_ship(B.target) && B.pause_s)
        B.pause = B.pause_s * LOGIC_HZ;     /* (its target gone: a breath before the next) */
    B.target_alive = B.target > 0 && alive_ship(B.target);
    if (B.pause) {
        B.pause--;
        B.mode = "pause";
        key_hold(K_SPACE, &B.held_space, 0);
        key_hold(K_TAB, &B.held_tab, 0);
        steer_to(0, 0);
        return;
    }
    t = pick_target();
    int ab_ok = g_sys.fuel > 150;
    double d, off;
    V aim;
    if (t != B.target) {
        B.target = t;
        B.target_frames = 0;
    }
    B.target_frames++;
    if (t < 0)
        return;
    if (!B.hold_target)
        g_sys.target = (int16_t)t;      /* (T round to it) */
    if (!B.fight) {
        int16_t w[1];
        B.fight = 1;
        if (logic_wingmen(w, 1))        /* the wingman: break and attack */
            queue(K_C), queue(K_1), queue(K_2);
    }
    d = km(0, t);
    aim = lead_of(t, 2000);
    /* a rock about to strike: away from it, slower (a rock's harm is the square of the closing speed) */
    {
        V away;
        if (rock_threat(&away)) {
            B.mode = "rock";
            steer_at(away, 0);
            setSpeed[0] = slot_type(0)->cruise_kps;
            key_hold(K_TAB, &B.held_tab, 0);
            key_hold(K_SPACE, &B.held_space, 0);
            return;
        }
    }
    /* in or by an asteroid field: draw them out of it (no chase into it; out at speed) */
    {
        V c;
        double r;
        if (field_near(pos(0), 2500, &c, &r)) {
            V out = vsub(pos(0), c);
            double din = vlen(out);
            int inside = din < r + 600;
            int aim_in = vlen(vsub(vadd(pos(0), aim), c)) < r + 1500;
            if (inside || (aim_in && d > 1500)) {
                double off2;
                B.mode = inside ? "field-out" : "field-lure";
                off2 = steer_at(vmul(out, 3000 / fmax(1, din)), 0);
                setSpeed[0] = slot_type(0)->max_kps;
                key_hold(K_TAB, &B.held_tab, ab_ok && off2 < 30);
                {
                    V F = fwd(0), p = vsub(pos(t), pos(0));
                    double o = acos(fmax(-1, fmin(1, vdot(p, F) / fmax(1, vlen(p))))) * 180 / M_PI;
                    key_hold(K_SPACE, &B.held_space, d < 3500 && o < 3);
                }
                return;
            }
        }
    }
    /* a ship close ahead: away from it (a ram costs the front's shield and armour) */
    {
        int s;
        for (s = 1; s <= RV_MAX_SHIP_SLOT; s++) {
            V p, rv;
            double dd, room;
            if (!alive_ship(s))
                continue;
            p = vsub(pos(s), pos(0));
            dd = vlen(p);
            room = radius_of(s) + radius_of(0) + (maybeEntityType[s] == ET_CAPSHIP ? 900 : 250);
            rv = vsub(vel(s), vel(0));
            if (dd < room && vdot(p, rv) < 0 && vdot(p, fwd(0)) > dd * 0.3) {
                V away = vsub(vmul(upv(0), dd), p);   /* (up and over it) */
                B.mode = "ram-avoid";
                steer_at(away, 1);
                setSpeed[0] = slot_type(0)->max_kps;
                key_hold(K_SPACE, &B.held_space, 0);
                return;
            }
        }
    }
    /* a break when one sits on the tail and the rear shield is going */
    if (!B.jink) {
        int s;
        for (s = 1; s <= RV_MAX_SHIP_SLOT; s++) {
            V p;
            if (!enemy(s) || !fighter(s))
                continue;
            p = vsub(pos(s), pos(0));
            if (vlen(p) < 1800 && vdot(p, fwd(0)) < -0.5 * vlen(p) && vdot(fwd(s), vmul(p, -1)) > 0.9 * vlen(p)
                && shield_pct(0, 1) < 35 && (int)(rnd() % 30) == 0)
                B.jink = 20 + (int)(rnd() % 15);
        }
    }
    if (B.jink) {
        V side;
        B.mode = "jink";
        side = rnd() & 1 ? rightv(0) : vmul(rightv(0), -1);
        B.jink--;
        steer_at(vadd(vmul(side, 1000), vmul(upv(0), 400)), 1);
        key_hold(K_TAB, &B.held_tab, ab_ok);
        key_hold(K_SPACE, &B.held_space, 0);
        return;
    }
    B.mode = "attack";
    B.aiming = 1;
    off = steer_at(aim, 0);
    B.aiming = 0;
    /* the guns: the lead point within the target's size of the nose, in reach */
    {
        double miss = vlen(aim) * sin(off * M_PI / 180), r = radius_of(t);
        key_hold(K_SPACE, &B.held_space, vlen(aim) < 3800 && off < 20 && miss < r * 0.9);
    }
    /* the throttle and the burner */
    {
        const RvShipType __far *me = slot_type(0);
        double tk = vlen(vel(t)) * KPS_HZ;   /* (km a step as kps) */
        if (d > 3500 && off < 30) {
            setSpeed[0] = me->max_kps;
            key_hold(K_TAB, &B.held_tab, ab_ok);
        } else {
            key_hold(K_TAB, &B.held_tab, 0);
            if (d < 1200 && off < 60)
                setSpeed[0] = (uint32_t)fmax(120, fmin(me->max_kps, tk * (d < 600 ? 0.7 : 1.0)));
            else
                setSpeed[0] = me->max_kps;
        }
    }
    /* missiles: for the escort's attackers (and anyone once the escort is gone): friend-or-foe at the nearest within
     * 3,000 km, a dumbfire dead ahead within 1,800, image recognition locked */
    if (B.missiles && fighter(t) && B.flight_frames - B.missile_at > 40 && (esc > 0 || B.escort_slot > 0) && B.nq == 0
        && !B.tap_up && !B.hold_target) {
        double nd;
        int ne = nearest_enemy_km(&nd);
        int fire = 0;
        if (ne == t && nd < 3500 && choose_launcher(RV_LOCK_AUTO))   /* (it takes the nearest: ours when it is) */
            fire = 1;
        else if (d < 2200 && off < 6 && choose_launcher(RV_LOCK_NONE))   /* (the dumbfire's aim helps within 15) */
            fire = 1;
        else if (choose_launcher(RV_LOCK_IMAGE) && g_sys.lock && d < 4500)
            fire = 1;
        if (fire) {
            queue(K_ENTER);
            B.missile_at = B.flight_frames;
            B.missiles_fired++;
        }
    }
    /* a taunt: an enemy at the escort that is not ours, now and then */
    if (B.taunt && esc > 0 && B.nq == 0) {
        int s;
        for (s = 1; s <= RV_MAX_SHIP_SLOT; s++) {
            if (s == t || !enemy(s) || !fighter(s) || rvTarget[s] != esc || km(s, esc) > 4000)
                continue;
            if (B.flight_frames - B.taunted_at[s] < 25 * LOGIC_HZ || B.taunted_at[s] < 0)
                continue;
            if (shield_pct(0, 0) + shield_pct(0, 1) < 70)
                continue;
            B.taunted_at[s] = B.flight_frames;
            queue(-100 - s);            /* (T to it) */
            queue(K_C);
            queue(K_2);
            queue(K_1);
            break;
        }
    }
}

static void say_result(void)
{
    if (B.result_said++)
        return;
    fprintf(stderr, "bot: result status %u victory %u escort_lost %d escort %ld/%ld seconds %d shots %ld hits %ld "
            "(%.1f%%) missiles %ld missile_hits %ld kills %u hits_taken %ld hull %d seed %u arrival esc %.0f near %.0f "
            "near_esc %.0f first_at_esc %d esc_dead_at %d targets(60s) esc %ld player %ld other %ld none %ld esc_hits %ld (60s %ld) esc_shots(60s) %ld\n",
            missionStatus, victoryPoints, logic_escort_lost(), B.escort_least, B.escort_start,
            B.flight_frames / LOGIC_HZ, B.shots, B.hits, B.shots ? 100.0 * B.hits / B.shots : 0.0, B.missiles_fired,
            B.missile_hits, logic_splashes, B.hits_taken, g_sys.hull, B.seed, B.arr_esc, B.arr_near, B.arr_near_esc,
            B.arr_first_at_esc, B.esc_dead_at, B.tgt_esc, B.tgt_player, B.tgt_other, B.tgt_none, B.esc_hits, B.esc_hits_60, B.esc_shots_60);
    fflush(stderr);
}

static int escort_health(int s)
{
    return SHIELD(s, 0) + SHIELD(s, 1) + ARMOR(s, 0) + ARMOR(s, 1) + ARMOR(s, 2) + ARMOR(s, 3)
           + (slot_type((int16_t)s) && slot_type((int16_t)s)->hull > damagePoints[s] ? slot_type((int16_t)s)->hull - damagePoints[s] : 0);
}

static void flight_tick(void)
{
    double nd;
    int ne, esc, i;
    if (B.flight_frames == 0) {
        if (B.seed)
            randomSeed = B.seed;
        memset(B.taunted_at, 0, sizeof B.taunted_at);
        B.target = -1;
        B.missile_at = -1000;
        fprintf(stderr, "bot: flight begins, seed %u\n", B.seed);
    }
    B.flight_frames++;
    if (B.flight_frames % 900 == 0) {   /* (how fast the machine runs it) */
        struct timespec ts;
        static double t0;
        double now;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = ts.tv_sec + ts.tv_nsec / 1e9;
        if (!t0)
            t0 = now;
        fprintf(stderr, "bot: %d s flown, %.0f s on the clock; hull %d%%, escort %ld/%ld\n", B.flight_frames / LOGIC_HZ,
                now - t0, g_sys.hull, B.escort_least, B.escort_start);
    }
    esc = escort_slot();
    if (esc > 0) {
        int k, nearest = -1;
        double nd3 = 1e18, ne3 = 1e18;
        for (k = 1; k <= RV_MAX_SHIP_SLOT; k++)
            if (enemy(k) && fighter(k)) {
                if (km(0, k) < nd3)
                    nd3 = km(0, k), nearest = k;
                if (km(esc, k) < ne3)
                    ne3 = km(esc, k);
            }
        if (!B.arrived && nearest > 0) {
            B.arrived = B.flight_frames;
            B.arr_esc = km(0, esc);
            B.arr_near = nd3;
            B.arr_near_esc = ne3;
        }
        if (B.arrived && B.flight_frames - B.arrived < 60 * LOGIC_HZ)
            for (k = 1; k <= RV_MAX_SHIP_SLOT; k++)
                if (enemy(k) && fighter(k)) {
                    if (rvTarget[k] == esc) B.tgt_esc++;
                    else if (rvTarget[k] == 0) B.tgt_player++;
                    else if (rvTarget[k] == 0xFF) B.tgt_none++;
                    else B.tgt_other++;
                }
        if (B.arrived && !B.arr_first_at_esc && ne3 < 1500)
            B.arr_first_at_esc = (B.flight_frames - B.arrived) / LOGIC_HZ + 1;
    }
    if (esc > 0) {
        int h = escort_health(esc);
        if (B.escort_slot != esc) {
            B.escort_slot = esc;
            B.escort_start = B.escort_least = h;
        }
        if (h < B.escort_least)
            B.escort_least = h;
    } else if (B.escort_slot > 0 && logic_escort_lost()) {
        B.escort_least = 0;
        if (!B.esc_lost_frame)
            B.esc_lost_frame = B.flight_frames;
        if (B.esc_end_s && B.flight_frames - B.esc_lost_frame > B.esc_end_s * LOGIC_HZ) {
            say_result();
            fprintf(stderr, "bot: the escort lost %d s ago: done\n", B.esc_end_s);
            exit(1);
        }
        if (!B.esc_dead_at && B.arrived)
            B.esc_dead_at = (B.flight_frames - B.arrived) / LOGIC_HZ + 1;
    }
    /* the aim's wander: a slow drift within RV_BOT_AIM tenths */
    if (B.aim_tenths) {
        double a = B.aim_tenths / 10.0;
        B.aim_dx = fmax(-a, fmin(a, B.aim_dx + ((int)(rnd() % 201) - 100) / 100.0 * a * 0.3));
        B.aim_dy = fmax(-a, fmin(a, B.aim_dy + ((int)(rnd() % 201) - 100) / 100.0 * a * 0.3));
    }
    /* a key tapped last frame comes up; the next of the queue goes down */
    if (B.tap_up) {
        hle_key(B.tap_up, 0);
        B.tap_up = 0;
    } else if (B.nq) {
        int k = B.queue[0];
        memmove(B.queue, B.queue + 1, (size_t)(B.nq - 1) * sizeof B.queue[0]);
        B.nq--;
        if (k <= -100) {
            g_sys.target = (int16_t)(-100 - k);
            B.hold_target = 8;          /* (the comm menu's keys to come talk to it) */
        } else {
            hle_key(k, 1);
            B.tap_up = k;
        }
    }
    if (B.hold_target)
        B.hold_target--;
    if (B.flight_frames < 2 * LOGIC_HZ) {   /* (out of the tube) */
        steer_to(0, 0);
        return;
    }
    if (B.debug && B.flight_frames % LOGIC_HZ == 0) {
        V c, p = pos(0);
        double r, fd = field_near(p, 1e9, &c, &r) ? vlen(vsub(p, c)) - r : -1;
        double nd2;
        int n2 = nearest_enemy_km(&nd2);
        fprintf(stderr, "bot: t %d %s at %.0f %.0f %.0f field %+.0f enemy %d %.0f km target %d hull %d sh %d/%d "
                "speed %lu set %lu fuel %d rocks %d\n", B.flight_frames / LOGIC_HZ, B.mode ? B.mode : "-", p.x, p.y, p.z,
                fd, n2, nd2, B.target, g_sys.hull, shield_pct(0, 0), shield_pct(0, 1), (unsigned long)speedLo[0],
                (unsigned long)setSpeed[0], g_sys.fuel, rocks_bot_count());
    }
    if (B.ap_wait) {                    /* the autopilot's flyby */
        B.mode = "flyby";
        B.ap_wait--;
        key_hold(K_SPACE, &B.held_space, 0);
        key_hold(K_TAB, &B.held_tab, 0);
        steer_to(0, 0);
        return;
    }
    ne = nearest_enemy_km(&nd);
    if (B.watch && esc > 0 && B.escort_start && escort_health(esc) * 100 < B.escort_start * B.watch) {
        V d = vsub(pos(esc), pos(0));   /* (the escort's last moments, in view: back off to see it whole) */
        B.mode = "watch";
        key_hold(K_SPACE, &B.held_space, 0);
        key_hold(K_TAB, &B.held_tab, 0);
        steer_at(d, 0);
        setSpeed[0] = vlen(d) < 6000 ? 0 : slot_type(0)->max_kps;
        return;
    }
    if (B.passive && (esc > 0 || B.escort_slot > 0)) {   /* (a look at the escort's own chances) */
        int16_t w[1];
        B.mode = "passive";
        playerCanBeDamaged = 0;         /* (as the old game's CHEAT rig: the escort's fight alone) */
        steer_to(0, 0);
        setSpeed[0] = 0;
        key_hold(K_SPACE, &B.held_space, 0);
        key_hold(K_TAB, &B.held_tab, 0);
        if (!B.passive_told && logic_wingmen(w, 1) && B.nq == 0) {
            B.passive_told = 1;
            queue(K_C), queue(K_1), queue(K_1);
        }
        if (B.esc_dead_at || B.flight_frames - B.arrived > 240 * LOGIC_HZ) {
            say_result();
            exit(1);
        }
        return;
    }
    if (ne > 0 && nd < 20000) {
        fight_step();
        return;
    }
    B.fight = 0;
    B.mode = "cruise";
    key_hold(K_SPACE, &B.held_space, 0);
    key_hold(K_TAB, &B.held_tab, 0);
    /* every point flown and an enemy still out there: after it (the mission wants every one) */
    if (!navs_left() && ne > 0) {
        V p = vsub(pos(ne), pos(0)), away;
        B.mode = "hunt";
        if (rock_threat(&away))
            steer_at(away, 0);
        else
            steer_at(p, 0);
        setSpeed[0] = slot_type(0)->max_kps;
        key_hold(K_TAB, &B.held_tab, g_sys.fuel > 300);
        return;
    }
    if (navs_left() || home_slot() < 0 || km(0, home_slot()) > 22000) {
        /* on to the next point (or home): the autopilot, else by hand round any field on the way */
        steer_to(0, 0);
        if (B.nq == 0 && !B.tap_up && logic_autopilot_ready() >= 0 && B.flight_frames - B.ap_last > 3 * LOGIC_HZ) {
            queue(K_A);
            B.ap_wait = 120;
            B.ap_last = B.flight_frames + 120;
            B.ap_tries++;
        } else if (B.nq == 0) {
            int n, bn = -1;
            int32_t k, bk = 0x7FFFFFFF;
            V at, c, go;
            double r;
            for (n = 1; n < 16; n++)
                if (logic_nav_km((int16_t)n, &k) && !logic_nav_seen((int16_t)n) && k < bk) {
                    bk = k;
                    bn = n;
                }
            if (bn > 0) {
                const uint8_t *rec = (const uint8_t *)navPointTable + navPointTable_SIZE * bn;
                at = v3(NAV_REC_KM(rec, 0), NAV_REC_KM(rec, 1), NAV_REC_KM(rec, 2));
            } else if (home_slot() > 0)
                at = pos(home_slot());
            else
                at = vadd(pos(0), vmul(fwd(0), 10000));
            go = at;
            if (field_on_way(at, 1500, &c, &r)) {   /* (round the field, not through it) */
                V u = vsub(at, pos(0)), off = vsub(pos(0), c), perp;
                double ul = vlen(u);
                u = vmul(u, 1 / fmax(1, ul));
                perp = vsub(off, vmul(u, vdot(off, u)));
                if (vlen(perp) < 100)
                    perp = upv(0);
                go = vadd(c, vmul(perp, (r + 3500) / vlen(perp)));
            }
            {
                V away;
                if (rock_threat(&away))
                    steer_at(away, 0);
                else
                    steer_at(vsub(go, pos(0)), 0);
            }
            setSpeed[0] = slot_type(0)->max_kps;
            key_hold(K_TAB, &B.held_tab, g_sys.fuel > 400 && !field_near(pos(0), 500, &c, &r));
            B.mode = "by-hand";
        }
        return;
    }
    /* home: cleared to land, then into the carrier (or the trigger when it may land) */
    {
        int h = home_slot();
        V d = vsub(pos(h), pos(0));
        B.mode = "land";
        if (!B.landing_asked && B.nq == 0) {
            B.landing_asked = 1;
            queue(-100 - h);
            queue(K_C);
            queue(K_2);
            queue(K_1);
        }
        steer_at(d, 0);
        setSpeed[0] = slot_type(0)->max_kps;
        key_hold(K_SPACE, &B.held_space, logic_autoland_ready() >= 0 && (B.flight_frames & 8));
    }
}

void bot_tick(void)
{
    const short *v = (const short *)viewRect;
    if (!B.inited) {
        const char *e = getenv("RV_BOT");
        B.inited = 1;
        B.on = e && *e && *e != '0';
        B.seed = getenv("RV_BOT_SEED") ? (unsigned)strtoul(getenv("RV_BOT_SEED"), 0, 0) : 0;
        B.rng = B.seed * 2654435761u + 1;
        B.aim_tenths = getenv("RV_BOT_AIM") ? atoi(getenv("RV_BOT_AIM")) : 0;
        B.taunt = getenv("RV_BOT_TAUNT") ? atoi(getenv("RV_BOT_TAUNT")) : 1;
        B.missiles = getenv("RV_BOT_MISSILES") ? atoi(getenv("RV_BOT_MISSILES")) : 1;
        B.after_s = getenv("RV_BOT_AFTER") ? atoi(getenv("RV_BOT_AFTER")) : 4;
        B.debug = getenv("RV_BOT_DEBUG") != 0;
        B.passive = getenv("RV_BOT_PASSIVE") ? atoi(getenv("RV_BOT_PASSIVE")) : 0;
        B.pause_s = getenv("RV_BOT_PAUSE") ? atoi(getenv("RV_BOT_PAUSE")) : 0;
        B.esc_end_s = getenv("RV_BOT_ESC_END") ? atoi(getenv("RV_BOT_ESC_END")) : 0;
        B.watch = getenv("RV_BOT_WATCH") ? atoi(getenv("RV_BOT_WATCH")) : 0;
    }
    if (!B.on)
        return;
    if (B.done) {                       /* (the game's own time: the flight's frames have stopped) */
        if (plat_now_us() - B.done_us > (uint64_t)B.after_s * 1000000u) {
            say_result();
            fprintf(stderr, "bot: done\n");
            exit(victoryPoints ? 0 : 1);
        }
        return;
    }
    if (B.frame == frameCounter)
        return;
    B.frame = frameCounter;
    if (v[2] > v[0] && slot_is_ship(0) && !missionStatus) {
        B.in_flight = 1;
        flight_tick();
    } else if (B.in_flight) {           /* the flight is over: let go, and the result once the game has it */
        key_hold(K_SPACE, &B.held_space, 0);
        key_hold(K_TAB, &B.held_tab, 0);
        B.done = 1;
        B.done_us = plat_now_us();
        fprintf(stderr, "bot: flight over, status %u\n", missionStatus);
    }
}

/* the thunks as they are called (play.c's host_thunk_enter): the player's hits */
static uint16_t g_added_before;

void bot_thunk_leave(uint16_t fn, const uint16_t *args)
{
    if (B.on && fn == RV_FN_maybe_fire_all_guns && args[0] == 0)
        B.shots += (uint16_t)(shots_added() - g_added_before);
    if (B.on && fn == RV_FN_maybe_fire_all_guns && args[0] != 0 && B.escort_slot > 0 && args[0] <= RV_MAX_SHIP_SLOT
        && rvTarget[args[0]] == B.escort_slot && B.arrived && B.flight_frames - B.arrived < 60 * LOGIC_HZ)
        B.esc_shots_60 += (uint16_t)(shots_added() - g_added_before);
}

void bot_thunk(uint16_t fn, const uint16_t *args)
{
    if (B.on && fn == RV_FN_maybe_fire_all_guns)
        g_added_before = shots_added();
    if (!B.on || fn != RV_FN_do_damage)
        return;
    {
        uint16_t src = args[0], dst = args[1];
        int shooter = -1, missile = 0;
        if (src <= RV_MAX_SHIP_SLOT && maybeEntityType[src] != ET_MISSILE)
            shooter = slot_is_ship((int16_t)src) ? src : -1;
        else if (src < 128 && parentShipForShip[src] != 0xFF) {
            shooter = parentShipForShip[src];
            missile = src <= RV_MAX_SHIP_SLOT && maybeEntityType[src] == ET_MISSILE;
        }
        if (dst == 0 && shooter != 0)
            B.hits_taken++;
        if (B.escort_slot > 0 && dst == B.escort_slot && shooter >= 0 && commAnimInfo[shooter] != commAnimInfo[0]) {
            B.esc_hits++;
            if (B.arrived && B.flight_frames - B.arrived < 60 * LOGIC_HZ)
                B.esc_hits_60++;
        }
        if (shooter == 0 && dst != 0 && dst <= RV_MAX_SHIP_SLOT && commAnimInfo[dst] != commAnimInfo[0]) {
            if (missile) {
                if (args[2] >= 60)      /* (the ship struck: the rest is its blast's) */
                    B.missile_hits++;
            } else
                B.hits++;
        }
    }
}
