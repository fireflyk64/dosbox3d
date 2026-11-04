/* rocks.c: asteroid fields (rocks.h).
 *
 * A field is an effect, as the old game's are (its overlay 168 read
 * 2026-11-02 and its rocks measured in flight: tests/wc1/README.md, "The
 * asteroid fields"), in its own numbers since 2026-11-03 (the user: the
 * fields felt "really sparse" and "easy", "only require a single juke";
 * "make sure they are just about the same size as WC1 fields and similar
 * difficult"): our ships, our view and our speeds are its own now (radius
 * 100 km, +-45 degrees across, a kps 1.5 km/s), and so are the fields
 * (missions.txt: its radii about its own nav points).  While the point
 * FX_CHECK_KM ahead of the player's nose is inside a field, rocks are made
 * ahead of them, at most FX_CAP at once (the old game's 20, scaled by the
 * field's density): FX_SPAWN_KM ahead, in a cone about the nose (its +-35 x
 * +-20 degrees) turned where the nose is turning, with its chance a frame of
 * its (the faster the player goes, the sooner), and only where that point is
 * in the field.  Most drift across the field (a right angle off the line to
 * its middle, up, down, left or right) at 100 to 160 kps and the player flies
 * into them, a fifth keep still, and some fly at where the player will be
 * after its time (its share and its time by the field's level, its
 * dseg:BFF4: 0 to 3) to meet them there.  A rock past FX_FAR_KM from the
 * player, less with the player's speed down to FX_FAR_MIN_KM (its 4,300 and
 * 3,100), is gone, and another is made ahead.  Mines come at the player.  A
 * shot that goes into a rock is spent and breaks it one time in three (the
 * old game's); the player's ship flying into one breaks it.
 * Once a frame every rock goes through the camera (rv_rocks_place,
 * rocks.asm): the ones on screen out, the ones too far to a list to be
 * removed, the ones within reach to a list tested against the player's hit
 * box.
 *
 * The bar's window (flight.c) keeps a field as a cluster instead
 * (rocks_field_cluster): its rocks stay in the field and drift.
 *
 * Every machine has its own: the rocks are no ship slots and nothing about
 * them is sent, they come from a random stream of this module's own (never
 * randomSeed, the play's shared one), and only the player's own ship hits
 * them.  A hit spends shields and armour through do_damage (the thunk: the
 * layer sees it), the field's slot as its source; the player is put back
 * out of the rock and loses most of their speed.
 *
 * The art (ROCKS.DAT) is read into XMS at its own level (XMS_HAZARD, above
 * the mission's: the archive's assets wanted meanwhile go there too) when
 * the player comes within the bubble of a field, and given back when they
 * are well away; so is the conventional memory (the pool, the lists, the
 * small sprites: about 30 KB at ROCK_MAX).  The module's data and the log's
 * formats are far (DGROUP is nearly full). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <malloc.h>
#include <dos.h>
#include <fcntl.h>
#include "rv.h"
#include "rvhooks.h"
#include "music.h"
#include "campaign.h"
#include "bolts.h"
#include "shots.h"
#include "sfx.h"
#include "rocks.h"
#include "world.h"
#include "flight.h"

#define FILE_HEAD 24
#define KINDS_MAX 4
#define SIZES_MAX 64
#define DMAX 512                        /* the largest diameter drawn: a ladder sprite as 4 x 4 blocks */
#define SUM_SQ(n) ((unsigned long)(n) * ((n) + 1) * (2 * (n) + 1) / 6)
#ifdef RV_HEAP
#define ROCK_MAX 1200
#define ROCK_OUT 128                    /* on screen at most, as the game's (400 for the old bubble's benchmarks, 2026-10-24:
                                           since the field became an effect no flight has that many, and by 2026-11-04
                                           RVT.EXE's heap had no room left for the lists at that size: no field at all) */
#else
#define ROCK_MAX 128                    /* (the bar window's cluster asks 154; an asteroid field 60 at the plain density) */
#define ROCK_OUT 128
#endif
#define ROCK_SMALL_BYTES 6528U          /* rocks.dat's small sprites: 4 kinds x 8 frames x 1 .. 8 px squared */
#define ROCK_REAP 96                    /* removed in a frame at most (the rest wait a frame) */
#define ROCK_HITS 16
#define ROCK_REACH_KM 700L              /* a ship's half box and some: within it and a rock's radius, tested */
#define FIELDS_MAX 7                    /* fieldTable's records */
#define FIELD_SLOT(i) (RV_CAMERA_SLOT - 1 - (i))   /* a field's local-only hazard slot */
#define ZONE_PAD_KM 2000L               /* the art stays this much past where it was read in */
#define SHIFT_KEY 18                    /* depth keys: camera units >> 18 = 64 km a key */
#define ROCK_KM_MIN 80L                 /* a field's rock's radius: about a fighter's, as the old game's (100 km, the
                                           Hornet's); 2026-10-24's quarter to a half of a fighter was of the bigger
                                           ships then, and the bar window's cluster keeps that look */
#define ROCK_KM_SPAN 40L
#define CLUSTER_KM_MIN 110L
#define CLUSTER_KM_SPAN 130L
#define FRAG_MAX 32                     /* pieces of broken rock in flight at once */
#define FRAG_N 7                        /* a rock breaks into so many */

/* The field as an effect: the old game's (ovr168: 20 rocks, made 3,050 km ahead within +-35 x +-20 degrees of the
 * nose, a chance of (speed/10 + 30)/215 a frame, gone past 3,100-4,300 km by the speed; its camera's focal length 160
 * on its 320, ours 320 on our 640; its rocks as big as its fighters, 100 km, as ours) in its own numbers */
#define FX_CAP 20                       /* the old game's rocks at once (its 20 slots): a kind's cap is a multiple of it,
                                           at the plain density (ROCK_DENSITY; a field's density= scales it) */
#define FX_SPAWN_KM 3050L               /* a new rock this far ahead (the old game's 2,940-3,130 measured) */
#define FX_FAR_KM 4300L                 /* a rock farther than this from the player is gone ... */
#define FX_FAR_MIN_KM 3100L             /* ... down to this, 60 km less for each of the old game's km a frame (kps / 10)
                                           of the player's speed (its 0890: 4,300 a rock drawn, 4,300-3,100 one not) */
#define FX_CHECK_KM 3050L               /* rocks come while the point this far ahead of the nose is in a field (each
                                           made only where its own point is, its 06A9) */
#define FX_NEAR_KM 4500L                /* the art read in when a field's edge is this near (rocks come from 3,050 km
                                           before it) */
#define FX_ENTRY 3                      /* rocks made at once as a field begins (the old game's 3) */
#define FX_LEAD_STEPS 30                /* the cone turned to where the nose will point in a second */
#define FX_YAW 35                       /* the cone, in the old game's degrees (+-35 across, +-20 up and down), each */
#define FX_PITCH 20                     /* turned into our view by its tangent x 160 / 400 */
#define BUBBLE_KM 9000L                 /* a cluster's (the window's): rocks kept within it of the camera */
#define BUBBLE_FADE 60                  /* percent of it from which a cluster's rock shrinks */

/* how a field's rocks come, by its kind (asteroids, mines) */
typedef struct {
    unsigned short kps_min, kps_span;   /* a rock's own speed (kps) */
    unsigned short miss_km;             /* (at the player) toward a point off them by up to this, across and up */
    unsigned char still;                /* percent of them that keep still (the old game's fifth) */
    unsigned char aimed;                /* 1: the old game's share flies at where the player will be */
    unsigned char across;               /* 1: the rest drift across the field (the old game's 036A), 0: at the player */
    unsigned char times;                /* its cap and its chance a frame of a new one, the old game's times this */
} FieldKind;
/* (asteroids three times the old game's: the user, 2026-11-04, "the asteroid field seems still sparser than WC1 can we
 * increase density while still remaining at 30 fps with 4-5 fighters inside?": its 20 kept some 12 rocks, 4 of them on
 * screen, flying through at 200 kps; three times it 38 and 11, and the frame with five fighters about 17.6 ms at the
 * page's 50,000 cycles where it was 17.5.  The mines as many as the old game's: they hunt now, seek_mines) */
static const FieldKind __far k_kind[2] = {
    { 100, 60, 0, 20, 1, 1, 3 },        /* asteroids: across at 10 to 16 of its km a frame */
    { 30, 20, 600, 20, 0, 0, 1 },       /* mines: they drift at the player (the old game's: 2 km a frame, at it) */
};
/* the old game's levels (dseg:BFF4, a mission's): a rock flies at where the player will be when it comes up within
 * the cone's pitch LO < e < HI and yaw +-19 (then 60%: its 06F2, dseg:39AA), after T + random(15) of its frames (its
 * 041B, dseg:39A6) */
static const signed char __far k_aim_lo[4] = { -10, -8, -12, -8 }, __far k_aim_hi[4] = { 4, 8, 8, 8 };
static const unsigned char __far k_aim_t[4] = { 56, 52, 75, 73 };

int g_rock_xms = 1;
static char s_file[] = "ROCKS.DAT";     /* (near: DOS wants it so) */

/* ROCKS.DAT, in XMS while near a field */
static XmsBlock __far g_blk;
static unsigned __far g_handle;
static unsigned long __far g_base;      /* the block's offset in the handle's memory */
static int __far g_kinds, __far g_frames, __far g_sframes, __far g_small, __far g_nbig, __far g_bigmax;
static unsigned char __far g_bigd[SIZES_MAX];
static unsigned long __far g_index_at;  /* the big sprites' index: u32 offsets in the file (= in the block) */
static unsigned char __far g_size_of[DMAX + 1], __far g_k_of[DMAX + 1];   /* a diameter's ladder size and blocks */
#define HDR_MAX (4 + 9 * 128 + 2)        /* a record's head and run table at most: 128 rows of 4 runs */
static unsigned char __far g_hdr[HDR_MAX];   /* a record's head and run table, fetched */
static unsigned long __far g_size;      /* ROCKS.DAT's bytes (the block's) */

