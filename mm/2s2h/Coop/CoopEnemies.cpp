#include "CoopEnemies.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopArgs.h"
#include "CoopEnemyTypes.h"
#include "CoopEnemyWire.h"
#include "CoopPlayerSync.h"
#include "CoopSession.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ObjectExtension/ActorListIndex.h"
#include "2s2h/ObjectExtension/ObjectExtension.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

#ifdef _WIN32
#include <windows.h>
#endif

namespace Coop::Enemies {

namespace {

using SteadyClock = std::chrono::steady_clock;
using namespace Wire;

constexpr float kNearDistance = 1000.0f; // From any player's Link
constexpr float kNearRateHz = 15.0f;
constexpr float kFarRateHz = 5.0f;
constexpr uint32_t kKeyframeMs = 2000;
constexpr uint32_t kSummaryMs = 2000;
constexpr size_t kBudgetBytesPerSecond = 30 * 1024;
constexpr uint32_t kMinDelayMs = 100;
constexpr uint32_t kMaxDelayMs = 250;
constexpr uint32_t kHistoryMs = 1500;
constexpr float kTeleportDistance = 300.0f;
constexpr int kMaxLoadedActors = 240; // Leave room under the engine's 255 for the game's own actors
constexpr uint32_t kDormantGraceMs = 1500;
constexpr uint32_t kRequestIntervalMs = 2000;
constexpr int kLogSampleEvery = 20; // States batches (about one per host frame)

const SteadyClock::time_point sEpoch = SteadyClock::now();

uint32_t NowMs() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - sEpoch).count());
}

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

bool SceneTearingDown() {
    return gPlayState == nullptr || !gPlayState->state.running;
}

float Distance(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool FiniteWithin(const Vec3f& v, float limit) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) <= limit &&
           std::fabs(v.y) <= limit && std::fabs(v.z) <= limit;
}

// MARK: - actionFunc as an offset inside this binary

uintptr_t Anchor() {
    return reinterpret_cast<uintptr_t>(&Actor_Kill);
}

// Address range of the game's own code: received offsets must land inside it.
void CodeRange(uintptr_t& begin, uintptr_t& end) {
    static uintptr_t sBegin = 0, sEnd = 0;
    if (sEnd == 0) {
#ifdef _WIN32
        const HMODULE module = GetModuleHandleW(nullptr);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        sBegin = reinterpret_cast<uintptr_t>(module);
        sEnd = sBegin + nt->OptionalHeader.SizeOfImage;
#else
        sBegin = Anchor() - 256 * 1024 * 1024;
        sEnd = Anchor() + 256 * 1024 * 1024;
#endif
    }
    begin = sBegin;
    end = sEnd;
}

bool ActionFuncOffsetValid(int32_t offset) {
    uintptr_t begin, end;
    CodeRange(begin, end);
    const int64_t address = static_cast<int64_t>(Anchor()) + offset;
    return address >= static_cast<int64_t>(begin) && address < static_cast<int64_t>(end);
}

void* ReadActionFunc(const Actor* actor, ptrdiff_t offset) {
    void* fn = nullptr;
    std::memcpy(&fn, reinterpret_cast<const uint8_t*>(actor) + offset, sizeof(fn));
    return fn;
}

void WriteActionFunc(Actor* actor, ptrdiff_t offset, void* fn) {
    std::memcpy(reinterpret_cast<uint8_t*>(actor) + offset, &fn, sizeof(fn));
}

// MARK: - Per actor bookkeeping (both sides)

// How an actor came to exist, recorded before its Init runs.
struct Origin {
    uint16_t params = 0;
    int16_t listIndex = -1;
    int8_t spawnRoom = -1;
    int8_t sceneLayer = 0;
    int16_t sceneId = -1;
    Vec3f homePos = {};
    Vec3s homeRot = {};
    uint16_t coopNetId = 0; // Client: created by a host message for this netId
};
static ObjectExtension::Register<Origin> sOriginRegister;

// netId the next spawn belongs to (client creating a host enemy), and the object to report as loaded meanwhile.
uint16_t sSpawningNetId = 0;
int16_t sSpawningObjectId = -1;

// Skeletons set up inside each actor's instance, in the order they were set up.
std::unordered_map<Actor*, std::vector<SkelAnime*>> sSkeletons;

void OnActorSpawn(Actor* actor) {
    if (!IsHosting() && !IsClientInSession()) {
        return;
    }
    Origin origin;
    origin.params = static_cast<uint16_t>(actor->params);
    origin.listIndex = GetActorListIndex(actor);
    origin.spawnRoom = actor->room;
    origin.sceneLayer = static_cast<int8_t>(gSaveContext.sceneLayer);
    origin.sceneId = gPlayState != nullptr ? gPlayState->sceneId : -1;
    origin.homePos = actor->home.pos;
    origin.homeRot = actor->home.rot;
    origin.coopNetId = sSpawningNetId;
    ObjectExtension::GetInstance().Set<Origin>(actor, std::move(origin));
}

// Any SkelAnime set up anywhere: if it lives inside an actor, that actor owns it. Covers skeletons set up outside
// Init and set up again later; a skeleton the client never sets up (the host's update did) cannot be drawn.
void OnSkelAnimeInit(SkelAnime* skelAnime) {
    if (gPlayState == nullptr || (!IsHosting() && !IsClientInSession())) {
        return;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(skelAnime);
    for (int category = 0; category < ACTORCAT_MAX; category++) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[category].first; actor != nullptr; actor = actor->next) {
            if (actor->overlayEntry == nullptr || actor->overlayEntry->profile == nullptr) {
                continue;
            }
            const uintptr_t begin = reinterpret_cast<uintptr_t>(actor);
            if (address >= begin && address + sizeof(SkelAnime) <= begin + actor->overlayEntry->profile->instanceSize) {
                std::vector<SkelAnime*>& list = sSkeletons[actor];
                if (std::find(list.begin(), list.end(), skelAnime) == list.end()) {
                    list.push_back(skelAnime);
                }
                return;
            }
        }
    }
}

// Lets a spawn find its object when the scene does not list it (assets load by name in the port).
void OnObjectDependency(GIVanillaBehavior, bool* should, va_list originalArgs) {
    if (sSpawningObjectId < 0) {
        return;
    }
    va_list args;
    va_copy(args, originalArgs);
    const s16 objectId = static_cast<s16>(va_arg(args, s32));
    va_end(args);
    if (objectId == sSpawningObjectId) {
        *should = false;
    }
}

// Spawns `actorId` like Actor_SpawnAsChildAndCutscene, but never refused for a missing object or because the
// current room was already cleared (the clear flag is put back right after).
Actor* SpawnUnchecked(int16_t actorId, const Vec3f& pos, const Vec3s& rot, uint16_t params, uint16_t netId) {
    PlayState* play = gPlayState;
    const ActorProfile* profile = gActorOverlayTable[actorId].profile;
    if (profile == nullptr) {
        return nullptr;
    }
    const s32 room = play->roomCtx.curRoom.num;
    const u32 clearedBefore = play->actorCtx.sceneFlags.clearedRoom;
    if (room >= 0 && room <= kMaxRoom) {
        play->actorCtx.sceneFlags.clearedRoom &= ~(1u << room);
    }
    sSpawningObjectId = profile->objectId;
    sSpawningNetId = netId;
    Actor* actor = Actor_SpawnAsChildAndCutscene(&play->actorCtx, play, actorId, pos.x, pos.y, pos.z, rot.x, rot.y,
                                                 rot.z, params, CS_ID_NONE, HALFDAYBIT_ALL, nullptr);
    sSpawningNetId = 0;
    sSpawningObjectId = -1;
    play->actorCtx.sceneFlags.clearedRoom = clearedBefore;
    return actor;
}

uint32_t HashNetIds(std::vector<uint16_t> ids) {
    std::sort(ids.begin(), ids.end());
    uint32_t hash = 2166136261u;
    for (uint16_t id : ids) {
        for (int shift : { 0, 8 }) {
            hash ^= (id >> shift) & 0xFF;
            hash *= 16777619u;
        }
    }
    return hash;
}

std::vector<uint8_t> Message(Type type, const void* body, size_t size) {
    Writer w;
    w.Put(Header{ kMagic, kFormatVersion, static_cast<uint8_t>(type), 0 });
    w.Bytes(body, size);
    return std::move(w.buffer);
}

// MARK: - Pose (what a state record describes, merged)

struct Pose {
    uint32_t seq = 0;
    uint32_t sentMs = 0;
    Vec3f pos = {};
    Vec3s shapeRot = {};
    int16_t worldRotY = 0;
    Vec3f scale = { 0.01f, 0.01f, 0.01f };
    LookPart look = {};
    bool haveActionFunc = false;
    int32_t actionFuncOffset = 0;
    uint8_t skelCount = 0;
    std::array<uint8_t, kMaxSkeletons> limbCount = {};
    std::array<bool, kMaxSkeletons> jointsValid = {};
    std::array<std::array<Vec3s, kMaxLimbs>, kMaxSkeletons> joints = {};
    uint8_t extraSize = 0;
    std::array<uint8_t, kMaxExtraSize> extra = {};
    bool haveBase = false; // A keyframe has been merged
};

// MARK: - Host

struct HostEnemy {
    Actor* actor = nullptr;
    SpawnRecord record = {};
    const TypeInfo* type = nullptr;
    Pose lastSent;
    bool sentOnce = false;
    uint32_t lastKeyMs = 0;
    float credit = 1.0f;
    bool isNear = false;
};

