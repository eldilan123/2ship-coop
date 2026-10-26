#include "CoopRooms.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "Coop.h"
#include "CoopClientDeath.h"
#include "CoopEnemies.h"
#include "CoopPlayerSync.h"
#include "CoopProtocol.h"
#include "CoopSession.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace Coop::Rooms {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr uint32_t kMismatchSettleMs = 300;  // A room must stay unloaded on the host this long before a move
constexpr uint32_t kFreshStateMs = 500;      // Only recent channel 3 states count
constexpr uint32_t kPendingTimeoutMs = 8000; // A move with no room_done by then is sent again
constexpr uint32_t kLoadTimeoutMs = 2000;    // Client: the new room must be loaded by then, or fall back
constexpr uint32_t kCheckDelayMs = 1500;     // After a move: compare puppets with the host's enemies
constexpr int kFadeFrames = 5;               // 0.25 s each way at 20 game frames per second

const SteadyClock::time_point sEpoch = SteadyClock::now();

uint32_t NowMs() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - sEpoch).count());
}

uint32_t MinMoveMs() {
    return static_cast<uint32_t>(std::clamp(CVarGetInteger(CVAR_COOP_ROOM_MOVE_MIN_MS, 2000), 0, 60000));
}

bool InGameplay() {
    return gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL && GET_PLAYER(gPlayState) != nullptr;
}

// Why the local game cannot take part in a room move right now (nullptr: it can).
const char* LocalBusy() {
    PlayState* play = gPlayState;
    Player* player = GET_PLAYER(play);
    if (play->transitionTrigger != TRANS_TRIGGER_OFF || play->transitionMode != TRANS_MODE_OFF) {
        return "scene change";
    }
    if (play->roomCtx.status != 0) {
        return "room loading";
    }
    if (play->roomCtx.curRoom.num < 0) {
        return "no room";
    }
    if (IS_PAUSED(&play->pauseCtx)) {
        return "pause menu";
    }
    if (play->msgCtx.msgMode != MSGMODE_NONE) {
        return "text box";
    }
    if (Play_InCsMode(play) || player->csAction != PLAYER_CSACTION_NONE) {
        return "cutscene";
    }
    if ((player->stateFlags1 & PLAYER_STATE1_DEAD) || play->gameOverCtx.state != GAMEOVER_INACTIVE) {
        return "dead";
    }
    return nullptr;
}

// The live flags of the current scene, as the game keeps them (ActorContextSceneFlags).
nlohmann::json FlagsJson() {
    const ActorContextSceneFlags& f = gPlayState->actorCtx.sceneFlags;
    nlohmann::json switches = nlohmann::json::array();
    for (u32 v : f.switches) {
        switches.push_back(v);
    }
    nlohmann::json collectible = nlohmann::json::array();
    for (u32 v : f.collectible) {
        collectible.push_back(v);
    }
    return { { "switches", switches },
             { "chest", f.chest },
             { "cleared", f.clearedRoom },
             { "clearedTemp", f.clearedRoomTemp },
             { "collectible", collectible } };
}

bool GetU32(const nlohmann::json& j, u32& out) {
    if (!j.is_number_unsigned() && !(j.is_number_integer() && j.get<int64_t>() >= 0)) {
        return false;
    }
    const uint64_t v = j.get<uint64_t>();
    if (v > 0xFFFFFFFFull) {
        return false;
    }
    out = static_cast<u32>(v);
    return true;
}

// Each array must have exactly the length of the game's own array: nothing is written outside the structure.
template <size_t N> bool GetU32Array(const nlohmann::json& j, u32 (&out)[N]) {
    if (!j.is_array() || j.size() != N) {
        return false;
    }
    for (size_t i = 0; i < N; i++) {
        if (!GetU32(j[i], out[i])) {
            return false;
        }
    }
    return true;
}

bool ParseFlags(const nlohmann::json& j, ActorContextSceneFlags& out) {
    return j.is_object() && j.contains("switches") && j.contains("chest") && j.contains("cleared") &&
           j.contains("clearedTemp") && j.contains("collectible") && GetU32Array(j["switches"], out.switches) &&
           GetU32(j["chest"], out.chest) && GetU32(j["cleared"], out.clearedRoom) &&
           GetU32(j["clearedTemp"], out.clearedRoomTemp) && GetU32Array(j["collectible"], out.collectible);
}