/* the rocks, round the player while near a field */
static LzRock __far *__far g_r;
static int __far g_n, __far g_cap, __far g_want_n;
static LzRockOut __far *__far g_o;
static int __far g_no;
static LzBallOut __far *__far g_so;    /* the small ones on screen, as balls */
static int __far g_ns;
static short __far *__far g_order;      /* the big ones on screen, farthest first (indices into g_o) */
static int __far g_nb, __far g_next;
static unsigned char __far *__far g_smallspr;   /* every kind's small sprites (g_sframes frames, 1 .. g_small px) */
static unsigned __far g_small_set;      /* a set's bytes */
static short __far g_reap[ROCK_REAP], __far g_hit[ROCK_HITS];
static LzCam __far g_cam;
static LzRockCam __far g_rc;
static unsigned long __far g_seed = 99991UL;
static long __far g_kb[3];              /* the player tossed aside by a rock: positions a frame, dying away */
static LfVec __far g_prev_front;        /* the ship's nose a frame ago (the cone leads the turn) */
static int __far g_cur = -1;            /* the field the rocks come from now (the point ahead is in it), -1 none */
static int __far g_src;                 /* the field the rocks came from last (their hits' source, the mines' colour) */
static LzRock __far g_frag[FRAG_MAX];   /* the pieces of the rocks the player broke */
static unsigned char __far g_frag_life[FRAG_MAX];   /* frames each has left */
static int __far g_nfrag;

/* the fields */
typedef struct {
    long x, y, z, r_km;
    int density;
    unsigned char type, slot, cluster, level;
} Field;
static Field __far g_f[FIELDS_MAX];
static int __far g_nf, __far g_active = -1;   /* the field the art was read in for (-1: none, no art; -2 - i: field
                                           i's would not come in) */
static int __far g_wanted;

/* the tally */
static unsigned long __far g_t_place, __far g_t_small, __far g_t_big, __far g_t_keep, __far g_t_blocks;
static unsigned long __far g_frames_in, __far g_sum_live, __far g_sum_small, __far g_sum_big, __far g_rows;
static unsigned long __far g_respawned, __far g_hits_n, __far g_aimed, __far g_blocks_n, __far g_made, __far g_shot_n,
    __far g_broken, __far g_seeks;
static int __far g_max_live, __far g_max_on;

static int16_t rocks_shot(const Shot __far *s);

static unsigned rnd(void)
{
    g_seed = g_seed * 1103515245UL + 12345UL;
    return (unsigned)(g_seed >> 16) & 0x7FFF;
}

void rocks_want(int on) { g_wanted = on; }

/* The trackers (the user: "share art with the rocks... the diamond is just gray when they are not [mines]... 6 diamonds
 * per mine, one per cube face").  Every rock's art has six diamonds in palette entry 255, which nothing else in a flight
 * uses (tools/rocks.py MARK_INDEX; tools/rvpack.py keeps it out of the cockpits' tables): grey, so an asteroid blends
 * them in, and in a mine field flashing red, about twice a second.  One entry changed costs nothing on the rocks */
#define TRACKER_INDEX 255
static signed char __far g_tracker_on = -1;

static void tracker_colour(void)
{
    static const unsigned char __far s_grey[3] = { 29, 29, 28 };   /* (6-bit VGA) */
    static const unsigned char __far s_red[3] = { 62, 8, 6 };
    int on = g_active >= 0 && g_f[g_src].type == RV_FIELD_MINE && !((frameCounter >> 3) & 1);
    if (on != g_tracker_on) {
        g_tracker_on = (signed char)on;
        vid_palette(on ? s_red : s_grey, TRACKER_INDEX, 1);
    }
}

const char *rocks_file(void) { return g_wanted ? s_file : 0; }

unsigned long rocks_room_kb(void)
{
    int fd;
    unsigned long size;
    if (!g_wanted || _dos_open(s_file, O_RDONLY, &fd) != 0)
        return 0;
    size = lseek(fd, 0, SEEK_END);
    _dos_close(fd);
    return size / 1024 + 64;
}

/* --- the art --- */

static int art_in(void)
{
    static const char __far s_magic[] = "RVROCKS", __far s_none[] = "rocks: no ROCKS.DAT", __far s_other[] = "rocks: ROCKS.DAT is not this build's",
        __far s_in[] = "rocks: %d kinds, %d frames, 1 .. %d px small, %d sizes to %d: %lu KB in XMS in %lu ms",
        __far s_mem[] = "rocks: no XMS for ROCKS.DAT (%lu KB)";
    unsigned char *b = rv_filebuf;
    unsigned long size, at, t0 = clock240();
    unsigned got;
    int fd, i, d;
    if (_dos_open(s_file, O_RDONLY, &fd) != 0) {
        rv_logf(s_none);
        return 0;
    }
    if (_dos_read(fd, b, 160, &got) != 0 || got < FILE_HEAD || _fmemcmp(b, s_magic, 8) != 0
        || (b[8] | b[9] << 8) != 2) {
        rv_logf(s_other);
        _dos_close(fd);
        return 0;
    }
    g_kinds = b[10] | b[11] << 8;
    g_frames = b[12] | b[13] << 8;
    g_sframes = b[14] | b[15] << 8;
    g_small = b[16] | b[17] << 8;
    g_nbig = b[18] | b[19] << 8;
    g_bigmax = b[20] | b[21] << 8;
    if (g_kinds < 1 || g_kinds > KINDS_MAX || g_nbig < 1 || g_nbig > SIZES_MAX || g_bigmax > 128 || g_small != LZ_BALL_SMALL
        || (g_frames & (g_frames - 1)) || g_sframes < 1 || g_frames % g_sframes || FILE_HEAD + g_nbig > 160) {
        rv_logf(s_other);
        _dos_close(fd);
        return 0;
    }
    _fmemcpy(g_bigd, b + FILE_HEAD, g_nbig);
    g_small_set = (unsigned)SUM_SQ(g_small);
    g_index_at = FILE_HEAD + ((g_nbig + 1) & ~1) + (unsigned long)g_kinds * g_sframes * g_small_set;
    size = lseek(fd, 0, SEEK_END);
    g_size = size;
    g_blk = xms_alloc(size + 2, XMS_HAZARD);
    if (!g_blk) {
        rv_logf(s_mem, size / 1024);
        _dos_close(fd);
        return 0;
    }
    lseek(fd, 0, SEEK_SET);
    for (at = 0; at < size; at += 32768U) {
        unsigned part = size - at < 32768U ? (unsigned)(size - at) : 32768U;
        if (_dos_read(fd, gfx_scratch(), part, &got) != 0 || got != part)
            break;
        xms_put(g_blk, at, gfx_scratch(), (part + 1) & ~1U);
    }
    _dos_close(fd);
    {
        unsigned h;
        unsigned long off;
        xms_where(g_blk, &h, &off);
        g_handle = h;
        g_base = off;
    }
    /* a diameter's ladder size (the nearest), and past the ladder its blocks: the fewest that reach it */
    for (d = 1; d <= DMAX; d++) {
        int k = d <= g_bigmax ? 1 : (d + g_bigmax - 1) / g_bigmax, want = (d + k / 2) / k, best = 0;
        for (i = 1; i < g_nbig; i++)
            if (abs(g_bigd[i] - want) < abs(g_bigd[best] - want))
                best = i;
        g_size_of[d] = (unsigned char)best;
        g_k_of[d] = (unsigned char)(k > 4 ? 4 : k);
    }
    rv_notef(s_in, g_kinds, g_frames, g_small, g_nbig, g_bigmax, size / 1024, (clock240() - t0) * 1000 / 240);
    return 1;
}

/* --- the pool --- */

static void pool_free(void)
{
    if (g_r) MEM_GIVE(g_r);
    if (g_o) MEM_GIVE(g_o);
    if (g_so) MEM_GIVE(g_so);
    if (g_order) MEM_GIVE(g_order);
    if (g_smallspr) MEM_GIVE(g_smallspr);
    g_r = 0;
    g_o = 0;
    g_so = 0;
    g_order = 0;
    g_smallspr = 0;
    g_n = g_cap = g_no = g_ns = g_nb = 0;
}

MEM_ARRAY(unsigned char, g_smallspr_arr, ROCK_SMALL_BYTES)
MEM_ARRAY(LzRockOut, g_o_arr, ROCK_OUT)
MEM_ARRAY(LzBallOut, g_so_arr, ROCK_OUT)
MEM_ARRAY(short, g_order_arr, ROCK_OUT)
MEM_ARRAY(LzRock, g_r_arr, ROCK_MAX)

static int pool_alloc(int want)
{
    static const char __far s_room[] = "rocks: room for %d rocks (%d asked), %lu KB of DOS memory left";
    unsigned long sbytes = (unsigned long)g_kinds * g_sframes * g_small_set;
    g_smallspr = MEM_TAKE(unsigned char, g_smallspr_arr, sbytes);
    g_o = MEM_TAKE(LzRockOut, g_o_arr, ROCK_OUT);
    g_so = MEM_TAKE(LzBallOut, g_so_arr, ROCK_OUT);
    g_order = MEM_TAKE(short, g_order_arr, ROCK_OUT);
    if (!g_smallspr || !g_o || !g_so || !g_order) {
        static const char __far s_room[] = "rocks: no room for their lists: no asteroid fields";
        mem_short(s_room);
        return 0;
    }
    xms_get(g_blk, FILE_HEAD + ((g_nbig + 1) & ~1), g_smallspr, sbytes);
    if (want > ROCK_MAX)
        want = ROCK_MAX;
    while (want >= 16 && (g_r = MEM_TAKE(LzRock, g_r_arr, want)) == 0)
        want = want * 3 / 4;
    g_cap = g_r ? want : 0;
    rv_notef(s_room, g_cap, g_want_n, bolts_dos_free() / 1024);
    return g_cap > 0;
}