std::unordered_map<uint16_t, HostEnemy> sHost;
std::unordered_map<Actor*, uint16_t> sHostByActor;
uint16_t sNextNetId = 1;
uint32_t sManifestSeq = 0;
bool sManifestSent = false;
bool sManifestRequested = false;
struct RoomSet {
    int16_t sceneId = -1;
    int8_t sceneLayer = 0;
    int8_t curRoom = -1;
    int8_t prevRoom = -1;
    bool operator==(const RoomSet&) const = default;
};
RoomSet sManifestRooms;
uint32_t sStatesSeq = 0;
uint32_t sLastStatesMs = 0;
uint32_t sLastSummaryMs = 0;
Actor* sUpdatingActor = nullptr;
bool sTestKilling = false;
// Bandwidth, per client (every message is broadcast)
size_t sBytesThisSecond = 0;
size_t sStatesThisSecond = 0;
uint32_t sBandwidthSecondStart = 0;
float sRateFactor = 1.0f;

size_t ClientCount() {
    size_t count = 0;
    for (const PlayerInfo& player : GetSnapshot().players) {
        if (player.id != kHostPlayerId) {
            count++;
        }
    }
    return count;
}

void HostSend(Type type, const void* body, size_t size) {
    const std::vector<uint8_t> message = Message(type, body, size);
    SendBinary(kBroadcast, Protocol::kChannelEnemies, message.data(), message.size());
    sBytesThisSecond += message.size() + Protocol::kBinaryHeaderSize;
}

uint16_t AllocNetId() {
    for (;;) {
        const uint16_t id = sNextNetId++;
        if (sNextNetId == 0) {
            sNextNetId = 1;
        }
        if (id != 0 && sHost.count(id) == 0) {
            return id;
        }
    }
}

SpawnRecord MakeRecord(uint16_t netId, Actor* actor) {
    SpawnRecord r = {};
    r.netId = netId;
    r.actorId = actor->id;
    r.room = actor->room;
    r.pos = actor->world.pos;
    r.rotY = actor->shape.rot.y;
    if (const Origin* origin = ObjectExtension::GetInstance().Get<Origin>(actor)) {
        r.params = origin->params;
        r.listIndex = origin->listIndex;
        r.sceneId = origin->sceneId;
        r.sceneLayer = origin->sceneLayer;
        r.spawnRoom = origin->spawnRoom;
        r.homePos = origin->homePos;
        r.homeRot = origin->homeRot;
    } else {
        // Already there when hosting started: no list index, so a client creates its own copy
        r.params = static_cast<uint16_t>(actor->params);
        r.listIndex = -1;
        r.sceneId = gPlayState->sceneId;
        r.sceneLayer = static_cast<int8_t>(gSaveContext.sceneLayer);
        r.spawnRoom = actor->room;
        r.homePos = actor->home.pos;
        r.homeRot = actor->home.rot;
    }
    return r;
}

uint16_t HostTrack(Actor* actor) {
    const uint16_t netId = AllocNetId();
    HostEnemy& enemy = sHost[netId];
    enemy.actor = actor;
    enemy.record = MakeRecord(netId, actor);
    enemy.type = FindType(actor->id);
    sHostByActor[actor] = netId;
    SPDLOG_INFO("[Coop] Enemy net {} tracked: {} params 0x{:04X} room {} index {}", netId, TypeName(actor->id),
                enemy.record.params, enemy.record.spawnRoom, enemy.record.listIndex);
    return netId;
}

void HostUntrack(uint16_t netId, KillReason reason, bool send) {
    auto it = sHost.find(netId);
    if (it == sHost.end()) {
        return;
    }
    sHostByActor.erase(it->second.actor);
    sHost.erase(it);
    if (send) {
        const KillMsg msg = { sManifestSeq, netId, static_cast<uint8_t>(reason), 0 };
        HostSend(Type::Kill, &msg, sizeof(msg));
        SPDLOG_INFO("[Coop] Enemy net {} gone on the host ({})", netId,
                    reason == KillReason::Died ? "died" : "despawned");
    }
}

void HostClear() {
    sHost.clear();
    sHostByActor.clear();
    sManifestSent = false;
}

RoomSet CurrentRooms() {
    PlayState* play = gPlayState;
    return { play->sceneId, static_cast<int8_t>(gSaveContext.sceneLayer), play->roomCtx.curRoom.num,
             play->roomCtx.prevRoom.num };
}

void SendManifest(const RoomSet& rooms) {
    sManifestSeq++;
    Writer w;
    const ManifestHeader header = { sManifestSeq,
                                    rooms.sceneId,
                                    rooms.sceneLayer,
                                    rooms.curRoom,
                                    rooms.prevRoom,
                                    0,
                                    static_cast<uint16_t>(sHost.size()) };
    w.Put(header);
    for (auto& [netId, enemy] : sHost) {
        enemy.record.room = enemy.actor->room;
        enemy.record.pos = enemy.actor->world.pos;
        enemy.record.rotY = enemy.actor->shape.rot.y;
        w.Put(enemy.record);
        enemy.sentOnce = false; // Keyframe next
    }
    HostSend(Type::Manifest, w.buffer.data(), w.buffer.size());
    sManifestRooms = rooms;
    sManifestSent = true;
    SPDLOG_INFO("[Coop] Enemy manifest #{}: {} enemies, scene {} rooms {}/{}", sManifestSeq, sHost.size(),
                rooms.sceneId, rooms.curRoom, rooms.prevRoom);
}

Pose CapturePose(const HostEnemy& enemy) {
    const Actor* a = enemy.actor;
    Pose p;
    p.pos = a->world.pos;
    p.shapeRot = a->shape.rot;
    p.worldRotY = a->world.rot.y;
    p.scale = a->scale;
    p.look.colorFilterParams = a->colorFilterParams;
    p.look.colorFilterTimer = a->colorFilterTimer;
    p.look.shadowAlpha = a->shape.shadowAlpha;
    p.look.actorFlags = ((a->flags & ACTOR_FLAG_ATTENTION_ENABLED) ? kLookAttention : 0) |
                        ((a->flags & ACTOR_FLAG_HOSTILE) ? kLookHostile : 0);
    p.look.focusOffset = { a->focus.pos.x - a->world.pos.x, a->focus.pos.y - a->world.pos.y,
                           a->focus.pos.z - a->world.pos.z };
    if (enemy.type != nullptr && enemy.type->actionFuncOffset >= 0) {
        const int64_t offset =
            static_cast<int64_t>(reinterpret_cast<uintptr_t>(ReadActionFunc(a, enemy.type->actionFuncOffset))) -
            static_cast<int64_t>(Anchor());
        if (offset >= INT32_MIN && offset <= INT32_MAX && ActionFuncOffsetValid(static_cast<int32_t>(offset))) {
            p.haveActionFunc = true;
            p.actionFuncOffset = static_cast<int32_t>(offset);
        }
    }
    auto skels = sSkeletons.find(enemy.actor);
    if (skels != sSkeletons.end()) {
        p.skelCount = static_cast<uint8_t>(std::min(skels->second.size(), kMaxSkeletons));
        for (size_t i = 0; i < p.skelCount; i++) {
            const SkelAnime* s = skels->second[i];
            if (s->jointTable == nullptr || s->limbCount > kMaxLimbs) {
                p.limbCount[i] = 0;
                continue;
            }
            p.limbCount[i] = s->limbCount;
            std::memcpy(p.joints[i].data(), s->jointTable, sizeof(Vec3s) * s->limbCount);
            p.jointsValid[i] = true;
        }
    }
    if (enemy.type != nullptr && enemy.type->extraSize > 0) {
        p.extraSize = enemy.type->extraSize;
        enemy.type->capture(a, p.extra.data());
    }
    return p;
}

