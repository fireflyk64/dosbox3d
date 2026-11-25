/*
 *  Privateer's crew (wcnet_priv.h, docs/privateer.md section 30): one ship flown by its pilot with the room's other
 *  players in its turrets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <map>
#include "wcnet_priv.h"
#include "wcnet_log.h"
#include "pic.h"
#include "dos_inc.h"
#include "dos_system.h"
#include "regs.h"
#include "mem.h"
#include <vector>

namespace wc {

namespace {

struct OpenFile { std::string name; bool created; unsigned read, written; };
std::map<Bit16u, OpenFile> g_files;         // the open files we follow, by handle entry
int g_fileLog = -1;                         // PRIVFILELOG=1: every file of the game's opened, read, written (a rig's aid)

bool file_log() {
    if (g_fileLog < 0) {
        const char *e = getenv("PRIVFILELOG");
        g_fileLog = e && atoi(e) ? 1 : 0;
    }
    return g_fileLog == 1;
}

// The ship a gunner's flight is flown in: the pilot's, not the gunner's own.  The game writes its ship to PLAYTYPE.IFF
// (an IFF FORM REAL: guns, launchers, shields, armour, the hold and the credits) at every launch, the flight reads the
// file back at once, and the landing reads it once more for the base (measured, PRIVFILELOG=1: created and written in
// pieces; opened and read whole; in flight nothing; at the landing opened and read whole).  The hull it is flown as is
// a byte of the game's data (ds:655E beside the credits at ds:655A: 0 Tarsus, 1 Orion, 2 Centurion, 3 Galaxy; another
// hull's file with the wrong byte is the game's error 6482).  So for a gunner's flight the pilot's file is written over
// the game's before the flight's read, with the pilot's hull byte, and the gunner's own file and byte are put back
// before the landing's read: its ship, hold and credits are its own again at the base.
enum { kHullByte = 0x655E, kCredits = 0x655A };
enum Phase { PHASE_BASE, PHASE_WRITTEN, PHASE_LAUNCH, PHASE_FLIGHT };
struct Ship {
    std::vector<Bit8u> file;
    int hull;
    Ship() : hull(-1) {}
};
Ship g_pilot;                               // the pilot's ship as last told (or a rig's: PRIVCREWTYPE=<file>:<hull>)
Ship g_own;                                 // ours, as the game wrote it at this launch
Phase g_phase = PHASE_BASE;
Bit8u g_drive = 2;                          // the drive PLAYTYPE.IFF is on (the game's own directory)
bool g_ownFlight;                           // this flight is flown in our own ship (no pilot's ship was known at its launch)

bool read_whole(Bit8u drive, const char *fullname, std::vector<Bit8u> &out) {
    DOS_File *f = NULL;
    char name[DOS_PATHLENGTH];
    strcpy(name, fullname);
    out.clear();
    if (drive >= DOS_DRIVES || !Drives[drive] || !Drives[drive]->FileOpen(&f, name, OPEN_READ) || !f) return false;
    f->AddRef();                // (a local file is closed only by its last holder)
    Bit8u buf[4096];
    Bit16u n = sizeof(buf);
    if (f->Read(buf, &n)) out.assign(buf, buf + n);
    f->Close();
    delete f;
    return !out.empty();
}

bool write_whole(Bit8u drive, const char *fullname, const std::vector<Bit8u> &in) {
    DOS_File *f = NULL;
    char name[DOS_PATHLENGTH];
    strcpy(name, fullname);
    if (in.empty() || drive >= DOS_DRIVES || !Drives[drive] || !Drives[drive]->FileCreate(&f, name, 0) || !f) return false;
    f->AddRef();
    Bit16u n = (Bit16u)in.size();
    const bool ok = f->Write(const_cast<Bit8u *>(&in[0]), &n) && n == in.size();
    f->Close();
    delete f;
    return ok;
}

void rig_ship_file() {
    static bool looked;
    if (looked) return;
    looked = true;
    const char *e = getenv("PRIVCREWTYPE");
    if (!e || !*e) return;
    std::string path = e;
    const size_t c = path.rfind(':');
    if (c == std::string::npos) return;
    g_pilot.hull = atoi(path.c_str() + c + 1);
    path.resize(c);
    if (FILE *f = fopen(path.c_str(), "rb")) {
        Bit8u buf[4096];
        const size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        g_pilot.file.assign(buf, buf + n);
        wclog(1, "Privateer: crew: a rig's pilot ship %s, %u bytes, hull %d", path.c_str(), (unsigned)n, g_pilot.hull);
    }
}

bool gunner() {
    rig_ship_file();
    return priv_crew_gunner() || getenv("PRIVCREWTYPE");
}

}  // namespace

void priv_file_opening(const char *fullname, Bit8u drive, Bit8u flags) {
    if (!priv_found() || (flags & 3) != 0) return;
    if (file_log() && strcasecmp(fullname, "PLAYTYPE.IFF") == 0) {
        // (a rig's: who opens it -- the far return addresses up the frames, as paragraphs from the program's load)
        char line[400];
        int at = snprintf(line, sizeof(line), "Privateer: file %s opening from", fullname);
        Bit16u bp = reg_bp;
        const PhysPt stack = SegPhys(ss);
        for (int i = 0; i < 14 && at < (int)sizeof(line) - 12; i++) {
            const Bit16u ip = mem_readw(stack + (Bit16u)(bp + 2)), cs = mem_readw(stack + (Bit16u)(bp + 4));
            at += snprintf(line + at, sizeof(line) - at, " %04X:%04X", (Bit16u)(cs - priv_load_seg()), ip);
            const Bit16u next = mem_readw(stack + bp);
            if (next <= bp) break;
            bp = next;
        }
        wclog(1, "%s", line);
    }
    if (strcasecmp(fullname, "PLAYSCOR.IFF") == 0 && g_phase == PHASE_WRITTEN) {
        // (a launch reads the pilot's score between writing the ship and reading it; a new pilot's registration and a
        // load write the ship and read it back with nothing between: no flight follows those)
        g_phase = PHASE_LAUNCH;
        return;
    }
    if (strcasecmp(fullname, "PLAYTYPE.IFF") != 0) return;
    g_drive = drive;
    if (g_phase == PHASE_WRITTEN) {
        g_phase = PHASE_BASE;
        if (!g_ownFlight) priv_w8(kHullByte, (Bit8u)g_own.hull);       // (no launch after all)
        g_ownFlight = true;
        return;
    }
    if (g_phase == PHASE_LAUNCH) {
        // the launch: the game has just written its own ship, the flight is about to read it
        g_phase = PHASE_FLIGHT;
        if (g_ownFlight) return;
        g_ownFlight = true;
        if (!read_whole(drive, fullname, g_own.file)) {
            priv_w8(kHullByte, (Bit8u)g_own.hull);
            wclog(1, "Privateer: crew: our own ship's file could not be read: flown as it is");
            return;
        }
        if (const char *dir = getenv("PRIVCREWDUMP")) {             // (a rig's: our own ship's file at each launch)
            static int n;
            char path[512];
            snprintf(path, sizeof(path), "%s/own-%d.iff", dir, ++n);
            if (FILE *o = fopen(path, "wb")) { fwrite(&g_own.file[0], 1, g_own.file.size(), o); fclose(o); }
        }
        if (!write_whole(drive, fullname, g_pilot.file)) {
            priv_w8(kHullByte, (Bit8u)g_own.hull);
            wclog(1, "Privateer: crew: the pilot's ship could not be written over %s: flown in our own", fullname);
            write_whole(drive, fullname, g_own.file);
            return;
        }
        g_ownFlight = false;
        wclog(1, "Privateer: crew: this flight is flown in the pilot's ship (hull %d, %u bytes; ours hull %d, %u bytes, kept)", g_pilot.hull,
              (unsigned)g_pilot.file.size(), g_own.hull, (unsigned)g_own.file.size());
        return;
    }
}

// The game builds the base's ship (ovr273:0000(base ship near, hull, the flight's records far), cdecl far, entered by
// its thunk stub273:0020): for a new pilot and a loaded game from PLAYTYPE.IFF (records 0: ovr269:4B05 reads the
// file), at a landing from the ship just flown (records: ovr269:49F0 takes the guns, the shields and armour, the hold
// and the credits from the flight's objects; it opens the file only to see that it is one).  A gunner's flight was
// flown in the pilot's ship: at its landing the call is turned into the file's kind -- our own ship's file written
// back over PLAYTYPE.IFF, the hull argument and the hull byte our own, the records 0 -- and the base has our own
// ship, hold and credits as they were at the launch (hull byte late or the records kept: the game's error 7022).
void priv_crew_ship_load() {
    const PhysPt sp = SegPhys(ss) + reg_sp;
    const Bit16u self = mem_readw(sp + 4);
    const Bit8u hull = mem_readb(sp + 6);
    const Bit32u records = mem_readd(sp + 8);
    if (file_log()) {
        wclog(1, "Privateer: crew: the base's ship built: object ds:%04X, hull %u, flight records %04X:%04X, from %04X:%04X (%s)", self, hull,
              (Bit16u)(records >> 16), (Bit16u)records, (Bit16u)(mem_readw(sp + 2) - priv_load_seg()), mem_readw(sp), priv_in_flight() ? "in flight" : "not in flight");
    }
    if (g_phase != PHASE_FLIGHT) return;
    g_phase = PHASE_BASE;
    if (g_ownFlight || g_own.file.empty() || !records) return;
    g_ownFlight = true;
    const bool ok = write_whole(g_drive, "PLAYTYPE.IFF", g_own.file);
    if (!ok) {
        wclog(1, "Privateer: crew: landed: OUR OWN SHIP'S FILE COULD NOT BE WRITTEN: the base has the pilot's ship");
        return;
    }
    priv_w8(kHullByte, (Bit8u)g_own.hull);
    mem_writeb(sp + 6, (Bit8u)g_own.hull);
    mem_writed(sp + 8, 0);
    wclog(1, "Privateer: crew: landed: the base's ship is our own again (hull %d, %u bytes)", g_own.hull, (unsigned)g_own.file.size());
}

bool priv_crew_script_item(const std::string &item, double t) {
    if (item.compare(0, 8, "!privw8=") == 0 && priv_found()) {
        // !privw8=<offset>:<value> (hex): a byte of the game's data segment written (a rig's aid)
        unsigned off = 0, v = 0;
        if (sscanf(item.c_str() + 8, "%x:%x", &off, &v) == 2) {
            wclog(1, "key script %.1fs: ds:%04X %02X -> %02X", t, off, priv_r8((Bit16u)off), v);
            priv_w8((Bit16u)off, (Bit8u)v);
        }
        return true;
    }
    if (item.compare(0, 8, "!privr8=") == 0 && priv_found()) {
        // !privr8=<offset>:<count> (hex, decimal): bytes of the game's data segment
        unsigned off = 0, n = 1;
        sscanf(item.c_str() + 8, "%x:%u", &off, &n);
        char hex[3 * 64 + 1] = "";
        for (unsigned i = 0; i < n && i < 64; i++) snprintf(hex + 3 * i, 4, "%02X ", priv_r8((Bit16u)(off + i)));
        wclog(1, "key script %.1fs: ds:%04X: %s", t, off, hex);
        return true;
    }
    return false;
}

bool priv_crew_gunner() {
    return false;
}

void priv_crew_tick() {
}

void priv_file_opened(const char *fullname, Bit16u entry, bool created) {
    if (!priv_found()) return;
    OpenFile f;
    f.name = fullname;
    f.created = created;
    f.read = f.written = 0;
    g_files[entry] = f;
    if (created && strcasecmp(fullname, "PLAYTYPE.IFF") == 0 && !priv_in_flight() && g_phase == PHASE_BASE) {
        // the game begins to write its ship: a launch (or a registration, or a load: PHASE_WRITTEN's read tells).  The
        // hull byte is set now: the flight's set-up reads it before it reads the ship's file (set at that read, or at
        // the score's read before it, the game's error 6482)
        g_ownFlight = true;
        bool loading = false;
        for (std::map<Bit16u, OpenFile>::const_iterator k = g_files.begin(); k != g_files.end(); ++k) {
            const std::string &n = k->second.name;
            if (n.size() > 4 && (!strcasecmp(n.c_str() + n.size() - 4, ".SAV") || !strcasecmp(n.c_str() + n.size() - 4, ".PRS"))) loading = true;
        }
        if (!loading && gunner() && !g_pilot.file.empty() && g_pilot.hull >= 0 && g_pilot.hull <= 3) {
            g_own.hull = priv_r8(kHullByte);
            priv_w8(kHullByte, (Bit8u)g_pilot.hull);
            g_ownFlight = false;
        }
    }
}

void priv_file_read(Bit16u entry, Bit8u *data, Bit16u amount) {
    (void)data;
    std::map<Bit16u, OpenFile>::iterator it = g_files.find(entry);
    if (it == g_files.end() || !priv_found()) return;
    it->second.read += amount;
}

void priv_file_written(Bit16u entry, const Bit8u *data, Bit16u amount) {
    (void)data;
    std::map<Bit16u, OpenFile>::iterator it = g_files.find(entry);
    if (it == g_files.end() || !priv_found()) return;
    it->second.written += amount;
}

void priv_file_closed(Bit16u entry) {
    std::map<Bit16u, OpenFile>::iterator it = g_files.find(entry);
    if (it == g_files.end()) return;
    const OpenFile &f = it->second;
    if (file_log() && priv_found() && f.name.find(".TRE") == std::string::npos && f.name.find("PRCD.EXE") == std::string::npos) {
        wclog(1, "Privateer: file %s %s, %u read, %u written, closed (%s)", f.name.c_str(), f.created ? "created" : "opened", f.read, f.written,
              priv_in_flight() ? "in flight" : "not in flight");
    }
    if (priv_found() && f.created && f.written && strcasecmp(f.name.c_str(), "PLAYTYPE.IFF") == 0 && !priv_in_flight()) {
        // (a load writes it too, from the saved game, and reads it back: that read finds no pilot's ship wanted --
        // told from a launch's by the saved game still being open)
        bool loading = false;
        for (std::map<Bit16u, OpenFile>::const_iterator k = g_files.begin(); k != g_files.end(); ++k) {
            const std::string &n = k->second.name;
            if (n.size() > 4 && (!strcasecmp(n.c_str() + n.size() - 4, ".SAV") || !strcasecmp(n.c_str() + n.size() - 4, ".PRS"))) loading = true;
        }
        g_phase = loading ? PHASE_BASE : PHASE_WRITTEN;
    }
    g_files.erase(it);
}

}  // namespace wc
