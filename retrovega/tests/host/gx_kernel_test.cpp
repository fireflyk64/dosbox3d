/* gx_kernel_test.cpp: the sprite accelerator's pixel code (src/hardware/rvgx_kernel.h) on its own: docs/gx.md.
 *
 *   gx_kernel_test views FILE.RV.. every view of real files decodes
 *   gx_kernel_test equal FILE.RV   every kernel path against the game's own (lightfield.c's lf_rotate_row), on real views
 *   gx_kernel_test fuzz FILE.RV N  N damaged copies decoded and drawn: for the sanitizers (gx_kernel_test.sh builds it so)
 *   gx_kernel_test bench FILE.RV   how many ships a frame the kernel can draw
 */
#include <chrono>
#include <stdio.h>
#include <vector>
#include "../../../src/hardware/rvgx_kernel.h"

static std::vector<uint8_t> load(const char *path)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END);
	b.resize(ftell(f));
	fseek(f, 0, SEEK_SET);
	if (fread(b.data(), 1, b.size(), f) != b.size()) exit(2);
	fclose(f);
	return b;
}

static uint32_t rnd_state = 12345;
static uint32_t rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

/* the game's way, the reference: lf_rotate_row with its own bounds test, a row at a time, over the whole clip */
static void reference(const RvgxView &d, int x, int y, int scale, int roll, uint8_t *fb, int stride, int y0, int y1, int xa, int xb,
                      const int16_t *ext, int ext_y0, int ext_rows)
{
	LfRot r;
	lf_rotate_setup((int16_t)d.w, (int16_t)d.h, (int16_t)d.ox, (int16_t)d.oy, (int16_t)roll, (int16_t)scale, &r);
	const int top = y - r.ocy, left = x - r.ocx;
	for (int oy = y0; oy < y1; oy++) {
		int ca = xa, cb = xb;
		if (oy < top || oy >= top + r.oh)
			continue;
		if (ext) {
			const int i = oy - ext_y0;
			if (i < 0 || i >= ext_rows)
				continue;
			if (ext[2 * i] > ca) ca = ext[2 * i];
			if (ext[2 * i + 1] < cb) cb = ext[2 * i + 1];
		}
		if (ca < left) ca = left;
		if (cb > left + r.ow) cb = left + r.ow;
		if (ca < cb)
			lf_rotate_row(d.px, (int16_t)d.w, (int16_t)d.h, &r, (int16_t)(oy - top), (int16_t)(ca - left), (int16_t)(cb - left), fb + oy * stride + left);
	}
}

static int equal(const char *path)
{
	enum { W = 640, H = 400 };
	std::vector<uint8_t> file = load(path), a(W * H), b(W * H);
	std::vector<int16_t> ext(2 * H);
	RvgxFile f;
	long draws = 0, pixels = 0;
	if (!rvgx_parse(file.data(), (uint32_t)file.size(), &f)) { printf("%s: not a bundle\n", path); return 1; }
	for (int i = 0; i < H; i++) {
		ext[2 * i] = (int16_t)(40 + i / 8);
		ext[2 * i + 1] = (int16_t)(600 - i / 8);
	}
	for (uint32_t k = 0; k < 4000; k++) {
		const uint32_t view = rnd() % f.nviews;
		const bool mirror = rnd() & 1;
		RvgxView d;
		if (!rvgx_decode(f, view, mirror, &d)) { printf("view %u will not decode\n", view); return 1; }
		const int x = -80 + (int)(rnd() % 800), y = -80 + (int)(rnd() % 560), roll = (k & 7) == 0 ? 0 : rnd() % 1024;
		const int scale = (k & 7) == 0 ? 256 : 8 + rnd() % 500;
		const int y0 = rnd() % H, y1 = y0 + 1 + rnd() % (H - y0), xa = rnd() % 200, xb = 400 + rnd() % 241;
		const bool with_ext = rnd() & 1;
		memset(a.data(), 77, a.size());
		memset(b.data(), 77, b.size());
		reference(d, x, y, scale, roll, a.data(), W, y0, y1, xa, xb, with_ext ? ext.data() : 0, 0, H);
		rvgx_draw(d, x, y, scale, roll, b.data() + y0 * W, y0, W, y0, y1, xa, xb, with_ext ? ext.data() : 0, 0, H);
		if (a != b) {
			for (int i = 0; i < W * H; i++)
				if (a[i] != b[i]) {
					printf("differs: view %u mirror %d at %d,%d scale %d roll %d: pixel %d,%d is %d, the game's %d\n", view, mirror, x, y, scale, roll,
					       i % W, i / W, b[i], a[i]);
					return 1;
				}
		}
		for (int i = 0; i < W * H; i++)
			pixels += b[i] != 77;
		draws++;
		free(d.px);
	}
	printf("GX EQUAL OK: %ld draws, %ld pixels the same as the game's kernel (%u views in the file)\n", draws, pixels, f.nviews);
	return 0;
}