bool SameVec3f(const Vec3f& a, const Vec3f& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

void EncodeRecord(Writer& w, uint16_t netId, const Pose& p, const Pose& last, bool key) {
    uint8_t flags = key ? kFull : 0;
    if (key || !SameVec3f(p.scale, last.scale)) {
        flags |= kScale;
    }
    if (key || std::memcmp(&p.look, &last.look, sizeof(p.look)) != 0) {
        flags |= kLook;
    }
    if (p.haveActionFunc && (key || !last.haveActionFunc || p.actionFuncOffset != last.actionFuncOffset)) {
        flags |= kActionFunc;
    }
    if (p.extraSize > 0 &&
        (key || last.extraSize != p.extraSize || std::memcmp(p.extra.data(), last.extra.data(), p.extraSize) != 0)) {
        flags |= kExtra;
    }
    w.Put(StateBase{ netId, flags, p.skelCount, p.pos, p.shapeRot, p.worldRotY });
    if (flags & kScale) {
        w.Put(p.scale);
    }
    if (flags & kLook) {
        w.Put(p.look);
    }
    if (flags & kActionFunc) {
        w.Put(p.actionFuncOffset);
    }
    for (size_t i = 0; i < p.skelCount; i++) {
        const uint8_t limbs = p.limbCount[i];
        const bool all = key || last.skelCount <= i || last.limbCount[i] != limbs;
        uint64_t mask = 0;
        for (uint8_t j = 0; j < limbs; j++) {
            if (all || std::memcmp(&p.joints[i][j], &last.joints[i][j], sizeof(Vec3s)) != 0) {
                mask |= 1ull << j;
            }
        }
        w.Put(limbs);
        w.Put(mask);
        for (uint8_t j = 0; j < limbs; j++) {
            if (mask & (1ull << j)) {
                w.Put(p.joints[i][j]);
            }
        }
    }
    if (flags & kExtra) {
        w.Put(p.extraSize);
        w.Bytes(p.extra.data(), p.extraSize);
    }
}

std::vector<Vec3f> PlayerPositions() {
    std::vector<Vec3f> positions = { GET_PLAYER(gPlayState)->actor.world.pos };
    for (const PlayerInfo& player : GetSnapshot().players) {
        PlayerSync::State s;
        if (player.id != kHostPlayerId && PlayerSync::LatestState(player.id, s) && s.sceneId == gPlayState->sceneId) {
            positions.push_back(s.pos);
        }
    }
    return positions;
}

void SendStates(uint32_t now) {
    const float dt = sLastStatesMs == 0 ? 0.05f : std::min(0.25f, (now - sLastStatesMs) / 1000.0f);
    sLastStatesMs = now;
    const std::vector<Vec3f> players = PlayerPositions();

    Writer body;
    uint16_t count = 0;
    for (auto& [netId, enemy] : sHost) {
        if (enemy.actor->update == nullptr) {
            continue; // Killed this frame; the kill message follows
        }
        enemy.isNear = std::any_of(players.begin(), players.end(), [&](const Vec3f& p) {
            return Distance(p, enemy.actor->world.pos) <= kNearDistance;
        });
        const float rate = (enemy.isNear ? kNearRateHz : kFarRateHz) / sRateFactor;
        enemy.credit = std::min(enemy.credit + rate * dt, 1.5f);
        const bool key = !enemy.sentOnce || now - enemy.lastKeyMs >= kKeyframeMs;
        if (enemy.credit < 1.0f && !key) {
            continue;
        }
        enemy.credit = std::max(0.0f, enemy.credit - 1.0f);
        const Pose pose = CapturePose(enemy);
        EncodeRecord(body, netId, pose, enemy.lastSent, key);
        enemy.lastSent = pose;
        enemy.sentOnce = true;
        if (key) {
            enemy.lastKeyMs = now;
        }
        count++;
    }
    // A batch goes out every frame, even empty, so both sides can line their logs up by batch number.
    sStatesSeq++;
    Writer w;
    w.Put(StatesHeader{ sStatesSeq, now, count, 0 });
    w.Bytes(body.buffer.data(), body.buffer.size());
    HostSend(Type::States, w.buffer.data(), w.buffer.size());
    sStatesThisSecond += count;

    if (GetLaunchOptions().testEnemyLog && sStatesSeq % kLogSampleEvery == 0) {
        for (const auto& [netId, enemy] : sHost) {
            const Vec3f& pos = enemy.actor->world.pos;
            SPDLOG_INFO("[Coop] Enemy host sample: seq {} net {} {} pos ({:.1f}, {:.1f}, {:.1f})", sStatesSeq, netId,
                        TypeName(enemy.record.actorId), pos.x, pos.y, pos.z);
        }
    }
}

void LogBandwidth(uint32_t now) {
    if (sBandwidthSecondStart == 0) {
        sBandwidthSecondStart = now;
        return;
    }
    if (now - sBandwidthSecondStart < 1000) {
        return;
    }
    const float seconds = (now - sBandwidthSecondStart) / 1000.0f;
    const size_t bytesPerSecond = static_cast<size_t>(sBytesThisSecond / seconds);
    const size_t nearCount = std::count_if(sHost.begin(), sHost.end(), [](const auto& e) { return e.second.isNear; });
    if (!sHost.empty() || sBytesThisSecond > 0) {
        SPDLOG_INFO("[Coop] enemy-bw: {} B/s per client ({} enemies, {} near, {} states/s, rate x{:.2f}, {} "
                    "client(s))",
                    bytesPerSecond, sHost.size(), nearCount, static_cast<size_t>(sStatesThisSecond / seconds),
                    1.0f / sRateFactor, ClientCount());
    }
    // Over budget: send less often (far ones notice first, they are already slow); back up when there is room.
    if (bytesPerSecond > kBudgetBytesPerSecond) {
        sRateFactor = std::min(4.0f, sRateFactor * 1.25f);
    } else if (bytesPerSecond < kBudgetBytesPerSecond * 2 / 3) {
        sRateFactor = std::max(1.0f, sRateFactor / 1.25f);
    }
    sBytesThisSecond = 0;
    sStatesThisSecond = 0;
    sBandwidthSecondStart = now;
    if (GetLaunchOptions().testEnemyLog) {
        SPDLOG_INFO("[Coop] Enemy host: {} tracked in scene {}, Link health {}/{}", sHost.size(), gPlayState->sceneId,
                    gSaveContext.save.saveInfo.playerData.health, gSaveContext.save.saveInfo.playerData.healthCapacity);
    }
}

void HostFrame() {
    if (!IsHosting()) {
        if (!sHost.empty() || sManifestSent) {
            HostClear();
        }
        return;
    }
    sUpdatingActor = nullptr;
    if (!InGameplay() || ClientCount() == 0) {
        return;
    }
    PlayState* play = gPlayState;
    const uint32_t now = NowMs();

    std::vector<uint16_t> fresh;
    for (Actor* actor = play->actorCtx.actorLists[ACTORCAT_ENEMY].first; actor != nullptr; actor = actor->next) {
        if (actor->init == nullptr && actor->update != nullptr && sHostByActor.count(actor) == 0) {
            fresh.push_back(HostTrack(actor));
        }
    }
    const RoomSet rooms = CurrentRooms();
    if (!sManifestSent || sManifestRequested || !(rooms == sManifestRooms)) {
        sManifestRequested = false;
        SendManifest(rooms);
    } else {
        for (uint16_t netId : fresh) {
            HostEnemy& enemy = sHost[netId];
            const SpawnMsg msg = { sManifestSeq, enemy.record };
            HostSend(Type::Spawn, &msg, sizeof(msg));
        }
    }

    SendStates(now);

    if (now - sLastSummaryMs >= kSummaryMs) {
        sLastSummaryMs = now;
        std::vector<uint16_t> ids;
        for (const auto& [netId, enemy] : sHost) {
            ids.push_back(netId);
        }
        const SummaryMsg msg = { sManifestSeq, static_cast<uint16_t>(ids.size()), 0, HashNetIds(ids) };
        HostSend(Type::Summary, &msg, sizeof(msg));
    }
    LogBandwidth(now);
}

// MARK: - Client

struct Known {
    SpawnRecord record = {};
    Actor* actor = nullptr; // The puppet, once created or matched
    Pose merged;
    std::deque<Pose> history;
    uint32_t nextTryMs = 0;
    int failures = 0;
    bool gaveUp = false;
    bool loggedWait = false;
    bool loggedJoints = false;
    float intervalMs = 1000.0f / kNearRateHz; // Average time between this enemy's states
    uint32_t lastSentMs = 0;
};

struct Puppet {
    uint16_t netId = 0; // 0: the client's own room actor, waiting to be matched (hidden, inert)
    uint64_t dormantKey = 0;
    uint32_t dormantSinceMs = 0;
    HOOK_ID shouldUpdate = 0;
    HOOK_ID shouldDraw = 0;
    HOOK_ID onUpdate = 0;
    bool visible = false;
    uint32_t updatesRun = 0;
    void* localActionFunc = nullptr;
    const TypeInfo* type = nullptr;
    uint16_t hostColorParams = 0;
    uint8_t hostColorTimer = 0;
};

std::unordered_map<uint16_t, Known> sKnown;
std::unordered_map<Actor*, Puppet> sPuppets;
std::unordered_map<uint64_t, Actor*> sDormant;
uint32_t sClientManifestSeq = 0;
bool sHaveManifest = false;
RoomSet sCovered;
uint32_t sManifestArrivalMs = 0;
uint32_t sLastRequestMs = 0;
RoomSet sStableRooms;
int sStableFrames = 0;
uint32_t sTotalUpdatesRun = 0;
// Host clock as seen from here: smallest arrival-minus-send offset over the last two seconds, plus jitter.
std::deque<std::pair<uint32_t, int64_t>> sOffsets;
bool sHavePrevBatch = false;
uint32_t sPrevArrivalMs = 0;
uint32_t sPrevSentMs = 0;
double sJitterMs = 0.0;
uint32_t sDelayMs = kMinDelayMs;
uint32_t sLoggedDelayMs = 0;
uint32_t sLastDelayLogMs = 0;
std::deque<std::pair<uint32_t, uint32_t>> sLogBatches; // (seq, sentMs) to log when shown

uint64_t DormantKey(int16_t sceneId, int8_t layer, int8_t spawnRoom, int16_t listIndex, int16_t actorId) {
    return (static_cast<uint64_t>(static_cast<uint16_t>(sceneId)) << 48) |
           (static_cast<uint64_t>(static_cast<uint8_t>(layer)) << 40) |
           (static_cast<uint64_t>(static_cast<uint8_t>(spawnRoom)) << 32) |
           (static_cast<uint64_t>(static_cast<uint16_t>(listIndex)) << 16) | static_cast<uint16_t>(actorId);
}

uint64_t RecordKey(const SpawnRecord& r) {
    return DormantKey(r.sceneId, r.sceneLayer, r.spawnRoom, r.listIndex, r.actorId);
}

void RequestManifest(const char* why) {
    const uint32_t now = NowMs();
    if (sLastRequestMs != 0 && now - sLastRequestMs < kRequestIntervalMs) {
        return;
    }
    sLastRequestMs = now;
    SPDLOG_INFO("[Coop] Asking the host for the enemy manifest ({})", why);
    const RequestMsg msg = { 0 };
    const std::vector<uint8_t> message = Message(Type::RequestManifest, &msg, sizeof(msg));
    SendBinary(kHostPlayerId, Protocol::kChannelEnemies, message.data(), message.size());
}

void OnPuppetShouldUpdate(Actor*, bool* should) {
    *should = false;
}

void OnPuppetShouldDraw(Actor* actor, bool* should) {
    auto it = sPuppets.find(actor);
    if (it != sPuppets.end() && !it->second.visible) {
        *should = false;
    }
}

// Never expected: counted so the test log can show that no puppet update ever ran.
void OnPuppetUpdated(Actor* actor) {
    auto it = sPuppets.find(actor);
    if (it != sPuppets.end()) {
        it->second.updatesRun++;
        sTotalUpdatesRun++;
    }
}

void UnregisterPuppetHooks(Actor* actor, Puppet& p) {
    auto* gi = GameInteractor::Instance;
    gi->UnregisterGameHookForPtr<GameInteractor::ShouldActorUpdate>(p.shouldUpdate);
    gi->UnregisterGameHookForPtr<GameInteractor::ShouldActorDraw>(p.shouldDraw);
    gi->UnregisterGameHookForPtr<GameInteractor::OnActorUpdate>(p.onUpdate);
    p.shouldUpdate = p.shouldDraw = p.onUpdate = 0;
}

// The puppet goes away: its own actionFunc is put back first, so nothing of the host's is left in it.
void RemovePuppet(Actor* actor, bool kill) {
    auto it = sPuppets.find(actor);
    if (it == sPuppets.end()) {
        return;
    }
    Puppet& p = it->second;
    if (p.type != nullptr && p.type->actionFuncOffset >= 0) {
        WriteActionFunc(actor, p.type->actionFuncOffset, p.localActionFunc);
    }
    UnregisterPuppetHooks(actor, p);
    if (p.netId == 0) {
        sDormant.erase(p.dormantKey);
    } else {
        auto known = sKnown.find(p.netId);
        if (known != sKnown.end() && known->second.actor == actor) {
            known->second.actor = nullptr;
        }
    }
    sPuppets.erase(it);
    if (kill) {
        Actor_Kill(actor);
    }
}

// Hidden and inert: the client's own room actor, waiting for the host's copy of the same room entry.
void MakeDormant(Actor* actor, const Origin& origin) {
    Puppet& p = sPuppets[actor];
    if (p.type != nullptr && p.type->actionFuncOffset >= 0) {
        WriteActionFunc(actor, p.type->actionFuncOffset, p.localActionFunc);
    }
    p.netId = 0;
    p.visible = false;
    actor->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
    p.dormantKey = DormantKey(origin.sceneId, origin.sceneLayer, origin.spawnRoom, origin.listIndex, actor->id);
    p.dormantSinceMs = NowMs();
    auto existing = sDormant.find(p.dormantKey);
    if (existing != sDormant.end() && existing->second != actor) {
        // Same room entry spawned twice (e.g. a room reloaded before the old copy was deleted): keep the new one.
        RemovePuppet(existing->second, true);
    }
    sDormant[p.dormantKey] = actor;
}

void MakePuppet(Actor* actor, uint16_t netId, const Origin* origin) {
    Puppet p;
    p.netId = netId;
    p.type = FindType(actor->id);
    if (p.type != nullptr && p.type->actionFuncOffset >= 0) {
        p.localActionFunc = ReadActionFunc(actor, p.type->actionFuncOffset);
    }
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(actor);
    auto* gi = GameInteractor::Instance;
    p.shouldUpdate = gi->RegisterGameHookForPtr<GameInteractor::ShouldActorUpdate>(ptr, OnPuppetShouldUpdate);
    p.shouldDraw = gi->RegisterGameHookForPtr<GameInteractor::ShouldActorDraw>(ptr, OnPuppetShouldDraw);
    p.onUpdate = gi->RegisterGameHookForPtr<GameInteractor::OnActorUpdate>(ptr, OnPuppetUpdated);
    // Hidden and not targetable until the host's first state arrives.
    actor->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
    sPuppets[actor] = p;
    if (netId == 0) {
        MakeDormant(actor, *origin);
    } else {
        auto known = sKnown.find(netId);
        if (known != sKnown.end()) {
            known->second.actor = actor;
        }
    }
}

// Client: every enemy becomes a puppet before its first update (Init has just run, its category is final).
void ClientOnActorInit(Actor* actor) {
    if (IsHosting() || !IsClientInSession() || actor->category != ACTORCAT_ENEMY || sPuppets.count(actor) != 0) {
        return;
    }
    if (actor->update == nullptr) {
        return; // Init killed it
    }
    const Origin* origin = ObjectExtension::GetInstance().Get<Origin>(actor);
    if (origin != nullptr && origin->coopNetId != 0) {
        MakePuppet(actor, origin->coopNetId, origin);
    } else if (origin != nullptr && origin->listIndex >= 0) {
        MakePuppet(actor, 0, origin);
    } else {
        // Spawned by something running on the client (another actor's Init, a drop...). Only the host creates
        // enemies; the host's copy, if it has one, arrives by message.
        SPDLOG_INFO("[Coop] Removing {} spawned locally on the client (the host creates enemies)", TypeName(actor->id));
        Actor_Kill(actor);
    }
}

void BindDormant(Actor* actor, uint16_t netId) {
    auto it = sPuppets.find(actor);
    if (it == sPuppets.end()) {
        return;
    }
    sDormant.erase(it->second.dormantKey);
    it->second.netId = netId;
    Known& known = sKnown[netId];
    known.actor = actor;
    SPDLOG_INFO("[Coop] Enemy net {} ({}) matched to the client's own room actor (room {} index {})", netId,
                TypeName(actor->id), known.record.spawnRoom, known.record.listIndex);
}

void DeathEffect(Actor* actor) {
    static Vec3f sVelocity = { 0.0f, 0.5f, 0.0f };
    PlayState* play = gPlayState;
    for (int i = 0; i < 12; i++) {
        Vec3f pos = { actor->world.pos.x + Rand_CenteredFloat(60.0f),
                      actor->world.pos.y + 20.0f + Rand_CenteredFloat(50.0f),
                      actor->world.pos.z + Rand_CenteredFloat(60.0f) };
        func_800B3030(play, &pos, &sVelocity, &sVelocity, 100, 0, 2);
    }
    SoundSource_PlaySfxEachFrameAtFixedWorldPos(play, &actor->world.pos, 36, NA_SE_EN_COMMON_EXTINCT_LEV - SFX_FLAG);
}

void ForgetKnown(uint16_t netId, KillReason reason) {
    auto it = sKnown.find(netId);
    if (it == sKnown.end()) {
        return;
    }
    Actor* actor = it->second.actor;
    sKnown.erase(it);
    const Origin* origin = actor != nullptr ? ObjectExtension::GetInstance().Get<Origin>(actor) : nullptr;
    if (reason == KillReason::Despawn && origin != nullptr && origin->listIndex >= 0 && origin->coopNetId == 0 &&
        sPuppets.count(actor) != 0) {
        // The host unloaded it (its room, its scene, a reload of the same scene). This is the client's own copy
        // from its room data: hidden again, so it matches the host's next copy of the same room entry.
        MakeDormant(actor, *origin);
        SPDLOG_INFO("[Coop] Enemy net {} despawned on the host; the client's own room actor is hidden again", netId);
        return;
    }
    if (actor != nullptr) {
        if (reason == KillReason::Died && !SceneTearingDown()) {
            DeathEffect(actor);
        }
        RemovePuppet(actor, true);
        SPDLOG_INFO("[Coop] Enemy net {} removed on the client ({})", netId,
                    reason == KillReason::Died ? "died" : "despawned");
    }
}

bool RecordValid(const SpawnRecord& r, const char*& why) {
    if (r.netId == 0) {
        why = "netId 0";
    } else if (r.actorId < 0 || r.actorId >= ACTOR_ID_MAX || gActorOverlayTable[r.actorId].profile == nullptr) {
        why = "actorId outside the actor table";
    } else if (r.room < -1 || r.room > kMaxRoom || r.spawnRoom < -1 || r.spawnRoom > kMaxRoom) {
        why = "room out of range";
    } else if (r.listIndex < -1) {
        why = "bad list index";
    } else if (!FiniteWithin(r.homePos, 100000.0f) || !FiniteWithin(r.pos, 100000.0f)) {
        why = "position not finite or out of range";
    } else {
        return true;
    }
    return false;
}

void Drop(const char* what, const char* why) {
    SPDLOG_WARN("[Coop] Dropping enemy {}: {}", what, why);
}

void ClientOnManifest(Reader& r) {
    const ManifestHeader header = r.Get<ManifestHeader>();
    if (!r.ok || header.count > kMaxRecords || r.Remaining() != header.count * sizeof(SpawnRecord)) {
        return Drop("manifest", "size does not match its count");
    }
    if (header.sceneId != -1 && (header.sceneId < 0 || header.sceneId >= SCENE_MAX)) {
        return Drop("manifest", "scene out of range");
    }
    std::vector<SpawnRecord> records(header.count);
    std::unordered_set<uint16_t> ids;
    for (SpawnRecord& record : records) {
        record = r.Get<SpawnRecord>();
        const char* why = nullptr;
        if (!RecordValid(record, why)) {
            return Drop("manifest", why);
        }
        if (!ids.insert(record.netId).second) {
            return Drop("manifest", "repeated netId");
        }
    }
    // Replaces what the client knew: enemies no longer listed go away.
    std::vector<uint16_t> gone;
    for (const auto& [netId, known] : sKnown) {
        if (ids.count(netId) == 0) {
            gone.push_back(netId);
        }
    }
    for (uint16_t netId : gone) {
        ForgetKnown(netId, KillReason::Despawn);
    }
    for (const SpawnRecord& record : records) {
        Known& known = sKnown[record.netId];
        if (known.actor != nullptr && known.record.actorId != record.actorId) {
            RemovePuppet(known.actor, true);
            known = Known{};
        }
        known.record = record;
    }
    sClientManifestSeq = header.manifestSeq;
    sHaveManifest = true;
    sCovered = { header.sceneId, header.sceneLayer, header.curRoom, header.prevRoom };
    sManifestArrivalMs = NowMs();
    sStableFrames = 0; // The host may have reloaded the client's scene: give the client's own copies time to appear
    SPDLOG_INFO("[Coop] Enemy manifest #{} from the host: {} enemies, scene {} rooms {}/{}", header.manifestSeq,
                header.count, header.sceneId, header.curRoom, header.prevRoom);
}

void ClientOnSpawn(Reader& r) {
    const SpawnMsg msg = r.Get<SpawnMsg>();
    const char* why = nullptr;
    if (!r.ok || r.Remaining() != 0) {
        return Drop("spawn", "wrong size");
    }
    if (!RecordValid(msg.record, why)) {
        return Drop("spawn", why);
    }
    if (!sHaveManifest || msg.manifestSeq != sClientManifestSeq) {
        SPDLOG_INFO("[Coop] enemy-resync: spawn for manifest #{} but the client has #{}", msg.manifestSeq,
                    sClientManifestSeq);
        return RequestManifest("spawn for another manifest");
    }
    Known& known = sKnown[msg.record.netId];
    if (known.actor != nullptr && known.record.actorId != msg.record.actorId) {
        RemovePuppet(known.actor, true);
        known = Known{};
    }
    known.record = msg.record;
}

void ClientOnKill(Reader& r) {
    const KillMsg msg = r.Get<KillMsg>();
    if (!r.ok || r.Remaining() != 0) {
        return Drop("kill", "wrong size");
    }
    if (msg.netId == 0 || msg.reason > static_cast<uint8_t>(KillReason::Died)) {
        return Drop("kill", "bad netId or reason");
    }
    if (!sHaveManifest || msg.manifestSeq != sClientManifestSeq) {
        SPDLOG_INFO("[Coop] enemy-resync: kill for manifest #{} but the client has #{}", msg.manifestSeq,
                    sClientManifestSeq);
        return RequestManifest("kill for another manifest");
    }
    ForgetKnown(msg.netId, static_cast<KillReason>(msg.reason));
}

void ClientOnSummary(Reader& r) {
    const SummaryMsg msg = r.Get<SummaryMsg>();
    if (!r.ok || r.Remaining() != 0) {
        return Drop("summary", "wrong size");
    }
    if (!IsClientPlayingInSession()) {
        return;
    }
    std::vector<uint16_t> ids;
    for (const auto& [netId, known] : sKnown) {
        ids.push_back(netId);
    }
    const uint32_t hash = HashNetIds(ids);
    if (!sHaveManifest || msg.manifestSeq != sClientManifestSeq || msg.count != ids.size() || msg.hash != hash) {
        SPDLOG_INFO("[Coop] enemy-resync: host has {} enemies (hash {:08X}, manifest #{}), client has {} (hash "
                    "{:08X}, manifest #{})",
                    msg.count, msg.hash, msg.manifestSeq, ids.size(), hash, sClientManifestSeq);
        RequestManifest("summary mismatch");
    }
}

// One state record, checked in full before anything is merged.
struct Decoded {
    StateBase base = {};
    Vec3f scale = {};
    LookPart look = {};
    int32_t actionFuncOffset = 0;
    std::array<uint8_t, kMaxSkeletons> limbCount = {};
    std::array<uint64_t, kMaxSkeletons> mask = {};
    std::array<std::array<Vec3s, kMaxLimbs>, kMaxSkeletons> joints = {};
    uint8_t extraSize = 0;
    std::array<uint8_t, kMaxExtraSize> extra = {};
};

bool DecodeRecord(Reader& r, Decoded& d, const char*& why) {
    d.base = r.Get<StateBase>();
    if (!r.ok) {
        why = "truncated record";
        return false;
    }
    const uint8_t flags = d.base.flags;
    if (d.base.netId == 0 || (flags & ~kKnownFlags) != 0 || d.base.skelCount > kMaxSkeletons) {
        why = "bad netId, flags or skeleton count";
        return false;
    }
    if ((flags & kFull) && (flags & (kScale | kLook)) != (kScale | kLook)) {
        why = "keyframe without every part";
        return false;
    }
    if (!FiniteWithin(d.base.pos, 100000.0f)) {
        why = "position not finite or out of range";
        return false;
    }
    if (flags & kScale) {
        d.scale = r.Get<Vec3f>();
        if (!FiniteWithin(d.scale, 100.0f)) {
            why = "scale not finite or out of range";
            return false;
        }
    }
    if (flags & kLook) {
        d.look = r.Get<LookPart>();
        if (!FiniteWithin(d.look.focusOffset, 10000.0f) || (d.look.actorFlags & ~(kLookAttention | kLookHostile))) {
            why = "bad focus offset or actor flags";
            return false;
        }
    }
    if (flags & kActionFunc) {
        d.actionFuncOffset = r.Get<int32_t>();
        if (!ActionFuncOffsetValid(d.actionFuncOffset)) {
            why = "actionFunc offset outside the game's code";
            return false;
        }
    }
    for (size_t i = 0; i < d.base.skelCount; i++) {
        d.limbCount[i] = r.Get<uint8_t>();
        d.mask[i] = r.Get<uint64_t>();
        if (d.limbCount[i] > kMaxLimbs || (d.limbCount[i] < kMaxLimbs && (d.mask[i] >> d.limbCount[i]) != 0)) {
            why = "joint mask beyond the limb count";
            return false;
        }
        const uint64_t all = d.limbCount[i] == kMaxLimbs ? ~0ull : (1ull << d.limbCount[i]) - 1;
        if ((flags & kFull) && d.mask[i] != all) {
            why = "keyframe without every joint";
            return false;
        }
        for (uint8_t j = 0; j < d.limbCount[i]; j++) {
            if (d.mask[i] & (1ull << j)) {
                d.joints[i][j] = r.Get<Vec3s>();
            }
        }
    }
    if (flags & kExtra) {
        d.extraSize = r.Get<uint8_t>();
        if (d.extraSize == 0 || d.extraSize > kMaxExtraSize) {
            why = "extra size out of range";
            return false;
        }
        r.Bytes(d.extra.data(), d.extraSize);
    }
    if (!r.ok) {
        why = "truncated record";
        return false;
    }
    // Type-specific checks, for the enemies the client knows.
    auto known = sKnown.find(d.base.netId);
    if (known != sKnown.end() && (flags & kExtra)) {
        const TypeInfo* type = FindType(known->second.record.actorId);
        if (type == nullptr || type->extraSize != d.extraSize || !type->apply(nullptr, d.extra.data())) {
            why = "draw fields wrong for the enemy type";
            return false;
        }
    }
    return true;
}

void Merge(Known& known, const Decoded& d, uint32_t seq, uint32_t sentMs) {
    Pose& m = known.merged;
    const uint8_t flags = d.base.flags;
    m.seq = seq;
    m.sentMs = sentMs;
    m.pos = d.base.pos;
    m.shapeRot = d.base.shapeRot;
    m.worldRotY = d.base.worldRotY;
    if (flags & kScale) {
        m.scale = d.scale;
    }
    if (flags & kLook) {
        m.look = d.look;
    }
    if (flags & kActionFunc) {
        m.haveActionFunc = true;
        m.actionFuncOffset = d.actionFuncOffset;
    }
    for (size_t i = 0; i < d.base.skelCount; i++) {
        const uint64_t all = d.limbCount[i] == kMaxLimbs ? ~0ull : (1ull << d.limbCount[i]) - 1;
        if (m.skelCount <= i || m.limbCount[i] != d.limbCount[i]) {
            m.jointsValid[i] = false; // A delta without its base: wait for the next keyframe
        }
        m.limbCount[i] = d.limbCount[i];
        for (uint8_t j = 0; j < d.limbCount[i]; j++) {
            if (d.mask[i] & (1ull << j)) {
                m.joints[i][j] = d.joints[i][j];
            }
        }
        if (d.mask[i] == all) {
            m.jointsValid[i] = true;
        }
    }
    m.skelCount = d.base.skelCount;
    if (flags & kExtra) {
        m.extraSize = d.extraSize;
        m.extra = d.extra;
    }
    if (flags & kFull) {
        m.haveBase = true;
    }
    if (known.lastSentMs != 0 && sentMs - known.lastSentMs < 1000) {
        known.intervalMs += (static_cast<float>(sentMs - known.lastSentMs) - known.intervalMs) / 4.0f;
    }
    known.lastSentMs = sentMs;
    if (m.haveBase) {
        known.history.push_back(m);
        while (known.history.size() > 2 && sentMs - known.history.front().sentMs > kHistoryMs) {
            known.history.pop_front();
        }
    }
}

void UpdateTiming(uint32_t sentMs) {
    const uint32_t now = NowMs();
    sOffsets.push_back({ now, static_cast<int64_t>(now) - sentMs });
    while (sOffsets.size() > 1 && now - sOffsets.front().first > 2000) {
        sOffsets.pop_front();
    }
    if (sHavePrevBatch) {
        const double transit = static_cast<double>(static_cast<int32_t>(now - sPrevArrivalMs)) -
                               static_cast<double>(static_cast<int32_t>(sentMs - sPrevSentMs));
        sJitterMs += (std::abs(transit) - sJitterMs) / 16.0;
    }
    sHavePrevBatch = true;
    sPrevArrivalMs = now;
    sPrevSentMs = sentMs;
    sDelayMs = std::clamp<uint32_t>(kMinDelayMs + static_cast<uint32_t>(3.0 * sJitterMs), kMinDelayMs, kMaxDelayMs);
    const uint32_t change = sDelayMs > sLoggedDelayMs ? sDelayMs - sLoggedDelayMs : sLoggedDelayMs - sDelayMs;
    if (sLoggedDelayMs == 0 || (change >= 20 && now - sLastDelayLogMs >= 1000)) {
        SPDLOG_INFO("[Coop] interp-delay enemies: {} ms (jitter {:.1f} ms)", sDelayMs, sJitterMs);
        sLoggedDelayMs = sDelayMs;
        sLastDelayLogMs = now;
    }
}

void ClientOnStates(Reader& r) {
    const StatesHeader header = r.Get<StatesHeader>();
    if (!r.ok || header.count > kMaxRecords) {
        return Drop("states", "bad header");
    }
    std::vector<Decoded> records(header.count);
    std::unordered_set<uint16_t> ids;
    for (Decoded& d : records) {
        const char* why = nullptr;
        if (!DecodeRecord(r, d, why)) {
            return Drop("states", why);
        }
        if (!ids.insert(d.base.netId).second) {
            return Drop("states", "repeated netId");
        }
    }
    if (r.Remaining() != 0) {
        return Drop("states", "bytes after the last record");
    }
    UpdateTiming(header.sentMs);
    for (const Decoded& d : records) {
        auto known = sKnown.find(d.base.netId);
        if (known != sKnown.end()) {
            Merge(known->second, d, header.seq, header.sentMs);
        }
    }
    if (GetLaunchOptions().testEnemyLog && header.seq % kLogSampleEvery == 0) {
        sLogBatches.push_back({ header.seq, header.sentMs });
        while (sLogBatches.size() > 64) {
            sLogBatches.pop_front();
        }
    }
}

int16_t LerpAngle(int16_t a, int16_t b, float t) {
    return static_cast<int16_t>(a + static_cast<int16_t>(static_cast<int16_t>(b - a) * t));
}

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

Vec3f LerpVec3f(const Vec3f& a, const Vec3f& b, float t) {
    return { Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t) };
}

