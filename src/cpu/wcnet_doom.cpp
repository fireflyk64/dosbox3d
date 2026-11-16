/*
 *  DOOM's launcher and network driver (wcnet_doom.h, docs/doom.md).
 *
 *  DOOMNET.COM (wcnet_doomnet.asm) is a file of the Z: drive made here.  Run in the game's directory ("doomnet", or
 *  "doomnet doom2 -skill 4": a program's name first if not DOOM.EXE, then the game's own arguments) it calls in three
 *  times, by the emulator's callback instruction:
 *
 *    0  at its start: where its buffers are.  The program is looked for, and the WADs beside it (their names and
 *       sizes are what the players must have alike).
 *    1  again and again while it waits (with a key, if one was pressed): the room is met meanwhile
 *       (wcnet_doom_session.cpp).  The answer is "wait", "run the game" (its name and command tail written into the
 *       buffers) or "leave".  With no room, or Esc, the game is run by itself, as from the DOS prompt.
 *    2  when the game it ran is over.
 *
 *  For a network game the block DOOM shares with its driver (doomcom_t) is filled in the launcher's memory -- how
 *  many players, which one we are -- and DOOM is run with "-net <the block's address>", as id's own drivers run it.
 *  DOOM then calls the block's interrupt for every packet: "send" (the node, the length and the packet in the block)
 *  and "get" (the same filled in, or node -1: none).  That interrupt is answered here.  DOOM's node 0 is itself; the
 *  other players are its nodes 1.. in the order of their numbers.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include "wcnet_doom.h"
#include "wcnet_doomnet.h"
#include "wcnet_game.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "callback.h"
#include "mem.h"
#include "regs.h"
#include "dos_inc.h"
#include "dos_system.h"
#include "pic.h"

extern std::string incoming_text;   // the line the emulator draws over the picture (sdlmain.cpp)

namespace wc {

namespace {

// doomcom_t (DOOM's d_net.h): long id; short intnum, command, remotenode, datalength, numnodes, ticdup, extratics,
// deathmatch, savegame, episode, map, skill, consoleplayer, numplayers, angleoffset, drone; doomdata_t data.
enum {
    DC_ID = 0, DC_INTNUM = 4, DC_COMMAND = 6, DC_REMOTENODE = 8, DC_DATALENGTH = 10, DC_NUMNODES = 12, DC_TICDUP = 14,
    DC_EXTRATICS = 16, DC_DEATHMATCH = 18, DC_SAVEGAME = 20, DC_EPISODE = 22, DC_MAP = 24, DC_SKILL = 26,
    DC_CONSOLEPLAYER = 28, DC_NUMPLAYERS = 30, DC_ANGLEOFFSET = 32, DC_DRONE = 34, DC_DATA = 36,
    DC_SIZE = DC_DATA + kDoomPacketMax,
    kDoomcomId = 0x12345678,
    CMD_SEND = 1, CMD_GET = 2,
    kVector = 0x64,             // (id's drivers took the first free of 60h-66h)
};

enum Stage { ST_NONE, ST_WAIT, ST_NET, ST_ALONE, ST_OVER };
Stage g_dstage = ST_NONE;
Bitu g_cbLauncher = 0, g_cbNet = 0;
Bit8u *g_com = NULL;
Bit16u g_seg = 0, g_offBlock = 0, g_offPath = 0, g_offTail = 0;
std::string g_program, g_args, g_gameId, g_lastLine;
bool g_enter = false, g_esc = false;
RealPt g_oldVec = 0;
DoomGame g_dgame;
int g_nodePlayer[kDoomMaxPlayers], g_playerNode[kDoomMaxPlayers];
// what went through the driver, for the log
Bit32u g_sent = 0, g_got = 0, g_asked = 0;
Bit32u g_tics = 0, g_ticsOf[kDoomMaxPlayers];
double g_startedAt = 0, g_loggedAt = 0;
Bit32u g_ticsLogged = 0;
// when each player's game was last heard (0: not yet), and the line said while one is waited for
double g_heardAt[kDoomMaxPlayers], g_waitCheckedAt = 0;
std::string g_waitLine;

void say(const char *fmt, ...) {
    char text[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    wclog(1, "DOOM: %s", text);
    std::string out = std::string(text) + "\r\n";
    Bit16u n = (Bit16u)out.size();
    DOS_WriteFile(STDOUT, (Bit8u *)out.data(), &n);
}

bool file_size(const char *name, Bit32u *size) {
    Bit16u handle;
    if (!DOS_OpenFile(name, 0, &handle)) return false;
    Bit32u pos = 0;
    DOS_SeekFile(handle, &pos, DOS_SEEK_END);
    DOS_CloseFile(handle);
    *size = pos;
    return true;
}

// The program and the WADs beside it, names and sizes: what DOOM would play from.  The players' must be alike.
std::string fingerprint() {
    char buf[64];
    Bit32u size = 0;
    file_size(g_program.c_str(), &size);
    snprintf(buf, sizeof(buf), "%s %u", g_program.c_str(), (unsigned)size);
    std::string out = buf;
    std::vector<std::string> wads;
    const RealPt save = dos.dta();
    dos.dta(dos.tables.tempdta);
    char pattern[] = "*.WAD";
    bool more = DOS_FindFirst(pattern, 0xffff & ~DOS_ATTR_VOLUME & ~DOS_ATTR_DIRECTORY);
    while (more && wads.size() < 16) {
        DOS_DTA dta(dos.dta());
        char name[DOS_NAMELENGTH_ASCII];
        Bit32u fsize;
        Bit16u date, time;
        Bit8u attr;
        dta.GetResult(name, fsize, date, time, attr);
        snprintf(buf, sizeof(buf), "%s %u", name, (unsigned)fsize);
        wads.push_back(buf);
        more = DOS_FindNext();
    }
    dos.dta(save);
    std::sort(wads.begin(), wads.end());
    for (size_t i = 0; i < wads.size(); i++) out += ", " + wads[i];
    return out;
}

std::string trimmed(const std::string &s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// The game's name and command tail written where the launcher's EXEC takes them.
void write_exec(const std::string &tailText) {
    const PhysPt base = (PhysPt)g_seg * 16;
    for (size_t i = 0; i <= g_program.size() && i < 127; i++) mem_writeb(base + g_offPath + (PhysPt)i, i < g_program.size() ? (Bit8u)g_program[i] : 0);
    const std::string tail = tailText.substr(0, 126);
    mem_writeb(base + g_offTail, (Bit8u)tail.size());
    for (size_t i = 0; i < tail.size(); i++) mem_writeb(base + g_offTail + 1 + (PhysPt)i, (Bit8u)tail[i]);
    mem_writeb(base + g_offTail + 1 + (PhysPt)tail.size(), 0x0D);
    wclog(1, "DOOM: running %s%s", g_program.c_str(), tail.c_str());
}

void launcher_begin() {
    g_seg = SegValue(cs);
    g_offBlock = reg_si;
    g_offPath = reg_di;
    g_offTail = reg_dx;
    g_enter = g_esc = false;
    g_lastLine.clear();
    g_dgame = DoomGame();
    // our own command tail: [the program] [the game's arguments]
    const PhysPt psp = (PhysPt)g_seg * 16;
    std::string tail;
    const int len = mem_readb(psp + 0x80);
    for (int i = 0; i < len && i < 127; i++) {
        const Bit8u c = mem_readb(psp + 0x81 + (PhysPt)i);
        if (c == 0x0D || c == 0) break;
        tail += (char)c;
    }
    tail = trimmed(tail);
    g_program.clear();
    const size_t cut = tail.find_first_of(" \t");
    std::string first = tail.substr(0, cut);
    for (size_t i = 0; i < first.size(); i++) first[i] = (char)toupper((unsigned char)first[i]);
    if (!first.empty() && first[0] != '-' && first[0] != '@') {
        const std::string named[2] = { first, first + ".EXE" };
        for (int i = (first.find('.') == std::string::npos ? 1 : 0); i < 2 && g_program.empty(); i++) {
            if (DOS_FileExists(named[i].c_str())) g_program = named[i];
        }
        if (!g_program.empty()) tail = cut == std::string::npos ? std::string() : trimmed(tail.substr(cut));
    }
    static const char *const kPrograms[] = { "DOOM.EXE", "DOOM2.EXE" };
    for (size_t i = 0; i < sizeof(kPrograms) / sizeof(kPrograms[0]) && g_program.empty(); i++) {
        if (DOS_FileExists(kPrograms[i])) g_program = kPrograms[i];
    }
    g_args = tail;
    if (g_program.empty()) {
        say("DOOMNET: no DOOM.EXE in this directory.");
        g_dstage = ST_OVER;
        return;
    }
    g_gameId = fingerprint();
    wclog(1, "DOOM: launcher at %04X: %s; arguments \"%s\"", g_seg, g_gameId.c_str(), g_args.c_str());
    g_dstage = ST_WAIT;
    if (doom_session_configured()) {
        say("DOOM together: the game's own co-operative network game, played over this room.");
    }
}

void run_alone() {
    doom_session_waiting(false, g_gameId, g_args);
    write_exec(g_args.empty() ? std::string() : " " + g_args);
    g_dstage = ST_ALONE;
}

void run_net() {
    static const char *const kColours[kDoomMaxPlayers] = { "green", "indigo", "brown", "red" };
    int node = 1;
    for (int p = 0; p < g_dgame.players; p++) {
        if (p == g_dgame.self) {
            g_playerNode[p] = 0;
        } else {
            g_playerNode[p] = node;
            g_nodePlayer[node] = p;
            node++;
        }
    }
    const PhysPt dc = (PhysPt)g_seg * 16 + g_offBlock;
    for (int i = 0; i < DC_SIZE; i++) mem_writeb(dc + (PhysPt)i, 0);
    mem_writed(dc + DC_ID, kDoomcomId);
    mem_writew(dc + DC_INTNUM, kVector);
    mem_writew(dc + DC_NUMNODES, (Bit16u)g_dgame.players);
    mem_writew(dc + DC_TICDUP, (Bit16u)g_dgame.ticdup);
    mem_writew(dc + DC_EXTRATICS, (Bit16u)g_dgame.extratics);
    mem_writew(dc + DC_SAVEGAME, 0xFFFF);
    mem_writew(dc + DC_EPISODE, 1);
    mem_writew(dc + DC_MAP, 1);
    mem_writew(dc + DC_SKILL, 2);
    mem_writew(dc + DC_CONSOLEPLAYER, (Bit16u)g_dgame.self);
    mem_writew(dc + DC_NUMPLAYERS, (Bit16u)g_dgame.players);
    g_oldVec = RealGetVec(kVector);
    RealSetVec(kVector, CALLBACK_RealPointer(g_cbNet));
    g_sent = g_got = g_asked = g_tics = g_ticsLogged = 0;
    memset(g_ticsOf, 0, sizeof(g_ticsOf));
    for (int p = 0; p < kDoomMaxPlayers; p++) g_heardAt[p] = 0;
    g_waitLine.clear();
    g_startedAt = g_loggedAt = PIC_FullIndex();
    std::string who;
    for (int p = 0; p < g_dgame.players; p++) {
        who += std::string(p ? ", " : "") + g_dgame.names[p] + (p == g_dgame.self ? " (you) " : " ") + kColours[p];
    }
    say("%d players: %s.", g_dgame.players, who.c_str());
    char net[32];
    snprintf(net, sizeof(net), " -net %u", (unsigned)dc);
    write_exec(std::string(net) + (g_dgame.args.empty() ? "" : " " + g_dgame.args));
    g_dstage = ST_NET;
}

Bit16u launcher_poll(Bit16u key) {
    if (g_dstage != ST_WAIT) return g_dstage == ST_NET || g_dstage == ST_ALONE ? 0 : 2;
    const Bit8u ascii = (Bit8u)(key & 0xff);
    if (ascii == 27) g_esc = true;
    if (ascii == 13) g_enter = true;
    if (!doom_session_configured()) g_esc = true;
    if (!g_esc) {
        doom_session_waiting(true, g_gameId, g_args);
        doom_session_tick();
        bool alone = false;
        doom_session_start(g_enter, &alone);
        g_enter = false;
        if (doom_session_game(&g_dgame)) {
            run_net();
            return 1;
        }
        if (!alone) {
            const std::string line = doom_session_line();
            if (line != g_lastLine) {
                g_lastLine = line;
                if (!line.empty()) say("%s", line.c_str());
            }
            return 0;
        }
    }
    if (doom_session_configured()) say("DOOM by itself.");
    run_alone();
    return 1;
}

void launcher_ended(Bit16u error) {
    if (g_dstage == ST_NET) {
        if (!g_waitLine.empty() && incoming_text == g_waitLine) incoming_text.clear();
        g_waitLine.clear();
        RealSetVec(kVector, g_oldVec);
        doom_session_game_over();
        wclog(1, "DOOM: the network game is over: %u packets sent, %u got, %u tics made", (unsigned)g_sent, (unsigned)g_got, (unsigned)g_tics);
    }
    if (error) say("DOOMNET: DOS could not run %s (error %u).", g_program.c_str(), (unsigned)error);
    wclog(1, "DOOM: %s ended", g_program.c_str());
    game_log_text_screen();
    g_dstage = ST_OVER;
}

Bitu launcher_callback() {
    switch (reg_ax) {
    case 0: launcher_begin(); break;
    case 1: reg_ax = launcher_poll(reg_bx); break;
    case 2: launcher_ended(reg_bx); break;
    default: break;
    }
    return CBRET_NONE;
}

// A packet's last tic (the low byte DOOM sends) carried on from the count so far.
void count_tics(Bit32u *count, const Bit8u *data, int len) {
    if (len < 8 || (data[3] & 0xF0)) return;        // (a setup, an exit, a retransmit request or a kill: not moves)
    const int delta = (Bit8s)((Bit8u)(data[5] + data[7]) - (Bit8u)*count);
    if (delta > 0) *count += (Bit32u)delta;
}

// DOOM's network interrupt: the command in the block.
Bitu net_callback() {
    if (g_dstage != ST_NET) return CBRET_NONE;
    const PhysPt dc = (PhysPt)g_seg * 16 + g_offBlock;
    const Bit16u command = mem_readw(dc + DC_COMMAND);
    Bit8u data[kDoomPacketMax];
    if (command == CMD_SEND) {
        const int node = (Bit16s)mem_readw(dc + DC_REMOTENODE);
        const int len = (Bit16s)mem_readw(dc + DC_DATALENGTH);
        if (node < 1 || node >= g_dgame.players || len < 1 || len > kDoomPacketMax) return CBRET_NONE;
        for (int i = 0; i < len; i++) data[i] = mem_readb(dc + DC_DATA + (PhysPt)i);
        count_tics(&g_tics, data, len);
        doom_session_send(g_nodePlayer[node], data, len);
        g_sent++;
    } else if (command == CMD_GET) {
        int player = -1;
        const int len = doom_session_recv(&player, data, sizeof(data));
        g_asked++;
        if (len <= 0 || player < 0 || player >= g_dgame.players || player == g_dgame.self) {
            mem_writew(dc + DC_REMOTENODE, 0xFFFF);
            return CBRET_NONE;
        }
        for (int i = 0; i < len; i++) mem_writeb(dc + DC_DATA + (PhysPt)i, data[i]);
        mem_writew(dc + DC_DATALENGTH, (Bit16u)len);
        mem_writew(dc + DC_REMOTENODE, (Bit16u)g_playerNode[player]);
        count_tics(&g_ticsOf[player], data, len);
        if (!doom_session_gone(player)) g_heardAt[player] = PIC_FullIndex();
        g_got++;
    }
    return CBRET_NONE;
}

// DOOM's lockstep waits for every player's moves, silently and for as long as it takes: a player whose game has
// stopped answering (a browser tab put in the background, a machine gone to sleep) stops everybody's.  Said over the
// picture while it lasts, so that the others know whom they wait for; nothing is done about it (the game goes on as
// it was when that player is back; a player gone for good is told by its link's end: wcnet_doom_session.cpp).
void wait_notice() {
    const double now = PIC_FullIndex();
    if (now - g_waitCheckedAt < 250) return;
    g_waitCheckedAt = now;
    std::string who;
    for (int p = 0; p < g_dgame.players; p++) {
        if (p == g_dgame.self || g_heardAt[p] <= 0 || doom_session_gone(p) || now - g_heardAt[p] < 2500) continue;
        who += (who.empty() ? "" : ", ") + g_dgame.names[p];
    }
    const std::string line = who.empty() ? std::string() : "Waiting for " + who + ": their game has stopped answering";
    if (line == g_waitLine) return;
    if (!g_waitLine.empty() && incoming_text == g_waitLine) incoming_text.clear();
    g_waitLine = line;
    if (!line.empty()) {
        incoming_text = line;
        wclog(1, "DOOM: %s", line.c_str());
    } else {
        wclog(1, "DOOM: everybody's game answers again");
    }
}

}  // namespace

void doom_setup() {
    if (g_com) return;
    g_cbLauncher = CALLBACK_Allocate();
    CallBack_Handlers[g_cbLauncher] = &launcher_callback;
    g_cbNet = CALLBACK_Allocate();
    CALLBACK_Setup(g_cbNet, &net_callback, CB_IRET, "DOOM network driver");
    g_com = new Bit8u[sizeof(kDoomnetCom)];
    memcpy(g_com, kDoomnetCom, sizeof(kDoomnetCom));
    for (size_t i = 0; i < sizeof(kDoomnetCalls) / sizeof(kDoomnetCalls[0]); i++) {
        g_com[kDoomnetCalls[i] + 2] = (Bit8u)(g_cbLauncher & 0xff);
        g_com[kDoomnetCalls[i] + 3] = (Bit8u)(g_cbLauncher >> 8);
    }
    VFILE_Register("DOOMNET.COM", g_com, (Bit32u)sizeof(kDoomnetCom));
}

bool doom_launched() { return g_dstage != ST_NONE; }

// A rig's aid: "!doomtext" writes the text screen into the log (DOOM's start-up lines, the launcher's, an error's).
bool doom_script_item(const std::string &item, double t) {
    if (item != "!doomtext") return false;
    wclog(1, "key script %.1fs: DOOM: the text screen", t);
    game_log_text_screen();
    return true;
}

bool doom_running() { return g_dstage == ST_NET || g_dstage == ST_ALONE; }

void doom_tick() {
    if (g_dstage == ST_NONE) return;
    doom_session_tick();
    if (g_dstage != ST_NET) return;
    wait_notice();
    // how the game goes, every five seconds: its tics made (35 a second when nothing holds it) and each player's seen
    const double now = PIC_FullIndex();
    if (now - g_loggedAt < 5000) return;
    char others[96];
    int at = 0;
    others[0] = 0;
    for (int p = 0; p < g_dgame.players; p++) {
        if (p != g_dgame.self) at += snprintf(others + at, sizeof(others) - (size_t)at, " p%d %u", p + 1, (unsigned)g_ticsOf[p]);
    }
    wclog(1, "DOOM: %.1fs: tics %u (%.1f a second), seen%s; packets sent %u, got %u of %u asked",
          (now - g_startedAt) / 1000.0, (unsigned)g_tics, (g_tics - g_ticsLogged) * 1000.0 / (now - g_loggedAt), others,
          (unsigned)g_sent, (unsigned)g_got, (unsigned)g_asked);
    g_loggedAt = now;
    g_ticsLogged = g_tics;
}

}  // namespace wc

void wc_net_dos_programs() {
    wc::doom_setup();
}
