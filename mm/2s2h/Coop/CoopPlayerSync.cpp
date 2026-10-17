#include "CoopPlayerSync.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include "Coop.h"
#include "CoopSession.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "macros.h"
#include "variables.h"
}

namespace Coop::PlayerSync {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr uint32_t kMinDelayMs = 100;
constexpr uint32_t kMaxDelayMs = 250;
constexpr uint32_t kBufferMs = 2000; // States older than this (behind the newest) are dropped
constexpr float kTeleportDistance = 300.0f;

const SteadyClock::time_point sEpoch = SteadyClock::now();
uint32_t sSendSeq = 0;

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

struct Received {
    State state;
    uint32_t arrivalMs;
};

struct Remote {
    std::deque<Received> states; // Ordered by state.sentMs
    uint32_t lastSeq = 0;
    bool havePrev = false;
    uint32_t prevArrivalMs = 0;
    uint32_t prevSentMs = 0;
    double jitterMs = 0.0;
    uint32_t delayMs = kMinDelayMs;
    uint32_t loggedDelayMs = 0;
    uint32_t lastDelayLogMs = 0;
    Vec3f lastSampledPos = {};
    bool sampledBefore = false;
};

std::unordered_map<uint32_t, Remote> sRemotes;

void CopyName(char* out, const void* animation) {
    std::memset(out, 0, kAnimNameSize);
    if (animation != nullptr) {
        std::strncpy(out, static_cast<const char*>(animation), kAnimNameSize - 1);
    }
}

// MARK: - Sending (local Link)

void CaptureAndSend(Actor* actor) {
    if (!SessionActive() || !InGameplay() || actor != &GET_PLAYER(gPlayState)->actor) {
        return;
    }
    Player* player = reinterpret_cast<Player*>(actor);
    State s = {};
    s.magic = kMagic;
    s.formatVersion = kFormatVersion;
    s.inGameplay = 1;
    s.playerId = IsHosting() ? kHostPlayerId : GetSnapshot().localId;
    s.seq = ++sSendSeq;
    s.sentMs = NowMs();
    s.sceneId = gPlayState->sceneId;
    s.roomNum = gPlayState->roomCtx.curRoom.num;
    s.sceneLayer = static_cast<int8_t>(gSaveContext.sceneLayer);

    s.pos = actor->world.pos;
    s.shapeRot = actor->shape.rot;
    s.speed = actor->speed;
    s.velocity = actor->velocity;
    s.shapeYOffset = actor->shape.yOffset;

    s.form = player->transformation;
    s.currentMask = player->currentMask;
    s.prevMask = player->prevMask;
    s.modelGroup = player->modelGroup;
    s.currentShield = player->currentShield;
    s.currentBoots = player->currentBoots;
    s.sword = static_cast<uint8_t>(GET_CUR_EQUIP_VALUE(EQUIP_TYPE_SWORD));
    s.face = actor->shape.face;
    s.stateFlags1 = player->stateFlags1;
    s.stateFlags2 = player->stateFlags2;
    s.stateFlags3 = player->stateFlags3;

    std::memcpy(&s.jointTable, player->skelAnime.jointTable, sizeof(s.jointTable));
    s.movementFlags = player->skelAnime.movementFlags;
    s.curFrame = player->skelAnime.curFrame;
    s.upperCurFrame = player->skelAnimeUpper.curFrame;
    CopyName(s.animName, player->skelAnime.animation);
    CopyName(s.upperAnimName, player->skelAnimeUpper.animation);

    s.headLimbRot = player->headLimbRot;
    s.upperLimbRot = player->upperLimbRot;
    s.upperLimbYawSecondary = player->upperLimbYawSecondary;
    s.unk_AAA = player->unk_AAA;
    s.unk_AB8 = player->unk_AB8;
    s.unk_ABC = player->unk_ABC;
    std::memcpy(s.unk_B10, player->unk_B10, sizeof(s.unk_B10));
    s.unk_B28 = player->unk_B28;
    s.unk_B62 = player->unk_B62;
    s.unk_B86[0] = player->unk_B86[0];
    s.unk_B86[1] = player->unk_B86[1];
    s.unk_B8A = player->unk_B8A;
    s.unk_B8C = player->unk_B8C;
    s.unk_B8E = player->unk_B8E;
    s.actionVar1 = player->av1.actionVar1;
    s.actionVar2 = player->av2.actionVar2;
    s.blastMaskTimer = player->blastMaskTimer;
    s.unk_AF0[0] = player->unk_AF0[0];
    s.unk_AF0[1] = player->unk_AF0[1];

    SendBinary(IsHosting() ? kBroadcast : kHostPlayerId, Protocol::kChannelPlayerState, &s, sizeof(s));
}

// MARK: - Receiving

bool IsFinite(const Vec3f& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Everything that indexes a table or reaches the draw code is checked; a bad state is dropped, never applied.
bool Validate(State& s, const char*& why) {
    s.animName[kAnimNameSize - 1] = '\0';
    s.upperAnimName[kAnimNameSize - 1] = '\0';
    if (s.magic != kMagic || s.formatVersion != kFormatVersion) {
        why = "bad magic or format version";
    } else if (s.form >= PLAYER_FORM_MAX || s.currentMask >= PLAYER_MASK_MAX || s.prevMask >= PLAYER_MASK_MAX) {
        why = "form or mask out of range";
    } else if (s.modelGroup >= PLAYER_MODELGROUP_MAX || s.face >= 16) {
        why = "model group or face out of range";
    } else if (!IsFinite(s.pos) || !IsFinite(s.velocity) || !std::isfinite(s.speed) || !std::isfinite(s.curFrame) ||
               !std::isfinite(s.upperCurFrame) || !std::isfinite(s.unk_AB8) || !std::isfinite(s.unk_ABC) ||
               !std::isfinite(s.shapeYOffset)) {
        why = "non-finite number";
    } else {
        return true;
    }
    return false;
}

void UpdateDelay(uint32_t id, Remote& r, uint32_t now) {
    const uint32_t delay =
        std::clamp<uint32_t>(kMinDelayMs + static_cast<uint32_t>(3.0 * r.jitterMs), kMinDelayMs, kMaxDelayMs);
    r.delayMs = delay;
    const uint32_t change = delay > r.loggedDelayMs ? delay - r.loggedDelayMs : r.loggedDelayMs - delay;
    if (r.loggedDelayMs == 0 || (change >= 20 && now - r.lastDelayLogMs >= 1000)) {
        SPDLOG_INFO("[Coop] interp-delay player {}: {} ms (jitter {:.1f} ms)", id, delay, r.jitterMs);
        r.loggedDelayMs = delay;
        r.lastDelayLogMs = now;
    }
}

void Store(uint32_t id, const State& s) {
    Remote& r = sRemotes[id];
    // A sender that restarted begins again from 1.
    if (s.seq <= r.lastSeq && r.lastSeq - s.seq < 1000) {
        return;
    }
    if (s.seq < r.lastSeq) {
        r = Remote{};
    }
    const uint32_t now = NowMs();
    if (r.states.empty() && r.lastSeq == 0) {
        SPDLOG_INFO("[Coop] First player state from player {} (form {}, scene {})", id, s.form, s.sceneId);
    }
    r.lastSeq = s.seq;

    // RFC 3550 style jitter: how much the spacing between arrivals differs from the spacing between sends.
    if (r.havePrev) {
        const double transit = static_cast<double>(static_cast<int32_t>(now - r.prevArrivalMs)) -
                               static_cast<double>(static_cast<int32_t>(s.sentMs - r.prevSentMs));
        r.jitterMs += (std::abs(transit) - r.jitterMs) / 16.0;
    }
    r.havePrev = true;
    r.prevArrivalMs = now;
    r.prevSentMs = s.sentMs;
    UpdateDelay(id, r, now);

    r.states.push_back({ s, now });
    while (r.states.size() > 1 && s.sentMs - r.states.front().state.sentMs > kBufferMs) {
        r.states.pop_front();
    }
}

void OnPlayerState(uint32_t from, const std::vector<uint8_t>& bytes) {
    if (!SessionActive()) {
        return;
    }
    if (bytes.size() != sizeof(State)) {
        SPDLOG_WARN("[Coop] Dropping player state of {} bytes from {} (expected {})", bytes.size(), from,
                    sizeof(State));
        NoteDroppedMessage();
        return;
    }
    State s;
    std::memcpy(&s, bytes.data(), sizeof(s));
    const char* why = nullptr;
    if (!Validate(s, why)) {
        SPDLOG_WARN("[Coop] Dropping player state from {}: {}", from, why);
        NoteDroppedMessage();
        return;
    }

    if (IsHosting()) {
        // The host decides who sent it, then passes it on to everyone else.
        s.playerId = from;
        for (const PlayerInfo& player : GetSnapshot().players) {
            if (player.id != from && player.id != kHostPlayerId) {
                SendBinary(player.id, Protocol::kChannelPlayerState, &s, sizeof(s));
            }
        }
    } else if (s.playerId == GetSnapshot().localId || s.playerId == 0) {
        return;
    }
    Store(s.playerId, s);
}

int16_t LerpAngle(int16_t a, int16_t b, float t) {
    return static_cast<int16_t>(a + static_cast<int16_t>(static_cast<int16_t>(b - a) * t));
}

void LerpVec3s(Vec3s& out, const Vec3s& a, const Vec3s& b, float t) {
    out.x = LerpAngle(a.x, b.x, t);
    out.y = LerpAngle(a.y, b.y, t);
    out.z = LerpAngle(a.z, b.z, t);
}

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

void LerpVec3f(Vec3f& out, const Vec3f& a, const Vec3f& b, float t) {
    out.x = Lerp(a.x, b.x, t);
    out.y = Lerp(a.y, b.y, t);
    out.z = Lerp(a.z, b.z, t);
}

float Distance(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Pose between two states; discrete fields come from the nearer one.
State Interpolate(const State& a, const State& b, float t) {
    State out = t < 0.5f ? a : b;
    if (Distance(a.pos, b.pos) > kTeleportDistance) {
        return out;
    }
    LerpVec3f(out.pos, a.pos, b.pos, t);
    LerpVec3s(out.shapeRot, a.shapeRot, b.shapeRot, t);
    out.speed = Lerp(a.speed, b.speed, t);
    LerpVec3f(out.velocity, a.velocity, b.velocity, t);
    // Joint 0 is the root translation, the rest are rotations; both blend fine as wrapped s16.
    for (int i = 0; i < PLAYER_LIMB_MAX; i++) {
        LerpVec3s(out.jointTable.frameTable[i], a.jointTable.frameTable[i], b.jointTable.frameTable[i], t);
    }
    LerpVec3s(out.headLimbRot, a.headLimbRot, b.headLimbRot, t);
    LerpVec3s(out.upperLimbRot, a.upperLimbRot, b.upperLimbRot, t);
    out.unk_AB8 = Lerp(a.unk_AB8, b.unk_AB8, t);
    out.unk_ABC = Lerp(a.unk_ABC, b.unk_ABC, t);
    return out;
}

void OnFrameCleanup() {
    if (!SessionActive()) {
        if (!sRemotes.empty()) {
            Reset();
        }
        return;
    }
    // Drop players that left the session.
    const std::vector<PlayerInfo> players = GetSnapshot().players;
    for (auto it = sRemotes.begin(); it != sRemotes.end();) {
        const bool present =
            std::any_of(players.begin(), players.end(), [&](const PlayerInfo& p) { return p.id == it->first; });
        if (present) {
            ++it;
        } else {
            SPDLOG_INFO("[Coop] Player {} left; dropping its states", it->first);
            it = sRemotes.erase(it);
        }
    }
}

void RegisterPlayerSync() {
    GameInteractor::Instance->RegisterGameHookForID<GameInteractor::OnActorUpdate>(ACTOR_PLAYER, CaptureAndSend);
    RegisterBinaryHandler(Protocol::kChannelPlayerState, OnPlayerState);
}

} // namespace

void Frame() {
    OnFrameCleanup();
}

std::vector<Sample> SampleRemotes() {
    std::vector<Sample> samples;
    const uint32_t now = NowMs();
    for (auto& [id, r] : sRemotes) {
        if (r.states.empty()) {
            continue;
        }
        // Map our clock to the sender's: the smallest arrival-minus-send offset is the fastest delivery seen.
        int64_t offset = INT64_MAX;
        for (const Received& rec : r.states) {
            offset = std::min<int64_t>(offset, static_cast<int64_t>(rec.arrivalMs) - rec.state.sentMs);
        }
        const int64_t target = static_cast<int64_t>(now) - offset - r.delayMs;

        State state;
        const Received& first = r.states.front();
        const Received& last = r.states.back();
        if (target <= static_cast<int64_t>(first.state.sentMs)) {
            state = first.state;
        } else if (target >= static_cast<int64_t>(last.state.sentMs)) {
            state = last.state; // Starved: hold the newest pose
        } else {
            size_t i = 0;
            while (i + 1 < r.states.size() && static_cast<int64_t>(r.states[i + 1].state.sentMs) <= target) {
                i++;
            }
            const State& a = r.states[i].state;
            const State& b = r.states[i + 1].state;
            const float span = static_cast<float>(b.sentMs - a.sentMs);
            const float t = span > 0.0f ? static_cast<float>(target - a.sentMs) / span : 1.0f;
            state = Interpolate(a, b, std::clamp(t, 0.0f, 1.0f));
        }

        Sample sample;
        sample.state = state;
        sample.delayMs = r.delayMs;
        sample.teleported = r.sampledBefore && Distance(r.lastSampledPos, state.pos) > kTeleportDistance;
        r.lastSampledPos = state.pos;
        r.sampledBefore = true;
        samples.push_back(sample);
    }
    return samples;
}

bool LatestState(uint32_t playerId, State& out) {
    auto it = sRemotes.find(playerId);
    if (it == sRemotes.end() || it->second.states.empty()) {
        return false;
    }
    out = it->second.states.back().state;
    return true;
}

void Reset() {
    sRemotes.clear();
}

static RegisterShipInitFunc initFunc(RegisterPlayerSync);

} // namespace Coop::PlayerSync