Vec3s LerpVec3s(const Vec3s& a, const Vec3s& b, float t) {
    return { LerpAngle(a.x, b.x, t), LerpAngle(a.y, b.y, t), LerpAngle(a.z, b.z, t) };
}

// Pose between two states; discrete fields come from the nearer one.
Pose Interpolate(const Pose& a, const Pose& b, float t) {
    Pose out = t < 0.5f ? a : b;
    if (Distance(a.pos, b.pos) > kTeleportDistance) {
        return out;
    }
    out.pos = LerpVec3f(a.pos, b.pos, t);
    out.shapeRot = LerpVec3s(a.shapeRot, b.shapeRot, t);
    out.worldRotY = LerpAngle(a.worldRotY, b.worldRotY, t);
    out.scale = LerpVec3f(a.scale, b.scale, t);
    out.look.focusOffset = LerpVec3f(a.look.focusOffset, b.look.focusOffset, t);
    for (size_t i = 0; i < std::min(a.skelCount, b.skelCount); i++) {
        if (a.jointsValid[i] && b.jointsValid[i] && a.limbCount[i] == b.limbCount[i]) {
            for (uint8_t j = 0; j < a.limbCount[i]; j++) {
                out.joints[i][j] = LerpVec3s(a.joints[i][j], b.joints[i][j], t);
            }
        }
    }
    return out;
}

