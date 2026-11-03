/* fx.c: the flight's effects from what happens in the world (fx.h).  All its data is far (DGROUP is nearly full). */
#include <string.h>
#include <stdlib.h>
#include "rv.h"
#include "rvhooks.h"
#include "rvdata.h"
#include "campaign.h"
#include "collide.h"
#include "world.h"
#include "explode.h"
#include "sfx.h"
#include "cockpit.h"
#include "damage.h"
#include "logic.h"
#include "fx.h"
#include "hud.h"

#define N (RV_MAX_SHIP_SLOT + 1)
#define HIDE_AFTER 2                    /* steps after its death a ship's sprite goes: the flash covers it by then */
#define NEAR_KM 700                     /* this near, a sound at its full volume */
#define GUNS_KM 9000                    /* gunfire heard out to this far */
#define BOOMS_KM 16000                  /* explosions */
#define SHIELD_KM_MIN 30                /* a shield's haze, radius in km: what a nearly spent shield shows */
#define SHIELD_KM_MAX 75                /* and a whole one */

static uint8_t __far g_type[N];         /* each slot's EntityType last step */
static uint8_t __far g_dying[N];        /* its explosion started */
static uint16_t __far g_died[N];        /* the step it began */
static RvVec3 __far g_pos[N], __far g_vel[N];   /* a missile's last place and velocity: its burst's */
static uint16_t __far g_last_frame[RV_SFX_COUNT];   /* a volley: one sound of a weapon a step and ship */
static uint8_t __far g_last_owner[RV_SFX_COUNT];
static uint32_t __far g_vis = 0x9E3779B9UL;   /* the effects' own random stream (never the play's) */
int16_t g_view_outside;                 /* the camera is outside the ship (flight.c): the player's own hull shows too */

static uint16_t vis_rand(void)
{
    g_vis = g_vis * UINT32_C(1664525) + UINT32_C(1013904223);
    return (uint16_t)(g_vis >> 16);
}

/* km from the player to a place (positions), roughly: the largest axis and half the rest */
static int32_t km_to(const RvVec3 *p)
{
    int32_t dx = labs(p->x - gPositionVector[0].x) / 256, dy = labs(p->y - gPositionVector[0].y) / 256,
            dz = labs(p->z - gPositionVector[0].z) / 256, m = dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
    return m + (dx + dy + dz - m) / 2;
}

/* a sound's volume at km: full near, falling with the square to nothing at far */
static int16_t vol_at(int32_t km, int32_t far_km)
{
    int32_t v;
    if (km <= NEAR_KM)
        return 256;
    if (km >= far_km)
        return 0;
    v = (far_km - km) * 256 / (far_km - NEAR_KM);
    return (int16_t)(v * v / 256);
}

/* a sound a little off its own pitch (a shot's variety): 248 .. 264 */
static int16_t pitch_jitter(void)
{
    return (int16_t)(248 + vis_rand() % 17);
}

#define JUMP_FLASH 60                   /* the screen's wash at a friend's jump, of 64; gone in 8 frames */
#define JUMP_FLASH_STEP 8
static uint8_t __far g_flash, __far g_flash_red;

/* Synthetic flow beside the gunsight, and a short pitch response at the start of acceleration or braking.
 * Q8 pixels; only the picture changes, never the ship's motion or the play's random stream. */
static uint16_t __far g_flow_phase, __far g_motion_speed;
static int16_t __far g_motion_bob;
static int8_t __far g_thrust_dir;
static uint8_t __far g_motion_valid, __far g_motion_cool, __far g_motion_burning, __far g_motion_idle;

