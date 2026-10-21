#include "CoopProjectiles.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopEnemies.h"
#include "CoopEnemyTypes.h"
#include "CoopEnemyWire.h"
#include "CoopProjectileTypes.h"
#include "CoopProjectileWire.h"
#include "CoopSession.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "overlays/actors/ovl_En_Arrow/z_en_arrow.h"
#include "overlays/actors/ovl_En_Bom/z_en_bom.h"
#include "overlays/actors/ovl_En_Bom_Chu/z_en_bom_chu.h"
#include "overlays/actors/ovl_En_Boom/z_en_boom.h"
}

namespace Coop::Projectiles {

namespace {

using SteadyClock = std::chrono::steady_clock;
using Enemies::TypeName;
using Enemies::Wire::Reader;
using Enemies::Wire::Writer;
using namespace Wire;

constexpr size_t kMaxLivePerOwner = 32;   // Shared projectiles of one owner at a time (10 arrows + 5 bombs fit)
constexpr size_t kMaxCopies = 64;         // Copies on this game, all owners
constexpr uint32_t kStateIntervalMs = 50; // 20 Hz
constexpr size_t kFullRateCount = 12;     // Projectiles that get the full rate
constexpr uint32_t kKeyframeMs = 500;     // A state even when nothing changed
constexpr uint32_t kMinDelayMs = 100;
constexpr uint32_t kMaxDelayMs = 250;
constexpr uint32_t kHistoryMs = 1500;
constexpr uint32_t kStaleMs = 3000;    // A copy with no news for this long is dropped
constexpr uint32_t kKillWaitMs = 1000; // Host: longest wait for the hits a Kill announces
constexpr uint32_t kStatsIntervalMs = 10000;
constexpr float kMaxCoord = 32000.0f;

const SteadyClock::time_point sEpoch = SteadyClock::now();

uint32_t NowMs() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - sEpoch).count());
}

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

bool SessionActive() {
    return IsHosting() || IsClientInSession();
}

// Playing in the session's world right now (a client in the session may still be in its own title screen).
bool Playing() {
    return InGameplay() && (IsHosting() || IsClientPlayingInSession());
}

uint32_t LocalId() {
    return IsHosting() ? kHostPlayerId : GetSnapshot().localId;
}

uint32_t Key(uint32_t owner, uint16_t id) {
    return (owner << 16) | id;
}

int16_t Round16(float f) {
    return static_cast<int16_t>(std::clamp(std::lround(f), -32767L, 32767L));
}

std::vector<uint8_t> Message(Type type, uint8_t owner, const void* body, size_t size) {
    Writer w;
    w.Put(Header{ kMagic, kFormatVersion, static_cast<uint8_t>(type), owner, 0 });
    w.Bytes(body, size);
    return std::move(w.buffer);
}

struct Stats {
    uint64_t sentBytes = 0;
    uint32_t spawnsSent = 0;
    uint32_t statesSent = 0; // Records
    uint32_t killsSent = 0;
    uint64_t receivedBytes = 0;
    uint32_t refusedCap = 0;    // Owner: not shared, too many alive
    uint32_t droppedCopies = 0; // Copies refused (cap, bad data) or lost
    size_t maxOwned = 0;
    size_t maxCopies = 0;
    uint64_t peakOutPerSecond = 0; // Busiest one-second window
    uint64_t peakInPerSecond = 0;
};
Stats sStats;
uint32_t sStatsMs = 0;

// Bytes in the current one-second window, for the peaks of the bandwidth line.
uint32_t sSecondStartMs = 0;
uint64_t sSecondOut = 0;
uint64_t sSecondIn = 0;

void CountBytes(uint64_t out, uint64_t in) {
    const uint32_t now = NowMs();
    if (now - sSecondStartMs >= 1000) {
        sSecondStartMs = now;
        sSecondOut = 0;
        sSecondIn = 0;
    }
    sSecondOut += out;
    sSecondIn += in;
    sStats.sentBytes += out;
    sStats.receivedBytes += in;
    sStats.peakOutPerSecond = std::max(sStats.peakOutPerSecond, sSecondOut);
    sStats.peakInPerSecond = std::max(sStats.peakInPerSecond, sSecondIn);
}

void Drop(const char* what, const char* why) {
    SPDLOG_WARN("[Coop] Dropping projectile {}: {}", what, why);
    NoteDroppedMessage();
    sStats.droppedCopies++;
}

// MARK: - Who is updating (ownership of what gets spawned)

Actor* sUpdating = nullptr;
bool sSpawningCopy = false;
bool sForceOwned = false; // TestShoot

void OnShouldActorUpdate(Actor* actor, bool*) {
    sUpdating = actor;
}

void OnActorUpdate(Actor*) {
    sUpdating = nullptr;
}

// MARK: - Owner

struct Owned {
    Actor* actor = nullptr; // nullptr once deleted
    const TypeInfo* type = nullptr;
    uint16_t id = 0; // 0 until shared
    uint16_t parentId = 0;
    bool dead = false;
    bool refused = false;
    uint16_t hitsSent = 0;
    uint32_t lastSentMs = 0;
    std::array<uint8_t, sizeof(StateRecord) + kMaxExtraSize> lastBytes = {};
    size_t lastSize = 0;
};

std::vector<Owned> sOwned; // In spawn order: a parent is always before what it spawned
uint16_t sNextId = 1;
uint16_t sStatesSeq = 0;
uint32_t sLastStatesMs = 0;
uint32_t sLastCapLogMs = 0;

Owned* FindOwned(const Actor* actor) {
    for (Owned& o : sOwned) {
        if (o.actor == actor && actor != nullptr) {
            return &o;
        }
    }
    return nullptr;
}

Owned* FindOwnedId(uint16_t id) {
    for (Owned& o : sOwned) {
        if (o.id == id && id != 0) {
            return &o;
        }
    }
    return nullptr;
}

void OwnerSend(Type type, const void* body, size_t size) {
    const std::vector<uint8_t> bytes = Message(type, static_cast<uint8_t>(LocalId()), body, size);
    SendBinary(IsHosting() ? kBroadcast : kHostPlayerId, Protocol::kChannelProjectiles, bytes.data(), bytes.size());
    CountBytes(bytes.size() + Protocol::kBinaryHeaderSize, 0);
}

