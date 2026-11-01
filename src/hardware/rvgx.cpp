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
	RVGX_SIG_IN_BX = 0x5256, RVGX_SIG_IN_CX = 0x4758, RVGX_SIG_OUT_BX = 0x7867,
	RVGX_VERSION = 0x0100
};

/* the result word */
enum { RVGX_E_INDEX = -1, RVGX_E_NOT_FOUND = -2, RVGX_E_NOT_YET = -3, RVGX_E_VIEW = -4, RVGX_E_TARGET = -5 };

/* the parameter block's fields, by byte offset: the game's GxBlock (src/gx.c) */
enum {
	B_RESULT = 0, B_BUNDLE = 2, B_VIEW = 4, B_FLAGS = 6, B_X = 8, B_Y = 10, B_SCALE = 12, B_ROLL = 14,
	B_FB_OFF = 16, B_FB_SEG = 18, B_STRIDE = 20, B_FB_Y0 = 22, B_Y0 = 24, B_Y1 = 26, B_XA = 28, B_XB = 30,
	B_EXT_OFF = 32, B_EXT_SEG = 34, B_EXT_Y0 = 36, B_EXT_ROWS = 38,
	B_NAME_OFF = 40, B_NAME_SEG = 42,
	B_STAT_DRAWS = 48, B_STAT_PX = 52, B_STAT_US = 56, B_STAT_READY = 60, B_STAT_WAITING = 62
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
	DOS_AddMultiplexHandler(RVGX_Multiplex);
}