static void motion_frame(uint16_t speed, uint16_t top, int16_t burning)
{
    int32_t dv = (int32_t)speed - g_motion_speed;
    int16_t dir = dv > 1 ? 1 : dv < -1 ? -1 : 0;
    uint32_t step = top ? (uint32_t)speed * 384 / top : 0;
    if (step > 1536) step = 1536;
    g_flow_phase = (uint16_t)((g_flow_phase + step) % (48U * 256U));
    g_motion_bob = (int16_t)(g_motion_bob * 7 / 8);
    if (g_motion_cool) g_motion_cool--;
    if (g_motion_valid && dir && ((dir != g_thrust_dir && (!g_motion_cool || dir * g_motion_bob < 0))
        || (burning && !g_motion_burning))) {
        g_motion_bob = (int16_t)(dir * (burning ? 3 : 2) * 256);
        g_motion_cool = 12;
    }
    g_motion_speed = speed;
    if (dir) {
        g_thrust_dir = (int8_t)dir;
        g_motion_idle = 0;
    } else if (g_motion_idle < 3 && ++g_motion_idle == 3) {
        g_thrust_dir = 0;               /* tolerate repeated speed samples between changes; one kick per acceleration */
    }
    g_motion_burning = (uint8_t)burning;
    g_motion_valid = 1;
}

void fx_hud(int16_t cx, int16_t cy)
{
    int16_t side, i;
    if (!g_motion_valid || shipAiState[0] == 9)
        return;
    for (side = -1; side <= 1; side += 2) {
        hud_line(cx + side * 34, cy - 24, cx + side * 50, cy + 24, UI_PANEL_EDGE);
        for (i = 0; i < 3; i++) {
            int16_t p = (int16_t)(((g_flow_phase >> 8) + i * 16) % 48);
            int16_t x = cx + side * (34 + p / 3), y = cy - 24 + p;
            hud_line(x, y, x + side * 4, y + 1, UI_GREY4);
        }
    }
}

/* --- the player's own hurt: the view shaken, sparks and smoke in the cockpit, the screen's wash, the alarms ---
 *
 * All of it read from what has changed (a hit from any source, a system broken): picture and sound only, from the effects'
 * own random stream, so none of it touches the play */
#define SPARKS 32
#define SPARK_FRAC 16                   /* its place and speed: sixteenths of a pixel */
typedef struct { int16_t x, y, vx, vy; uint8_t life, smoke; } Spark;
static Spark __far g_spark[SPARKS];
static uint16_t __far g_seen_shield, __far g_seen_armour, __far g_seen_hull;
static uint8_t __far g_seen_valid;      /* (a flight's first frame: only what is there, no hit) */
static int16_t __far g_seen_reserve;
static uint8_t __far g_shake;           /* the view's shake: pixels each way */
static uint8_t __far g_shake_left;      /* frames until it eases a pixel */
static uint16_t __far g_klaxon_at;      /* the frame the klaxon next sounds */
#define KLAXON_STEPS 45

static void spark_add(int16_t x, int16_t y, int16_t smoke)
{
    int16_t i;
    for (i = 0; i < SPARKS; i++)
        if (!g_spark[i].life) {
            Spark __far *p = &g_spark[i];
            p->x = (int16_t)(x * SPARK_FRAC);
            p->y = (int16_t)(y * SPARK_FRAC);
            p->smoke = (uint8_t)smoke;
            if (smoke) {
                p->vx = (int16_t)((int16_t)(vis_rand() % 9) - 4);
                p->vy = (int16_t)(-6 - (int16_t)(vis_rand() % 8));
                p->life = (uint8_t)(30 + vis_rand() % 30);
            } else {
                p->vx = (int16_t)((int16_t)(vis_rand() % 97) - 48);
                p->vy = (int16_t)(-(int16_t)(vis_rand() % 56));
                p->life = (uint8_t)(10 + vis_rand() % 16);
            }
            return;
        }
}

/* a burst from what broke (where its overlay is; a kit that paints none of it, somewhere along the panel) */
static void sparks_from(int16_t bit, int16_t n, int16_t smoke)
{
    int x = 60 + (int)(vis_rand() % 520), y = 300, w = 0, h = 0;
    if (bit >= 0 && cockpit_layer_box(bit, &x, &y, &w, &h)) {
        x += (int)(vis_rand() % (uint16_t)(w ? w : 1));
        y += (int)(vis_rand() % (uint16_t)(h ? h : 1));
    }
    while (n-- > 0)
        spark_add((int16_t)x, (int16_t)y, smoke);
}

