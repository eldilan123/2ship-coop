#include "CoopCombat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <spdlog/spdlog.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "Coop.h"
#include "CoopEnemies.h"
#include "CoopArgs.h"
#include "CoopEnemyTypes.h"
#include "CoopEnemyWire.h"
#include "CoopPlayerSync.h"
#include "CoopPuppet.h"
#include "CoopSession.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace Coop::Combat {

namespace {

using Enemies::Wire::Header;
using Enemies::Wire::Reader;
using Enemies::Wire::Writer;
using SteadyClock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x53544948; // "HITS"
constexpr uint8_t kFormatVersion = 1;

enum class MsgType : uint8_t {
    Hit = 1,    // Client -> host
    Result = 2, // Host -> client
};

// What on the attacking Link touched the enemy.
enum class Kind : uint8_t {
    Melee = 0,   // meleeWeaponQuads: sword, Goron punches, Zora fins and kicks, Deku stick
    Body = 1,    // cylinder: Goron roll/pound/spikes, rolls, Deku spin and launch
    Barrier = 2, // shieldCylinder: Zora barrier
    Count = 3,
};

enum class Outcome : uint8_t {
    Applied = 0,  // The enemy took the hit
    Bounced = 1,  // It hit a hard collider (no damage), as it would for the host's Link
    Missed = 2,   // The enemy did not take a hit then (invulnerable, hidden, not hittable there)
    Rejected = 3, // Failed the checks, see Reason
};

enum class Reason : uint8_t {
    None = 0,
    OtherScene = 1,
    NoEnemy = 2,
    TooFar = 3,
    TooOld = 4,
    Duplicate = 5,
    BadData = 6,
    RateLimit = 7,
    NoPuppet = 8,
    Expired = 9,
    Count = 10,
};

const char* const kKindNames[] = { "melee", "body", "barrier" };
const char* const kOutcomeNames[] = { "applied", "bounced", "missed", "rejected" };
const char* const kReasonNames[] = {
    "-",         "other scene", "no such enemy", "too far",        "too old",
    "duplicate", "bad data",    "rate limit",    "no puppet Link", "expired in queue"
};

struct HitMsg {
    uint32_t seq;
    uint32_t clientMs;
    uint16_t netId;
    uint8_t kind;
    uint8_t form;
    uint32_t dmgFlags;
    uint8_t damage;
    uint8_t effect;
    uint16_t colliderOffset; // Which of the enemy's colliders was hit, and which element of it
    uint8_t element;
    uint8_t bounced; // The client's own check bounced off a hard collider
    int16_t hit[3];  // Hit point relative to the enemy's position
    int16_t yaw;     // Attacker's facing
    uint16_t instance;
};
static_assert(sizeof(HitMsg) == 32);

struct ResultMsg {
    uint32_t seq;
    uint16_t netId;
    uint8_t outcome;
    uint8_t reason;
    int16_t healthBefore;
    int16_t healthAfter;
    uint8_t died;
    uint8_t reserved[3];
};
static_assert(sizeof(ResultMsg) == 16);

// Damage the attack kinds can carry (Player's own tables, z_player.c).
constexpr uint32_t kAllowedFlags[] = {
    DMG_SWORD | DMG_GORON_PUNCH | DMG_ZORA_PUNCH | DMG_DEKU_STICK,
    DMG_GORON_POUND | DMG_GORON_SPIKES | DMG_NORMAL_ROLL | DMG_DEKU_SPIN | DMG_DEKU_LAUNCH,
    DMG_ZORA_BARRIER,
};
constexpr uint8_t kMaxDamage = 8;
constexpr float kMaxReach = 450.0f;     // From the attacker's puppet to the enemy (puppets lag 100-250 ms)
constexpr float kMaxHitOffset = 400.0f; // Hit point from the enemy's position
constexpr uint32_t kMaxAgeMs = 500;
constexpr uint32_t kResendFrames = 3; // Continuous attacks: one event per enemy every this many frames
constexpr double kRatePerSecond = 30.0;
constexpr double kRateBurst = 10.0;
constexpr uint32_t kPredictionTimeoutMs = 500;
constexpr uint32_t kStatsIntervalMs = 10000;
constexpr int kQuadsPerFrame = 2; // The puppet Link's two weapon quads

const SteadyClock::time_point sEpoch = SteadyClock::now();

uint32_t NowMs() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - sEpoch).count());
}

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

