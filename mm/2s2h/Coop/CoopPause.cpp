#include "CoopPause.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "Coop.h"
#include "CoopPlayerSync.h"
#include "CoopSession.h"
#include "CoopTargeting.h"
#include "2s2h/BenGui/Notification.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace Coop::Pause {

namespace {

using SteadyClock = std::chrono::steady_clock;

// The clock waits this long before 6:00 and 18:00 while the host's menu is open.
constexpr u16 kDayChangeMargin = 2 * CLOCK_TIME_MINUTE;

struct HostMenu {
    bool open = false;
    SteadyClock::time_point since;
    u16 timeAtOpen = 0;
    int dayAtOpen = 0;
    uint32_t droppedDamage = 0;
    uint32_t skippedKnockbacks = 0;
};
HostMenu sMenu;
// Remote players last seen paused (for the notice when one opens the menu).
std::unordered_map<uint32_t, bool> sRemotePaused;

void Notify(const std::string& message) {
    Notification::Emit({ .message = message });
}

// The clock is about to reach 6:00 or 18:00 (En_Test4 starts the dawn and the night when it does).
bool NearDayChange() {
    const u16 time = CURRENT_TIME;
    return (time >= CLOCK_TIME(6, 0) - kDayChangeMargin && time < CLOCK_TIME(6, 0)) ||
           (time >= CLOCK_TIME(18, 0) - kDayChangeMargin && time < CLOCK_TIME(18, 0));
}

void OnShouldActorUpdate(Actor* actor, bool* should) {
    if (!HostWorldRuns()) {
        return;
    }
    Player* link = GET_PLAYER(gPlayState);
    if (actor == &link->actor) {
        // A knockback an enemy asked for (func_800B8D10) would be taken when the menu closes.
        if (link->unk_B75 != 0) {
            sMenu.skippedKnockbacks++;
        }
        link->unk_B74 = 0;
        link->unk_B75 = 0;
        *should = false;
    } else if (actor == link->rideActor || actor->id == ACTOR_EN_TEST4 || Targeting::StaysOnHost(actor)) {
        *should = false;
    }
}

void HostFrame() {
    const bool runs = HostWorldRuns();
    if (runs && !sMenu.open) {
        sMenu = {};
        sMenu.open = true;
        sMenu.since = SteadyClock::now();
        sMenu.timeAtOpen = CURRENT_TIME;
        sMenu.dayAtOpen = gSaveContext.save.day;
        SPDLOG_INFO("[Coop] pause: the host opened the menu; its world keeps running (time 0x{:04X}, day {})",
                    CURRENT_TIME, gSaveContext.save.day);
    } else if (!runs && sMenu.open) {
        sMenu.open = false;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - sMenu.since).count();
        SPDLOG_INFO("[Coop] pause: the host closed the menu after {} ms; the clock went 0x{:04X} -> 0x{:04X} (day {} "
                    "-> {}); direct damage dropped {}, knockbacks dropped {}",
                    ms, sMenu.timeAtOpen, CURRENT_TIME, sMenu.dayAtOpen, gSaveContext.save.day, sMenu.droppedDamage,
                    sMenu.skippedKnockbacks);
    }
}

void NoticeRemotePauses() {
    const SessionSnapshot snapshot = GetSnapshot();
    for (const PlayerInfo& info : snapshot.players) {
        if (info.id == snapshot.localId) {
            continue;
        }
        PlayerSync::State s;
        uint32_t age = 0;
        const bool paused = PlayerSync::LatestState(info.id, s, &age) && age < 1000 && s.paused != 0;
        bool& was = sRemotePaused[info.id];
        if (paused != was) {
            was = paused;
            SPDLOG_INFO("[Coop] pause: player {} {} the menu", info.id, paused ? "opened" : "closed");
            if (paused) {
                Notify(info.id == kHostPlayerId ? "Coop: el host esta en el menu de pausa"
                                                : "Coop: " + info.name + " esta en el menu de pausa");
            }
        }
    }
}

void RegisterPauseHooks() {
    REGISTER_VB_SHOULD(VB_PAUSE_STOPS_WORLD, {
        if (HostWorldRuns()) {
            *should = false;
        }
    });
    REGISTER_VB_SHOULD(VB_ADVANCE_TIME, {
        if (HostWorldRuns() && NearDayChange()) {
            *should = false;
        }
    });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::ShouldActorUpdate>(OnShouldActorUpdate);
}

} // namespace

bool HostWorldRuns() {
    PlayState* play = gPlayState;
    return play != nullptr && play->pauseCtx.state != PAUSE_STATE_OFF && IsHosting() &&
           gSaveContext.gameMode == GAMEMODE_NORMAL && GetSnapshot().players.size() > 1 &&
           CVarGetInteger(CVAR_COOP_PAUSE_KEEPS_WORLD, 1) != 0;
}

void NoteDroppedDamage() {
    sMenu.droppedDamage++;
}

void Frame() {
    if (gPlayState == nullptr || (!IsHosting() && !IsClientInSession())) {
        sMenu = {};
        sRemotePaused.clear();
        return;
    }
    if (IsHosting()) {
        HostFrame();
    }
    NoticeRemotePauses();
}

static RegisterShipInitFunc initFunc(RegisterPauseHooks);

} // namespace Coop::Pause
