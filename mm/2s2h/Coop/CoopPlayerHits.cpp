#include "CoopStory.h"
#include "CoopPlayerHits.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <vector>
#include <spdlog/spdlog.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopCombat.h"
#include "CoopEnemies.h"
#include "CoopEnemyTypes.h"
#include "CoopPlayerSync.h"
#include "CoopProjectiles.h"
#include "CoopPuppet.h"
#include "CoopSession.h"
#include "CoopTargeting.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace Coop::PlayerHits {

namespace {

using SteadyClock = std::chrono::steady_clock;
using Enemies::Wire::Reader;

enum class Kind : uint8_t {
    Enemy = 0,      // An enemy's AT collider touched the puppet
    Projectile = 1, // One of the host's shared projectiles touched it
    Knockback = 2,  // The enemy asked for a knockback (Player's unk_B74..B7C) without colliders
    Damage = 3,     // The enemy called damagePlayer
    Count = 4,
};
const char* const kKindNames[] = { "enemy", "projectile", "knockback", "damage" };

struct PlayerHitMsg {
    uint32_t seq;
    uint32_t hostMs;
    uint8_t kind;
    uint8_t shield;    // The host's check touched the puppet's shield
    uint16_t attacker; // Enemy netId or projectile id (owner: the host)
    uint32_t dmgFlags;
    uint8_t damage;
    uint8_t effect;
    uint8_t elemMaterial;
    uint8_t atElemFlags; // ATELEM_SFX_* bits of the attacker's element
    int16_t yaw;         // From the attacker to the player
    int16_t knockYaw;
    float knockSpeed;
    float knockVelocityY;
    uint8_t knockType;
    uint8_t knockDamage;
    int16_t directDamage;
    float puppetPos[3]; // Where the host saw this player when it was hit
};
static_assert(sizeof(PlayerHitMsg) == 48);

constexpr uint32_t kStatsIntervalMs = 10000;
constexpr u8 kAtElemSfxMask = 3 << 3; // ATELEM_SFX_* (z64collision_check.h)
constexpr double kClientRatePerSecond = 20.0;
constexpr double kClientRateBurst = 10.0;
constexpr int kReplaysPerFrame = 4;
constexpr float kMovedThreshold = 50.0f; // Stats: the player had moved at least this far since the host saw it

const SteadyClock::time_point sEpoch = SteadyClock::now();

uint32_t NowMs() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - sEpoch).count());
}

bool Enabled() {
    return CVarGetInteger(CVAR_COOP_ENEMIES_HURT_CLIENTS, 1) != 0;
}

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

void Send(uint32_t playerId, const PlayerHitMsg& msg) {
    const std::vector<uint8_t> bytes = Combat::HitsMessage(kMsgPlayerHit, &msg, sizeof(msg));
    SendBinary(playerId, Protocol::kChannelHits, bytes.data(), bytes.size());
}

// MARK: - Host

struct HostPlayer {
    uint32_t seq = 0;
    uint32_t acOffUntilFrame = 0;
};
std::unordered_map<uint32_t, HostPlayer> sHostPlayers;
uint32_t sFrame = 0;
// One event per attacker and player per frame (an AT can touch several elements of the puppet).
std::vector<std::pair<const Actor*, uint32_t>> sSentThisFrame;
struct HostStats {
    uint32_t sent[static_cast<size_t>(Kind::Count)] = {};
    uint32_t ignored = 0; // Attackers that are neither shared enemies nor shared projectiles
    // Puppet frames by whether its AC was on, and why not
    uint32_t hittable = 0;
    uint32_t cooldown = 0;
    uint32_t invulnerable = 0;
    uint32_t other = 0; // Dead, in a cutscene, no state, or the CVar is off
} sHostStats;
uint32_t sHostStatsMs = 0;
uint32_t sLastIgnoredLogMs = 0;

bool AlreadySent(const Actor* attacker, uint32_t playerId) {
    for (const auto& [a, p] : sSentThisFrame) {
        if (a == attacker && p == playerId) {
            return true;
        }
    }
    sSentThisFrame.push_back({ attacker, playerId });
    return false;
}