static void shake_up(int16_t px)
{
    if (px > g_shake)
        g_shake = (uint8_t)px;
    g_shake_left = 3;
}

static void hurt_frame(void)
{
    const RvShipType __far *t = slot_type(0);
    uint16_t shield, armour, hull;
    int16_t i, k, lv, reserve;
    if (!t || !slot_is_ship(0) || rvController[0] != CTL_PLAYER)
        return;
    shield = (uint16_t)(SHIELD(0, 0) + SHIELD(0, 1));
    armour = (uint16_t)(ARMOR(0, 0) + ARMOR(0, 1) + ARMOR(0, 2) + ARMOR(0, 3));
    hull = damagePoints[0];
    reserve = damage_in_reserve() ? damage_reserve_pct(t->hull) : 100;
    /* a hit: the harder it got through the harder the jolt */
    if (!g_seen_valid) {
        g_seen_valid = 1;
    } else if (hull > g_seen_hull || reserve < g_seen_reserve) {
        shake_up(reserve < 100 ? 7 : 5);
        g_flash_red = 26;
        sparks_from(-1, 6, 0);
    } else if (armour < g_seen_armour) {
        shake_up(3);
        g_flash = g_flash > 12 ? g_flash : 12;
    } else if (shield < g_seen_shield) {
        shake_up(1);
    }
    g_seen_shield = shield;
    g_seen_armour = armour;
    g_seen_hull = hull;
    g_seen_reserve = reserve;
    /* what broke: sparks from its panel, a spit of sound; a wrecked one a clunk and smoke */
    while (damage_event(&k, &lv)) {
        sparks_from(damage_overlay(k), lv >= SYS_WRECKED ? 14 : 8, 0);
        if (lv >= SYS_WRECKED)
            sparks_from(damage_overlay(k), 3, 1);
        sfx_play(lv >= SYS_WRECKED ? RV_SFX_WRECKED : RV_SFX_SPARK, 256, pitch_jitter());
    }
    /* on the reserve: the klaxon, and the cockpit smoking */
    if (damage_in_reserve() && shipAiState[0] != 9) {
        if ((int16_t)(frameCounter - g_klaxon_at) >= 0) {
            sfx_play(RV_SFX_KLAXON, 180, 256);
            g_klaxon_at = (uint16_t)(frameCounter + KLAXON_STEPS);
        }
        if ((frameCounter & 7) == 0)
            sparks_from(-1, 1, 1);
    }
    /* the jolt eases */
    if (g_shake && g_shake_left && --g_shake_left == 0) {
        g_shake--;
        g_shake_left = 3;
    }
    /* the sparks fall and the smoke rises: their rows are the panel's to draw again, before and after */
    for (i = 0; i < SPARKS; i++) {
        Spark __far *p = &g_spark[i];
        int16_t row;
        if (!p->life)
            continue;
        row = (int16_t)(p->y / SPARK_FRAC);
        vid_dirty(row - 1, row + 4);
        p->x = (int16_t)(p->x + p->vx);
        p->y = (int16_t)(p->y + p->vy);
        if (!p->smoke)
            p->vy = (int16_t)(p->vy + 4);
        p->life--;
        row = (int16_t)(p->y / SPARK_FRAC);
        vid_dirty(row - 1, row + 4);
    }
}

void fx_shake(int16_t *dx, int16_t *dy)
{
    int16_t px = g_shake;
    *dx = *dy = 0;
    if (slot_is_ship(0) && rvController[0] == CTL_PLAYER && shipAiState[0] != 9) {
        /* a rumble with nothing hitting: the engine badly hurt, every other frame; the absorbers hurt, in a hard turn; on the
         * reserve, the whole ship */
        if (px < 1 && ((sys_level(SYS_ENGINE) >= 3 && speedLo[0] && (frameCounter & 1)) || damage_in_reserve()))
            px = 1;
        if (px < 2 && sys_level(SYS_ABSORBERS) >= 2 && (g_pilot.steer_x >= 7 || g_pilot.steer_x <= -7 || g_pilot.steer_y >= 7 || g_pilot.steer_y <= -7))
            px = (int16_t)(sys_level(SYS_ABSORBERS) >= 4 ? 2 : 1);
    }
    if (px) {
        *dx = (int16_t)((int16_t)(vis_rand() % (uint16_t)(2 * px + 1)) - px);
        *dy = (int16_t)((int16_t)(vis_rand() % (uint16_t)(2 * px + 1)) - px);
    }
    if (!g_view_outside)
        *dy += g_motion_bob / 256;
}