/* --- the fields --- */

static void field_slot(int i, int on)
{
    int s = FIELD_SLOT(i);
    const Field __far *f = &g_f[i];
    unsigned char *rec = fieldTable + fieldTable_SIZE * i;
    if (!on) {
        maybeEntityType[s] = 0;
        rvFlags[s] = 0;
        memset(rec, 0, fieldTable_SIZE);
        return;
    }
    maybeEntityType[s] = f->type;
    gInstanceShipTypes[s] = f->type == RV_FIELD_ASTEROID ? 0x16 : 0x17;
    parentShipForShip[s] = 0xFF;
    rvController[s] = 4;                /* (frozen: nothing flies it) */
    rvFlags[s] = 2;                     /* a local-only hazard: every machine has its own */
    gPositionVector[s].x = f->x;
    gPositionVector[s].y = f->y;
    gPositionVector[s].z = f->z;
    gMaybeShipRadius[s] = (unsigned short)(f->r_km > 65535L ? 65535U : (unsigned)f->r_km);
    /* the record the layer reads: type, position, radius (km), density */
    *(unsigned short *)(rec + 0) = f->type;
    *(long *)(rec + 2) = f->x;
    *(long *)(rec + 6) = f->y;
    *(long *)(rec + 10) = f->z;
    *(long *)(rec + 14) = f->r_km;
    rec[18] = (unsigned char)f->density;
    rec[19] = f->level;
}

int rocks_field_add(int type, long x, long y, long z, long radius_km, int density, int level)
{
    static const char __far s_add[] = "rocks: field %d (%Fs) at %ld,%ld,%ld km, %ld km across, %d rocks per (10,000 km)^3";
    static const char __far s_ast[] = "asteroids", __far s_mine[] = "mines";
    Field __far *f;
    if (g_nf >= FIELDS_MAX)
        return -1;
    f = &g_f[g_nf];
    f->type = (unsigned char)type;
    f->x = x;
    f->y = y;
    f->z = z;
    f->r_km = radius_km;
    f->density = density > 0 ? density : ROCK_DENSITY;
    f->level = (unsigned char)(level >= 0 && level <= 3 ? level : 0);
    f->slot = (unsigned char)FIELD_SLOT(g_nf);
    f->cluster = 0;
    field_slot(g_nf, 1);
    rv_notef(s_add, g_nf, type == RV_FIELD_ASTEROID ? s_ast : s_mine, x / 256, y / 256, z / 256, 2 * radius_km, f->density);
    fieldCount = (unsigned short)++g_nf;
    return g_nf - 1;
}

int rocks_field_cluster(long x, long y, long z, long radius_km, int density)
{
    int i = rocks_field_add(RV_FIELD_ASTEROID, x, y, z, radius_km, density, 0);
    if (i >= 0)
        g_f[i].cluster = 1;
    return i;
}

/* the player's distance from a field's middle, past its edge: km (negative inside) */
static long beyond(const Field __far *f)
{
    long dx = (gPositionVector[0].x - f->x) / 256 / 16, dy = (gPositionVector[0].y - f->y) / 256 / 16,
        dz = (gPositionVector[0].z - f->z) / 256 / 16, m;
    /* (sixteen km a step: the squares stay in 31 bits to 740,000 km) */
    m = labs(dx) > labs(dy) ? labs(dx) : labs(dy);
    if (labs(dz) > m)
        m = labs(dz);
    if (m > 40000L)
        return m * 16 - f->r_km;
    return (long)lf_sqrt((unsigned long)(dx * dx + dy * dy + dz * dz)) * 16 - f->r_km;
}

/* a point (positions) inside a field */
static int in_field(const Field __far *f, long x, long y, long z)
{
    long dx = (x - f->x) / 256 / 16, dy = (y - f->y) / 256 / 16, dz = (z - f->z) / 256 / 16, r = f->r_km / 16;
    if (labs(dx) > r || labs(dy) > r || labs(dz) > r)
        return 0;
    return (unsigned long)(dx * dx + dy * dy + dz * dz) <= (unsigned long)(r * r);
}

/* a new rock's looks: its kind, its radius (a field's 80 .. 120 km, a fighter's; the window's cluster's 110 .. 240),
 * its spin's phase and speed (a turn in one to four seconds, either way) */
static void rock_looks(LzRock __far *r, int cluster)
{
    unsigned spin;
    r->kind = (unsigned char)(rnd() % g_kinds);
    r->r8 = (unsigned char)((cluster ? CLUSTER_KM_MIN + (long)(rnd() % (CLUSTER_KM_SPAN + 1))
                                     : ROCK_KM_MIN + (long)(rnd() % (ROCK_KM_SPAN + 1))) / 8);
    r->phase = (unsigned char)(rnd() & (g_frames - 1));
    spin = 1 + rnd() % 4;
    r->spin = (signed char)((rnd() & 1) ? spin : -(int)spin);
    r->pad = 0;
}

/* --- the field as an effect --- */

/* the ship's own frame (the camera's may be an outside view's) */
static void ship_basis(LfBasis *b)
{
    b->front = *(const LfVec *)&gOrientationFrontVector[0];
    b->right = *(const LfVec *)&gOrientationRightVector[0];
    b->up = *(const LfVec *)&gOrientationUpVector[0];
}

/* the old game's angle a (degrees, 0 .. 35) as the same share of our view: tan(a) x (its 160 px over its half-width
 * 160) x (our half-width 320 over FLIGHT_FOCAL), 1,024 = 1.0 */
static long view_share(int a)
{
    long r = (long)a * 1787 / 100;      /* (radians x 1,024) */
    long t = r + r * r / 1024 * r / (3L * 1024);   /* (the tangent to its cube: 2% short at 35 degrees) */
    return t * 320 / FLIGHT_FOCAL;
}

/* the rocks a field keeps at most: its kind's at the plain density, more or fewer by its own */
static long field_cap(const Field __far *f)
{
    return (long)FX_CAP * k_kind[f->type == RV_FIELD_MINE].times * f->density / ROCK_DENSITY;
}

static int cap_of(int fi)
{
    long n = field_cap(&g_f[fi]);
    if (n < 1)
        n = 1;
    return n > g_cap ? g_cap : (int)n;   /* (never past the list: pool_alloc may have found room for fewer) */
}

/* the field the point FX_CHECK_KM ahead of the nose is in (the rocks come from it), -1 none (a cluster is none) */
static int field_ahead(const LfBasis *b)
{
    long x = gPositionVector[0].x + b->front.x * FX_CHECK_KM, y = gPositionVector[0].y + b->front.y * FX_CHECK_KM,
        z = gPositionVector[0].z + b->front.z * FX_CHECK_KM;
    int i;
    for (i = 0; i < g_nf; i++)
        if (!g_f[i].cluster && in_field(&g_f[i], x, y, z))
            return i;
    return -1;
}

/* a velocity (positions a step) to cover d (positions) in t steps, no axis past 30,000 (117 km a step) */
static void fly_in(LzRock __far *r, long dx, long dy, long dz, long t)
{
    long vx = dx / t, vy = dy / t, vz = dz / t, m = labs(vx), k;
    if (labs(vy) > m) m = labs(vy);
    if (labs(vz) > m) m = labs(vz);
    if (m > 30000L) {
        k = (m + 29999L) / 30000L;
        vx /= k;
        vy /= k;
        vz /= k;
    }
    r->vx = (short)vx;
    r->vy = (short)vy;
    r->vz = (short)vz;
}

/* a rock's way across its field: a right angle off the line from it to the field's middle, up, down, left or right of
 * that line (the old game's 036A: the rock faces the middle and turns), at kps.  0: it is at the middle */
static int fly_across(LzRock __far *r, const Field __far *f, long kps)
{
    long m[3], s[3], u[3], l, k = kps * 64 / 5;   /* (a kps: 12.8 positions a step) */
    int i, pick = (int)(rnd() & 3);
    m[0] = (f->x - r->x) / 256;         /* (km) */
    m[1] = (f->y - r->y) / 256;
    m[2] = (f->z - r->z) / 256;
    l = (long)lf_sqrt((unsigned long)(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]));
    if (l < 16)
        return 0;
    for (i = 0; i < 3; i++)
        m[i] = m[i] * 1024 / l;         /* (1,024 a unit) */
    /* (across: the line crossed with the world's up, or with its x where the line is near the vertical) */
    if (labs(m[1]) < 1000) {
        s[0] = m[2]; s[1] = 0; s[2] = -m[0];
    } else {
        s[0] = 0; s[1] = -m[2]; s[2] = m[1];
    }
    l = (long)lf_sqrt((unsigned long)(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]));
    if (l < 16)
        return 0;
    for (i = 0; i < 3; i++)
        s[i] = s[i] * 1024 / l;
    u[0] = (s[1] * m[2] - s[2] * m[1]) / 1024;   /* (up off the line: across x the line) */
    u[1] = (s[2] * m[0] - s[0] * m[2]) / 1024;
    u[2] = (s[0] * m[1] - s[1] * m[0]) / 1024;
    for (i = 0; i < 3; i++) {
        long v = pick & 2 ? u[i] : s[i];
        v = (pick & 1 ? -v : v) * k / 1024;
        if (i == 0) r->vx = (short)v;
        else if (i == 1) r->vy = (short)v;
        else r->vz = (short)v;
    }
    return 1;
}