void OnActorSpawn(Actor* actor) {
    if (sSpawningCopy || !SessionActive() || !InGameplay()) {
        return;
    }
    const TypeInfo* type = FindType(actor->id);
    Player* local = GET_PLAYER(gPlayState);
    if (type == nullptr || local == nullptr) {
        return;
    }
    const Actor* spawner = sUpdating;
    const bool byLocalLink =
        type->fromPlayer && spawner != nullptr && (spawner == &local->actor || FindOwned(spawner) != nullptr);
    const bool byEnemy = IsHosting() && type->fromEnemy && spawner != nullptr &&
                         (spawner->category == ACTORCAT_ENEMY || spawner->category == ACTORCAT_BOSS);
    if (sForceOwned || byLocalLink || byEnemy) {
        Owned o;
        o.actor = actor;
        o.type = type;
        sOwned.push_back(o);
    }
}

size_t SharedAlive() {
    size_t n = 0;
    for (const Owned& o : sOwned) {
        n += (o.id != 0 && !o.dead) ? 1 : 0;
    }
    return n;
}

uint16_t AllocId() {
    static bool sFirstIdSet = false;
    if (!sFirstIdSet) {
        sFirstIdSet = true;
        if (GetLaunchOptions().testProjectileFirstId > 0) {
            sNextId = static_cast<uint16_t>(GetLaunchOptions().testProjectileFirstId);
            SPDLOG_INFO("[Coop] TEST: projectile ids start at {}", sNextId);
        }
    }
    // The counter wraps; an id still in use is skipped, never reused while its projectile lives.
    for (int tries = 0; tries < 0x10000; tries++) {
        const uint16_t id = sNextId++;
        if (sNextId == 0) {
            sNextId = 1;
        }
        if (id != 0 && FindOwnedId(id) == nullptr) {
            return id;
        }
        if (id != 0) {
            SPDLOG_INFO("[Coop] proj-id: {} is still alive, skipped", id);
        }
    }
    return 0;
}

bool Announce(Owned& o) {
    if (o.id != 0 || o.refused || o.dead || o.actor == nullptr) {
        return o.id != 0;
    }
    Actor* a = o.actor;
    if (!o.type->shareable(a)) {
        return false;
    }
    uint16_t parentId = 0;
    if (a->id == ACTOR_ARROW_FIRE || a->id == ACTOR_ARROW_ICE || a->id == ACTOR_ARROW_LIGHT) {
        Owned* parent = FindOwned(a->parent);
        if (parent == nullptr || !Announce(*parent)) {
            return false; // Drawn around its arrow: shared once the arrow is
        }
        parentId = parent->id;
    }
    if (SharedAlive() >= kMaxLivePerOwner) {
        o.refused = true;
        sStats.refusedCap++;
        const uint32_t now = NowMs();
        if (now - sLastCapLogMs >= 1000 || sLastCapLogMs == 0) {
            sLastCapLogMs = now;
            SPDLOG_WARN("[Coop] projectile-cap: {} not shared, {} of this player's projectiles are already alive "
                        "(max {})",
                        TypeName(a->id), kMaxLivePerOwner, kMaxLivePerOwner);
        }
        return false;
    }
    o.id = AllocId();
    if (o.id == 0) {
        o.refused = true;
        return false;
    }
    o.parentId = parentId;
    SpawnMsg msg = {};
    msg.id = o.id;
    msg.parentId = parentId;
    msg.actorId = a->id;
    msg.params = static_cast<uint16_t>(a->params);
    msg.sceneId = gPlayState->sceneId;
    msg.sceneLayer = static_cast<int8_t>(gSaveContext.sceneLayer);
    msg.sentMs = NowMs();
    msg.pos[0] = a->world.pos.x;
    msg.pos[1] = a->world.pos.y;
    msg.pos[2] = a->world.pos.z;
    msg.shapeRot[0] = a->shape.rot.x;
    msg.shapeRot[1] = a->shape.rot.y;
    msg.shapeRot[2] = a->shape.rot.z;
    msg.worldRotY = a->world.rot.y;
    OwnerSend(Type::Spawn, &msg, sizeof(msg));
    sStats.spawnsSent++;
    sStats.maxOwned = std::max(sStats.maxOwned, SharedAlive());
    SPDLOG_INFO("[Coop] proj-spawn: owner {} id {} {} params {} at ({:.0f}, {:.0f}, {:.0f}){}", LocalId(), o.id,
                TypeName(a->id), a->params, a->world.pos.x, a->world.pos.y, a->world.pos.z,
                parentId != 0 ? fmt::format(" on {}", parentId) : std::string());
    return true;
}

void SendKill(Owned& o, KillReason reason) {
    KillMsg msg = {};
    msg.id = o.id;
    msg.reason = static_cast<uint8_t>(reason);
    msg.hitsSent = o.hitsSent;
    msg.sentMs = NowMs();
    OwnerSend(Type::Kill, &msg, sizeof(msg));
    sStats.killsSent++;
    SPDLOG_INFO("[Coop] proj-kill: owner {} id {} ({}), {} hit(s) sent", LocalId(), o.id,
                reason == KillReason::Gone ? "gone" : "left", o.hitsSent);
}

size_t EncodeState(const Owned& o, uint8_t* out) {
    const Actor* a = o.actor;
    StateRecord r = {};
    r.id = o.id;
    r.flags = (a->draw == nullptr ? kStateHidden : 0) | (a->velocity.y != 0.0f ? kStateAirborne : 0);
    r.extraSize = o.type->extraSize;
    r.pos[0] = Round16(a->world.pos.x);
    r.pos[1] = Round16(a->world.pos.y);
    r.pos[2] = Round16(a->world.pos.z);
    r.shapeRot[0] = a->shape.rot.x;
    r.shapeRot[1] = a->shape.rot.y;
    r.shapeRot[2] = a->shape.rot.z;
    r.worldRotY = a->world.rot.y;
    r.speed = Round16(a->speed * 16.0f);
    r.scale = static_cast<uint16_t>(std::clamp(a->scale.x * 10000.0f, 0.0f, 65535.0f));
    std::memcpy(out, &r, sizeof(r));
    o.type->capture(a, out + sizeof(r));
    return sizeof(r) + o.type->extraSize;
}