void ApplyPose(Actor* actor, Puppet& p, Known& known, const Pose& s) {
    actor->world.pos = s.pos;
    actor->shape.rot = s.shapeRot;
    actor->world.rot = { s.shapeRot.x, s.worldRotY, s.shapeRot.z };
    actor->scale = s.scale;
    actor->focus.pos = { s.pos.x + s.look.focusOffset.x, s.pos.y + s.look.focusOffset.y,
                         s.pos.z + s.look.focusOffset.z };
    actor->focus.rot = actor->world.rot;
    // A new flash on the host restarts it here; in between the engine counts it down locally.
    if (s.look.colorFilterParams != p.hostColorParams || s.look.colorFilterTimer > p.hostColorTimer) {
        actor->colorFilterParams = s.look.colorFilterParams;
        actor->colorFilterTimer = s.look.colorFilterTimer;
    }
    p.hostColorParams = s.look.colorFilterParams;
    p.hostColorTimer = s.look.colorFilterTimer;
    actor->shape.shadowAlpha = s.look.shadowAlpha;
    actor->flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
    actor->flags |= ((s.look.actorFlags & kLookAttention) ? ACTOR_FLAG_ATTENTION_ENABLED : 0) |
                    ((s.look.actorFlags & kLookHostile) ? ACTOR_FLAG_HOSTILE : 0);
    if (p.type != nullptr && p.type->actionFuncOffset >= 0 && s.haveActionFunc) {
        // Only compared by the draw code: the puppet's update, the one caller, never runs.
        WriteActionFunc(actor, p.type->actionFuncOffset,
                        reinterpret_cast<void*>(Anchor() + static_cast<intptr_t>(s.actionFuncOffset)));
    }
    auto skels = sSkeletons.find(actor);
    for (size_t i = 0; i < s.skelCount; i++) {
        if (!s.jointsValid[i]) {
            continue;
        }
        SkelAnime* skel = skels != sSkeletons.end() && i < skels->second.size() ? skels->second[i] : nullptr;
        if (skel == nullptr || skel->jointTable == nullptr || skel->limbCount != s.limbCount[i]) {
            if (!known.loggedJoints) {
                known.loggedJoints = true;
                SPDLOG_WARN("[Coop] Dropping enemy joints for net {} ({}): skeleton {} has {} limbs here, the host "
                            "sent {}",
                            p.netId, TypeName(actor->id), i, skel != nullptr ? skel->limbCount : 0, s.limbCount[i]);
            }
            continue;
        }
        std::memcpy(skel->jointTable, s.joints[i].data(), sizeof(Vec3s) * s.limbCount[i]);
    }
    if (p.type != nullptr && s.extraSize == p.type->extraSize && s.extraSize > 0) {
        p.type->apply(actor, s.extra.data());
    }
    // The shadow needs the floor under the puppet; its own update would have found it.
    Vec3f probe = { s.pos.x, s.pos.y + 50.0f, s.pos.z };
    s32 bgId;
    actor->floorHeight = BgCheck_EntityRaycastFloor5(&gPlayState->colCtx, &actor->floorPoly, &bgId, actor, &probe);
    p.visible = true;
}

