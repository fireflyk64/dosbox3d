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
#include "wc_net.h"
#include "wcnet_session.h"
#include "wcnet_events.h"
#include "wcnet_vm.h"
#include "wcnet_memory.h"
#include "wcnet_code.h"
#include "wcnet_log.h"
#include "cpu.h"
#include "regs.h"
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

// (Re)connect when nothing is connected, at most every few seconds so a
// missing server does not stall every frame with a connect() attempt.
static void ensure_session() {
    static time_t lastAttempt = 0;
    if (g_session) {
        return;
    }
    time_t now = time(NULL);
    if (now - lastAttempt < 5) {
        return;
    }
    lastAttempt = now;
    init_network();
}

static void on_after_startup() {
    has_started_up = true;
    const char *misenv = getenv("MIS");
    const char *serenv = getenv("SERIES");
    int mission = atoi(misenv ? misenv : "0");
    int series = atoi(serenv ? serenv : "1");
    if (mission != 0 || series != 1) {
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
        ensure_session();
        if (g_session) {
            g_session->on_frame_top();
        }
    }
}

}  // namespace wc

void wc_net_check_cpu_hooks() {
    using namespace wc;
    if (++g_asyncCounter == 1000) {
        g_asyncCounter = 0;
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
