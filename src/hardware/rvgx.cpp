/*
 *  Copyright (C) 2002-2015  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/* rvgx.cpp: RVGX, a sprite accelerator that exists only in this emulator
 * (retrovega/docs/gx.md is the card's manual; retrovega/src/gx.c its driver).
 *
 * A program finds it with INT 2Fh AX=E700h: every other DOS and DOSBox leaves
 * AL at 0, so a game asks once and otherwise never knows it was here.  Told a
 * name for one of its slots, it fetches that image bundle (a ship's SHIPnn.RV)
 * by itself, in the background, into its own memory: the browser's fetch on the
 * page, a directory natively.  Then one interrupt draws a view of a slot's bundle
 * turned and scaled straight into the program's frame buffer, in no emulated
 * time, or says in the parameter block, with a negative number, why it did not:
 * the program then draws the picture its own way.
 *
 * Natively it is installed only when RVGX_DIR names the directory it "fetches"
 * from (RVGX_DELAY_MS pretends the transfer takes that long); in the browser
 * always, unless RVGX=0.  So nothing else that runs in this emulator changes. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <string>
#include <unordered_map>

#include "dosbox.h"
#include "mem.h"
#include "paging.h"
#include "regs.h"
#include "pic.h"
#include "dos_system.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "rvgx_kernel.h"

enum {
	RVGX_AH = 0xE7,
	FN_INSTALLED = 0x00,  /* BX 'RV', CX 'GX' -> AL FFh, BX 'gx', CH.CL the version */
	FN_ATTACH = 0x01,     /* ES:BX the parameter block -> AX 0 */
	FN_NAME = 0x10,       /* the block's bundle: a slot; its name: what it is to hold, replacing what it holds */
	FN_STATUS = 0x11,     /* the block's stat_* filled in */
	FN_DRAW = 0x20,       /* the block's bundle .. ext_rows; its result says how it went */
	FN_DEBRIS = 0x30,     /* (1.2) a wreck's debris: the block's fb .. ext_rows and its debris part; result 0 */
	FN_PLANET = 0x40,     /* (1.3) a planet to make (terrain.c), from its seed, stamps and palette */
	FN_VIEW = 0x41,       /* the frame's eye: the picture made */
	FN_GROUND = 0x42,     /* a band of it: the block's fb .. ext_rows */
	FN_COLUMN = 0x43,     /* the ground under a point */
	FN_SEEN = 0x44,       /* is a point hidden by the ground: result 1 seen, 0 hidden */
	FN_MAKE = 0x45,       /* make more of it: result how far it has got, 100 made */
	FN_DROP = 0x46,       /* no planet */
	FN_PALETTE = 0x47,    /* the palette its picture is coloured from: a far pointer to 768 bytes at 64 */
	FN_PLACE = 0x48,      /* a nav point put over the ground */
	FN_RAISE = 0x49,      /* a ship raised over the ground */
	RVGX_SIG_IN_BX = 0x5256, RVGX_SIG_IN_CX = 0x4758, RVGX_SIG_OUT_BX = 0x7867,
	RVGX_VERSION = 0x0103
};

/* the result word */
enum { RVGX_E_INDEX = -1, RVGX_E_NOT_FOUND = -2, RVGX_E_NOT_YET = -3, RVGX_E_VIEW = -4, RVGX_E_TARGET = -5 };

/* the parameter block's fields, by byte offset: the game's GxBlock (src/gx.c) */
enum {
	B_RESULT = 0, B_BUNDLE = 2, B_VIEW = 4, B_FLAGS = 6, B_X = 8, B_Y = 10, B_SCALE = 12, B_ROLL = 14,
	B_FB_OFF = 16, B_FB_SEG = 18, B_STRIDE = 20, B_FB_Y0 = 22, B_Y0 = 24, B_Y1 = 26, B_XA = 28, B_XB = 30,
	B_EXT_OFF = 32, B_EXT_SEG = 34, B_EXT_Y0 = 36, B_EXT_ROWS = 38,
	B_NAME_OFF = 40, B_NAME_SEG = 42,
	B_STAT_DRAWS = 48, B_STAT_PX = 52, B_STAT_US = 56, B_STAT_READY = 60, B_STAT_WAITING = 62,
	/* (1.2) the debris part, the game's RvDebris (retrovega/src/debris.h) packed (1.1's had six colours a ramp) */
	B_D_SEED = 64, B_D_AGE = 66, B_D_LIFE = 68, B_D_COUNT = 70, B_D_FOCAL = 72, B_D_CX = 74, B_D_CY = 76,
	B_D_X = 78, B_D_Y = 82, B_D_Z = 86, B_D_SPREAD = 90, B_D_PIECE = 94, B_D_M = 98, B_D_METAL = 116, B_D_HOT = 124,
	/* (1.3) the planet's calls' own second half (the game's GxBlock union) */
	B_V_EYE = 64, B_V_M = 76, B_V_FOCAL = 94, B_V_CX = 96, B_V_CY = 98, B_V_W = 100, B_V_H = 102,
	B_C_AT = 64, B_C_GROUND = 76, B_C_LO = 80, B_C_HI = 84, B_C_R = 88,
	B_S_EYE = 64, B_S_PT = 76,
	B_M_MS = 64,
	B_SIZE = 128
};

