/* rvgx_kernel.h: what the sprite accelerator does with pixels (rvgx.cpp is the part that is a DOS device): a bundle's
 * views read and expanded, and one drawn turned and scaled into a frame buffer.  Plain C++ with nothing of DOSBox in
 * it, so it compiles for anything (the emulator natively, wasm) and retrovega/tests/host/gx_kernel_test.cpp can check
 * it against the game's own kernels, fuzz it under the sanitizers and time it. */
#ifndef RVGX_KERNEL_H
#define RVGX_KERNEL_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* the game's own placement and turn, so a ship lands exactly where the native build's does */
extern "C" {
#include "../../retrovega/src/lightfield.c"
#include "../../retrovega/src/debris.c"   /* (a wreck's debris: E730h) */
}

#define RVGX_VIEW_MAX 4096              /* a view's width or height: more is a damaged file */

struct RvgxFile {                       /* a bundle: SHIPnn.RV, WC's container, its table of views then what it is */
	const uint8_t *data;
	uint32_t size;
	uint32_t table;                 /* where its offsets are: table + 4 * view */
	uint32_t base, total;           /* what they are relative to, and how far the last view goes */
	uint32_t nviews;
};

struct RvgxView {                       /* a view expanded: w x h, 0 = clear, malloc'd */
	uint8_t *px;
	int32_t w, h, ox, oy;
};