void Fill(PlayerHitMsg& msg, uint32_t playerId, Kind kind, const Actor* attacker, Player* puppet) {
    HostPlayer& hp = sHostPlayers[playerId];
    msg.seq = ++hp.seq;
    msg.hostMs = NowMs();
    msg.kind = static_cast<uint8_t>(kind);
    const uint16_t netId = Enemies::HostNetIdOf(attacker);
    msg.attacker = netId != 0 ? netId : Projectiles::OwnedId(const_cast<Actor*>(attacker));
    msg.yaw = Math_Vec3f_Yaw(const_cast<Vec3f*>(&attacker->world.pos), &puppet->actor.world.pos);
    msg.puppetPos[0] = puppet->actor.world.pos.x;
    msg.puppetPos[1] = puppet->actor.world.pos.y;
    msg.puppetPos[2] = puppet->actor.world.pos.z;
}

void HostSend(uint32_t playerId, const PlayerHitMsg& msg, const Actor* attacker, const char* extra) {
    Send(playerId, msg);
    sHostStats.sent[msg.kind]++;
    SPDLOG_INFO("[Coop] player-hit-send: player {} seq {} {} by {} {} dmg 0x{:X} damage {} effect {}{}{}", playerId,
                msg.seq, kKindNames[msg.kind],
                msg.kind == static_cast<uint8_t>(Kind::Projectile) ? "projectile" : "net", msg.attacker, msg.dmgFlags,
                msg.damage, msg.effect, msg.shield ? " (shield)" : "", extra);
}

// The attacker as the client knows it: an enemy with a netId, or one of the host's shared projectiles.
bool Shared(const Actor* attacker, Kind& kind) {
    if (Enemies::HostNetIdOf(attacker) != 0) {
        kind = Kind::Enemy;
        return true;
    }
    if (Projectiles::OwnedId(const_cast<Actor*>(attacker)) != 0) {
        kind = Kind::Projectile;
        return true;
    }
    return false;
}

void HostOnPuppetHit(Collider* at, Collider* ac, ColliderElement* atElem, uint32_t playerId, Player* puppet) {
    Actor* attacker = at->actor;
    if (attacker == nullptr || atElem == nullptr) {
        return;
    }
    // The engine takes the hit only if it does damage or has an effect (Player has no damage table).
    const ColliderElementDamageInfoAT& dmg = atElem->atDmgInfo;
    if (dmg.damage == 0 && dmg.effect == 0) {
        return;
    }
    Kind kind;
    if (!Shared(attacker, kind)) {
        sHostStats.ignored++;
        const uint32_t now = NowMs();
        if (now - sLastIgnoredLogMs > 2000) {
            sLastIgnoredLogMs = now;
            SPDLOG_INFO("[Coop] player-hit: player {} touched by {}, which the client does not have (no event)",
                        playerId, Enemies::TypeName(attacker->id));
        }
        return;
    }
    if (AlreadySent(attacker, playerId)) {
        return;
    }
    PlayerHitMsg msg = {};
    Fill(msg, playerId, kind, attacker, puppet);
    msg.shield = ac == &puppet->shieldQuad.base ? 1 : 0;
    msg.dmgFlags = dmg.dmgFlags;
    msg.damage = dmg.damage;
    msg.effect = dmg.effect;
    msg.elemMaterial = atElem->elemMaterial;
    msg.atElemFlags = atElem->atElemFlags & (kAtElemSfxMask | ATELEM_NEAREST);
    HostSend(playerId, msg, attacker, "");
    // Not hittable again until its own invulnerability (reported on channel 3) takes over.
    sHostPlayers[playerId].acOffUntilFrame =
        sFrame + static_cast<uint32_t>(std::clamp(CVarGetInteger(CVAR_COOP_PUPPET_AC_OFF_FRAMES, 30), 0, 300));
    // The player that was hit is the one this enemy deals with in its next update (knockback, recoil).
    Targeting::NoteVictim(attacker, playerId);
}