/* A rock of field fi made km ahead of the nose, in the cone (the old game's +-35 x +-20 degrees) turned to where the
 * nose is turning, where that point is in the field (the old game's 06A9), and its way: the old game's share by the
 * field's level (asteroids) at the point the player will be at after its time (its frames: the level's 52 to 75 and
 * up to 14 more, fewer by up to twice how far the player's speed is from 250 kps; less up to 4 for the rock's own:
 * each WC_FRAME_STEPS of our steps), the field's still ones still, and the rest across the field (asteroids) or at
 * the player toward a point off them by up to the kind's miss (mines).  0: no rock (outside the field, or the way
 * would not come out) */
static int rock_make(LzRock __far *r, const LfBasis *b, int fi, long km)
{
    const Field __far *f = &g_f[fi];
    const FieldKind __far *k = &k_kind[f->type == RV_FIELD_MINE];
    int a = (int)(rnd() % (2 * FX_YAW + 1)) - FX_YAW, e = (int)(rnd() % (2 * FX_PITCH + 1)) - FX_PITCH, i;
    long sa = a < 0 ? -view_share(-a) : view_share(a), se = e < 0 ? -view_share(-e) : view_share(e), d[3], lead[3], len;
    lead[0] = (b->front.x - g_prev_front.x) * FX_LEAD_STEPS * 4;   /* (1,024 = 1.0) */
    lead[1] = (b->front.y - g_prev_front.y) * FX_LEAD_STEPS * 4;
    lead[2] = (b->front.z - g_prev_front.z) * FX_LEAD_STEPS * 4;
    for (i = 0; i < 3; i++)
        lead[i] = lead[i] > 400 ? 400 : lead[i] < -400 ? -400 : lead[i];
    d[0] = b->front.x * 4 + (b->right.x * sa + b->up.x * se) / 256 + lead[0];
    d[1] = b->front.y * 4 + (b->right.y * sa + b->up.y * se) / 256 + lead[1];
    d[2] = b->front.z * 4 + (b->right.z * sa + b->up.z * se) / 256 + lead[2];
    len = (long)lf_sqrt((unsigned long)(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    if (len < 64)
        return 0;
    r->x = gPositionVector[0].x + d[0] * km / len * 256;
    r->y = gPositionVector[0].y + d[1] * km / len * 256;
    r->z = gPositionVector[0].z + d[2] * km / len * 256;
    if (!f->cluster && !in_field(f, r->x, r->y, r->z))
        return 0;
    rock_looks(r, 0);
    if (k->aimed && e > k_aim_lo[f->level] && e < k_aim_hi[f->level] && a > -19 && a < 19 && rnd() % 100 < 60) {
        long off = 25 - (long)(speedLo[0] / 10), t, t2;   /* (the old game's speed: kps / 10, km a frame) */
        if (off < 0)
            off = -off;
        if (off > 10000)
            off = 10000;
        t = k_aim_t[f->level] + (long)(rnd() % 15) - (off ? (long)(rnd() % (unsigned)(2 * off)) : 0);
        if (t < 7)
            t = 7;
        t2 = t - (long)(rnd() % 5);
        if (t2 < 3)
            t2 = 3;
        t *= WC_FRAME_STEPS;            /* (its frames as our steps) */
        t2 *= WC_FRAME_STEPS;
        fly_in(r, gPositionVector[0].x + gVelocityVector[0].x * t - r->x, gPositionVector[0].y + gVelocityVector[0].y * t - r->y,
               gPositionVector[0].z + gVelocityVector[0].z * t - r->z, t2);
        g_aimed++;
    } else if (rnd() % 100 < k->still
               || (k->across && !fly_across(r, f, k->kps_min + (long)(rnd() % (k->kps_span + 1)))))
        r->vx = r->vy = r->vz = 0;
    else if (!k->across) {
        long m1 = (long)(rnd() % (2 * k->miss_km + 1)) - k->miss_km, m2 = (long)(rnd() % (2 * k->miss_km + 1)) - k->miss_km,
            kps = k->kps_min + (long)(rnd() % (k->kps_span + 1)), dx, dy, dz, dl;
        dx = (gPositionVector[0].x + b->right.x * m1 + b->up.x * m2 - r->x) / 256;   /* (km) */
        dy = (gPositionVector[0].y + b->right.y * m1 + b->up.y * m2 - r->y) / 256;
        dz = (gPositionVector[0].z + b->right.z * m1 + b->up.z * m2 - r->z) / 256;
        dl = (long)lf_sqrt((unsigned long)(dx * dx + dy * dy + dz * dz));
        if (dl < 1)
            dl = 1;
        r->vx = (short)(dx * (kps * 64 / 5) / dl);   /* (a kps: 12.8 positions a step) */
        r->vy = (short)(dy * (kps * 64 / 5) / dl);
        r->vz = (short)(dz * (kps * 64 / 5) / dl);
    }
    g_made++;
    return 1;
}

/* --- the window's cluster --- */

/* A cluster (the bar's window: rocks_field_cluster) keeps its rocks in the field, drifting, seven times the plain
 * density (at the plain figure a small field holds six) */
#define CLUSTER_DENSITY 7
#define CLUSTER_AWAY_KM 1500L           /* a rock put back no nearer the camera than this */

static void rock_in_field(LzRock __far *r, const Field __far *f)
{
    int tries;
    for (tries = 0; tries < 8; tries++) {
        long x = (long)(rnd() % 2001) - 1000, y = (long)(rnd() % 2001) - 1000, z = (long)(rnd() % 2001) - 1000, dx, dy, dz;
        if (x * x + y * y + z * z > 1000000L)
            continue;
        r->x = f->x + x * f->r_km / 1000 * 256;
        r->y = f->y + y * f->r_km / 1000 * 256;
        r->z = f->z + z * f->r_km / 1000 * 256;
        dx = labs(r->x - gPositionVector[0].x) / 256;
        dy = labs(r->y - gPositionVector[0].y) / 256;
        dz = labs(r->z - gPositionVector[0].z) / 256;
        if (dx > CLUSTER_AWAY_KM || dy > CLUSTER_AWAY_KM || dz > CLUSTER_AWAY_KM)
            break;
    }
    rock_looks(r, 1);
    r->vx = (short)((int)(rnd() % 257) - 128);   /* (half a km a frame at most, each way: it drifts) */
    r->vy = (short)((int)(rnd() % 257) - 128);
    r->vz = (short)((int)(rnd() % 257) - 128);
}

#ifdef RV_TESTS
static void rocks_time(void);
#endif

static void field_enter(int i, int filled, const LfBasis *b)
{
    static const char __far s_in[] = "rocks: near field %d: up to %d rocks (%Fs)",
        __far s_full[] = "the flight starts in it", __far s_edge[] = "they come as the point ahead is in a field",
        __far s_cl[] = "a cluster";
    const Field __far *f = &g_f[i];
    int j;
    if (f->cluster) {
        /* (the field's volume, (4/3) pi R^3, in thousandths of (10,000 km)^3) */
        long rv = f->r_km, vol1000 = 4189L * (rv / 100) * (rv / 100) / 1000 * (rv / 100) / 1000;
        g_want_n = (int)((long)f->density * vol1000 / 1000) * CLUSTER_DENSITY;
    } else                              /* (room for the most any of the flight's fields keeps: they share the art) */
        for (j = 0, g_want_n = 1; j < g_nf; j++)
            if (!g_f[j].cluster && field_cap(&g_f[j]) > g_want_n)
                g_want_n = (int)field_cap(&g_f[j]);
    if (g_want_n < 1)
        g_want_n = 1;
    dat_level(XMS_HAZARD);              /* (first: the art goes at the new level, the archive's reads after it) */
    if (!art_in() || !pool_alloc(g_want_n)) {
        pool_free();
        xms_free_level(XMS_HAZARD);
        dat_level(XMS_MISSION);
        g_blk = 0;
        g_active = -2 - i;              /* (tried: not again until the player leaves) */
        return;
    }
    g_seed = 99991UL + 7919UL * i + frameCounter;
    g_n = 0;
    g_cur = -1;
    g_src = i;
    g_prev_front = b->front;
    if (f->cluster) {
        while (g_n < g_want_n && g_n < g_cap) {
            rock_in_field(&g_r[g_n], f);
            g_n++;
        }
    } else if (filled) {                /* (the flight begins in it: rocks already about, near and far) */
        int fi = field_ahead(b), cap;
        if (fi < 0)
            fi = i;
        cap = cap_of(fi) * 2 / 3;
        while (g_n < cap && rock_make(&g_r[g_n], b, fi, FX_SPAWN_KM * (25 + rnd() % 75) / 100))
            g_n++;
        g_cur = fi;
    }
#ifdef RV_TESTS
    {
        static int __far checked;
        if (!checked++) {
#ifndef RV_NO_TWINS                     /* (DOS's RVT.EXE has no twins: RVP.EXE /twins checks the place there) */
            rocks_selftest();
#endif
            rocks_time();
        }
    }
#endif
    g_active = i;
    g_nfrag = 0;
    shots_on_rocks = f->cluster ? 0 : rocks_shot;
    rv_notef(s_in, i, g_cap, f->cluster ? s_cl : filled ? s_full : s_edge);
}

static void field_leave(void)
{
    static const char __far s_out[] = "rocks: away from field %d: its rocks and art given back";
    if (g_active >= 0)
        rv_notef(s_out, g_active);
    shots_on_rocks = 0;
    pool_free();
    if (g_blk) {
        xms_free_level(XMS_HAZARD);
        dat_level(XMS_MISSION);
    }
    g_blk = 0;
    g_active = -1;
    g_cur = -1;
    g_nfrag = 0;
    fieldRockTally = 0;
}

void rocks_flight_start(void)
{
    int i;
    g_nf = 0;
    g_active = -1;
    g_cur = -1;
    g_src = 0;
    g_tracker_on = -1;                  /* (the flight's palette has just been loaded, or is about to be) */
    g_kb[0] = g_kb[1] = g_kb[2] = 0;
    fieldCount = 0;
    fieldRockTally = 0;
    currentField = 0;
    memset(entitiesToDespawn, 0xFF, entitiesToDespawn_SIZE);   /* (no rock is a slot) */
    for (i = 0; i < FIELDS_MAX; i++)
        memset(fieldTable + fieldTable_SIZE * i, 0, fieldTable_SIZE);
    g_t_place = g_t_small = g_t_big = g_t_keep = g_t_blocks = 0;
    g_frames_in = g_sum_live = g_sum_small = g_sum_big = g_rows = g_respawned = g_hits_n = g_aimed = g_blocks_n = 0;
    g_made = g_shot_n = g_broken = g_seeks = 0;
    g_max_live = g_max_on = 0;
}

void rocks_flight_end(void)
{
    int i;
    field_leave();
    for (i = 0; i < g_nf; i++)
        field_slot(i, 0);
    g_nf = 0;
    fieldCount = 0;
    currentField = 0;
}

/* the layer's "rocks off" (the thunk's body): this machine's rocks gone, the fields forgotten */
void __far __cdecl clearFields_impl(void)
{
    static const char __far s_clear[] = "rocks: the fields cleared";
    rocks_flight_end();
    rv_notef(s_clear);
}

/* --- a frame --- */

/* a broken rock (the user, 2026-10-24): FRAG_N pieces a third its size, from about where it was, flung out every way
 * and going on with the velocity given (positions a step: the player's three quarters when they flew into it, the
 * rock's own when it was shot), each spinning fast, gone in one to two seconds; never hit */
static void shatter(const LzRock __far *r, long bx, long by, long bz)
{
    long R = 8L * r->r8;
    int k;
    for (k = 0; k < FRAG_N && g_nfrag < FRAG_MAX; k++) {
        LzRock __far *f = &g_frag[g_nfrag];
        long ox = (long)(rnd() % 2001) - 1000, oy = (long)(rnd() % 2001) - 1000, oz = (long)(rnd() % 2001) - 1000, v;
        *f = *r;
        f->r8 = (unsigned char)(r->r8 / 4 + 1 + rnd() % (r->r8 / 4 + 1));
        f->x = r->x + ox * R / 1000 * 128;  /* (within half its radius) */
        f->y = r->y + oy * R / 1000 * 128;
        f->z = r->z + oz * R / 1000 * 128;
        v = bx + ox * 6 * 256 / 1000;       /* (6 km a frame outward at most) */
        f->vx = (short)(v > 32000L ? 32000L : v < -32000L ? -32000L : v);
        v = by + oy * 6 * 256 / 1000;
        f->vy = (short)(v > 32000L ? 32000L : v < -32000L ? -32000L : v);
        v = bz + oz * 6 * 256 / 1000;
        f->vz = (short)(v > 32000L ? 32000L : v < -32000L ? -32000L : v);
        f->phase = (unsigned char)(rnd() & (g_frames - 1));
        f->spin = (signed char)((rnd() & 1) ? 4 : -4);
        g_frag_life[g_nfrag++] = (unsigned char)(30 + rnd() % 30);
    }
}

/* a rock taken out of the set: sent far, so the next frame's placing reaps it (keep_effect removes it, a cluster puts
 * it back in its field) */
#define ROCK_GONE(r) ((r)->x = gPositionVector[0].x + 0x3000000L)

/* a shot's last step (from its head less its velocity, to its head) against the rocks (shots_move calls it while a
 * field's rocks are out): 1 when it went into one, spent; the rock breaks one time in three (the old game's) */
static int16_t rocks_shot(const Shot __far *s)
{
    long ax = s->x - s->vx, ay = s->y - s->vy, az = s->z - s->vz, dx = s->vx >> 4, dy = s->vy >> 4, dz = s->vz >> 4, dd;
    int i;
    if (g_active < 0 || !g_r || labs(dx) > 16000L || labs(dy) > 16000L || labs(dz) > 16000L)
        return 0;
    dd = dx * dx + dy * dy + dz * dz;   /* (sixteenths of positions: the squares fit) */
    for (i = 0; i < g_n; i++) {
        LzRock __far *r = &g_r[i];
        long R = 8L * 256 * r->r8, px, py, pz, t, km;
        if (labs(s->x - r->x) > R + labs(s->vx) || labs(s->y - r->y) > R + labs(s->vy) || labs(s->z - r->z) > R + labs(s->vz))
            continue;
        px = (r->x - ax) >> 4;
        py = (r->y - ay) >> 4;
        pz = (r->z - az) >> 4;
        t = dd ? (px * dx + py * dy + pz * dz) * 16 / dd : 0;   /* (16ths of the way along) */
        if (t < 0) t = 0;
        if (t > 16) t = 16;
        px -= dx * t / 16;
        py -= dy * t / 16;
        pz -= dz * t / 16;
        if (px * px + py * py + pz * pz > (R >> 4) * (R >> 4))
            continue;
        g_shot_n++;
        km = labs(r->x - gPositionVector[0].x) / 256 + labs(r->y - gPositionVector[0].y) / 256
            + labs(r->z - gPositionVector[0].z) / 256;
        if (km > 8000)
            km = 8000;
        if (rnd() % 3 == 0) {
            shatter(r, r->vx, r->vy, r->vz);
            sfx_play(RV_SFX_BOOM_SMALL, (int16_t)(256 - km * 224 / 8000), 256);
            ROCK_GONE(r);
            g_broken++;
        } else
            sfx_play(RV_SFX_HIT_FAR, (int16_t)(200 - km * 180 / 8000), 256);
        return 1;
    }
    return 0;
}

/* the player's hits: each rock within reach against their ship's hit box (a sphere against a box, in the ship's
 * frame, km) */
static void hits(int n, const LfBasis *cam)
{
    static const char __far s_hit[] = "rocks: frame %u: a %d km rock (kind %d) hit the player at %lu kps, %d%% head on: "
        "%u damage";
    const RvShipType __far *t = ship_type(gInstanceShipTypes[0]);
    long h[3], H;
    int i, k;
    if (!t || maybeEntityType[0] != 0xC)
        return;
    H = t->radius_km;                   /* (collide.c's half box, km) */
    for (k = 0; k < 3; k++)
        h[k] = H * t->hit[k] / 127;
    for (i = 0; i < n; i++) {
        LzRock __far *r = &g_r[g_hit[i]];
        long rx = (r->x - gPositionVector[0].x) / 16, ry = (r->y - gPositionVector[0].y) / 16,
            rz = (r->z - gPositionVector[0].z) / 16, p[3], c[3], d[3], R = 8L * r->r8, d2, dl, wx, wy, wz;
        unsigned long speed;
        int head;
        unsigned short qty;
        /* into the ship's frame, km (sixteenths x 256-scaled axes / 4096) */
        p[0] = (rx * cam->right.x + ry * cam->right.y + rz * cam->right.z) / 4096;
        p[1] = (rx * cam->up.x + ry * cam->up.y + rz * cam->up.z) / 4096;
        p[2] = (rx * cam->front.x + ry * cam->front.y + rz * cam->front.z) / 4096;
        for (k = 0; k < 3; k++) {
            c[k] = p[k] < -h[k] ? -h[k] : p[k] > h[k] ? h[k] : p[k];
            d[k] = p[k] - c[k];
        }
        d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (d2 >= R * R)
            continue;
        dl = (long)lf_sqrt((unsigned long)d2);
        if (dl < 1)
            dl = 1;
        g_hits_n++;
        /* the harm: the speed they met at (the player's less the rock's, kps), the more head on the more, and the
         * rock's size */
        wx = (gVelocityVector[0].x - r->vx) / 16;
        wy = (gVelocityVector[0].y - r->vy) / 16;
        wz = (gVelocityVector[0].z - r->vz) / 16;
        speed = lf_sqrt((unsigned long)(wx * wx + wy * wy + wz * wz)) * 16UL * 5 / 64;
        head = (int)(d[2] * 100 / dl);
        if (head < 20)
            head = 20;
        qty = (unsigned short)(speed * head / 100 * R / 4000 + 1);
        ((RvVec3 *)tempVector)->x = gPositionVector[0].x + (c[0] * cam->right.x + c[1] * cam->up.x + c[2] * cam->front.x);
        ((RvVec3 *)tempVector)->y = gPositionVector[0].y + (c[0] * cam->right.y + c[1] * cam->up.y + c[2] * cam->front.y);
        ((RvVec3 *)tempVector)->z = gPositionVector[0].z + (c[0] * cam->right.z + c[1] * cam->up.z + c[2] * cam->front.z);
        rv_notef(s_hit, frameCounter, (int)(2 * R), r->kind, speed, head, qty);
        do_damage((unsigned short)g_f[g_src].slot, 0, qty, (unsigned short)(unsigned)tempVector);
        /* tossed aside (the user, 2026-10-24): across the player's way, away from the rock (dead ahead: a side
         * at random), at a speed of its own dying away over a second; theirs is kept */
        {
            long tx = -d[0], ty = -d[1], tl, kb = (long)speedLo[0] / 20 + 30;   /* (km a frame) */
            if (labs(tx) + labs(ty) < R / 4) {
                tx = (rnd() & 1) ? R : -R;
                ty = (long)(rnd() % (2 * R + 1)) - R;
            }
            tl = (long)lf_sqrt((unsigned long)(tx * tx + ty * ty));
            if (tl < 1)
                tl = 1;
            g_kb[0] = (tx * cam->right.x + ty * cam->up.x) * kb / tl;
            g_kb[1] = (tx * cam->right.y + ty * cam->up.y) * kb / tl;
            g_kb[2] = (tx * cam->right.z + ty * cam->up.z) * kb / tl;
        }
        /* the rock breaks: its pieces fly on with the player, and it is gone */
        shatter(r, gVelocityVector[0].x * 3 / 4, gVelocityVector[0].y * 3 / 4, gVelocityVector[0].z * 3 / 4);
        ROCK_GONE(r);
    }
}

/* The old game's mines close in (its 092E, called for each mine in turn every 20 of its frames from 09BB): a mine turns
 * at where the player will be 20 of its frames on and puts 2 km a frame (20 kps) more of speed that way, so a mine
 * missed comes round again, faster.  Ours had been aimed once, as they were made (the user, 2026-11-04: "the mine field
 * does not seek towards the player actively").  At most MINE_KPS (my cap: the old game's had none that was found), and
 * not within MINE_NEAR_KM, where its way is set (its own check of some 1,500 there, read as a distance) */
#define MINE_SEEK_STEPS (20 * WC_FRAME_STEPS)
#define MINE_PUSH_KPS 20L
#define MINE_KPS 200L
#define MINE_NEAR_KM 300L
static void seek_mines(void)
{
    int i;
    if (g_src < 0 || g_src >= g_nf || g_f[g_src].type != RV_FIELD_MINE || g_f[g_src].cluster)
        return;
    for (i = 0; i < g_n; i++) {
        LzRock __far *r = &g_r[i];
        long dx, dy, dz, d, vx, vy, vz, v, cap = MINE_KPS * 64 / 5, push = MINE_PUSH_KPS * 64 / 5;
        if ((frameCounter + (unsigned)i) % MINE_SEEK_STEPS)
            continue;
        dx = (gPositionVector[0].x + gVelocityVector[0].x * MINE_SEEK_STEPS - r->x) / 256;   /* (km) */
        dy = (gPositionVector[0].y + gVelocityVector[0].y * MINE_SEEK_STEPS - r->y) / 256;
        dz = (gPositionVector[0].z + gVelocityVector[0].z * MINE_SEEK_STEPS - r->z) / 256;
        if (labs(dx) > 20000L || labs(dy) > 20000L || labs(dz) > 20000L)
            continue;                   /* (far past where it would be reaped: nothing to seek) */
        d = (long)lf_sqrt((unsigned long)(dx * dx + dy * dy + dz * dz));
        if (d <= MINE_NEAR_KM)
            continue;
        vx = r->vx + dx * push / d;     /* (a kps: 12.8 positions a step) */
        vy = r->vy + dy * push / d;
        vz = r->vz + dz * push / d;
        v = (long)lf_sqrt((unsigned long)(vx * vx + vy * vy + vz * vz));
        if (v > cap) {
            vx = vx * cap / v;
            vy = vy * cap / v;
            vz = vz * cap / v;
        }
        r->vx = (short)vx;
        r->vy = (short)vy;
        r->vz = (short)vz;
        g_seeks++;
    }
}

/* the reaped ones removed, and new ones made while the point ahead is in a field: the old game's three as a field
 * begins, then its chance ((speed/10 + 30)/215 a frame of its, WC_FRAME_STEPS of our steps), up to the field's number */
static void keep_effect(const LfBasis *b, int nreap)
{
    int i, fi;
    for (i = 0; i < nreap; i++) {       /* (the highest index first: a removal moves the last rock into the hole) */
        int k, j = i;
        for (k = i + 1; k < nreap; k++)
            if (g_reap[k] > g_reap[j])
                j = k;
        k = g_reap[j];
        g_reap[j] = g_reap[i];
        g_reap[i] = (short)k;
        g_r[k] = g_r[--g_n];
        g_respawned++;
    }
    fi = maybeEntityType[0] == 0xC ? field_ahead(b) : -1;
    if (fi >= 0) {
        int cap = cap_of(fi);
        unsigned long chance = (speedLo[0] / 10 + 30) * k_kind[g_f[fi].type == RV_FIELD_MINE].times;
        if (g_cur != fi) {
            for (i = 0; i < FX_ENTRY && g_n < cap; i++)
                if (rock_make(&g_r[g_n], b, fi, FX_SPAWN_KM))
                    g_n++;
        } else if (g_n < cap && rnd() % (215U * WC_FRAME_STEPS) < (chance > 215U * WC_FRAME_STEPS ? 215U * WC_FRAME_STEPS
                                                                    : (unsigned)chance))
            if (rock_make(&g_r[g_n], b, fi, FX_SPAWN_KM))
                g_n++;
        g_src = fi;
    }
    g_cur = fi;
    g_prev_front = b->front;
    fieldRockTally = (unsigned short)g_n;
}

/* a cluster's: the reaped ones put back in it, and the set topped up */
static void keep_cluster(int nreap)
{
    const Field __far *f = &g_f[g_active];
    int i, top = 0;
    for (i = 0; i < nreap; i++)
        rock_in_field(&g_r[g_reap[i]], f);
    while (g_n < g_want_n && g_n < g_cap && top++ < 8) {
        rock_in_field(&g_r[g_n], f);
        g_n++;
    }
    fieldRockTally = (unsigned short)g_n;
}

/* the depth keys: farthest first (a counting sort on 64 km steps) */
static void order_big(void)
{
    static unsigned short __far count[256];
    static unsigned char __far key[ROCK_OUT];
    int i, k, at;
    _fmemset(count, 0, sizeof count);
    for (i = 0; i < g_nb; i++) {
        long z = g_o[g_order[i]].z >> SHIFT_KEY;
        key[i] = (unsigned char)(255 - (z > 255 ? 255 : z));
        count[key[i]]++;
    }
    for (k = 0, at = 0; k < 256; k++) {
        int c = count[k];
        count[k] = (unsigned short)at;
        at += c;
    }
    {
        static short __far tmp[ROCK_OUT];
        for (i = 0; i < g_nb; i++)
            tmp[count[key[i]]++] = g_order[i];
        _fmemcpy(g_order, tmp, g_nb * sizeof(short));
    }
}

void rocks_frame(const LfBasis *cam, int cx, int cy, int focal, int y0, int y1)
{
    unsigned long t0 = clock240();
    long f3[3], r3[3], u3[3], best = 0x7FFFFFFFL;
    int i, near_one = -1, nreap, nhit, cluster;
    LfBasis ship;
    ship_basis(&ship);
    /* which field the player is in, and the nearest (every 16 frames is plenty): the art read in as one's edge comes
     * within FX_NEAR_KM, kept while one is near or rocks still fly (they go past FX_FAR_KM), the fields sharing it (a
     * mine field takes the same rocks: its mines are rocks with trackers, tracker_colour()) */
    if ((frameCounter & 15) == 0 || g_active == -1) {
        currentField = 0;
        for (i = 0; i < g_nf; i++) {
            long out = beyond(&g_f[i]);
            if (out < 0)
                currentField = (unsigned short)(unsigned)(fieldTable + fieldTable_SIZE * i);
            if (out < best) {
                best = out;
                near_one = i;
            }
        }
        if (g_active >= 0 && best >= FX_NEAR_KM + ZONE_PAD_KM && (g_n == 0 || best >= FX_NEAR_KM + FX_FAR_KM + ZONE_PAD_KM))
            field_leave();
        if (g_active <= -2 && (best >= FX_NEAR_KM || near_one != -2 - g_active))
            g_active = -1;              /* (left the one whose art would not come in) */
        if (g_active == -1 && near_one >= 0 && best < FX_NEAR_KM)
            field_enter(near_one, frameCounter < 2 && best < 0, &ship);
    }
    g_ns = g_nb = g_no = 0;
    if (g_kb[0] || g_kb[1] || g_kb[2]) {    /* (tossed aside by a rock, dying away) */
        gPositionVector[0].x += g_kb[0];
        gPositionVector[0].y += g_kb[1];
        gPositionVector[0].z += g_kb[2];
        for (i = 0; i < 3; i++)
            g_kb[i] = labs(g_kb[i]) < 64 ? 0 : g_kb[i] * 7 / 8;
    }
    tracker_colour();
    if (g_active < 0 || !g_r)
        return;
    f3[0] = cam->front.x; f3[1] = cam->front.y; f3[2] = cam->front.z;
    r3[0] = cam->right.x; r3[1] = cam->right.y; r3[2] = cam->right.z;
    u3[0] = cam->up.x; u3[1] = cam->up.y; u3[2] = cam->up.z;
    lz_camera(&g_cam, gPositionVector[0].x, gPositionVector[0].y, gPositionVector[0].z, f3, r3, u3, cx, cy, focal, 0,
              100, 70000L, 0, 0, y0, y1);
    g_rc.dk = (unsigned long)focal * 65536UL * 16;   /* 2 x 8 km x 4,096 x focal x 16 / 16: d x16 = r8 dk / Z */
    cluster = g_f[g_active].cluster;
    if (cluster) {
        g_rc.far2 = (unsigned long)BUBBLE_KM * BUBBLE_KM;
        g_rc.fade2 = g_rc.far2 / 10000 * BUBBLE_FADE * BUBBLE_FADE;
    } else {                            /* (the old game's: gone past 4,300 km, 60 less for each km a frame of speed, to
                                           3,100; a rock made at 3,050 full size, smaller only out past it) */
        long reap = FX_FAR_KM - 60L * (long)(speedLo[0] / 10);   /* (not "far": Watcom's keyword) */
        if (reap < FX_FAR_MIN_KM)
            reap = FX_FAR_MIN_KM;
        g_rc.far2 = (unsigned long)reap * reap;
        g_rc.fade2 = (unsigned long)FX_SPAWN_KM * FX_SPAWN_KM;
    }
    g_rc.reach = ROCK_REACH_KM * 4096;
    g_rc.anim = frameCounter;
    g_rc.mask = (unsigned short)(g_frames - 1);
    g_rc.dmax = DMAX;
    g_rc.maxreap = ROCK_REAP;
    g_rc.maxclose = ROCK_HITS;
    if (!cluster)
        seek_mines();                   /* (before they fly: rv_rocks_place moves them by their velocity) */
    g_no = rv_rocks_place(g_r, g_n, &g_cam, &g_rc, g_o, ROCK_OUT, g_reap, g_hit);
    nreap = g_rc.nreap;
    nhit = g_rc.nclose;
    if (g_nfrag) {                      /* the broken rocks' pieces: flown and placed the same way, never hit or reaped */
        LzRockCam rc = g_rc;
        rc.maxreap = rc.maxclose = 0;
        g_no += rv_rocks_place(g_frag, g_nfrag, &g_cam, (LzRockCam __far *)&rc, g_o + g_no, ROCK_OUT - g_no, g_reap,
                               g_hit);
        for (i = g_nfrag - 1; i >= 0; i--)
            if (--g_frag_life[i] == 0) {
                g_frag[i] = g_frag[--g_nfrag];
                g_frag_life[i] = g_frag_life[g_nfrag];
            }
    }
    g_t_place += clock240() - t0;
    /* small ones as balls, big ones in depth order */
    for (i = 0; i < g_no; i++) {
        const LzRockOut __far *o = &g_o[i];
        if (o->d <= (unsigned)g_small) {
            LzBallOut __far *b = &g_so[g_ns++];
            int d = o->d, f = o->frame * g_sframes / g_frames;
            b->x = (short)(o->x - (d - 1) / 2);
            b->y = (short)(o->y - (d - 1) / 2);
            b->d = (unsigned char)d;
            b->set = 0;
            b->pad = 0;
            b->spr = g_smallspr + (unsigned)((o->kind * g_sframes + f) * g_small_set + SUM_SQ(d - 1));
        } else
            g_order[g_nb++] = (short)i;
    }
    order_big();
    t0 = clock240();
    if (nhit)
        hits(nhit, &ship);
    if (cluster)
        keep_cluster(nreap);
    else
        keep_effect(&ship, nreap);
    g_t_keep += clock240() - t0;
    g_frames_in++;
    g_sum_live += g_n;
    g_sum_small += g_ns;
    g_sum_big += g_nb;
    if (g_n > g_max_live) g_max_live = g_n;
    if (g_no > g_max_on) g_max_on = g_no;
}

int rocks_on_screen(int *small, int *big)
{
    *small = g_ns;
    *big = g_nb;
    return g_ns + g_nb;
}

int rocks_live(void) { return g_active >= 0 ? g_n : 0; }

/* (a field whose art had not come in: tried again at the next frame) */
void rocks_retry(void)
{
    if (g_active <= -2)
        g_active = -1;
}

/* --- the draws --- */

void rocks_draw_small(int y0, int y1, const short __far *ext, int ext_y0, int ext_rows)
{
    unsigned long t0 = clock240();
    int run0, run1;
    if (g_ns && vid_run(&run0, &run1) && bolts_rows(&y0, &y1, &ext, &ext_y0, ext_rows))
        rv_balls_draw(g_so, g_ns, vid_row(run0), run0, y0, y1, ext, ext_y0);
    g_t_small += clock240() - t0;
}

void rocks_draw_begin(void) { g_next = 0; }

/* a big sprite's record: its head and run table into g_hdr (one move: as much as there is room for, the pixels'
 * first bytes with it); its pixels' offset and (copy) their bytes returned */
static unsigned long record(int kind, int frame, int s, unsigned long *len)
{
    unsigned char e[4];
    unsigned long rec, n;
    unsigned i, rows, at;
    xms_get(g_blk, g_index_at + 4UL * (((unsigned long)kind * g_frames + frame) * g_nbig + s), e, 4);
    rec = e[0] | ((unsigned long)e[1] << 8) | ((unsigned long)e[2] << 16) | ((unsigned long)e[3] << 24);
    n = g_size - rec < HDR_MAX ? g_size - rec : HDR_MAX;
    xms_get(g_blk, rec, g_hdr, n);
    rows = g_hdr[0] | g_hdr[1] << 8;
    if (len) {
        for (i = 0, at = 4, *len = 0; i < rows; i++) {
            unsigned runs = g_hdr[at++];
            for (; runs; runs--, at += 2)
                *len += g_hdr[at + 1];
        }
    }
    return rec + 4 + (g_hdr[2] | g_hdr[3] << 8);
}

static void draw_big(const LzRockOut __far *o, int y0, int y1, const short __far *ext, int ext_y0, int run0)
{
    int d = o->d > DMAX ? DMAX : o->d, s = g_size_of[d], k = g_k_of[d], ds = g_bigd[s], D = ds * k, top, left;
    unsigned long pix, len;
    top = o->y - (D - 1) / 2;
    if (top >= y1 || top + D <= y0)
        return;
    left = o->x - (D - 1) / 2;
    if (left >= SCR_W || left + D <= 0)
        return;
    if (k == 1 && g_rock_xms) {
        pix = record(o->kind, o->frame, s, 0);
        rv_rocks_xms(g_hdr + 4, ds, left, top, g_handle, g_base + pix, vid_row(run0), run0, y0, y1, ext, ext_y0);
        g_rows += ds;
        return;
    }
    {
        unsigned long t0 = clock240();
        pix = record(o->kind, o->frame, s, &len);
        xms_get(g_blk, pix, gfx_scratch(), len);
        rv_rocks_copy(g_hdr + 4, ds, left, top, k, gfx_scratch(), vid_row(run0), run0, y0, y1, ext, ext_y0);
        g_rows += ds;
        if (k > 1) {
            g_t_blocks += clock240() - t0;
            g_blocks_n++;
        }
    }
}

void rocks_draw_to(long depth, int y0, int y1, const short __far *ext, int ext_y0, int ext_rows)
{
    unsigned long t0;
    int run0, run1;
    long z = depth * 16;                /* (positions to camera units) */
    if (g_next >= g_nb || g_o[g_order[g_next]].z <= z)
        return;
    t0 = clock240();
    if (vid_run(&run0, &run1) && bolts_rows(&y0, &y1, &ext, &ext_y0, ext_rows))
        while (g_next < g_nb && g_o[g_order[g_next]].z > z)
            draw_big(&g_o[g_order[g_next++]], y0, y1, ext, ext_y0, run0);
    g_t_big += clock240() - t0;
}

void rocks_draw_rest(int y0, int y1, const short __far *ext, int ext_y0, int ext_rows)
{
    rocks_draw_to(-0x7FFFFFFL, y0, y1, ext, ext_y0, ext_rows);
}

void rocks_report(void)
{
    static const char __far s_r[] = "rocks: %lu frames in a field: %lu rocks kept on average (%d at most), on screen "
        "%lu small and %lu big (%d at most), %lu made (%lu at where the player would be), %lu gone, %lu hits on the "
        "player, %lu shots into rocks (%lu broke them), %lu mines' turns at the player",
        __far s_ms[] = "rocks: ms a frame: placed %lu.%02lu, small drawn %lu.%02lu, big drawn %lu.%02lu (%lu rows, %Fs; "
        "of it the nearest, past 128 px, as blocks %lu.%02lu in %lu.%02lu band draws), kept and hit %lu.%02lu",
        __far s_xms[] = "a move a row", __far s_copy[] = "copied";
    unsigned long f = g_frames_in ? g_frames_in : 1;
#define MS(t) ((t) * 100000UL / 240 / f / 100), ((t) * 100000UL / 240 / f % 100)
    if (!g_frames_in)
        return;
    rv_logf(s_r, g_frames_in, g_sum_live / f, g_max_live, g_sum_small / f, g_sum_big / f, g_max_on, g_made, g_aimed,
            g_respawned, g_hits_n, g_shot_n, g_broken, g_seeks);
    rv_logf(s_ms, MS(g_t_place), MS(g_t_small), MS(g_t_big), g_rows / f, g_rock_xms ? s_xms : s_copy, MS(g_t_blocks),
            g_blocks_n / f, g_blocks_n * 100 / f % 100, MS(g_t_keep));
#undef MS
}

#ifdef RV_TESTS
/* --- rv_rocks_place checked (its C reference, lz_rocks_place, is twins.c's: the host draws with it) --- */

/* what a big rock's row costs each way (logged): one rock of each of a few sizes drawn again and again into a
 * band of its own, the whole width open */
static void rocks_time(void)
{
    static const char __far s_t[] = "rocks: a %d px rock: %ld.%02ld us a row moved by XMS, %ld.%02ld copied (%ld.%02ld with "
        "its one move), %ld.%02ld as 2 x 2 blocks a source row (%ld.%02ld a screen row)",
        __far s_mem[] = "rocks: no memory for the timings";
    static const char __far s_p[] = "rocks: placing %d rocks: %ld.%02ld us a rock (%d of them on screen)";
    static const unsigned char __far want[3] = { 32, 64, 128 };
    int rows = 64, w, i, k;
    unsigned char __far *band = (unsigned char __far *)_fmalloc((unsigned)rows * SCR_W);
    short __far *ext = (short __far *)_fmalloc(rows * 2 * sizeof(short));
    if (!band || !ext) {
        rv_logf(s_mem);
        goto out;
    }
    for (i = 0; i < rows; i++) {
        ext[2 * i] = 0;
        ext[2 * i + 1] = SCR_W;
    }
    if (g_n) {                          /* the set as it is, through a camera where the player is, looking down +z */
        long f3[3] = { 0, 0, 256 }, r3[3] = { 256, 0, 0 }, u3[3] = { 0, 256, 0 };
        unsigned long t0 = clock240(), it = 0, us;
        int on = 0;
        LzRockCam rc = g_rc;
        lz_camera(&g_cam, gPositionVector[0].x, gPositionVector[0].y, gPositionVector[0].z, f3, r3, u3, SCR_W / 2, 140,
                  400, 0, 100, 70000L, 0, 0, 20, 260);
        rc.dk = 400UL * 65536UL * 16;
        rc.far2 = (unsigned long)BUBBLE_KM * BUBBLE_KM;
        rc.fade2 = rc.far2 / 10000 * BUBBLE_FADE * BUBBLE_FADE;
        rc.reach = ROCK_REACH_KM * 4096;
        rc.mask = (unsigned short)(g_frames - 1);
        rc.dmax = DMAX;
        rc.maxreap = ROCK_REAP;
        rc.maxclose = ROCK_HITS;
        do {
            on = rv_rocks_place(g_r, g_n, &g_cam, (LzRockCam __far *)&rc, g_o, ROCK_OUT, g_reap, g_hit);
            it++;
        } while (clock240() - t0 < 60);
        us = (clock240() - t0) * 416667UL / it / g_n;
        rv_logf(s_p, g_n, us / 100, us % 100, on);
    }
    for (w = 0; w < 3; w++) {
        int s = g_size_of[want[w]], ds = g_bigd[s], n;
        unsigned long pix, len, t0, it, dt[4];
        pix = record(0, 0, s, &len);
        n = ds < rows ? ds : rows;      /* (the rows that fall in the band) */
        for (k = 0; k < 4; k++) {
            t0 = clock240();
            it = 0;
            do {
                if (k == 0)
                    rv_rocks_xms(g_hdr + 4, ds, 100, 0, g_handle, g_base + pix, band, 0, 0, rows, ext, 0);
                else {
                    if (k == 2 || it == 0)
                        xms_get(g_blk, pix, gfx_scratch(), len);
                    rv_rocks_copy(g_hdr + 4, ds, 100, 0, k == 3 ? 2 : 1, gfx_scratch(), band, 0, 0, rows, ext, 0);
                }
                it++;
            } while (clock240() - t0 < 60);
            /* hundredths of a microsecond a row */
            dt[k] = (clock240() - t0) * 416667UL / it / (k == 3 ? (unsigned long)(rows + 1) / 2 : (unsigned long)n);
        }
        rv_logf(s_t, ds, dt[0] / 100, dt[0] % 100, dt[1] / 100, dt[1] % 100, dt[2] / 100, dt[2] % 100,
                dt[3] / 100, dt[3] % 100, dt[3] / 200, dt[3] / 2 % 100);
    }
out:
    if (band) _ffree(band);
    if (ext) _ffree(ext);
}

#ifndef RV_NO_TWINS
long rocks_selftest(void)
{
    static const char __far s_mem[] = "rocks: no memory for the self-test",
        __far s_n[] = "rocks: THE PLACING DISAGREES: asm %d on screen, %d reaped, %d close; C %d, %d, %d",
        __far s_one[] = "rocks: rock %d: C %d,%d d %u k %d f %d z %ld; asm %d,%d d %u k %d f %d z %ld",
        __far s_ok[] = "rocks: the asm agrees with the C (%d of %d rocks on screen, %d reaped, %d close)";
#define TN 600
    LzRock __far *r = (LzRock __far *)_fmalloc(2 * TN * sizeof(LzRock)), __far *r2 = r + TN;   /* (placing flies them:
                                           the C and the asm each its own copy) */
    LzRockOut __far *oc = (LzRockOut __far *)_fmalloc(2 * ROCK_OUT * sizeof(LzRockOut)), __far *oa = oc + ROCK_OUT;
    short __far *lists = (short __far *)_fmalloc(4 * 64 * sizeof(short));
    LzRockCam rcc, rca;
    long f3[3] = { 0, 0, 256 }, r3[3] = { 256, 0, 0 }, u3[3] = { 0, 256, 0 };
    int i, nc, na, bad = 0;
    unsigned long seed = g_seed;
    if (!r || !oc || !lists) {
        rv_logf(s_mem);
        bad = -2;
        goto out;
    }
    /* a turned camera (front a little up and right, rolled), rocks every way round it, some at the edges */
    f3[0] = 60; f3[1] = 40; f3[2] = 246;
    r3[0] = 247; r3[1] = -30; r3[2] = -55;
    u3[0] = -38; u3[1] = 250; u3[2] = -33;
    lz_camera(&g_cam, 1000000L, -2000000L, 3000000L, f3, r3, u3, SCR_W / 2, 180, 400, 0, 100, 70000L, 0, 0, 40, 320);
    for (i = 0; i < TN; i++) {
        long s = i < 40 ? 1500 : i < 80 ? 40000 : 12000;   /* (some within reach, some far out of the bubble) */
        r[i].x = 1000000L + ((long)(rnd() % 2001) - 1000) * s / 1000 * 256;
        r[i].y = -2000000L + ((long)(rnd() % 2001) - 1000) * s / 1000 * 256;
        r[i].z = 3000000L + ((long)(rnd() % 2001) - 1000) * s / 1000 * 256;
        r[i].kind = (unsigned char)(rnd() & 3);
        r[i].r8 = (unsigned char)(10 + rnd() % 116);
        r[i].phase = (unsigned char)(rnd() & 31);
        r[i].spin = (signed char)((int)(rnd() % 9) - 4);
        r[i].vx = (short)((int)(rnd() % 4001) - 2000);
        r[i].vy = (short)((int)(rnd() % 4001) - 2000);
        r[i].vz = (short)((int)(rnd() % 4001) - 2000);
        r[i].pad = 0;
    }
    rcc.dk = 400UL * 65536UL * 16;
    rcc.far2 = (unsigned long)BUBBLE_KM * BUBBLE_KM;
    rcc.fade2 = rcc.far2 / 10000 * BUBBLE_FADE * BUBBLE_FADE;
    rcc.reach = ROCK_REACH_KM * 4096;
    rcc.anim = 12345;
    rcc.mask = 31;
    rcc.dmax = DMAX;
    rcc.maxreap = 64;
    rcc.maxclose = 64;
    rca = rcc;
    _fmemcpy(r2, r, TN * sizeof(LzRock));
    nc = lz_rocks_place(r, TN, &g_cam, (LzRockCam __far *)&rcc, oc, ROCK_OUT, lists, lists + 64);
    na = rv_rocks_place(r2, TN, &g_cam, (LzRockCam __far *)&rca, oa, ROCK_OUT, lists + 128, lists + 192);
    if (nc != na || _fmemcmp(r, r2, TN * sizeof(LzRock)) || rcc.nreap != rca.nreap || rcc.nclose != rca.nclose
        || _fmemcmp(lists, lists + 128, rcc.nreap * sizeof(short))
        || _fmemcmp(lists + 64, lists + 192, rcc.nclose * sizeof(short))) {
        rv_logf(s_n, na, rca.nreap, rca.nclose, nc, rcc.nreap, rcc.nclose);
        bad = -1;
    }
    for (i = 0; i < nc && i < na && !bad; i++)
        if (_fmemcmp(&oc[i], &oa[i], sizeof(LzRockOut))) {
            rv_logf(s_one, i, oc[i].x, oc[i].y, oc[i].d, oc[i].kind, oc[i].frame, oc[i].z, oa[i].x, oa[i].y, oa[i].d,
                    oa[i].kind, oa[i].frame, oa[i].z);
            bad = -1;
        }
    if (!bad)
        rv_logf(s_ok, na, TN, rca.nreap, rca.nclose);
out:
    g_seed = seed;
    if (r) _ffree(r);
    if (oc) _ffree(oc);
    if (lists) _ffree(lists);
    return bad ? bad : TN;
#undef TN
}
#endif /* RV_NO_TWINS */
#endif /* RV_TESTS */

#ifdef RV_TESTS
/* a script's way into a field (flight.c's test key #): the player put 3,550 km outside the next field of the mission
 * (the first, then the one after it at each press), on the line through its middle along the nose, facing it, at
 * 200 kps, taking no damage (the rocks still hit, and are logged): a straight pass, as tests/wc1 flies the old game's */
void rocks_test_to_field(void)
{
    static const char __far s_to[] = "rocks: test: the player %ld km before field %d (radius %ld km), facing its middle";
    static int __far next;
    int i = -1, k;
    long back;
    for (k = 0; k < g_nf; k++)
        if (!g_f[(next + k) % g_nf].cluster) {
            i = (next + k) % g_nf;
            break;
        }
    if (i < 0)
        return;
    next = i + 1;
    back = g_f[i].r_km + 3550;
    gPositionVector[0].x = g_f[i].x - gOrientationFrontVector[0].x * back;
    gPositionVector[0].y = g_f[i].y - gOrientationFrontVector[0].y * back;
    gPositionVector[0].z = g_f[i].z - gOrientationFrontVector[0].z * back;
    setSpeed[0] = speedLo[0] = 200;
    playerCanBeDamaged = 0;
    rv_logf(s_to, back, i, g_f[i].r_km);
}
#endif