void SendStates(uint32_t now) {
    // 20 Hz up to kFullRateCount projectiles; with more alive, less often, so they stay within the budget (32 arrows
    // in the air: every 150 ms).
    const size_t alive = SharedAlive();
    const uint32_t interval =
        kStateIntervalMs * static_cast<uint32_t>(std::max<size_t>(1, (alive + kFullRateCount - 1) / kFullRateCount));
    if (now - sLastStatesMs < interval) {
        return;
    }
    sLastStatesMs = now;
    Writer w;
    uint8_t count = 0;
    std::vector<uint8_t> records;
    for (Owned& o : sOwned) {
        if (o.id == 0 || o.dead || o.actor == nullptr || count >= kMaxRecords) {
            continue;
        }
        std::array<uint8_t, sizeof(StateRecord) + kMaxExtraSize> bytes = {};
        const size_t size = EncodeState(o, bytes.data());
        if (size == o.lastSize && std::memcmp(bytes.data(), o.lastBytes.data(), size) == 0 &&
            now - o.lastSentMs < kKeyframeMs) {
            continue;
        }
        o.lastBytes = bytes;
        o.lastSize = size;
        o.lastSentMs = now;
        records.insert(records.end(), bytes.begin(), bytes.begin() + size);
        count++;
    }
    if (count == 0) {
        return;
    }
    w.Put(StatesHeader{ now, ++sStatesSeq, count, 0 });
    w.Bytes(records.data(), records.size());
    OwnerSend(Type::States, w.buffer.data(), w.buffer.size());
    sStats.statesSent += count;
}

// Client: on the host a bomb or a bombchu blows up when it bumps into an enemy (their OC colliders touch). The
// enemy puppets here only have the colliders they can be hit on, so that contact is checked by hand against the
// nearest puppet's body (its colChkInfo cylinder) and the projectile is made to blow up as its own update would.
void TouchPuppets(Owned& o) {
    Actor* a = o.actor;
    bool armed = false;
    float radius = 0.0f;
    if (a->id == ACTOR_EN_BOM) {
        const EnBom* bomb = reinterpret_cast<const EnBom*>(a);
        armed = a->params == BOMB_TYPE_BODY && !bomb->isPowderKeg && a->parent == nullptr && bomb->timer > 0 &&
                bomb->unk_1F8 != 0;
        radius = 6.0f;
    } else if (a->id == ACTOR_EN_BOM_CHU) {
        armed = reinterpret_cast<const EnBomChu*>(a)->isMoving && reinterpret_cast<const EnBomChu*>(a)->timer > 1;
        radius = 13.0f;
    }
    if (!armed) {
        return;
    }
    Actor* enemy = Enemies::NearestEnemy(a->world.pos, false);
    if (enemy == nullptr) {
        return;
    }
    const float cylRadius = enemy->colChkInfo.cylRadius > 0 ? enemy->colChkInfo.cylRadius : 20.0f;
    const float cylHeight = enemy->colChkInfo.cylHeight > 0 ? enemy->colChkInfo.cylHeight : 40.0f;
    const float dx = a->world.pos.x - enemy->world.pos.x;
    const float dz = a->world.pos.z - enemy->world.pos.z;
    const float dy = a->world.pos.y - enemy->world.pos.y;
    if (std::sqrt(dx * dx + dz * dz) > cylRadius + radius || dy < -radius || dy > cylHeight + radius) {
        return;
    }
    if (a->id == ACTOR_EN_BOM) {
        reinterpret_cast<EnBom*>(a)->timer = 0; // EnBom_Update blows it up this frame
        a->shape.rot.z = 0;
    } else {
        reinterpret_cast<EnBomChu*>(a)->timer = 1; // Counts down to 0 and blows up
    }
    SPDLOG_INFO("[Coop] proj-contact: id {} {} touched the puppet {} here", o.id, TypeName(a->id), TypeName(enemy->id));
}

void OwnerFrame(uint32_t now) {
    for (Owned& o : sOwned) {
        if (o.actor != nullptr && o.actor->update == nullptr) {
            o.dead = true; // Killed this frame or earlier; deleted soon
        }
        if (!o.dead) {
            Announce(o);
            if (!IsHosting() && o.id != 0) {
                TouchPuppets(o);
            }
        }
    }
    for (auto it = sOwned.begin(); it != sOwned.end();) {
        if (it->dead) {
            if (it->id != 0) {
                SendKill(*it, KillReason::Gone);
            }
            it = sOwned.erase(it);
        } else {
            ++it;
        }
    }
    SendStates(now);
}

// MARK: - Copies

struct Sample {
    uint32_t sentMs = 0;
    StateRecord r = {};
    std::array<uint8_t, kMaxExtraSize> extra = {};
};

struct Copy {
    uint32_t owner = 0;
    uint16_t id = 0;
    const TypeInfo* type = nullptr;
    Actor* actor = nullptr;
    uint32_t spawnMs = 0;    // Owner's clock
    uint32_t lastNewsMs = 0; // Local clock
    std::deque<Sample> history;
    Vec3f lastPos = {};
    bool shown = false;
    HOOK_ID shouldUpdate = 0;
    HOOK_ID shouldDraw = 0;
    // Kill
    bool killed = false;
    uint32_t killMs = 0;        // Owner's clock
    uint32_t killArrivalMs = 0; // Local clock
    uint16_t hitsSent = 0;
    uint16_t hitsArrived = 0;
    uint16_t pending = 0;
};

std::unordered_map<uint32_t, Copy> sCopies;
std::unordered_map<const Actor*, uint32_t> sCopyByActor;

// Owner's clock as seen from here: smallest arrival-minus-send offset over the last two seconds, plus jitter.
struct OwnerClock {
    std::deque<std::pair<uint32_t, int64_t>> offsets;
    bool havePrev = false;
    uint32_t prevArrivalMs = 0;
    uint32_t prevSentMs = 0;
    double jitterMs = 0.0;
    uint32_t delayMs = kMinDelayMs;
};
std::unordered_map<uint32_t, OwnerClock> sClocks;