#define RVGX_SLOTS 256
#define RVGX_NAME_MAX 64
#define RVGX_DENSE_BYTES (48u * 1024u * 1024u)   /* the views kept expanded, all bundles together */

struct Bundle {
	uint32_t id;
	std::string name;
	int state;                  /* 0 ready, or the result a draw of it gets: RVGX_E_INDEX, the slot is empty */
	uint8_t *data;              /* the file, malloc'd */
	RvgxFile file;
	double due;                 /* (native) the emulated ms the pretended transfer ends */
};

struct Dense {
	RvgxView view;
	uint32_t used;
};

static Bundle g_slots[RVGX_SLOTS];
static std::unordered_map<uint64_t, Dense> g_dense;
static uint64_t g_dense_bytes;
static uint32_t g_clock, g_next_id = 1;
static uint16_t g_block_seg, g_block_off;
static bool g_attached;
static uint32_t g_draws, g_px;
static uint64_t g_ns;
#define RVGX_EXT_ROWS 1024
static int16_t g_ext[2 * RVGX_EXT_ROWS];
#ifndef __EMSCRIPTEN__
static const char *g_dir;
static double g_delay_ms;
#endif

static inline uint16_t rw(unsigned at) { return real_readw(g_block_seg, g_block_off + at); }
static inline int16_t rs(unsigned at) { return (int16_t)rw(at); }
static inline void put_result(int v) { real_writew(g_block_seg, g_block_off + B_RESULT, (uint16_t)(int16_t)v); }

/* the file for a bundle has come (data malloc'd, this takes it) or size < 0: it is not to be had */
static void bundle_arrived(Bundle &b, uint8_t *data, int size)
{
	if (size < 0 || !data) {
		b.state = RVGX_E_NOT_FOUND;
		free(data);
		LOG_MSG("RVGX: %s is not to be had", b.name.c_str());
		return;
	}
	b.data = data;
	if (!rvgx_parse(data, (uint32_t)size, &b.file)) {
		b.state = RVGX_E_NOT_FOUND;
		LOG_MSG("RVGX: %s is not a bundle (%d bytes)", b.name.c_str(), size);
		return;
	}
	b.state = 0;
	LOG_MSG("RVGX: %s in, %d KB, %u views", b.name.c_str(), size / 1024, (unsigned)b.file.nviews);
}

#ifdef __EMSCRIPTEN__
/* the page's answer to Module.rvgxWant(id, name) */
extern "C" EMSCRIPTEN_KEEPALIVE void wc_web_rvgx_loaded(int id, uint8_t *data, int size)
{
	for (Bundle &b : g_slots)
		if (b.id == (uint32_t)id) {
			if (b.state == RVGX_E_NOT_YET)
				bundle_arrived(b, data, size);
			else
				free(data);
			return;
		}
	free(data);  /* (no longer wanted: the slot has been given another name since) */
}
#else
static void native_arrive(Bundle &b)
{
	std::string path = std::string(g_dir) + "/" + b.name;
	FILE *f = fopen(path.c_str(), "rb");
	uint8_t *data = 0;
	long size = -1;
	if (f) {
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);
		data = (uint8_t *)malloc(size > 0 ? size : 1);
		if (!data || size <= 0 || fread(data, 1, size, f) != (size_t)size)
			size = -1;
		fclose(f);
	}
	bundle_arrived(b, data, (int)size);
}
#endif

