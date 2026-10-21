#include "CoopTargeting.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopEnemies.h"
#include "CoopEnemyTypes.h"
#include "CoopPlayerHits.h"
#include "CoopPlayerSync.h"
#include "CoopPuppet.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace Coop::Targeting {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr float kSwitchMargin = 150.0f;  // A new target must be this much nearer
constexpr uint32_t kSwitchHoldMs = 1000; // ...and the last switch this long ago
constexpr uint32_t kFreshStateMs = 500;  // A remote player whose newest state is older is not a target
constexpr uint32_t kStatsIntervalMs = 10000;
constexpr float kSampleRange = 1500.0f; // Test log: enemies this close to some player

const SteadyClock::time_point sEpoch = SteadyClock::now();

uint32_t NowMs() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - sEpoch).count());
}

// Enemy types that stay on the host in this milestone, and why.
const std::unordered_map<int16_t, const char*> kHostOnly = {
    { ACTOR_EN_RR, "grabs (and eats the shield)" },
    { ACTOR_EN_FLOORMAS, "grabs" },
    { ACTOR_EN_WALLMAS, "grabs (cutscene)" },
    { ACTOR_EN_RD, "grabs and paralyzes" },
    { ACTOR_EN_RAILGIBUD, "grabs and paralyzes" },
    { ACTOR_EN_TALK_GIBUD, "grabs, paralyzes and takes items" },
    { ACTOR_EN_BIGOKUTA, "grabs" },
    { ACTOR_EN_RAF, "grabs" },
    { ACTOR_EN_WDHAND, "grabs" },
    { ACTOR_EN_KNIGHT, "grabs (cutscene)" },
    { ACTOR_EN_BIGSLIME, "grabs" },
    { ACTOR_EN_DRAGON, "grabs" },
    { ACTOR_EN_JSO, "paralyzes (intro cutscene)" },
    { ACTOR_EN_THIEFBIRD, "steals items" },
    { ACTOR_EN_BIGPO, "intro cutscene" },
    { ACTOR_EN_DEATH, "intro cutscene" },
    { ACTOR_EN_DINOFOS, "intro cutscene" },
    { ACTOR_EN_IK, "intro cutscene" },
    { ACTOR_EN_JSO2, "intro cutscene" },
    { ACTOR_EN_KAIZOKU, "intro cutscene" },
    { ACTOR_EN_WIZ, "intro cutscene" },
    { ACTOR_EN_CROW, "keeps a pointer to the player" },
    { ACTOR_EN_MINIDEATH, "keeps a pointer to the player" },
};

struct TargetState {
    uint32_t playerId = 0; // 0: not chosen yet
    uint32_t switchedMs = 0;
    uint32_t switches = 0;
    bool hostOnlyLogged = false;
};
std::unordered_map<const Actor*, TargetState> sTargets;

struct Candidate {
    uint32_t playerId;
    Player* player;
};

struct Stats {
    uint32_t switches = 0;
    uint32_t windows = 0;
    uint32_t restores = 0; // Safety nets that had to put the player list back (must stay 0)
    uint32_t maxSwitchesOneEnemy = 0;
    uint32_t awake = 0; // Enemy updates that ran only because a remote player was near (KeepAwakeNearRemotes)
};
Stats sStats;
uint32_t sStatsMs = 0;
uint32_t sLastSampleSecond = 0;

// MARK: - The player view

struct View {
    Actor* enemy = nullptr; // Non-null while an enemy's update sees a puppet as "the player"
    Actor* savedHead = nullptr;
    Player* puppet = nullptr;
    uint32_t playerId = 0;
    // Puppet fields set only for the window (its own draw must not see them)
    s8 meleeWeaponState = 0;
    s8 meleeWeaponAnimation = 0;
    s8 invincibilityTimer = 0;
    u8 csAction = 0;
    u8 unk_D57 = 0;
    Actor* focusActor = nullptr;
};
View sView;