void UpdateClock(uint32_t owner, uint32_t sentMs) {
    OwnerClock& c = sClocks[owner];
    const uint32_t now = NowMs();
    c.offsets.push_back({ now, static_cast<int64_t>(now) - sentMs });
    while (c.offsets.size() > 1 && now - c.offsets.front().first > 2000) {
        c.offsets.pop_front();
    }
    if (c.havePrev) {
        const double transit = static_cast<double>(static_cast<int32_t>(now - c.prevArrivalMs)) -
                               static_cast<double>(static_cast<int32_t>(sentMs - c.prevSentMs));
        c.jitterMs += (std::abs(transit) - c.jitterMs) / 16.0;
    }
    c.havePrev = true;
    c.prevArrivalMs = now;
    c.prevSentMs = sentMs;
    c.delayMs = std::clamp<uint32_t>(kMinDelayMs + static_cast<uint32_t>(3.0 * c.jitterMs), kMinDelayMs, kMaxDelayMs);
}

// The owner's time this game is showing now, or false if nothing arrived from it yet.
bool RenderTime(uint32_t owner, uint32_t now, int64_t& target) {
    auto it = sClocks.find(owner);
    if (it == sClocks.end() || it->second.offsets.empty()) {
        return false;
    }
    int64_t offset = INT64_MAX;
    for (const auto& [arrival, o] : it->second.offsets) {
        offset = std::min(offset, o);
    }
    target = static_cast<int64_t>(now) - offset - it->second.delayMs;
    return true;
}

void OnCopyShouldUpdate(Actor*, bool* should) {
    *should = false;
}

void OnCopyShouldDraw(Actor* actor, bool* should) {
    auto key = sCopyByActor.find(actor);
    if (key == sCopyByActor.end()) {
        return;
    }
    auto it = sCopies.find(key->second);
    if (it != sCopies.end() && !it->second.shown) {
        *should = false;
    }
}

size_t CopiesOf(uint32_t owner) {
    size_t n = 0;
    for (const auto& [key, c] : sCopies) {
        n += c.owner == owner ? 1 : 0;
    }
    return n;
}

void RemoveCopy(uint32_t key, const char* why) {
    auto it = sCopies.find(key);
    if (it == sCopies.end()) {
        return;
    }
    Copy& c = it->second;
    if (c.actor != nullptr) {
        auto* gi = GameInteractor::Instance;
        gi->UnregisterGameHookForPtr<GameInteractor::ShouldActorUpdate>(c.shouldUpdate);
        gi->UnregisterGameHookForPtr<GameInteractor::ShouldActorDraw>(c.shouldDraw);
        sCopyByActor.erase(c.actor);
        Actor_Kill(c.actor);
    }
    if (why != nullptr) {
        SPDLOG_INFO("[Coop] proj-copy-removed: owner {} id {} {} ({})", c.owner, c.id, TypeName(c.type->actorId), why);
    }
    sCopies.erase(it);
}

Actor* SpawnCopy(const TypeInfo* type, const SpawnMsg& m, Actor* parent) {
    PlayState* play = gPlayState;
    // En_Bom's Init clears the powder keg timer kept in the save context: the copy leaves it as it was.
    const u16 kegTimer = gSaveContext.powderKegTimer;
    sSpawningCopy = true;
    Actor* actor = parent != nullptr ? Actor_SpawnAsChild(&play->actorCtx, parent, play, m.actorId, m.pos[0], m.pos[1],
                                                          m.pos[2], 0, m.worldRotY, 0, static_cast<s16>(m.params))
                                     : Actor_Spawn(&play->actorCtx, play, m.actorId, m.pos[0], m.pos[1], m.pos[2], 0,
                                                   m.worldRotY, 0, static_cast<s16>(m.params));
    sSpawningCopy = false;
    gSaveContext.powderKegTimer = kegTimer;
    if (actor == nullptr || actor->update == nullptr) {
        return nullptr;
    }
    actor->shape.rot = { m.shapeRot[0], m.shapeRot[1], m.shapeRot[2] };
    actor->flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
    type->prepare(actor);
    return actor;
}

bool SameSceneAsHere(int16_t sceneId, int8_t layer) {
    return Playing() && gPlayState->sceneId == sceneId && static_cast<int8_t>(gSaveContext.sceneLayer) == layer;
}

void OnSpawn(uint32_t owner, Reader& r) {
    const SpawnMsg m = r.Get<SpawnMsg>();
    if (!r.ok || r.Remaining() != 0) {
        return Drop("spawn", "wrong size");
    }
    const TypeInfo* type = FindType(m.actorId);
    const Vec3f pos = { m.pos[0], m.pos[1], m.pos[2] };
    if (type == nullptr || m.id == 0 || !type->validParams(m.params) ||
        !(std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z)) || std::fabs(pos.x) > kMaxCoord ||
        std::fabs(pos.y) > kMaxCoord || std::fabs(pos.z) > kMaxCoord) {
        return Drop("spawn", "unknown type, bad params or position");
    }
    if (owner != kHostPlayerId && !type->fromPlayer) {
        return Drop("spawn", "a type only the host's enemies spawn");
    }
    UpdateClock(owner, m.sentMs);
    if (!SameSceneAsHere(m.sceneId, m.sceneLayer)) {
        return; // Its owner is elsewhere; nothing to show here
    }
    const uint32_t key = Key(owner, m.id);
    if (sCopies.count(key) != 0) {
        return Drop("spawn", "id already in use");
    }
    if (CopiesOf(owner) >= kMaxLivePerOwner || sCopies.size() >= kMaxCopies) {
        SPDLOG_WARN("[Coop] projectile-cap: copy of owner {} id {} ({}) refused, {} copies here", owner, m.id,
                    TypeName(m.actorId), sCopies.size());
        sStats.droppedCopies++;
        return;
    }
    Actor* parent = nullptr;
    if (m.parentId != 0) {
        auto p = sCopies.find(Key(owner, m.parentId));
        if (p == sCopies.end() || p->second.actor == nullptr) {
            return Drop("spawn", "parent projectile unknown");
        }
        parent = p->second.actor;
    }
    Actor* actor = SpawnCopy(type, m, parent);
    if (actor == nullptr) {
        SPDLOG_WARN("[Coop] Could not create the copy of owner {} id {} ({})", owner, m.id, TypeName(m.actorId));
        sStats.droppedCopies++;
        return;
    }
    Copy c;
    c.owner = owner;
    c.id = m.id;
    c.type = type;
    c.actor = actor;
    c.spawnMs = m.sentMs;
    c.lastNewsMs = NowMs();
    c.lastPos = pos;
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(actor);
    auto* gi = GameInteractor::Instance;
    c.shouldUpdate = gi->RegisterGameHookForPtr<GameInteractor::ShouldActorUpdate>(ptr, OnCopyShouldUpdate);
    c.shouldDraw = gi->RegisterGameHookForPtr<GameInteractor::ShouldActorDraw>(ptr, OnCopyShouldDraw);
    sCopies[key] = std::move(c);
    sCopyByActor[actor] = key;
    sStats.maxCopies = std::max(sStats.maxCopies, sCopies.size());
    SPDLOG_INFO("[Coop] proj-copy: owner {} id {} {} params {} at ({:.0f}, {:.0f}, {:.0f})", owner, m.id,
                TypeName(m.actorId), m.params, pos.x, pos.y, pos.z);
}