static inline uint32_t rvgx_le32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline int16_t rvgx_le16s(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

static inline bool rvgx_parse(const uint8_t *data, uint32_t size, RvgxFile *f)
{
	uint32_t base, rec, first, n;
	if (size < 21)
		return false;
	base = rvgx_le32(data + 4) & 0xFFFFFF;
	rec = rvgx_le32(data + 8) & 0xFFFFFF;
	if (rec > size - 13 || memcmp(data + rec, "RVSH", 4) != 0 || base > size - 8)
		return false;
	first = rvgx_le32(data + base + 4) & 0xFFFFFF;   /* the first offset says how long the table is */
	if (first < 8 || first > size - base)
		return false;
	n = (first - 4) / 4;
	f->data = data;
	f->size = size;
	f->base = base;
	f->table = base + 4;
	f->total = rvgx_le32(data + base);
	f->nviews = data[rec + 5] | (data[rec + 6] << 8);
	if (f->nviews > n)
		f->nviews = n;
	return f->nviews > 0;
}

static inline void rvgx_span(uint8_t *px, int32_t w, int32_t h, int32_t row, int32_t x, const uint8_t *src, int32_t n, int fill)
{
	if (row < 0 || row >= h || x < 0 || n <= 0 || x > w - n)
		return;
	if (src)
		memcpy(px + row * w + x, src, n);
	else
		memset(px + row * w + x, fill, n);
}

/* the records of WC's sprite code into pixels, every step checked: the data came over a network, and a damaged file
 * must not write out of the picture.  False when the view makes no sense or there is no memory */
static inline bool rvgx_decode(const RvgxFile &f, uint32_t view, bool mirror, RvgxView *out)
{
	if (view >= f.nviews)
		return false;
	uint32_t start = (rvgx_le32(f.data + f.table + 4 * view) & 0xFFFFFF);
	uint32_t end = view + 1 < f.nviews ? (rvgx_le32(f.data + f.table + 4 * (view + 1)) & 0xFFFFFF) : f.total;
	const uint32_t room = f.size - f.base;
	if (end > room)
		end = room;
	if (start >= end || end - start < 8)
		return false;
	const uint8_t *buf = f.data + f.base + start;
	const uint32_t len = end - start;
	uint32_t off = 8;
	const int32_t x2 = rvgx_le16s(buf), x1 = rvgx_le16s(buf + 2), y1 = rvgx_le16s(buf + 4), y2 = rvgx_le16s(buf + 6);
	const int32_t w = x1 + x2 + 2, h = y1 + y2 + 2;
	if (w < 1 || h < 1 || w > RVGX_VIEW_MAX || h > RVGX_VIEW_MAX || x1 < -1 || y1 < -1 || x1 + 1 >= w || y1 + 1 >= h)
		return false;                   /* (the origin inside the picture, as every real view has it) */
	uint8_t *px = (uint8_t *)calloc((size_t)w * h, 1);
	if (!px)
		return false;
	while (off + 6 <= len) {
		const int32_t key = rvgx_le16s(buf + off), vx = rvgx_le16s(buf + off + 2), vy = rvgx_le16s(buf + off + 4);
		const int32_t row = vy + y1 + 1, x = vx + x1 + 1, cnt = key / 2;
		off += 6;
		if (key <= 0)
			break;
		if (key % 2 == 0) {
			if ((uint32_t)cnt > len - off)
				break;
			rvgx_span(px, w, h, row, x, buf + off, cnt, 0);
			off += cnt;
		} else {
			int32_t got = 0;
			while (got < cnt && off < len) {
				const int32_t k = buf[off++], n = k / 2;
				if (k % 2 == 0) {
					if ((uint32_t)n > len - off)
						break;
					rvgx_span(px, w, h, row, x + got, buf + off, n, 0);
					off += n;
				} else {
					if (off >= len)
						break;
					rvgx_span(px, w, h, row, x + got, 0, n, buf[off++]);
				}
				got += n;
			}
		}
	}
	int32_t ox = x1 + 1;
	if (mirror) {
		for (int32_t r = 0; r < h; r++) {
			uint8_t *line = px + r * w;
			for (int32_t i = 0, j = w - 1; i < j; i++, j--) {
				const uint8_t t = line[i];
				line[i] = line[j];
				line[j] = t;
			}
		}
		ox = w - 1 - ox;
	}
	out->px = px;
	out->w = w;
	out->h = h;
	out->ox = ox;
	out->oy = y1 + 1;
	return true;
}

/* the view turned clockwise by roll (1024 a turn) and scaled by scale / 256 about its origin, the origin at (x, y): rows
 * y0 .. y1-1 and columns xa .. xb-1 of the frame buffer, whose row fb_y0 is at fb and whose rows are stride apart;
 * with ext (two shorts a row, from row ext_y0, ext_rows of them) each row also within that extent.  By inverse mapping,
 * as the game's rv_rotate_row: the span says which columns stay in the picture, so there is no bounds test a pixel.
 * Returns the pixels looked at. */
static inline uint32_t rvgx_draw(const RvgxView &d, int x, int y, int scale, int roll, uint8_t *fb, int fb_y0, int stride, int y0,
                                 int y1, int xa, int xb, const int16_t *ext, int ext_y0, int ext_rows)
{
	LfRot r;
	uint32_t seen = 0;
	const int32_t w = d.w, h = d.h;
	const uint8_t *const dense = d.px;
	lf_rotate_setup((int16_t)w, (int16_t)h, (int16_t)d.ox, (int16_t)d.oy, (int16_t)roll, (int16_t)scale, &r);
	const int top = y - r.ocy, left = x - r.ocx;
	if (y0 < top) y0 = top;
	if (y1 > top + r.oh) y1 = top + r.oh;
	if (xa < left) xa = left;
	if (xb > left + r.ow) xb = left + r.ow;
	if (y0 >= y1 || xa >= xb)
		return 0;
	const int32_t du = r.du, dv = r.dv;
	for (int oy = y0; oy < y1; oy++) {
		int ca = xa, cb = xb;
		int16_t xs, xe;
		if (ext) {
			const int i = oy - ext_y0;
			if (i < 0 || i >= ext_rows)
				continue;
			if (ext[2 * i] > ca) ca = ext[2 * i];
			if (ext[2 * i + 1] < cb) cb = ext[2 * i + 1];
			if (ca >= cb)
				continue;
		}
		if (!lf_rotate_span(&r, (int16_t)w, (int16_t)h, (int16_t)(oy - top), (int16_t)(ca - left), (int16_t)(cb - left), &xs, &xe))
			continue;
		uint8_t *const row = fb + (oy - fb_y0) * stride + left;
		int32_t u = r.u00 + (int32_t)(oy - top) * r.dru + (int32_t)xs * du;
		int32_t v = r.v00 + (int32_t)(oy - top) * r.drv + (int32_t)xs * dv;
		for (int xx = xs; xx < xe; xx++, u += du, v += dv) {
			const uint8_t p = dense[(v >> 16) * w + (u >> 16)];
			if (p)
				row[xx] = p;
		}
		seen += (uint32_t)(xe - xs);
	}
	return seen;
}

#endif