/* a bundle not yet here asked for: nothing waits */
static void bundle_request(Bundle &b)
{
	b.state = RVGX_E_NOT_YET;
#ifdef __EMSCRIPTEN__
	EM_ASM({ if (Module.rvgxWant) Module.rvgxWant($0, UTF8ToString($1)); else Module._wc_web_rvgx_loaded($0, 0, -1); },
	       (int)b.id, b.name.c_str());
#else
	b.due = PIC_FullIndex() + g_delay_ms;
#endif
}

static inline void bundle_service(Bundle &b)
{
#ifndef __EMSCRIPTEN__
	if (b.state == RVGX_E_NOT_YET && PIC_FullIndex() >= b.due)
		native_arrive(b);
#else
	(void)b;
#endif
}

static void forget_dense(uint32_t id)
{
	for (auto it = g_dense.begin(); it != g_dense.end();) {
		if ((uint32_t)(it->first >> 32) == id) {
			g_dense_bytes -= (uint64_t)it->second.view.w * it->second.view.h;
			free(it->second.view.px);
			it = g_dense.erase(it);
		} else
			++it;
	}
}

/* --- the views kept expanded, the least recently used going out while too many are kept --- */

static Dense *dense_of(Bundle &b, uint32_t view, bool mirror)
{
	const uint64_t key = ((uint64_t)b.id << 32) | (view << 1) | (mirror ? 1 : 0);
	auto found = g_dense.find(key);
	if (found != g_dense.end()) {
		found->second.used = g_clock;
		return &found->second;
	}
	Dense d;
	if (!rvgx_decode(b.file, view, mirror, &d.view))
		return 0;
	d.used = g_clock;
	g_dense_bytes += (uint64_t)d.view.w * d.view.h;
	while (g_dense_bytes > RVGX_DENSE_BYTES && !g_dense.empty()) {
		auto oldest = g_dense.begin();
		for (auto it = g_dense.begin(); it != g_dense.end(); ++it)
			if ((uint32_t)(g_clock - it->second.used) > (uint32_t)(g_clock - oldest->second.used))
				oldest = it;
		g_dense_bytes -= (uint64_t)oldest->second.view.w * oldest->second.view.h;
		free(oldest->second.view.px);
		g_dense.erase(oldest);
	}
	return &(g_dense[key] = d);
}

/* --- the functions --- */

/* a slot given a name, replacing what it held.  The name it already holds changes nothing, so a program may name its
 * slots in a loop as often as it likes; one that could not be had is tried again.  An empty name or none: empty */
static void slot_name(void)
{
	const unsigned slot = rw(B_BUNDLE);
	const uint16_t seg = rw(B_NAME_SEG), off = rw(B_NAME_OFF);
	std::string name;
	bool clean = true;
	if (slot >= RVGX_SLOTS) {
		put_result(RVGX_E_INDEX);
		return;
	}
	if (seg || off) {
		for (unsigned i = 0;; i++) {
			const char c = (char)real_readb(seg, (uint16_t)(off + i));
			if (!c)
				break;
			if (i >= RVGX_NAME_MAX) {
				clean = false;
				break;
			}
			clean = clean && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-');
			name += c;
		}
	}
	Bundle &b = g_slots[slot];
	put_result(0);
	if (b.name == name && b.state != RVGX_E_NOT_FOUND)
		return;
	if (b.id)
		forget_dense(b.id);
	free(b.data);
	b.data = 0;
	memset(&b.file, 0, sizeof b.file);
	b.name = name;
	b.id = 0;
	b.due = 0;
	b.state = RVGX_E_INDEX;
	if (name.empty())
		return;
	b.id = g_next_id++;
	if (clean)
		bundle_request(b);
	else
		b.state = RVGX_E_NOT_FOUND;
}

static void status_fill(void)
{
	unsigned ready = 0, waiting = 0;
	for (Bundle &b : g_slots) {
		bundle_service(b);
		ready += b.state == 0;
		waiting += b.state == RVGX_E_NOT_YET;
	}
	real_writed(g_block_seg, g_block_off + B_STAT_DRAWS, g_draws);
	real_writed(g_block_seg, g_block_off + B_STAT_PX, g_px);
	real_writed(g_block_seg, g_block_off + B_STAT_US, (uint32_t)(g_ns / 1000));
	real_writew(g_block_seg, g_block_off + B_STAT_READY, (uint16_t)ready);
	real_writew(g_block_seg, g_block_off + B_STAT_WAITING, (uint16_t)waiting);
	put_result(0);
}