void EndView() {
    PlayState* play = gPlayState;
    if (play != nullptr) {
        play->actorCtx.actorLists[ACTORCAT_PLAYER].first = sView.savedHead;
    }
    Player* p = sView.puppet;
    p->meleeWeaponState = sView.meleeWeaponState;
    p->meleeWeaponAnimation = sView.meleeWeaponAnimation;
    p->invincibilityTimer = sView.invincibilityTimer;
    p->csAction = sView.csAction;
    p->unk_D57 = sView.unk_D57;
    p->focusActor = sView.focusActor;
    // A knockback the enemy asked for during its update (func_800B8D10 writes these on "the player"): the puppet
    // never takes it, its player does.
    if (p->unk_B75 != 0 && sView.enemy != nullptr) {
        PlayerHits::HostDirectKnockback(sView.playerId, sView.enemy, p->unk_B75, p->unk_B74, p->unk_B76, p->unk_B78,
                                        p->unk_B7C);
    }
    p->unk_B74 = 0;
    p->unk_B75 = 0;
    sView = {};
}

// A safety net found the view still open: put the list back and count it.
void RestoreLeftover(const char* where) {
    if (sView.enemy == nullptr) {
        return;
    }
    sStats.restores++;
    SPDLOG_WARN("[Coop] view-restore: the player list still pointed at a puppet after {} ({}); restored", where,
                Enemies::TypeName(sView.enemy->id));
    EndView();
}

void BeginView(Actor* enemy, Player* puppet, uint32_t playerId, const PlayerSync::State& s) {
    PlayState* play = gPlayState;
    sView.enemy = enemy;
    sView.playerId = playerId;
    puppet->unk_B74 = 0;
    puppet->unk_B75 = 0;
    sView.savedHead = play->actorCtx.actorLists[ACTORCAT_PLAYER].first;
    sView.puppet = puppet;
    sView.meleeWeaponState = puppet->meleeWeaponState;
    sView.meleeWeaponAnimation = puppet->meleeWeaponAnimation;
    sView.invincibilityTimer = puppet->invincibilityTimer;
    sView.csAction = puppet->csAction;
    sView.unk_D57 = puppet->unk_D57;
    sView.focusActor = puppet->focusActor;
    puppet->meleeWeaponState = s.meleeWeaponState;
    puppet->meleeWeaponAnimation = s.meleeWeaponAnimation;
    puppet->invincibilityTimer = s.invincibilityTimer;
    puppet->csAction = s.csAction;
    puppet->unk_D57 = s.unk_D57;
    puppet->focusActor = s.focusNetId != 0 ? Enemies::HostEnemyActor(s.focusNetId) : nullptr;
    play->actorCtx.actorLists[ACTORCAT_PLAYER].first = &puppet->actor;
    sStats.windows++;
}

// Damage an enemy deals with damagePlayer during its update: while it sees a puppet, its player gets it, not the
// local Link (Player_InflictDamage changes the local health).
s32 (*sOriginalDamagePlayer)(PlayState*, s32) = nullptr;

s32 DamagePlayer(PlayState* play, s32 damage) {
    if (sView.enemy != nullptr) {
        PlayerHits::HostDirectDamage(sView.playerId, sView.enemy, damage);
        return true;
    }
    return sOriginalDamagePlayer != nullptr ? sOriginalDamagePlayer(play, damage) : false;
}

void WrapDamagePlayer() {
    PlayState* play = gPlayState;
    if (play == nullptr || play->damagePlayer == nullptr || play->damagePlayer == DamagePlayer) {
        return;
    }
    sOriginalDamagePlayer = play->damagePlayer; // Set again by every Player_Init of the local Link
    play->damagePlayer = DamagePlayer;
}

// Enemies whose AT hit a puppet this frame (CoopPlayerHits): their next update deals with that player.
std::unordered_map<const Actor*, uint32_t> sVictims;
// Enemies drawn this frame only because a remote player is near (KeepAwakeNearRemotes).
std::vector<Actor*> sForcedDraw;

// MARK: - Choosing

Player* LocalLink() {
    // The real head of the list, even if a view were (wrongly) still open.
    PlayState* play = gPlayState;
    Actor* head = sView.enemy != nullptr ? sView.savedHead : play->actorCtx.actorLists[ACTORCAT_PLAYER].first;
    return reinterpret_cast<Player*>(head);
}

bool RoomLoaded(int8_t room) {
    const RoomContext& rooms = gPlayState->roomCtx;
    return room == rooms.curRoom.num || room == rooms.prevRoom.num;
}

