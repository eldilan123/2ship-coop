#include "CoopBosses.h"

#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopSession.h"
#include "2s2h/BenGui/Notification.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace Coop::Bosses {

namespace {

int sRewardsBlocked = 0;
int sMajoraRemoved = 0;
int16_t sMajoraNoticeScene = -1;

bool ClientPlaying() {
    return !IsHosting() && IsClientInSession() && gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

bool BossReward(u8 item) {
    return item == ITEM_HEART_CONTAINER || (item >= ITEM_REMAINS_ODOLWA && item <= ITEM_REMAINS_TWINMOLD);
}

void OnShouldItemGive(u8 item, bool* should) {
    if (!*should || !ClientPlaying() || !BossReward(item)) {
        return;
    }
    *should = false;
    sRewardsBlocked++;
    SPDLOG_INFO("[Coop] boss-reward-blocked: item 0x{:02X} not given by the client's game (only the host's boss "
                "gives it; it arrives with the shared inventory); {} so far",
                item, sRewardsBlocked);
}

void OnActorInit(Actor* actor) {
    if (actor->id != ACTOR_BOSS_07 || actor->update == nullptr || IsHosting() || !IsClientInSession()) {
        return;
    }
    sMajoraRemoved++;
    SPDLOG_INFO("[Coop] majora-removed: Boss_07 params 0x{:04X} removed on the client (Majora is the host's for now)",
                static_cast<uint16_t>(actor->params));
    Actor_Kill(actor);
    if (gPlayState != nullptr && sMajoraNoticeScene != gPlayState->sceneId) {
        sMajoraNoticeScene = gPlayState->sceneId;
        Notification::Emit({ .message = "Coop: Majora todavia lo pelea solo el host" });
    }
}

void RegisterBossHooks() {
    auto* gi = GameInteractor::Instance;
    gi->RegisterGameHook<GameInteractor::ShouldItemGive>(OnShouldItemGive);
    gi->RegisterGameHook<GameInteractor::OnActorInit>(OnActorInit);
}

} // namespace

void Frame() {
    if (!IsHosting() && !IsClientInSession()) {
        sMajoraNoticeScene = -1;
    }
}

static RegisterShipInitFunc initFunc(RegisterBossHooks);

} // namespace Coop::Bosses