void Drop(const char* what, const char* why) {
    SPDLOG_WARN("[Coop] Dropping {}: {}", what, why);
    NoteDroppedMessage();
}

int CountActorsFromOtherRooms(int8_t room) {
    PlayState* play = gPlayState;
    int n = 0;
    for (int category = 0; category < ACTORCAT_MAX; category++) {
        for (Actor* a = play->actorCtx.actorLists[category].first; a != nullptr; a = a->next) {
            if (a->room >= 0 && a->room != room && a->room != play->roomCtx.prevRoom.num) {
                n++;
            }
        }
    }
    return n;
}

// MARK: - Host

struct Follow {
    bool mismatch = false;
    uint32_t since = 0;
    uint32_t moves = 0;
    uint32_t lastMoveMs = 0;
    uint32_t pendingSeq = 0;
    uint32_t pendingSentMs = 0;
    int8_t pendingRoom = -1;
    std::string deferred; // Last deferral reason logged for this mismatch
    uint32_t checkSeq = 0;
    uint32_t checkAtMs = 0;
    int8_t checkRoom = -1;
};
std::unordered_map<uint32_t, Follow> sFollow;
uint32_t sSeq = 0;

void LogDeferred(uint32_t playerId, Follow& f, const char* why) {
    if (f.deferred != why) {
        f.deferred = why;
        SPDLOG_INFO("[Coop] room-move deferred for player {} ({})", playerId, why);
    }
}

// Where the client arrives: 50 units beside the host (left, then right), on a floor at about the host's height;
// the host's own spot if neither has one. Two Links on the same spot push each other hard on the next frame.
Vec3f ArrivalSpot(Player* host) {
    PlayState* play = gPlayState;
    const Vec3f& at = host->actor.world.pos;
    for (s16 side : { static_cast<s16>(0x4000), static_cast<s16>(-0x4000) }) {
        const s16 yaw = static_cast<s16>(host->actor.shape.rot.y + side);
        Vec3f probe = { at.x + Math_SinS(yaw) * 50.0f, at.y + 40.0f, at.z + Math_CosS(yaw) * 50.0f };
        CollisionPoly* poly = nullptr;
        s32 bgId = 0;
        const f32 floorY = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &probe);
        if (poly != nullptr && std::fabs(floorY - at.y) < 30.0f) {
            return { probe.x, floorY, probe.z };
        }
    }
    return at;
}

void HostSendMove(uint32_t playerId, Follow& f, uint32_t now) {
    PlayState* play = gPlayState;
    Player* host = GET_PLAYER(play);
    const int8_t room = play->roomCtx.curRoom.num;
    const Vec3f spot = ArrivalSpot(host);
    f.pendingSeq = ++sSeq;
    f.pendingSentMs = now;
    f.pendingRoom = room;
    f.deferred.clear();
    SendJson(playerId, { { "type", "room_move" },
                         { "seq", f.pendingSeq },
                         { "scene", play->sceneId },
                         { "layer", gSaveContext.sceneLayer },
                         { "room", room },
                         { "pos", { spot.x, spot.y, spot.z } },
                         { "yaw", host->actor.shape.rot.y },
                         { "flags", FlagsJson() } });
    SPDLOG_INFO("[Coop] room-move: player {} seq {} to room {} of scene {} at ({:.0f}, {:.0f}, {:.0f}), next to the "
                "host at ({:.0f}, {:.0f}, {:.0f})",
                playerId, f.pendingSeq, room, play->sceneId, spot.x, spot.y, spot.z, host->actor.world.pos.x,
                host->actor.world.pos.y, host->actor.world.pos.z);
}

