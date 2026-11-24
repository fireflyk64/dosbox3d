/*
 *  RESEARCH AID, not part of the mode (included by wcnet_tie640.cpp, inside its namespace; on only with
 *  WCNET_T640_DUMP=1 and WCNET_T640_FAST=0).  The question it answers: can the whole 640x378 scene be drawn in ONE
 *  pass of the game's scene section, with native code in the place of the game's edge tracing and scanline pass?
 *
 *  The key script's `!t640dump=<name>`: on the next frame, after the four quarters, the scene section is run a fifth
 *  time with the window 640x378, the projection's centre doubled and the scale doubled (no quarters).  The game's
 *  transform, clip, lighting and object code runs as it is; the five places where the scene is handed to the
 *  renderer's lower half are left at their entry, what they were given written to <name>_prims.txt:
 *      1CCA:53F3  trace2_drawface         F  a mesh face: its edges' screen points, lights, flags
 *      1CCA:4FF3  trace2_drawscreencoords P  a flat polygon (its screen points' ring)
 *      1CCA:2BB0  drawln2_tracelineedges  L  a thick line (two points, thickness, lights)
 *      1CCA:A02A  rotscale_rotatescaleimage S  a sprite or backdrop
 *      1CCA:85E9  xtrans2_drawxtrans      O, T  the object records and the flat polygons' tables, then left
 *  and the four quarters' picture of the same frame (before the stars) as <name>_scene.pgm (palette indices,
 *  640x400) with <name>_pal.bin.  $S/tools/raster640.py draws the primitives and compares.
 */

bool g_dumpOn = false, g_dumpAsked = false;
int g_dumpChecked = -1;
std::string g_dumpName;
FILE *g_dumpFile = NULL;
Bit16u g_dumpKeep[4];

bool dump_enabled() {
    if (g_dumpChecked < 0) {
        const char *e = getenv("WCNET_T640_DUMP"), *f = getenv("WCNET_T640_FAST");
        g_dumpChecked = e && e[0] == '1' && f && f[0] == '0' && !f[1] ? 1 : 0;
    }
    return g_dumpChecked == 1;
}

void dump_places() {
    if (!dump_enabled()) return;
    static const Bit16u ips[] = { 0x53F3, 0x4FF3, 0x2BB0, 0xA02A, 0x85E9 };
    for (size_t i = 0; i < sizeof(ips) / sizeof(ips[0]); i++) place_at(kRendPara, ips[i]);
}

inline Bit32s r32s(Bit16u off) { return (Bit32s)mem_readd(dsa(off)); }
inline Bit8u r8(Bit16u off) { return mem_readb(dsa(off)); }

void far_return() {
    const Bit16u ip = CPU_Pop16();
    const Bit16u seg = CPU_Pop16();
    SegSet16(cs, seg);
    reg_eip = ip;
}

// the fifth pass about to begin (the CPU goes to the section's start)
void dump_begin() {
    const char *dir = getenv("WCNET_DUMP_DIR");
    const std::string base = std::string(dir && dir[0] ? dir : ".") + "/" + g_dumpName;
    g_dumpFile = fopen((base + "_prims.txt").c_str(), "w");
    g_dumpOn = g_dumpFile != NULL;
    if (g_dumpFile) setvbuf(g_dumpFile, NULL, _IOLBF, 0);
    g_dumpAsked = false;
    for (int i = 0; i < 64; i++) for (int k = 0; k < 3; k++) mem_writeb(dsa((Bit16u)(kStatics + i * 0x12 + k)), g_statics[i * 3 + k]);
    mem_writew(dsa(kFullUpdate), 1);
}