bool DecodeSample(Reader& r, Sample& s, const char*& why) {
    s.r = r.Get<StateRecord>();
    if (!r.ok) {
        why = "cut short";
        return false;
    }
    if ((s.r.flags & ~kKnownStateFlags) != 0 || s.r.extraSize > kMaxExtraSize || s.r.id == 0) {
        why = "bad flags, id or extra size";
        return false;
    }
    r.Bytes(s.extra.data(), s.r.extraSize);
    if (!r.ok) {
        why = "extra cut short";
        return false;
    }
    if (s.r.scale > 30000) {
        why = "scale out of range";
        return false;
    }
    return true;
}

void OnStates(uint32_t owner, Reader& r) {
    const StatesHeader h = r.Get<StatesHeader>();
    if (!r.ok || h.count == 0 || h.count > kMaxRecords) {
        return Drop("states", "bad header");
    }
    std::vector<Sample> samples(h.count);
    std::unordered_set<uint16_t> ids;
    for (Sample& s : samples) {
        const char* why = nullptr;
        if (!DecodeSample(r, s, why)) {
            return Drop("states", why);
        }
        if (!ids.insert(s.r.id).second) {
            return Drop("states", "repeated id");
        }
        s.sentMs = h.sentMs;
    }
    if (r.Remaining() != 0) {
        return Drop("states", "bytes after the last record");
    }
    // The extras are checked against each copy's type before anything is kept.
    for (const Sample& s : samples) {
        auto it = sCopies.find(Key(owner, s.r.id));
        if (it != sCopies.end() &&
            (s.r.extraSize != it->second.type->extraSize || !it->second.type->apply(nullptr, s.extra.data()))) {
            return Drop("states", "extra data out of range for its type");
        }
    }
    UpdateClock(owner, h.sentMs);
    const uint32_t now = NowMs();
    for (const Sample& s : samples) {
        auto it = sCopies.find(Key(owner, s.r.id));
        if (it == sCopies.end()) {
            continue; // Not shown here (other scene, refused) or already gone
        }
        Copy& c = it->second;
        if (!c.history.empty() && static_cast<int32_t>(s.sentMs - c.history.back().sentMs) <= 0) {
            continue;
        }
        c.history.push_back(s);
        while (c.history.size() > 2 && s.sentMs - c.history.front().sentMs > kHistoryMs) {
            c.history.pop_front();
        }
        c.lastPos = { static_cast<f32>(s.r.pos[0]), static_cast<f32>(s.r.pos[1]), static_cast<f32>(s.r.pos[2]) };
        c.lastNewsMs = now;
    }
}

void OnKill(uint32_t owner, Reader& r) {
    const KillMsg m = r.Get<KillMsg>();
    if (!r.ok || r.Remaining() != 0 || m.reason >= static_cast<uint8_t>(KillReason::Count) || m.id == 0) {
        return Drop("kill", "wrong size or values");
    }
    UpdateClock(owner, m.sentMs);
    auto it = sCopies.find(Key(owner, m.id));
    if (it == sCopies.end() || it->second.killed) {
        return;
    }
    Copy& c = it->second;
    c.killed = true;
    c.killMs = m.sentMs;
    c.killArrivalMs = NowMs();
    c.hitsSent = m.hitsSent;
    c.lastNewsMs = c.killArrivalMs;
}

// MARK: - Showing the copies

int16_t LerpAngle(int16_t a, int16_t b, float t) {
    return static_cast<int16_t>(a + static_cast<int16_t>(static_cast<int16_t>(b - a) * t));
}

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// The owner's state at its time `target`: positions and angles in between, the rest from the nearer state.
Sample SampleAt(const std::deque<Sample>& h, int64_t target, float& t) {
    t = 0.0f;
    if (target <= static_cast<int64_t>(h.front().sentMs)) {
        return h.front();
    }
    if (target >= static_cast<int64_t>(h.back().sentMs)) {
        return h.back();
    }
    size_t i = 0;
    while (i + 1 < h.size() && static_cast<int64_t>(h[i + 1].sentMs) <= target) {
        i++;
    }
    const Sample& a = h[i];
    const Sample& b = h[i + 1];
    const float span = static_cast<float>(b.sentMs - a.sentMs);
    t = std::clamp(span > 0.0f ? static_cast<float>(target - a.sentMs) / span : 1.0f, 0.0f, 1.0f);
    Sample out = t < 0.5f ? a : b;
    for (int k = 0; k < 3; k++) {
        out.r.pos[k] = static_cast<int16_t>(std::lround(Lerp(a.r.pos[k], b.r.pos[k], t)));
        out.r.shapeRot[k] = LerpAngle(a.r.shapeRot[k], b.r.shapeRot[k], t);
    }
    out.r.worldRotY = LerpAngle(a.r.worldRotY, b.r.worldRotY, t);
    out.r.speed = static_cast<int16_t>(std::lround(Lerp(a.r.speed, b.r.speed, t)));
    out.r.scale = static_cast<uint16_t>(std::lround(Lerp(a.r.scale, b.r.scale, t)));
    return out;
}