void HostFrame(uint32_t now) {
    PlayState* play = gPlayState;
    for (const PlayerInfo& info : GetSnapshot().players) {
        if (info.id == kHostPlayerId) {
            continue;
        }
        Follow& f = sFollow[info.id];
        if (f.checkSeq != 0 && now >= f.checkAtMs) {
            SPDLOG_INFO("[Coop] room-move-check (host): player {} seq {} room {} enemies {}", info.id, f.checkSeq,
                        f.checkRoom, Enemies::HostEnemiesInRoom(f.checkRoom));
            f.checkSeq = 0;
        }
        PlayerSync::State s;
        uint32_t age = 0;
        const bool known = PlayerSync::LatestState(info.id, s, &age) && age <= kFreshStateMs && s.inGameplay &&
                           s.sceneId == play->sceneId && s.sceneLayer == static_cast<int8_t>(gSaveContext.sceneLayer);
        const bool mismatch = known && !RoomLoadedHere(s.roomNum);
        if (!mismatch) {
            if (f.mismatch) {
                SPDLOG_INFO(
                    "[Coop] room-mismatch: player {} end after {} ms ({} move(s); host room {}, player room {})",
                    info.id, now - f.since, f.moves, play->roomCtx.curRoom.num, known ? s.roomNum : -1);
                f.mismatch = false;
            }
            continue;
        }
        if (!f.mismatch) {
            f.mismatch = true;
            f.since = now;
            f.moves = 0;
            f.deferred.clear();
            SPDLOG_INFO("[Coop] room-mismatch: player {} start (host rooms {}/{}, player room {})", info.id,
                        play->roomCtx.curRoom.num, play->roomCtx.prevRoom.num, s.roomNum);
        }
        if (now - f.since < kMismatchSettleMs) {
            continue;
        }
        if (f.pendingSeq != 0 && now - f.pendingSentMs < kPendingTimeoutMs) {
            continue; // The client is on its way (or waiting for its own cutscene to end)
        }
        if (f.lastMoveMs != 0 && now - f.lastMoveMs < MinMoveMs()) {
            LogDeferred(info.id, f, "minimum time between moves");
            continue;
        }
        if (const char* why = LocalBusy()) {
            LogDeferred(info.id, f, (std::string("host: ") + why).c_str());
            continue;
        }
        if (s.csAction != PLAYER_CSACTION_NONE || (s.stateFlags1 & PLAYER_STATE1_DEAD)) {
            LogDeferred(info.id, f, "player: cutscene or dead");
            continue;
        }
        f.moves++;
        HostSendMove(info.id, f, now);
    }
}

void HostOnRoomDone(uint32_t from, const nlohmann::json& m) {
    if (!IsHosting()) {
        return;
    }
    const int64_t seq = Protocol::GetInt(m, "seq", -1);
    const int64_t room = Protocol::GetInt(m, "room", -2);
    const std::string result = Protocol::GetString(m, "result");
    if (seq <= 0 || seq > 0xFFFFFFFFll || room < -1 || room > 127 || result.empty() || result.size() > 32) {
        return Drop("room_done", "bad seq, room or result");
    }
    auto it = sFollow.find(from);
    if (it == sFollow.end() || it->second.pendingSeq != static_cast<uint32_t>(seq)) {
        SPDLOG_INFO("[Coop] room-move: player {} reports seq {} ({}), not the one in progress", from, seq, result);
        return;
    }
    Follow& f = it->second;
    const uint32_t now = NowMs();
    SPDLOG_INFO("[Coop] room-move: player {} seq {} done ({}) in room {} after {} ms", from, seq, result, room,
                now - f.pendingSentMs);
    f.pendingSeq = 0;
    f.lastMoveMs = now;
    f.checkSeq = static_cast<uint32_t>(seq);
    f.checkAtMs = now + kCheckDelayMs;
    f.checkRoom = static_cast<int8_t>(room);
}

// MARK: - Client

struct Order {
    bool active = false;
    uint32_t seq = 0;
    int16_t sceneId = -1;
    int8_t layer = 0;
    int8_t room = -1;
    Vec3f pos = {};
    s16 yaw = 0;
    ActorContextSceneFlags flags = {};
    uint32_t receivedMs = 0;
};
Order sOrder;
uint32_t sLastSeq = 0;
std::string sClientDeferred;

enum class Stage { Idle, FadeOut, WaitRoom, FadeIn };
Stage sStage = Stage::Idle;
Order sMove; // The order being carried out
int sFadeFrame = 0;
uint32_t sStageMs = 0;
uint32_t sMoveStartMs = 0;
int8_t sFromRoom = -1;
uint32_t sCheckSeq = 0;
uint32_t sCheckAtMs = 0;
uint32_t sFallbacks = 0;