// 0090:0B36 of the fifth pass: xtrans2_initxtrans has come back (its tables cleared for the game's own 189 rows)
void dump_centre() {
    if (!g_dumpOn) return;
    const int w = r16(kWide), h = r16(kDeep);
    const int cx = (Bit16s)g_keepHalfWide, half = (Bit16s)r16(kHalfDeep), cy = half + (Bit32s)g_keepYOffset;
    g_dumpKeep[0] = (Bit16u)w; g_dumpKeep[1] = r16(0x48C7); g_dumpKeep[2] = (Bit16u)h; g_dumpKeep[3] = r16(0x48D3);
    mem_writew(dsa(kWide), (Bit16u)(2 * w));
    mem_writew(dsa(0x48C7), (Bit16u)(2 * w - 1));
    mem_writew(dsa(kDeep), (Bit16u)(2 * h));
    mem_writew(dsa(0x48D3), (Bit16u)(2 * h - 1));
    mem_writew(dsa(kHalfWide), (Bit16u)(2 * cx));
    mem_writed(dsa(kYOffset), (Bit32u)(2 * cy - half));
    fprintf(g_dumpFile, "W %d %d centre %d %d back %02X\n", 2 * w, 2 * h, 2 * cx, 2 * cy, g_key);
}

// 0090:1069 of the fifth pass
void dump_end() {
    if (!g_dumpOn) return;
    mem_writew(dsa(kWide), g_dumpKeep[0]);
    mem_writew(dsa(0x48C7), g_dumpKeep[1]);
    mem_writew(dsa(kDeep), g_dumpKeep[2]);
    mem_writew(dsa(0x48D3), g_dumpKeep[3]);
    fclose(g_dumpFile);
    g_dumpFile = NULL;
    g_dumpOn = false;
    const char *dir = getenv("WCNET_DUMP_DIR");
    const std::string base = std::string(dir && dir[0] ? dir : ".") + "/" + g_dumpName;
    FILE *f = fopen((base + "_scene.pgm").c_str(), "wb");
    if (f) { fprintf(f, "P5\n%d %d\n255\n", (int)HW, (int)HH); fwrite(g_draw, 1, HW * HH, f); fclose(f); }
    f = fopen((base + "_pal.bin").c_str(), "wb");
    if (f) {
        for (int i = 0; i < 256; i++) {
            const RGBEntry &c = vga.dac.rgb[i];
            fputc((c.red << 2) | (c.red >> 4), f); fputc((c.green << 2) | (c.green >> 4), f); fputc((c.blue << 2) | (c.blue >> 4), f);
        }
        fclose(f);
    }
    // the shading table (16 colours a material, in the renderer's code segment at 07A0) and the per-material offsets
    f = fopen((base + "_shade.bin").c_str(), "wb");
    if (f) {
        const PhysPt cs0 = (PhysPt)(Bit16u)(g_load + kRendPara) << 4;
        for (int i = 0; i < 0x300; i++) fputc(mem_readb(cs0 + 0x7A0 + (PhysPt)i), f);
        fclose(f);
    }
    wclog(1, "TIE demo 640: one-pass primitives in %s_prims.txt, the quarters' picture in %s_scene.pgm", base.c_str(), base.c_str());
}

void dump_point(Bit16u at) {
    if (!at) { fprintf(g_dumpFile, " - -"); return; }
    fprintf(g_dumpFile, " %d %d", (int)r32s(at), (int)r32s((Bit16u)(at + 4)));
}