/* every page of [a, a + len) ordinary RAM, so the host's pointer to it is good for all of it */
static bool plain_ram(PhysPt a, uint32_t len)
{
	PageHandler *ram = MEM_GetPageHandler(0);
	for (Bitu p = a >> 12; p <= (a + len - 1) >> 12; p++)
		if (p >= MEM_TotalPages() || MEM_GetPageHandler(p) != ram)
			return false;
	return true;
}

static int draw(void)
{
	const auto t0 = std::chrono::steady_clock::now();
	const unsigned index = rw(B_BUNDLE), view = rw(B_VIEW), flags = rw(B_FLAGS);
	const int scale = rs(B_SCALE), stride = rw(B_STRIDE), fb_y0 = rs(B_FB_Y0);
	int y0 = rs(B_Y0), y1 = rs(B_Y1), xa = rs(B_XA), xb = rs(B_XB);
	if (index >= RVGX_SLOTS)
		return RVGX_E_INDEX;
	Bundle &b = g_slots[index];
	bundle_service(b);
	if (b.state)
		return b.state;
	if (view >= b.file.nviews || scale < 1)
		return RVGX_E_VIEW;
	g_clock++;
	Dense *d = dense_of(b, view, flags & 1);
	if (!d)
		return RVGX_E_VIEW;
	if (y0 < fb_y0) y0 = fb_y0;
	if (xa < 0) xa = 0;
	if (xb > stride) xb = stride;
	if (y0 >= y1 || xa >= xb)
		return 0;
	const PhysPt fb = PhysMake(rw(B_FB_SEG), rw(B_FB_OFF));
	if (stride < 1 || y1 - fb_y0 > RVGX_VIEW_MAX || !plain_ram(fb + (y0 - fb_y0) * stride, (uint32_t)(y1 - y0) * stride))
		return RVGX_E_TARGET;
	/* the extents out of the program's table (only the rows drawn), once, into the kernel's plain array */
	const uint16_t ext_seg = rw(B_EXT_SEG), ext_off = rw(B_EXT_OFF);
	int ext_y0 = rs(B_EXT_Y0), ext_rows = rs(B_EXT_ROWS);
	if (ext_seg || ext_off) {
		if (y1 - y0 > RVGX_EXT_ROWS)
			return RVGX_E_TARGET;
		for (int oy = y0; oy < y1; oy++) {
			const int i = oy - ext_y0;
			const bool in = i >= 0 && i < ext_rows;
			g_ext[2 * (oy - y0)] = in ? (int16_t)real_readw(ext_seg, (uint16_t)(ext_off + 4 * i)) : 0;
			g_ext[2 * (oy - y0) + 1] = in ? (int16_t)real_readw(ext_seg, (uint16_t)(ext_off + 4 * i + 2)) : 0;
		}
		ext_y0 = y0;
		ext_rows = y1 - y0;
	}
	const uint32_t px = rvgx_draw(d->view, rs(B_X), rs(B_Y), scale, rs(B_ROLL), GetMemBase() + fb, fb_y0, stride, y0, y1, xa, xb,
	                              ext_seg || ext_off ? g_ext : 0, ext_y0, ext_rows);
	g_draws++;
	g_px += px;
	g_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
	return 0;
}