void ClientOnRoomMove(uint32_t, const nlohmann::json& m) {
    if (IsHosting()) {
        return;
    }
    Order o;
    const int64_t seq = Protocol::GetInt(m, "seq", -1);
    const int64_t scene = Protocol::GetInt(m, "scene", -1);
    const int64_t layer = Protocol::GetInt(m, "layer", -1);
    const int64_t room = Protocol::GetInt(m, "room", -1);
    const int64_t yaw = Protocol::GetInt(m, "yaw", 0x10000);
    if (seq <= 0 || seq > 0xFFFFFFFFll || scene < 0 || scene > 0xFF || layer < 0 || layer > 0x7F || room < 0 ||
        room > 127 || yaw < -0x8000 || yaw > 0x7FFF) {
        return Drop("room_move", "bad seq, scene, layer, room or yaw");
    }
    const auto pos = m.find("pos");
    if (pos == m.end() || !pos->is_array() || pos->size() != 3 || !(*pos)[0].is_number() || !(*pos)[1].is_number() ||
        !(*pos)[2].is_number()) {
        return Drop("room_move", "bad position");
    }
    o.pos = { (*pos)[0].get<float>(), (*pos)[1].get<float>(), (*pos)[2].get<float>() };
    if (!std::isfinite(o.pos.x) || !std::isfinite(o.pos.y) || !std::isfinite(o.pos.z) || std::fabs(o.pos.x) > 32000 ||
        std::fabs(o.pos.y) > 32000 || std::fabs(o.pos.z) > 32000) {
        return Drop("room_move", "position out of range");
    }
    const auto flags = m.find("flags");
    if (flags == m.end() || !ParseFlags(*flags, o.flags)) {
        return Drop("room_move", "bad scene flags (fields, sizes or values)");
    }
    if (static_cast<uint32_t>(seq) <= sLastSeq) {
        return Drop("room_move", "duplicate or out of order");
    }
    sLastSeq = static_cast<uint32_t>(seq);
    o.active = true;
    o.seq = static_cast<uint32_t>(seq);
    o.sceneId = static_cast<int16_t>(scene);
    o.layer = static_cast<int8_t>(layer);
    o.room = static_cast<int8_t>(room);
    o.yaw = static_cast<s16>(yaw);
    o.receivedMs = NowMs();
    sOrder = o;
    sClientDeferred.clear();
    SPDLOG_INFO("[Coop] room-move: seq {} order to room {} of scene {} received", o.seq, o.room, o.sceneId);
}

void SendDone(const Order& o, const char* result) {
    SendJson(kHostPlayerId, { { "type", "room_done" },
                              { "seq", o.seq },
                              { "room", gPlayState != nullptr ? gPlayState->roomCtx.curRoom.num : -1 },
                              { "result", result } });
}

void Fallback(const char* why) {
    sFallbacks++;
    SPDLOG_WARN("[Coop] room-move-fallback: seq {} to room {} ({}); reloading the scene next to the host ({} so far)",
                sMove.seq, sMove.room, why, sFallbacks);
    Play_FillScreen(gPlayState, false, 0, 0, 0, 0);
    sStage = Stage::Idle;
    SendDone(sMove, "fallback");
    ClientReturnToHost("room move fallback");
}

// The same steps the game takes when a fallen Link respawns in another room (z_player.c, Player_Action_1).
void SwapRooms() {
    PlayState* play = gPlayState;
    Player* player = GET_PLAYER(play);
    RoomContext& rooms = play->roomCtx;
    rooms.curRoom.num = -1;
    rooms.prevRoom.num = -1;
    rooms.curRoom.segment = NULL;
    rooms.prevRoom.segment = NULL;
    // Deletes every actor of the old rooms and clears the temporary flags, then the host's flags replace ours: the
    // new room's doors, chests and clear state come up as on the host. Memory only (saving is blocked for clients).
    Room_FinishRoomChange(play, &rooms);
    if (sMove.sceneId == play->sceneId && sMove.layer == static_cast<int8_t>(gSaveContext.sceneLayer)) {
        play->actorCtx.sceneFlags = sMove.flags;
    }
    if (!Room_RequestNewRoom(play, &rooms, sMove.room)) {
        Fallback("the room request was refused");
        return;
    }
    // Off any moving floor (platform, a door's collision): the game would carry the Link back on it.
    player->actor.floorBgId = BGCHECK_SCENE;
    player->actor.world.pos = sMove.pos;
    player->actor.prevPos = sMove.pos;
    player->actor.home.pos = sMove.pos;
    player->actor.shape.rot.y = sMove.yaw;
    player->actor.world.rot.y = sMove.yaw;
    player->yaw = sMove.yaw;
    player->speedXZ = 0.0f;
    player->actor.speed = 0.0f;
    player->actor.velocity = { 0.0f, 0.0f, 0.0f };
    // A later fall into the void respawns here, in the new room.
    RespawnData& respawn = gSaveContext.respawn[RESPAWN_MODE_DOWN];
    respawn.roomIndex = static_cast<u8>(sMove.room);
    respawn.pos = sMove.pos;
    respawn.yaw = sMove.yaw;
    respawn.tempSwitchFlags = sMove.flags.switches[2];
    respawn.tempCollectFlags = sMove.flags.collectible[2];
    sStage = Stage::WaitRoom;
    sStageMs = NowMs();
}