void ApplySample(Copy& c, const Sample& s) {
    Actor* a = c.actor;
    a->prevPos = a->world.pos;
    a->world.pos = { static_cast<f32>(s.r.pos[0]), static_cast<f32>(s.r.pos[1]), static_cast<f32>(s.r.pos[2]) };
    a->shape.rot = { s.r.shapeRot[0], s.r.shapeRot[1], s.r.shapeRot[2] };
    a->world.rot.y = s.r.worldRotY;
    // Enemies that dodge projectiles look at where it is, which way it flies and how fast.
    a->speed = s.r.speed / 16.0f;
    a->velocity.y = (s.r.flags & kStateAirborne) ? -1.0f : 0.0f;
    const f32 scale = s.r.scale / 10000.0f;
    a->scale = { scale, scale, scale };
    a->focus.pos = a->world.pos;
    c.type->apply(a, s.extra.data());
}

void CopiesFrame(uint32_t now) {
    // Copies of players that left.
    const std::vector<PlayerInfo> players = GetSnapshot().players;
    std::vector<uint32_t> remove;
    for (auto& [key, c] : sCopies) {
        const bool present =
            std::any_of(players.begin(), players.end(), [&](const PlayerInfo& p) { return p.id == c.owner; });
        if (!present) {
            remove.push_back(key);
        }
    }
    for (uint32_t key : remove) {
        RemoveCopy(key, "its owner left");
    }
    remove.clear();
    for (auto& [key, c] : sCopies) {
        if (c.actor == nullptr) {
            remove.push_back(key);
            continue;
        }
        int64_t target = 0;
        const bool haveTime = RenderTime(c.owner, now, target);
        if (c.killed) {
            const bool due = !haveTime || target >= static_cast<int64_t>(c.killMs);
            if (due) {
                c.shown = false;
                const bool hitsDone = !IsHosting() || (c.hitsArrived >= c.hitsSent && c.pending == 0);
                if (hitsDone) {
                    remove.push_back(key);
                    continue;
                }
                if (now - c.killArrivalMs > kKillWaitMs) {
                    SPDLOG_WARN("[Coop] proj-kill: owner {} id {} removed after {} ms: {} of {} hit(s) arrived, {} "
                                "pending",
                                c.owner, c.id, kKillWaitMs, c.hitsArrived, c.hitsSent, c.pending);
                    remove.push_back(key);
                    continue;
                }
            }
        } else if (now - c.lastNewsMs > kStaleMs) {
            SPDLOG_WARN("[Coop] proj-copy: owner {} id {} {}: no news for {} ms, removed", c.owner, c.id,
                        TypeName(c.type->actorId), kStaleMs);
            remove.push_back(key);
            continue;
        }
        if (!haveTime || c.history.empty() || target < static_cast<int64_t>(c.spawnMs)) {
            continue; // Not shown yet: the owner's projectile appears here when its time is drawn
        }
        if (c.killed && target >= static_cast<int64_t>(c.killMs)) {
            continue;
        }
        float t = 0.0f;
        const Sample s = SampleAt(c.history, target, t);
        ApplySample(c, s);
        c.shown = (s.r.flags & kStateHidden) == 0;
        if (c.shown) {
            c.type->tick(c.actor);
        }
    }
    for (uint32_t key : remove) {
        RemoveCopy(key, nullptr);
    }
}

// MARK: - Messages

void Relay(uint32_t from, const std::vector<uint8_t>& bytes) {
    for (const PlayerInfo& p : GetSnapshot().players) {
        if (p.id != kHostPlayerId && p.id != from) {
            SendBinary(p.id, Protocol::kChannelProjectiles, bytes.data(), bytes.size());
            CountBytes(bytes.size() + Protocol::kBinaryHeaderSize, 0);
        }
    }
}

void OnMessage(uint32_t from, const std::vector<uint8_t>& bytes) {
    if (!SessionActive()) {
        return;
    }
    CountBytes(0, bytes.size() + Protocol::kBinaryHeaderSize);
    Reader r(bytes.data(), bytes.size());
    const Header header = r.Get<Header>();
    if (!r.ok || header.magic != kMagic || header.formatVersion != kFormatVersion) {
        return Drop("message", "bad header, magic or format version");
    }
    const uint32_t owner = header.owner;
    if (IsHosting()) {
        if (owner != from) {
            return Drop("message", "a client may only send its own projectiles");
        }
    } else if (from != kHostPlayerId || owner == LocalId() || owner == 0) {
        return Drop("message", "not from the host, or claims to be ours");
    }
    const size_t droppedBefore = sStats.droppedCopies;
    switch (static_cast<Type>(header.type)) {
        case Type::Spawn:
            OnSpawn(owner, r);
            break;
        case Type::States:
            OnStates(owner, r);
            break;
        case Type::Kill:
            OnKill(owner, r);
            break;
        default:
            return Drop("message", "unknown type");
    }
    // The other clients see a client's projectiles too, through the host, unless the message was refused.
    if (IsHosting() && sStats.droppedCopies == droppedBefore) {
        Relay(from, bytes);
    }
}

void OnActorDestroy(Actor* actor) {
    if (Owned* o = FindOwned(actor)) {
        o->dead = true;
        o->actor = nullptr;
    }
    auto key = sCopyByActor.find(actor);
    if (key != sCopyByActor.end()) {
        // Deleted by the game, not by its owner (a scene change, something that ate it): forget it.
        const uint32_t k = key->second;
        sCopyByActor.erase(key);
        auto it = sCopies.find(k);
        if (it != sCopies.end()) {
            auto* gi = GameInteractor::Instance;
            gi->UnregisterGameHookForPtr<GameInteractor::ShouldActorUpdate>(it->second.shouldUpdate);
            gi->UnregisterGameHookForPtr<GameInteractor::ShouldActorDraw>(it->second.shouldDraw);
            if (gPlayState != nullptr && gPlayState->state.running) {
                SPDLOG_INFO("[Coop] proj-copy-removed: owner {} id {} {} (deleted by the game here)", it->second.owner,
                            it->second.id, TypeName(it->second.type->actorId));
                sStats.droppedCopies++;
            }
            sCopies.erase(it);
        }
    }
}