/* a wreck's debris (E730h): the block's debris part into the kernel's RvDebris, the target checked as draw()'s */
static int debris(void)
{
	const auto t0 = std::chrono::steady_clock::now();
	const int stride = rw(B_STRIDE), fb_y0 = rs(B_FB_Y0);
	int y0 = rs(B_Y0), y1 = rs(B_Y1), xa = rs(B_XA), xb = rs(B_XB);
	RvDebris d;
	d.seed = rw(B_D_SEED);
	d.age = rw(B_D_AGE);
	d.life = rw(B_D_LIFE);
	d.count = rw(B_D_COUNT);
	d.focal = rs(B_D_FOCAL);
	d.cx = rs(B_D_CX);
	d.cy = rs(B_D_CY);
	d.x = (int32_t)real_readd(g_block_seg, g_block_off + B_D_X);
	d.y = (int32_t)real_readd(g_block_seg, g_block_off + B_D_Y);
	d.z = (int32_t)real_readd(g_block_seg, g_block_off + B_D_Z);
	d.spread = (int32_t)real_readd(g_block_seg, g_block_off + B_D_SPREAD);
	d.piece = (int32_t)real_readd(g_block_seg, g_block_off + B_D_PIECE);
	for (int i = 0; i < 9; i++)
		d.m[i] = rs(B_D_M + 2 * i);
	for (int i = 0; i < DEBRIS_METAL; i++)
		d.metal[i] = real_readb(g_block_seg, g_block_off + B_D_METAL + i);
	for (int i = 0; i < DEBRIS_HOT; i++)
		d.hot[i] = real_readb(g_block_seg, g_block_off + B_D_HOT + i);
	if (y0 < fb_y0) y0 = fb_y0;
	if (xa < 0) xa = 0;
	if (xb > stride) xb = stride;
	if (y0 >= y1 || xa >= xb)
		return 0;
	const PhysPt fb = PhysMake(rw(B_FB_SEG), rw(B_FB_OFF));
	if (stride < 1 || y1 - fb_y0 > RVGX_VIEW_MAX || !plain_ram(fb + (y0 - fb_y0) * stride, (uint32_t)(y1 - y0) * stride))
		return RVGX_E_TARGET;
	const uint16_t ext_seg = rw(B_EXT_SEG), ext_off = rw(B_EXT_OFF);
	int ext_y0 = rs(B_EXT_Y0), ext_rows = rs(B_EXT_ROWS);
	if (ext_seg || ext_off) {
		if (y1 - y0 > RVGX_EXT_ROWS)
			return RVGX_E_TARGET;
		for (int oy = y0; oy < y1; oy++) {
			const int i = oy - ext_y0;
			const bool in = i >= 0 && i < ext_rows;
			g_ext[2 * (oy - y0)] = in ? (int16_t)real_readw(ext_seg, (uint16_t)(ext_off + 4 * i)) : 0;
			g_ext[2 * (oy - y0) + 1] = in ? (int16_t)real_readw(ext_seg, (uint16_t)(ext_off + 4 * i + 2)) : 0;
		}
		ext_y0 = y0;
		ext_rows = y1 - y0;
	}
	g_px += rv_debris_draw(&d, GetMemBase() + fb, fb_y0, stride, y0, y1, xa, xb, ext_seg || ext_off ? g_ext : 0, ext_y0,
	                       ext_rows);
	g_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
	return 0;
}

/* --- (1.3) a planet: terrain.c's, one at a time.  Made a piece at a time between frames (FN_MAKE, and some of every
 * view asked for before it is made) so the game never stalls for it; the queries a game's logic makes (FN_COLUMN) make
 * the rest at once, since they must have an answer --- */

static TrPlanet *g_planet;
static bool g_planet_viewed;            /* a view made since the planet was: FN_GROUND has a picture */
static double g_view_ms;                /* (the views' cost, smoothed, and the detail it sets: tr_detail) */
static float g_step = 2.0f;

static int32_t rd(unsigned at) { return (int32_t)real_readd(g_block_seg, g_block_off + at); }

/* make more of the planet for up to ms of the host's (0: until it is made): how far it has got, 100 made */
static int planet_make(unsigned ms)
{
	if (!g_planet)
		return RVGX_E_INDEX;
	const auto t0 = std::chrono::steady_clock::now();
	while (!tr_ready(g_planet)) {
		if (tr_build(g_planet) < 0) {
			tr_free(g_planet);
			g_planet = 0;
			LOG_MSG("RVGX: no memory for the planet");
			return RVGX_E_NOT_FOUND;
		}
		if (ms && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms))
			break;
	}
	g_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
	if (tr_ready(g_planet) && ms != 1)
		LOG_MSG("RVGX: the planet is made");
	return tr_ready(g_planet) ? 100 : tr_progress(g_planet);
}

static int32_t g_centre[3];             /* the planet's centre, km, the game's world (PL_PLACE, PL_RAISE) */

/* the game's mission record's planet tail (terrain.h TrPlanetRec, at the far pointer at 64) and its nav points (TrNavRec,
 * at 68, as many as the word at 76 says), and its palette (72) */