std::vector<uint8_t> Message(MsgType type, const void* body, size_t size) {
    Writer w;
    w.Put(Header{ kMagic, kFormatVersion, static_cast<uint8_t>(type), 0 });
    w.Bytes(body, size);
    return std::move(w.buffer);
}

struct TokenBucket {
    double tokens = kRateBurst;
    uint32_t lastMs = 0;
    bool Take(uint32_t now) {
        if (lastMs != 0) {
            tokens = std::min(kRateBurst, tokens + (now - lastMs) * kRatePerSecond / 1000.0);
        }
        lastMs = now;
        if (tokens < 1.0) {
            return false;
        }
        tokens -= 1.0;
        return true;
    }
};

float Length(float x, float y, float z) {
    return std::sqrt(x * x + y * y + z * z);
}

// MARK: - Client

struct Prediction {
    uint16_t netId;
    uint16_t params;
    uint32_t sentMs;
};

struct ClientStats {
    uint32_t sent = 0;
    uint32_t rateDropped = 0;
    uint32_t outcomes[4] = {};
    uint32_t unconfirmed = 0;
    uint32_t predicted = 0;
    uint32_t cancelled = 0;
};

uint32_t sSeq = 0;
int8_t sPrevMeleeState = 0;
uint16_t sSwing = 0;
bool sPrevBodyAttack = false;
uint16_t sBodyAttack = 0;
uint32_t sFrame = 0;
struct LastSent {
    uint16_t instance;
    uint32_t frame;
};
std::unordered_map<uint32_t, LastSent> sLastSent; // (netId << 8 | kind)
TokenBucket sClientRate;
std::unordered_map<uint32_t, Prediction> sPredictions; // by seq
ClientStats sClientStats;
uint32_t sClientStatsMs = 0;

ColliderElement* HitElement(Collider* col, uint8_t& index) {
    index = 0;
    switch (col->shape) {
        case COLSHAPE_JNTSPH: {
            auto* j = reinterpret_cast<ColliderJntSph*>(col);
            for (s32 i = 0; i < j->count; i++) {
                if (j->elements[i].base.acElemFlags & ACELEM_HIT) {
                    index = static_cast<uint8_t>(i);
                    return &j->elements[i].base;
                }
            }
            return nullptr;
        }
        case COLSHAPE_TRIS: {
            auto* tris = reinterpret_cast<ColliderTris*>(col);
            for (s32 i = 0; i < tris->count; i++) {
                if (tris->elements[i].base.acElemFlags & ACELEM_HIT) {
                    index = static_cast<uint8_t>(i);
                    return &tris->elements[i].base;
                }
            }
            return nullptr;
        }
        case COLSHAPE_CYLINDER:
            return &reinterpret_cast<ColliderCylinder*>(col)->elem;
        case COLSHAPE_QUAD:
            return &reinterpret_cast<ColliderQuad*>(col)->elem;
        case COLSHAPE_SPHERE:
            return &reinterpret_cast<ColliderSphere*>(col)->elem;
        default:
            return nullptr;
    }
}

void Predict(Actor* actor, uint32_t seq, uint16_t netId, uint32_t now) {
    if (CVarGetInteger(CVAR_COOP_PREDICT_HIT_FLASH, 1) == 0) {
        return;
    }
    // Same look as most enemies' own hit flash; the host's real one replaces it when it arrives.
    const uint16_t params = COLORFILTER_COLORFLAG_RED | COLORFILTER_BUFFLAG_OPA | ((255 & 0xF8) << 5) | 8;
    actor->colorFilterParams = params;
    actor->colorFilterTimer = 8;
    sPredictions[seq] = { netId, params, now };
    sClientStats.predicted++;
}

