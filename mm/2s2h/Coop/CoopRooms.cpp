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
#include "overlays/actors/ovl_En_Holl/z_en_holl.h"
}

namespace Coop::Rooms {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr uint32_t kMismatchSettleMs = 300;  // A room must stay unloaded on the host this long before a move
constexpr uint32_t kCrossSettleMs = 300;     // Client: its own new room must stay this long before it is reported
constexpr uint32_t kCrossLingerMs = 1500;    // Client: ... or this long while a room plane keeps both rooms loaded
constexpr uint32_t kHostWinsMs = 1000;       // Host: it crossed this recently itself when a client's cross arrives
constexpr uint32_t kCrossGraceMs = 4000;     // Host: wait this long for a client's room_cross (its door takes ~2.5 s)
constexpr uint32_t kRespawnWindowMs = 3000;  // Client: a room change this soon after a void respawn is not a cross
constexpr uint32_t kFreshStateMs = 500;      // Only recent channel 3 states count
constexpr uint32_t kPendingTimeoutMs = 8000; // A move with no room_done by then is sent again
constexpr uint32_t kLoadTimeoutMs = 2000;    // The new room must be loaded by then, or fall back
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

uint32_t MaxCourtesyMs() {
    return static_cast<uint32_t>(std::clamp(CVarGetInteger(CVAR_COOP_ROOM_FOLLOW_MAX_DEFER_MS, 10000), 0, 120000));
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

// The local Link is falling into the void: being voided out (Player_Action_1), or still in the air on the way down to
// a void floor or to no floor at all (z_player.c voids the Link out when it lands there, floor properties 5, 12 and
// 13, or falls below -4000).
bool LocalFallingIntoVoid() {
    Player* player = GET_PLAYER(gPlayState);
    if (player->stateFlags3 & PLAYER_STATE3_10000000) {
        return true;
    }
    const int floor = player->floorProperty;
    return !(player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) && player->actor.velocity.y < 0.0f &&
           (floor == FLOOR_PROPERTY_5 || floor == FLOOR_PROPERTY_12 || floor == FLOOR_PROPERTY_13 ||
            player->actor.floorHeight <= BGCHECK_Y_MIN + 1.0f);
}

// Logs when the local Link starts falling into the void (both sides; tests look for it).
void LogVoidFall() {
    static bool sFalling = false;
    const bool falling = LocalFallingIntoVoid();
    if (falling && !sFalling) {
        const Player* player = GET_PLAYER(gPlayState);
        const Vec3f& pos = player->actor.world.pos;
        SPDLOG_INFO("[Coop] void-fall: the Link is falling into the void in room {} at ({:.0f}, {:.0f}, {:.0f}) (floor "
                    "property {}, floor at {:.0f}, state3 0x{:08X})",
                    gPlayState->roomCtx.curRoom.num, pos.x, pos.y, pos.z, static_cast<int>(player->floorProperty),
                    player->actor.floorHeight, player->stateFlags3);
    }
    sFalling = falling;
}

// Host, phase 2 courtesy: the host is not pulled to a client's room while it reels from a hit or falls into the
// void (up to gCoop.RoomFollowMaxDeferMs). Moving it during the fall would cut the fall short.
const char* LocalReeling() {
    Player* player = GET_PLAYER(gPlayState);
    if (LocalFallingIntoVoid()) {
        return "falling into the void";
    }
    if (player->stateFlags1 & PLAYER_STATE1_4000000) {
        return "reeling from a hit";
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

// The common part of room_move and room_cross: sequence, scene, layer, room, position and yaw.
struct Place {
    uint32_t seq = 0;
    int16_t sceneId = -1;
    int8_t layer = 0;
    int8_t room = -1;
    Vec3f pos = {};
    s16 yaw = 0;
};

const char* ParsePlace(const nlohmann::json& m, Place& out) {
    const int64_t seq = Protocol::GetInt(m, "seq", -1);
    const int64_t scene = Protocol::GetInt(m, "scene", -1);
    const int64_t layer = Protocol::GetInt(m, "layer", -1);
    const int64_t room = Protocol::GetInt(m, "room", -1);
    const int64_t yaw = Protocol::GetInt(m, "yaw", 0x10000);
    if (seq <= 0 || seq > 0xFFFFFFFFll || scene < 0 || scene > 0xFF || layer < 0 || layer > 0x7F || room < 0 ||
        room > 127 || yaw < -0x8000 || yaw > 0x7FFF) {
        return "bad seq, scene, layer, room or yaw";
    }
    const auto pos = m.find("pos");
    if (pos == m.end() || !pos->is_array() || pos->size() != 3 || !(*pos)[0].is_number() || !(*pos)[1].is_number() ||
        !(*pos)[2].is_number()) {
        return "bad position";
    }
    out.pos = { (*pos)[0].get<float>(), (*pos)[1].get<float>(), (*pos)[2].get<float>() };
    if (!std::isfinite(out.pos.x) || !std::isfinite(out.pos.y) || !std::isfinite(out.pos.z) ||
        std::fabs(out.pos.x) > 32000 || std::fabs(out.pos.y) > 32000 || std::fabs(out.pos.z) > 32000) {
        return "position out of range";
    }
    out.seq = static_cast<uint32_t>(seq);
    out.sceneId = static_cast<int16_t>(scene);
    out.layer = static_cast<int8_t>(layer);
    out.room = static_cast<int8_t>(room);
    out.yaw = static_cast<s16>(yaw);
    return nullptr;
}

nlohmann::json PlaceJson(const char* type, const Place& p) {
    return { { "type", type },     { "seq", p.seq },   { "scene", p.sceneId },
             { "layer", p.layer }, { "room", p.room }, { "pos", { p.pos.x, p.pos.y, p.pos.z } },
             { "yaw", p.yaw } };
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

// Room planes (En_Holl walls) switch rooms by themselves for a Link within their range, as if it were crossing (200
// units for the visible kind, 100 for the transparent one; z_en_holl.c); a Link put down there by a room move makes
// the plane load one room twice (every actor of it twice). The range is taken a little wider than the game's
// (halfwidth 150, 200 or 280; height -50 to 200 or 280).
float PlaneClearDepth(const TransitionActorEntry& t) {
    return ((t.params >> 7) & 7) == EN_HOLL_TYPE_TRANSPARENT ? 130.0f : 230.0f;
}

// The first room plane of a room whose range holds `spot`, with the spot in the plane's frame (z along its facing).
const TransitionActorEntry* PlaneInRange(const Vec3f& spot, int8_t room, float& localZ) {
    PlayState* play = gPlayState;
    for (int i = 0; i < play->transitionActors.count; i++) {
        const TransitionActorEntry& t = play->transitionActors.list[i];
        const int16_t id = static_cast<int16_t>(t.id < 0 ? -t.id : t.id) & 0x1FFF;
        const int type = (t.params >> 7) & 7;
        if (id != ACTOR_EN_HOLL || (type != EN_HOLL_TYPE_DEFAULT && type != EN_HOLL_TYPE_TRANSPARENT) ||
            (t.sides[0].room != room && t.sides[1].room != room)) {
            continue;
        }
        const s16 yaw = DEG_TO_BINANG((t.rotY >> 7) & 0x1FF);
        const float dx = spot.x - t.pos.x, dy = spot.y - t.pos.y, dz = spot.z - t.pos.z;
        const float localX = dx * Math_CosS(yaw) - dz * Math_SinS(yaw);
        localZ = dx * Math_SinS(yaw) + dz * Math_CosS(yaw);
        if (std::fabs(localX) < 300.0f && dy > -80.0f && dy < 300.0f && std::fabs(localZ) < PlaneClearDepth(t)) {
            return &t;
        }
    }
    return nullptr;
}

// `spot` moved straight out of the range of any room plane of `room`, to the side of `room` (sides[1] lies along the
// plane's facing), onto the floor there. Unchanged when it is clear already or there is no floor out there.
Vec3f ClearOfRoomPlanes(const Vec3f& spot, int8_t room) {
    PlayState* play = gPlayState;
    float localZ = 0.0f;
    const TransitionActorEntry* t = PlaneInRange(spot, room, localZ);
    if (t == nullptr) {
        return spot;
    }
    const s16 yaw = DEG_TO_BINANG((t->rotY >> 7) & 0x1FF);
    const float out = (t->sides[1].room == room ? PlaneClearDepth(*t) : -PlaneClearDepth(*t)) - localZ;
    Vec3f probe = { spot.x + Math_SinS(yaw) * out, spot.y + 40.0f, spot.z + Math_CosS(yaw) * out };
    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    const f32 floorY = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &probe);
    if (poly == nullptr || std::fabs(floorY - spot.y) > 60.0f) {
        SPDLOG_INFO("[Coop] room-move: ({:.0f}, {:.0f}, {:.0f}) is within a room plane's range and there is no floor "
                    "out of it",
                    spot.x, spot.y, spot.z);
        return spot;
    }
    SPDLOG_INFO("[Coop] room-move: ({:.0f}, {:.0f}, {:.0f}) is within a room plane's range; arriving at ({:.0f}, "
                "{:.0f}, {:.0f}) in room {}",
                spot.x, spot.y, spot.z, probe.x, floorY, probe.z, room);
    return { probe.x, floorY, probe.z };
}

// Where another Link arrives next to a Link at `at` facing `facing` in `room`: beside it (50 units left or right, then
// 100, then behind), on a floor at about its height and out of the range of room planes; its own spot if none fits
// (moved out of a plane's range when the room moves). Two Links on the same spot push each other hard on the next
// frame.
Vec3f ArrivalSpotAt(const Vec3f& at, s16 facing, int8_t room) {
    PlayState* play = gPlayState;
    float localZ = 0.0f;
    const struct {
        s16 angle;
        float distance;
    } candidates[] = { { 0x4000, 50.0f },   { -0x4000, 50.0f }, { 0x4000, 100.0f },
                       { -0x4000, 100.0f }, { -0x8000, 80.0f }, { -0x8000, 150.0f } };
    for (const auto& c : candidates) {
        const s16 yaw = static_cast<s16>(facing + c.angle);
        Vec3f probe = { at.x + Math_SinS(yaw) * c.distance, at.y + 40.0f, at.z + Math_CosS(yaw) * c.distance };
        CollisionPoly* poly = nullptr;
        s32 bgId = 0;
        const f32 floorY = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &probe);
        const Vec3f spot = { probe.x, floorY, probe.z };
        if (poly != nullptr && std::fabs(floorY - at.y) < 30.0f && PlaneInRange(spot, room, localZ) == nullptr) {
            return spot;
        }
    }
    return at;
}

Vec3f ArrivalSpot(Player* link) {
    return ArrivalSpotAt(link->actor.world.pos, link->actor.shape.rot.y, gPlayState->roomCtx.curRoom.num);
}

Place HerePlace(uint32_t seq) {
    PlayState* play = gPlayState;
    Player* link = GET_PLAYER(play);
    Place p;
    p.seq = seq;
    p.sceneId = play->sceneId;
    p.layer = static_cast<int8_t>(gSaveContext.sceneLayer);
    p.room = play->roomCtx.curRoom.num;
    p.pos = ArrivalSpot(link);
    p.yaw = link->actor.shape.rot.y;
    return p;
}

// MARK: - Local move (both sides)
//
// Moves the local Link into another room of the same scene without reloading it: the same steps the game takes when
// a fallen Link respawns in another room (z_player.c, Player_Action_1), under a short fade.

enum class Kind { ClientOrder, HostFollow };
enum class Stage { Idle, FadeOut, WaitRoom, FadeIn };

struct Move {
    Kind kind = Kind::ClientOrder;
    Place place;
    bool applyFlags = false; // Client: the host's live scene flags
    ActorContextSceneFlags flags = {};
    uint32_t peer = 0; // Host: the player followed
};
Stage sStage = Stage::Idle;
Move sMove;
int sFadeFrame = 0;
uint32_t sStageMs = 0;
uint32_t sMoveStartMs = 0;
int8_t sFromRoom = -1;
uint32_t sFallbacks = 0;
uint32_t sLastMoveEndMs = 0; // Either kind; a room change right after one is ours, not a cross

void OnMoveDone(const char* result);

void SwapRooms() {
    PlayState* play = gPlayState;
    Player* player = GET_PLAYER(play);
    RoomContext& rooms = play->roomCtx;
    rooms.curRoom.num = -1;
    rooms.prevRoom.num = -1;
    rooms.curRoom.segment = NULL;
    rooms.prevRoom.segment = NULL;
    // Deletes every actor of the old rooms and clears the temporary flags. On the client the host's flags replace
    // ours: the new room's doors, chests and clear state come up as on the host. Memory only (clients never save).
    Room_FinishRoomChange(play, &rooms);
    if (sMove.applyFlags && sMove.place.sceneId == play->sceneId &&
        sMove.place.layer == static_cast<int8_t>(gSaveContext.sceneLayer)) {
        play->actorCtx.sceneFlags = sMove.flags;
    }
    if (!Room_RequestNewRoom(play, &rooms, sMove.place.room)) {
        OnMoveDone("fallback: the room request was refused");
        return;
    }
    sMove.place.pos = ClearOfRoomPlanes(sMove.place.pos, sMove.place.room);
    // Off any moving floor (platform, a door's collision): the game would carry the Link back on it.
    player->actor.floorBgId = BGCHECK_SCENE;
    player->actor.world.pos = sMove.place.pos;
    player->actor.prevPos = sMove.place.pos;
    player->actor.home.pos = sMove.place.pos;
    player->actor.shape.rot.y = sMove.place.yaw;
    player->actor.world.rot.y = sMove.place.yaw;
    player->yaw = sMove.place.yaw;
    player->speedXZ = 0.0f;
    player->actor.speed = 0.0f;
    player->actor.velocity = { 0.0f, 0.0f, 0.0f };
    // A later fall into the void respawns here, in the new room.
    RespawnData& respawn = gSaveContext.respawn[RESPAWN_MODE_DOWN];
    respawn.roomIndex = static_cast<u8>(sMove.place.room);
    respawn.pos = sMove.place.pos;
    respawn.yaw = sMove.place.yaw;
    respawn.tempSwitchFlags = play->actorCtx.sceneFlags.switches[2];
    respawn.tempCollectFlags = play->actorCtx.sceneFlags.collectible[2];
    sStage = Stage::WaitRoom;
    sStageMs = NowMs();
}

void StartMove(const Move& m, uint32_t now) {
    sMove = m;
    sFromRoom = gPlayState->roomCtx.curRoom.num;
    sMoveStartMs = now;
    sStage = Stage::FadeOut;
    sFadeFrame = 0;
}

void MoveFrame(uint32_t now) {
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
            if (play->roomCtx.status == 0 && play->roomCtx.curRoom.num == sMove.place.room) {
                if (sMove.kind == Kind::HostFollow && sMove.peer == 0) {
                    // --coop-test-room: the room is loaded now; stand the Link on its floor below the given spot.
                    Vec3f probe = { sMove.place.pos.x, sMove.place.pos.y + 100.0f, sMove.place.pos.z };
                    CollisionPoly* poly = nullptr;
                    s32 bgId = 0;
                    const f32 floorY = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &probe);
                    if (poly != nullptr) {
                        player->actor.world.pos.y = player->actor.prevPos.y = floorY;
                    }
                    SPDLOG_INFO("[Coop] TEST: host room move: floor {:.0f} under ({:.0f}, {:.0f})", floorY,
                                sMove.place.pos.x, sMove.place.pos.z);
                }
                Map_InitRoomData(play, sMove.place.room);
                Map_SetAreaEntrypoint(play);
                sStage = Stage::FadeIn;
                sFadeFrame = kFadeFrames;
            } else if (now - sStageMs > kLoadTimeoutMs) {
                OnMoveDone("fallback: the room did not load in time");
            }
            break;
        case Stage::FadeIn:
            sFadeFrame--;
            Play_FillScreen(play, sFadeFrame > 0, 0, 0, 0,
                            static_cast<u8>(std::max(0, sFadeFrame * 255 / kFadeFrames)));
            if (sFadeFrame <= 0) {
                OnMoveDone("ok");
            }
            break;
    }
}

void StopFade() {
    if (sStage != Stage::Idle && gPlayState != nullptr) {
        Play_FillScreen(gPlayState, false, 0, 0, 0, 0);
    }
    sStage = Stage::Idle;
}

// MARK: - Host

struct Follow {
    bool mismatch = false;
    uint32_t since = 0;
    uint32_t moves = 0;
    uint32_t lastMoveMs = 0;
    uint32_t pendingSeq = 0; // room_move sent, waiting for room_done
    uint32_t pendingSentMs = 0;
    std::string deferred; // Last deferral reason logged for this mismatch
    uint32_t checkSeq = 0;
    uint32_t checkAtMs = 0;
    int8_t checkRoom = -1;
    const char* checkKind = "seq"; // "seq": a move of the player; "cross": the host followed it
    // Phase 2: the player's own crossing, reported with room_cross
    bool cross = false;
    Place crossPlace;
    uint32_t crossMs = 0;
    uint32_t reelingSince = 0; // Courtesy deferral started (0: none)
};
std::unordered_map<uint32_t, Follow> sFollow;
uint32_t sSeq = 0;
int8_t sHostRoom = -1;
int8_t sHostPrevRoom = -1;
uint32_t sHostRoomChangedMs = 0; // The host's own last room change (not a follow)

void LogDeferred(uint32_t playerId, Follow& f, const std::string& why) {
    if (f.deferred != why) {
        f.deferred = why;
        SPDLOG_INFO("[Coop] room-move deferred for player {} ({})", playerId, why);
    }
}

void HostSendMove(uint32_t playerId, Follow& f, uint32_t now, const char* why) {
    PlayState* play = gPlayState;
    Player* host = GET_PLAYER(play);
    Place p = HerePlace(++sSeq);
    f.pendingSeq = p.seq;
    f.pendingSentMs = now;
    f.deferred.clear();
    f.moves++;
    nlohmann::json m = PlaceJson("room_move", p);
    m["flags"] = FlagsJson();
    SendJson(playerId, m);
    SPDLOG_INFO("[Coop] room-move: player {} seq {} to room {} of scene {} at ({:.0f}, {:.0f}, {:.0f}), next to the "
                "host at ({:.0f}, {:.0f}, {:.0f}) ({})",
                playerId, p.seq, p.room, play->sceneId, p.pos.x, p.pos.y, p.pos.z, host->actor.world.pos.x,
                host->actor.world.pos.y, host->actor.world.pos.z, why);
}

void SendCrossDone(uint32_t playerId, uint32_t seq, const char* result) {
    SendJson(playerId, { { "type", "room_cross_done" }, { "seq", seq }, { "result", result } });
}

// The host goes to the room a player crossed into, next to it.
void HostStartFollow(uint32_t playerId, Follow& f, uint32_t now) {
    Move m;
    m.kind = Kind::HostFollow;
    m.place = f.crossPlace;
    m.peer = playerId;
    f.deferred.clear();
    f.moves++;
    SPDLOG_INFO("[Coop] room-follow: the host follows player {} (seq {}) from room {} to room {} at ({:.0f}, {:.0f}, "
                "{:.0f}), {} ms after the cross",
                playerId, m.place.seq, gPlayState->roomCtx.curRoom.num, m.place.room, m.place.pos.x, m.place.pos.y,
                m.place.pos.z, now - f.crossMs);
    StartMove(m, now);
}

void HostFrame(uint32_t now) {
    PlayState* play = gPlayState;
    // The host's own room changes (doors, planes), to tell who crossed first.
    // A door changes the current room when it opens and drops the previous one when it closes (2 s later); a client's
    // room stops being loaded here only then, so both count as the host's own crossing.
    if (sStage == Stage::Idle &&
        (play->roomCtx.curRoom.num != sHostRoom || play->roomCtx.prevRoom.num != sHostPrevRoom)) {
        if (play->roomCtx.curRoom.num >= 0 && sHostRoom >= 0 && now - sLastMoveEndMs > 500) {
            sHostRoomChangedMs = now;
        }
        sHostRoom = play->roomCtx.curRoom.num;
        sHostPrevRoom = play->roomCtx.prevRoom.num;
    }
    for (const PlayerInfo& info : GetSnapshot().players) {
        if (info.id == kHostPlayerId) {
            continue;
        }
        Follow& f = sFollow[info.id];
        if (f.cross && (f.crossPlace.sceneId != play->sceneId ||
                        f.crossPlace.layer != static_cast<int8_t>(gSaveContext.sceneLayer))) {
            SPDLOG_INFO("[Coop] room-follow: player {} cross seq {} dropped (the host is in another scene now)",
                        info.id, f.crossPlace.seq);
            SendCrossDone(info.id, f.crossPlace.seq, "other scene");
            f.cross = false;
        }
        if (f.checkSeq != 0 && now >= f.checkAtMs) {
            SPDLOG_INFO("[Coop] room-move-check (host): player {} {} {} room {} enemies {}", info.id, f.checkKind,
                        f.checkSeq, f.checkRoom, Enemies::HostEnemiesInRoom(f.checkRoom));
            f.checkSeq = 0;
        }
        if (sStage != Stage::Idle) {
            continue; // The host is moving itself
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
            if (f.cross) {
                SPDLOG_INFO("[Coop] room-follow: player {} cross seq {} dropped (same room again)", info.id,
                            f.crossPlace.seq);
                SendCrossDone(info.id, f.crossPlace.seq, "same room again");
                f.cross = false;
            }
            f.reelingSince = 0;
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
            continue; // The player is on its way (or waiting for its own cutscene to end)
        }
        // Who crossed: the host itself (recently), or maybe the player, whose room_cross comes once its door or plane
        // is done; until then (or while it keeps changing rooms) the host waits instead of ordering it back.
        const bool hostCrossed = now - sHostRoomChangedMs < (now - f.since) + kHostWinsMs;
        if (!hostCrossed && (!f.cross || f.crossPlace.room != s.roomNum) && now - f.since < kCrossGraceMs) {
            LogDeferred(info.id, f, "waiting for the player's cross report");
            continue;
        }
        // The player crossed into the room it is in now: the host follows, unless the host crossed too (it wins).
        const bool followCross = f.cross && f.crossPlace.room == s.roomNum;
        if (followCross && now - sHostRoomChangedMs < kHostWinsMs + (now - f.crossMs)) {
            SPDLOG_INFO("[Coop] room-follow: player {} cross seq {} and the host crossed too ({} ms apart): the host "
                        "wins",
                        info.id, f.crossPlace.seq, static_cast<int>(f.crossMs) - static_cast<int>(sHostRoomChangedMs));
            SendCrossDone(info.id, f.crossPlace.seq, "host wins");
            f.cross = false;
        }
        if (f.lastMoveMs != 0 && now - f.lastMoveMs < MinMoveMs()) {
            LogDeferred(info.id, f, "minimum time between moves");
            continue;
        }
        if (const char* why = LocalBusy()) {
            LogDeferred(info.id, f, std::string("host: ") + why);
            continue;
        }
        if (f.cross && f.crossPlace.room == s.roomNum) {
            if (const char* why = LocalReeling()) {
                if (f.reelingSince == 0) {
                    f.reelingSince = now;
                }
                if (now - f.reelingSince < MaxCourtesyMs()) {
                    LogDeferred(info.id, f, std::string("courtesy: host ") + why);
                    continue;
                }
                SPDLOG_INFO("[Coop] room-follow: courtesy limit reached after {} ms (host {}); following anyway",
                            now - f.reelingSince, why);
            }
            HostStartFollow(info.id, f, now);
            return; // One move at a time
        }
        if (s.csAction != PLAYER_CSACTION_NONE || (s.stateFlags1 & PLAYER_STATE1_DEAD)) {
            LogDeferred(info.id, f, "player: cutscene or dead");
            continue;
        }
        HostSendMove(info.id, f, now, f.cross ? "player crossed elsewhere" : "the player is in another room");
        f.cross = false;
    }
}

void HostOnRoomDone(uint32_t from, const nlohmann::json& m) {
    if (!IsHosting()) {
        return;
    }
    const int64_t seq = Protocol::GetInt(m, "seq", -1);
    const int64_t room = Protocol::GetInt(m, "room", -2);
    const std::string result = Protocol::GetString(m, "result");
    if (seq <= 0 || seq > 0xFFFFFFFFll || room < -1 || room > 127 || result.empty() || result.size() > 64) {
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
    f.checkKind = "seq";
}

void HostOnRoomCross(uint32_t from, const nlohmann::json& m) {
    if (!IsHosting()) {
        return;
    }
    Place p;
    if (const char* why = ParsePlace(m, p)) {
        return Drop("room_cross", why);
    }
    Follow& f = sFollow[from];
    if (f.cross && p.seq <= f.crossPlace.seq) {
        return Drop("room_cross", "duplicate or out of order");
    }
    if (p.sceneId != gPlayState->sceneId || p.layer != static_cast<int8_t>(gSaveContext.sceneLayer)) {
        SPDLOG_INFO("[Coop] room-follow: player {} cross seq {} is for scene {} (host in scene {}); ignored", from,
                    p.seq, p.sceneId, gPlayState->sceneId);
        return;
    }
    if (p.room >= gPlayState->roomList.count) {
        return Drop("room_cross", "room outside this scene's room list");
    }
    f.cross = true;
    f.crossPlace = p;
    f.crossMs = NowMs();
    f.reelingSince = 0;
    SPDLOG_INFO("[Coop] room-follow: player {} crossed into room {} (seq {})", from, p.room, p.seq);
}

// MARK: - Client

struct Order {
    bool active = false;
    Move move;
    uint32_t receivedMs = 0;
};
Order sOrder;
uint32_t sLastSeq = 0;
std::string sClientDeferred;
uint32_t sCheckSeq = 0;
uint32_t sCheckAtMs = 0;
const char* sCheckKind = "seq";
// Phase 2: the client's own room changes
int8_t sClientRoom = -1;
int8_t sCrossRoom = -1; // A room the client walked into, not reported yet
uint32_t sCrossSinceMs = 0;
uint32_t sVoidRespawnMs = 0; // Last frame the Link was falling into the void
bool sVoidSpotSet = false;   // The current fall respawns next to the host
uint32_t sCrossSeq = 0;

void ClientOnRoomMove(uint32_t, const nlohmann::json& m) {
    if (IsHosting()) {
        return;
    }
    Order o;
    if (const char* why = ParsePlace(m, o.move.place)) {
        return Drop("room_move", why);
    }
    const auto flags = m.find("flags");
    if (flags == m.end() || !ParseFlags(*flags, o.move.flags)) {
        return Drop("room_move", "bad scene flags (fields, sizes or values)");
    }
    if (o.move.place.seq <= sLastSeq) {
        return Drop("room_move", "duplicate or out of order");
    }
    sLastSeq = o.move.place.seq;
    o.active = true;
    o.move.kind = Kind::ClientOrder;
    o.move.applyFlags = true;
    o.receivedMs = NowMs();
    sOrder = o;
    sClientDeferred.clear();
    sCrossRoom = -1; // The host decided: whatever the client crossed into is settled by this order
    SPDLOG_INFO("[Coop] room-move: seq {} order to room {} of scene {} received", o.move.place.seq, o.move.place.room,
                o.move.place.sceneId);
}

void ClientOnRoomCrossDone(uint32_t, const nlohmann::json& m) {
    if (IsHosting()) {
        return;
    }
    const int64_t seq = Protocol::GetInt(m, "seq", -1);
    const std::string result = Protocol::GetString(m, "result");
    if (seq <= 0 || seq > 0xFFFFFFFFll || result.empty() || result.size() > 64) {
        return Drop("room_cross_done", "bad seq or result");
    }
    SPDLOG_INFO("[Coop] room-follow: the host answered cross seq {}: {}", seq, result);
    if (result == "followed") {
        sCheckSeq = static_cast<uint32_t>(seq);
        sCheckAtMs = NowMs() + kCheckDelayMs;
        sCheckKind = "cross";
    }
}

void SendDone(const Place& p, const char* result) {
    SendJson(kHostPlayerId, { { "type", "room_done" },
                              { "seq", p.seq },
                              { "room", gPlayState != nullptr ? gPlayState->roomCtx.curRoom.num : -1 },
                              { "result", result } });
}

// A client that falls into the void comes back next to the host, not where the game would put it (its last safe
// spot, maybe in another room): the game's own void respawn is pointed there while the Link falls. Player_Action_1
// puts the Link at its last safe spot (unk_3C0, in room unk_3CE, loading that room if needed); a void floor that
// reloads the scene uses respawn[RESPAWN_MODE_DOWN]. Without a recent host state in this scene, the game's own.
void ClientRespawnNextToHost() {
    static Vec3f sSpot;
    static s16 sYaw;
    static int8_t sRoom;
    PlayState* play = gPlayState;
    Player* link = GET_PLAYER(play);
    if (!sVoidSpotSet) {
        PlayerSync::State host;
        uint32_t age = 0;
        if (!PlayerSync::LatestState(kHostPlayerId, host, &age) || age > kFreshStateMs || !host.inGameplay ||
            host.sceneId != play->sceneId || host.sceneLayer != static_cast<int8_t>(gSaveContext.sceneLayer) ||
            host.roomNum < 0 || host.roomNum >= play->roomList.count || (host.stateFlags3 & PLAYER_STATE3_10000000) ||
            !std::isfinite(host.pos.x) || !std::isfinite(host.pos.y) || !std::isfinite(host.pos.z)) {
            return;
        }
        sRoom = host.roomNum;
        sYaw = host.shapeRot.y;
        sSpot = ClearOfRoomPlanes(ArrivalSpotAt(host.pos, sYaw, sRoom), sRoom);
        sVoidSpotSet = true;
        SPDLOG_INFO("[Coop] void-fall: respawning next to the host in room {} at ({:.0f}, {:.0f}, {:.0f})", sRoom,
                    sSpot.x, sSpot.y, sSpot.z);
    }
    // Every frame of the fall: the game saves its last safe spot again while the Link touches safe ground.
    link->unk_3C0 = sSpot;
    link->unk_3CC = sYaw;
    link->unk_3CE = sRoom;
    link->unk_3CF = 1;
    RespawnData& respawn = gSaveContext.respawn[RESPAWN_MODE_DOWN];
    respawn.roomIndex = static_cast<u8>(sRoom);
    respawn.pos = sSpot;
    respawn.yaw = sYaw;
}

// The client's own room changes: a door or room plane it went through is reported (room_cross) once the new room
// has stayed 300 ms and the Link is free again; a void respawn, a death or one of our moves is not a cross.
void ClientWatchCrossing(uint32_t now) {
    PlayState* play = gPlayState;
    if (LocalFallingIntoVoid()) {
        sVoidRespawnMs = now;
        ClientRespawnNextToHost();
    } else {
        sVoidSpotSet = false;
    }
    const int8_t room = play->roomCtx.curRoom.num;
    if (room != sClientRoom) {
        const int8_t from = sClientRoom;
        sClientRoom = room;
        if (room < 0 || from < 0) {
            sCrossRoom = -1;
            return;
        }
        if (now - sLastMoveEndMs < 500) {
            sCrossRoom = -1; // Our own move
            return;
        }
        if (now - sVoidRespawnMs < kRespawnWindowMs || ClientDeath::IsDown()) {
            sCrossRoom = -1;
            SPDLOG_INFO("[Coop] room-follow: room {} -> {} after a fall into the void: not a cross (back next to the "
                        "host)",
                        from, room);
            return;
        }
        sCrossRoom = room;
        sCrossSinceMs = now;
        return;
    }
    if (sCrossRoom < 0 || sCrossRoom != room || now - sCrossSinceMs < kCrossSettleMs || LocalBusy() != nullptr) {
        return;
    }
    // Inside a room plane's range both rooms stay loaded and the current one is the room ahead, not the one the Link
    // stands in: wait until the plane drops the other room (the Link is through), unless the Link stays there.
    if (play->roomCtx.prevRoom.num >= 0 && now - sCrossSinceMs < kCrossLingerMs) {
        return;
    }
    // Only if the host is not in this room already. Its room may still be loaded here as the previous one (a Link
    // staying inside a room plane's range), but the host does not have ours: it must come.
    PlayerSync::State host;
    if (PlayerSync::LatestState(kHostPlayerId, host) && host.roomNum == room) {
        sCrossRoom = -1;
        return;
    }
    const Place p = HerePlace(++sCrossSeq);
    SendJson(kHostPlayerId, PlaceJson("room_cross", p));
    SPDLOG_INFO("[Coop] room-follow: crossed into room {} (seq {}); asking the host to follow", p.room, p.seq);
    sCrossRoom = -1;
}

void ClientFrame(uint32_t now) {
    PlayState* play = gPlayState;
    if (sCheckSeq != 0 && now >= sCheckAtMs && sStage == Stage::Idle) {
        size_t shown = 0, duplicates = 0;
        Enemies::ClientPuppetCounts(shown, duplicates);
        SPDLOG_INFO("[Coop] room-move-check: {} {} room {} puppets shown {}, duplicate netIds {}, actors from other "
                    "rooms {}",
                    sCheckKind, sCheckSeq, play->roomCtx.curRoom.num, shown, duplicates,
                    CountActorsFromOtherRooms(play->roomCtx.curRoom.num));
        sCheckSeq = 0;
    }
    if (sStage != Stage::Idle) {
        MoveFrame(now);
        return;
    }
    ClientWatchCrossing(now);
    if (!sOrder.active) {
        return;
    }
    const Place& p = sOrder.move.place;
    if (p.sceneId != play->sceneId || p.layer != static_cast<int8_t>(gSaveContext.sceneLayer)) {
        // Another scene: following the host's scene (milestone 2) brings the client there.
        SPDLOG_INFO("[Coop] room-move: seq {} dropped (it is for scene {}, this is scene {})", p.seq, p.sceneId,
                    play->sceneId);
        SendDone(p, "other scene");
        sOrder.active = false;
        return;
    }
    const char* why = ClientDeath::IsDown() ? "dead" : LocalBusy();
    if (why != nullptr) {
        if (sClientDeferred != why) {
            sClientDeferred = why;
            SPDLOG_INFO("[Coop] room-move: seq {} deferred ({})", p.seq, why);
        }
        return;
    }
    const Move m = sOrder.move;
    sOrder.active = false;
    if (play->roomCtx.curRoom.num == m.place.room) {
        SPDLOG_INFO("[Coop] room-move: seq {} already in room {}", m.place.seq, m.place.room);
        SendDone(m.place, "already there");
        return;
    }
    if (m.place.room >= play->roomList.count) {
        sMove = m;
        sStage = Stage::FadeOut;
        OnMoveDone("fallback: room outside this scene's room list");
        return;
    }
    SPDLOG_INFO("[Coop] room-move: seq {} starting: room {} -> {} (order waited {} ms)", m.place.seq,
                play->roomCtx.curRoom.num, m.place.room, now - sOrder.receivedMs);
    StartMove(m, now);
}

void OnMoveDone(const char* result) {
    PlayState* play = gPlayState;
    const uint32_t now = NowMs();
    if (play->roomCtx.curRoom.num >= 0 && play->roomCtx.prevRoom.num == play->roomCtx.curRoom.num) {
        // The same room loaded twice: every actor of it exists twice. Never expected; the tests fail on it.
        SPDLOG_WARN("[Coop] room-move-duplicate-room: room {} is loaded twice after the move",
                    play->roomCtx.curRoom.num);
    }
    const bool fallback = std::string(result).rfind("fallback", 0) == 0;
    Play_FillScreen(play, false, 0, 0, 0, 0);
    sStage = Stage::Idle;
    sLastMoveEndMs = now;
    if (sMove.kind == Kind::ClientOrder) {
        if (fallback) {
            sFallbacks++;
            SPDLOG_WARN(
                "[Coop] room-move-fallback: seq {} to room {} ({}); reloading the scene next to the host ({} so "
                "far)",
                sMove.place.seq, sMove.place.room, result, sFallbacks);
            SendDone(sMove.place, "fallback");
            ClientReturnToHost("room move fallback");
            return;
        }
        SPDLOG_INFO("[Coop] room-move: seq {} done: room {} -> {} in {} ms", sMove.place.seq, sFromRoom,
                    play->roomCtx.curRoom.num, now - sMoveStartMs);
        SendDone(sMove.place, "ok");
        sCheckSeq = sMove.place.seq;
        sCheckAtMs = now + kCheckDelayMs;
        sCheckKind = "seq";
        sClientRoom = play->roomCtx.curRoom.num;
        return;
    }
    if (sMove.peer == 0) {
        // --coop-test-room: the host moved itself; for the clients it is like crossing a door.
        sHostRoomChangedMs = now;
        sHostRoom = play->roomCtx.curRoom.num;
        sHostPrevRoom = play->roomCtx.prevRoom.num;
        SPDLOG_INFO("[Coop] TEST: host room move done ({}) in room {}", result, play->roomCtx.curRoom.num);
        return;
    }
    // The host followed a player.
    Follow& f = sFollow[sMove.peer];
    f.cross = false;
    f.lastMoveMs = now;
    sHostRoom = play->roomCtx.curRoom.num;
    sHostPrevRoom = play->roomCtx.prevRoom.num;
    if (fallback) {
        sFallbacks++;
        SPDLOG_WARN("[Coop] room-move-fallback: the host could not follow player {} to room {} ({}) ({} so far)",
                    sMove.peer, sMove.place.room, result, sFallbacks);
        SendCrossDone(sMove.peer, sMove.place.seq, "host could not follow");
        return;
    }
    SPDLOG_INFO("[Coop] room-follow: done: the host went from room {} to room {} in {} ms", sFromRoom,
                play->roomCtx.curRoom.num, now - sMoveStartMs);
    SendCrossDone(sMove.peer, sMove.place.seq, "followed");
    f.checkSeq = sMove.place.seq;
    f.checkAtMs = now + kCheckDelayMs;
    f.checkRoom = play->roomCtx.curRoom.num;
    f.checkKind = "cross";
}

void ResetClient() {
    sOrder = {};
    sCheckSeq = 0;
    sCrossRoom = -1;
    sClientRoom = -1;
}

void OnPlayDestroy() {
    StopFade();
    ResetClient();
    // The host's follow state stays: a void-out that reloads the scene must not lose a client's cross into another
    // room of it (HostFrame drops crosses for another scene).
    sHostRoom = -1;
}

void RegisterRoomsHooks() {
    RegisterJsonHandler("room_move", ClientOnRoomMove);
    RegisterJsonHandler("room_done", HostOnRoomDone);
    RegisterJsonHandler("room_cross", HostOnRoomCross);
    RegisterJsonHandler("room_cross_done", ClientOnRoomCrossDone);
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
        if (!InGameplay()) {
            StopFade();
            return;
        }
        LogVoidFall();
        if (sStage != Stage::Idle) {
            MoveFrame(now);
        }
        HostFrame(now);
        return;
    }
    sFollow.clear();
    if (!IsClientPlayingInSession() || !InGameplay()) {
        StopFade();
        ResetClient();
        if (!IsClientInSession()) {
            sLastSeq = 0;
        }
        return;
    }
    LogVoidFall();
    ClientFrame(now);
}

void TestFuzz() {
    auto send = [](const char* what, const nlohmann::json& m) {
        SPDLOG_INFO("[Coop] TEST: fuzz room message: {}", what);
        SendJson(IsHosting() ? kBroadcast : kHostPlayerId, m);
    };
    auto place = [](const char* type, uint32_t seq, int room) {
        return nlohmann::json{ { "type", type }, { "seq", seq },         { "scene", 27 }, { "layer", 0 },
                               { "room", room }, { "pos", { 0, 0, 0 } }, { "yaw", 0 } };
    };
    if (!IsHosting()) {
        send("room_done with no seq", { { "type", "room_done" }, { "room", 1 }, { "result", "ok" } });
        send("room_done with room 300", { { "type", "room_done" }, { "seq", 1 }, { "room", 300 }, { "result", "ok" } });
        send("room_cross to room 200", place("room_cross", 0x7FFFFF20, 200));
        nlohmann::json m = place("room_cross", 0x7FFFFF21, 1);
        m["pos"] = { 1.0, nullptr, 2.0 };
        send("room_cross with a NaN position (null in JSON)", m);
        m = place("room_cross", 0x7FFFFF22, 1);
        m["pos"] = { 1, 2 };
        send("room_cross with a 2-number position", m);
        m = place("room_cross", 0x7FFFFF23, 1);
        m["yaw"] = 70000;
        send("room_cross with yaw 70000", m);
        send("room_cross with seq 0", place("room_cross", 0, 1));
        return;
    }
    const nlohmann::json good = { { "switches", { 0, 0, 0, 0 } },
                                  { "chest", 0 },
                                  { "cleared", 0 },
                                  { "clearedTemp", 0 },
                                  { "collectible", { 0, 0, 0, 0 } } };
    auto move = [&](uint32_t seq, int room, const nlohmann::json& flags) {
        nlohmann::json m = place("room_move", seq, room);
        m["flags"] = flags;
        return m;
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
    send("seq 0", move(0, 1, good));
    send("room_cross_done with no seq", { { "type", "room_cross_done" }, { "result", "followed" } });
}

void TestMoveTo(int8_t room, const Vec3f& pos) {
    if (!IsHosting() || !InGameplay() || sStage != Stage::Idle || room < 0 || room >= gPlayState->roomList.count) {
        SPDLOG_INFO("[Coop] TEST: room move to room {} not possible now", room);
        return;
    }
    Move m;
    m.kind = Kind::HostFollow;
    m.place.seq = 0;
    m.place.sceneId = gPlayState->sceneId;
    m.place.layer = static_cast<int8_t>(gSaveContext.sceneLayer);
    m.place.room = room;
    m.place.pos = pos;
    m.place.yaw = GET_PLAYER(gPlayState)->actor.shape.rot.y;
    m.peer = 0;
    SPDLOG_INFO("[Coop] TEST: host room move to room {} at ({:.0f}, {:.0f}, {:.0f})", room, pos.x, pos.y, pos.z);
    StartMove(m, NowMs());
}

static RegisterShipInitFunc initFunc(RegisterRoomsHooks);

} // namespace Coop::Rooms