// The host's pose of one enemy at host time `target`, from the states received for it.
Pose PoseAt(const std::deque<Pose>& h, int64_t target) {
    if (target <= static_cast<int64_t>(h.front().sentMs)) {
        return h.front();
    }
    if (target >= static_cast<int64_t>(h.back().sentMs)) {
        return h.back(); // Starved (or not updated on the host): hold the newest pose
    }
    size_t i = 0;
    while (i + 1 < h.size() && static_cast<int64_t>(h[i + 1].sentMs) <= target) {
        i++;
    }
    const float span = static_cast<float>(h[i + 1].sentMs - h[i].sentMs);
    const float t = span > 0.0f ? static_cast<float>(target - h[i].sentMs) / span : 1.0f;
    return Interpolate(h[i], h[i + 1], std::clamp(t, 0.0f, 1.0f));
}

void SampleAndApply(uint32_t now) {
    if (sOffsets.empty()) {
        return;
    }
    int64_t offset = INT64_MAX;
    for (const auto& [arrival, o] : sOffsets) {
        offset = std::min(offset, o);
    }
    const int64_t target = static_cast<int64_t>(now) - offset - sDelayMs;
    for (auto& [netId, known] : sKnown) {
        if (known.actor == nullptr || known.history.empty()) {
            continue;
        }
        auto puppet = sPuppets.find(known.actor);
        if (puppet != sPuppets.end()) {
            // Far enemies arrive less often than the delay covers: wait a little longer for them instead of
            // holding their last pose (they move in steps otherwise).
            const int64_t own = std::clamp<int64_t>(static_cast<int64_t>(known.intervalMs * 1.25f) + 20, 0, 600);
            const int64_t extra = std::max<int64_t>(0, own - sDelayMs);
            ApplyPose(known.actor, puppet->second, known, PoseAt(known.history, target - extra));
        }
    }
    // Test log: the puppet's pose at exactly the host time of a logged batch, as the client reconstructs it, once
    // the states around that time have arrived (far enemies send every 200 ms). The host logs the same batch.
    while (!sLogBatches.empty() && static_cast<int64_t>(sLogBatches.front().second) + 400 <= target) {
        const auto [seq, sentMs] = sLogBatches.front();
        sLogBatches.pop_front();
        for (const auto& [netId, known] : sKnown) {
            auto puppet = known.actor != nullptr ? sPuppets.find(known.actor) : sPuppets.end();
            if (puppet == sPuppets.end() || !puppet->second.visible || known.history.empty() ||
                known.history.front().sentMs > sentMs) {
                continue;
            }
            const Vec3f at = PoseAt(known.history, sentMs).pos;
            SPDLOG_INFO("[Coop] Enemy puppet sample: seq {} net {} {} pos ({:.1f}, {:.1f}, {:.1f})", seq, netId,
                        TypeName(known.record.actorId), at.x, at.y, at.z);
        }
    }
}

bool RoomLoaded(int8_t room) {
    const RoomContext& rooms = gPlayState->roomCtx;
    return room == -1 || room == rooms.curRoom.num || room == rooms.prevRoom.num;
}