std::vector<Candidate> Candidates() {
    std::vector<Candidate> out;
    Player* local = LocalLink();
    if (local != nullptr && !(local->stateFlags1 & PLAYER_STATE1_DEAD) && local->csAction == PLAYER_CSACTION_NONE) {
        out.push_back({ kHostPlayerId, local });
    }
    for (const PlayerInfo& info : GetSnapshot().players) {
        if (info.id == kHostPlayerId) {
            continue;
        }
        Player* puppet = Puppet::PuppetOf(info.id);
        PlayerSync::State s;
        uint32_t age = 0;
        if (puppet == nullptr || puppet->actor.update == nullptr || !PlayerSync::LatestState(info.id, s, &age) ||
            age > kFreshStateMs || !s.inGameplay || s.sceneId != gPlayState->sceneId ||
            s.sceneLayer != static_cast<int8_t>(gSaveContext.sceneLayer) || (s.stateFlags1 & PLAYER_STATE1_DEAD) ||
            s.csAction != PLAYER_CSACTION_NONE || !RoomLoaded(s.roomNum)) {
            continue;
        }
        out.push_back({ info.id, puppet });
    }
    return out;
}

std::string Distances(Actor* enemy, const std::vector<Candidate>& candidates) {
    std::string out;
    for (const Candidate& c : candidates) {
        out += fmt::format("{}player {} {:.0f}", out.empty() ? "" : ", ", c.playerId,
                           Actor_WorldDistXYZToActor(enemy, &c.player->actor));
    }
    return out.empty() ? "no valid player" : out;
}

void Choose(Actor* enemy, TargetState& t, const std::vector<Candidate>& candidates, uint32_t now) {
    const Candidate* current = nullptr;
    const Candidate* best = nullptr;
    float currentDist = 0.0f, bestDist = 0.0f;
    for (const Candidate& c : candidates) {
        const float d = Actor_WorldDistXYZToActor(enemy, &c.player->actor);
        if (c.playerId == t.playerId) {
            current = &c;
            currentDist = d;
        }
        if (best == nullptr || d < bestDist) {
            best = &c;
            bestDist = d;
        }
    }
    if (best == nullptr) {
        // Nobody valid (the host dead or in a cutscene, nobody else here): the host, as without coop.
        if (t.playerId != kHostPlayerId) {
            t.playerId = kHostPlayerId;
            t.switchedMs = now;
        }
        return;
    }
    const char* reason = nullptr;
    if (t.playerId == 0) {
        reason = "first choice";
    } else if (current == nullptr) {
        reason = "its target is no longer valid";
    } else if (best != current && bestDist < currentDist - kSwitchMargin && now - t.switchedMs >= kSwitchHoldMs) {
        reason = "nearer";
    }
    if (reason == nullptr || best->playerId == t.playerId) {
        if (t.playerId == 0) {
            t.playerId = best->playerId;
        }
        return;
    }
    const bool first = t.playerId == 0;
    t.playerId = best->playerId;
    t.switchedMs = now;
    if (!first) {
        t.switches++;
        sStats.switches++;
        sStats.maxSwitchesOneEnemy = std::max(sStats.maxSwitchesOneEnemy, t.switches);
    }
    SPDLOG_INFO("[Coop] enemy-target: net {} {} -> player {} ({}; {})", Enemies::HostNetIdOf(enemy),
                Enemies::TypeName(enemy->id), t.playerId, reason, Distances(enemy, candidates));
}

bool Active() {
    return IsHosting() && gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL &&
           GetSnapshot().players.size() > 1;
}

// MARK: - Hooks