void CancelPrediction(uint32_t seq) {
    auto it = sPredictions.find(seq);
    if (it == sPredictions.end()) {
        return;
    }
    // Only the drawing color: if the puppet still shows the predicted flash, it stops.
    Actor* actor = Enemies::PuppetActor(it->second.netId);
    if (actor != nullptr && actor->colorFilterParams == it->second.params && actor->colorFilterTimer > 0) {
        actor->colorFilterTimer = 0;
        sClientStats.cancelled++;
    }
    sPredictions.erase(it);
}

// Attack instances of the local Link: a new swing each time the weapon becomes active, a new body attack each
// time the cylinder starts attacking (it lasts the whole roll / spin).
void TrackAttackInstances(Player* player) {
    sFrame++;
    if (player->meleeWeaponState >= 1 && sPrevMeleeState < 1) {
        sSwing++;
    }
    sPrevMeleeState = player->meleeWeaponState;
    const bool bodyAttack = (player->cylinder.base.atFlags & AT_ON) || (player->shieldCylinder.base.atFlags & AT_ON);
    if (bodyAttack && !sPrevBodyAttack) {
        sBodyAttack++;
    }
    sPrevBodyAttack = bodyAttack;
}

void ClientDetect(uint32_t now) {
    PlayState* play = gPlayState;
    Player* player = GET_PLAYER(play);
    if (player == nullptr) {
        return;
    }
    TrackAttackInstances(player);

    std::unordered_set<uint16_t> hitThisFrame;
    Enemies::ForEachRegisteredPuppetCollider([&](uint16_t netId, Actor* actor, Collider* col) {
        if (!(col->acFlags & AC_HIT) || col->ac != &player->actor || hitThisFrame.count(netId) != 0) {
            return;
        }
        uint8_t index = 0;
        ColliderElement* elem = HitElement(col, index);
        if (elem == nullptr || elem->acHit == nullptr || elem->acHitElem == nullptr) {
            return;
        }
        Kind kind;
        if (elem->acHit == &player->meleeWeaponQuads[0].base || elem->acHit == &player->meleeWeaponQuads[1].base) {
            kind = Kind::Melee;
        } else if (elem->acHit == &player->cylinder.base) {
            kind = Kind::Body;
        } else if (elem->acHit == &player->shieldCylinder.base) {
            kind = Kind::Barrier;
        } else {
            return; // Shield bash and anything else: not an attack on the enemy
        }
        if (elem->acHitElem->atDmgInfo.dmgFlags == 0) {
            return; // An attack collider left on with no damage (the end of a spin): no attack in vanilla either
        }
        const uint16_t instance = kind == Kind::Melee ? sSwing : sBodyAttack;
        const uint32_t key = (static_cast<uint32_t>(netId) << 8) | static_cast<uint8_t>(kind);
        auto last = sLastSent.find(key);
        if (last != sLastSent.end() && last->second.instance == instance &&
            (kind == Kind::Melee || sFrame - last->second.frame < kResendFrames)) {
            return; // Same swing already reported; continuous attacks again after a few frames
        }
        hitThisFrame.insert(netId);
        if (!sClientRate.Take(now)) {
            sClientStats.rateDropped++;
            return;
        }
        sLastSent[key] = { instance, sFrame };

        const ColliderElementDamageInfoAT& at = elem->acHitElem->atDmgInfo;
        HitMsg msg = {};
        msg.seq = ++sSeq;
        msg.clientMs = now;
        msg.netId = netId;
        msg.kind = static_cast<uint8_t>(kind);
        msg.form = player->transformation;
        msg.dmgFlags = at.dmgFlags;
        msg.damage = at.damage;
        msg.effect = at.effect;
        msg.colliderOffset = static_cast<uint16_t>(reinterpret_cast<uint8_t*>(col) - reinterpret_cast<uint8_t*>(actor));
        msg.element = index;
        msg.bounced = (col->acFlags & AC_BOUNCED) ? 1 : 0;
        msg.hit[0] = static_cast<int16_t>(elem->acDmgInfo.hitPos.x - std::lround(actor->world.pos.x));
        msg.hit[1] = static_cast<int16_t>(elem->acDmgInfo.hitPos.y - std::lround(actor->world.pos.y));
        msg.hit[2] = static_cast<int16_t>(elem->acDmgInfo.hitPos.z - std::lround(actor->world.pos.z));
        msg.yaw = player->actor.shape.rot.y;
        msg.instance = instance;
        const std::vector<uint8_t> bytes = Message(MsgType::Hit, &msg, sizeof(msg));
        SendBinary(kHostPlayerId, Protocol::kChannelHits, bytes.data(), bytes.size());
        sClientStats.sent++;
        SPDLOG_INFO("[Coop] hit-send: seq {} net {} {} {} form {} dmg 0x{:X} damage {} effect {}{}", msg.seq, netId,
                    Enemies::TypeName(actor->id), kKindNames[msg.kind], msg.form, msg.dmgFlags, msg.damage, msg.effect,
                    msg.bounced ? " (bounced here)" : "");
        // Only a real hit on a collider the host had active, never a bounce or a hard collider.
        if (!msg.bounced && !(col->acFlags & AC_HARD)) {
            Predict(actor, msg.seq, netId, now);
        }
    });

    // Predictions the host never confirmed.
    std::vector<uint32_t> stale;
    for (const auto& [seq, prediction] : sPredictions) {
        if (now - prediction.sentMs > kPredictionTimeoutMs) {
            stale.push_back(seq);
        }
    }
    for (uint32_t seq : stale) {
        sClientStats.unconfirmed++;
        CancelPrediction(seq);
    }
}