void fx_cockpit_draw(int y0, int y1)
{
    static const unsigned char __far hot[4] = { UI_RED, UI_AMBER2, UI_AMBER3, UI_WHITE };
    int16_t i;
    for (i = 0; i < SPARKS; i++) {
        const Spark __far *p = &g_spark[i];
        int x, y;
        if (!p->life)
            continue;
        x = p->x / SPARK_FRAC;
        y = p->y / SPARK_FRAC;
        if (x < 0 || x >= SCR_W - 4 || y < 0 || y >= SCR_H - 4)
            continue;
        if (p->smoke)                   /* (a dark wisp, fading out to specks as it goes) */
            gfx_fill(x, y, p->life > 12 ? 4 : 2, p->life > 12 ? 3 : 2, p->life > 24 ? UI_GREY1 : UI_GREY2, y0, y1);
        else
            gfx_line(x, y, x - p->vx / (SPARK_FRAC / 2), y - p->vy / (SPARK_FRAC / 2),   /* (a streak: where it was two frames ago) */
                     hot[p->life >> 2 > 3 ? 3 : p->life >> 2], 0, y0, y1);
    }
}

/* now and then a ship that has been hurt badly shows it: a spit of sparks off its hull */
static void ships_smoke(void)
{
    int16_t s;
    for (s = (g_view_outside ? 0 : 1); s < N; s++) {
        const RvShipType __far *t;
        RvVec3 at;
        int32_t half;
        if (!slot_is_ship(s) || shipAiState[s] == 9 || ((frameCounter + s * 7) & 15) || maybeEntityType[s] != ET_SHIP)
            continue;
        t = slot_type(s);
        if (!t || !t->hull || (uint32_t)damagePoints[s] * 2 < (uint32_t)t->hull || km_to(&gPositionVector[s]) > GUNS_KM)
            continue;
        half = collide_half(s) / 256;
        at.x = gPositionVector[s].x + ((int32_t)(vis_rand() % 256) - 128) * (half / 2);
        at.y = gPositionVector[s].y + ((int32_t)(vis_rand() % 256) - 128) * (half / 4);
        at.z = gPositionVector[s].z + ((int32_t)(vis_rand() % 256) - 128) * (half / 2);
        explode_add(&at, &gVelocityVector[s], 50, BOOM_HIT, 0, (uint16_t)(frameCounter * 7 + s));
    }
}

void fx_flight_start(void)
{
    int16_t s;
    g_flash = 0;
    g_flash_red = 0;
    g_shake = g_shake_left = 0;
    g_klaxon_at = 0;
    _fmemset(g_spark, 0, sizeof g_spark);
    g_seen_valid = 0;
    g_flow_phase = g_motion_speed = 0;
    g_motion_bob = g_thrust_dir = 0;
    g_motion_valid = g_motion_cool = g_motion_burning = g_motion_idle = 0;
    for (s = 0; s < N; s++) {
        g_type[s] = (uint8_t)maybeEntityType[s];
        g_dying[s] = (uint8_t)(slot_is_ship(s) && shipAiState[s] == 9);
        g_died[s] = frameCounter;
    }
    _fmemset(g_last_frame, 0xFF, sizeof g_last_frame);
}