static int planet_load(void)
{
	TrPlanetRec pr;
	TrNavRec navs[16];
	TrMissionKm m;
	TrPlanetDesc d;
	TrStamp st[TR_STAMPS_MAX];
	uint8_t pal[768];
	const uint16_t r_off = rw(64), r_seg = rw(66), n_off = rw(68), n_seg = rw(70), p_off = rw(72), p_seg = rw(74);
	unsigned nnav = rw(76);
	if (nnav > 16)
		return RVGX_E_VIEW;
	for (unsigned i = 0; i < sizeof pr; i++)
		((uint8_t *)&pr)[i] = real_readb(r_seg, (uint16_t)(r_off + i));
	for (unsigned i = 0; i < nnav * sizeof(TrNavRec); i++)
		((uint8_t *)navs)[i] = real_readb(n_seg, (uint16_t)(n_off + i));
	if (pr.radius < 1000 || pr.n > 4)
		return RVGX_E_VIEW;             /* (anything else is a damaged record) */
	for (unsigned i = 0; i < 768; i++)
		pal[i] = (uint8_t)(real_readb(p_seg, (uint16_t)(p_off + i)) & 63);
	tr_from_records(&pr, navs, (int)nnav, &m);
	tr_from_km(&m, &d, st);
	memcpy(g_centre, m.centre, sizeof g_centre);
	if (g_planet)
		tr_free(g_planet);
	g_planet = tr_begin(&d, st, pal);
	g_planet_viewed = false;
	if (!g_planet)
		return RVGX_E_NOT_FOUND;
	LOG_MSG("RVGX: a planet to make: seed %u, radius %d km, %u ground shapes", (unsigned)m.seed, (int)m.radius, (unsigned)m.n);
	return 0;
}

/* a nav point (world km at 64) put as high over the ground under it as it is over the planet's radius (at 76) */
static int planet_place(void)
{
	int32_t v[3];
	TrColumn col;
	if (!g_planet)
		return RVGX_E_INDEX;
	if (!tr_ready(g_planet) && planet_make(0) != 100)
		return RVGX_E_NOT_FOUND;
	for (int k = 0; k < 3; k++)
		v[k] = (rd(64 + 4 * k) - g_centre[k]) * 256;
	tr_column(g_planet, v[0], v[1], v[2], &col);
	tr_place(g_planet, v, col.r - tr_radius(g_planet));
	for (int k = 0; k < 3; k++)
		real_writed(g_block_seg, g_block_off + 76 + 4 * k, (uint32_t)(v[k] / 256 + g_centre[k]));
	return 0;
}

/* a ship (world positions at 64) raised to the km at 76 over the ground if it is under that; back at 76 */
static int planet_raise(void)
{
	int32_t v[3], km = rd(76);
	TrColumn col;
	if (!g_planet)
		return RVGX_E_INDEX;
	if (!tr_ready(g_planet) && planet_make(0) != 100)
		return RVGX_E_NOT_FOUND;
	for (int k = 0; k < 3; k++)
		v[k] = rd(64 + 4 * k) - g_centre[k] * 256;
	tr_column(g_planet, v[0], v[1], v[2], &col);
	if (col.r - col.ground < km * 256)
		tr_place(g_planet, v, km * 256);
	for (int k = 0; k < 3; k++)
		real_writed(g_block_seg, g_block_off + 76 + 4 * k, (uint32_t)(v[k] + g_centre[k] * 256));
	return 0;
}