void ClientMoveFrame(uint32_t now) {
    PlayState* play = gPlayState;
    Player* player = GET_PLAYER(play);
    switch (sStage) {
        case Stage::Idle:
            break;
        case Stage::FadeOut:
            player->speedXZ = 0.0f;
            sFadeFrame++;
            Play_FillScreen(play, true, 0, 0, 0, static_cast<u8>(std::min(255, sFadeFrame * 255 / kFadeFrames)));
            if (sFadeFrame >= kFadeFrames) {
                SwapRooms();
            }
            break;
        case Stage::WaitRoom:
            if (play->roomCtx.status == 0 && play->roomCtx.curRoom.num == sMove.room) {
                Map_InitRoomData(play, sMove.room);
                Map_SetAreaEntrypoint(play);
                sStage = Stage::FadeIn;
                sFadeFrame = kFadeFrames;
            } else if (now - sStageMs > kLoadTimeoutMs) {
                Fallback("the room did not load in time");
            }
            break;
        case Stage::FadeIn:
            sFadeFrame--;
            Play_FillScreen(play, sFadeFrame > 0, 0, 0, 0,
                            static_cast<u8>(std::max(0, sFadeFrame * 255 / kFadeFrames)));
            if (sFadeFrame <= 0) {
                sStage = Stage::Idle;
                SPDLOG_INFO("[Coop] room-move: seq {} done: room {} -> {} in {} ms", sMove.seq, sFromRoom,
                            play->roomCtx.curRoom.num, now - sMoveStartMs);
                SendDone(sMove, "ok");
                sCheckSeq = sMove.seq;
                sCheckAtMs = now + kCheckDelayMs;
            }
            break;
    }
}

void ClientFrame(uint32_t now) {
    PlayState* play = gPlayState;
    if (sCheckSeq != 0 && now >= sCheckAtMs && sStage == Stage::Idle) {
        size_t shown = 0, duplicates = 0;
        Enemies::ClientPuppetCounts(shown, duplicates);
        SPDLOG_INFO("[Coop] room-move-check: seq {} room {} puppets shown {}, duplicate netIds {}, actors from other "
                    "rooms {}",
                    sCheckSeq, play->roomCtx.curRoom.num, shown, duplicates,
                    CountActorsFromOtherRooms(play->roomCtx.curRoom.num));
        sCheckSeq = 0;
    }
    if (sStage != Stage::Idle) {
        ClientMoveFrame(now);
        return;
    }
    if (!sOrder.active) {
        return;
    }
    if (sOrder.sceneId != play->sceneId || sOrder.layer != static_cast<int8_t>(gSaveContext.sceneLayer)) {
        // Another scene: following the host's scene (milestone 2) brings the client there.
        SPDLOG_INFO("[Coop] room-move: seq {} dropped (it is for scene {}, this is scene {})", sOrder.seq,
                    sOrder.sceneId, play->sceneId);
        SendDone(sOrder, "other scene");
        sOrder.active = false;
        return;
    }
    const char* why = ClientDeath::IsDown() ? "dead" : LocalBusy();
    if (why != nullptr) {
        if (sClientDeferred != why) {
            sClientDeferred = why;
            SPDLOG_INFO("[Coop] room-move: seq {} deferred ({})", sOrder.seq, why);
        }
        return;
    }
    sMove = sOrder;
    sOrder.active = false;
    if (play->roomCtx.curRoom.num == sMove.room) {
        SPDLOG_INFO("[Coop] room-move: seq {} already in room {}", sMove.seq, sMove.room);
        SendDone(sMove, "already there");
        return;
    }
    if (sMove.room >= play->roomList.count) {
        sStage = Stage::FadeOut;
        Fallback("room outside this scene's room list");
        return;
    }
    sFromRoom = play->roomCtx.curRoom.num;
    sMoveStartMs = now;
    sStage = Stage::FadeOut;
    sFadeFrame = 0;
    SPDLOG_INFO("[Coop] room-move: seq {} starting: room {} -> {} (order waited {} ms)", sMove.seq, sFromRoom,
                sMove.room, now - sMove.receivedMs);
}