// one of the five places, in the fifth pass (CS = the renderer's)
bool dump_hook(Bit32u ip) {
    if (!g_dumpOn) return false;
    FILE *o = g_dumpFile;
    switch (ip) {
    case 0x53F3: {      // trace2_drawface: CX = the face's vertices; its record at [2488]:[2486] (vertex, edge pairs)
        const int n = reg_cx & 0x3F;
        const PhysPt face = ((PhysPt)r16(0x2488) << 4) + r16(0x2486);
        const PhysPt tabs = (PhysPt)r16(kTablesSeg) << 4;
        const Bit16u rec = r16(0x247E), num = r16(0x2474);
        const int colour = mem_readb(tabs + (Bit16u)(rec + 2 * num + 0x65)), index = mem_readb(tabs + (Bit16u)(rec + 2 * num + 0x66));
        fprintf(o, "F obj %u face %u n %d gouraud %u colour %d index %d parent %04X\n", r16(0x2464), num, n, r8(0x243A), colour, index, r16(0x2480));
        for (int i = 0; i < n; i++) {
            const int v1 = mem_readb(face + (PhysPt)(2 * i)), e = mem_readb(face + (PhysPt)(2 * i + 1)), v2 = mem_readb(face + (PhysPt)(2 * i + 2));
            const Bit8u flags = r8((Bit16u)(0x2FF0 + e));
            const Bit16u p1 = r16((Bit16u)(0x2AF0 + 2 * e)), p2 = r16((Bit16u)(0x2BF0 + 2 * e));
            const Bit16u s1 = r16((Bit16u)(0x213A + 2 * v1)), s2 = r16((Bit16u)(0x213A + 2 * v2));
            int l1 = (Bit16s)r16((Bit16u)(0x223A + 2 * v1)), l2 = (Bit16s)r16((Bit16u)(0x223A + 2 * v2));
            const bool swapped = s1 == p2;       // (the edge's first point is this face's second vertex)
            if (swapped) std::swap(l1, l2);
            fprintf(o, " e %d v %d %d flags %02X sw %d pts", e, v1, v2, flags, swapped ? 1 : 0);
            const bool none = flags == 0x80 || flags == 0xA0;
            dump_point(none ? 0 : p1);
            dump_point(none ? 0 : p2);
            fprintf(o, " lt %d %d seen %d %d\n", l1, l2, s1 ? 1 : 0, s2 ? 1 : 0);
            if (!none && !(flags & 4) && !(flags & 0x20)) mem_writeb(dsa((Bit16u)(0x2FF0 + e)), flags | 0x20);
        }
        far_return();
        return true;
    }
    case 0x4FF3: {      // trace2_drawscreencoords: [2472] points in the ring [2452]..[2454), 8 bytes each
        const Bit16u lo = r16(0x2452), hi = r16(0x2454), maxY = r16(0x245C), maxX = r16(0x2458), minY = r16(0x245A), minX = r16(0x2456);
        bool draw = true;
        if ((Bit16s)r16((Bit16u)(maxY + 6)) < 0 || (Bit16s)r16((Bit16u)(maxX + 2)) < 0) draw = false;
        if (draw && (Bit16s)r16((Bit16u)(minY + 6)) >= 0 && (r16((Bit16u)(minY + 6)) != 0 || r16((Bit16u)(minY + 4)) >= r16(kDeep))) draw = false;
        if (draw && (Bit16s)r16((Bit16u)(minX + 2)) >= 0 && (r16((Bit16u)(minX + 2)) != 0 || r16(minX) >= r16(kWide))) draw = false;
        const Bit16u idx = r16(0x26E3);
        if (draw && idx == 0x77) draw = false;
        if (draw) {
            mem_writew(dsa(0x26E3), (Bit16u)(idx + 1));
            mem_writeb(dsa((Bit16u)(0x410C + idx)), r8(0x2447));
            mem_writeb(dsa((Bit16u)(0x428C + idx)), r8(0x2464));
            mem_writew(dsa((Bit16u)(0x418C + 2 * idx)), r16(0x2480));
            mem_writew(dsa((Bit16u)(0x400C + 2 * idx)), 0x8001);
            mem_writeb(dsa(0x274D), (Bit8u)(0x80 + idx));
            mem_writeb(dsa(0x274C), r8(0x2484));
            fprintf(o, "P flat %u colour %d obj %u parent %04X n %u pts", idx, r8(0x2447), r16(0x2464), r16(0x2480), r16(0x2472));
            for (Bit16u at = lo; at < hi && at >= lo; at = (Bit16u)(at + 8)) dump_point(at);
            fprintf(o, "\n");
        }
        reg_ax = draw ? 1 : 0;
        far_return();
        return true;
    }
    case 0x2BB0: {      // drawln2_tracelineedges: DI -> two points; [2730] thickness, [2738], [273A] the ends' lights
        fprintf(o, "L id %04X obj %u face %u thick %u lt %d %d flats %u pts", r16(0x274C), r16(0x2464), r16(0x2474), r16(0x2730),
                (int)(Bit16s)r16(0x2738), (int)(Bit16s)r16(0x273A), r16(0x26E3));
        dump_point(reg_di);
        dump_point((Bit16u)(reg_di + 8));
        dump_point(r16(0x2732));
        fprintf(o, " si %04X\n", reg_si);
        far_return();
        return true;
    }
    case 0xA02A: {      // rotscale_rotatescaleimage(x, y, scale, far image)
        const PhysPt sp = ((PhysPt)SegValue(ss) << 4) + (Bit16u)(reg_esp & 0xFFFF);
        const Bit16u off = mem_readw(sp + 10), seg = mem_readw(sp + 12);
        fprintf(o, "S x %d y %d scale %04X image %04X:%04X flats %u id %04X head", (int)(Bit16s)mem_readw(sp + 4), (int)(Bit16s)mem_readw(sp + 6),
                mem_readw(sp + 8), seg, off, r16(0x26E3), r16(0x274C));
        for (int i = 0; i < 12; i++) fprintf(o, " %02X", mem_readb(((PhysPt)seg << 4) + (Bit16u)(off + i)));
        fprintf(o, "\n");
        far_return();
        return true;
    }
    case 0x85E9: {      // xtrans2_drawxtrans: the object records (in the tables' segment, by DS:233A) and the flats
        const PhysPt tabs = (PhysPt)r16(kTablesSeg) << 4;
        const int objs = r16(0x2464), flats = r16(0x26E3);
        for (int i = 1; i < objs && i < 0x80; i++) {
            const Bit16u rec = r16((Bit16u)(0x233A + 2 * i));
            fprintf(o, "O %d at %04X box", i, rec);
            for (int k = 0; k < 6; k++) fprintf(o, " %d", (int)(Bit16s)mem_readw(tabs + (Bit16u)(rec + 2 * k)));
            const int faces = mem_readw(tabs + (Bit16u)(rec + 0x10));
            fprintf(o, " parent %04X id %04X faces %d covers", mem_readw(tabs + (Bit16u)(rec + 0xC)), mem_readw(tabs + (Bit16u)(rec + 0xE)), faces);
            for (int k = 0; k < 5; k++) fprintf(o, " %u", mem_readb(tabs + (Bit16u)(rec + 0x12 + k)));
            fprintf(o, " facebytes");
            for (int k = 1; k <= faces && k < 128; k++) fprintf(o, " %u:%u", mem_readb(tabs + (Bit16u)(rec + 0x65 + 2 * k)), mem_readb(tabs + (Bit16u)(rec + 0x66 + 2 * k)));
            fprintf(o, "\n");
        }
        for (int i = 1; i < flats && i < 0x78; i++)
            fprintf(o, "T %d colour %d comp %u parent %04X x %d y %d z %d\n", i, r8((Bit16u)(0x410C + i)), r8((Bit16u)(0x428C + i)), r16((Bit16u)(0x418C + 2 * i)),
                    (int)(Bit16s)r16((Bit16u)(0x3E0C + 2 * i)), (int)(Bit16s)r16((Bit16u)(0x3F0C + 2 * i)), (int)(Bit16s)r16((Bit16u)(0x400C + 2 * i)));
        fprintf(o, "X objs %d flats %d newobjectdef %04X\n", objs, flats, r16(0x2468));
        far_return();
        return true;
    }
    }
    return false;
}
