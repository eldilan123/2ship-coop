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

void RegisterBossHooks() {
    auto* gi = GameInteractor::Instance;
    gi->RegisterGameHook<GameInteractor::ShouldItemGive>(OnShouldItemGive);
}

} // namespace

void Frame() {
}

static RegisterShipInitFunc initFunc(RegisterBossHooks);

} // namespace Coop::Bosses