void ResetClient() {
    if (sStage != Stage::Idle && gPlayState != nullptr) {
        Play_FillScreen(gPlayState, false, 0, 0, 0, 0);
    }
    sStage = Stage::Idle;
    sOrder = {};
    sCheckSeq = 0;
}

void OnPlayDestroy() {
    ResetClient();
    sFollow.clear();
}

void RegisterRoomsHooks() {
    RegisterJsonHandler("room_move", ClientOnRoomMove);
    RegisterJsonHandler("room_done", HostOnRoomDone);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDestroy>(OnPlayDestroy);
}

} // namespace

bool RoomLoadedHere(int8_t room) {
    if (gPlayState == nullptr) {
        return false;
    }
    const RoomContext& rooms = gPlayState->roomCtx;
    return room == -1 || room == rooms.curRoom.num || room == rooms.prevRoom.num;
}

bool MoveInProgress() {
    return sStage != Stage::Idle;
}

void Frame() {
    const uint32_t now = NowMs();
    if (IsHosting()) {
        ResetClient();
        if (InGameplay()) {
            HostFrame(now);
        }
        return;
    }
    sFollow.clear();
    if (!IsClientPlayingInSession() || !InGameplay()) {
        ResetClient();
        if (!IsClientInSession()) {
            sLastSeq = 0;
        }
        return;
    }
    ClientFrame(now);
}

void TestFuzz() {
    auto send = [](const char* what, const nlohmann::json& m) {
        SPDLOG_INFO("[Coop] TEST: fuzz room message: {}", what);
        SendJson(IsHosting() ? kBroadcast : kHostPlayerId, m);
    };
    if (!IsHosting()) {
        send("room_done with no seq", { { "type", "room_done" }, { "room", 1 }, { "result", "ok" } });
        send("room_done with room 300", { { "type", "room_done" }, { "seq", 1 }, { "room", 300 }, { "result", "ok" } });
        return;
    }
    const nlohmann::json good = { { "switches", { 0, 0, 0, 0 } },
                                  { "chest", 0 },
                                  { "cleared", 0 },
                                  { "clearedTemp", 0 },
                                  { "collectible", { 0, 0, 0, 0 } } };
    auto move = [&](uint32_t seq, int room, const nlohmann::json& flags) {
        return nlohmann::json{ { "type", "room_move" }, { "seq", seq },         { "scene", 27 }, { "layer", 0 },
                               { "room", room },        { "pos", { 0, 0, 0 } }, { "yaw", 0 },    { "flags", flags } };
    };
    nlohmann::json f = good;
    f["switches"] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    send("9 switch words (the game has 4)", move(0x7FFFFF00, 1, f));
    f = good;
    f["switches"] = { 1, 2 };
    send("2 switch words", move(0x7FFFFF01, 1, f));
    f = good;
    f["collectible"] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
    send("20 collectible words (the game has 4)", move(0x7FFFFF02, 1, f));
    f = good;
    f["chest"] = 0x1FFFFFFFFll;
    send("chest flags over 32 bits", move(0x7FFFFF03, 1, f));
    f = good;
    f["cleared"] = -5;
    send("negative cleared flags", move(0x7FFFFF04, 1, f));
    f = good;
    f["switches"] = { "a", "b", "c", "d" };
    send("switch words that are text", move(0x7FFFFF05, 1, f));
    f = good;
    f.erase("clearedTemp");
    send("flags with a field missing", move(0x7FFFFF06, 1, f));
    send("room 200", move(0x7FFFFF07, 200, good));
    nlohmann::json m = move(0x7FFFFF08, 1, good);
    m["pos"] = { 1.0, nullptr, 2.0 };
    send("position with a NaN (null in JSON)", m);
    m = move(0x7FFFFF09, 1, good);
    m["pos"] = { 1e9, 0, 0 };
    send("position far outside the world", m);
    m = move(0x7FFFFF0A, 1, good);
    m.erase("flags");
    send("no flags", m);
    m = move(0, 1, good);
    send("seq 0", m);
}

static RegisterShipInitFunc initFunc(RegisterRoomsHooks);

} // namespace Coop::Rooms