void HostStatsLine(uint32_t now) {
    if (sHostStatsMs == 0) {
        sHostStatsMs = now;
    }
    if (now - sHostStatsMs < kStatsIntervalMs) {
        return;
    }
    sHostStatsMs = now;
    const HostStats& s = sHostStats;
    const uint32_t total = s.sent[0] + s.sent[1] + s.sent[2] + s.sent[3];
    if (total > 0 || s.ignored > 0 || s.hittable > 0) {
        SPDLOG_INFO("[Coop] player-hit-stats (host): sent {} (enemy {}, projectile {}, knockback {}, damage {}); "
                    "touches by attackers the client lacks {}; puppet frames hittable {}, after a hit {}, "
                    "invulnerable {}, other {}",
                    total, s.sent[0], s.sent[1], s.sent[2], s.sent[3], s.ignored, s.hittable, s.cooldown,
                    s.invulnerable, s.other);
    }
    sHostStats = {};
}

// MARK: - Client

struct Pending {
    PlayerHitMsg msg;
    Actor* attacker;
    uint32_t arrivalMs;
};

struct Replay {
    PlayerHitMsg msg;
    int16_t healthBefore;
    float moved;
    bool collider; // A quad was registered (read its flags back); otherwise a direct call
};

struct ClientStats {
    uint32_t received = 0;
    uint32_t hit = 0;
    uint32_t bounced = 0;
    uint32_t missed = 0;
    uint32_t direct = 0;
    uint32_t rejected = 0;
    uint32_t moved = 0; // Applied after the player had moved kMovedThreshold or more since the host saw it
} sClientStats;
uint32_t sClientStatsMs = 0;

struct TokenBucket {
    double tokens = kClientRateBurst;
    uint32_t lastMs = 0;
    bool Take(uint32_t now) {
        if (lastMs != 0) {
            tokens = std::min(kClientRateBurst, tokens + (now - lastMs) * kClientRatePerSecond / 1000.0);
        }
        lastMs = now;
        if (tokens < 1.0) {
            return false;
        }
        tokens -= 1.0;
        return true;
    }
} sClientRate;

std::deque<Pending> sQueue;
std::vector<Replay> sReplays;
std::array<ColliderQuad, kReplaysPerFrame> sQuads;
int64_t sMinOffset = INT64_MAX;
uint32_t sLastSeq = 0;

void Reject(const PlayerHitMsg& m, const char* why) {
    sClientStats.rejected++;
    SPDLOG_INFO("[Coop] player-hit-apply: seq {} {} by {}: rejected ({})", m.seq,
                m.kind < static_cast<uint8_t>(Kind::Count) ? kKindNames[m.kind] : "?", m.attacker, why);
}

Actor* ResolveAttacker(const PlayerHitMsg& m) {
    if (m.kind == static_cast<uint8_t>(Kind::Projectile)) {
        return Projectiles::CopyActor(kHostPlayerId, m.attacker);
    }
    return Enemies::PuppetActor(m.attacker);
}