void OnShouldActorUpdate(Actor* actor, bool*) {
    if (sView.enemy != nullptr) {
        RestoreLeftover("the next actor's update");
    }
    if (actor->category != ACTORCAT_ENEMY || !Active()) {
        return;
    }
    TargetState& t = sTargets[actor];
    auto hostOnly = kHostOnly.find(actor->id);
    if (hostOnly != kHostOnly.end()) {
        if (!t.hostOnlyLogged) {
            t.hostOnlyLogged = true;
            t.playerId = kHostPlayerId;
            SPDLOG_INFO("[Coop] enemy-target: net {} {} stays on the host (sin soporte para B: {})",
                        Enemies::HostNetIdOf(actor), Enemies::TypeName(actor->id), hostOnly->second);
        }
        return;
    }
    const std::vector<Candidate> candidates = Candidates();
    Choose(actor, t, candidates, NowMs());
    // The player its attack just hit, for this update (its recoil, a follow-up knockback), whoever its target is.
    uint32_t viewId = t.playerId;
    auto victim = sVictims.find(actor);
    if (victim != sVictims.end()) {
        viewId = victim->second;
        sVictims.erase(victim);
    }
    if (viewId == kHostPlayerId) {
        return; // The engine already measured everything against the host's Link
    }
    Player* puppet = Puppet::PuppetOf(viewId);
    PlayerSync::State s;
    if (puppet == nullptr || !PlayerSync::LatestState(viewId, s)) {
        return;
    }
    // What Actor_UpdateActor worked out against the local Link, now against the target.
    actor->xzDistToPlayer = Actor_WorldDistXZToActor(actor, &puppet->actor);
    actor->playerHeightRel = Actor_HeightDiff(actor, &puppet->actor);
    actor->xyzDistToPlayerSq = SQ(actor->xzDistToPlayer) + SQ(actor->playerHeightRel);
    actor->yawTowardsPlayer = Actor_WorldYawTowardActor(actor, &puppet->actor);
    actor->isLockedOn = s.focusNetId != 0 && Enemies::HostNetIdOf(actor) == s.focusNetId;
    BeginView(actor, puppet, viewId, s);
}

void OnActorUpdate(Actor* actor) {
    if (sView.enemy == actor) {
        EndView(); // The normal end of the window
    }
}

void OnPlayDrawWorldStart() {
    RestoreLeftover("the frame's updates (before the draw)");
}

void OnActorDestroy(Actor* actor) {
    sTargets.erase(actor);
    sForcedDraw.erase(std::remove(sForcedDraw.begin(), sForcedDraw.end(), actor), sForcedDraw.end());
    sVictims.erase(actor);
}

void OnPlayDestroy() {
    RestoreLeftover("the scene's end");
    sTargets.clear();
    sVictims.clear();
    sForcedDraw.clear();
}

void StatsAndSamples(uint32_t now) {
    if (sStatsMs == 0) {
        sStatsMs = now;
    }
    if (now - sStatsMs >= kStatsIntervalMs) {
        sStatsMs = now;
        size_t onHost = 0, onOthers = 0;
        for (const auto& [actor, t] : sTargets) {
            (t.playerId == kHostPlayerId || t.playerId == 0 ? onHost : onOthers)++;
        }
        if (!sTargets.empty() || sStats.windows > 0 || sStats.restores > 0) {
            SPDLOG_INFO("[Coop] enemy-target-stats: {} enemies (on the host {}, on other players {}); switches {} "
                        "(most for one enemy {}); view windows {}; view restores {}; kept awake near other players {}",
                        sTargets.size(), onHost, onOthers, sStats.switches, sStats.maxSwitchesOneEnemy, sStats.windows,
                        sStats.restores, sStats.awake);
        }
        sStats = {};
    }
    // Test log: once a second, each enemy near some player, its target and its distance to every candidate.
    const uint32_t second = now / 1000;
    if (!GetLaunchOptions().testEnemyLog || second == sLastSampleSecond || !Active()) {
        return;
    }
    sLastSampleSecond = second;
    const std::vector<Candidate> candidates = Candidates();
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].first; actor != nullptr; actor = actor->next) {
        auto t = sTargets.find(actor);
        if (t == sTargets.end() || actor->update == nullptr) {
            continue;
        }
        bool nearSomeone = false;
        for (const Candidate& c : candidates) {
            nearSomeone |= Actor_WorldDistXYZToActor(actor, &c.player->actor) < kSampleRange;
        }
        if (nearSomeone) {
            SPDLOG_INFO("[Coop] enemy-target-sample: net {} {} target {} ({})", Enemies::HostNetIdOf(actor),
                        Enemies::TypeName(actor->id), t->second.playerId, Distances(actor, candidates));
        }
    }
}