void ClientOnResult(Reader& r) {
    const ResultMsg msg = r.Get<ResultMsg>();
    if (!r.ok || r.Remaining() != 0 || msg.outcome > static_cast<uint8_t>(Outcome::Rejected) ||
        msg.reason >= static_cast<uint8_t>(Reason::Count)) {
        SPDLOG_WARN("[Coop] Dropping hit result: bad size or values");
        NoteDroppedMessage();
        return;
    }
    sClientStats.outcomes[msg.outcome]++;
    if (msg.outcome != static_cast<uint8_t>(Outcome::Applied)) {
        CancelPrediction(msg.seq);
    } else {
        sPredictions.erase(msg.seq);
    }
    SPDLOG_INFO("[Coop] hit-result: seq {} net {}: {}{}{} (health {} -> {}{})", msg.seq, msg.netId,
                kOutcomeNames[msg.outcome], msg.reason != 0 ? ", " : "",
                msg.reason != 0 ? kReasonNames[msg.reason] : "", msg.healthBefore, msg.healthAfter,
                msg.died ? ", died" : "");
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
    if (s.sent == 0 && s.rateDropped == 0) {
        return;
    }
    SPDLOG_INFO("[Coop] hit-stats: sent {}, applied {}, bounced {}, missed {}, rejected {}, unconfirmed {}, "
                "rate-dropped {}, flashes predicted {} cancelled {}",
                s.sent, s.outcomes[0], s.outcomes[1], s.outcomes[2], s.outcomes[3], s.unconfirmed, s.rateDropped,
                s.predicted, s.cancelled);
    sClientStats = {};
}

void ClientReset() {
    sPredictions.clear();
    sLastSent.clear();
    sSeq = 0;
}

// MARK: - Host

struct Queued {
    HitMsg msg;
    uint32_t arrivalMs;
};

struct HostPlayer {
    uint32_t lastSeq = 0;
    std::deque<Queued> queue;
    std::deque<uint64_t> recent; // (netId, kind, instance) of melee hits already queued
    int64_t minOffset = INT64_MAX;
    TokenBucket rate;
};

struct Recast {
    uint32_t playerId;
    Player* puppet;
    ColliderQuad* quad;
    Actor* target;
    HitMsg msg;
    int16_t healthBefore;
    uint8_t savedAtFlags;
    uint8_t savedAtElemFlags;
    ColliderElementDamageInfoAT savedDmg;
};

struct HostStats {
    uint32_t received = 0;
    uint32_t outcomes[4] = {};
    uint32_t reasons[static_cast<size_t>(Reason::Count)] = {};
};

std::unordered_map<uint32_t, HostPlayer> sHostPlayers;
std::vector<Recast> sRecasts; // Registered before this frame's collision check; read back next frame
HostStats sHostStats;
uint32_t sHostStatsMs = 0;