float Distance(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool ClientPlaying() {
    return !IsHosting() && IsClientPlayingInSession() && InGameplay();
}

void ApplyDirect(const Pending& p, Player* local) {
    PlayState* play = gPlayState;
    const PlayerHitMsg& m = p.msg;
    if (m.kind == static_cast<uint8_t>(Kind::Knockback)) {
        // As the enemy's own call would have done on this Link (z_actor.c, func_800B8D10).
        func_800B8D10(play, p.attacker, m.knockSpeed, m.knockYaw, m.knockVelocityY, m.knockType, m.knockDamage);
    } else {
        play->damagePlayer(play, m.directDamage);
    }
}

void StartReplay(const Pending& p, Player* local, int index) {
    PlayState* play = gPlayState;
    const PlayerHitMsg& m = p.msg;
    Replay rc = {};
    rc.msg = m;
    rc.healthBefore = gSaveContext.save.saveInfo.playerData.health;
    const Vec3f seen = { m.puppetPos[0], m.puppetPos[1], m.puppetPos[2] };
    rc.moved = Distance(local->actor.world.pos, seen);
    if (m.kind == static_cast<uint8_t>(Kind::Knockback) || m.kind == static_cast<uint8_t>(Kind::Damage)) {
        ApplyDirect(p, local);
        rc.collider = false;
        sReplays.push_back(rc);
        return;
    }
    ColliderQuad* quad = &sQuads[index];
    static ColliderQuadInit sInit = {
        { COL_MATERIAL_NONE, AT_ON | AT_TYPE_ENEMY, AC_NONE, OC1_NONE, OC2_TYPE_1, COLSHAPE_QUAD },
        { ELEM_MATERIAL_UNK0, { 0, 0, 0 }, { 0, 0, 0 }, ATELEM_ON, ACELEM_NONE, OCELEM_NONE },
        { { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } } },
    };
    Collider_InitQuad(play, quad);
    Collider_SetQuad(play, quad, p.attacker, &sInit);
    quad->elem.elemMaterial = m.elemMaterial;
    quad->elem.atElemFlags = ATELEM_ON | (m.atElemFlags & (kAtElemSfxMask | ATELEM_NEAREST));
    quad->elem.atDmgInfo.dmgFlags = m.dmgFlags;
    quad->elem.atDmgInfo.damage = m.damage;
    quad->elem.atDmgInfo.effect = m.effect;
    // A 40-unit square across the Link's body, facing the attacker: Player takes the damage, the effect and the
    // knockback direction from the attacker's element and position, the shield blocks it if it is up.
    const Vec3f c = { local->actor.world.pos.x, local->actor.world.pos.y + 30.0f, local->actor.world.pos.z };
    const Vec3f right = { Math_CosS(m.yaw) * 20.0f, 0.0f, -Math_SinS(m.yaw) * 20.0f };
    Vec3f v0 = { c.x + right.x, c.y + 20.0f, c.z + right.z };
    Vec3f v1 = { c.x - right.x, c.y + 20.0f, c.z - right.z };
    Vec3f v2 = { c.x + right.x, c.y - 20.0f, c.z + right.z };
    Vec3f v3 = { c.x - right.x, c.y - 20.0f, c.z - right.z };
    Collider_SetQuadVertices(quad, &v0, &v1, &v2, &v3);
    CollisionCheck_SetAT(play, &play->colChkCtx, &quad->base);
    rc.collider = true;
    sReplays.push_back(rc);
}

// The previous frame's replays: what the engine and the Link made of them.
void FinishReplays() {
    PlayState* play = gPlayState;
    for (size_t i = 0; i < sReplays.size(); i++) {
        const Replay& rc = sReplays[i];
        const int16_t after = gSaveContext.save.saveInfo.playerData.health;
        const char* outcome = "applied";
        if (rc.collider) {
            ColliderQuad* quad = &sQuads[i];
            outcome = (quad->base.atFlags & AT_BOUNCED) ? "bounced" : (quad->base.atFlags & AT_HIT) ? "hit" : "missed";
            Collider_ResetQuadAT(play, &quad->base);
            quad->base.atFlags = AT_NONE;
            quad->base.actor = nullptr;
            if (outcome[0] == 'b') {
                sClientStats.bounced++;
            } else if (outcome[0] == 'h') {
                sClientStats.hit++;
            } else {
                sClientStats.missed++;
            }
        } else {
            sClientStats.direct++;
        }
        if (rc.moved >= kMovedThreshold) {
            sClientStats.moved++;
        }
        SPDLOG_INFO(
            "[Coop] player-hit-apply: seq {} {} by {}: {} (health {} -> {}; moved {:.0f} since the host saw it)",
            rc.msg.seq, kKindNames[rc.msg.kind], rc.msg.attacker, outcome, rc.healthBefore, after, rc.moved);
    }
    sReplays.clear();
}

void ClientFrame(uint32_t now) {
    FinishReplays();
    Player* local = GET_PLAYER(gPlayState);
    if (local == nullptr) {
        sQueue.clear();
        return;
    }
    int used = 0;
    while (!sQueue.empty() && used < kReplaysPerFrame) {
        Pending p = sQueue.front();
        sQueue.pop_front();
        // Checked again now: the attacker could have gone since the event arrived.
        p.attacker = ResolveAttacker(p.msg);
        if (p.attacker == nullptr) {
            Reject(p.msg, "the attacker is not here");
            continue;
        }
        const float radius = static_cast<float>(std::clamp(CVarGetInteger(CVAR_COOP_PLAYER_HIT_RADIUS, 600), 50, 5000));
        if (Distance(p.attacker->world.pos, local->actor.world.pos) > radius) {
            Reject(p.msg, "the attacker is too far from the Link");
            continue;
        }
        if (local->stateFlags1 & PLAYER_STATE1_DEAD) {
            Reject(p.msg, "the Link is dead");
            continue;
        }
        // Talking, in a cutscene or paused (and 500 ms after): the host's enemies do not freeze for this game
        // (milestones 6, 7).
        if (Story::ClientBusy()) {
            Reject(p.msg, "the Link is talking, in a cutscene or paused");
            continue;
        }
        StartReplay(p, local, used++);
    }
}