void OnPlayDestroy() {
    // Every actor of the scene is gone: the owner's projectiles ended with it, the copies too.
    if (SessionActive()) {
        for (Owned& o : sOwned) {
            if (o.id != 0 && !o.refused) {
                SendKill(o, KillReason::Left);
            }
        }
    }
    sOwned.clear();
    for (auto& [key, c] : sCopies) {
        auto* gi = GameInteractor::Instance;
        gi->UnregisterGameHookForPtr<GameInteractor::ShouldActorUpdate>(c.shouldUpdate);
        gi->UnregisterGameHookForPtr<GameInteractor::ShouldActorDraw>(c.shouldDraw);
    }
    sCopies.clear();
    sCopyByActor.clear();
    sUpdating = nullptr;
}

void StatsLine(uint32_t now) {
    if (sStatsMs == 0) {
        sStatsMs = now;
    }
    const uint32_t elapsed = now - sStatsMs;
    if (elapsed < kStatsIntervalMs) {
        return;
    }
    sStatsMs = now;
    const Stats& s = sStats;
    if (s.sentBytes == 0 && s.receivedBytes == 0 && s.refusedCap == 0 && s.droppedCopies == 0) {
        return;
    }
    const double seconds = elapsed / 1000.0;
    // The local player's magic and powder keg timer, so a test can see the copies never touch them.
    SPDLOG_INFO("[Coop] projectile-bw: out {} B/s (peak {} in one second; spawns {}, state records {}, kills {}), in "
                "{} B/s (peak {}); shared alive {} (max {}), copies {} (max {}); not shared (cap) {}, copies refused "
                "or lost {}; local magic {} state {} keg {}",
                static_cast<uint64_t>(s.sentBytes / seconds), s.peakOutPerSecond, s.spawnsSent, s.statesSent,
                s.killsSent, static_cast<uint64_t>(s.receivedBytes / seconds), s.peakInPerSecond, SharedAlive(),
                s.maxOwned, sCopies.size(), s.maxCopies, s.refusedCap, s.droppedCopies,
                gSaveContext.save.saveInfo.playerData.magic, gSaveContext.magicState, gSaveContext.powderKegTimer);
    sStats = {};
}

void RegisterProjectileHooks() {
    auto* gi = GameInteractor::Instance;
    gi->RegisterGameHook<GameInteractor::OnActorSpawn>(OnActorSpawn);
    gi->RegisterGameHook<GameInteractor::ShouldActorUpdate>(OnShouldActorUpdate);
    gi->RegisterGameHook<GameInteractor::OnActorUpdate>(OnActorUpdate);
    gi->RegisterGameHook<GameInteractor::OnActorDestroy>(OnActorDestroy);
    gi->RegisterGameHook<GameInteractor::OnPlayDestroy>(OnPlayDestroy);
    RegisterBinaryHandler(Protocol::kChannelProjectiles, OnMessage);
}

} // namespace

void Frame() {
    const uint32_t now = NowMs();
    if (!SessionActive()) {
        if (!sOwned.empty() || !sCopies.empty() || !sClocks.empty()) {
            for (auto& [key, c] : sCopies) {
                auto* gi = GameInteractor::Instance;
                gi->UnregisterGameHookForPtr<GameInteractor::ShouldActorUpdate>(c.shouldUpdate);
                gi->UnregisterGameHookForPtr<GameInteractor::ShouldActorDraw>(c.shouldDraw);
                if (c.actor != nullptr && InGameplay()) {
                    sCopyByActor.erase(c.actor);
                    Actor_Kill(c.actor);
                }
            }
            sOwned.clear();
            sCopies.clear();
            sCopyByActor.clear();
            sClocks.clear();
        }
        return;
    }
    if (Playing()) {
        OwnerFrame(now);
        CopiesFrame(now);
    }
    StatsLine(now);
}

uint16_t OwnedId(Actor* actor) {
    Owned* o = FindOwned(actor);
    if (o == nullptr || o->dead || !Playing()) {
        return 0;
    }
    return Announce(*o) ? o->id : 0;
}

void NoteHitSent(uint16_t id) {
    if (Owned* o = FindOwnedId(id)) {
        o->hitsSent++;
    }
}

uint32_t HitFlags(const Actor* actor) {
    const TypeInfo* type = FindType(actor->id);
    return type != nullptr ? type->hitFlags(actor) : 0;
}

bool IsCopy(const Actor* actor) {
    return actor != nullptr && sCopyByActor.count(actor) != 0;
}

bool HostCopy(uint32_t owner, uint16_t id, CopyView& out) {
    auto it = sCopies.find(Key(owner, id));
    if (it == sCopies.end() || it->second.actor == nullptr || it->second.actor->update == nullptr) {
        return false;
    }
    const Copy& c = it->second;
    out.actor = c.actor;
    out.hitFlags = c.type->hitFlags(c.actor);
    out.reach = c.type->reach;
    out.lastPos = c.lastPos;
    out.elemMaterial = c.type->elemMaterial;
    out.atElemFlags = c.type->atElemFlags;
    return true;
}

void HostHitArrived(uint32_t owner, uint16_t id) {
    auto it = sCopies.find(Key(owner, id));
    if (it != sCopies.end()) {
        it->second.hitsArrived++;
    }
}

void HostHitQueued(uint32_t owner, uint16_t id) {
    auto it = sCopies.find(Key(owner, id));
    if (it != sCopies.end()) {
        it->second.pending++;
    }
}

void HostHitFinished(uint32_t owner, uint16_t id) {
    auto it = sCopies.find(Key(owner, id));
    if (it != sCopies.end() && it->second.pending > 0) {
        it->second.pending--;
    }
}

// MARK: - Tests