void SendResult(uint32_t playerId, const HitMsg& hit, Outcome outcome, Reason reason, int16_t before, int16_t after,
                bool died) {
    const ResultMsg msg = { hit.seq, hit.netId, static_cast<uint8_t>(outcome),      static_cast<uint8_t>(reason),
                            before,  after,     static_cast<uint8_t>(died ? 1 : 0), {} };
    const std::vector<uint8_t> bytes = Message(MsgType::Result, &msg, sizeof(msg));
    SendBinary(playerId, Protocol::kChannelHits, bytes.data(), bytes.size());
    sHostStats.outcomes[static_cast<uint8_t>(outcome)]++;
    if (outcome == Outcome::Rejected) {
        sHostStats.reasons[static_cast<uint8_t>(reason)]++;
    }
}

void Reject(uint32_t playerId, const HitMsg& hit, Reason reason) {
    SPDLOG_INFO("[Coop] hit-apply: player {} seq {} net {}: rejected ({})", playerId, hit.seq, hit.netId,
                kReasonNames[static_cast<uint8_t>(reason)]);
    SendResult(playerId, hit, Outcome::Rejected, reason, -1, -1, false);
}

// Checks that only depend on the message and the current state of the world.
Reason CheckHit(uint32_t playerId, const HitMsg& m, Actor*& target) {
    target = nullptr;
    PlayerSync::State s;
    if (!PlayerSync::LatestState(playerId, s) || !s.inGameplay || s.sceneId != gPlayState->sceneId ||
        s.sceneLayer != static_cast<int8_t>(gSaveContext.sceneLayer)) {
        return Reason::OtherScene;
    }
    target = Enemies::HostEnemyActor(m.netId);
    if (target == nullptr) {
        return Reason::NoEnemy;
    }
    Player* puppet = Puppet::PuppetOf(playerId);
    if (puppet == nullptr) {
        return Reason::NoPuppet;
    }
    const Vec3f& a = puppet->actor.world.pos;
    const Vec3f& b = target->world.pos;
    if (Length(a.x - b.x, a.y - b.y, a.z - b.z) > kMaxReach) {
        return Reason::TooFar;
    }
    return Reason::None;
}

void HostOnHit(uint32_t from, Reader& r) {
    const HitMsg m = r.Get<HitMsg>();
    if (!r.ok || r.Remaining() != 0) {
        SPDLOG_WARN("[Coop] Dropping hit from player {}: wrong size", from);
        NoteDroppedMessage();
        return;
    }
    const uint32_t now = NowMs();
    HostPlayer& player = sHostPlayers[from];
    sHostStats.received++;
    SPDLOG_INFO("[Coop] hit-recv: player {} seq {} net {} {} dmg 0x{:X} damage {} effect {} form {}", from, m.seq,
                m.netId, m.kind < static_cast<uint8_t>(Kind::Count) ? kKindNames[m.kind] : "?", m.dmgFlags, m.damage,
                m.effect, m.form);
    // Shape of the data first.
    const bool singleFlag = m.dmgFlags != 0 && (m.dmgFlags & (m.dmgFlags - 1)) == 0;
    if (m.kind >= static_cast<uint8_t>(Kind::Count) || !singleFlag || (m.dmgFlags & ~kAllowedFlags[m.kind]) ||
        m.damage > kMaxDamage || m.effect > 15 || m.form >= PLAYER_FORM_MAX ||
        Length(m.hit[0], m.hit[1], m.hit[2]) > kMaxHitOffset) {
        return Reject(from, m, Reason::BadData);
    }
    if (m.seq <= player.lastSeq) {
        return Reject(from, m, Reason::Duplicate);
    }
    player.lastSeq = m.seq;
    // How late it is compared to the fastest delivery seen from this player.
    const int64_t offset = static_cast<int64_t>(now) - m.clientMs;
    player.minOffset = std::min(player.minOffset, offset);
    if (offset - player.minOffset > kMaxAgeMs) {
        return Reject(from, m, Reason::TooOld);
    }
    if (m.kind == static_cast<uint8_t>(Kind::Melee)) {
        const uint64_t key =
            (static_cast<uint64_t>(m.netId) << 32) | (static_cast<uint64_t>(m.kind) << 16) | m.instance;
        if (std::find(player.recent.begin(), player.recent.end(), key) != player.recent.end()) {
            return Reject(from, m, Reason::Duplicate);
        }
        player.recent.push_back(key);
        while (player.recent.size() > 64) {
            player.recent.pop_front();
        }
    }
    if (!player.rate.Take(now)) {
        return Reject(from, m, Reason::RateLimit);
    }
    Actor* target = nullptr;
    const Reason reason = CheckHit(from, m, target);
    if (reason != Reason::None) {
        return Reject(from, m, reason);
    }
    player.queue.push_back({ m, now });
}