static void death(int16_t s)
{
    int32_t half_km = collide_half(s) / 256;
    int32_t boom_km = half_km * 7 / 5;   /* 40% larger blasts; burst positions still follow the hull */
    RvVec3 at = gPositionVector[s];
    uint16_t seed = (uint16_t)(frameCounter * 31 + s * 977);
    g_dying[s] = 1;
    g_died[s] = frameCounter;
    if (s == 0) {                       /* the player's own: the camera is in it; only the sound */
        sfx_play(RV_SFX_BOOM_SHIP, 256, 256);
        return;
    }
    explode_add(&at, &gVelocityVector[s], half_km, BOOM_WRECK, 0, seed);
    if (maybeEntityType[s] == ET_CAPSHIP) {
        /* a capital ship: bursts over its hull, then the last and biggest */
        int16_t k;
        for (k = 0; k < 6; k++) {
            RvVec3 p;
            p.x = at.x + ((int32_t)(vis_rand() % 512) - 256) * (half_km / 2);
            p.y = at.y + ((int32_t)(vis_rand() % 512) - 256) * (half_km / 6);
            p.z = at.z + ((int32_t)(vis_rand() % 512) - 256) * (half_km / 2);
            explode_add(&p, &gVelocityVector[s], boom_km / 3, BOOM_SHIP, (uint16_t)(k * 9), (uint16_t)(seed + k));
        }
        explode_add(&at, &gVelocityVector[s], boom_km * 5 / 4, BOOM_CAPITAL, 50, seed);
        sfx_play(RV_SFX_BOOM_CAPITAL, vol_at(km_to(&at), BOOMS_KM * 2), 256);
        return;
    }
    explode_add(&at, &gVelocityVector[s], boom_km * 13 / 10, BOOM_SHIP, 0, seed);
    sfx_play(RV_SFX_BOOM_SHIP, vol_at(km_to(&at), BOOMS_KM), pitch_jitter());
}

/* a ship jumps out (the user: a star, camera aligned; "may flash the whole screen white to indicate success jump out") */
void fx_jump(int16_t slot, int16_t friendly)
{
    static const RvVec3 still = { 0, 0, 0 };
    RvVec3 at = gPositionVector[slot], vel = still;
    explode_add(&at, &vel, collide_half(slot) / 256, BOOM_JUMP, 0, (uint16_t)(frameCounter * 31 + slot * 977));
    sfx_play(RV_SFX_LAUNCH, friendly ? 256 : vol_at(km_to(&at), BOOMS_KM), 180);
    if (friendly)
        g_flash = JUMP_FLASH;
}

void fx_frame(void)
{
    int16_t s;
    const RvShipType __far *t = slot_type(0);
    if (t && slot_is_ship(0) && shipAiState[0] != 9)
        motion_frame((uint16_t)speedLo[0], t->max_kps, rvAfterburner[0]);
    else
        g_motion_bob = 0;
    hurt_frame();
    ships_smoke();
    if (g_flash_red) {
        vid_flash_red(g_flash_red);
        g_flash_red = (uint8_t)(g_flash_red > JUMP_FLASH_STEP ? g_flash_red - JUMP_FLASH_STEP : 0);
        if (!g_flash_red)
            vid_flash_red(0);
    } else if (g_flash) {
        vid_flash(g_flash);
        g_flash = (uint8_t)(g_flash > JUMP_FLASH_STEP ? g_flash - JUMP_FLASH_STEP : 0);
        if (!g_flash)
            vid_flash(0);
    }
    for (s = 0; s < N; s++) {
        uint8_t et = (uint8_t)maybeEntityType[s];
        if (slot_is_ship(s)) {
            if (shipAiState[s] == 9 && !g_dying[s])
                death(s);
            else if (shipAiState[s] != 9)
                g_dying[s] = 0;
        } else {
            g_dying[s] = 0;
        }
        if (et == ET_MISSILE) {
            if (g_type[s] != ET_MISSILE) {   /* away */
                const RvWeapon __far *w = weapon(gInstanceShipTypes[s]);
                int16_t id = w && w->sfx != RV_SFX_NONE ? w->sfx : RV_SFX_LAUNCH;
                sfx_play(id, parentShipForShip[s] == 0 ? 256 : vol_at(km_to(&gPositionVector[s]), GUNS_KM),
                         pitch_jitter());
            }
            g_pos[s] = gPositionVector[s];
            g_vel[s] = gVelocityVector[s];
        } else if (g_type[s] == ET_MISSILE) {   /* its end: a burst where it was */
            RvVec3 at = g_pos[s], vel = g_vel[s];   /* (near copies: the calls take near vectors) */
            explode_add(&at, &vel, 260, BOOM_MISSILE, 0, (uint16_t)(frameCounter * 13 + s));
            sfx_play(RV_SFX_BOOM_SMALL, vol_at(km_to(&at), BOOMS_KM), pitch_jitter());
        }
        g_type[s] = et;
    }
}