void TrySpawn(uint16_t netId, Known& known, uint32_t now) {
    const SpawnRecord& r = known.record;
    const TypeInfo* type = FindType(r.actorId);
    if (type == nullptr || !type->validParams(r.params)) {
        known.gaveUp = true;
        SPDLOG_INFO("[Coop] Enemy net {} ({}) cannot be created on the client yet (type not supported); it stays "
                    "invisible here",
                    netId, TypeName(r.actorId));
        return;
    }
    if (gPlayState->actorCtx.totalLoadedActors >= kMaxLoadedActors) {
        known.nextTryMs = now + 1000;
        if (!known.loggedWait) {
            known.loggedWait = true;
            SPDLOG_INFO("[Coop] Enemy net {} waits: {} actors loaded", netId, gPlayState->actorCtx.totalLoadedActors);
        }
        return;
    }
    Actor* actor = SpawnUnchecked(r.actorId, r.homePos, r.homeRot, r.params, netId);
    if (actor == nullptr || actor->update == nullptr) {
        known.failures++;
        known.nextTryMs = now + 1000;
        if (known.failures >= 5) {
            known.gaveUp = true;
        }
        SPDLOG_WARN("[Coop] Could not create enemy net {} ({}) on the client (try {}{})", netId, TypeName(r.actorId),
                    known.failures, known.gaveUp ? ", giving up" : "");
        return;
    }
    actor->room = r.room;
    actor->world.pos = r.pos;
    actor->shape.rot.y = actor->world.rot.y = r.rotY;
    known.actor = actor;
    SPDLOG_INFO("[Coop] Enemy net {} ({}) created on the client in room {}", netId, TypeName(r.actorId), r.room);
}

void Materialize(uint32_t now) {
    PlayState* play = gPlayState;
    const RoomSet rooms = CurrentRooms();
    if (!(rooms == sStableRooms) || play->roomCtx.status != 0) {
        sStableRooms = rooms;
        sStableFrames = 0;
    } else {
        sStableFrames++;
    }
    if (play->transitionMode != TRANS_MODE_OFF || GET_PLAYER(play) == nullptr) {
        return;
    }
    for (auto& [netId, known] : sKnown) {
        const SpawnRecord& r = known.record;
        if (known.actor != nullptr || r.sceneId != play->sceneId ||
            r.sceneLayer != static_cast<int8_t>(gSaveContext.sceneLayer) || !RoomLoaded(r.room)) {
            continue;
        }
        if (r.listIndex >= 0) {
            // The client's own copy from its room, whenever it shows up (even for types it cannot create).
            auto dormant = sDormant.find(RecordKey(r));
            if (dormant != sDormant.end()) {
                BindDormant(dormant->second, netId);
                continue;
            }
            // The client's room may still be setting up its own copy (objects load a frame later).
            if (sStableFrames < 10) {
                continue;
            }
        }
        if (!known.gaveUp && now >= known.nextTryMs) {
            TrySpawn(netId, known, now);
        }
    }
    // The client's own room actors that the host does not have (already defeated, other half day...).
    if (!sHaveManifest || sCovered.sceneId != play->sceneId ||
        sCovered.sceneLayer != static_cast<int8_t>(gSaveContext.sceneLayer)) {
        return;
    }
    std::unordered_set<uint64_t> keys;
    for (const auto& [netId, known] : sKnown) {
        if (known.record.listIndex >= 0) {
            keys.insert(RecordKey(known.record));
        }
    }
    std::vector<Actor*> extra;
    for (const auto& [key, actor] : sDormant) {
        const Puppet& p = sPuppets[actor];
        const Origin* origin = ObjectExtension::GetInstance().Get<Origin>(actor);
        const int8_t room = origin != nullptr ? origin->spawnRoom : actor->room;
        const bool covered = room == sCovered.curRoom || room == sCovered.prevRoom;
        if (covered && keys.count(key) == 0 &&
            now - std::max(p.dormantSinceMs, sManifestArrivalMs) >= kDormantGraceMs) {
            extra.push_back(actor);
        }
    }
    for (Actor* actor : extra) {
        SPDLOG_INFO("[Coop] Removing the client's own {} (room {}): the host does not have it", TypeName(actor->id),
                    actor->room);
        RemovePuppet(actor, true);
    }
}

void LogPuppets() {
    static int64_t sLastSecond = 0;
    const int64_t second =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (!GetLaunchOptions().testEnemyLog || second == sLastSecond) {
        return;
    }
    sLastSecond = second;
    size_t bound = 0, dormant = 0, waiting = 0;
    for (const auto& [actor, p] : sPuppets) {
        (p.netId != 0 ? bound : dormant)++;
    }
    for (const auto& [netId, known] : sKnown) {
        if (known.actor == nullptr) {
            waiting++;
        }
    }
    SPDLOG_INFO("[Coop] Enemy puppets: {} shown, {} own room actors hidden, {} not created, {} known, updates run {}, "
                "scene {}, Link health {}/{}",
                bound, dormant, waiting, sKnown.size(), sTotalUpdatesRun, gPlayState->sceneId,
                gSaveContext.save.saveInfo.playerData.health, gSaveContext.save.saveInfo.playerData.healthCapacity);
    for (const auto& [netId, known] : sKnown) {
        if (known.actor != nullptr && sPuppets.count(known.actor)) {
            const Vec3f& pos = known.actor->world.pos;
            SPDLOG_INFO("[Coop] Enemy puppet: net {} {} pos ({:.1f}, {:.1f}, {:.1f}) updates {}", netId,
                        TypeName(known.record.actorId), pos.x, pos.y, pos.z, sPuppets[known.actor].updatesRun);
        }
    }
}

void ClientReleaseAll() {
    std::vector<Actor*> actors;
    for (const auto& [actor, p] : sPuppets) {
        actors.push_back(actor);
    }
    // The session ended under a scene with puppets: they go, they never start running on their own.
    for (Actor* actor : actors) {
        RemovePuppet(actor, !SceneTearingDown());
    }
    sKnown.clear();
    sDormant.clear();
    sHaveManifest = false;
    sClientManifestSeq = 0;
    sOffsets.clear();
    sHavePrevBatch = false;
    sLogBatches.clear();
}

void ClientFrame() {
    if (IsHosting()) {
        return;
    }
    if (!IsClientInSession()) {
        if (!sPuppets.empty() || !sKnown.empty()) {
            ClientReleaseAll();
        }
        return;
    }
    if (!IsClientPlayingInSession() || !InGameplay()) {
        return;
    }
    const uint32_t now = NowMs();
    if (!sHaveManifest) {
        RequestManifest("none yet");
    }
    Materialize(now);
    SampleAndApply(now);
    LogPuppets();
}

// MARK: - Messages

void OnEnemyMessage(uint32_t from, const std::vector<uint8_t>& bytes) {
    if (!IsHosting() && !IsClientInSession()) {
        return;
    }
    Reader r(bytes.data(), bytes.size());
    const Header header = r.Get<Header>();
    if (!r.ok || header.magic != kMagic || header.formatVersion != kFormatVersion) {
        return Drop("message", "bad header, magic or format version");
    }
    const Type type = static_cast<Type>(header.type);
    if (IsHosting()) {
        if (type != Type::RequestManifest) {
            return Drop("message", "clients may only ask for the manifest");
        }
        const RequestMsg msg = r.Get<RequestMsg>();
        if (!r.ok || r.Remaining() != 0) {
            return Drop("manifest request", "wrong size");
        }
        SPDLOG_INFO("[Coop] Player {} asked for the enemy manifest", from);
        sManifestRequested = true;
        return;
    }
    if (from != kHostPlayerId) {
        return Drop("message", "not from the host");
    }
    switch (type) {
        case Type::Manifest:
            return ClientOnManifest(r);
        case Type::Spawn:
            return ClientOnSpawn(r);
        case Type::Kill:
            return ClientOnKill(r);
        case Type::States:
            return ClientOnStates(r);
        case Type::Summary:
            return ClientOnSummary(r);
        default:
            return Drop("message", "unknown type");
    }
}

// MARK: - Engine hooks

void OnActorInit(Actor* actor) {
    ClientOnActorInit(actor);
}

void OnShouldActorUpdate(Actor* actor, bool*) {
    sUpdatingActor = actor;
}

void OnActorUpdate(Actor*) {
    sUpdatingActor = nullptr;
}

void OnActorKill(Actor* actor) {
    if (!IsHosting()) {
        return;
    }
    auto it = sHostByActor.find(actor);
    if (it != sHostByActor.end()) {
        const bool died = sTestKilling || sUpdatingActor == actor;
        HostUntrack(it->second, died ? KillReason::Died : KillReason::Despawn, !SceneTearingDown());
    }
}

void OnActorDestroy(Actor* actor) {
    sSkeletons.erase(actor);
    if (IsHosting()) {
        auto it = sHostByActor.find(actor);
        if (it != sHostByActor.end()) {
            HostUntrack(it->second, KillReason::Despawn, !SceneTearingDown());
        }
        return;
    }
    if (sPuppets.count(actor) != 0) {
        RemovePuppet(actor, false);
    }
    // A puppet created for a netId whose Init had not run yet.
    if (const Origin* origin = ObjectExtension::GetInstance().Get<Origin>(actor); origin && origin->coopNetId) {
        auto known = sKnown.find(origin->coopNetId);
        if (known != sKnown.end() && known->second.actor == actor) {
            known->second.actor = nullptr;
        }
    }
}

void OnPlayDestroy() {
    if (IsHosting() && sManifestSent && ClientCount() > 0) {
        // The host leaves its scene: nothing is simulated until the next one.
        sHost.clear();
        sHostByActor.clear();
        SendManifest(RoomSet{});
        sManifestSent = false;
    } else {
        HostClear();
    }
    // Every actor of the scene has been deleted by now.
    for (auto& [actor, p] : sPuppets) {
        UnregisterPuppetHooks(actor, p);
    }
    sPuppets.clear();
    sDormant.clear();
    for (auto& [netId, known] : sKnown) {
        known.actor = nullptr;
        known.history.clear();
        known.gaveUp = false;
        known.failures = 0;
    }
    sSkeletons.clear();
}