// The quad of the hitting player's puppet Link, placed at the hit point on the real enemy for this frame's check.
void StartRecast(uint32_t playerId, const HitMsg& m, Actor* target, int quadIndex) {
    PlayState* play = gPlayState;
    Player* puppet = Puppet::PuppetOf(playerId);
    ColliderQuad* quad = &puppet->meleeWeaponQuads[quadIndex];
    Recast rc = { playerId,
                  puppet,
                  quad,
                  target,
                  m,
                  static_cast<int16_t>(target->colChkInfo.health),
                  quad->base.atFlags,
                  quad->elem.atElemFlags,
                  quad->elem.atDmgInfo };
    quad->elem.atDmgInfo.dmgFlags = m.dmgFlags;
    quad->elem.atDmgInfo.damage = m.damage;
    quad->elem.atDmgInfo.effect = m.effect;
    quad->elem.atElemFlags = ATELEM_ON | ATELEM_NEAREST;
    quad->base.atFlags = AT_ON | AT_TYPE_PLAYER;
    // A 30-unit square facing the attack, centered on the hit point.
    const Vec3f c = { target->world.pos.x + m.hit[0], target->world.pos.y + m.hit[1], target->world.pos.z + m.hit[2] };
    const Vec3f right = { Math_CosS(m.yaw) * 15.0f, 0.0f, -Math_SinS(m.yaw) * 15.0f };
    Vec3f v0 = { c.x + right.x, c.y + 15.0f, c.z + right.z };
    Vec3f v1 = { c.x - right.x, c.y + 15.0f, c.z - right.z };
    Vec3f v2 = { c.x + right.x, c.y - 15.0f, c.z + right.z };
    Vec3f v3 = { c.x - right.x, c.y - 15.0f, c.z - right.z };
    Collider_SetQuadVertices(quad, &v0, &v1, &v2, &v3);
    CollisionCheck_SetAT(play, &play->colChkCtx, &quad->base);
    sRecasts.push_back(rc);
}

// The previous frame's replays: what the engine made of them, then the quads go back to how they were.
void FinishRecasts() {
    PlayState* play = gPlayState;
    for (Recast& rc : sRecasts) {
        if (Puppet::PuppetOf(rc.playerId) != rc.puppet) {
            continue; // That player's puppet is gone (left, changed form or scene)
        }
        const uint8_t atFlags = rc.quad->base.atFlags;
        Outcome outcome = Outcome::Missed;
        if (atFlags & AT_BOUNCED) {
            outcome = Outcome::Bounced;
        } else if (atFlags & AT_HIT) {
            outcome = Outcome::Applied;
        }
        Actor* alive = Enemies::HostEnemyActor(rc.msg.netId);
        const int16_t after = alive == rc.target ? static_cast<int16_t>(rc.target->colChkInfo.health) : 0;
        const bool died = alive != rc.target || after == 0;
        Collider_ResetQuadAT(play, &rc.quad->base);
        rc.quad->base.atFlags = rc.savedAtFlags;
        rc.quad->elem.atElemFlags = rc.savedAtElemFlags;
        rc.quad->elem.atDmgInfo = rc.savedDmg;
        SPDLOG_INFO("[Coop] hit-apply: player {} seq {} net {} {}: {} (health {} -> {}{})", rc.playerId, rc.msg.seq,
                    rc.msg.netId, Enemies::TypeName(rc.target->id), kOutcomeNames[static_cast<uint8_t>(outcome)],
                    rc.healthBefore, after, died ? ", died" : "");
        SendResult(rc.playerId, rc.msg, outcome, Reason::None, rc.healthBefore, after, died);
    }
    sRecasts.clear();
}