/* the frame's eye; a view asked for before the planet is made makes some more of it (8 ms) instead */
static int planet_view(void)
{
	if (!g_planet)
		return RVGX_E_INDEX;
	if (!tr_ready(g_planet)) {
		planet_make(8);
		g_planet_viewed = false;
		return RVGX_E_NOT_YET;
	}
	TrCam c;
	double m[9];
	for (int k = 0; k < 3; k++)
		c.pos[k] = rd(B_V_EYE + 4 * k);
	for (int k = 0; k < 9; k++)
		m[k] = rs(B_V_M + 2 * k) / 16384.0;
	/* the game's frame is 256 = 1 and not quite square: made so, front first */
	double f[3] = { m[6], m[7], m[8] }, u[3] = { m[3], m[4], m[5] }, r[3];
	double l = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	if (l < 1e-6)
		return RVGX_E_VIEW;
	for (int k = 0; k < 3; k++)
		f[k] /= l;
	double d = u[0] * f[0] + u[1] * f[1] + u[2] * f[2];
	for (int k = 0; k < 3; k++)
		u[k] -= d * f[k];
	l = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
	if (l < 1e-6)
		return RVGX_E_VIEW;
	for (int k = 0; k < 3; k++)
		u[k] /= l;
	r[0] = u[1] * f[2] - u[2] * f[1];
	r[1] = u[2] * f[0] - u[0] * f[2];
	r[2] = u[0] * f[1] - u[1] * f[0];
	if (r[0] * m[0] + r[1] * m[1] + r[2] * m[2] < 0)   /* (whichever way the game's axes turn) */
		for (int k = 0; k < 3; k++)
			r[k] = -r[k];
	for (int k = 0; k < 3; k++) {
		c.fwd[k] = (float)f[k];
		c.up[k] = (float)u[k];
		c.right[k] = (float)r[k];
	}
	c.focal = rs(B_V_FOCAL);
	c.cx = rs(B_V_CX);
	c.cy = rs(B_V_CY);
	c.w = rs(B_V_W);
	c.h = rs(B_V_H);
	if (c.focal < 16 || c.w < 1 || c.h < 1 || c.w > 1024 || c.h > 1024)
		return RVGX_E_VIEW;
	const auto t0 = std::chrono::steady_clock::now();
	tr_detail(g_planet, g_step);
	tr_view(g_planet, &c);
	g_planet_viewed = true;
	const double ms = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count() / 1000.0;
	g_ns += (uint64_t)(ms * 1e6);
	/* the detail by what the views cost: the march's step longer while they run over 6 ms, shorter again under 3 */
	g_view_ms = g_view_ms * 0.8 + ms * 0.2;
	if (g_view_ms > 6 && g_step < 4)
		g_step *= 1.08f;
	else if (g_view_ms < 3 && g_step > 2)
		g_step /= 1.05f;
	if (g_step < 2)
		g_step = 2;
	return 0;
}

static int planet_ground(void)
{
	if (!g_planet || !g_planet_viewed)
		return RVGX_E_NOT_YET;
	const auto t0 = std::chrono::steady_clock::now();
	const int stride = rw(B_STRIDE), fb_y0 = rs(B_FB_Y0);
	int y0 = rs(B_Y0), y1 = rs(B_Y1), xa = rs(B_XA), xb = rs(B_XB);
	if (y0 < fb_y0) y0 = fb_y0;
	if (xa < 0) xa = 0;
	if (xb > stride) xb = stride;
	if (y0 >= y1 || xa >= xb)
		return 0;
	const PhysPt fb = PhysMake(rw(B_FB_SEG), rw(B_FB_OFF));
	if (stride < 1 || y1 - fb_y0 > RVGX_VIEW_MAX || !plain_ram(fb + (y0 - fb_y0) * stride, (uint32_t)(y1 - y0) * stride))
		return RVGX_E_TARGET;
	const uint16_t ext_seg = rw(B_EXT_SEG), ext_off = rw(B_EXT_OFF);
	int ext_y0 = rs(B_EXT_Y0), ext_rows = rs(B_EXT_ROWS);
	if (ext_seg || ext_off) {
		if (y1 - y0 > RVGX_EXT_ROWS)
			return RVGX_E_TARGET;
		for (int oy = y0; oy < y1; oy++) {
			const int i = oy - ext_y0;
			const bool in = i >= 0 && i < ext_rows;
			g_ext[2 * (oy - y0)] = in ? (int16_t)real_readw(ext_seg, (uint16_t)(ext_off + 4 * i)) : 0;
			g_ext[2 * (oy - y0) + 1] = in ? (int16_t)real_readw(ext_seg, (uint16_t)(ext_off + 4 * i + 2)) : 0;
		}
		ext_y0 = y0;
		ext_rows = y1 - y0;
	}
	g_px += tr_blit(g_planet, GetMemBase() + fb, fb_y0, stride, y0, y1, xa, xb, ext_seg || ext_off ? g_ext : 0, ext_y0, ext_rows);
	g_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
	return 0;
}