void TestShoot(const char* kindName) {
    PlayState* play = gPlayState;
    Player* player = GET_PLAYER(play);
    if (player == nullptr || !Playing()) {
        return;
    }
    const std::string kind = kindName;
    Actor* enemy = Enemies::NearestEnemy(player->actor.world.pos);
    Vec3f from = player->actor.world.pos;
    from.y += 40.0f;
    const s16 facing = player->actor.shape.rot.y;
    from.x += Math_SinS(facing) * 20.0f;
    from.z += Math_CosS(facing) * 20.0f;
    Vec3f to = { from.x + Math_SinS(facing) * 300.0f, from.y, from.z + Math_CosS(facing) * 300.0f };
    if (enemy != nullptr) {
        to = enemy->focus.pos;
        if (to.y < enemy->world.pos.y + 10.0f) {
            to.y = enemy->world.pos.y + 20.0f;
        }
    }
    const s16 yaw = Math_Vec3f_Yaw(&from, &to);
    s16 pitch = Math_Vec3f_Pitch(&from, &to); // Positive aims down, as Actor_SetSpeeds reads world.rot.x
    Actor* spawned = nullptr;
    sForceOwned = true;
    if (kind == "arrow" || kind == "fire" || kind == "ice" || kind == "light" || kind == "bubble" || kind == "nut") {
        const s16 params = kind == "arrow"    ? ARROW_TYPE_NORMAL
                           : kind == "fire"   ? ARROW_TYPE_FIRE
                           : kind == "ice"    ? ARROW_TYPE_ICE
                           : kind == "light"  ? ARROW_TYPE_LIGHT
                           : kind == "bubble" ? ARROW_TYPE_DEKU_BUBBLE
                                              : ARROW_TYPE_DEKU_NUT;
        if (params == ARROW_TYPE_DEKU_NUT && pitch < 0) {
            pitch = 0; // A nut aimed upwards is a dropped nut
        }
        player->unk_D57 = 4; // "Just fired": the arrow's first update lets it fly
        spawned = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_ARROW, from.x, from.y, from.z, pitch, yaw, 0, params);
    } else if (kind == "bomb") {
        Vec3f at = to;
        if (enemy != nullptr) {
            const s16 back = Math_Vec3f_Yaw(&enemy->world.pos, &player->actor.world.pos);
            at = { enemy->world.pos.x + Math_SinS(back) * 30.0f, enemy->world.pos.y + 5.0f,
                   enemy->world.pos.z + Math_CosS(back) * 30.0f };
        }
        spawned = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_BOM, at.x, at.y, at.z, BOMB_EXPLOSIVE_TYPE_BOMB, yaw, 0,
                              BOMB_TYPE_BODY);
        if (spawned != nullptr) {
            Actor_SetScale(spawned, 0.01f);
            reinterpret_cast<EnBom*>(spawned)->timer = 25;
        }
    } else if (kind == "chu") {
        spawned = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_BOM_CHU, player->actor.world.pos.x,
                              player->actor.world.pos.y, player->actor.world.pos.z, 0, facing, 0, 0);
    } else if (kind == "fins") {
        EnBoom* left = reinterpret_cast<EnBoom*>(Actor_Spawn(&play->actorCtx, play, ACTOR_EN_BOOM, from.x, from.y,
                                                             from.z, 0, yaw - 0x190, 0, ZORA_BOOMERANG_LEFT));
        EnBoom* right = reinterpret_cast<EnBoom*>(Actor_Spawn(&play->actorCtx, play, ACTOR_EN_BOOM, from.x, from.y,
                                                              from.z, 0, yaw + 0x190, 0, ZORA_BOOMERANG_RIGHT));
        for (EnBoom* fin : { left, right }) {
            if (fin != nullptr) {
                fin->moveTo = enemy;
                fin->unk_1CF = enemy != nullptr ? 0x10 : 0;
                fin->unk_1CC = fin->unk_1CF + 0x24;
            }
        }
        if (left != nullptr && right != nullptr) {
            left->actor.child = &right->actor;
            right->actor.parent = &left->actor;
            player->zoraBoomerangActor = &left->actor;
            player->stateFlags1 |= PLAYER_STATE1_ZORA_BOOMERANG_THROWN;
        }
        spawned = left != nullptr ? &left->actor : &right->actor;
    }
    sForceOwned = false;
    SPDLOG_INFO("[Coop] TEST: shoot {} at {} ({})", kind, enemy != nullptr ? TypeName(enemy->id) : "nothing",
                spawned != nullptr ? "spawned" : "failed");
}

void TestFuzz() {
    const uint32_t target = IsHosting() ? kBroadcast : kHostPlayerId;
    const uint8_t self = static_cast<uint8_t>(LocalId());
    auto send = [&](const char* what, const std::vector<uint8_t>& bytes) {
        SPDLOG_INFO("[Coop] TEST: fuzz projectile channel: {} ({} bytes)", what, bytes.size());
        SendBinary(target, Protocol::kChannelProjectiles, bytes.data(), bytes.size());
    };
    send("empty message", {});
    Writer badMagic;
    badMagic.Put(Header{ 0xDEADBEEF, kFormatVersion, static_cast<uint8_t>(Type::Spawn), self, 0 });
    send("bad magic", badMagic.buffer);
    SpawnMsg spawn = {};
    spawn.id = 0xFFF0;
    spawn.actorId = 0x7FFF;
    spawn.sceneId = Playing() ? gPlayState->sceneId : 0;
    spawn.sceneLayer = static_cast<int8_t>(gSaveContext.sceneLayer);
    send("spawn of an actor outside the table", Message(Type::Spawn, self, &spawn, sizeof(spawn)));
    spawn.actorId = ACTOR_EN_ARROW;
    spawn.params = 0x1234;
    send("arrow with params out of range", Message(Type::Spawn, self, &spawn, sizeof(spawn)));
    spawn.params = ARROW_TYPE_NORMAL;
    spawn.pos[0] = NAN;
    send("arrow at NaN", Message(Type::Spawn, self, &spawn, sizeof(spawn)));
    spawn.pos[0] = 0.0f;
    spawn.actorId = ACTOR_EN_NUTSBALL;
    send("enemy-only type from a player", Message(Type::Spawn, self, &spawn, sizeof(spawn)));
    send("someone else's projectile", Message(Type::Spawn, static_cast<uint8_t>(self + 7), &spawn, sizeof(spawn)));
    Writer states;
    states.Put(StatesHeader{ 0, 1, 200, 0 });
    send("states with 200 records", Message(Type::States, self, states.buffer.data(), states.buffer.size()));
    Writer cut;
    cut.Put(StatesHeader{ 0, 1, 2, 0 });
    StateRecord rec = {};
    rec.id = 1;
    rec.extraSize = 12;
    cut.Put(rec);
    send("states cut short", Message(Type::States, self, cut.buffer.data(), cut.buffer.size()));
    KillMsg kill = {};
    kill.id = 1;
    kill.reason = 9;
    send("kill with an unknown reason", Message(Type::Kill, self, &kill, sizeof(kill)));
}

static RegisterShipInitFunc initFunc(RegisterProjectileHooks);

} // namespace Coop::Projectiles
