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
#include "wc_net.h"
#include "wcnet_session.h"
#include "wcnet_events.h"
#include "wcnet_vm.h"
#include "wcnet_memory.h"
#include "wcnet_code.h"
#include "wcnet_log.h"
#include "cpu.h"
#include "regs.h"
#include "keyboard.h"
#include "pic.h"
#include <time.h>

extern std::string incoming_text;

namespace wc {

// The instructions the main loop uses to test dseg:00AE; we look at the
// status right before each so a client can hold its own mission open.
static const code::RootLoc kStatusCheckAfterKeys = { code::SEG001, 0x20F4, "main_loop+AB: cmp missionStatus after handle_key" };
static const code::RootLoc kStatusCheckAfterFrame = { code::SEG001, 0x2108, "main_loop+BF: cmp missionStatus after the frame" };
static const code::RootLoc kStatusSetByExitKey = { code::SEG001, 0x20F2, "main_loop+A9: after missionStatus = EndExit" };

static bool g_watch[0x10000];
static bool g_watchReady = false;
static int g_asyncCounter = 0;
static bool g_skipBarracks = false;

static void watch(Bit16u ip) { g_watch[ip] = true; }

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
        bool flying = g_session && g_session->in_space();
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

static void build_watch_list() {
    memset(g_watch, 0, sizeof(g_watch));
    watch(code::briefingStarted.ovrOff);
    watch(code::missionStarting.ovrOff);
    watch(code::missionVictoryCalc.ovrOff);
    watch(code::missionEnded.ovrOff);
    watch(code::simulatorStart.ovrOff);
    watch(code::simulatorEnd.ovrOff);
    watch(code::do_damage.ovrOff);
    watch(code::fireGunFromShip.ovrOff);
    watch(code::delayedDespawn.ovrOff);
    watch(code::outerSpawnShipEntity.ovrOff);
    watch(code::despawn.ovrOff);
    watch(code::aiShipThink.ovrOff);
    watch(code::aiSetSpeed.ovrOff);
    watch(code::enterBarracks.ovrOff);
    watch(code::autoAnimationBody.ovrOff);
    watch(code::autopilotFinished.ovrOff);
    watch(code::skipOrchestra.off);
    watch(code::afterStartup.off);
    watch(code::autopilotKey.off);
    watch(code::mainLoopTop.off);
    watch(kStatusCheckAfterKeys.off);
    watch(kStatusCheckAfterFrame.off);
    watch(kStatusSetByExitKey.off);
    watch(Trampoline::hook_ip());
    g_watchReady = true;
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
        run_campaign(mission, series);
        g_skipBarracks = true;
    }
}

static void on_barracks() {
    ensure_session();
    if (g_session) {
        g_session->on_barracks();
    }
    if (g_skipBarracks) {
        g_skipBarracks = false;
        reg_eax = 7;
        reg_eip = 0x1391;  // ovr150: return from enterBarracks
    }
}

static void check_hooks_slow() {
    if (at_location(code::briefingStarted)) {
        wclog(2, "briefing animation starting for mission %d/%d", rd8(ds::currentMission), rd8(ds::currentSeries));
    }
    if (at_location(code::missionStarting)) {
        ensure_session();
        if (g_session) {
            g_session->on_mission_starting(rd8(ds::currentMission), rd8(ds::currentSeries));
        }
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
    if (g_session && (at_location(kStatusCheckAfterKeys) || at_location(kStatusCheckAfterFrame) ||
                      at_location(kStatusSetByExitKey))) {
        g_session->check_mission_status();
    }
    if (at_location(code::missionEnded) && g_session) {
        g_session->on_mission_ended();
    }
    if (at_location(code::autopilotKey) && g_session && g_session->is_client()) {
        reg_eip += 5;  // only the server may engage autopilot
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
        maybe_dump_data_segment();
        ensure_session();
        if (g_session) {
            g_session->on_frame_top();
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
// One byte of the game's data segment, for checks from the page.
extern "C" EMSCRIPTEN_KEEPALIVE int wc_web_ds_byte(int off) {
    return (off < 0 || off > 0xffff) ? -1 : (int)wc::rd8((Bit16u)off);
}
#endif

void wc_net_check_cpu_hooks() {
    using namespace wc;
    if (++g_asyncCounter == 1000) {
        g_asyncCounter = 0;
        auto_keys();
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