void ClientStatsLine(uint32_t now) {
    if (sClientStatsMs == 0) {
        sClientStatsMs = now;
    }
    if (now - sClientStatsMs < kStatsIntervalMs) {
        return;
    }
    sClientStatsMs = now;
    const ClientStats& s = sClientStats;
    if (s.received > 0) {
        SPDLOG_INFO("[Coop] player-hit-stats (client): received {}, hit {}, bounced {}, missed {}, direct {}, "
                    "rejected {}; applied after the Link had moved {}+ units: {}",
                    s.received, s.hit, s.bounced, s.missed, s.direct, s.rejected, static_cast<int>(kMovedThreshold),
                    s.moved);
    }
    sClientStats = {};
}

void OnPlayDestroy() {
    sQueue.clear();
    sReplays.clear();
    sSentThisFrame.clear();
}

void RegisterPlayerHitsHooks() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDestroy>(OnPlayDestroy);
}

} // namespace

void Frame() {
    const uint32_t now = NowMs();
    sFrame++;
    sSentThisFrame.clear();
    if (IsHosting()) {
        HostStatsLine(now);
        return;
    }
    sHostPlayers.clear();
    if (!IsClientInSession()) {
        sQueue.clear();
        sReplays.clear();
        sMinOffset = INT64_MAX;
        sLastSeq = 0;
        return;
    }
    if (ClientPlaying()) {
        ClientFrame(now);
    }
    ClientStatsLine(now);
}

bool MayHurt(uint32_t playerId) {
    if (!Enabled() || !IsHosting()) {
        sHostStats.other++;
        return false;
    }
    auto it = sHostPlayers.find(playerId);
    if (it != sHostPlayers.end() && sFrame < it->second.acOffUntilFrame) {
        sHostStats.cooldown++;
        return false;
    }
    PlayerSync::State s;
    if (!PlayerSync::LatestState(playerId, s) || (s.stateFlags1 & PLAYER_STATE1_DEAD) ||
        s.csAction != PLAYER_CSACTION_NONE) {
        sHostStats.other++;
        return false;
    }
    if (s.invincibilityTimer != 0) {
        sHostStats.invulnerable++;
        return false;
    }
    sHostStats.hittable++;
    return true;
}

void OnAcCollision(Collider* at, Collider* ac, ColliderElement* atElem, bool* should) {
    if (IsHosting()) {
        if (ac->actor == nullptr || !Enabled()) {
            return;
        }
        const uint32_t playerId = Puppet::PlayerIdOf(ac->actor);
        if (playerId == 0) {
            return;
        }
        Player* puppet = reinterpret_cast<Player*>(ac->actor);
        if (ac != &puppet->cylinder.base && ac != &puppet->shieldQuad.base) {
            *should = false; // Nothing else of a puppet takes hits
            return;
        }
        if (at->actor != nullptr && Targeting::StaysOnHost(at->actor)) {
            *should = false; // Its grab or steal would take the host's Link
            return;
        }
        HostOnPuppetHit(at, ac, atElem, playerId, puppet);
        return;
    }
    // Client: a replay quad only ever touches the local Link.
    for (const ColliderQuad& quad : sQuads) {
        if (at == &quad.base && gPlayState != nullptr && ac->actor != &GET_PLAYER(gPlayState)->actor) {
            *should = false;
        }
    }
}

void HostDirectKnockback(uint32_t playerId, Actor* attacker, u8 type, u8 damage, s16 yaw, f32 speed, f32 velocityY) {
    Player* puppet = Puppet::PuppetOf(playerId);
    Kind kind;
    if (!Enabled() || puppet == nullptr || !Shared(attacker, kind)) {
        return;
    }
    PlayerHitMsg msg = {};
    Fill(msg, playerId, Kind::Knockback, attacker, puppet);
    msg.knockType = type;
    msg.knockDamage = damage;
    msg.knockYaw = yaw;
    msg.knockSpeed = speed;
    msg.knockVelocityY = velocityY;
    HostSend(playerId, msg, attacker, "");
}