void RegisterEnemyHooks() {
    auto* gi = GameInteractor::Instance;
    gi->RegisterGameHook<GameInteractor::OnActorSpawn>(OnActorSpawn);
    gi->RegisterGameHook<GameInteractor::OnSkelAnimeInit>(OnSkelAnimeInit);
    gi->RegisterGameHook<GameInteractor::OnActorInit>(OnActorInit);
    gi->RegisterGameHook<GameInteractor::ShouldActorUpdate>(OnShouldActorUpdate);
    gi->RegisterGameHook<GameInteractor::OnActorUpdate>(OnActorUpdate);
    gi->RegisterGameHook<GameInteractor::OnActorKill>(OnActorKill);
    gi->RegisterGameHook<GameInteractor::OnActorDestroy>(OnActorDestroy);
    gi->RegisterGameHook<GameInteractor::OnPlayDestroy>(OnPlayDestroy);
    REGISTER_VB_SHOULD(VB_ENABLE_OBJECT_DEPENDENCY, { OnObjectDependency(_, should, originalArgs); });
    RegisterBinaryHandler(Protocol::kChannelEnemies, OnEnemyMessage);
}

} // namespace

void Frame() {
    HostFrame();
    ClientFrame();
}

// MARK: - Tests

void TestSpawn(int16_t actorId, uint16_t params, int index) {
    if (!IsHosting() || !InGameplay()) {
        return;
    }
    if (actorId < 0 || actorId >= ACTOR_ID_MAX || gActorOverlayTable[actorId].profile == nullptr) {
        SPDLOG_WARN("[Coop] TEST: actor id 0x{:X} is not in the actor table", actorId);
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    const s16 yaw = static_cast<s16>(player->actor.shape.rot.y + index * 0x2000);
    const Vec3f pos = { player->actor.world.pos.x + 150.0f * Math_SinS(yaw), player->actor.world.pos.y,
                        player->actor.world.pos.z + 150.0f * Math_CosS(yaw) };
    const Vec3s rot = { 0, static_cast<s16>(yaw + 0x8000), 0 };
    Actor* actor = SpawnUnchecked(actorId, pos, rot, params, 0);
    if (actor == nullptr || actor->update == nullptr) {
        SPDLOG_WARN("[Coop] TEST: could not spawn {} params 0x{:04X}", TypeName(actorId), params);
        return;
    }
    SPDLOG_INFO("[Coop] TEST: spawned {} params 0x{:04X} at ({:.0f}, {:.0f}, {:.0f})", TypeName(actorId), params, pos.x,
                pos.y, pos.z);
}

void TestKill(int netId) {
    if (!IsHosting()) {
        return;
    }
    uint16_t id = static_cast<uint16_t>(std::max(netId, 0));
    if (netId < 0) {
        // The newest enemy still alive (several kills in a row each take the next one).
        id = 0;
        for (const auto& [candidate, enemy] : sHost) {
            if (enemy.actor->update != nullptr && candidate > id) {
                id = candidate;
            }
        }
    }
    auto it = sHost.find(id);
    if (it == sHost.end()) {
        SPDLOG_WARN("[Coop] TEST: no enemy with net {} to kill", id);
        return;
    }
    SPDLOG_INFO("[Coop] TEST: killing enemy net {} ({})", id, TypeName(it->second.record.actorId));
    sTestKilling = true;
    Actor_Kill(it->second.actor);
    sTestKilling = false;
}

void TestFuzz() {
    const bool host = IsHosting();
    const uint32_t target = host ? kBroadcast : kHostPlayerId;
    int sent = 0;
    auto send = [&](const char* what, const std::vector<uint8_t>& bytes) {
        SPDLOG_INFO("[Coop] TEST: fuzz enemy channel: {} ({} bytes)", what, bytes.size());
        SendBinary(target, Protocol::kChannelEnemies, bytes.data(), bytes.size());
        sent++;
    };
    auto header = [](uint8_t type, uint32_t magic = kMagic, uint8_t version = kFormatVersion) {
        Writer w;
        w.Put(Header{ magic, version, type, 0 });
        return w;
    };
    const uint32_t seq = host ? sManifestSeq : sClientManifestSeq;
    // Some real netId on this side, so the type-specific checks run too.
    uint16_t someNetId = 1;
    if (host && !sHost.empty()) {
        someNetId = sHost.begin()->first;
    }

    send("empty message", {});
    send("3 bytes", { 1, 2, 3 });
    send("bad magic", header(static_cast<uint8_t>(Type::Summary), 0xDEADBEEF).buffer);
    send("bad format version", header(static_cast<uint8_t>(Type::Summary), kMagic, 99).buffer);
    send("unknown type", header(99).buffer);
    {
        Writer w = header(static_cast<uint8_t>(Type::Manifest));
        w.Put(ManifestHeader{ seq + 1, 0, 0, 0, -1, 0, 60000 });
        send("manifest claiming 60000 records", w.buffer);
    }
    {
        SpawnRecord r = {};
        r.netId = 9000;
        r.actorId = 0x7FFF;
        Writer w = header(static_cast<uint8_t>(Type::Manifest));
        w.Put(ManifestHeader{ seq + 1, 0, 0, 0, -1, 0, 1 });
        w.Put(r);
        send("manifest with an actorId outside the table", w.buffer);
    }
    {
        SpawnRecord r = {};
        r.netId = 0;
        r.actorId = ACTOR_EN_WF;
        Writer w = header(static_cast<uint8_t>(Type::Spawn));
        w.Put(SpawnMsg{ seq, r });
        send("spawn with netId 0", w.buffer);
    }
    {
        SpawnRecord r = {};
        r.netId = 9001;
        r.actorId = ACTOR_EN_WF;
        r.room = 77;
        Writer w = header(static_cast<uint8_t>(Type::Spawn));
        w.Put(SpawnMsg{ seq, r });
        send("spawn in room 77", w.buffer);
    }
    {
        SpawnRecord r = {};
        r.netId = 9002;
        r.actorId = ACTOR_EN_WF;
        r.pos.x = NAN;
        Writer w = header(static_cast<uint8_t>(Type::Spawn));
        w.Put(SpawnMsg{ seq, r });
        send("spawn at a NaN position", w.buffer);
    }
    auto states = [&](const char* what, auto&& fill) {
        Writer w = header(static_cast<uint8_t>(Type::States));
        w.Put(StatesHeader{ 1, NowMs(), 1, 0 });
        fill(w);
        send(what, w.buffer);
    };
    states("state with 9 skeletons", [&](Writer& w) { w.Put(StateBase{ someNetId, 0, 9, {}, {}, 0 }); });
    states("state with unknown flags", [&](Writer& w) { w.Put(StateBase{ someNetId, 0x80, 0, {}, {}, 0 }); });
    states("joint mask beyond the limb count", [&](Writer& w) {
        w.Put(StateBase{ someNetId, 0, 1, {}, {}, 0 });
        w.Put(uint8_t{ 4 });
        w.Put(uint64_t{ 0xFF00 });
    });
    states("limb count 200", [&](Writer& w) {
        w.Put(StateBase{ someNetId, 0, 1, {}, {}, 0 });
        w.Put(uint8_t{ 200 });
        w.Put(uint64_t{ 1 });
    });
    states("actionFunc offset outside the code", [&](Writer& w) {
        w.Put(StateBase{ someNetId, kActionFunc, 0, {}, {}, 0 });
        w.Put(int32_t{ INT32_MAX });
    });
    states("extra size 200", [&](Writer& w) {
        w.Put(StateBase{ someNetId, kExtra, 0, {}, {}, 0 });
        w.Put(uint8_t{ 200 });
    });
    states("draw fields of the wrong size for the type", [&](Writer& w) {
        w.Put(StateBase{ someNetId, kExtra, 0, {}, {}, 0 });
        w.Put(uint8_t{ 3 });
        w.Put(uint8_t{ 9 });
        w.Put(uint8_t{ 9 });
        w.Put(uint8_t{ 9 });
    });
    states("record cut in half", [&](Writer& w) { w.Put(uint32_t{ someNetId }); });
    states("position 1e8 units away", [&](Writer& w) {
        w.Put(StateBase{ someNetId, 0, 1, { 0, -100000000.0f, 0 }, {}, 0 });
    });
    for (int i = 0; i < 20; i++) {
        Writer w = header(static_cast<uint8_t>(1 + Rand_ZeroOne() * 6));
        const int size = static_cast<int>(Rand_ZeroOne() * 120);
        for (int j = 0; j < size; j++) {
            w.Put(static_cast<uint8_t>(Rand_ZeroOne() * 256));
        }
        send("random bytes after a valid header", w.buffer);
    }
    if (host) {
        Writer w = header(static_cast<uint8_t>(Type::Summary));
        w.Put(SummaryMsg{ sManifestSeq, static_cast<uint16_t>(sHost.size() + 1), 0, 0x12345678 });
        send("summary with a wrong hash (the client must resync)", w.buffer);
    } else {
        Writer w = header(static_cast<uint8_t>(Type::RequestManifest));
        w.Put(RequestMsg{ 7 });
        send("valid manifest request", w.buffer);
    }
    SPDLOG_INFO("[Coop] TEST: fuzz enemy channel: {} messages sent", sent);
}

static RegisterShipInitFunc initFunc(RegisterEnemyHooks);

} // namespace Coop::Enemies
