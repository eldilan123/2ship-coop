// Host: enemies get more health with more players (milestone 5). Applied once, right after the enemy's Init, to
// enemies created while two or more players are in the session (those already alive when someone joins keep theirs).
// x(1 + (gCoop.EnemyHealthMult - 1) x (players - 1)), rounded up: x1.5 with two players by default.
//
// Not scaled (x1): enemies with 2 health or less (a Keese must still die in one hit), and types whose code compares
// their health with fixed numbers or sets it back to fixed numbers (phases, revivals): scaling those would break
// their fights. If a scaled enemy's health goes up later (it revives), the new value is scaled too.
#include "CoopEnemyHealth.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopEnemyTypes.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "variables.h"
}

namespace Coop::EnemyHealth {

namespace {

// Types whose health is tied to fixed numbers in their code.
const std::unordered_map<int16_t, const char*> kThresholds = {
    { ACTOR_EN_IK, "set back to 9 when its armor breaks" },
    { ACTOR_EN_WIZ, "second phase set back to 8 or 6" },
    { ACTOR_EN_DODONGO, "loses a fixed 4 per bomb eaten (health <= 4)" },
    { ACTOR_EN_TALK_GIBUD, "revives with 8" },
    { ACTOR_EN_FLOORMAS, "splits and merges back to its base health" },
    { ACTOR_EN_KNIGHT, "fixed health per phase" },
    { ACTOR_EN_BB, "revives with its base health" },
    { ACTOR_EN_BBFALL, "revives with its base health" },
    { ACTOR_EN_SLIME, "revives with its base health" },
};

struct Scaled {
    float mult;
    int16_t last;
};
std::unordered_map<Actor*, Scaled> sScaled;

float Multiplier(size_t players) {
    const float perPlayer = std::clamp(CVarGetFloat(CVAR_COOP_ENEMY_HEALTH_MULT, 1.5f), 1.0f, 4.0f);
    return 1.0f + (perPlayer - 1.0f) * static_cast<float>(players - 1);
}

int16_t Scale(int16_t health, float mult) {
    return static_cast<int16_t>(std::min(255.0f, std::ceil(health * mult - 0.0001f)));
}

void OnActorInit(Actor* actor) {
    if (!IsHosting() || gPlayState == nullptr || gSaveContext.gameMode != GAMEMODE_NORMAL ||
        actor->category != ACTORCAT_ENEMY || actor->update == nullptr) {
        return;
    }
    const size_t players = GetSnapshot().players.size();
    if (players < 2) {
        if (GetLaunchOptions().testEnemyLog) {
            SPDLOG_INFO("[Coop] enemy-health: {} params 0x{:04X} {} x1 (1 player)", Enemies::TypeName(actor->id),
                        static_cast<uint16_t>(actor->params), actor->colChkInfo.health);
        }
        return;
    }
    const int16_t base = actor->colChkInfo.health;
    const float mult = Multiplier(players);
    auto threshold = kThresholds.find(actor->id);
    if (threshold != kThresholds.end()) {
        SPDLOG_INFO("[Coop] enemy-health: {} params 0x{:04X} {} x1 (thresholds: {})", Enemies::TypeName(actor->id),
                    static_cast<uint16_t>(actor->params), base, threshold->second);
        return;
    }
    if (base <= 2) {
        SPDLOG_INFO("[Coop] enemy-health: {} params 0x{:04X} {} x1 (2 or less)", Enemies::TypeName(actor->id),
                    static_cast<uint16_t>(actor->params), base);
        return;
    }
    const int16_t scaled = Scale(base, mult);
    actor->colChkInfo.health = static_cast<u8>(scaled);
    sScaled[actor] = { mult, scaled };
    SPDLOG_INFO("[Coop] enemy-health: {} params 0x{:04X} {} -> {} (x{:.2f}, {} players)", Enemies::TypeName(actor->id),
                static_cast<uint16_t>(actor->params), base, scaled, mult, players);
}

void OnActorDestroy(Actor* actor) {
    sScaled.erase(actor);
}

void OnPlayDestroy() {
    sScaled.clear();
}

void RegisterEnemyHealthHooks() {
    auto* gi = GameInteractor::Instance;
    gi->RegisterGameHook<GameInteractor::OnActorInit>(OnActorInit);
    gi->RegisterGameHook<GameInteractor::OnActorDestroy>(OnActorDestroy);
    gi->RegisterGameHook<GameInteractor::OnPlayDestroy>(OnPlayDestroy);
}

} // namespace

void Frame() {
    if (!IsHosting()) {
        sScaled.clear();
        return;
    }
    // A scaled enemy whose health went up (it revived): the new value is scaled as well.
    for (auto& [actor, s] : sScaled) {
        if (actor->update == nullptr) {
            continue;
        }
        const int16_t health = actor->colChkInfo.health;
        if (health > s.last) {
            const int16_t scaled = Scale(health, s.mult);
            actor->colChkInfo.health = static_cast<u8>(scaled);
            SPDLOG_INFO("[Coop] enemy-health: {} went up to {} -> {} (x{:.2f})", Enemies::TypeName(actor->id), health,
                        scaled, s.mult);
            s.last = scaled;
        } else {
            s.last = health;
        }
    }
}

static RegisterShipInitFunc initFunc(RegisterEnemyHealthHooks);

} // namespace Coop::EnemyHealth