void HostDirectDamage(uint32_t playerId, Actor* attacker, s32 damage) {
    Player* puppet = Puppet::PuppetOf(playerId);
    Kind kind;
    if (!Enabled() || puppet == nullptr || !Shared(attacker, kind)) {
        return;
    }
    PlayerHitMsg msg = {};
    Fill(msg, playerId, Kind::Damage, attacker, puppet);
    msg.directDamage = static_cast<int16_t>(std::clamp(damage, -0x140, 0x140));
    HostSend(playerId, msg, attacker, "");
}

void ClientOnPlayerHit(Reader& r) {
    const PlayerHitMsg m = r.Get<PlayerHitMsg>();
    if (!r.ok || r.Remaining() != 0 || m.kind >= static_cast<uint8_t>(Kind::Count) || m.effect > 15 ||
        m.knockType > 4 || m.directDamage < -0x140 || m.directDamage > 0x140 || !std::isfinite(m.knockSpeed) ||
        !std::isfinite(m.knockVelocityY) || std::fabs(m.knockSpeed) > 100.0f || std::fabs(m.knockVelocityY) > 100.0f ||
        !std::isfinite(m.puppetPos[0]) || !std::isfinite(m.puppetPos[1]) || !std::isfinite(m.puppetPos[2])) {
        SPDLOG_WARN("[Coop] Dropping player hit: bad size or values");
        NoteDroppedMessage();
        return;
    }
    if (!ClientPlaying() || !Enabled()) {
        return;
    }
    const uint32_t now = NowMs();
    sClientStats.received++;
    SPDLOG_INFO("[Coop] player-hit-recv: seq {} {} by {} dmg 0x{:X} damage {} effect {}", m.seq, kKindNames[m.kind],
                m.attacker, m.dmgFlags, m.damage, m.effect);
    if (m.seq <= sLastSeq) {
        return Reject(m, "duplicate or out of order");
    }
    if (ResolveAttacker(m) == nullptr) {
        return Reject(m, "the attacker is not here");
    }
    // How late it is compared to the fastest delivery seen from the host.
    const int64_t offset = static_cast<int64_t>(now) - m.hostMs;
    sMinOffset = std::min(sMinOffset, offset);
    const int64_t maxAge = std::clamp(CVarGetInteger(CVAR_COOP_PLAYER_HIT_MAX_AGE, 500), 50, 5000);
    if (offset - sMinOffset > maxAge) {
        return Reject(m, "too old");
    }
    if (!sClientRate.Take(now)) {
        return Reject(m, "rate limit");
    }
    sLastSeq = m.seq;
    sQueue.push_back({ m, nullptr, now });
}

void TestFuzz() {
    if (IsHosting()) {
        auto send = [](const char* what, const std::vector<uint8_t>& bytes) {
            SPDLOG_INFO("[Coop] TEST: fuzz player hit: {} ({} bytes)", what, bytes.size());
            SendBinary(kBroadcast, Protocol::kChannelHits, bytes.data(), bytes.size());
        };
        PlayerHitMsg m = {};
        m.seq = 0x7FFFFFF0;
        m.kind = 9;
        send("unknown kind", Combat::HitsMessage(kMsgPlayerHit, &m, sizeof(m)));
        m.kind = 2;
        m.knockSpeed = NAN;
        send("knockback speed NaN", Combat::HitsMessage(kMsgPlayerHit, &m, sizeof(m)));
        m.knockSpeed = 0.0f;
        m.directDamage = 30000;
        send("direct damage 30000", Combat::HitsMessage(kMsgPlayerHit, &m, sizeof(m)));
        m.directDamage = 0;
        m.kind = 0;
        m.attacker = 0xFFFF;
        send("hit by an enemy that does not exist", Combat::HitsMessage(kMsgPlayerHit, &m, sizeof(m)));
        std::vector<uint8_t> cut = Combat::HitsMessage(kMsgPlayerHit, &m, sizeof(m));
        cut.resize(cut.size() - 7);
        send("player hit cut short", cut);
    }
}

static RegisterShipInitFunc initFunc(RegisterPlayerHitsHooks);

} // namespace Coop::PlayerHits