/* every view of a real file decodes: the checks that turn damage away turn none of the art away */
static int every_view(const char *path)
{
	std::vector<uint8_t> file = load(path);
	RvgxFile f;
	if (!rvgx_parse(file.data(), (uint32_t)file.size(), &f)) { printf("%s: not a bundle\n", path); return 1; }
	for (uint32_t v = 0; v < f.nviews; v++) {
		RvgxView d;
		if (!rvgx_decode(f, v, false, &d)) { printf("%s: view %u turned away\n", path, v); return 1; }
		free(d.px);
	}
	return 0;
}

static int fuzz(const char *path, long n)
{
	enum { W = 640, H = 100 };
	const std::vector<uint8_t> file = load(path);
	std::vector<uint8_t> fb(W * H);
	long parsed = 0, decoded = 0;
	for (long k = 0; k < n; k++) {
		/* a piece of the file, or all of it, with bytes changed: the header and table most of the time (where a value
		 * can send a reader astray), the views the rest */
		std::vector<uint8_t> c(file);
		const uint32_t rec = rvgx_le32(file.data() + 8) & 0xFFFFFF;
		const int flips = 1 + rnd() % 400;
		for (int i = 0; i < flips; i++) {
			const uint32_t at = (k & 1) ? rnd() % c.size() : (k & 2) ? 4 + rnd() % 4000 : rec + rnd() % 13;
			if (at < c.size())
				c[at] = (uint8_t)rnd();
		}
		if ((k & 7) == 7)
			c.resize(rnd() % c.size());
		RvgxFile f;
		if (!rvgx_parse(c.data(), (uint32_t)c.size(), &f))
			continue;
		parsed++;
		for (int v = 0; v < 24; v++) {
			RvgxView d;
			const uint32_t view = rnd() % (f.nviews + 2);
			if (!rvgx_decode(f, view, rnd() & 1, &d))
				continue;
			decoded++;
			rvgx_draw(d, rnd() % W, rnd() % H, 8 + rnd() % 600, rnd() % 1024, fb.data(), 0, W, 0, H, 0, W, 0, 0, 0);
			free(d.px);
		}
	}
	printf("GX FUZZ OK: %ld damaged copies, %ld still parsed, %ld views decoded and drawn\n", n, parsed, decoded);
	return 0;
}

static int bench(const char *path)
{
	enum { W = 640, H = 400 };
	std::vector<uint8_t> file = load(path), fb(W * H);
	RvgxFile f;
	std::vector<RvgxView> views;
	if (!rvgx_parse(file.data(), (uint32_t)file.size(), &f)) return 1;
	for (uint32_t v = 0; v < f.nviews; v += f.nviews / 64 + 1) {
		RvgxView d;
		if (rvgx_decode(f, v, false, &d))
			views.push_back(d);
	}
	printf("%zu views, the first %dx%d\n", views.size(), views[0].w, views[0].h);
	static const int scales[] = { 64, 128, 256, 400 };
	for (int s = 0; s < 4; s++) {
		long px = 0, draws = 0;
		double ms = 0;
		for (int rep = 0; rep < 2000 && ms < 400; rep++) {
			const RvgxView &d = views[rep % views.size()];
			auto t0 = std::chrono::steady_clock::now();
			px += rvgx_draw(d, 320, 200, scales[s], rep * 37 % 1024, fb.data(), 0, W, 0, H, 0, W, 0, 0, 0);
			ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			draws++;
		}
		printf("scale %3d/256: %6.1f us a ship (%5ld pixels looked at), %6.0f Mpx/s: %5.0f ships in a 33 ms frame if they took it all, %5.0f in a tenth of it\n",
		       scales[s], ms * 1000 / draws, px / draws, px / ms / 1000, 33.3 * draws / ms, 3.33 * draws / ms);
	}
	double dec = 0; int n = 0;
	for (uint32_t v = 0; v < f.nviews; v += 3) {
		RvgxView d;
		auto t0 = std::chrono::steady_clock::now();
		if (rvgx_decode(f, v, false, &d)) { n++; free(d.px); }
		dec += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}
	printf("a view expanded (once, then kept): %.1f us\n", dec * 1000 / n);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "views")) {
		for (int i = 2; i < argc; i++)
			if (every_view(argv[i]))
				return 1;
		printf("GX VIEWS OK: every view of %d files\n", argc - 2);
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "equal")) return equal(argv[2]);
	if (argc >= 4 && !strcmp(argv[1], "fuzz")) return fuzz(argv[2], atol(argv[3]));
	if (argc >= 3 && !strcmp(argv[1], "bench")) return bench(argv[2]);
	fprintf(stderr, "usage: gx_kernel_test equal|bench FILE.RV | fuzz FILE.RV N\n");
	return 2;
}