void HostFrame(uint32_t now) {
    FinishRecasts();
    // Players that left.
    const std::vector<PlayerInfo> players = GetSnapshot().players;
    for (auto it = sHostPlayers.begin(); it != sHostPlayers.end();) {
        const bool present =
            std::any_of(players.begin(), players.end(), [&](const PlayerInfo& p) { return p.id == it->first; });
        it = present ? std::next(it) : sHostPlayers.erase(it);
    }
    for (auto& [playerId, player] : sHostPlayers) {
        int used = 0;
        while (!player.queue.empty() && used < kQuadsPerFrame) {
            const Queued q = player.queue.front();
            player.queue.pop_front();
            if (now - q.arrivalMs > kMaxAgeMs) {
                Reject(playerId, q.msg, Reason::Expired);
                continue;
            }
            Actor* target = nullptr;
            const Reason reason = CheckHit(playerId, q.msg, target);
            if (reason != Reason::None) {
                Reject(playerId, q.msg, reason);
                continue;
            }
            StartRecast(playerId, q.msg, target, used++);
        }
    }
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
    if (s.received == 0) {
        return;
    }
    std::string reasons;
    for (size_t i = 1; i < static_cast<size_t>(Reason::Count); i++) {
        if (s.reasons[i] > 0) {
            reasons += fmt::format("{}{} {}", reasons.empty() ? "" : ", ", kReasonNames[i], s.reasons[i]);
        }
    }
    SPDLOG_INFO("[Coop] hit-stats: received {}, applied {}, bounced {}, missed {}, rejected {}{}{}{}", s.received,
                s.outcomes[0], s.outcomes[1], s.outcomes[2], s.outcomes[3], reasons.empty() ? "" : " (", reasons,
                reasons.empty() ? "" : ")");
    sHostStats = {};
}

// --coop-test-enemy-log on the host: its own Link's contacts with enemies (one per swing or body attack and enemy),
// so a test can compare them with a client's hits.
std::unordered_map<uint16_t, uint16_t> sHostContactInstance;

void LogHostContact(Collider* at, Collider* ac) {
    Player* player = GET_PLAYER(gPlayState);
    if (!GetLaunchOptions().testEnemyLog || player == nullptr || at->actor != &player->actor || ac->actor == nullptr) {
        return;
    }
    const uint16_t netId = Enemies::HostNetIdOf(ac->actor);
    const bool melee = at == &player->meleeWeaponQuads[0].base || at == &player->meleeWeaponQuads[1].base;
    const uint16_t instance = static_cast<uint16_t>(melee ? sSwing : (0x8000 | sBodyAttack));
    auto it = sHostContactInstance.find(netId);
    if (netId == 0 || (it != sHostContactInstance.end() && it->second == instance)) {
        return;
    }
    sHostContactInstance[netId] = instance;
    const uint32_t dmgFlags = melee ? reinterpret_cast<ColliderQuad*>(at)->elem.atDmgInfo.dmgFlags
                                    : reinterpret_cast<ColliderCylinder*>(at)->elem.atDmgInfo.dmgFlags;
    SPDLOG_INFO("[Coop] host-contact: net {} {} {} dmg 0x{:X}", netId, Enemies::TypeName(ac->actor->id),
                melee ? "melee" : "body", dmgFlags);
}

// Only the replayed quad's own target may take its hit.
void OnAcCollision(GIVanillaBehavior, bool* should, va_list originalArgs) {
    va_list args;
    va_copy(args, originalArgs);
    Collider* at = va_arg(args, Collider*);
    Collider* ac = va_arg(args, Collider*);
    va_end(args);
    if (IsHosting()) {
        LogHostContact(at, ac);
    }
    for (const Recast& rc : sRecasts) {
        if (at == &rc.quad->base && ac->actor != rc.target) {
            *should = false;
        }
    }
}

