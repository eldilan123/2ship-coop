#include "CoopCombat.h"

#include <algorithm>
#include <array>
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
#include "CoopEnemyColliders.h"
#include "CoopArgs.h"
#include "CoopEnemyTypes.h"
#include "CoopEnemyWire.h"
#include "CoopPlayerSync.h"
#include "CoopPlayerHits.h"
#include "CoopProjectiles.h"
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
    Melee = 0,      // meleeWeaponQuads: sword, Goron punches, Zora fins and kicks, Deku stick
    Body = 1,       // cylinder: Goron roll/pound/spikes, rolls, Deku spin and launch
    Barrier = 2,    // shieldCylinder: Zora barrier
    Projectile = 3, // One of the attacker's own projectiles (CoopProjectiles); `instance` is its id
    Count = 4,
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
    NoProjectile = 10,
    Count = 11,
};

const char* const kKindNames[] = { "melee", "body", "barrier", "projectile" };
const char* const kOutcomeNames[] = { "applied", "bounced", "missed", "rejected" };
const char* const kReasonNames[] = {
    "-",        "other scene", "no such enemy",  "too far",          "too old",           "duplicate",
    "bad data", "rate limit",  "no puppet Link", "expired in queue", "no such projectile"
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
    // Projectiles; each type is also checked against its own (CoopProjectileTypes)
    DMG_NORMAL_ARROW | DMG_FIRE_ARROW | DMG_ICE_ARROW | DMG_LIGHT_ARROW | DMG_DEKU_BUBBLE | DMG_DEKU_NUT |
        DMG_UNK_0x12 | DMG_EXPLOSIVES | DMG_ZORA_BOOMERANG,
};
constexpr uint8_t kMaxDamage = 8;
constexpr float kMaxReach = 450.0f;     // From the attacker's puppet to the enemy (puppets lag 100-250 ms)
constexpr float kMaxHitOffset = 400.0f; // Hit point from the enemy's position
constexpr uint32_t kMaxAgeMs = 500;
constexpr uint32_t kResendFrames = 3; // Continuous attacks: one event per enemy every this many frames
constexpr double kRatePerSecond = 30.0;
constexpr double kRateBurst = 10.0;
// Projectile hits come in bursts: a blast stays out 10 frames and may hit each enemy around on every one of them.
constexpr double kProjectileRatePerSecond = 90.0;
constexpr double kProjectileRateBurst = 40.0;
constexpr uint32_t kPredictionTimeoutMs = 500;
constexpr uint32_t kStatsIntervalMs = 10000;
constexpr int kQuadsPerFrame = 2;    // The puppet Link's two weapon quads
constexpr int kProjectileQuads = 16; // Host: projectile hits replayed in one frame, all players

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
    double perSecond = kRatePerSecond;
    double burst = kRateBurst;
    double tokens = kRateBurst;
    uint32_t lastMs = 0;
    bool Take(uint32_t now) {
        if (lastMs != 0) {
            tokens = std::min(burst, tokens + (now - lastMs) * perSecond / 1000.0);
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
TokenBucket sClientProjectileRate = { kProjectileRatePerSecond, kProjectileRateBurst, kProjectileRateBurst };
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

// Every element of the collider that took a hit this check (a blast can cover several spheres of one enemy).
std::vector<std::pair<ColliderElement*, uint8_t>> HitElements(Collider* col) {
    std::vector<std::pair<ColliderElement*, uint8_t>> out;
    if (col->shape == COLSHAPE_JNTSPH) {
        auto* j = reinterpret_cast<ColliderJntSph*>(col);
        for (s32 i = 0; i < j->count; i++) {
            if (j->elements[i].base.acElemFlags & ACELEM_HIT) {
                out.push_back({ &j->elements[i].base, static_cast<uint8_t>(i) });
            }
        }
    } else if (col->shape == COLSHAPE_TRIS) {
        auto* tris = reinterpret_cast<ColliderTris*>(col);
        for (s32 i = 0; i < tris->count; i++) {
            if (tris->elements[i].base.acElemFlags & ACELEM_HIT) {
                out.push_back({ &tris->elements[i].base, static_cast<uint8_t>(i) });
            }
        }
    } else {
        uint8_t index = 0;
        if (ColliderElement* elem = HitElement(col, index)) {
            out.push_back({ elem, index });
        }
    }
    return out;
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
        if (!(col->acFlags & AC_HIT) || col->ac == nullptr) {
            return;
        }
        // The attacker is the local Link or one of its projectiles still alive (only compared until it is known to
        // be one: a projectile deleted since is no longer in the owned list and its hit is not reported).
        const bool byLink = col->ac == &player->actor;
        const uint16_t projectileId = byLink ? 0 : Projectiles::OwnedId(col->ac);
        if ((!byLink && projectileId == 0) || (byLink && hitThisFrame.count(netId) != 0)) {
            return;
        }
        // The Link's attacks: the first element hit. A projectile: every element it covered this frame, since the
        // enemy takes damage for each of them in one check (a blast on a Deku Baba's head and stem).
        std::vector<std::pair<ColliderElement*, uint8_t>> elems = HitElements(col);
        if (byLink && elems.size() > 1) {
            elems.resize(1);
        }
        if (elems.empty() || elems[0].first->acHit == nullptr || elems[0].first->acHitElem == nullptr) {
            return;
        }
        const ColliderElement* first = elems[0].first;
        Kind kind;
        if (!byLink) {
            kind = Kind::Projectile;
            // A magic arrow's flames, a bomb's body: not an attack on enemies.
            if ((first->acHitElem->atDmgInfo.dmgFlags & Projectiles::HitFlags(col->ac)) == 0) {
                return;
            }
        } else if (first->acHit == &player->meleeWeaponQuads[0].base ||
                   first->acHit == &player->meleeWeaponQuads[1].base) {
            kind = Kind::Melee;
        } else if (first->acHit == &player->cylinder.base) {
            kind = Kind::Body;
        } else if (first->acHit == &player->shieldCylinder.base) {
            kind = Kind::Barrier;
        } else {
            return; // Shield bash and anything else: not an attack on the enemy
        }
        if (first->acHitElem->atDmgInfo.dmgFlags == 0) {
            return; // An attack collider left on with no damage (the end of a spin): no attack in vanilla either
        }
        const uint16_t instance = kind == Kind::Projectile ? projectileId : kind == Kind::Melee ? sSwing : sBodyAttack;
        // A projectile's hits go out on every frame the engine finds them (a blast or a fin stays out several frames;
        // the enemy's own invulnerability decides whether a second frame counts). The Link's, once per swing.
        if (kind != Kind::Projectile) {
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
        }

        for (const auto& [elem, index] : elems) {
            if (elem->acHitElem == nullptr) {
                continue;
            }
            if (kind == Kind::Projectile && !sClientProjectileRate.Take(now)) {
                sClientStats.rateDropped++;
                continue;
            }
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
            msg.colliderOffset =
                static_cast<uint16_t>(reinterpret_cast<uint8_t*>(col) - reinterpret_cast<uint8_t*>(actor));
            msg.element = index;
            msg.bounced = (col->acFlags & AC_BOUNCED) ? 1 : 0;
            msg.hit[0] = static_cast<int16_t>(elem->acDmgInfo.hitPos.x - std::lround(actor->world.pos.x));
            msg.hit[1] = static_cast<int16_t>(elem->acDmgInfo.hitPos.y - std::lround(actor->world.pos.y));
            msg.hit[2] = static_cast<int16_t>(elem->acDmgInfo.hitPos.z - std::lround(actor->world.pos.z));
            msg.yaw = byLink ? player->actor.shape.rot.y : col->ac->world.rot.y;
            msg.instance = instance;
            const std::vector<uint8_t> bytes = Message(MsgType::Hit, &msg, sizeof(msg));
            SendBinary(kHostPlayerId, Protocol::kChannelHits, bytes.data(), bytes.size());
            sClientStats.sent++;
            if (kind == Kind::Projectile) {
                Projectiles::NoteHitSent(projectileId);
            }
            SPDLOG_INFO("[Coop] hit-send: seq {} net {} {} {} form {} dmg 0x{:X} damage {} effect {}{}{}", msg.seq,
                        netId, Enemies::TypeName(actor->id), kKindNames[msg.kind], msg.form, msg.dmgFlags, msg.damage,
                        msg.effect, msg.bounced ? " (bounced here)" : "",
                        kind == Kind::Projectile
                            ? fmt::format(" by {} id {}", Enemies::TypeName(col->ac->id), projectileId)
                            : std::string());
            // Only a real hit on a collider the host had active, never a bounce or a hard collider.
            if (!msg.bounced && !(col->acFlags & AC_HARD)) {
                Predict(actor, msg.seq, netId, now);
            }
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
    std::deque<Queued> projectileQueue;
    // (netId, projectile id, clientMs) and (collider, element) of projectile hits already queued
    std::deque<std::pair<uint64_t, uint32_t>> projectileRecent;
    std::deque<uint64_t> recent; // (netId, kind, instance) of melee hits already queued
    int64_t minOffset = INT64_MAX;
    TokenBucket rate;
    TokenBucket projectileRate = { kProjectileRatePerSecond, kProjectileRateBurst, kProjectileRateBurst };
};

struct Recast {
    uint32_t playerId;
    Player* puppet; // nullptr: a projectile hit, replayed with a quad of sProjectileQuads
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
std::array<ColliderQuad, kProjectileQuads> sProjectileQuads;
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
    if (m.kind == static_cast<uint8_t>(Kind::Projectile)) {
        Projectiles::CopyView view;
        if (!Projectiles::HostCopy(playerId, m.instance, view)) {
            return Reason::NoProjectile;
        }
        if ((m.dmgFlags & view.hitFlags) == 0) {
            return Reason::BadData;
        }
        const Vec3f& p = view.lastPos;
        const Vec3f& t = target->world.pos;
        return Length(p.x - t.x, p.y - t.y, p.z - t.z) > view.reach ? Reason::TooFar : Reason::None;
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
    const bool projectile = m.kind == static_cast<uint8_t>(Kind::Projectile);
    if (projectile) {
        Projectiles::HostHitArrived(from, m.instance); // Counted against the hitsSent of its Kill, whatever happens
    }
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
    if (projectile) {
        // The client reports a projectile's hit on each element of an enemy at most once per frame (its clock moves
        // every frame).
        const std::pair<uint64_t, uint32_t> key = { (static_cast<uint64_t>(m.netId) << 48) |
                                                        (static_cast<uint64_t>(m.instance) << 32) | m.clientMs,
                                                    (static_cast<uint32_t>(m.colliderOffset) << 8) | m.element };
        if (std::find(player.projectileRecent.begin(), player.projectileRecent.end(), key) !=
            player.projectileRecent.end()) {
            return Reject(from, m, Reason::Duplicate);
        }
        player.projectileRecent.push_back(key);
        while (player.projectileRecent.size() > 128) {
            player.projectileRecent.pop_front();
        }
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
    if (!(projectile ? player.projectileRate : player.rate).Take(now)) {
        return Reject(from, m, Reason::RateLimit);
    }
    Actor* target = nullptr;
    const Reason reason = CheckHit(from, m, target);
    if (reason != Reason::None) {
        return Reject(from, m, reason);
    }
    if (projectile) {
        player.projectileQueue.push_back({ m, now });
        Projectiles::HostHitQueued(from, m.instance); // The copy stays until it is replayed
    } else {
        player.queue.push_back({ m, now });
    }
}

// Current center of the element the client hit, if `offset` really is one of the enemy's colliders.
bool ElementCenter(Actor* target, uint16_t offset, uint8_t element, Vec3f& out) {
    if (target->overlayEntry == nullptr || target->overlayEntry->profile == nullptr ||
        static_cast<size_t>(offset) + sizeof(Collider) > target->overlayEntry->profile->instanceSize) {
        return false;
    }
    Collider* col = reinterpret_cast<Collider*>(reinterpret_cast<uint8_t*>(target) + offset);
    if (col->actor != target ||
        Enemies::PuppetCollider(target, offset, col->shape,
                                col->shape == COLSHAPE_JNTSPH ? reinterpret_cast<ColliderJntSph*>(col)->count
                                : col->shape == COLSHAPE_TRIS ? reinterpret_cast<ColliderTris*>(col)->count
                                                              : 1) != col) {
        return false;
    }
    auto sphere = [&](const Sphere16& s) {
        out = { static_cast<f32>(s.center.x), static_cast<f32>(s.center.y), static_cast<f32>(s.center.z) };
    };
    switch (col->shape) {
        case COLSHAPE_CYLINDER: {
            const Cylinder16& d = reinterpret_cast<ColliderCylinder*>(col)->dim;
            out = { static_cast<f32>(d.pos.x), d.pos.y + d.yShift + d.height * 0.5f, static_cast<f32>(d.pos.z) };
            return true;
        }
        case COLSHAPE_SPHERE:
            sphere(reinterpret_cast<ColliderSphere*>(col)->dim.worldSphere);
            return true;
        case COLSHAPE_JNTSPH: {
            auto* j = reinterpret_cast<ColliderJntSph*>(col);
            if (element >= j->count) {
                return false;
            }
            sphere(j->elements[element].dim.worldSphere);
            return true;
        }
        case COLSHAPE_TRIS: {
            auto* tris = reinterpret_cast<ColliderTris*>(col);
            if (element >= tris->count) {
                return false;
            }
            const Vec3f* v = tris->elements[element].dim.vtx;
            out = { (v[0].x + v[1].x + v[2].x) / 3.0f, (v[0].y + v[1].y + v[2].y) / 3.0f,
                    (v[0].z + v[1].z + v[2].z) / 3.0f };
            return true;
        }
        case COLSHAPE_QUAD: {
            const Vec3f* q = reinterpret_cast<ColliderQuad*>(col)->dim.quad;
            out = { (q[0].x + q[1].x + q[2].x + q[3].x) / 4.0f, (q[0].y + q[1].y + q[2].y + q[3].y) / 4.0f,
                    (q[0].z + q[1].z + q[2].z + q[3].z) / 4.0f };
            return true;
        }
        default:
            return false;
    }
}

// A 30-unit square facing the attack, centered where the hit element is now (what this frame's check uses); the hit
// point relative to the enemy if that element cannot be found.
void PlaceAndRegister(ColliderQuad* quad, const HitMsg& m, Actor* target) {
    PlayState* play = gPlayState;
    Vec3f c = { target->world.pos.x + m.hit[0], target->world.pos.y + m.hit[1], target->world.pos.z + m.hit[2] };
    ElementCenter(target, m.colliderOffset, m.element, c);
    const Vec3f right = { Math_CosS(m.yaw) * 15.0f, 0.0f, -Math_SinS(m.yaw) * 15.0f };
    Vec3f v0 = { c.x + right.x, c.y + 15.0f, c.z + right.z };
    Vec3f v1 = { c.x - right.x, c.y + 15.0f, c.z - right.z };
    Vec3f v2 = { c.x + right.x, c.y - 15.0f, c.z + right.z };
    Vec3f v3 = { c.x - right.x, c.y - 15.0f, c.z - right.z };
    Collider_SetQuadVertices(quad, &v0, &v1, &v2, &v3);
    CollisionCheck_SetAT(play, &play->colChkCtx, &quad->base);
}

// The quad of the hitting player's puppet Link, placed at the hit point on the real enemy for this frame's check.
void StartRecast(uint32_t playerId, const HitMsg& m, Actor* target, int quadIndex) {
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
    PlaceAndRegister(quad, m, target);
    sRecasts.push_back(rc);
}

// A projectile hit: a quad of our own whose attacker is the projectile's copy, so the enemy reacts to the arrow, the
// bomb or the fin itself (knockback away from it, arrows that stick, explosion checks).
void StartProjectileRecast(uint32_t playerId, const HitMsg& m, Actor* target, const Projectiles::CopyView& view,
                           int quadIndex) {
    PlayState* play = gPlayState;
    ColliderQuad* quad = &sProjectileQuads[quadIndex];
    static ColliderQuadInit sInit = {
        { COL_MATERIAL_NONE, AT_ON | AT_TYPE_PLAYER, AC_NONE, OC1_NONE, OC2_TYPE_PLAYER, COLSHAPE_QUAD },
        { ELEM_MATERIAL_UNK0, { 0, 0, 0 }, { 0, 0, 0 }, ATELEM_ON, ACELEM_NONE, OCELEM_NONE },
        { { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } } },
    };
    Collider_InitQuad(play, quad);
    Collider_SetQuad(play, quad, view.actor, &sInit);
    quad->elem.elemMaterial = view.elemMaterial;
    quad->elem.atElemFlags = ATELEM_ON | view.atElemFlags;
    quad->elem.atDmgInfo.dmgFlags = m.dmgFlags;
    quad->elem.atDmgInfo.damage = m.damage;
    quad->elem.atDmgInfo.effect = m.effect;
    PlaceAndRegister(quad, m, target);
    Recast rc = {};
    rc.playerId = playerId;
    rc.puppet = nullptr;
    rc.quad = quad;
    rc.target = target;
    rc.msg = m;
    rc.healthBefore = static_cast<int16_t>(target->colChkInfo.health);
    sRecasts.push_back(rc);
}

// The previous frame's replays: what the engine made of them, then the quads go back to how they were.
void FinishRecasts() {
    PlayState* play = gPlayState;
    for (Recast& rc : sRecasts) {
        const bool projectile = rc.puppet == nullptr;
        if (!projectile && Puppet::PuppetOf(rc.playerId) != rc.puppet) {
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
        if (projectile) {
            rc.quad->base.atFlags = AT_NONE;
            rc.quad->base.actor = nullptr;
            Projectiles::HostHitFinished(rc.playerId, rc.msg.instance);
        } else {
            rc.quad->base.atFlags = rc.savedAtFlags;
            rc.quad->elem.atElemFlags = rc.savedAtElemFlags;
            rc.quad->elem.atDmgInfo = rc.savedDmg;
        }
        SPDLOG_INFO("[Coop] hit-apply: player {} seq {} net {} {}: {} (health {} -> {}{}){}", rc.playerId, rc.msg.seq,
                    rc.msg.netId, Enemies::TypeName(rc.target->id), kOutcomeNames[static_cast<uint8_t>(outcome)],
                    rc.healthBefore, after, died ? ", died" : "",
                    projectile ? fmt::format(" by projectile id {}", rc.msg.instance) : std::string());
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
    // Projectile hits, in order; one per enemy per frame (an enemy takes one hit per frame, and the client found
    // them in different frames even when they arrive together).
    int quads = 0;
    std::unordered_map<Actor*, std::pair<uint32_t, uint32_t>> targetsUsed; // -> (player, client frame time)
    for (auto& [playerId, player] : sHostPlayers) {
        std::deque<Queued> later;
        while (!player.projectileQueue.empty()) {
            const Queued q = player.projectileQueue.front();
            player.projectileQueue.pop_front();
            if (now - q.arrivalMs > kMaxAgeMs) {
                Reject(playerId, q.msg, Reason::Expired);
                Projectiles::HostHitFinished(playerId, q.msg.instance);
                continue;
            }
            Actor* target = nullptr;
            const Reason reason = CheckHit(playerId, q.msg, target);
            Projectiles::CopyView view;
            if (reason != Reason::None || !Projectiles::HostCopy(playerId, q.msg.instance, view)) {
                Reject(playerId, q.msg, reason != Reason::None ? reason : Reason::NoProjectile);
                Projectiles::HostHitFinished(playerId, q.msg.instance);
                continue;
            }
            // Hits the client found in the same frame (several elements of one enemy) are replayed together, so the
            // enemy adds their damage as the engine does; hits from later frames wait for a later frame.
            auto used = targetsUsed.find(target);
            const bool sameFrame =
                used != targetsUsed.end() && used->second == std::make_pair(playerId, q.msg.clientMs);
            if (quads >= kProjectileQuads || (used != targetsUsed.end() && !sameFrame)) {
                later.push_back(q); // Next frame, in order
                continue;
            }
            targetsUsed[target] = { playerId, q.msg.clientMs };
            StartProjectileRecast(playerId, q.msg, target, view, quads++);
        }
        player.projectileQueue = std::move(later);
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

std::unordered_set<uint32_t> sHostProjectileContacts; // (netId << 16 | projectile id)

void LogHostContact(Collider* at, Collider* ac) {
    Player* player = GET_PLAYER(gPlayState);
    if (!GetLaunchOptions().testEnemyLog || player == nullptr || ac->actor == nullptr || at->actor == nullptr) {
        return;
    }
    if (at->actor != &player->actor) {
        // The host's own projectiles (shared ones, and hits that can damage enemies).
        const uint16_t projectileId = Projectiles::IsCopy(at->actor) ? 0 : Projectiles::OwnedId(at->actor);
        const uint16_t netId = Enemies::HostNetIdOf(ac->actor);
        const ColliderElement* elem = nullptr;
        if (at->shape == COLSHAPE_QUAD) {
            elem = &reinterpret_cast<ColliderQuad*>(at)->elem;
        } else if (at->shape == COLSHAPE_CYLINDER) {
            elem = &reinterpret_cast<ColliderCylinder*>(at)->elem;
        } else if (at->shape == COLSHAPE_JNTSPH && reinterpret_cast<ColliderJntSph*>(at)->count > 0) {
            elem = &reinterpret_cast<ColliderJntSph*>(at)->elements[0].base;
        }
        if (projectileId == 0 || netId == 0 || elem == nullptr ||
            (elem->atDmgInfo.dmgFlags & Projectiles::HitFlags(at->actor)) == 0 ||
            !sHostProjectileContacts.insert((static_cast<uint32_t>(netId) << 16) | projectileId).second) {
            return;
        }
        SPDLOG_INFO("[Coop] host-contact: net {} {} projectile dmg 0x{:X} by {} id {}", netId,
                    Enemies::TypeName(ac->actor->id), elem->atDmgInfo.dmgFlags, Enemies::TypeName(at->actor->id),
                    projectileId);
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
// --coop-test-hit: one quad of the local Link on the nearest enemy, registered for this frame's check.
ColliderQuad sTestQuad;
Actor* sTestTarget = nullptr;

void OnAcCollision(GIVanillaBehavior, bool* should, va_list originalArgs) {
    va_list args;
    va_copy(args, originalArgs);
    Collider* at = va_arg(args, Collider*);
    Collider* ac = va_arg(args, Collider*);
    ColliderElement* atElem = va_arg(args, ColliderElement*);
    va_end(args);
    // Enemies hitting the other players' puppets (host) and the client's replays of those hits.
    PlayerHits::OnAcCollision(at, ac, atElem, should);
    if (!*should) {
        return;
    }
    if (at == &sTestQuad.base && ac->actor != sTestTarget) {
        *should = false;
        return;
    }
    if (IsHosting()) {
        LogHostContact(at, ac);
    }
    bool recast = false;
    for (const Recast& rc : sRecasts) {
        if (at == &rc.quad->base) {
            recast = true;
            if (ac->actor != rc.target) {
                *should = false;
            }
        }
    }
    // A copy of another player's projectile never hits anything by itself: its hits are the owner's to report.
    if (!recast && Projectiles::IsCopy(at->actor)) {
        *should = false;
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
    } else if (!IsHosting() && header.type == PlayerHits::kMsgPlayerHit && from == kHostPlayerId) {
        PlayerHits::ClientOnPlayerHit(r);
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
        player.projectileQueue.clear();
    }
    sHostProjectileContacts.clear();
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

std::vector<uint8_t> HitsMessage(uint8_t type, const void* body, size_t size) {
    return Message(static_cast<MsgType>(type), body, size);
}

void TestHitNearest(uint32_t dmgFlags) {
    PlayState* play = gPlayState;
    Player* player = GET_PLAYER(play);
    // The nearest enemy actor, tracked or not (with no client the host tracks nothing).
    Actor* target = nullptr;
    float best = 1e9f;
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ENEMY].first; a != nullptr && player != nullptr; a = a->next) {
        const float d = Actor_WorldDistXYZToActor(&player->actor, a);
        if (a->update != nullptr && a->init == nullptr && a->draw != nullptr && d < best) {
            best = d;
            target = a;
        }
    }
    if (target == nullptr || !IsHosting()) {
        SPDLOG_INFO("[Coop] TEST: hit: no enemy");
        return;
    }
    static ColliderQuadInit sInit = {
        { COL_MATERIAL_NONE, AT_ON | AT_TYPE_PLAYER, AC_NONE, OC1_NONE, OC2_TYPE_PLAYER, COLSHAPE_QUAD },
        { ELEM_MATERIAL_UNK2, { 0, 0, 0 }, { 0, 0, 0 }, ATELEM_ON | ATELEM_NEAREST, ACELEM_NONE, OCELEM_NONE },
        { { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } } },
    };
    Collider_InitQuad(play, &sTestQuad);
    Collider_SetQuad(play, &sTestQuad, &player->actor, &sInit);
    sTestQuad.elem.atDmgInfo.dmgFlags = dmgFlags;
    sTestQuad.elem.atDmgInfo.damage = 1;
    sTestTarget = target;
    // A 60-unit square between its feet and its focus, facing the Link.
    Vec3f c = { (target->world.pos.x + target->focus.pos.x) * 0.5f,
                (target->world.pos.y + target->focus.pos.y) * 0.5f + 10.0f,
                (target->world.pos.z + target->focus.pos.z) * 0.5f };
    const s16 yaw = Math_Vec3f_Yaw(&player->actor.world.pos, &c);
    const Vec3f right = { Math_CosS(yaw) * 30.0f, 0.0f, -Math_SinS(yaw) * 30.0f };
    Vec3f v0 = { c.x + right.x, c.y + 30.0f, c.z + right.z };
    Vec3f v1 = { c.x - right.x, c.y + 30.0f, c.z - right.z };
    Vec3f v2 = { c.x + right.x, c.y - 30.0f, c.z + right.z };
    Vec3f v3 = { c.x - right.x, c.y - 30.0f, c.z - right.z };
    Collider_SetQuadVertices(&sTestQuad, &v0, &v1, &v2, &v3);
    CollisionCheck_SetAT(play, &play->colChkCtx, &sTestQuad.base);
    SPDLOG_INFO("[Coop] TEST: hit {} with 0x{:X} (health {})", Enemies::TypeName(target->id), dmgFlags,
                target->colChkInfo.health);
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
    m.hit[0] = 0;
    m.kind = static_cast<uint8_t>(Kind::Projectile);
    m.dmgFlags = DMG_SWORD;
    send("projectile hit with sword damage", Message(MsgType::Hit, &m, sizeof(m)));
    m.dmgFlags = DMG_NORMAL_ARROW;
    m.instance = 0xFFFF;
    send("hit by a projectile that does not exist", Message(MsgType::Hit, &m, sizeof(m)));
    ResultMsg res = {};
    res.outcome = 9;
    send("result with an unknown outcome", Message(MsgType::Result, &res, sizeof(res)));
    std::vector<uint8_t> cut = Message(MsgType::Hit, &m, sizeof(m));
    cut.resize(cut.size() - 5);
    send("hit cut short", cut);
}

static RegisterShipInitFunc initFunc(RegisterCombatHooks);

} // namespace Coop::Combat