void RegisterTargetingHooks() {
    auto* gi = GameInteractor::Instance;
    gi->RegisterGameHook<GameInteractor::ShouldActorUpdate>(OnShouldActorUpdate);
    gi->RegisterGameHook<GameInteractor::OnActorUpdate>(OnActorUpdate);
    gi->RegisterGameHook<GameInteractor::OnPlayDrawWorldStart>(OnPlayDrawWorldStart);
    gi->RegisterGameHook<GameInteractor::OnActorDestroy>(OnActorDestroy);
    gi->RegisterGameHook<GameInteractor::OnPlayDestroy>(OnPlayDestroy);
}

} // namespace

// The engine updates an actor only if the host's camera saw it in the last draw (ACTOR_FLAG_INSIDE_CULLING_VOLUME,
// set by Actor_DrawAll) or it never culls, and draws it only if the camera sees it now. An enemy next to a remote
// player but off the host's screen would stand frozen, and an enemy whose attack colliders follow its skeleton (set
// while it is drawn: a Wolfos's claws) would attack with colliders left where it was last drawn. So before this
// frame's update, an enemy within its own culling distance of a remote player (as that player's camera would see it)
// counts as seen, and is drawn this frame even off the host's screen (the GPU clips it). The draw flag is put back at
// the start of the next frame; the seen flag is set again by every draw. Enemies that stay on the host are left as
// they are, and so are the actors enemies spawn (projectiles).
void PutBackForcedDraws() {
    for (Actor* actor : sForcedDraw) {
        actor->flags &= ~ACTOR_FLAG_DRAW_CULLING_DISABLED;
    }
    sForcedDraw.clear();
}

void KeepAwakeNearRemotes() {
    PutBackForcedDraws();
    PlayState* play = gPlayState;
    if (play == nullptr || gSaveContext.gameMode != GAMEMODE_NORMAL || play->soaringCsOrSoTCsPlaying ||
        LocalLink() == nullptr) {
        return;
    }
    std::vector<Candidate> remotes = Candidates();
    remotes.erase(
        std::remove_if(remotes.begin(), remotes.end(), [](const Candidate& c) { return c.playerId == kHostPlayerId; }),
        remotes.end());
    if (remotes.empty()) {
        return;
    }
    for (int category = 0; category < ACTORCAT_MAX; category++) {
        for (Actor* actor = play->actorCtx.actorLists[category].first; actor != nullptr; actor = actor->next) {
            if (actor->flags & ACTOR_FLAG_INSIDE_CULLING_VOLUME) {
                continue; // The host sees it: updated and drawn as usual
            }
            const Actor* enemy = actor->category == ACTORCAT_ENEMY                                       ? actor
                                 : actor->parent != nullptr && actor->parent->category == ACTORCAT_ENEMY ? actor->parent
                                                                                                         : nullptr;
            if (enemy == nullptr || kHostOnly.count(enemy->id) != 0) {
                continue;
            }
            for (const Candidate& c : remotes) {
                if (Actor_WorldDistXYZToActor(actor, &c.player->actor) <= actor->cullingVolumeDistance) {
                    actor->flags |= ACTOR_FLAG_INSIDE_CULLING_VOLUME;
                    if (!(actor->flags & ACTOR_FLAG_DRAW_CULLING_DISABLED)) {
                        actor->flags |= ACTOR_FLAG_DRAW_CULLING_DISABLED;
                        sForcedDraw.push_back(actor);
                    }
                    sStats.awake++;
                    break;
                }
            }
        }
    }
}

void Frame() {
    RestoreLeftover("the end of the frame");
    const uint32_t now = NowMs();
    if (!IsHosting()) {
        sTargets.clear();
        sVictims.clear();
        PutBackForcedDraws();
        return;
    }
    WrapDamagePlayer();
    KeepAwakeNearRemotes();
    StatsAndSamples(now);
}

void NoteVictim(const Actor* enemy, uint32_t playerId) {
    if (enemy->category == ACTORCAT_ENEMY && kHostOnly.count(enemy->id) == 0) {
        sVictims[enemy] = playerId;
    }
}

bool StaysOnHost(const Actor* enemy) {
    return enemy->category == ACTORCAT_ENEMY && kHostOnly.count(enemy->id) != 0;
}

uint32_t TargetOf(const Actor* enemy) {
    auto it = sTargets.find(enemy);
    return it != sTargets.end() && it->second.playerId != 0 ? it->second.playerId : kHostPlayerId;
}

static RegisterShipInitFunc initFunc(RegisterTargetingHooks);

} // namespace Coop::Targeting
