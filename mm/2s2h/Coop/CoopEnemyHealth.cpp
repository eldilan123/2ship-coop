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
#include <cstddef>
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
#include "overlays/actors/ovl_En_Ik/z_en_ik.h"
#include "overlays/actors/ovl_En_Wiz/z_en_wiz.h"
}

namespace Coop::EnemyHealth {

namespace {

// Types whose health is tied to fixed numbers in their code.
const std::unordered_map<int16_t, const char*> kThresholds = {
    { ACTOR_EN_DODONGO, "loses a fixed 4 per bomb eaten (health <= 4)" },
    { ACTOR_EN_KNIGHT, "fixed health per phase" },
    { ACTOR_EN_FLOORMAS, "splits into three with 1 each; the rule for rises would scale only one of them" },
    { ACTOR_EN_BB, "revives with its base health" },
    { ACTOR_EN_BBFALL, "revives with its base health" },
    { ACTOR_EN_SLIME, "revives with its base health" },
};

// Types whose second phase starts with a fixed health: when the field at `offset` leaves 0 (Iron Knuckle: its armor
// broke; Wizrobe: first phase over) the new health is scaled once. Their health is not otherwise watched: a single
// strong hit could take them from above that value to it, and that is not a revival.
struct PhaseField {
    ptrdiff_t offset;
    size_t size;
};
const std::unordered_map<int16_t, PhaseField> kPhaseFields = {
    { ACTOR_EN_IK, { offsetof(EnIk, drawArmorFlags), sizeof(u8) } },
    { ACTOR_EN_WIZ, { offsetof(EnWiz, fightState), sizeof(s16) } },
};

int32_t ReadField(const Actor* actor, const PhaseField& f) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(actor) + f.offset;
    return f.size == 1 ? *p : *reinterpret_cast<const s16*>(p);
}

struct Scaled {
    float mult;
    int16_t last;
    const PhaseField* phase = nullptr;
    int32_t lastPhase = 0;
};
// --coop-test-enemy-log: every change of an enemy's health on the host, with or without clients.
std::unordered_map<Actor*, int16_t> sLoggedHealth;
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
    Scaled entry = { mult, scaled };
    auto phase = kPhaseFields.find(actor->id);
    if (phase != kPhaseFields.end()) {
        entry.phase = &phase->second;
        entry.lastPhase = ReadField(actor, phase->second);
    }
    sScaled[actor] = entry;
    SPDLOG_INFO("[Coop] enemy-health: {} params 0x{:04X} {} -> {} (x{:.2f}, {} players)", Enemies::TypeName(actor->id),
                static_cast<uint16_t>(actor->params), base, scaled, mult, players);
}

void OnActorDestroy(Actor* actor) {
    sScaled.erase(actor);
    sLoggedHealth.erase(actor);
}

void OnPlayDestroy() {
    sScaled.clear();
    sLoggedHealth.clear();
}

void LogHealthChanges() {
    if (!GetLaunchOptions().testEnemyLog || gPlayState == nullptr || gSaveContext.gameMode != GAMEMODE_NORMAL) {
        return;
    }
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].first; actor != nullptr; actor = actor->next) {
        if (actor->update == nullptr || actor->init != nullptr) {
            continue;
        }
        const int16_t health = actor->colChkInfo.health;
        auto it = sLoggedHealth.find(actor);
        if (it != sLoggedHealth.end() && it->second != health) {
            SPDLOG_INFO("[Coop] enemy-health-change: {} {} -> {}", Enemies::TypeName(actor->id), it->second, health);
        }
        sLoggedHealth[actor] = health;
    }
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
        sLoggedHealth.clear();
        return;
    }
    LogHealthChanges();
    // A scaled enemy whose health went up (it revived), or whose second phase started: the new value is scaled too.
    for (auto& [actor, s] : sScaled) {
        if (actor->update == nullptr) {
            continue;
        }
        const int16_t health = actor->colChkInfo.health;
        if (s.phase != nullptr) {
            const int32_t phase = ReadField(actor, *s.phase);
            if (s.lastPhase == 0 && phase != 0) {
                const int16_t scaled = Scale(health, s.mult);
                actor->colChkInfo.health = static_cast<u8>(scaled);
                SPDLOG_INFO("[Coop] enemy-health: {} second phase starts with {} -> {} (x{:.2f})",
                            Enemies::TypeName(actor->id), health, scaled, s.mult);
            }
            s.lastPhase = phase;
            continue;
        }
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