int16_t fx_hidden(int16_t s)
{
    return s >= 0 && s < N && g_dying[s] && shipAiState[s] == 9 && (uint16_t)(frameCounter - g_died[s]) >= HIDE_AFTER;
}

void fx_shot(int16_t owner, uint16_t weapon_type)
{
    const RvWeapon __far *w = weapon(weapon_type);
    int16_t id;
    if (!w || w->sfx == RV_SFX_NONE || owner < 0 || owner >= N)
        return;
    id = w->sfx;
    if (g_last_frame[id] == frameCounter && g_last_owner[id] == owner)
        return;                         /* (a volley: its guns sound as one) */
    g_last_frame[id] = frameCounter;
    g_last_owner[id] = (uint8_t)owner;
    sfx_play(id, owner == 0 ? 220 : vol_at(km_to(&gPositionVector[owner]), GUNS_KM), pitch_jitter());
}

/* a shot met fighter `slot` there, before its damage is done: the shield on that side (front or rear, as do_damage
 * picks it) shows the hit as a haze as strong as the hit leaves it.  Whether it stopped the shot: if not (no shield
 * there, or too little) the sparks tell of the hurt getting through */
static int16_t shield_takes(int16_t slot, const RvVec3 *where, uint16_t damage)
{
    int32_t dx, dy, dz, left;
    int16_t k, strength;
    uint16_t have, most;
    if (maybeEntityType[slot] != ET_SHIP || shipAiState[slot] == 9)
        return 0;
    dx = TO_KM(where->x - gPositionVector[slot].x);
    dy = TO_KM(where->y - gPositionVector[slot].y);
    dz = TO_KM(where->z - gPositionVector[slot].z);
    k = dx * gOrientationFrontVector[slot].x + dy * gOrientationFrontVector[slot].y
        + dz * gOrientationFrontVector[slot].z >= 0 ? 0 : 1;
    have = SHIELD(slot, k);
    most = SHIELD_MAX(slot, k);
    if (!have || !most)
        return 0;
    left = have > damage ? (int32_t)have - damage : 0;
    strength = (int16_t)(left * 255 / most);
    explode_shield(where, &gVelocityVector[slot], SHIELD_KM_MIN + (int32_t)strength * (SHIELD_KM_MAX - SHIELD_KM_MIN) / 255,
                   (uint8_t)strength, slot, (uint16_t)(frameCounter * 7 + slot + where->x));
    return damage <= have;
}

void fx_hit(int16_t slot, const RvVec3 *where, int16_t owner, uint16_t damage)
{
    int16_t v;
    if (slot < 0 || slot >= N)
        return;
    if (slot == 0) {                    /* the player hit: the shield or the hull, by what is left of the shields */
        sfx_play(SHIELD(0, 0) + SHIELD(0, 1) > 0 ? RV_SFX_HIT_SHIELD : RV_SFX_HIT_HULL, 256, pitch_jitter());
        return;
    }
    if (!shield_takes(slot, where, damage))
        explode_add(where, &gVelocityVector[slot], 70, BOOM_HIT, 0, (uint16_t)(frameCounter * 7 + slot));
    v = vol_at(km_to(where), GUNS_KM);
    if (owner == 0 && v < 150)
        v = 150;                        /* (the player's own hits always heard) */
    sfx_play(RV_SFX_HIT_FAR, v, pitch_jitter());
}
