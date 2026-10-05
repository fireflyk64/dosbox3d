/*
 *  Per-instruction hook dispatcher.
 *
 *  wc_net_check_cpu_hooks() runs before every instruction of the normal CPU
 *  core.  A bitmap of interesting instruction pointers keeps the common case
 *  to a single table lookup; only when IP matches do we read the overlay
 *  stubs to confirm the segment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <string>
#include <vector>
#include "wc_net.h"
#include "wcnet_session.h"
#include "wcnet_events.h"
#include "wcnet_vm.h"
#include "wcnet_memory.h"
#include "wcnet_code.h"
#include "wcnet_game.h"
#include "wcnet_perf.h"
#include "wcnet_log.h"
#include "cpu.h"
#include "regs.h"
#include "keyboard.h"
#include "mouse.h"
#include "pic.h"
#include "vga.h"
#include <time.h>

extern std::string incoming_text;

bool wc_net_watch[0x10000];   // IPs with a hook, whatever the segment (tested in line by the CPU cores)
Bit32s wc_net_countdown = 0;  // instructions until the next periodic call

namespace wc {

#define g_watch wc_net_watch
static bool g_watchReady = false;
static int g_asyncCounter = 0;
static bool g_skipBarracks = false;

static void watch(Bit16u ip) { g_watch[ip] = true; }

// Between "mission about to start" and "mission ended", and once the first
// in-flight frame has run: what the test aids mean by "in space".  It does
// not depend on a network session or on stale slot data from the last mission.
static bool g_inMission = false;
static bool g_briefed = false;
static int g_rockOnto = -1;      // test aid: the slot a rock is to be put onto when the next frame begins   // WC2: the session was told of the mission in the barracks, before the briefing
static bool g_frameSeen = false;
static int g_flightCount = 0;  // missions that reached their first frame
static bool in_flight() { return g_inMission && g_frameSeen; }

// The mission most recently started on this machine: after a death the game
// falls back to its title loop, and a forced-mission game flies it again.
static bool g_haveLastMission = false;
static int g_lastMission = 0;
static int g_lastSeries = 1;

// A campaign begun in the middle of a series (MIS > 0): the series' earlier
// missions were not flown, and the game adds up a series' victory points
// to choose between its winning and its losing path (compute_victory,
// ovr161:027D).  Unflown they count nothing -- Gimle 2 and 3 flown and won
// after starting at Gimle 2 led to the losing path's Scimitars.  They are
// credited as won when the first mission flown is scored.
static int g_creditSeries = 0, g_creditMissions = 0;

static void credit_skipped_missions() {
    if (!g_creditMissions || !ds::known(ds::campaignTable) || !ds::known(ds::victoryPoints)) {
        return;
    }
    int series = rd8(ds::currentSeries), missions = g_creditMissions;
    g_creditMissions = 0;
    if (series != g_creditSeries || series < 1) {
        return;
    }
    Bit16u record = (Bit16u)(rd16(ds::campaignTable) + 0x5a * (series - 1));
    int points = 0;
    for (int m = 0; m < missions && m < 4; m++) {
        for (int objective = 0; objective < 16; objective++) {
            points += (Bit8s)rd8((Bit16u)(record + 0xa + 0x14 * m + 4 + objective));
        }
    }
    wr16(ds::victoryPoints, (Bit16u)(rd16(ds::victoryPoints) + points));
    wclog(1, "series %d began at its mission %d: the %d before it count as won, %d victory points (%d now, %d win the series)",
          series, missions, missions, points, (int)rd16(ds::victoryPoints), (int)rd16((Bit16u)(record + 3)));
}

// One line of campaign state for the logs: mission, status, the pilot byte of
// each ship slot and the eight "killed in mission" words.
static std::string mission_state_line() {
    char buf[64];
    snprintf(buf, sizeof(buf), "mission %d/%d status %d pilots", rd8(ds::currentMission), rd8(ds::currentSeries),
             rd16(ds::missionStatus));
    std::string line = buf;
    for (int slot = 0; slot <= kMaxShipSlot; slot++) {
        snprintf(buf, sizeof(buf), " %02x", rd8((Bit16u)(ds::shipStateByte + slot)));
        line += buf;
    }
    line += " kia";
    for (int i = 0; i < 8; i++) {
        snprintf(buf, sizeof(buf), " %d", rd16((Bit16u)(ds::statusPilots + 2 * i)));
        line += buf;
    }
    return line;
}

// The asteroid or mine field around this machine's own ship, for the logs:
// how many fields the nav point has, whether we are inside one, and the
// slots of its rocks or mines.
static std::string field_state_line() {
    char buf[64];
    snprintf(buf, sizeof(buf), "fields %d %s rocks", rd16(ds::fieldCount), rd16(ds::currentField) ? "inside" : "outside");
    std::string line = buf;
    for (int i = 0; i < 0x14; i++) {
        int slot = rd8((Bit16u)(ds::entitiesToDespawn + i));
        if (slot != 0xff && (entity_type(slot) == ET_ASTEROID || entity_type(slot) == ET_MINE)) {
            snprintf(buf, sizeof(buf), " %d", slot);
            line += buf;
        }
    }
    return line;
}

// Test aids for the key script: a ship of the mission that no human flies,
// our own ship turned to face a slot, and one of our field's rocks (or mines)
// put on a ship so the game's own collision code finds them touching.
static int first_npc_ship() {
    for (int slot = kMinShipSlot; slot <= kMaxShipSlot; slot++) {
        if (entity_type(slot) == ET_SHIP && !(g_session && g_session->is_remote_player_slot(slot))) {
            return slot;
        }
    }
    return -1;
}

static int first_rock() {
    for (int i = 0; i < 0x14; i++) {
        int slot = rd8((Bit16u)(ds::entitiesToDespawn + i));
        if (slot != 0xff && (entity_type(slot) == ET_ASTEROID || entity_type(slot) == ET_MINE)) {
            return slot;
        }
    }
    return -1;
}

static int script_slot(const char *arg) {
    return strcmp(arg, "npc") == 0 ? first_npc_ship() : strcmp(arg, "rock") == 0 ? first_rock() : atoi(arg);
}

static void face_slot(int target) {
    double r[3], u[3], f[3], nf[3], nr[3], nu[3];
    Vec3 own = read_vec(vec_slot(ds::gPositionVector, kPlayerSlot));
    Vec3 pos = read_vec(vec_slot(ds::gPositionVector, target));
    Vec3 vr = read_vec(vec_slot(ds::gOrientationRightVector, kPlayerSlot));
    Vec3 vu = read_vec(vec_slot(ds::gOrientationUpVector, kPlayerSlot));
    Vec3 vf = read_vec(vec_slot(ds::gOrientationFrontVector, kPlayerSlot));
    r[0] = vr.x; r[1] = vr.y; r[2] = vr.z;
    u[0] = vu.x; u[1] = vu.y; u[2] = vu.z;
    f[0] = vf.x; f[1] = vf.y; f[2] = vf.z;
    nf[0] = (double)pos.x - own.x; nf[1] = (double)pos.y - own.y; nf[2] = (double)pos.z - own.z;
    double len = sqrt(nf[0] * nf[0] + nf[1] * nf[1] + nf[2] * nf[2]);
    if (len < 1) {
        return;
    }
    for (int i = 0; i < 3; i++) nf[i] /= len;
    // right = up x front, up = front x right, keeping the game's handedness
    #define WC_CROSS(o, a, b) do { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; } while (0)
    double c[3];
    WC_CROSS(c, r, u);
    double hand = (c[0] * f[0] + c[1] * f[1] + c[2] * f[2]) < 0 ? -1 : 1;
    WC_CROSS(nr, u, nf);
    len = sqrt(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
    if (len < 1) {
        WC_CROSS(nr, f, nf);
        len = sqrt(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
        if (len < 1) {
            return;
        }
    }
    for (int i = 0; i < 3; i++) nr[i] = nr[i] / len * hand;
    WC_CROSS(nu, nf, nr);
    for (int i = 0; i < 3; i++) nu[i] *= hand;
    #undef WC_CROSS
    Vec3 o;
    o.x = (Bit32s)lround(nr[0] * 256); o.y = (Bit32s)lround(nr[1] * 256); o.z = (Bit32s)lround(nr[2] * 256);
    write_vec(vec_slot(ds::gOrientationRightVector, kPlayerSlot), o);
    o.x = (Bit32s)lround(nu[0] * 256); o.y = (Bit32s)lround(nu[1] * 256); o.z = (Bit32s)lround(nu[2] * 256);
    write_vec(vec_slot(ds::gOrientationUpVector, kPlayerSlot), o);
    o.x = (Bit32s)lround(nf[0] * 256); o.y = (Bit32s)lround(nf[1] * 256); o.z = (Bit32s)lround(nf[2] * 256);
    write_vec(vec_slot(ds::gOrientationFrontVector, kPlayerSlot), o);
}

// Returns the rock's slot, or -1 when we have none.  With our own ship as
// the target the rock goes dead ahead of it instead, flying along, so our
// guns cannot miss it.
static int rock_onto(int target) {
    int rock = first_rock();
    if (rock != -1 && target == kPlayerSlot) {
        enum { kAhead = 2800 };  // in lengths of the front vector (256): where the two guns' bolts have converged
        Vec3 p = read_vec(vec_slot(ds::gPositionVector, kPlayerSlot));
        Vec3 f = read_vec(vec_slot(ds::gOrientationFrontVector, kPlayerSlot));
        p.x += f.x * kAhead; p.y += f.y * kAhead; p.z += f.z * kAhead;
        write_vec(vec_slot(ds::gPositionVector, rock), p);
        write_vec(vec_slot(ds::gVelocityVector, rock), read_vec(vec_slot(ds::gVelocityVector, kPlayerSlot)));
    } else if (rock != -1) {
        // Coming the other way, so the two meet at twice the ship's speed;
        // and they meet in the frame that begins now: the ship will have
        // moved by its velocity and the rock by the opposite, so the rock
        // starts two of those steps ahead of the ship.
        Vec3 p = read_vec(vec_slot(ds::gPositionVector, target));
        Vec3 v = read_vec(vec_slot(ds::gVelocityVector, target));
        p.x += 2 * v.x; p.y += 2 * v.y; p.z += 2 * v.z;
        write_vec(vec_slot(ds::gPositionVector, rock), p);
        v.x = -v.x; v.y = -v.y; v.z = -v.z;
        write_vec(vec_slot(ds::gVelocityVector, rock), v);
        // where the last frame drew the ship: the game lets a rock it did not
        // draw vanish instead of colliding (ovr141:2411)
        wr16((Bit16u)(ds::entityCullStatus + 2 * rock), rd16((Bit16u)(ds::entityCullStatus + 2 * target)));
        wr16((Bit16u)(ds::entityScreenY + 2 * rock), rd16((Bit16u)(ds::entityScreenY + 2 * target)));
    }
    return rock;
}

// Test aid: the game's screen (VGA mode 13h, 320x200, 256 colours) as a PPM
// file, for runs with no display.
static bool write_screenshot(const std::string &path) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    fprintf(f, "P6\n320 200\n255\n");
    for (Bit32u i = 0; i < 320 * 200; i++) {
        const RGBEntry &c = vga.dac.rgb[mem_readb(0xA0000 + i)];
        fputc((c.red << 2) | (c.red >> 4), f);
        fputc((c.green << 2) | (c.green >> 4), f);
        fputc((c.blue << 2) | (c.blue >> 4), f);
    }
    fclose(f);
    return true;
}

// Test aid: with WCNET_AUTOKEYS=1 press Enter every 1.5 s so a headless
// instance advances through the briefing on its own (scripts/wcnet-smoke.sh).
static void auto_keys() {
    static int enabled = -1;
    static double lastMs = 0;
    static bool pressed = false;
    if (enabled < 0) {
        const char *env = getenv("WCNET_AUTOKEYS");
        enabled = (env && env[0] && env[0] != '0') ? 1 : 0;
    }
    if (!enabled) {
        return;
    }
    static KBD_KEYS key = KBD_enter;
    static int autopilots = 0;
    double now = PIC_FullIndex();
    if (pressed && now - lastMs > 60) {
        KEYBOARD_AddKey(key, false);
        pressed = false;
    } else if (!pressed && now - lastMs > 1500) {
        bool flying = in_flight();
        if (flying && getenv("WCNET_KEYSCRIPT")) {
            return;  // the key script flies this instance
        }
        if (flying && autopilots < 3 && now - lastMs > 8000) {
            key = KBD_a;  // engage autopilot: the enemies come to us
            autopilots++;
        } else if (flying) {
            return;  // in space: leave the controls alone
        } else {
            key = KBD_enter;  // advance the briefing / menus
        }
        KEYBOARD_AddKey(key, true);
        pressed = true;
        lastMs = now;
    }
}

// Test aid: WCNET_KEYSCRIPT="<t>:<item>,<t>:<item>,..." runs a scripted
// flight once the player is in space: at <t> emulated seconds after that,
// <item> is a key to hold for 150 ms (one letter or digit, enter, esc) or
// "@<tag>", which writes the data segment to WCNET_DUMP_DIR/<tag>.bin and
// logs the comm line and the VDU text, so two instances' views of the same
// orders can be diffed, or "!status=<n>", which ends the mission the way the
// game would (1 landed, 2 ejected, 4 died, 5 quit), "!kill=<slot>", which on
// the server destroys that ship as a kill by the player would,
// "!poke=<hex offset>:<hex byte>", which writes the data segment,
// "!mouse=<x>:<y>", which puts the pointer there (fractions of the range),
// "!button=<n>:<1|0>", which presses or releases a mouse button,
// "!pos=<x>:<y>:<z>", which moves our own ship, "!face=<slot|npc|rock>",
// which turns it towards that slot, "!rock=<slot|npc>", which puts one of our
// field's rocks or mines on that ship (0: dead ahead of our own),
// "!rocks=<1|0|2>", which switches the asteroid and mine fields (2: soft),
// "!shot=<name>", which writes the screen to WCNET_DUMP_DIR/<name>.ppm,
// "!hit=<qty>[:back]", which damages our own ship as a hit from ahead would,
// "!say=<text>", which sends a comms message or command ("_" for a space),
// "!where", which logs where the program is and who called it,
// "+<key>" / "-<key>", which hold and release a key, or "wait", which holds
// the script until the next mission reaches its first frame and restarts
// the clock there.  The clock starts at the first in-flight frame of the
// first mission and keeps running through debriefings and cutscenes.
struct ScriptStep {
    double at;
    std::string item;
};

static KBD_KEYS script_key(const std::string &name) {
    static const struct { const char *name; KBD_KEYS key; } kNamed[] = {
        { "enter", KBD_enter }, { "esc", KBD_esc }, { "space", KBD_space }, { "tab", KBD_tab },
        { "backspace", KBD_backspace }, { "up", KBD_up }, { "down", KBD_down }, { "left", KBD_left },
        { "right", KBD_right }, { "comma", KBD_comma }, { "period", KBD_period }, { "equals", KBD_equals },
        { "minus", KBD_minus },
    };
    for (size_t i = 0; i < sizeof(kNamed) / sizeof(kNamed[0]); i++) {
        if (name == kNamed[i].name) return kNamed[i].key;
    }
    if (name.size() >= 2 && name[0] == 'f' && atoi(name.c_str() + 1) >= 1 && atoi(name.c_str() + 1) <= 12) {
        return (KBD_KEYS)(KBD_f1 + atoi(name.c_str() + 1) - 1);
    }
    if (name.size() == 1) {
        char c = name[0];
        static const char kLetters[] = "qwertyuiopasdfghjklzxcvbnm";  // the enum's order
        const char *p = (c >= 'a' && c <= 'z') ? strchr(kLetters, c) : NULL;
        if (p) return (KBD_KEYS)(KBD_q + (p - kLetters));
        if (c >= '1' && c <= '9') return (KBD_KEYS)(KBD_1 + (c - '1'));
        if (c == '0') return KBD_0;
    }
    return KBD_NONE;
}

// An absolute pointer position, 0..1 across the mouse driver's range (what a
// controller's stick asks for).  The matching relative motion is reported as
// well, for programs that read mickeys instead of the position.
static void set_pointer(double fx, double fy) {
    static double lastX = 0.5, lastY = 0.5;
    Mouse_CursorMoved((float)((fx - lastX) * 640.0), (float)((fy - lastY) * 200.0), (float)fx, (float)fy, false);
    lastX = fx;
    lastY = fy;
}

static std::string ds_text(Bit16u off, int max) {
    std::string s;
    int nuls = 0;
    for (int i = 0; i < max; i++) {
        Bit8u c = rd8((Bit16u)(off + i));
        nuls = c ? 0 : nuls + 1;
        if (nuls > 2) break;  // the VDU keeps several NUL-separated lines
        s += !c ? '~' : (c == '\n') ? '|' : (c < 32 || c > 126) ? '.' : (char)c;
    }
    return s;
}

static void key_script() {
    static int parsed = 0;
    static std::vector<ScriptStep> steps;
    static size_t next = 0;
    static double startMs = -1;
    static KBD_KEYS held = KBD_NONE;
    static double releaseAt = 0;
    if (!parsed) {
        parsed = 1;
        const char *env = getenv("WCNET_KEYSCRIPT");
        std::string all = env ? env : "";
        size_t pos = 0;
        while (pos < all.size()) {
            size_t comma = all.find(',', pos);
            if (comma == std::string::npos) comma = all.size();
            std::string tok = all.substr(pos, comma - pos);
            size_t colon = tok.find(':');
            if (colon != std::string::npos) {
                ScriptStep st;
                st.at = atof(tok.substr(0, colon).c_str());
                st.item = tok.substr(colon + 1);
                steps.push_back(st);
            }
            pos = comma + 1;
        }
        if (!steps.empty()) wclog(1, "key script: %d steps", (int)steps.size());
    }
    if (next >= steps.size() && held == KBD_NONE) {
        return;
    }
    double now = PIC_FullIndex();
    if (held != KBD_NONE) {
        if (now >= releaseAt) {
            KEYBOARD_AddKey(held, false);
            held = KBD_NONE;
        }
        return;
    }
    static int waitingSince = -1;  // flight count when a "wait" item ran
    if (waitingSince >= 0) {
        if (g_flightCount == waitingSince || !in_flight()) {
            return;
        }
        waitingSince = -1;
        startMs = now;
        wclog(1, "key script: next mission in space, clock restarted (t=%.1f)", now / 1000.0);
    }
    if (startMs < 0) {
        // WCNET_KEYSCRIPT_FROM=boot starts the clock with the program instead
        // of in space (for menus, cutscenes and games the hooks do not know).
        static int fromBoot = -1;
        if (fromBoot < 0) {
            const char *from = getenv("WCNET_KEYSCRIPT_FROM");
            fromBoot = from && !strcmp(from, "boot") ? 1 : 0;
        }
        if (fromBoot || in_flight()) {
            startMs = now;
            wclog(1, "key script: %s, clock started (t=%.1f)", fromBoot ? "from boot" : "in space", now / 1000.0);
        }
        return;
    }
    if ((now - startMs) / 1000.0 < steps[next].at) {
        return;
    }
    const std::string &item = steps[next].item;
    double t = (now - startMs) / 1000.0;
    if (!item.empty() && item[0] == '@') {
        const char *dir = getenv("WCNET_DUMP_DIR");
        std::string path = std::string(dir && dir[0] ? dir : ".") + "/" + item.substr(1) + ".bin";
        FILE *f = fopen(path.c_str(), "wb");
        if (f) {
            for (Bit32u off = 0; off < 0x10000; off++) fputc(rd8((Bit16u)off), f);
            fclose(f);
        }
        wclog(1, "key script %.1fs (t=%.1f): dump %s; comm \"%s\"; vdu \"%s\"; %s; %s", t, now / 1000.0, path.c_str(),
              ds_text(ds::commGlobalTxt, 80).c_str(), is_wc1() ? ds_text(0x8E4A, 160).c_str() : "", mission_state_line().c_str(),
              field_state_line().c_str());
    } else if (item == "wait") {
        wclog(1, "key script %.1fs (t=%.1f): waiting for the next mission", t, now / 1000.0);
        waitingSince = g_flightCount;
    } else if (item.compare(0, 7, "!mouse=") == 0) {
        // !mouse=<x>:<y>, fractions of the mouse range (0.5:0.5 is the centre)
        double fx = atof(item.c_str() + 7);
        size_t colon = item.find(':');
        double fy = colon == std::string::npos ? 0.5 : atof(item.c_str() + colon + 1);
        wclog(2, "key script %.1fs (t=%.1f): pointer to %.2f, %.2f", t, now / 1000.0, fx, fy);
        set_pointer(fx, fy);
    } else if (item.compare(0, 8, "!button=") == 0) {
        // !button=<0 left, 1 right>:<1 down, 0 up>
        int button = atoi(item.c_str() + 8);
        size_t colon = item.find(':');
        bool down = colon != std::string::npos && atoi(item.c_str() + colon + 1) != 0;
        wclog(2, "key script %.1fs (t=%.1f): mouse button %d %s", t, now / 1000.0, button, down ? "down" : "up");
        if (down) Mouse_ButtonPressed((Bit8u)button); else Mouse_ButtonReleased((Bit8u)button);
    } else if (item.size() > 1 && (item[0] == '+' || item[0] == '-') && script_key(item.substr(1)) != KBD_NONE) {
        // +<key> holds a key down, -<key> lets it go
        wclog(2, "key script %.1fs (t=%.1f): %s %s", t, now / 1000.0, item[0] == '+' ? "hold" : "release", item.c_str() + 1);
        KEYBOARD_AddKey(script_key(item.substr(1)), item[0] == '+');
    } else if (item.compare(0, 6, "!poke=") == 0) {
        // !poke=<hex offset>:<hex byte>
        Bit16u off = (Bit16u)strtol(item.c_str() + 6, NULL, 16);
        size_t colon = item.find(':');
        Bit8u val = colon == std::string::npos ? 0 : (Bit8u)strtol(item.c_str() + colon + 1, NULL, 16);
        wclog(2, "key script %.1fs (t=%.1f): dseg:%04X = %02x (was %02x)", t, now / 1000.0, off, val, rd8(off));
        wr8(off, val);
    } else if (item.compare(0, 6, "!face=") == 0 || item.compare(0, 6, "!rock=") == 0) {
        // !face=<slot|npc|rock> turns our ship towards that slot;
        // !rock=<slot|npc> puts one of our field's rocks or mines on that
        // ship, !rock=0 dead ahead of our own
        int slot = script_slot(item.c_str() + 6);
        bool face = item[1] == 'f';
        if (slot >= 0 && slot < kNumSlots && slot_in_use(slot)) {
            // The rock goes on at the start of the next frame, when the ship
            // is where this frame will have it (a client has just taken the
            // server's positions): put there now, a frame's motion of the
            // ship could carry it clear before anything tests for a hit.
            if (face) face_slot(slot); else g_rockOnto = slot;
        }
        wclog(1, "key script %.1fs (t=%.1f): %s slot %d (%s, drawn at %04x); %s", t, now / 1000.0,
              face ? "face" : "rock onto", slot, slot >= 0 && slot_in_use(slot) ? "in use" : "empty",
              slot >= 0 ? rd16((Bit16u)(ds::entityCullStatus + 2 * slot)) : 0, field_state_line().c_str());
    } else if (item.compare(0, 5, "!pos=") == 0) {
        // !pos=<x>:<y>:<z> moves our own ship there
        Vec3 v = read_vec(vec_slot(ds::gPositionVector, kPlayerSlot));
        size_t c1 = item.find(':'), c2 = c1 == std::string::npos ? c1 : item.find(':', c1 + 1);
        if (c2 != std::string::npos) {
            v.x = atoi(item.c_str() + 5);
            v.y = atoi(item.c_str() + c1 + 1);
            v.z = atoi(item.c_str() + c2 + 1);
            write_vec(vec_slot(ds::gPositionVector, kPlayerSlot), v);
        }
        wclog(1, "key script %.1fs (t=%.1f): own ship at %d, %d, %d", t, now / 1000.0, v.x, v.y, v.z);
    } else if (item.compare(0, 5, "!hit=") == 0) {
        // !hit=<qty>[:back] damages our own ship from dead ahead (or behind)
        int qty = atoi(item.c_str() + 5);
        bool back = item.find(":back") != std::string::npos;
        ShipHealthState h = read_health(kPlayerSlot);
        wclog(1, "key script %.1fs (t=%.1f): hit our ship with %d from %s; shield %d/%d armor %d/%d/%d/%d core %d; "
              "systems %d %d %d %d %d %d %d %d %d; cockpit %d %d %d %d", t, now / 1000.0, qty, back ? "behind" : "ahead",
              h.shield[0], h.shield[1], h.armor[0], h.armor[1], h.armor[2], h.armor[3], h.coreHp,
              rd8(ds::systemDamage), rd8(ds::systemDamage + 1), rd8(ds::systemDamage + 2), rd8(ds::systemDamage + 3),
              rd8(ds::systemDamage + 4), rd8(ds::systemDamage + 5), rd8(ds::systemDamage + 6), rd8(ds::systemDamage + 7),
              rd8(ds::systemDamage + 8), rd16(ds::cockpitDamage), rd16(ds::cockpitDamage + 2), rd16(ds::cockpitDamage + 4),
              rd16(ds::cockpitDamage + 6));
        enqueue_test_hit(qty, back);
    } else if (item == "!where") {
        // !where logs where the program is: CS:IP with the bytes there (for
        // scripts/wcexe.py find) and the far return addresses up the stack
        char bytes[3 * 16 + 1];
        for (int i = 0; i < 16; i++) {
            snprintf(bytes + 3 * i, 4, "%02x ", mem_readb(SegPhys(cs) + ((reg_eip + i) & 0xffff)));
        }
        std::string chain;
        Bit16u bp = (Bit16u)reg_ebp;
        for (int i = 0; i < 10 && bp; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), " %04x:%04x", mem_readw(SegPhys(ss) + ((bp + 4) & 0xffff)), mem_readw(SegPhys(ss) + ((bp + 2) & 0xffff)));
            chain += buf;
            Bit16u next = mem_readw(SegPhys(ss) + bp);
            if (next <= bp) {
                break;
            }
            bp = next;
        }
        wclog(1, "key script %.1fs (t=%.1f): at %04x:%04x (load segment %04x) bytes %s callers%s", t, now / 1000.0,
              SegValue(cs), (unsigned)reg_eip, g_loadSeg, bytes, chain.c_str());
    } else if (item.compare(0, 5, "!say=") == 0) {
        // !say=<text> is a line typed at the comms prompt ("/chase", "/rocks_off")
        std::string text = item.substr(5);
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] == '_') {
                text[i] = ' ';
            }
        }
        wclog(1, "key script %.1fs (t=%.1f): say \"%s\"", t, now / 1000.0, text.c_str());
        wcnetSendChatMessage(text);
    } else if (item.compare(0, 6, "!shot=") == 0) {
        // !shot=<name> writes the screen to WCNET_DUMP_DIR/<name>.ppm
        const char *dir = getenv("WCNET_DUMP_DIR");
        std::string path = std::string(dir && dir[0] ? dir : ".") + "/" + item.substr(6) + ".ppm";
        wclog(1, "key script %.1fs (t=%.1f): screenshot %s%s", t, now / 1000.0, path.c_str(),
              write_screenshot(path) ? "" : " failed");
    } else if (item.compare(0, 7, "!rocks=") == 0) {
        // !rocks=<1|0|2> switches the asteroid and mine fields on, off or
        // soft, as the host's lobby option or "/rocks ..." in the comms
        // prompt does
        int mode = atoi(item.c_str() + 7);
        wclog(1, "key script %.1fs (t=%.1f): rocks mode %d; %s", t, now / 1000.0, mode, field_state_line().c_str());
        wc_net_set_rocks(mode);
    } else if (item.compare(0, 6, "!kill=") == 0) {
        int slot = atoi(item.c_str() + 6);
        bool server = g_session && g_session->is_server();
        wclog(1, "key script %.1fs (t=%.1f): kill slot %d (%s); %s", t, now / 1000.0, slot,
              !server ? "ignored: not the server" : slot_in_use(slot) ? "in use" : "empty", mission_state_line().c_str());
        if (server && slot_in_use(slot)) {
            enqueue_test_kill(slot);
        }
    } else if (item.compare(0, 8, "!status=") == 0) {
        int status = atoi(item.c_str() + 8);
        wclog(1, "key script %.1fs (t=%.1f): mission status %d -> %d", t, now / 1000.0, rd16(ds::missionStatus), status);
        wr16(ds::missionStatus, (Bit16u)status);
    } else {
        KBD_KEYS k = script_key(item);
        if (k == KBD_NONE) {
            wclog(0, "key script: unknown key \"%s\"", item.c_str());
        } else {
            wclog(1, "key script %.1fs (t=%.1f): press %s", t, now / 1000.0, item.c_str());
            KEYBOARD_AddKey(k, true);
            held = k;
            releaseAt = now + 150;
        }
    }
    next++;
}

// Every place the dispatcher looks at, as a bitmap over IP: built for the
// game that is running, empty while none is.
static void build_watch_list() {
    memset(g_watch, 0, sizeof(g_watch));
#define X(name) if (code::name.known()) watch(code::name.off);
    WC_CODE_LIST(X)
#undef X
    if (g_game != GAME_NONE) {
        watch(Trampoline::hook_ip());
    }
    g_watchReady = true;
}

void hooks_game_changed() {
    g_watchReady = false;
    wc_net_countdown = 0;  // the table is rebuilt before the next instruction
}

bool g_pendingUninit = false;

enum { kMinAttemptSpacingSeconds = 5, kRetryDelaySeconds = 10 };

// Debugging aid: writes the game's data segment to WCNET_DUMP_FILE (default
// wcnet-ds.bin, a counter is appended after the first dump) at the top of
// in-flight frame WCNET_DUMP_FRAME=<n>, and whenever SIGUSR1 arrives, so two
// machines' views of the same mission can be diffed.
static volatile sig_atomic_t g_dumpRequested = 0;

static void on_dump_signal(int) {
    g_dumpRequested = 1;
}

static void maybe_dump_data_segment() {
    static int frame = 0;
    static int wanted = -1;
    static int dumps = 0;
    if (wanted < 0) {
        const char *env = getenv("WCNET_DUMP_FRAME");
        wanted = env ? atoi(env) : 0;
        signal(SIGUSR1, on_dump_signal);
    }
    ++frame;
    if (frame != wanted && !g_dumpRequested) {
        return;
    }
    g_dumpRequested = 0;
    const char *env = getenv("WCNET_DUMP_FILE");
    std::string path = env && env[0] ? env : "wcnet-ds.bin";
    if (dumps++ > 0) {
        char suffix[16];
        snprintf(suffix, sizeof(suffix), ".%d", dumps);
        path += suffix;
    }
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    for (Bit32u off = 0; off < 0x10000; off++) {
        fputc(rd8((Bit16u)off), f);
    }
    fclose(f);
    wclog(1, "data segment dumped to %s at frame %d", path.c_str(), frame);
}

// (Re)connect when there is no session: at most every few seconds, and not
// for a while after a failed attempt.  A missing TCP server or a lobby join
// costs real time, and this runs at the top of every frame in space.
static void ensure_session() {
    static time_t nextAttempt = 0;
    if (g_session) {
        return;
    }
    time_t now = time(NULL);
    if (now < nextAttempt) {
        return;
    }
    nextAttempt = now + kMinAttemptSpacingSeconds;
    if (!init_network()) {
        nextAttempt = now + kRetryDelaySeconds;
    }
}

static void on_after_startup() {
    has_started_up = true;
    // MIS / SERIES (the game's own indices: series 1.., mission 0.. within
    // the series) skip the barracks and fly that mission; setting either is
    // enough, so MIS=0 SERIES=1 (the first mission) can be asked for too.
    const char *misenv = getenv("MIS");
    const char *serenv = getenv("SERIES");
    // WCCALLSIGN / WCLASTNAME name the pilot even without a network session
    // (the briefing would otherwise address the save game's Bluehair).
    get_callsign();
    get_last_name();
    if ((misenv && misenv[0]) || (serenv && serenv[0])) {
        int mission = atoi(misenv ? misenv : "0");
        int series = atoi(serenv ? serenv : "1");
        if (g_haveLastMission) {
            // Not the start of the program: the game is back at its title
            // loop because the last mission ended in a death (or the carrier
            // was lost, or somebody quit).  Fly that mission again, wherever
            // the campaign had got to, instead of the one the lobby picked.
            mission = g_lastMission;
            series = g_lastSeries;
            wclog(1, "the last mission did not end with a landing: flying %d/%d again", mission, series);
        } else {
            g_creditSeries = series;
            g_creditMissions = mission;
        }
        run_campaign(mission, series);
        g_skipBarracks = true;
    }
}

// WCNET_SKIPBARRACKS=1 (test aid): never stop in the rec room or the
// barracks, so a scripted run goes from one debriefing to the next briefing.
static bool always_skip_barracks() {
    static int always = -1;
    if (always < 0) {
        const char *env = getenv("WCNET_SKIPBARRACKS");
        always = (env && env[0] && env[0] != '0') ? 1 : 0;
    }
    return always == 1;
}

static void on_barracks() {
    ensure_session();
    if (g_session) {
        g_session->on_barracks();
    }
    if (g_skipBarracks || always_skip_barracks()) {
        g_skipBarracks = false;
        reg_eax = 7;
        reg_eip = code::enterBarracksReturn.off;  // ovr150: return from enterBarracks
    }
}

static void check_hooks_slow() {
    if (at_location(code::briefingStarted)) {
        wclog(2, "briefing animation starting for mission %d/%d", rd8(ds::currentMission), rd8(ds::currentSeries));
    }
    if (at_location(code::flyMission)) {
        // WC2's barracks: "Fly mission" was chosen and the briefing scene
        // is next.  The client meets the server here, not when the mission
        // loads, so that it watches the briefing of the mission it is
        // going to fly: both come from the campaign record.
        ensure_session();
        PhysPt rec = PhysMake(rd16((Bit16u)(ds::campaignState + 2)), rd16(ds::campaignState));
        if (g_session && rec) {
            has_started_up = true;
            g_session->on_barracks();
            wr8(ds::currentSeries, (Bit8u)mem_readw(rec + 4));
            wr8(ds::currentMission, (Bit8u)mem_readw(rec + 6));
            g_session->on_mission_starting(rd8(ds::currentMission), rd8(ds::currentSeries));
            // (A client's record is the server's now: wcnet_session.cpp, load_campaign_state.)
            g_briefed = true;
        }
    }
    bool direct = at_location(code::missionStartingDirect);
    if (at_location(code::missionStarting) || direct) {
        bool briefed = g_briefed;  // the session's part is done (flyMission above)
        g_briefed = false;
        if (!briefed) {
            ensure_session();
        }
        if (direct && !briefed && !(g_session && g_session->is_client())) {
            // WC2's mission test flies what the command line named
            // (dseg:00D0 mission, 00D2 series), and that is the mission
            // the clients are told.
            wr8(ds::currentMission, (Bit8u)rd16(0x00D0));
            wr8(ds::currentSeries, (Bit8u)rd16(0x00D2));
        }
        if (g_session && !briefed && !code::enterBarracks.known()) {
            // WC2's mission test has no barracks: this is where a client meets the server.
            has_started_up = true;
            g_session->on_barracks();
        }
        if (g_session && !briefed) {
            g_session->on_mission_starting(rd8(ds::currentMission), rd8(ds::currentSeries));
        }
        // After the session: a client flies the mission the server names.
        g_inMission = true;
        g_frameSeen = false;
        g_haveLastMission = true;
        g_lastMission = rd8(ds::currentMission);
        g_lastSeries = rd8(ds::currentSeries);
        if (direct) {
            // A client flies the server's mission there too.
            wr16(0x00D0, rd8(ds::currentMission));
            wr16(0x00D2, rd8(ds::currentSeries));
        }
        wclog(2, "starting: %s", mission_state_line().c_str());
    }
    if (at_location(code::missionVictoryCalc) && g_session) {
        g_session->on_mission_reset();
    }
    if (at_location(code::simulatorStart)) {
        in_simulation += 1;
        incoming_text = "";
    }
    if (in_simulation) {
        if (at_location(code::simulatorEnd)) {
            in_simulation -= 1;
        }
        return;
    }

    if (at_function(code::cloak)) {
        on_cloak_entry(true);
    } else if (at_function(code::uncloak)) {
        on_cloak_entry(false);
    }
    if (at_function(code::do_damage)) {
        on_do_damage_entry();
    } else if (at_function(code::fireGunFromShip)) {
        on_fire_entry();
    } else if (at_function(code::delayedDespawn)) {
        on_delayed_despawn_entry();
    } else if (at_function(code::outerSpawnShipEntity)) {
        on_spawn_entry();
    } else if (at_function(code::despawn)) {
        on_despawn_entry();
    } else if (at_function(code::aiShipThink)) {
        on_ai_think_entry();
    } else if (at_function(code::aiSetSpeed)) {
        on_ai_set_speed_entry();
    }

    if (at_location(code::missionVictoryCalc)) {
        credit_skipped_missions();  // (a client then takes the server's count)
    }
    if (at_location(code::missionVictoryCalc) && g_session) {
        g_session->on_mission_victory_calc();
    }
    if (at_function(code::enterBarracks)) {
        on_barracks();
    }
    if (at_location(code::skipOrchestra)) {
        has_started_up = true;
        reg_eip += 5;
    }
    if (g_session && (at_location(code::statusCheckAfterKeys) || at_location(code::statusCheckAfterFrame) ||
                      at_location(code::statusSetByExitKey))) {
        g_session->check_mission_status();
    }
    if (at_location(code::missionEnded) || at_location(code::missionEndedDirect)) {
        g_inMission = false;
        wclog(2, "ended: %s", mission_state_line().c_str());
        if (always_skip_barracks()) {
            wr16(ds::skipRecRoom, 1);
        }
        if (g_session) {
            g_session->on_mission_ended();
        }
        if (g_session && !code::missionVictoryCalc.known()) {
            // WC2 has no score computation to hang the reset and the last
            // frame on: do both here.
            g_session->on_mission_reset();
            g_session->on_mission_victory_calc();
        }
    }
    if (at_location(code::turretFireShot)) {
        on_turret_fire_shot();
    }
    if (at_location(code::turretAutoNext)) {
        on_turret_auto_next();
    }
    if (at_location(code::autopilotKey) && g_session && g_session->is_client()) {
        reg_eip += 5;  // only the server may engage autopilot
    }
    if (at_location(code::replayKey)) {
        // WC2's R shows the last in-flight scene again: the flight stands
        // still for it on this machine alone, and the key sits next to the
        // ones a pilot uses.  It does nothing here.
        wclog(2, "R (the game's replay of the last seconds) is switched off");
        reg_eip += 0x10;  // to the handler's `jmp` out (seg001:1046)
    }
    if (at_location(code::autopilotFinished) && g_session && g_session->is_server()) {
        g_session->on_autopilot_finished();
    }
    if (at_location(code::autoAnimationBody) && g_session && g_session->is_server()) {
        Bit16u camShipType = mem_readw(DS_OFF + (reg_ebp & 0xffff) + 0x6);
        Bit16u camMode = mem_readw(DS_OFF + (reg_ebp & 0xffff) + 0x8);
        Bit16u duration = mem_readw(DS_OFF + (reg_ebp & 0xffff) + 0xa);
        g_session->on_autopilot_begin(camShipType, camMode, duration);
    }
    if (at_location(code::afterStartup)) {
        on_after_startup();
    }
    if (Trampoline::at_hook()) {
        g_trampoline.on_bounce();
    }
    if (at_location(code::mainLoopTop)) {
        if (!g_frameSeen) {
            g_frameSeen = true;
            g_flightCount++;
        }
        maybe_dump_data_segment();
        // The loop comes here once per game frame, and again whenever a
        // trampoline run started here is over: only the first is a frame.
        static int lastFrame = -1;
        if (rd16(ds::frameCounter) != lastFrame) {
            if (pace_frame(!(g_session && g_session->is_client()))) {
                g_trampoline.run_before_current_instruction();
                return;  // early: back here when the frame is due
            }
            lastFrame = rd16(ds::frameCounter);
            perf_frame();
        }
        ensure_session();
        if (g_session) {
            g_session->on_frame_top();
        } else if (g_trampoline.has_pending()) {
            // Flying alone: nothing else runs the test aids' queued game calls.
            g_trampoline.run_before_current_instruction();
        }
        if (!g_trampoline.is_running() && g_rockOnto >= 0) {
            // The frame begins now (see "!rock=").
            int rock = slot_in_use(g_rockOnto) ? rock_onto(g_rockOnto) : -1;
            wclog(1, "key script: rock %d put onto slot %d", rock, g_rockOnto);
            g_rockOnto = -1;
        }
    }
}

}  // namespace wc

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// For the page (web/wc.js): emulated milliseconds so far, to compare the
// emulation speed with wall-clock time.
extern "C" EMSCRIPTEN_KEEPALIVE double wc_web_emulated_ms() {
    return PIC_FullIndex();
}
// The flight loop's performance over the last interval (wcnet_perf.h):
// 0 fps, 1 emulator speed against real time, 2 ms per frame waiting for the
// network, 3 cycles, 4 the longest frame in ms, 5 the share of the time the
// game was working; -1 before the first interval.
extern "C" EMSCRIPTEN_KEEPALIVE double wc_web_perf(int what) {
    const wc::PerfSample &p = wc::perf_last();
    if (!p.valid) {
        return -1;
    }
    return what == 0 ? p.fps : what == 1 ? p.speed : what == 2 ? p.waitMs : what == 3 ? p.cycles : what == 4 ? p.worstFrameMs : p.load;
}
// One byte of the game's data segment, for checks from the page.
extern "C" EMSCRIPTEN_KEEPALIVE int wc_web_ds_byte(int off) {
    return (off < 0 || off > 0xffff) ? -1 : (int)wc::rd8((Bit16u)off);
}
// Input from the page's controller support (web/gamepad.js): a game pad is
// presented to the game as its mouse (the sticks) and its keyboard (the
// buttons).  The page calls these between slices of emulation, so they only
// record what is wanted; the async tick applies it from inside the emulator.
static_assert(KBD_q == 11 && KBD_a == 21 && KBD_x == 31 && KBD_esc == 49 && KBD_tab == 50 && KBD_enter == 52 &&
              KBD_space == 53 && KBD_minus == 64 && KBD_equals == 65 && KBD_period == 71 && KBD_comma == 72 &&
              KBD_right == 86, "web/gamepad.js numbers the keys in this order");
enum { kWebKeyQueue = 64 };
static struct { Bit8u key; Bit8u pressed; } g_webKeys[kWebKeyQueue];
static unsigned g_webKeyHead = 0, g_webKeyTail = 0;
static double g_webPointerX = 0.5, g_webPointerY = 0.5;
static bool g_webPointerWanted = false;
static unsigned g_webButtonsWanted = 0, g_webButtonsDown = 0;

extern "C" EMSCRIPTEN_KEEPALIVE void wc_web_key(int kbd, int pressed) {
    unsigned next = (g_webKeyHead + 1) % kWebKeyQueue;
    if (kbd <= KBD_NONE || kbd >= KBD_LAST || next == g_webKeyTail) {
        return;
    }
    g_webKeys[g_webKeyHead].key = (Bit8u)kbd;
    g_webKeys[g_webKeyHead].pressed = pressed ? 1 : 0;
    g_webKeyHead = next;
}
// The pointer as fractions (0..1) of the mouse driver's range.
extern "C" EMSCRIPTEN_KEEPALIVE void wc_web_pointer(double fx, double fy) {
    g_webPointerX = fx < 0 ? 0 : fx > 1 ? 1 : fx;
    g_webPointerY = fy < 0 ? 0 : fy > 1 ? 1 : fy;
    g_webPointerWanted = true;
}
extern "C" EMSCRIPTEN_KEEPALIVE void wc_web_mouse_button(int button, int pressed) {
    if (button < 0 || button > 2) {
        return;
    }
    if (pressed) g_webButtonsWanted |= 1u << button; else g_webButtonsWanted &= ~(1u << button);
}
// The host's page switches the asteroid and mine fields (its lobby option).
static int g_webRocksWanted = -1;
extern "C" EMSCRIPTEN_KEEPALIVE void wc_web_set_rocks(int mode) {
    g_webRocksWanted = mode < 0 ? 0 : mode;
}
extern "C" EMSCRIPTEN_KEEPALIVE int wc_web_rocks() {
    return wc_net_rocks();
}
// Nonzero while a mission's frame loop runs (flying, autopilot included):
// the page steers with the stick then, and moves a menu pointer otherwise.
extern "C" EMSCRIPTEN_KEEPALIVE int wc_web_in_flight() {
    return wc::in_flight() ? 1 : 0;
}
static void web_input_tick() {
    while (g_webKeyTail != g_webKeyHead) {
        KEYBOARD_AddKey((KBD_KEYS)g_webKeys[g_webKeyTail].key, g_webKeys[g_webKeyTail].pressed != 0);
        g_webKeyTail = (g_webKeyTail + 1) % kWebKeyQueue;
    }
    if (g_webPointerWanted) {
        g_webPointerWanted = false;
        wc::set_pointer(g_webPointerX, g_webPointerY);
    }
    for (unsigned b = 0; b < 3; b++) {
        unsigned bit = 1u << b;
        if ((g_webButtonsWanted & bit) && !(g_webButtonsDown & bit)) Mouse_ButtonPressed((Bit8u)b);
        if (!(g_webButtonsWanted & bit) && (g_webButtonsDown & bit)) Mouse_ButtonReleased((Bit8u)b);
    }
    g_webButtonsDown = g_webButtonsWanted;
    if (g_webRocksWanted >= 0) {
        wc_net_set_rocks(g_webRocksWanted);
        g_webRocksWanted = -1;
    }
}

// A short Esc press from the page (its Caps Lock handler): browsers keep the
// real Esc key for leaving full screen.  Released by the async tick.
static bool g_escTapWanted = false;
extern "C" EMSCRIPTEN_KEEPALIVE void wc_web_tap_escape() {
    g_escTapWanted = true;
}
static void escape_tap() {
    static bool held = false;
    static double releaseAt = 0;
    double now = PIC_FullIndex();
    if (held) {
        if (now >= releaseAt) {
            KEYBOARD_AddKey(KBD_esc, false);
            held = false;
        }
    } else if (g_escTapWanted) {
        g_escTapWanted = false;
        KEYBOARD_AddKey(KBD_esc, true);
        held = true;
        releaseAt = now + 150;
    }
}
#endif

// Debugging aid: WCNET_WATCH=<hex offset>:<length> logs every change of that
// part of the data segment with the instruction that follows the write.
static void memory_watch() {
    static int len = -1;
    static Bit16u off = 0;
    static Bit8u seen[64];
    if (len < 0) {
        const char *env = getenv("WCNET_WATCH");
        len = 0;
        if (env && env[0]) {
            off = (Bit16u)strtol(env, NULL, 16);
            const char *colon = strchr(env, ':');
            len = colon ? atoi(colon + 1) : 2;
            if (len > (int)sizeof(seen)) len = sizeof(seen);
            for (int i = 0; i < len; i++) seen[i] = wc::rd8((Bit16u)(off + i));
        }
    }
    for (int i = 0; i < len; i++) {
        Bit8u now = wc::rd8((Bit16u)(off + i));
        if (now != seen[i]) {
            ::wclog(0, "watch: dseg:%04X %02x -> %02x before %04X:%04X", off + i, seen[i], now,
                      (unsigned)SegValue(cs), (unsigned)(reg_eip & 0xffff));
            seen[i] = now;
        }
    }
}

void wc_net_check_cpu_hooks() {
    using namespace wc;
    static int watching = -1;
    if (watching < 0) {
        const char *env = getenv("WCNET_WATCH");
        watching = (env && env[0]) ? 1 : 0;
    }
    bool tick = false;
    if (watching) {
        // (Debugging: every instruction comes here.)
        memory_watch();
        wc_net_countdown = 0;
        tick = ++g_asyncCounter >= 1000;
    } else if (wc_net_countdown < 0) {
        wc_net_countdown = 999;
        tick = true;
    }
    if (tick) {
        g_asyncCounter = 0;
        wc::pace_tick();
        auto_keys();
        key_script();
#ifdef __EMSCRIPTEN__
        escape_tap();
        web_input_tick();
#endif
        if (g_session) {
            g_session->on_async_tick();
        }
    }
    if (!g_watchReady) {
        build_watch_list();
    }
    if (g_pendingUninit) {
        g_pendingUninit = false;
        uninit_network();
    }
    Bit32u ip = reg_eip;
    if (ip >= 0x10000 || !g_watch[ip]) {
        return;
    }
    check_hooks_slow();
}