static int planet_column(void)
{
	TrColumn col;
	if (!g_planet)
		return RVGX_E_INDEX;
	if (!tr_ready(g_planet) && planet_make(0) != 100)   /* (a query must have its answer: the rest made now) */
		return RVGX_E_NOT_FOUND;
	tr_column(g_planet, rd(B_C_AT), rd(B_C_AT + 4), rd(B_C_AT + 8), &col);
	real_writed(g_block_seg, g_block_off + B_C_GROUND, (uint32_t)col.ground);
	real_writed(g_block_seg, g_block_off + B_C_LO, (uint32_t)col.slab_lo);
	real_writed(g_block_seg, g_block_off + B_C_HI, (uint32_t)col.slab_hi);
	real_writed(g_block_seg, g_block_off + B_C_R, (uint32_t)col.r);
	return 0;
}

static int planet_seen(void)
{
	double e[3], q[3];
	if (!g_planet)
		return RVGX_E_INDEX;
	if (!tr_ready(g_planet))
		return RVGX_E_NOT_YET;
	for (int k = 0; k < 3; k++) {
		e[k] = rd(B_S_EYE + 4 * k);
		q[k] = rd(B_S_PT + 4 * k);
	}
	return tr_seen(g_planet, e, q);
}

static int planet_palette(void)
{
	uint8_t pal[768];
	const uint16_t off = rw(64), seg = rw(66);
	if (!g_planet)
		return RVGX_E_INDEX;
	for (unsigned i = 0; i < 768; i++)
		pal[i] = (uint8_t)(real_readb(seg, (uint16_t)(off + i)) & 63);
	tr_palette(g_planet, pal);
	return 0;
}

static bool RVGX_Multiplex(void)
{
	if (reg_ah != RVGX_AH)
		return false;
	switch (reg_al) {
	case FN_INSTALLED:
		if (reg_bx != RVGX_SIG_IN_BX || reg_cx != RVGX_SIG_IN_CX)
			return false;
		reg_al = 0xFF;
		reg_bx = RVGX_SIG_OUT_BX;
		reg_cx = RVGX_VERSION;
		return true;
	case FN_ATTACH:
		g_block_seg = SegValue(es);
		g_block_off = reg_bx;
		g_attached = true;
		reg_ax = 0;
		return true;
	case FN_NAME:
		if (g_attached)
			slot_name();
		return true;
	case FN_STATUS:
		if (g_attached)
			status_fill();
		return true;
	case FN_DRAW:
		if (g_attached)
			put_result(draw());
		return true;
	case FN_DEBRIS:
		if (g_attached)
			put_result(debris());
		return true;
	case FN_PLANET:
		if (g_attached)
			put_result(planet_load());
		return true;
	case FN_VIEW:
		if (g_attached)
			put_result(planet_view());
		return true;
	case FN_GROUND:
		if (g_attached)
			put_result(planet_ground());
		return true;
	case FN_COLUMN:
		if (g_attached)
			put_result(planet_column());
		return true;
	case FN_SEEN:
		if (g_attached)
			put_result(planet_seen());
		return true;
	case FN_MAKE:
		if (g_attached)
			put_result(planet_make(rw(B_M_MS)));
		return true;
	case FN_PLACE:
		if (g_attached)
			put_result(planet_place());
		return true;
	case FN_RAISE:
		if (g_attached)
			put_result(planet_raise());
		return true;
	case FN_PALETTE:
		if (g_attached)
			put_result(planet_palette());
		return true;
	case FN_DROP:
		if (g_planet)
			tr_free(g_planet);
		g_planet = 0;
		g_planet_viewed = false;
		if (g_attached)
			put_result(0);
		return true;
	}
	return false;
}

void RVGX_Init(void)
{
	const char *off = getenv("RVGX");
	if (off && !strcmp(off, "0"))
		return;
	for (Bundle &b : g_slots) {
		b.id = 0;
		b.state = RVGX_E_INDEX;
		b.data = 0;
		b.due = 0;
		memset(&b.file, 0, sizeof b.file);
	}
#ifndef __EMSCRIPTEN__
	g_dir = getenv("RVGX_DIR");
	if (!g_dir)
		return;
	if (getenv("RVGX_DELAY_MS"))
		g_delay_ms = atof(getenv("RVGX_DELAY_MS"));
	LOG_MSG("RVGX: the sprite accelerator is installed, its bundles from %s", g_dir);
#else
	LOG_MSG("RVGX: the sprite accelerator is installed, its bundles from the site");
#endif
	rv_debris_prepare();  /* (the debris' tumble made now, not at a flight's first kill) */
	DOS_AddMultiplexHandler(RVGX_Multiplex);
}
