/*
 *  Privateer: finding the program and watching it (wcnet_priv.h, docs/privateer.md).
 *
 *  PRCD.EXE (the CD-ROM edition's program, 937,760 bytes: a Borland C++ 1991 overlay program of 454 segments and 183
 *  overlays, DGROUP at paragraph 0x6E4D of the image) is loaded by DOS for PRIV.EXE, Origin's loader, so the layer
 *  knows where it is from the start.  scripts/wcexe.py reads it as it reads the Wing Commanders
 *  (`scripts/wcexe.py -e <game>/PRCD.EXE segs`).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <map>
#include "wcnet_priv.h"
#include "wcnet_log.h"
#include "wcnet_game.h"
#include "wc_net.h"
#include "mem.h"
#include "regs.h"
#include "pic.h"

namespace wc {

namespace {

enum {
    kDgroupPara = 0x6E4D,           // DGROUP, paragraphs from the image's start
    kSeg088 = 0x3632,               // seg088's paragraph: the controls (wcnet_priv_controls.cpp)
    kAfterKeyboard = 0x0BAD,        // seg088:0BAD, after the call of the keyboard's routine
    kSignatureOff = 0x0074,         // "Type PRIV to run Privateer, or RF to run Righteous Fire."
};
const char kSignature[] = "Type PRIV to run Privateer";

bool g_found;
bool g_rf;
Bit16u g_privLoadSeg, g_ds, g_psp;
double g_nextReportMs;

// The code trace (a test aid, "!privtrace=<name>:<seconds>"): every instruction's CS:IP counted for a while, written
// to WCNET_DUMP_DIR/<name>.trace as "cs:ip count" lines (cs - the load segment is scripts/wcexe.py's paragraph).
bool g_tracing;
double g_traceEndMs;
std::string g_traceName;
std::map<Bit32u, unsigned> g_trace;

void trace_step(double now) {
    g_trace[((Bit32u)SegValue(cs) << 16) | (reg_eip & 0xffff)]++;
    if (now < g_traceEndMs) {
        wc_net_countdown = 0;   // (the next instruction comes here too)
        return;
    }
    g_tracing = false;
    const char *dir = getenv("WCNET_DUMP_DIR");
    std::string path = std::string(dir && dir[0] ? dir : ".") + "/" + g_traceName + ".trace";
    FILE *f = fopen(path.c_str(), "w");
    if (f) {
        for (std::map<Bit32u, unsigned>::const_iterator it = g_trace.begin(); it != g_trace.end(); ++it) {
            fprintf(f, "%04x:%04x %u\n", it->first >> 16, it->first & 0xffff, it->second);
        }
        fclose(f);
    }
    wclog(1, "Privateer: code trace of %u places to %s (load segment %04X)", (unsigned)g_trace.size(), path.c_str(), g_privLoadSeg);
    g_trace.clear();
}

PhysPt ds_at(Bit16u off) { return ((PhysPt)g_ds << 4) + off; }

}  // namespace

bool priv_found() { return g_found; }
Bit16u priv_load_seg() { return g_found ? g_privLoadSeg : 0; }
Bit16u priv_ds() { return g_found ? g_ds : 0; }
bool priv_righteous_fire() { return g_found && g_rf; }

Bit8u priv_r8(Bit16u off) { return g_found ? mem_readb(ds_at(off)) : 0; }
Bit16u priv_r16(Bit16u off) { return g_found ? mem_readw(ds_at(off)) : 0; }
Bit32u priv_r32(Bit16u off) { return g_found ? mem_readd(ds_at(off)) : 0; }
void priv_w8(Bit16u off, Bit8u v) { if (g_found) mem_writeb(ds_at(off), v); }
void priv_w16(Bit16u off, Bit16u v) { if (g_found) mem_writew(ds_at(off), v); }

void priv_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp) {
    if (strcmp(name, "PRCD.EXE") != 0) {
        return;
    }
    const Bit16u ds = (Bit16u)(loadSeg + kDgroupPara);
    for (size_t i = 0; i < sizeof(kSignature) - 1; i++) {
        if (mem_readb(((PhysPt)ds << 4) + kSignatureOff + i) != (Bit8u)kSignature[i]) {
            wclog(0, "PRCD.EXE is not the Privateer build the hooks know (GOG's CD-ROM edition): not watched");
            return;
        }
    }
    // The command line (PSP:80): PRIV.EXE passes "r" for Righteous Fire.
    std::string tail;
    const Bit8u len = mem_readb(((PhysPt)psp << 4) + 0x80);
    for (Bit8u i = 0; i < len && i < 126; i++) {
        tail += (char)mem_readb(((PhysPt)psp << 4) + 0x81 + i);
    }
    g_rf = false;
    for (size_t i = 0; i < tail.size(); i++) {
        if ((tail[i] == 'r' || tail[i] == 'R') && (i == 0 || tail[i - 1] == ' ') && (i + 1 == tail.size() || tail[i + 1] == ' ')) {
            g_rf = true;
        }
    }
    g_found = true;
    g_privLoadSeg = loadSeg;
    g_ds = ds;
    g_psp = psp;
    g_nextReportMs = PIC_FullIndex() + 1000;
    hooks_game_changed();       // (the dispatcher's table: our places)
    wclog(1, "Privateer%s: PRCD.EXE loaded at %04X (PSP %04X, data segment %04X), command line \"%s\"",
          g_rf ? " (Righteous Fire)" : "", loadSeg, psp, ds, tail.c_str());
}

bool priv_program_ended(Bit16u psp) {
    if (!g_found || psp != g_psp) {
        return false;
    }
    wclog(1, "Privateer: PRCD.EXE ended");
    g_found = false;
    g_ds = g_privLoadSeg = g_psp = 0;
    hooks_game_changed();
    return true;
}

void priv_tick() {
    if (!g_found) {
        return;
    }
    const double now = PIC_FullIndex();
    if (g_tracing) {
        trace_step(now);
    }
    if (now < g_nextReportMs) {
        return;
    }
    g_nextReportMs = now + 1000;
    static unsigned lastFrames;
    const unsigned frames = priv_controls_frames();
    if (frames != lastFrames) {
        wclog(2, "Privateer: t=%.1f, %u keyboard frames, controls: %s", now / 1000.0, frames - lastFrames, priv_control_mode());
        lastFrames = frames;
    }
}

bool priv_check_hooks(Bit32u ip) {
    if (!g_found) {
        return false;
    }
    const Bit16u codeSeg = (Bit16u)SegValue(cs);
    if (ip == kAfterKeyboard && codeSeg == (Bit16u)(g_privLoadSeg + kSeg088)) {
        priv_controls_frame();
        return true;
    }
    return false;
}

void priv_watch_list(void (*watch)(Bit16u ip)) {
    watch(kAfterKeyboard);
}

bool priv_script_item(const std::string &item, double t) {
    if (item.compare(0, 8, "!privds=") == 0 && g_found) {
        const char *dir = getenv("WCNET_DUMP_DIR");
        std::string path = std::string(dir && dir[0] ? dir : ".") + "/" + item.substr(8) + ".ds";
        FILE *f = fopen(path.c_str(), "wb");
        if (f) {
            for (Bit32u off = 0; off < 0x10000; off++) {
                fputc(priv_r8((Bit16u)off), f);
            }
            fclose(f);
        }
        wclog(1, "key script %.1fs: Privateer's data segment (%04X) to %s%s", t, g_ds, path.c_str(), f ? "" : " failed");
        return true;
    }
    if (item.compare(0, 11, "!privtrace=") == 0 && g_found) {
        // !privtrace=<name>:<seconds>
        std::string spec = item.substr(11);
        size_t colon = spec.find(':');
        g_traceName = spec.substr(0, colon);
        g_traceEndMs = PIC_FullIndex() + 1000.0 * (colon == std::string::npos ? 1.0 : atof(spec.c_str() + colon + 1));
        g_tracing = true;
        g_trace.clear();
        wc_net_countdown = 0;
        wclog(1, "key script %.1fs: Privateer's code traced to %s.trace", t, g_traceName.c_str());
        return true;
    }
    return false;
}

bool priv_in_flight() {
    return false;
}

}  // namespace wc