// MARK: - Messages and hooks

void OnHitMessage(uint32_t from, const std::vector<uint8_t>& bytes) {
    if (!IsHosting() && !IsClientInSession()) {
        return;
    }
    Reader r(bytes.data(), bytes.size());
    const Header header = r.Get<Header>();
    if (!r.ok || header.magic != kMagic || header.formatVersion != kFormatVersion) {
        SPDLOG_WARN("[Coop] Dropping hit message from {}: bad header, magic or format version", from);
        NoteDroppedMessage();
        return;
    }
    const MsgType type = static_cast<MsgType>(header.type);
    if (IsHosting() && type == MsgType::Hit) {
        if (InGameplay()) {
            HostOnHit(from, r);
        }
    } else if (!IsHosting() && type == MsgType::Result && from == kHostPlayerId) {
        ClientOnResult(r);
    } else {
        SPDLOG_WARN("[Coop] Dropping hit message from {}: unexpected type {}", from, header.type);
        NoteDroppedMessage();
    }
}

void OnPlayDestroy() {
    // Every actor of the scene is gone: nothing to read back or restore.
    sRecasts.clear();
    for (auto& [playerId, player] : sHostPlayers) {
        player.queue.clear();
    }
    sPredictions.clear();
}

void RegisterCombatHooks() {
    RegisterBinaryHandler(Protocol::kChannelHits, OnHitMessage);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDestroy>(OnPlayDestroy);
    REGISTER_VB_SHOULD(VB_PERFORM_AC_COLLISION, { OnAcCollision(_, should, originalArgs); });
}

} // namespace

void Frame() {
    const uint32_t now = NowMs();
    if (IsHosting()) {
        if (InGameplay()) {
            if (Player* player = GET_PLAYER(gPlayState)) {
                TrackAttackInstances(player);
            }
            HostFrame(now);
        }
        HostStatsLine(now);
        return;
    }
    if (!IsClientInSession()) {
        if (!sPredictions.empty() || sSeq != 0) {
            ClientReset();
        }
        return;
    }
    if (IsClientPlayingInSession() && InGameplay()) {
        ClientDetect(now);
    }
    ClientStatsLine(now);
}

void TestFuzz() {
    const bool host = IsHosting();
    const uint32_t target = host ? kBroadcast : kHostPlayerId;
    auto send = [&](const char* what, const std::vector<uint8_t>& bytes) {
        SPDLOG_INFO("[Coop] TEST: fuzz hit channel: {} ({} bytes)", what, bytes.size());
        SendBinary(target, Protocol::kChannelHits, bytes.data(), bytes.size());
    };
    send("empty message", {});
    Writer badMagic;
    badMagic.Put(Header{ 0xDEADBEEF, kFormatVersion, static_cast<uint8_t>(MsgType::Hit), 0 });
    send("bad magic", badMagic.buffer);
    HitMsg m = {};
    m.seq = 0x7FFFFFF0;
    m.netId = 1;
    m.kind = 9;
    m.dmgFlags = DMG_SWORD;
    send("unknown attack kind", Message(MsgType::Hit, &m, sizeof(m)));
    m.kind = 0;
    m.dmgFlags = DMG_POWDER_KEG | DMG_SWORD;
    send("two damage flags", Message(MsgType::Hit, &m, sizeof(m)));
    m.dmgFlags = DMG_SWORD;
    m.damage = 200;
    send("damage 200", Message(MsgType::Hit, &m, sizeof(m)));
    m.damage = 1;
    m.hit[0] = 30000;
    send("hit point 30000 units away", Message(MsgType::Hit, &m, sizeof(m)));
    ResultMsg res = {};
    res.outcome = 9;
    send("result with an unknown outcome", Message(MsgType::Result, &res, sizeof(res)));
    std::vector<uint8_t> cut = Message(MsgType::Hit, &m, sizeof(m));
    cut.resize(cut.size() - 5);
    send("hit cut short", cut);
}

static RegisterShipInitFunc initFunc(RegisterCombatHooks);

} // namespace Coop::Combat
